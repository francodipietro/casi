/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_SYNC_H
#define CASI_SYNC_H

#include "casi/store.h"

/*
 * Because transcripts only ever gain bytes at the end, comparing two chunk-id
 * lists answers everything -- with no timestamps and therefore no clock skew,
 * no mtime games, and no dependence on machines agreeing about the time.
 *
 * Only the final chunk of a session can be partial, so the comparison is
 * exact rather than heuristic.
 *
 * The design does not *assume* strict append-only. A rewritten prefix -- a
 * rewind, a compaction -- simply fails the prefix test and surfaces as a
 * conflict, which is the honest answer.
 */
typedef enum {
    CASI_SYNC_SAME,          /* identical; nothing to do            */
    CASI_SYNC_LOCAL_AHEAD,   /* remote is a prefix of local; push    */
    CASI_SYNC_REMOTE_AHEAD,  /* local is a prefix of remote; pull    */
    CASI_SYNC_DIVERGED       /* neither is a prefix; real conflict   */
} casi_sync_relation;

casi_sync_relation casi_sync_compare(const casi_entry *local, const casi_entry *remote);

const char *casi_sync_relation_name(casi_sync_relation relation);

#endif /* CASI_SYNC_H */
