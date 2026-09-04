/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/sync.h"

#include <git2.h>

casi_sync_relation casi_sync_compare(const casi_entry *local, const casi_entry *remote)
{
    size_t shorter, i;

    shorter = local->chunk_count < remote->chunk_count
            ? local->chunk_count : remote->chunk_count;

    /* Any difference before the end of the shorter list means the two
     * histories genuinely disagree about bytes they both already had. */
    for (i = 0; i < shorter; i++)
        if (!git_oid_equal(&local->chunks[i], &remote->chunks[i]))
            return CASI_SYNC_DIVERGED;

    if (local->chunk_count == remote->chunk_count)
        return CASI_SYNC_SAME;

    return local->chunk_count > remote->chunk_count
         ? CASI_SYNC_LOCAL_AHEAD
         : CASI_SYNC_REMOTE_AHEAD;
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
