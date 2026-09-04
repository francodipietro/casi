/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/store.h"
#include "casi/casi.h"
#include "casi/chunk.h"
#include "casi/jsonl.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* --- entry bookkeeping ------------------------------------------------ */

static int entry_list_push(casi_entry_list *list, const casi_entry *entry);

static void entry_dispose(casi_entry *entry)
{
    free(entry->session_id);
    free(entry->project_path);
    free(entry->project_id);
    free(entry->chunks);
    memset(entry, 0, sizeof(*entry));
}

void casi_entry_list_dispose(casi_entry_list *list)
{
    size_t i;

    for (i = 0; i < list->len; i++)
        entry_dispose(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

void casi_entry_dispose_one(casi_entry *entry)
{
    entry_dispose(entry);
}

void casi_entry_swap(casi_entry *a, casi_entry *b)
{
    casi_entry tmp = *a;

    *a = *b;
    *b = tmp;
}

casi_entry *casi_entry_list_find(casi_entry_list *list, const char *session_id)
{
    size_t i;

    for (i = 0; i < list->len; i++)
        if (strcmp(list->items[i].session_id, session_id) == 0)
            return &list->items[i];

    return NULL;
}

int casi_entry_list_push_owned(casi_entry_list *list, casi_entry *entry)
{
    int rc = entry_list_push(list, entry);

    if (rc == CASI_OK)
        memset(entry, 0, sizeof(*entry));

    return rc;
}

static int entry_list_push(casi_entry_list *list, const casi_entry *entry)
{
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 16;
        casi_entry *p = realloc(list->items, cap * sizeof(*p));

        if (p == NULL)
            return casi_error_set(CASI_ENOMEM, "out of memory listing entries");
        list->items = p;
        list->cap = cap;
    }

    list->items[list->len++] = *entry;
    return CASI_OK;
}

/* --- paths within the tree -------------------------------------------- */

static int session_dir(casi_buf *out, const char *provider, const casi_entry *e)
{
    casi_buf_clear(out);
    return casi_buf_printf(out, "sessions/%s/%s/%s",
                           provider, e->project_id, e->session_id);
}

static int chunk_path(casi_buf *out, const char *dir, size_t index)
{
    casi_buf_clear(out);
    /* Fixed width so the tree sorts in chunk order, which is also read order. */
    return casi_buf_printf(out, "%s/chunks/%06zu", dir, index);
}

/* --- writing ---------------------------------------------------------- */

struct chunk_writer {
    casi_repo *repo;
    git_oid   *oids;
    size_t     capacity;
    size_t     written;
};

static int write_one_chunk(const void *data, size_t len, size_t index, void *payload)
{
    struct chunk_writer *w = payload;

    if (index >= w->capacity)
        return casi_error_set(CASI_ERROR, "chunk count changed mid-write");

    w->written = index + 1;
    return casi_repo_write_blob(w->repo, data, len, &w->oids[index]);
}

static bool is_excluded(const casi_strvec *exclude, const char *project_path)
{
    size_t i;

    if (exclude == NULL)
        return false;

    for (i = 0; i < exclude->len; i++)
        if (strcmp(exclude->items[i], project_path) == 0)
            return true;

    return false;
}

static int scan_one_session(casi_repo *repo, const casi_roots *roots,
                            const casi_session *session, casi_entry_list *out)
{
    casi_buf raw = CASI_BUF_INIT, normalized = CASI_BUF_INIT;
    struct chunk_writer writer;
    casi_entry entry;
    size_t count;
    int rc;

    memset(&entry, 0, sizeof(entry));
    memset(&writer, 0, sizeof(writer));

    if ((rc = casi_fs_read_file(session->local_path, &raw)) != CASI_OK)
        goto done;

    /* Normalise first, chunk second: this ordering is what makes two machines
     * with different layouts produce identical blobs. */
    if ((rc = casi_roots_normalize_text(roots, &raw, &normalized)) != CASI_OK)
        goto done;

    count = casi_chunk_count(normalized.ptr, normalized.len, CASI_CHUNK_TARGET);

    if (count > 0) {
        writer.repo = repo;
        writer.capacity = count;
        writer.oids = calloc(count, sizeof(git_oid));
        if (writer.oids == NULL) {
            rc = casi_error_set(CASI_ENOMEM, "out of memory chunking %s",
                                session->session_id);
            goto done;
        }

        if ((rc = casi_chunk_split(normalized.ptr, normalized.len,
                                   CASI_CHUNK_TARGET, write_one_chunk,
                                   &writer)) != CASI_OK)
            goto done;
    }

    entry.session_id   = casi_strdup(session->session_id);
    entry.project_path = casi_strdup(session->project_path);
    entry.project_id   = casi_strdup(session->project_id);
    entry.chunks       = writer.oids;
    entry.chunk_count  = count;
    entry.bytes        = (uint64_t)normalized.len;

    if (entry.session_id == NULL || entry.project_path == NULL ||
        entry.project_id == NULL) {
        rc = CASI_ENOMEM;
        goto done;
    }

    writer.oids = NULL;  /* ownership moved into the entry */
    if ((rc = entry_list_push(out, &entry)) != CASI_OK)
        goto done;

    memset(&entry, 0, sizeof(entry));
    rc = CASI_OK;

done:
    entry_dispose(&entry);
    free(writer.oids);
    casi_buf_dispose(&raw);
    casi_buf_dispose(&normalized);
    return rc;
}

int casi_store_scan_local(casi_repo *repo, const casi_roots *roots,
                          const casi_provider *provider,
                          const casi_strvec *exclude,
                          casi_entry_list *out, size_t *excluded_out)
{
    casi_session_list sessions;
    size_t i, skipped = 0;
    int rc;

    memset(&sessions, 0, sizeof(sessions));

    if ((rc = provider->discover(roots, &sessions)) != CASI_OK)
        goto done;

    for (i = 0; i < sessions.len; i++) {
        if (is_excluded(exclude, sessions.items[i].project_path)) {
            casi_verbose("excluded: %s", sessions.items[i].project_path);
            skipped++;
            continue;
        }

        if ((rc = scan_one_session(repo, roots, &sessions.items[i], out)) != CASI_OK)
            goto done;
    }

    rc = CASI_OK;

done:
    if (excluded_out != NULL)
        *excluded_out = skipped;
    casi_session_list_dispose(&sessions);
    return rc;
}

int casi_store_write_tree(casi_repo *repo, const char *provider_name,
                          const casi_entry_list *entries, git_oid *tree_out)
{
    casi_tree *tree = NULL;
    casi_buf dir = CASI_BUF_INIT, path = CASI_BUF_INIT, meta = CASI_BUF_INIT;
    casi_buf session_json = CASI_BUF_INIT, project_json = CASI_BUF_INIT;
    size_t i, c;
    int rc;

    if ((rc = casi_tree_new(repo, &tree)) != CASI_OK)
        return rc;

    casi_buf_clear(&meta);
    if ((rc = casi_buf_printf(&meta, "{\"format\":%d}\n", CASI_FORMAT_VERSION)) != CASI_OK)
        goto done;
    if ((rc = casi_tree_add_text(tree, "casi.json", casi_buf_cstr(&meta))) != CASI_OK)
        goto done;

    for (i = 0; i < entries->len; i++) {
        const casi_entry *e = &entries->items[i];

        if ((rc = session_dir(&dir, provider_name, e)) != CASI_OK)
            goto done;

        if ((rc = casi_json_escape_string(e->session_id, &session_json)) != CASI_OK)
            goto done;
        if ((rc = casi_json_escape_string(e->project_path, &project_json)) != CASI_OK)
            goto done;

        casi_buf_clear(&meta);
        rc = casi_buf_printf(&meta,
                             "{\"sessionId\":\"%s\","
                             "\"projectPath\":\"%s\","
                             "\"chunks\":%zu,"
                             "\"bytes\":%" PRIu64 "}\n",
                             casi_buf_cstr(&session_json), casi_buf_cstr(&project_json),
                             e->chunk_count, e->bytes);
        if (rc != CASI_OK)
            goto done;

        casi_buf_clear(&path);
        if ((rc = casi_buf_printf(&path, "%s/meta.json", casi_buf_cstr(&dir))) != CASI_OK)
            goto done;
        if ((rc = casi_tree_add_text(tree, casi_buf_cstr(&path),
                                     casi_buf_cstr(&meta))) != CASI_OK)
            goto done;

        for (c = 0; c < e->chunk_count; c++) {
            if ((rc = chunk_path(&path, casi_buf_cstr(&dir), c)) != CASI_OK)
                goto done;
            if ((rc = casi_tree_add(tree, casi_buf_cstr(&path), &e->chunks[c])) != CASI_OK)
                goto done;
        }
    }

    rc = casi_tree_write(tree, tree_out);

done:
    casi_tree_free(tree);
    casi_buf_dispose(&dir);
    casi_buf_dispose(&path);
    casi_buf_dispose(&meta);
    casi_buf_dispose(&session_json);
    casi_buf_dispose(&project_json);
    return rc;
}

/* --- reading ---------------------------------------------------------- */

static int read_session_entry(casi_repo *repo, const git_oid *tree,
                              const char *provider_name, const char *project_id,
                              const char *session_id, casi_entry_list *out)
{
    casi_buf path = CASI_BUF_INIT, meta = CASI_BUF_INIT, value = CASI_BUF_INIT;
    casi_strvec chunk_names = CASI_STRVEC_INIT;
    casi_entry entry;
    size_t i;
    int rc;

    memset(&entry, 0, sizeof(entry));

    if ((rc = casi_buf_printf(&path, "sessions/%s/%s/%s/meta.json",
                              provider_name, project_id, session_id)) != CASI_OK)
        goto done;
    if ((rc = casi_repo_tree_entry_blob(repo, tree, casi_buf_cstr(&path), &meta)) != CASI_OK)
        goto done;

    if (!casi_json_find_string(casi_buf_cstr(&meta), meta.len, "projectPath", &value)) {
        rc = casi_error_set(CASI_EINVAL, "malformed metadata for session %s", session_id);
        goto done;
    }

    entry.session_id   = casi_strdup(session_id);
    entry.project_id   = casi_strdup(project_id);
    entry.project_path = casi_strdup(casi_buf_cstr(&value));
    entry.bytes        = casi_json_find_uint(casi_buf_cstr(&meta), meta.len, "bytes");
    if (entry.session_id == NULL || entry.project_id == NULL ||
        entry.project_path == NULL) {
        rc = CASI_ENOMEM;
        goto done;
    }

    casi_buf_clear(&path);
    if ((rc = casi_buf_printf(&path, "sessions/%s/%s/%s/chunks",
                              provider_name, project_id, session_id)) != CASI_OK)
        goto done;

    rc = casi_repo_tree_list(repo, tree, casi_buf_cstr(&path), &chunk_names);
    if (rc == CASI_ENOTFOUND) {
        /* A session with no chunks at all: an empty transcript. */
        casi_error_clear();
        rc = CASI_OK;
    } else if (rc != CASI_OK) {
        goto done;
    } else {
        entry.chunks = calloc(chunk_names.len, sizeof(git_oid));
        if (entry.chunks == NULL && chunk_names.len > 0) {
            rc = casi_error_set(CASI_ENOMEM, "out of memory reading %s", session_id);
            goto done;
        }

        for (i = 0; i < chunk_names.len; i++) {
            casi_buf_clear(&path);
            rc = casi_buf_printf(&path, "sessions/%s/%s/%s/chunks/%s",
                                 provider_name, project_id, session_id,
                                 chunk_names.items[i]);
            if (rc != CASI_OK)
                goto done;
            if ((rc = casi_repo_tree_entry_oid(repo, tree, casi_buf_cstr(&path),
                                               &entry.chunks[i])) != CASI_OK)
                goto done;
        }
        entry.chunk_count = chunk_names.len;
    }

    if ((rc = entry_list_push(out, &entry)) != CASI_OK)
        goto done;

    memset(&entry, 0, sizeof(entry));
    rc = CASI_OK;

done:
    entry_dispose(&entry);
    casi_strvec_dispose(&chunk_names);
    casi_buf_dispose(&path);
    casi_buf_dispose(&meta);
    casi_buf_dispose(&value);
    return rc;
}

int casi_store_read_tree(casi_repo *repo, const git_oid *tree,
                         const char *provider_name, casi_entry_list *out)
{
    casi_buf path = CASI_BUF_INIT;
    casi_strvec project_ids = CASI_STRVEC_INIT, session_ids = CASI_STRVEC_INIT;
    size_t i, j;
    int rc;

    if ((rc = casi_buf_printf(&path, "sessions/%s", provider_name)) != CASI_OK)
        goto done;

    rc = casi_repo_tree_list(repo, tree, casi_buf_cstr(&path), &project_ids);
    if (rc == CASI_ENOTFOUND) {
        /* Nothing from this provider in that tree yet. */
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    for (i = 0; i < project_ids.len; i++) {
        casi_strvec_dispose(&session_ids);
        memset(&session_ids, 0, sizeof(session_ids));

        casi_buf_clear(&path);
        if ((rc = casi_buf_printf(&path, "sessions/%s/%s",
                                  provider_name, project_ids.items[i])) != CASI_OK)
            goto done;
        if ((rc = casi_repo_tree_list(repo, tree, casi_buf_cstr(&path),
                                      &session_ids)) != CASI_OK)
            goto done;

        for (j = 0; j < session_ids.len; j++)
            if ((rc = read_session_entry(repo, tree, provider_name,
                                         project_ids.items[i],
                                         session_ids.items[j], out)) != CASI_OK)
                goto done;
    }

    rc = CASI_OK;

done:
    casi_strvec_dispose(&project_ids);
    casi_strvec_dispose(&session_ids);
    casi_buf_dispose(&path);
    return rc;
}

/* --- materialising ---------------------------------------------------- */

static int reassemble(casi_repo *repo, const casi_roots *roots,
                      const casi_entry *entry, casi_buf *out, casi_buf *unmapped_out)
{
    casi_buf joined = CASI_BUF_INIT, piece = CASI_BUF_INIT;
    size_t i;
    int rc = CASI_OK;

    for (i = 0; i < entry->chunk_count; i++) {
        if ((rc = casi_repo_read_blob(repo, &entry->chunks[i], &piece)) != CASI_OK)
            goto done;
        if ((rc = casi_buf_put(&joined, piece.ptr, piece.len)) != CASI_OK)
            goto done;
    }

    rc = casi_roots_denormalize_text(roots, &joined, out, unmapped_out);

done:
    casi_buf_dispose(&joined);
    casi_buf_dispose(&piece);
    return rc;
}

int casi_store_materialize_to(casi_repo *repo, const casi_roots *roots,
                              const casi_entry *entry, const char *path,
                              casi_buf *unmapped_out)
{
    casi_buf content = CASI_BUF_INIT;
    int rc;

    if ((rc = reassemble(repo, roots, entry, &content, unmapped_out)) != CASI_OK)
        goto done;

    /* Atomic: an interrupted pull never leaves a half-written transcript
     * where Claude Code would try to resume from it. */
    rc = casi_fs_write_file_atomic(path, content.ptr, content.len);

done:
    casi_buf_dispose(&content);
    return rc;
}

int casi_store_materialize(casi_repo *repo, const casi_roots *roots,
                           const casi_provider *provider, const casi_entry *entry,
                           casi_buf *unmapped_out)
{
    casi_buf path = CASI_BUF_INIT;
    int rc;

    if ((rc = provider->local_path_for(roots, entry->project_path,
                                       entry->session_id, &path)) != CASI_OK)
        goto done;

    rc = casi_store_materialize_to(repo, roots, entry, casi_buf_cstr(&path),
                                   unmapped_out);

done:
    casi_buf_dispose(&path);
    return rc;
}
