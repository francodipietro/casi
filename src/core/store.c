/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/store.h"
#include "casi/casi.h"
#include "casi/chunk.h"
#include "casi/index.h"
#include "casi/jsonl.h"

#include <inttypes.h>
#include <stdint.h>
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

static void asset_dispose(casi_asset *asset)
{
    free(asset->project_path);
    free(asset->project_id);
    free(asset->session_id);
    free(asset->name);
    memset(asset, 0, sizeof(*asset));
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

void casi_asset_list_dispose(casi_asset_list *list)
{
    size_t i;

    for (i = 0; i < list->len; i++)
        asset_dispose(&list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

int casi_asset_list_push_owned(casi_asset_list *list, casi_asset *asset)
{
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 16;
        casi_asset *p = realloc(list->items, cap * sizeof(*p));

        if (p == NULL)
            return casi_error_set(CASI_ENOMEM, "out of memory growing asset list");
        list->items = p;
        list->cap = cap;
    }

    list->items[list->len++] = *asset;
    memset(asset, 0, sizeof(*asset));
    return CASI_OK;
}

casi_asset *casi_asset_list_find(casi_asset_list *list, casi_aux_kind kind,
                                 const char *project_id, const char *session_id,
                                 const char *name)
{
    size_t i;

    for (i = 0; i < list->len; i++) {
        casi_asset *item = &list->items[i];

        if (item->kind != kind || strcmp(item->project_id, project_id) != 0 ||
            strcmp(item->name, name) != 0)
            continue;
        if ((item->session_id == NULL) != (session_id == NULL))
            continue;
        if (session_id == NULL || strcmp(item->session_id, session_id) == 0)
            return item;
    }

    return NULL;
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

static int asset_path(casi_buf *out, const char *provider, const casi_asset *asset)
{
    casi_buf_clear(out);
    if (asset->kind == CASI_AUX_SUBAGENT)
        return casi_buf_printf(out, "sessions/%s/%s/%s/subagents/%s", provider,
                               asset->project_id, asset->session_id, asset->name);
    if (asset->kind == CASI_AUX_MEMORY)
        return casi_buf_printf(out, "projects/%s/%s/memory/%s", provider,
                               asset->project_id, asset->name);
    return casi_error_set(CASI_EINVAL, "unknown auxiliary file kind");
}

static bool project_meta_already_written(const casi_asset_list *assets, size_t end,
                                         const casi_asset *asset)
{
    size_t i;

    for (i = 0; i < end; i++)
        if (assets->items[i].kind == CASI_AUX_MEMORY &&
            strcmp(assets->items[i].project_id, asset->project_id) == 0)
            return true;
    return false;
}

/* --- writing ---------------------------------------------------------- */

struct chunk_writer {
    casi_repo *repo;
    git_oid   *oids;
    size_t     capacity;
    size_t     written;
    size_t     offset;
    size_t     last_start;
};

static int write_one_chunk(const void *data, size_t len, size_t index, void *payload)
{
    struct chunk_writer *w = payload;

    if (index >= w->capacity)
        return casi_error_set(CASI_ERROR, "chunk count changed mid-write");

    w->last_start = w->offset;
    w->offset += len;
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

static bool stats_equal(const casi_stat *a, const casi_stat *b)
{
    return a->size == b->size && a->mtime_sec == b->mtime_sec &&
           a->mtime_nsec == b->mtime_nsec && a->ino == b->ino;
}

/* Normalisation only substitutes path bytes, never newlines.  Chunk starts
 * are always after newlines, so counting them maps a normalized chunk start
 * back to its exact source-file offset without retaining the whole mapping. */
static int raw_offset_for_normalized_boundary(const casi_buf *raw,
                                              const casi_buf *normalized,
                                              size_t normalized_offset,
                                              size_t *out)
{
    size_t needed = 0, seen = 0, i;

    if (normalized_offset > normalized->len)
        return casi_error_set(CASI_ERROR, "invalid normalized chunk boundary");
    for (i = 0; i < normalized_offset; i++)
        if (normalized->ptr[i] == '\n')
            needed++;
    if (needed == 0) {
        *out = 0;
        return CASI_OK;
    }
    for (i = 0; i < raw->len; i++) {
        if (raw->ptr[i] != '\n')
            continue;
        if (++seen == needed) {
            *out = i + 1;
            return CASI_OK;
        }
    }
    return casi_error_set(CASI_ERROR, "cannot map normalized chunk boundary to source");
}

static int write_chunks(casi_repo *repo, const casi_buf *normalized,
                        struct chunk_writer *writer)
{
    size_t count = casi_chunk_count(normalized->ptr, normalized->len, CASI_CHUNK_TARGET);

    memset(writer, 0, sizeof(*writer));
    if (count == 0)
        return CASI_OK;
    writer->oids = calloc(count, sizeof(*writer->oids));
    if (writer->oids == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory chunking transcript");
    writer->repo = repo;
    writer->capacity = count;
    return casi_chunk_split(normalized->ptr, normalized->len, CASI_CHUNK_TARGET,
                            write_one_chunk, writer);
}

static int entry_take_chunks(const casi_session *session, git_oid **chunks,
                             size_t chunk_count, uint64_t bytes,
                             casi_entry_list *out)
{
    casi_entry entry;
    int rc;

    memset(&entry, 0, sizeof(entry));
    entry.session_id = casi_strdup(session->session_id);
    entry.project_path = casi_strdup(session->project_path);
    entry.project_id = casi_strdup(session->project_id);
    entry.chunks = *chunks;
    entry.chunk_count = chunk_count;
    entry.bytes = bytes;
    *chunks = NULL;
    if (entry.session_id == NULL || entry.project_path == NULL || entry.project_id == NULL) {
        entry_dispose(&entry);
        return casi_error_set(CASI_ENOMEM, "out of memory storing %s", session->session_id);
    }
    if ((rc = entry_list_push(out, &entry)) != CASI_OK) {
        entry_dispose(&entry);
        return rc;
    }
    return CASI_OK;
}

static bool cache_entry_is_valid(casi_repo *repo, const casi_index_entry *cached)
{
    uint64_t total = 0, last_size = 0;
    size_t i;

    if (cached == NULL)
        return false;
    if (!casi_index_entry_is_well_formed(cached))
        return false;
    for (i = 0; i < cached->chunk_count; i++) {
        uint64_t size;

        /* Blob lookup rejects tree and commit OIDs as well as missing objects.
         * A malformed local cache must be a miss, never input to a push. */
        if (casi_repo_blob_size(repo, &cached->chunks[i], &size) != CASI_OK) {
            casi_error_clear();
            return false;
        }
        if (size > UINT64_MAX - total)
            return false;
        total += size;
        last_size = size;
    }
    if (total != cached->normalized_bytes ||
        cached->tail_normalized_offset != total - last_size)
        return false;
    if (cached->tail_raw_offset > cached->stat.size)
        return false;
    return true;
}

static int scan_one_session(casi_repo *repo, const casi_roots *roots,
                            casi_index *index, const char *provider_name,
                            const casi_session *session, casi_entry_list *out)
{
    casi_buf raw = CASI_BUF_INIT, normalized = CASI_BUF_INIT;
    struct chunk_writer writer;
    const casi_index_entry *cached;
    casi_stat before, after;
    git_oid *chunks = NULL;
    size_t chunk_count, raw_tail, tail_normalized;
    uint64_t bytes;
    bool resume = false;
    int rc;

    memset(&writer, 0, sizeof(writer));

    if ((rc = casi_fs_stat(session->local_path, &before)) != CASI_OK)
        goto done;
    cached = casi_index_find(index, provider_name, session->local_path);
    if (casi_index_entry_matches(cached, &before) && cache_entry_is_valid(repo, cached)) {
        if (cached->chunk_count > 0) {
            chunks = malloc(cached->chunk_count * sizeof(*chunks));
            if (chunks == NULL) {
                rc = casi_error_set(CASI_ENOMEM, "out of memory reusing stat cache");
                goto done;
            }
            memcpy(chunks, cached->chunks, cached->chunk_count * sizeof(*chunks));
        }
        rc = entry_take_chunks(session, &chunks, cached->chunk_count,
                               cached->normalized_bytes, out);
        goto done;
    }

    resume = casi_index_entry_can_resume(cached, &before) && cache_entry_is_valid(repo, cached);
    if (resume)
        rc = casi_fs_read_file_from(session->local_path, cached->tail_raw_offset, &raw);
    else
        rc = casi_fs_read_file(session->local_path, &raw);
    if (rc != CASI_OK)
        goto done;

    /* Normalise first, chunk second: this ordering is what makes two machines
     * with different layouts produce identical blobs. */
    if ((rc = casi_roots_normalize_text(roots, &raw, &normalized)) != CASI_OK)
        goto done;
    if ((rc = write_chunks(repo, &normalized, &writer)) != CASI_OK)
        goto done;

    if (resume) {
        size_t prefix_count = cached->chunk_count > 0 ? cached->chunk_count - 1 : 0;
        size_t tail_raw_relative;

        if (prefix_count > SIZE_MAX - writer.written) {
            rc = casi_error_set(CASI_ENOMEM, "too many transcript chunks");
            goto done;
        }
        chunk_count = prefix_count + writer.written;
        if (chunk_count > 0) {
            chunks = calloc(chunk_count, sizeof(*chunks));
            if (chunks == NULL) {
                rc = casi_error_set(CASI_ENOMEM, "out of memory extending stat cache");
                goto done;
            }
            if (prefix_count > 0)
                memcpy(chunks, cached->chunks, prefix_count * sizeof(*chunks));
            if (writer.written > 0)
                memcpy(chunks + prefix_count, writer.oids,
                       writer.written * sizeof(*chunks));
        }
        if ((rc = raw_offset_for_normalized_boundary(&raw, &normalized,
                                                      writer.last_start,
                                                      &tail_raw_relative)) != CASI_OK)
            goto done;
        raw_tail = (size_t)cached->tail_raw_offset + tail_raw_relative;
        tail_normalized = (size_t)cached->tail_normalized_offset + writer.last_start;
        bytes = cached->tail_normalized_offset + normalized.len;
    } else {
        chunk_count = writer.written;
        chunks = writer.oids;
        writer.oids = NULL;
        if ((rc = raw_offset_for_normalized_boundary(&raw, &normalized,
                                                      writer.last_start, &raw_tail)) != CASI_OK)
            goto done;
        tail_normalized = writer.last_start;
        bytes = normalized.len;
    }

    if ((rc = entry_take_chunks(session, &chunks, chunk_count, bytes, out)) != CASI_OK)
        goto done;
    if ((rc = casi_fs_stat(session->local_path, &after)) != CASI_OK)
        goto done;
    if (stats_equal(&before, &after))
        rc = casi_index_update(index, provider_name, session->local_path, &before,
                               raw_tail, tail_normalized, bytes,
                               out->items[out->len - 1].chunks, chunk_count);

done:
    free(chunks);
    free(writer.oids);
    casi_buf_dispose(&raw);
    casi_buf_dispose(&normalized);
    return rc;
}

static int scan_one_asset(casi_repo *repo, const casi_roots *roots,
                          const casi_aux_file *file, casi_asset_list *out)
{
    casi_buf raw = CASI_BUF_INIT, normalized = CASI_BUF_INIT;
    casi_asset asset;
    int rc;

    memset(&asset, 0, sizeof(asset));
    if ((rc = casi_fs_read_file(file->local_path, &raw)) != CASI_OK)
        goto done;
    if ((rc = casi_roots_normalize_text(roots, &raw, &normalized)) != CASI_OK)
        goto done;
    if ((rc = casi_repo_write_blob(repo, normalized.ptr, normalized.len, &asset.oid)) != CASI_OK)
        goto done;

    asset.kind = file->kind;
    asset.project_path = casi_strdup(file->project_path);
    asset.project_id = casi_strdup(file->project_id);
    asset.session_id = file->session_id != NULL ? casi_strdup(file->session_id) : NULL;
    asset.name = casi_strdup(file->name);
    asset.bytes = (uint64_t)normalized.len;
    if (asset.project_path == NULL || asset.project_id == NULL || asset.name == NULL ||
        (file->session_id != NULL && asset.session_id == NULL)) {
        rc = casi_error_set(CASI_ENOMEM, "out of memory storing auxiliary file");
        goto done;
    }

    rc = casi_asset_list_push_owned(out, &asset);

done:
    asset_dispose(&asset);
    casi_buf_dispose(&raw);
    casi_buf_dispose(&normalized);
    return rc;
}

int casi_store_scan_local(casi_repo *repo, const casi_roots *roots,
                          const casi_provider *provider,
                          const casi_strvec *exclude,
                          casi_entry_list *out, casi_asset_list *assets_out,
                          size_t *excluded_out)
{
    casi_session_list sessions;
    casi_aux_file_list aux_files;
    casi_index *index = NULL;
    size_t i, skipped = 0;
    int rc;

    memset(&sessions, 0, sizeof(sessions));
    memset(&aux_files, 0, sizeof(aux_files));

    if ((rc = provider->discover(roots, &sessions, &aux_files)) != CASI_OK)
        goto done;
    if ((rc = casi_index_open(roots, &index)) != CASI_OK)
        goto done;

    for (i = 0; i < sessions.len; i++) {
        if (is_excluded(exclude, sessions.items[i].project_path)) {
            casi_verbose("excluded: %s", sessions.items[i].project_path);
            skipped++;
            continue;
        }

        if ((rc = scan_one_session(repo, roots, index, provider->name,
                                   &sessions.items[i], out)) != CASI_OK)
            goto done;
    }

    for (i = 0; i < aux_files.len; i++) {
        if (is_excluded(exclude, aux_files.items[i].project_path))
            continue;
        if ((rc = scan_one_asset(repo, roots, &aux_files.items[i], assets_out)) != CASI_OK)
            goto done;
    }

    rc = casi_index_flush(index);

done:
    if (excluded_out != NULL)
        *excluded_out = skipped;
    casi_session_list_dispose(&sessions);
    casi_aux_file_list_dispose(&aux_files);
    casi_index_free(index);
    return rc;
}

int casi_store_write_tree(casi_repo *repo, const char *provider_name,
                          const casi_entry_list *entries,
                          const casi_asset_list *assets, git_oid *tree_out)
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

    for (i = 0; i < assets->len; i++) {
        const casi_asset *asset = &assets->items[i];

        if (asset->kind == CASI_AUX_MEMORY &&
            !project_meta_already_written(assets, i, asset)) {
            if ((rc = casi_json_escape_string(asset->project_path, &project_json)) != CASI_OK)
                goto done;
            casi_buf_clear(&path);
            if ((rc = casi_buf_printf(&path, "projects/%s/%s/meta.json", provider_name,
                                      asset->project_id)) != CASI_OK)
                goto done;
            casi_buf_clear(&meta);
            if ((rc = casi_buf_printf(&meta, "{\"projectPath\":\"%s\"}\n",
                                      casi_buf_cstr(&project_json))) != CASI_OK)
                goto done;
            if ((rc = casi_tree_add_text(tree, casi_buf_cstr(&path),
                                         casi_buf_cstr(&meta))) != CASI_OK)
                goto done;
        }

        if ((rc = asset_path(&path, provider_name, asset)) != CASI_OK)
            goto done;
        if ((rc = casi_tree_add(tree, casi_buf_cstr(&path), &asset->oid)) != CASI_OK)
            goto done;
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
                              const char *session_id, casi_entry_list *out,
                              casi_asset_list *assets_out)
{
    casi_buf path = CASI_BUF_INIT, meta = CASI_BUF_INIT, value = CASI_BUF_INIT;
    casi_strvec chunk_names = CASI_STRVEC_INIT;
    casi_strvec asset_names = CASI_STRVEC_INIT;
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

    casi_buf_clear(&path);
    if ((rc = casi_buf_printf(&path, "sessions/%s/%s/%s/subagents", provider_name,
                              project_id, session_id)) != CASI_OK)
        goto done;
    rc = casi_repo_tree_list(repo, tree, casi_buf_cstr(&path), &asset_names);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
    } else if (rc != CASI_OK) {
        goto done;
    }
    for (i = 0; i < asset_names.len; i++) {
        casi_asset asset;

        memset(&asset, 0, sizeof(asset));
        asset.kind = CASI_AUX_SUBAGENT;
        asset.project_path = casi_strdup(out->items[out->len - 1].project_path);
        asset.project_id = casi_strdup(project_id);
        asset.session_id = casi_strdup(session_id);
        asset.name = casi_strdup(asset_names.items[i]);
        if (asset.project_path == NULL || asset.project_id == NULL ||
            asset.session_id == NULL || asset.name == NULL) {
            asset_dispose(&asset);
            rc = casi_error_set(CASI_ENOMEM, "out of memory reading subagent metadata");
            goto done;
        }
        casi_buf_clear(&path);
        if ((rc = casi_buf_printf(&path, "sessions/%s/%s/%s/subagents/%s", provider_name,
                                  project_id, session_id, asset.name)) != CASI_OK ||
            (rc = casi_repo_tree_entry_oid(repo, tree, casi_buf_cstr(&path),
                                           &asset.oid)) != CASI_OK ||
            (rc = casi_repo_blob_size(repo, &asset.oid, &asset.bytes)) != CASI_OK ||
            (rc = casi_asset_list_push_owned(assets_out, &asset)) != CASI_OK) {
            asset_dispose(&asset);
            goto done;
        }
    }

    rc = CASI_OK;

done:
    entry_dispose(&entry);
    casi_strvec_dispose(&chunk_names);
    casi_strvec_dispose(&asset_names);
    casi_buf_dispose(&path);
    casi_buf_dispose(&meta);
    casi_buf_dispose(&value);
    return rc;
}

static int read_project_assets(casi_repo *repo, const git_oid *tree,
                               const char *provider_name, const char *project_id,
                               casi_asset_list *assets_out)
{
    casi_buf path = CASI_BUF_INIT, meta = CASI_BUF_INIT, project_path = CASI_BUF_INIT;
    casi_strvec names = CASI_STRVEC_INIT;
    size_t i;
    int rc;

    if ((rc = casi_buf_printf(&path, "projects/%s/%s/meta.json", provider_name,
                              project_id)) != CASI_OK ||
        (rc = casi_repo_tree_entry_blob(repo, tree, casi_buf_cstr(&path), &meta)) != CASI_OK)
        goto done;
    if (!casi_json_find_string(casi_buf_cstr(&meta), meta.len, "projectPath", &project_path)) {
        rc = casi_error_set(CASI_EINVAL, "malformed metadata for project %s", project_id);
        goto done;
    }
    casi_buf_clear(&path);
    if ((rc = casi_buf_printf(&path, "projects/%s/%s/memory", provider_name,
                              project_id)) != CASI_OK)
        goto done;
    rc = casi_repo_tree_list(repo, tree, casi_buf_cstr(&path), &names);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    for (i = 0; i < names.len; i++) {
        casi_asset asset;

        memset(&asset, 0, sizeof(asset));
        asset.kind = CASI_AUX_MEMORY;
        asset.project_path = casi_strdup(casi_buf_cstr(&project_path));
        asset.project_id = casi_strdup(project_id);
        asset.name = casi_strdup(names.items[i]);
        if (asset.project_path == NULL || asset.project_id == NULL || asset.name == NULL) {
            asset_dispose(&asset);
            rc = casi_error_set(CASI_ENOMEM, "out of memory reading memory metadata");
            goto done;
        }
        casi_buf_clear(&path);
        if ((rc = casi_buf_printf(&path, "projects/%s/%s/memory/%s", provider_name,
                                  project_id, asset.name)) != CASI_OK ||
            (rc = casi_repo_tree_entry_oid(repo, tree, casi_buf_cstr(&path),
                                           &asset.oid)) != CASI_OK ||
            (rc = casi_repo_blob_size(repo, &asset.oid, &asset.bytes)) != CASI_OK ||
            (rc = casi_asset_list_push_owned(assets_out, &asset)) != CASI_OK) {
            asset_dispose(&asset);
            goto done;
        }
    }

    rc = CASI_OK;
done:
    casi_strvec_dispose(&names);
    casi_buf_dispose(&path);
    casi_buf_dispose(&meta);
    casi_buf_dispose(&project_path);
    return rc;
}

int casi_store_read_tree(casi_repo *repo, const git_oid *tree,
                         const char *provider_name, casi_entry_list *out,
                         casi_asset_list *assets_out)
{
    casi_buf path = CASI_BUF_INIT;
    casi_strvec project_ids = CASI_STRVEC_INIT, session_ids = CASI_STRVEC_INIT,
                memory_project_ids = CASI_STRVEC_INIT;
    size_t i, j;
    int rc;

    if ((rc = casi_buf_printf(&path, "sessions/%s", provider_name)) != CASI_OK)
        goto done;

    rc = casi_repo_tree_list(repo, tree, casi_buf_cstr(&path), &project_ids);
    if (rc == CASI_ENOTFOUND) {
        /* Nothing from this provider in that tree yet. */
        casi_error_clear();
        rc = CASI_OK;
    } else if (rc != CASI_OK) {
        goto done;
    }

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
                                         session_ids.items[j], out, assets_out)) != CASI_OK)
                goto done;
    }

    casi_buf_clear(&path);
    if ((rc = casi_buf_printf(&path, "projects/%s", provider_name)) != CASI_OK)
        goto done;
    rc = casi_repo_tree_list(repo, tree, casi_buf_cstr(&path), &memory_project_ids);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
    } else if (rc != CASI_OK) {
        goto done;
    }
    for (i = 0; i < memory_project_ids.len; i++)
        if ((rc = read_project_assets(repo, tree, provider_name,
                                      memory_project_ids.items[i], assets_out)) != CASI_OK)
            goto done;

    rc = CASI_OK;

done:
    casi_strvec_dispose(&project_ids);
    casi_strvec_dispose(&session_ids);
    casi_strvec_dispose(&memory_project_ids);
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

    if (entry->bytes > SIZE_MAX)
        return casi_error_set(CASI_ENOMEM, "session is too large to materialize");
    if ((rc = casi_buf_grow(&joined, (size_t)entry->bytes)) != CASI_OK)
        goto done;

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

int casi_store_materialize_asset_to(casi_repo *repo, const casi_roots *roots,
                                    const casi_asset *asset, const char *path,
                                    casi_buf *unmapped_out)
{
    casi_buf normalized = CASI_BUF_INIT, local = CASI_BUF_INIT;
    int rc;

    if ((rc = casi_repo_read_blob(repo, &asset->oid, &normalized)) != CASI_OK)
        goto done;
    if ((rc = casi_roots_denormalize_text(roots, &normalized, &local,
                                          unmapped_out)) != CASI_OK)
        goto done;
    rc = casi_fs_write_file_atomic(path, local.ptr, local.len);

done:
    casi_buf_dispose(&normalized);
    casi_buf_dispose(&local);
    return rc;
}

int casi_store_materialize_asset(casi_repo *repo, const casi_roots *roots,
                                 const casi_provider *provider,
                                 const casi_asset *asset, casi_buf *unmapped_out)
{
    casi_buf path = CASI_BUF_INIT;
    int rc;

    if ((rc = provider->aux_local_path_for(roots, asset->kind, asset->project_path,
                                           asset->session_id, asset->name, &path)) == CASI_OK)
        rc = casi_store_materialize_asset_to(repo, roots, asset, casi_buf_cstr(&path),
                                             unmapped_out);

    casi_buf_dispose(&path);
    return rc;
}
