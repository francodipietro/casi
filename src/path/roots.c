/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/roots.h"
#include "casi/error.h"
#include "casi/fs.h"
#include "casi/log.h"
#include "casi/str.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

struct root_entry {
    char  *name;
    char  *path;
    size_t path_len;
    bool   alias;
};

struct casi_roots {
    struct root_entry *items;
    size_t len;
    size_t cap;
    /* Which byte values start some root path. Lets the text scanner skip
     * almost every position without a single strncmp. */
    unsigned char first_byte[256];
};

static int continues_path_component(char c);
static bool same_path_ignoring_trailing_slashes(const char *a, const char *b);

int casi_roots_new(casi_roots **out)
{
    casi_roots *roots = calloc(1, sizeof(*roots));

    if (roots == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory allocating roots");

    *out = roots;
    return CASI_OK;
}

void casi_roots_free(casi_roots *roots)
{
    size_t i;

    if (roots == NULL)
        return;

    for (i = 0; i < roots->len; i++) {
        free(roots->items[i].name);
        free(roots->items[i].path);
    }
    free(roots->items);
    free(roots);
}

/* Trailing slashes are stripped so "/a/src" and "/a/src/" behave alike. */
static size_t trimmed_len(const char *path)
{
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/')
        len--;

    return len;
}

static void rebuild_first_byte_index(casi_roots *roots)
{
    size_t i;

    memset(roots->first_byte, 0, sizeof(roots->first_byte));
    for (i = 0; i < roots->len; i++)
        if (roots->items[i].path_len > 0)
            roots->first_byte[(unsigned char)roots->items[i].path[0]] = 1;
}

/* Keeps the table ordered by descending path length, so a linear scan is
 * automatically a longest-prefix match. */
static void sort_by_path_len_desc(casi_roots *roots)
{
    size_t i, j;

    for (i = 1; i < roots->len; i++) {
        struct root_entry key = roots->items[i];

        for (j = i; j > 0 && roots->items[j - 1].path_len < key.path_len; j--)
            roots->items[j] = roots->items[j - 1];
        roots->items[j] = key;
    }
}

bool casi_root_name_is_valid(const char *name)
{
    const char *p;

    if (name == NULL || name[0] == '\0')
        return false;
    for (p = name; *p != '\0'; p++)
        if (!continues_path_component(*p))
            return false;
    return true;
}

static int add_root(casi_roots *roots, const char *name, const char *local_path,
                    bool alias)
{
    struct root_entry entry;
    size_t i;

    if (name == NULL || name[0] == '\0')
        return casi_error_set(CASI_EINVAL, "root name cannot be empty");
    if (local_path == NULL)
        return casi_error_set(CASI_EINVAL, "root path cannot be null");
    if (local_path[0] == '\0')
        return casi_error_set(CASI_EINVAL, "root \"%s\" has an empty path", name);
    if (!casi_root_name_is_valid(name))
        return casi_error_set(CASI_EINVAL,
                              "root name \"%s\" contains an invalid character", name);

    /* Aliases share the logical name but never replace its destination. */
    for (i = 0; i < roots->len; i++) {
        if (strcmp(roots->items[i].name, name) == 0 &&
            roots->items[i].alias == alias) {
            if (alias && same_path_ignoring_trailing_slashes(roots->items[i].path,
                                                               local_path))
                return CASI_OK;
            if (alias)
                continue;
            char *dup = casi_strndup(local_path, trimmed_len(local_path));

            if (dup == NULL)
                return CASI_ENOMEM;
            free(roots->items[i].path);
            roots->items[i].path = dup;
            roots->items[i].path_len = strlen(dup);
            sort_by_path_len_desc(roots);
            rebuild_first_byte_index(roots);
            return CASI_OK;
        }
    }

    if (roots->len == roots->cap) {
        size_t cap = roots->cap ? roots->cap * 2 : 8;
        struct root_entry *p = realloc(roots->items, cap * sizeof(*p));

        if (p == NULL)
            return casi_error_set(CASI_ENOMEM, "out of memory growing roots");
        roots->items = p;
        roots->cap = cap;
    }

    entry.name = casi_strdup(name);
    entry.path = casi_strndup(local_path, trimmed_len(local_path));
    if (entry.name == NULL || entry.path == NULL) {
        free(entry.name);
        free(entry.path);
        return CASI_ENOMEM;
    }
    entry.path_len = strlen(entry.path);
    entry.alias = alias;

    roots->items[roots->len++] = entry;
    sort_by_path_len_desc(roots);
    rebuild_first_byte_index(roots);
    return CASI_OK;
}

int casi_roots_add(casi_roots *roots, const char *name, const char *local_path)
{
    return add_root(roots, name, local_path, false);
}

int casi_roots_add_alias(casi_roots *roots, const char *name, const char *old_path)
{
    return add_root(roots, name, old_path, true);
}

struct load_ctx {
    casi_roots *roots;
};

/* Picks "root.<name>.path" out of the config stream. libgit2 lowercases the
 * section and variable but preserves subsection case, which is what we want:
 * root names are the user's to choose. */
static int load_one(const char *key, const char *value, void *payload)
{
    struct load_ctx *ctx = payload;
    const char *rest, *dot;
    char *name;
    int rc;

    if (!casi_str_has_prefix(key, "root."))
        return CASI_OK;

    rest = key + strlen("root.");
    dot = strrchr(rest, '.');
    if (dot == NULL || (strcmp(dot, ".path") != 0 && strcmp(dot, ".alias") != 0))
        return CASI_OK;

    name = casi_strndup(rest, (size_t)(dot - rest));
    if (name == NULL)
        return CASI_ENOMEM;

    rc = strcmp(dot, ".alias") == 0
       ? casi_roots_add_alias(ctx->roots, name, value)
       : casi_roots_add(ctx->roots, name, value);
    free(name);
    return rc;
}

int casi_roots_load(casi_roots *roots, casi_config *cfg)
{
    struct load_ctx ctx = { roots };
    const char *home;
    int rc;

    if ((rc = casi_config_foreach(cfg, load_one, &ctx)) != CASI_OK)
        return rc;

    /* Keep the home root as the lowest-priority fallback. Project roots are
     * longer and therefore win for project-local paths, while references to
     * other files under the user's home remain portable across machines. */
    if ((home = casi_fs_home()) != NULL &&
        (rc = casi_roots_add(roots, CASI_HOME_ROOT, home)) != CASI_OK)
        return rc;

    return CASI_OK;
}

size_t casi_roots_count(const casi_roots *roots)
{
    return roots->len;
}

const char *casi_roots_name_at(const casi_roots *roots, size_t i)
{
    return i < roots->len ? roots->items[i].name : NULL;
}

const char *casi_roots_path_at(const casi_roots *roots, size_t i)
{
    return i < roots->len ? roots->items[i].path : NULL;
}

bool casi_roots_is_alias_at(const casi_roots *roots, size_t i)
{
    return i < roots->len && roots->items[i].alias;
}

/*
 * Whether a byte could extend the final component of a path. Used to reject
 * "/a/src" matching inside "/a/srcfoo", which is a different directory.
 */
static int continues_path_component(char c)
{
    unsigned char u = (unsigned char)c;

    return (u >= '0' && u <= '9') ||
           (u >= 'a' && u <= 'z') ||
           (u >= 'A' && u <= 'Z') ||
           u == '-' || u == '_' || u == '.' || u == '~' || u == '@' || u == '+' ||
           u >= 0x80;  /* any UTF-8 continuation or lead byte */
}

/*
 * Two callers with genuinely different boundary rules.
 *
 * CASI_MATCH_PATH: the input is exactly a path, so a root only matches when
 * what follows is '/' or nothing. Anything else means a different directory --
 * including a literal quote, which is a legal character in a Unix path.
 *
 * CASI_MATCH_TEXT: the path is embedded in a transcript, so it ends wherever
 * its surroundings end it -- a JSON quote, a space, a comma. Requiring '/'
 * there would miss every path that appears without a trailing component, which
 * is exactly what the `cwd` field looks like.
 */
enum match_mode { CASI_MATCH_PATH, CASI_MATCH_TEXT };

static int root_matches_at(const struct root_entry *entry,
                           const char *text, size_t text_len,
                           enum match_mode mode)
{
    char next;

    if (entry->path_len > text_len)
        return 0;
    if (memcmp(text, entry->path, entry->path_len) != 0)
        return 0;

    if (entry->path_len == text_len)
        return 1;

    next = text[entry->path_len];
    if (next == '/')
        return 1;

    return mode == CASI_MATCH_TEXT && !continues_path_component(next);
}

static const struct root_entry *find_match(const casi_roots *roots,
                                           const char *text, size_t text_len,
                                           enum match_mode mode)
{
    size_t i;

    if (text_len == 0 || !roots->first_byte[(unsigned char)text[0]])
        return NULL;

    /* Table is sorted longest-first, so the first hit is the longest. */
    for (i = 0; i < roots->len; i++)
        if (root_matches_at(&roots->items[i], text, text_len, mode))
            return &roots->items[i];

    return NULL;
}

int casi_roots_normalize_path(const casi_roots *roots, const char *local, casi_buf *out)
{
    size_t len = strlen(local);
    const struct root_entry *match = find_match(roots, local, len, CASI_MATCH_PATH);
    int rc;

    casi_buf_clear(out);

    if (match == NULL)
        return casi_buf_puts(out, local);

    if ((rc = casi_buf_printf(out, CASI_CANONICAL_SCHEME "%s", match->name)) != CASI_OK)
        return rc;

    return casi_buf_put(out, local + match->path_len, len - match->path_len);
}

/* Last path component, trailing slashes ignored. A project's basename is its
 * stable, human-facing identity across machines. */
static const char *basename_of(const char *path)
{
    const char *end = path + strlen(path);
    const char *p;

    while (end > path && end[-1] == '/')
        end--;
    for (p = end; p > path && p[-1] != '/'; p--)
        ;
    return p;
}

static const char *root_path_by_name(const casi_roots *roots, const char *name)
{
    size_t i;

    for (i = 0; i < roots->len; i++)
        if (!roots->items[i].alias && strcmp(roots->items[i].name, name) == 0)
            return roots->items[i].path;
    return NULL;
}

static int auto_root_name(const char *base, casi_buf *out)
{
    const unsigned char *p = (const unsigned char *)base;
    int rc;

    casi_buf_clear(out);
    for (; *p != '\0'; p++) {
        /* Keep the readable spelling for ordinary names, but encode every
         * other byte (including '~') so the mapping is injective: a literal
         * "~20" cannot collide with a basename containing a space. */
        if (((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
             (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_' ||
             *p == '.' || *p == '@' || *p == '+')) {
            if ((rc = casi_buf_putc(out, (char)*p)) != CASI_OK)
                return rc;
        } else if (*p >= 0x80) {
            /* UTF-8 bytes are already valid path-component bytes. */
            if ((rc = casi_buf_putc(out, (char)*p)) != CASI_OK)
                return rc;
        } else if ((rc = casi_buf_printf(out, "~%02X", *p)) != CASI_OK) {
            return rc;
        }
    }
    if (out->len == 0)
        return casi_error_set(CASI_EINVAL, "project path has no basename");
    return CASI_OK;
}

static bool same_path_ignoring_trailing_slashes(const char *a, const char *b)
{
    size_t alen = strlen(a), blen = strlen(b);

    while (alen > 1 && a[alen - 1] == '/')
        alen--;
    while (blen > 1 && b[blen - 1] == '/')
        blen--;
    return alen == blen && memcmp(a, b, alen) == 0;
}

int casi_roots_canonicalize_project(casi_roots *roots, const char *local, casi_buf *out)
{
    char *trimmed = casi_strndup(local, trimmed_len(local));
    const char *base;
    casi_buf name = CASI_BUF_INIT;
    const char *existing;
    int rc;

    if (trimmed == NULL)
        return CASI_ENOMEM;
    base = basename_of(trimmed);
    if ((rc = auto_root_name(base, &name)) != CASI_OK)
        goto done;
    existing = root_path_by_name(roots, casi_buf_cstr(&name));

    /* Two directories with the same basename are ambiguous: refusing the
     * second registration prevents the later discovery from silently
     * rerouting the first project's sessions. */
    if (existing != NULL && !same_path_ignoring_trailing_slashes(existing, trimmed)) {
        rc = casi_error_set(CASI_EEXISTS,
                            "project basename \"%s\" is ambiguous: \"%s\" and \"%s\"",
                            base, existing, trimmed);
        goto done;
    }

    if ((rc = casi_roots_add(roots, casi_buf_cstr(&name), trimmed)) == CASI_OK)
        rc = casi_roots_normalize_path(roots, trimmed, out);

done:
    free(trimmed);
    casi_buf_dispose(&name);
    return rc;
}

struct project_search {
    const char *wanted;
    casi_buf found;
    size_t visited;
    bool ambiguous;
    bool incomplete;
};

/* Bounded search: it runs only when a remote project has no local transcript.
 * Hidden directories and dependency/build trees cannot be useful destinations. */
static int search_project_dir(const char *dir, unsigned int depth,
                              struct project_search *search)
{
    casi_strvec names = CASI_STRVEC_INIT;
    casi_buf path = CASI_BUF_INIT, encoded = CASI_BUF_INIT, marker = CASI_BUF_INIT;
    size_t i;
    int rc;

    if (search->ambiguous)
        return CASI_OK;
    /* The supported search scope is shallow checkout layouts, not arbitrary
     * files inside a deep tree. Reaching the depth boundary is normal; only
     * exhausting the visit budget makes the search inconclusive. */
    if (depth > 6)
        return CASI_OK;
    if (search->visited >= 20000) {
        search->incomplete = true;
        return CASI_OK;
    }
    /* A checkout is a search endpoint, not a tree to recurse into. */
    if (depth > 0) {
        if ((rc = casi_fs_join(&marker, dir, ".git")) != CASI_OK)
            goto done;
        if (casi_fs_exists(casi_buf_cstr(&marker))) {
            rc = CASI_OK;
            goto done;
        }
    }
    rc = casi_fs_listdir(dir, &names);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc == CASI_EIO && access(dir, R_OK | X_OK) != 0 && errno == EACCES) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    for (i = 0; i < names.len && !search->ambiguous; i++) {
        struct stat st;
        const char *name = names.items[i];
        bool link;

        if (name[0] == '.' || strcmp(name, "Library") == 0 ||
            strcmp(name, "Applications") == 0 ||
            strcmp(name, "Pictures") == 0 || strcmp(name, "Movies") == 0 ||
            strcmp(name, "Music") == 0 ||
            strcmp(name, "node_modules") == 0 ||
            strcmp(name, "vendor") == 0 || strcmp(name, "target") == 0 ||
            strcmp(name, "build") == 0 || strcmp(name, "dist") == 0)
            continue;
        casi_buf_clear(&path);
        if ((rc = casi_fs_join(&path, dir, name)) != CASI_OK)
            goto done;
        if (lstat(casi_buf_cstr(&path), &st) != 0)
            continue;
        link = S_ISLNK(st.st_mode);
        if (link && stat(casi_buf_cstr(&path), &st) != 0)
            continue;
        if (!S_ISDIR(st.st_mode))
            continue;
        search->visited++;
        if ((rc = auto_root_name(name, &encoded)) != CASI_OK)
            goto done;
        if (strcmp(casi_buf_cstr(&encoded), search->wanted) == 0) {
            if (search->found.len > 0) {
                search->ambiguous = true;
                break;
            }
            if ((rc = casi_buf_puts(&search->found, casi_buf_cstr(&path))) != CASI_OK)
                goto done;
            /* The matched checkout is the endpoint, even if it has no .git
             * marker. Scanning all of its dependencies is both slow and can
             * hit the visit limit before another checkout is considered. */
            continue;
        }
        if (!link &&
            (rc = search_project_dir(casi_buf_cstr(&path), depth + 1, search)) != CASI_OK)
            goto done;
    }
    rc = CASI_OK;

done:
    casi_strvec_dispose(&names);
    casi_buf_dispose(&path);
    casi_buf_dispose(&encoded);
    casi_buf_dispose(&marker);
    return rc;
}

int casi_roots_discover_project(casi_roots *roots, const char *canonical)
{
    static const char *locations[] = {
        "src", "code", "dev", "work", "projects", "repos", NULL
    };
    struct project_search search = { 0 };
    casi_buf base = CASI_BUF_INIT;
    const char *name, *home;
    size_t i;
    int rc = CASI_OK;

    if (!casi_str_has_prefix(canonical, CASI_CANONICAL_SCHEME))
        return CASI_OK;
    name = canonical + strlen(CASI_CANONICAL_SCHEME);
    /* A legacy multi-component project name has no unambiguous destination
     * rule. Keep it unmapped rather than place transcripts under a wrong dir. */
    if (*name == '\0' || strchr(name, '/') != NULL ||
        root_path_by_name(roots, name) != NULL)
        return CASI_OK;
    home = casi_fs_home();
    if (home == NULL)
        return casi_error_last_code();
    search.wanted = name;
    /* Search likely checkout locations only. Traversing the entire home on a
     * real Mac can exhaust the bound inside Downloads or backup archives
     * before reaching ~/src, and then a valid project looks unmapped. */
    for (i = 0; locations[i] != NULL && !search.ambiguous; i++) {
        casi_buf_clear(&base);
        if ((rc = casi_fs_join(&base, home, locations[i])) != CASI_OK)
            goto done;
        if ((rc = search_project_dir(casi_buf_cstr(&base), 0, &search)) != CASI_OK)
            goto done;
    }
    /* A checkout may also live directly below HOME. */
    if (!search.ambiguous) {
        casi_stat st;

        casi_buf_clear(&base);
        if ((rc = casi_fs_join(&base, home, name)) != CASI_OK)
            goto done;
        rc = casi_fs_stat(casi_buf_cstr(&base), &st);
        if (rc == CASI_ENOTFOUND) {
            casi_error_clear();
            rc = CASI_OK;
        } else if (rc == CASI_OK && st.is_dir) {
            if (search.found.len > 0)
                search.ambiguous = true;
            else
                rc = casi_buf_puts(&search.found, casi_buf_cstr(&base));
        }
        if (rc != CASI_OK)
            goto done;
    }
    casi_debug("project discovery %s: candidate=%s visited=%zu ambiguous=%d incomplete=%d",
               canonical, casi_buf_cstr(&search.found), search.visited,
               search.ambiguous, search.incomplete);
    if (!search.ambiguous && !search.incomplete && search.found.len > 0)
        rc = casi_roots_add(roots, name, casi_buf_cstr(&search.found));

done:
    casi_buf_dispose(&search.found);
    casi_buf_dispose(&base);
    return rc;
}

/* Splits "casi://<name>/<rest>" and looks the name up. */
static const struct root_entry *lookup_canonical(const casi_roots *roots,
                                                 const char *canonical,
                                                 size_t *name_len_out)
{
    const char *name = canonical + strlen(CASI_CANONICAL_SCHEME);
    const char *slash = strchr(name, '/');
    size_t name_len = slash != NULL ? (size_t)(slash - name) : strlen(name);
    size_t i;

    *name_len_out = name_len;

    for (i = 0; i < roots->len; i++)
        if (!roots->items[i].alias && strlen(roots->items[i].name) == name_len &&
            memcmp(roots->items[i].name, name, name_len) == 0)
            return &roots->items[i];

    return NULL;
}

int casi_roots_denormalize_path(const casi_roots *roots, const char *canonical,
                                casi_buf *out)
{
    const struct root_entry *match;
    size_t name_len, prefix_len;
    int rc;

    casi_buf_clear(out);

    if (!casi_str_has_prefix(canonical, CASI_CANONICAL_SCHEME))
        return casi_buf_puts(out, canonical);

    match = lookup_canonical(roots, canonical, &name_len);
    if (match == NULL)
        return casi_error_set(CASI_EUNMAPPED,
                              "project \"%.*s\" is on the remote but not on this machine yet",
                              (int)name_len,
                              canonical + strlen(CASI_CANONICAL_SCHEME));

    if ((rc = casi_buf_puts(out, match->path)) != CASI_OK)
        return rc;

    prefix_len = strlen(CASI_CANONICAL_SCHEME) + name_len;
    return casi_buf_puts(out, canonical + prefix_len);
}

int casi_roots_normalize_text(const casi_roots *roots, const casi_buf *in, casi_buf *out)
{
    const char *p = in->ptr != NULL ? in->ptr : "";
    size_t len = in->len, i = 0, span_start = 0;
    int rc;

    casi_buf_clear(out);
    if ((rc = casi_buf_grow(out, len)) != CASI_OK)
        return rc;

    while (i < len) {
        const struct root_entry *match;

        /* Cheap rejection: almost every byte cannot start a root. */
        if (!roots->first_byte[(unsigned char)p[i]]) {
            i++;
            continue;
        }

        match = find_match(roots, p + i, len - i, CASI_MATCH_TEXT);
        if (match == NULL) {
            i++;
            continue;
        }

        /* Flush the untouched span, then the replacement. */
        if ((rc = casi_buf_put(out, p + span_start, i - span_start)) != CASI_OK)
            return rc;
        if ((rc = casi_buf_printf(out, CASI_CANONICAL_SCHEME "%s", match->name)) != CASI_OK)
            return rc;

        i += match->path_len;
        span_start = i;
    }

    return casi_buf_put(out, p + span_start, len - span_start);
}

int casi_roots_denormalize_text(const casi_roots *roots, const casi_buf *in,
                                casi_buf *out, casi_buf *unmapped_out)
{
    static const char scheme[] = CASI_CANONICAL_SCHEME;
    const size_t scheme_len = sizeof(scheme) - 1;
    const char *p = in->ptr != NULL ? in->ptr : "";
    size_t len = in->len, i = 0, span_start = 0;
    int rc;

    casi_buf_clear(out);
    if (unmapped_out != NULL)
        casi_buf_clear(unmapped_out);
    if ((rc = casi_buf_grow(out, len)) != CASI_OK)
        return rc;

    while (i < len) {
        const struct root_entry *match;
        const char *name;
        size_t name_len, j;

        if (p[i] != 'c' || len - i < scheme_len ||
            memcmp(p + i, scheme, scheme_len) != 0) {
            i++;
            continue;
        }

        /* Use the same component rule as normalize_text(): anything that can
         * terminate a local path also terminates the canonical root token.
         * Keeping both directions symmetric avoids treating punctuation such
         * as ')' as part of an otherwise known root name. */
        name = p + i + scheme_len;
        name_len = 0;
        while (name + name_len < p + len &&
               continues_path_component(name[name_len]))
            name_len++;

        /* The scheme can appear as prose (for example, in a message that
         * explains how casi works). Without a root name it is not a canonical
         * path, so leave it alone rather than reporting an empty root. */
        if (name_len == 0) {
            i += scheme_len;
            continue;
        }

        match = NULL;
        for (j = 0; j < roots->len; j++)
            if (!roots->items[j].alias && strlen(roots->items[j].name) == name_len &&
                memcmp(roots->items[j].name, name, name_len) == 0) {
                match = &roots->items[j];
                break;
            }

        if (match == NULL) {
            if (unmapped_out != NULL &&
                (rc = casi_buf_put(unmapped_out, name, name_len)) != CASI_OK)
                return rc;
            return casi_error_set(CASI_EUNMAPPED,
                                  "project \"%.*s\" is on the remote but not on this machine yet",
                                  (int)name_len, name);
        }

        if ((rc = casi_buf_put(out, p + span_start, i - span_start)) != CASI_OK)
            return rc;
        if ((rc = casi_buf_puts(out, match->path)) != CASI_OK)
            return rc;

        i += scheme_len + name_len;
        span_start = i;
    }

    return casi_buf_put(out, p + span_start, len - span_start);
}
