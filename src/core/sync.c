/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/sync.h"
#include "casi/error.h"

#include <stdbool.h>
#include <git2.h>
#include <string.h>

static bool is_prefix(const casi_buf *prefix, const casi_buf *whole)
{
    return prefix->len <= whole->len &&
           (prefix->len == 0 || memcmp(prefix->ptr, whole->ptr, prefix->len) == 0);
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
