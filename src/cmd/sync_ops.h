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

#endif /* CASI_SYNC_OPS_H */
