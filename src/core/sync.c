/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/sync.h"
#include "casi/error.h"

#include <git2.h>
#include <string.h>

/* Reassemble only the portion after the sealed prefix. This is normally one
 * sub-megabyte tail; reading more is reserved for a real rewrite, where the
 * exact answer is worth the exceptional cost. */
static int read_tail(casi_repo *repo, const casi_entry *entry, size_t from,
                     casi_buf *out)
{
    casi_buf piece = CASI_BUF_INIT;
    size_t i;
    int rc = CASI_OK;

    casi_buf_clear(out);
    for (i = from; i < entry->chunk_count; i++) {
        if ((rc = casi_repo_read_blob(repo, &entry->chunks[i], &piece)) != CASI_OK)
            break;
        if ((rc = casi_buf_put(out, piece.ptr, piece.len)) != CASI_OK)
            break;
    }

    casi_buf_dispose(&piece);
    return rc;
}

int casi_sync_compare(casi_repo *repo, const casi_entry *local,
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
            return CASI_OK;
        }

        if ((rc = read_tail(repo, local, i, &local_tail)) != CASI_OK)
            goto done;
        if ((rc = read_tail(repo, remote, i, &remote_tail)) != CASI_OK)
            goto done;

        if (local_tail.len <= remote_tail.len &&
            memcmp(local_tail.ptr, remote_tail.ptr, local_tail.len) == 0)
            *out = CASI_SYNC_REMOTE_AHEAD;
        else if (remote_tail.len <= local_tail.len &&
                 memcmp(remote_tail.ptr, local_tail.ptr, remote_tail.len) == 0)
            *out = CASI_SYNC_LOCAL_AHEAD;
        else
            *out = CASI_SYNC_DIVERGED;
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
