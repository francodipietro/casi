/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/sync.h"
#include "casi/error.h"
#include "casi/str.h"
#include <stdbool.h>
#include <git2.h>
#include <stdlib.h>
#include <string.h>

/* Old stores may have used a different root name for the same project. Keep
 * this exceptional comparison bounded; the ordinary OID path stays cheap. */
#define LEGACY_COMPARE_LIMIT (256u * 1024u * 1024u)

static bool is_prefix(const casi_buf *prefix, const casi_buf *whole)
{
    return prefix->len <= whole->len &&
           (prefix->len == 0 || memcmp(prefix->ptr, whole->ptr, prefix->len) == 0);
}

static bool home_component(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_' || c == '-' ||
           c == '.' || c == '@' || c == '+' || c >= 0x80;
}

static int substitute_home(const casi_buf *source, const char *old_home,
                           const char *new_home, casi_buf *out)
{
    size_t old_len = strlen(old_home), i = 0, start = 0;
    int rc;

    while (i + old_len <= source->len) {
        if (memcmp(source->ptr + i, old_home, old_len) != 0 ||
            (i + old_len < source->len &&
             home_component((unsigned char)source->ptr[i + old_len]))) {
            i++;
            continue;
        }
        if ((rc = casi_buf_put(out, source->ptr + start, i - start)) != CASI_OK ||
            (rc = casi_buf_puts(out, new_home)) != CASI_OK)
            return rc;
        i += old_len;
        start = i;
    }
    return casi_buf_put(out, source->ptr + start, source->len - start);
}

/* Some transcripts imported by an old release retain the other machine's
 * home path inside attachments. This is comparison-only: live files are never
 * rewritten. An exact equality/prefix after one observed home substitution
 * is the only automatic resolution. */
static int compare_foreign_home(const casi_roots *roots, const casi_buf *local,
                                const casi_buf *remote, casi_sync_relation *out)
{
    static const char *starts[] = { "/home/", "/Users/", NULL };
    casi_buf home = CASI_BUF_INIT, translated = CASI_BUF_INIT;
    char *tried[16] = { 0 };
    size_t tried_count = 0, i, k;
    int rc;

    rc = casi_roots_denormalize_path(roots,
                                     CASI_CANONICAL_SCHEME CASI_HOME_ROOT, &home);
    if (rc == CASI_EUNMAPPED) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    for (i = 0; i < local->len && tried_count < 16; i++) {
        size_t start_len = 0, end;
        char *candidate;
        bool seen = false;

        for (k = 0; starts[k] != NULL; k++) {
            size_t n = strlen(starts[k]);

            if (i + n < local->len &&
                memcmp(local->ptr + i, starts[k], n) == 0) {
                start_len = n;
                break;
            }
        }
        if (start_len == 0)
            continue;
        end = i + start_len;
        while (end < local->len && home_component((unsigned char)local->ptr[end]))
            end++;
        if (end == i + start_len)
            continue;
        candidate = casi_strndup(local->ptr + i, end - i);
        if (candidate == NULL) {
            rc = CASI_ENOMEM;
            goto done;
        }
        if (strcmp(candidate, casi_buf_cstr(&home)) == 0)
            seen = true;
        for (k = 0; k < tried_count; k++)
            if (strcmp(tried[k], candidate) == 0)
                seen = true;
        if (seen) {
            free(candidate);
            continue;
        }
        tried[tried_count++] = candidate;
        casi_buf_clear(&translated);
        if ((rc = substitute_home(local, candidate, casi_buf_cstr(&home),
                                  &translated)) != CASI_OK)
            goto done;
        if (translated.len == remote->len && is_prefix(&translated, remote)) {
            *out = CASI_SYNC_SAME;
            break;
        }
        if (is_prefix(remote, &translated)) {
            *out = CASI_SYNC_LOCAL_AHEAD;
            break;
        }
    }

done:
    for (k = 0; k < tried_count; k++)
        free(tried[k]);
    casi_buf_dispose(&home);
    casi_buf_dispose(&translated);
    return rc;
}

/* A checkout moved from src/work/bookit to src/bookit can leave references
 * to sibling directories under src/work in old transcripts. Try dropping one
 * observed intermediate directory in the remote copy, but only as proof of
 * exact equality or a local append. Never use this to materialize files. */
static int compare_parent_layout(const casi_roots *roots,
                                 const casi_entry *local_entry,
                                 const casi_buf *local, const casi_buf *remote,
                                 casi_sync_relation *out)
{
    casi_buf project = CASI_BUF_INIT, rewritten = CASI_BUF_INIT;
    char *tried[16] = { 0 };
    char *parent = NULL;
    size_t tried_count = 0, parent_len, i, k;
    int rc;

    rc = casi_roots_denormalize_path(roots, local_entry->project_path, &project);
    if (rc == CASI_EUNMAPPED) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;
    parent_len = project.len;
    while (parent_len > 0 && project.ptr[parent_len - 1] != '/')
        parent_len--;
    if (parent_len <= 1)
        goto done;
    parent_len--;
    parent = casi_strndup(project.ptr, parent_len);
    if (parent == NULL) {
        rc = CASI_ENOMEM;
        goto done;
    }

    for (i = 0; i + parent_len + 2 < remote->len && tried_count < 16; i++) {
        size_t end;
        char *candidate;
        bool seen = false;

        if (memcmp(remote->ptr + i, project.ptr, parent_len) != 0 ||
            remote->ptr[i + parent_len] != '/')
            continue;
        end = i + parent_len + 1;
        while (end < remote->len &&
               home_component((unsigned char)remote->ptr[end]))
            end++;
        if (end == i + parent_len + 1)
            continue;
        candidate = casi_strndup(remote->ptr + i, end - i);
        if (candidate == NULL) {
            rc = CASI_ENOMEM;
            goto done;
        }
        if (strcmp(candidate, casi_buf_cstr(&project)) == 0)
            seen = true;
        for (k = 0; k < tried_count; k++)
            if (strcmp(tried[k], candidate) == 0)
                seen = true;
        if (seen) {
            free(candidate);
            continue;
        }
        tried[tried_count++] = candidate;
        casi_buf_clear(&rewritten);
        if ((rc = substitute_home(remote, candidate, parent,
                                  &rewritten)) != CASI_OK)
            goto done;
        if (local->len == rewritten.len && is_prefix(local, &rewritten)) {
            *out = CASI_SYNC_SAME;
            break;
        }
        if (is_prefix(&rewritten, local)) {
            *out = CASI_SYNC_LOCAL_AHEAD;
            break;
        }
        if ((rc = compare_foreign_home(roots, local, &rewritten, out)) != CASI_OK ||
            *out != CASI_SYNC_DIVERGED)
            break;
    }

done:
    for (k = 0; k < tried_count; k++)
        free(tried[k]);
    free(parent);
    casi_buf_dispose(&project);
    casi_buf_dispose(&rewritten);
    return rc;
}

/* A former checkout may have had one extra directory below the stable project
 * root (src/work became src). Try removing one complete component only while
 * proving equality/prefix; it is never used to write a transcript. */
static int compare_child_layout(const casi_roots *roots,
                                const casi_entry *local_entry,
                                const casi_buf *local, const casi_buf *remote,
                                casi_sync_relation *out)
{
    casi_buf project = CASI_BUF_INIT, rewritten = CASI_BUF_INIT;
    char *tried[16] = { 0 };
    size_t project_len, tried_count = 0, i, k;
    int rc;

    rc = casi_roots_denormalize_path(roots, local_entry->project_path, &project);
    if (rc == CASI_EUNMAPPED) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;
    project_len = project.len;

    for (i = 0; i + project_len + 2 < remote->len && tried_count < 16; i++) {
        size_t end;
        char *candidate;
        bool seen = false;

        if (memcmp(remote->ptr + i, project.ptr, project_len) != 0 ||
            remote->ptr[i + project_len] != '/')
            continue;
        end = i + project_len + 1;
        while (end < remote->len &&
               home_component((unsigned char)remote->ptr[end]))
            end++;
        if (end == i + project_len + 1)
            continue;
        candidate = casi_strndup(remote->ptr + i, end - i);
        if (candidate == NULL) {
            rc = CASI_ENOMEM;
            goto done;
        }
        for (k = 0; k < tried_count; k++)
            if (strcmp(tried[k], candidate) == 0)
                seen = true;
        if (seen) {
            free(candidate);
            continue;
        }
        tried[tried_count++] = candidate;
        casi_buf_clear(&rewritten);
        if ((rc = substitute_home(remote, candidate, casi_buf_cstr(&project),
                                  &rewritten)) != CASI_OK)
            goto done;
        if (local->len == rewritten.len && is_prefix(local, &rewritten)) {
            *out = CASI_SYNC_SAME;
            break;
        }
        if (is_prefix(&rewritten, local)) {
            *out = CASI_SYNC_LOCAL_AHEAD;
            break;
        }
        if ((rc = compare_foreign_home(roots, local, &rewritten, out)) != CASI_OK ||
            *out != CASI_SYNC_DIVERGED)
            break;
    }

done:
    for (k = 0; k < tried_count; k++)
        free(tried[k]);
    casi_buf_dispose(&project);
    casi_buf_dispose(&rewritten);
    return rc;
}

static int compare_materialized(const casi_roots *roots,
                                const casi_entry *local_entry,
                                const casi_buf *local, const casi_buf *remote,
                                casi_sync_relation *out)
{
    int rc = CASI_OK;

    if (local->len == remote->len && is_prefix(local, remote))
        *out = CASI_SYNC_SAME;
    else if (is_prefix(remote, local))
        *out = CASI_SYNC_LOCAL_AHEAD;
    else
        rc = compare_foreign_home(roots, local, remote, out);
    if (rc == CASI_OK && *out == CASI_SYNC_DIVERGED)
        rc = compare_parent_layout(roots, local_entry, local, remote, out);
    if (rc == CASI_OK && *out == CASI_SYNC_DIVERGED)
        rc = compare_child_layout(roots, local_entry, local, remote, out);
    return rc;
}

static int compare_legacy_project(casi_repo *repo, const casi_roots *roots,
                                  const casi_entry *local,
                                  const casi_entry *remote,
                                  casi_sync_relation *out)
{
    casi_buf l = CASI_BUF_INIT, r = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;
    casi_buf project = CASI_BUF_INIT, child = CASI_BUF_INIT;
    char *missing = NULL, *parent = NULL;
    int rc = CASI_OK;

    if (roots == NULL || local->project_path == NULL || remote->project_path == NULL ||
        local->session_id == NULL || remote->session_id == NULL ||
        strcmp(local->session_id, remote->session_id) != 0 ||
        local->bytes > LEGACY_COMPARE_LIMIT || remote->bytes > LEGACY_COMPARE_LIMIT)
        return CASI_OK;

    rc = casi_store_materialize_content(repo, roots, local, &l, &unmapped);
    if (rc == CASI_EUNMAPPED) {
        casi_error_clear();
        missing = casi_strdup(casi_buf_cstr(&unmapped));
        if (missing == NULL) {
            rc = CASI_ENOMEM;
            goto done;
        }
        rc = casi_roots_denormalize_path(roots, local->project_path, &project);
        if (rc == CASI_EUNMAPPED) {
            casi_error_clear();
            rc = CASI_OK;
            goto done;
        }
        if (rc != CASI_OK)
            goto done;
        casi_buf_clear(&l);
        rc = casi_store_materialize_content_with_root(repo, roots, local,
                                                       missing,
                                                       casi_buf_cstr(&project),
                                                       &l, &unmapped);
        free(missing);
        missing = NULL;
        if (rc == CASI_EUNMAPPED) {
            casi_error_clear();
            rc = CASI_OK;
            goto done;
        }
    }
    if (rc != CASI_OK)
        goto done;
    rc = casi_store_materialize_content(repo, roots, remote, &r, &unmapped);
    if (rc == CASI_EUNMAPPED) {
        casi_error_clear();
        missing = casi_strdup(casi_buf_cstr(&unmapped));
        if (missing == NULL) {
            rc = CASI_ENOMEM;
            goto done;
        }
        rc = casi_roots_denormalize_path(roots, local->project_path, &project);
        if (rc == CASI_EUNMAPPED) {
            casi_error_clear();
            rc = CASI_OK;
            goto done;
        }
        if (rc != CASI_OK)
            goto done;
        {
            const char *slash = strrchr(casi_buf_cstr(&project), '/');

            if (slash != NULL && slash > project.ptr)
                parent = casi_strndup(project.ptr, (size_t)(slash - project.ptr));
        }
        if (parent == NULL) {
            rc = CASI_ENOMEM;
            goto done;
        }
        casi_buf_clear(&r);
        rc = casi_store_materialize_content_with_root(repo, roots, remote,
                                                       missing, parent, &r,
                                                       &unmapped);
        if (rc == CASI_EUNMAPPED) {
            casi_error_clear();
            rc = CASI_OK;
        } else if (rc == CASI_OK) {
            rc = compare_materialized(roots, local, &l, &r, out);
        }
        if (rc != CASI_OK || *out != CASI_SYNC_DIVERGED)
            goto done;
        casi_buf_clear(&r);
        rc = casi_store_materialize_content_with_root(repo, roots, remote,
                                                       missing,
                                                       casi_buf_cstr(&project),
                                                       &r, &unmapped);
        if (rc == CASI_EUNMAPPED) {
            casi_error_clear();
            rc = CASI_OK;
        } else if (rc == CASI_OK) {
            rc = compare_materialized(roots, local, &l, &r, out);
        }
        if (rc != CASI_OK || *out != CASI_SYNC_DIVERGED)
            goto done;
        if ((rc = casi_buf_printf(&child, "%s/%s", casi_buf_cstr(&project),
                                  missing)) != CASI_OK)
            goto done;
        casi_buf_clear(&r);
        rc = casi_store_materialize_content_with_root(repo, roots, remote,
                                                       missing,
                                                       casi_buf_cstr(&child),
                                                       &r, &unmapped);
        if (rc == CASI_EUNMAPPED) {
            casi_error_clear();
            rc = CASI_OK;
        } else if (rc == CASI_OK) {
            rc = compare_materialized(roots, local, &l, &r, out);
        }
        goto done;
    }
    if (rc != CASI_OK)
        goto done;
    rc = compare_materialized(roots, local, &l, &r, out);
    /* A remote-ahead legacy transcript needs path rebasing at materialization
     * as well. Until that is implemented, never auto-overwrite the local. */

done:
    casi_buf_dispose(&l);
    casi_buf_dispose(&r);
    casi_buf_dispose(&unmapped);
    casi_buf_dispose(&project);
    casi_buf_dispose(&child);
    free(missing);
    free(parent);
    return rc;
}

int casi_sync_compare(casi_repo *repo, const casi_roots *roots,
                      const casi_entry *local,
                      const casi_entry *remote, casi_sync_relation *out)
{
    casi_buf local_tail = CASI_BUF_INIT, remote_tail = CASI_BUF_INIT;
    size_t shorter, i;
    int rc = CASI_OK;

    if (repo == NULL || local == NULL || remote == NULL || out == NULL)
        return casi_error_set(CASI_EINVAL, "invalid sync comparison");

    shorter = local->chunk_count < remote->chunk_count
            ? local->chunk_count : remote->chunk_count;

    for (i = 0; i < shorter; i++) {
        if (git_oid_equal(&local->chunks[i], &remote->chunks[i]))
            continue;

        /* A mismatch before the shorter side's final chunk cannot be an
         * append: that chunk was already sealed on both sides. */
        if (i + 1 < shorter) {
            *out = CASI_SYNC_DIVERGED;
            return compare_legacy_project(repo, roots, local, remote, out);
        }

        /* The same index is the only mutable blob that can establish an
         * append. Do not reassemble later chunks: they cannot affect whether
         * the shorter side ends inside this one, and could be arbitrarily
         * large under a genuine rewrite. */
        if ((rc = casi_repo_read_blob(repo, &local->chunks[i], &local_tail)) != CASI_OK)
            goto done;
        if ((rc = casi_repo_read_blob(repo, &remote->chunks[i], &remote_tail)) != CASI_OK)
            goto done;

        if (local->chunk_count < remote->chunk_count)
            *out = is_prefix(&local_tail, &remote_tail)
                 ? CASI_SYNC_REMOTE_AHEAD : CASI_SYNC_DIVERGED;
        else if (remote->chunk_count < local->chunk_count)
            *out = is_prefix(&remote_tail, &local_tail)
                 ? CASI_SYNC_LOCAL_AHEAD : CASI_SYNC_DIVERGED;
        else if (is_prefix(&local_tail, &remote_tail))
            *out = CASI_SYNC_REMOTE_AHEAD;
        else if (is_prefix(&remote_tail, &local_tail))
            *out = CASI_SYNC_LOCAL_AHEAD;
        else
            *out = CASI_SYNC_DIVERGED;
        if (*out == CASI_SYNC_DIVERGED)
            rc = compare_legacy_project(repo, roots, local, remote, out);
        goto done;
    }

    if (local->chunk_count == remote->chunk_count)
        *out = CASI_SYNC_SAME;
    else
        *out = local->chunk_count > remote->chunk_count
             ? CASI_SYNC_LOCAL_AHEAD : CASI_SYNC_REMOTE_AHEAD;

done:
    casi_buf_dispose(&local_tail);
    casi_buf_dispose(&remote_tail);
    return rc;
}

const char *casi_sync_relation_name(casi_sync_relation relation)
{
    switch (relation) {
    case CASI_SYNC_SAME:         return "up to date";
    case CASI_SYNC_LOCAL_AHEAD:  return "local is ahead";
    case CASI_SYNC_REMOTE_AHEAD: return "remote is ahead";
    case CASI_SYNC_DIVERGED:     return "diverged";
    }
    return "unknown";
}

size_t casi_sync_common_prefix(const casi_entry *a, const casi_entry *b)
{
    size_t shorter = a->chunk_count < b->chunk_count
                   ? a->chunk_count : b->chunk_count;
    size_t i;

    for (i = 0; i < shorter; i++)
        if (!git_oid_equal(&a->chunks[i], &b->chunks[i]))
            return i;

    return shorter;
}
