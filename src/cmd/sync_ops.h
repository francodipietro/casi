/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_SYNC_OPS_H
#define CASI_SYNC_OPS_H

#include "casi/ctx.h"
#include "casi/store.h"
#include "casi/sync.h"

/* What push, pull and status all need: the two sides, and the verdict per
 * session. Kept in one place so `casi status` cannot drift from what `casi
 * push` and `casi pull` would actually do. */

typedef struct {
    size_t to_push;
    size_t to_pull;
    size_t up_to_date;
    size_t conflicts;
    size_t excluded;
    uint64_t push_bytes;
    uint64_t pull_bytes;
    size_t auxiliary_to_push;
    size_t auxiliary_to_pull;
    size_t auxiliary_conflicts;
    uint64_t auxiliary_push_bytes;
    uint64_t auxiliary_pull_bytes;
} casi_sync_summary;

/* Local sessions, chunked into the store and ready to compare. */
int casi_ops_scan_local(casi_ctx *ctx, casi_entry_list *out,
                         casi_asset_list *assets_out, size_t *excluded_out);

/* The union of every other machine's branch. Sessions present on several
 * machines collapse to the most advanced copy. */
int casi_ops_load_remote(casi_ctx *ctx, casi_entry_list *out,
                          casi_asset_list *assets_out);

/* Compares the two sides without changing anything. */
int casi_ops_summarise(casi_repo *repo, casi_entry_list *local,
                       casi_entry_list *remote, casi_asset_list *local_assets,
                       casi_asset_list *remote_assets, casi_sync_summary *out);

/* One divergent session: the two sides and how far they still agree. */
typedef struct {
    casi_entry *local;
    casi_entry *remote;
    size_t      common_chunks;
} casi_conflict;

typedef struct {
    casi_conflict *items;
    size_t         len;
    size_t         cap;
} casi_conflict_list;

/* Every session that exists on both sides and genuinely diverged, with the
 * number of leading chunks the two copies still share. Pointers into the two
 * entry lists; the caller must keep them alive while the list is in use. */
int  casi_ops_list_conflicts(casi_repo *repo, casi_entry_list *local,
                             casi_entry_list *remote, casi_conflict_list *out);
void casi_conflict_list_dispose(casi_conflict_list *list);

/* Human-readable byte count for status/pull conflict reports. */
int casi_ops_human_size(uint64_t bytes, casi_buf *out);

/* Human-readable timestamp ("YYYY-MM-DD HH:MM") for a conflict report; the
 * epoch is display-only and never part of a sync decision. */
int casi_ops_format_time(uint64_t epoch, casi_buf *out);

#endif /* CASI_SYNC_OPS_H */
