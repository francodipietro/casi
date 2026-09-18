/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/index.h"

#include "casi/casi.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* This is a private, local cache format.  Changing it invalidates safely. */
#define CASI_INDEX_MAGIC "CASIIDX3"
#define CASI_INDEX_MAGIC_LEN (sizeof(CASI_INDEX_MAGIC) - 1)
#define CASI_INDEX_CHECKSUM_LEN GIT_OID_RAWSZ

struct index_owned_entry {
    casi_index_entry view;
    char            *path;
    char            *provider;
    git_oid         *chunks;
};

struct casi_index {
    casi_buf                 roots_key;
    struct index_owned_entry *entries;
    size_t                   len;
    size_t                   cap;
    bool                     dirty;
};

static void entry_dispose(struct index_owned_entry *entry)
{
    free(entry->path);
    free(entry->provider);
    free(entry->chunks);
    memset(entry, 0, sizeof(*entry));
}

static int roots_key_build(const casi_roots *roots, const char *crypto_key_id,
                           casi_buf *out)
{
    size_t i;
    int rc;

    casi_buf_clear(out);
    for (i = 0; i < casi_roots_count(roots); i++) {
        if ((rc = casi_buf_puts(out, casi_roots_name_at(roots, i))) != CASI_OK ||
            (rc = casi_buf_putc(out, '\0')) != CASI_OK ||
            (rc = casi_buf_puts(out, casi_roots_path_at(roots, i))) != CASI_OK ||
            (rc = casi_buf_putc(out, '\0')) != CASI_OK)
            return rc;
    }
    if ((rc = casi_buf_puts(out, "crypto")) != CASI_OK ||
        (rc = casi_buf_putc(out, '\0')) != CASI_OK ||
        (rc = casi_buf_puts(out, crypto_key_id != NULL ? crypto_key_id : "none")) != CASI_OK ||
        (rc = casi_buf_putc(out, '\0')) != CASI_OK)
        return rc;
    return CASI_OK;
}

static int put_u32(casi_buf *out, uint32_t value)
{
    unsigned char raw[4] = {
        (unsigned char)value, (unsigned char)(value >> 8),
        (unsigned char)(value >> 16), (unsigned char)(value >> 24)
    };

    return casi_buf_put(out, raw, sizeof(raw));
}

static int put_u64(casi_buf *out, uint64_t value)
{
    unsigned char raw[8];
    size_t i;

    for (i = 0; i < sizeof(raw); i++)
        raw[i] = (unsigned char)(value >> (i * 8));
    return casi_buf_put(out, raw, sizeof(raw));
}

static bool get_u32(const casi_buf *in, size_t *offset, uint32_t *out)
{
    const unsigned char *p;

    if (*offset > in->len || in->len - *offset < 4)
        return false;
    p = (const unsigned char *)in->ptr + *offset;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *offset += 4;
    return true;
}

static bool get_u64(const casi_buf *in, size_t *offset, uint64_t *out)
{
    const unsigned char *p;
    uint64_t value = 0;
    size_t i;

    if (*offset > in->len || in->len - *offset < 8)
        return false;
    p = (const unsigned char *)in->ptr + *offset;
    for (i = 0; i < 8; i++)
        value |= (uint64_t)p[i] << (i * 8);
    *out = value;
    *offset += 8;
    return true;
}

static bool get_bytes(const casi_buf *in, size_t *offset, size_t len, const char **out)
{
    if (*offset > in->len || len > in->len - *offset)
        return false;
    *out = in->ptr + *offset;
    *offset += len;
    return true;
}

static bool size_multiply(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a)
        return false;
    *out = a * b;
    return true;
}

/* The cache is local, not an adversarial input boundary, but it must not turn
 * a torn write or a flipped offset into a plausible incremental scan.  Hash
 * its complete serialized form; a format change also makes old files misses. */
static bool index_checksum_matches(const casi_buf *raw)
{
    git_oid checksum;
    size_t content_len;

    if (raw->len < CASI_INDEX_MAGIC_LEN + CASI_INDEX_CHECKSUM_LEN)
        return false;
    content_len = raw->len - CASI_INDEX_CHECKSUM_LEN;
    if (git_odb_hash(&checksum, raw->ptr, content_len, GIT_OBJECT_BLOB) < 0) {
        casi_error_clear();
        return false;
    }
    return memcmp(checksum.id, raw->ptr + content_len,
                  CASI_INDEX_CHECKSUM_LEN) == 0;
}

static int entries_grow(casi_index *index)
{
    struct index_owned_entry *p;
    size_t cap;

    if (index->len < index->cap)
        return CASI_OK;
    cap = index->cap ? index->cap * 2 : 32;
    if (cap > SIZE_MAX / sizeof(*p))
        return casi_error_set(CASI_ENOMEM, "stat cache is too large");
    p = realloc(index->entries, cap * sizeof(*p));
    if (p == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory growing stat cache");
    index->entries = p;
    index->cap = cap;
    return CASI_OK;
}

static int entry_copy_parts(struct index_owned_entry *dst,
                            const char *provider, size_t provider_len,
                            const char *path, size_t path_len,
                            const casi_stat *stat, uint64_t tail_raw_offset,
                            uint64_t tail_normalized_offset,
                            uint64_t normalized_bytes, const void *chunks,
                            size_t chunk_count)
{
    memset(dst, 0, sizeof(*dst));
    dst->path = casi_strndup(path, path_len);
    dst->provider = casi_strndup(provider, provider_len);
    if (dst->path == NULL || dst->provider == NULL)
        goto oom;
    if (chunk_count > 0) {
        if (chunk_count > SIZE_MAX / sizeof(*dst->chunks))
            goto oom;
        dst->chunks = malloc(chunk_count * sizeof(*dst->chunks));
        if (dst->chunks == NULL)
            goto oom;
        memcpy(dst->chunks, chunks, chunk_count * sizeof(*dst->chunks));
    }

    dst->view.path = dst->path;
    dst->view.provider = dst->provider;
    dst->view.stat = *stat;
    dst->view.tail_raw_offset = tail_raw_offset;
    dst->view.tail_normalized_offset = tail_normalized_offset;
    dst->view.normalized_bytes = normalized_bytes;
    dst->view.chunks = dst->chunks;
    dst->view.chunk_count = chunk_count;
    return CASI_OK;

oom:
    entry_dispose(dst);
    return casi_error_set(CASI_ENOMEM, "out of memory storing stat-cache entry");
}

static int entry_copy(struct index_owned_entry *dst, const char *provider,
                      const char *path, const casi_stat *stat,
                      uint64_t tail_raw_offset, uint64_t tail_normalized_offset,
                      uint64_t normalized_bytes, const git_oid *chunks,
                      size_t chunk_count)
{
    return entry_copy_parts(dst, provider, strlen(provider), path, strlen(path), stat,
                            tail_raw_offset, tail_normalized_offset, normalized_bytes,
                            chunks, chunk_count);
}

static bool stat_equal(const casi_stat *a, const casi_stat *b)
{
    return a->size == b->size && a->mtime_sec == b->mtime_sec &&
           a->mtime_nsec == b->mtime_nsec && a->ino == b->ino;
}

static void index_clear_entries(casi_index *index)
{
    size_t i;

    for (i = 0; i < index->len; i++)
        entry_dispose(&index->entries[i]);
    free(index->entries);
    index->entries = NULL;
    index->len = 0;
    index->cap = 0;
}

static int index_read(casi_index *index)
{
    casi_buf raw = CASI_BUF_INIT;
    size_t offset = 0, content_len, i;
    uint32_t key_len, count;
    int rc;

    rc = casi_fs_read_file(casi_paths_index(), &raw);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    if (raw.len < CASI_INDEX_MAGIC_LEN + CASI_INDEX_CHECKSUM_LEN ||
        memcmp(raw.ptr, CASI_INDEX_MAGIC, CASI_INDEX_MAGIC_LEN) != 0) {
        rc = CASI_OK;
        goto done;
    }
    if (!index_checksum_matches(&raw)) {
        rc = CASI_OK;
        goto done;
    }
    content_len = raw.len - CASI_INDEX_CHECKSUM_LEN;
    offset = CASI_INDEX_MAGIC_LEN;
    {
        const char *stored_key;

        if (!get_u32(&raw, &offset, &key_len) || key_len != index->roots_key.len ||
            !get_bytes(&raw, &offset, key_len, &stored_key) ||
            memcmp(stored_key, index->roots_key.ptr, key_len) != 0 ||
            !get_u32(&raw, &offset, &count)) {
            rc = CASI_OK;
            goto done;
        }
    }

    for (i = 0; i < count; i++) {
        const char *provider, *path, *oid_data;
        uint32_t provider_len, path_len, chunk_count;
        uint64_t size, mtime_sec, ino, tail_raw, tail_normalized, bytes;
        uint32_t mtime_nsec;
        size_t oid_bytes;
        casi_stat stat;
        struct index_owned_entry entry;

        if (!get_u32(&raw, &offset, &provider_len) ||
            !get_bytes(&raw, &offset, provider_len, &provider) ||
            !get_u32(&raw, &offset, &path_len) ||
            !get_bytes(&raw, &offset, path_len, &path) ||
            !get_u64(&raw, &offset, &size) || !get_u64(&raw, &offset, &mtime_sec) ||
            !get_u32(&raw, &offset, &mtime_nsec) || !get_u64(&raw, &offset, &ino) ||
            !get_u64(&raw, &offset, &tail_raw) ||
            !get_u64(&raw, &offset, &tail_normalized) ||
            !get_u64(&raw, &offset, &bytes) || !get_u32(&raw, &offset, &chunk_count) ||
            !size_multiply((size_t)chunk_count, GIT_OID_RAWSZ, &oid_bytes) ||
            !get_bytes(&raw, &offset, oid_bytes, &oid_data)) {
            index_clear_entries(index);
            rc = CASI_OK;
            goto done;
        }
        stat.size = size;
        stat.mtime_sec = (int64_t)mtime_sec;
        stat.mtime_nsec = mtime_nsec;
        stat.ino = ino;
        stat.is_dir = false;
        if ((rc = entries_grow(index)) != CASI_OK)
            goto done;
        if ((rc = entry_copy_parts(&entry, provider, provider_len, path, path_len,
                                   &stat, tail_raw, tail_normalized, bytes, oid_data,
                                   chunk_count)) != CASI_OK)
            goto done;
        index->entries[index->len++] = entry;
    }
    if (offset != content_len)
        index_clear_entries(index);

done:
    casi_buf_dispose(&raw);
    return rc;
}

int casi_index_open(const casi_roots *roots, const char *crypto_key_id, casi_index **out)
{
    casi_index *index;
    int rc;

    index = calloc(1, sizeof(*index));
    if (index == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory opening stat cache");
    if ((rc = roots_key_build(roots, crypto_key_id, &index->roots_key)) != CASI_OK)
        goto fail;
    if ((rc = index_read(index)) != CASI_OK)
        goto fail;
    *out = index;
    return CASI_OK;

fail:
    casi_index_free(index);
    return rc;
}

void casi_index_free(casi_index *index)
{
    if (index == NULL)
        return;
    index_clear_entries(index);
    casi_buf_dispose(&index->roots_key);
    free(index);
}

const casi_index_entry *casi_index_find(const casi_index *index,
                                        const char *provider, const char *path)
{
    size_t i;

    for (i = 0; i < index->len; i++)
        if (strcmp(index->entries[i].provider, provider) == 0 &&
            strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i].view;
    return NULL;
}

bool casi_index_entry_matches(const casi_index_entry *entry, const casi_stat *stat)
{
    return entry != NULL && stat_equal(&entry->stat, stat);
}

bool casi_index_entry_is_well_formed(const casi_index_entry *entry)
{
    if (entry == NULL)
        return false;
    if (entry->chunk_count == 0)
        return entry->stat.size == 0 && entry->normalized_bytes == 0 &&
               entry->tail_raw_offset == 0 && entry->tail_normalized_offset == 0;
    return entry->chunks != NULL && entry->stat.size > 0 &&
           entry->normalized_bytes > 0 &&
           entry->tail_raw_offset < entry->stat.size &&
           entry->tail_normalized_offset < entry->normalized_bytes;
}

bool casi_index_entry_can_resume(const casi_index_entry *entry, const casi_stat *stat)
{
    return casi_index_entry_is_well_formed(entry) && entry->stat.ino == stat->ino &&
           stat->size > entry->stat.size && entry->tail_raw_offset <= entry->stat.size;
}

int casi_index_update(casi_index *index, const char *provider, const char *path,
                      const casi_stat *stat, uint64_t tail_raw_offset,
                      uint64_t tail_normalized_offset, uint64_t normalized_bytes,
                      const git_oid *chunks, size_t chunk_count)
{
    struct index_owned_entry replacement;
    size_t i;
    int rc;

    if ((rc = entry_copy(&replacement, provider, path, stat, tail_raw_offset,
                         tail_normalized_offset, normalized_bytes, chunks,
                         chunk_count)) != CASI_OK)
        return rc;
    for (i = 0; i < index->len; i++) {
        if (strcmp(index->entries[i].provider, provider) == 0 &&
            strcmp(index->entries[i].path, path) == 0) {
            entry_dispose(&index->entries[i]);
            index->entries[i] = replacement;
            index->dirty = true;
            return CASI_OK;
        }
    }
    if ((rc = entries_grow(index)) != CASI_OK) {
        entry_dispose(&replacement);
        return rc;
    }
    index->entries[index->len++] = replacement;
    index->dirty = true;
    return CASI_OK;
}

int casi_index_flush(casi_index *index)
{
    casi_buf raw = CASI_BUF_INIT;
    git_oid checksum;
    size_t i;
    int rc = CASI_OK;

    if (!index->dirty)
        return CASI_OK;
    if (index->roots_key.len > UINT32_MAX || index->len > UINT32_MAX) {
        rc = casi_error_set(CASI_ENOMEM, "stat cache is too large to write");
        goto done;
    }
    if ((rc = casi_buf_put(&raw, CASI_INDEX_MAGIC, CASI_INDEX_MAGIC_LEN)) != CASI_OK ||
        (rc = put_u32(&raw, (uint32_t)index->roots_key.len)) != CASI_OK ||
        (rc = casi_buf_put(&raw, index->roots_key.ptr, index->roots_key.len)) != CASI_OK ||
        (rc = put_u32(&raw, (uint32_t)index->len)) != CASI_OK)
        goto done;

    for (i = 0; i < index->len; i++) {
        const casi_index_entry *entry = &index->entries[i].view;
        size_t provider_len = strlen(entry->provider), path_len = strlen(entry->path);
        size_t oid_bytes;

        if (provider_len > UINT32_MAX || path_len > UINT32_MAX ||
            entry->chunk_count > UINT32_MAX) {
            rc = casi_error_set(CASI_ENOMEM, "stat-cache entry is too large to write");
            goto done;
        }
        if (!size_multiply(entry->chunk_count, GIT_OID_RAWSZ, &oid_bytes)) {
            rc = casi_error_set(CASI_ENOMEM, "stat-cache entry is too large to write");
            goto done;
        }
        if (
            (rc = put_u32(&raw, (uint32_t)provider_len)) != CASI_OK ||
            (rc = casi_buf_put(&raw, entry->provider, provider_len)) != CASI_OK ||
            (rc = put_u32(&raw, (uint32_t)path_len)) != CASI_OK ||
            (rc = casi_buf_put(&raw, entry->path, path_len)) != CASI_OK ||
            (rc = put_u64(&raw, entry->stat.size)) != CASI_OK ||
            (rc = put_u64(&raw, (uint64_t)entry->stat.mtime_sec)) != CASI_OK ||
            (rc = put_u32(&raw, entry->stat.mtime_nsec)) != CASI_OK ||
            (rc = put_u64(&raw, entry->stat.ino)) != CASI_OK ||
            (rc = put_u64(&raw, entry->tail_raw_offset)) != CASI_OK ||
            (rc = put_u64(&raw, entry->tail_normalized_offset)) != CASI_OK ||
            (rc = put_u64(&raw, entry->normalized_bytes)) != CASI_OK ||
            (rc = put_u32(&raw, (uint32_t)entry->chunk_count)) != CASI_OK ||
            (rc = casi_buf_put(&raw, entry->chunks, oid_bytes)) != CASI_OK)
            goto done;
    }

    if (git_odb_hash(&checksum, raw.ptr, raw.len, GIT_OBJECT_BLOB) < 0) {
        rc = casi_error_set(CASI_ERROR, "cannot checksum stat cache");
        goto done;
    }
    if ((rc = casi_buf_put(&raw, checksum.id, CASI_INDEX_CHECKSUM_LEN)) != CASI_OK)
        goto done;

    rc = casi_fs_write_file_atomic(casi_paths_index(), raw.ptr, raw.len);
    if (rc == CASI_OK)
        index->dirty = false;

done:
    casi_buf_dispose(&raw);
    return rc;
}
