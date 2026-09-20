/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* One machine-readable line per session, classifying it as same, one side
 * ahead, a genuine divergence, or present on only one side. This is the
 * diagnosis tool for "why does everything look divergent": run it once, with a
 * safe copy of the data, and the whole classification is captured without any
 * transcript content leaving the machine. */
static void emit(const char *relation, const char *sid, const char *project,
                 const char *origin, uint64_t lbytes, uint64_t rbytes,
                 size_t lchunks, size_t rchunks, size_t common,
                 uint64_t lupdated, uint64_t rupdated)
{
    printf("session\t%s\t%s\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\t%zu\t%zu\t%zu\t%"
           PRIu64 "\t%" PRIu64 "\n",
           relation, sid, project, origin, lbytes, rbytes, lchunks, rchunks,
           common, lupdated, rupdated);
}

int casi_cmd_diagnose(int argc, char **argv)
{
    casi_ctx ctx;
    casi_entry_list local, remote;
    casi_asset_list local_assets, remote_assets;
    size_t counts[6] = { 0 };
    size_t i;
    int rc;

    (void)argv;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    memset(&local_assets, 0, sizeof(local_assets));
    memset(&remote_assets, 0, sizeof(remote_assets));

    if (argc != 0)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("diagnose")->usage);

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        goto done;
    if ((rc = casi_ops_scan_local(&ctx, &local, &local_assets, NULL)) != CASI_OK)
        goto done;
    if ((rc = casi_ops_load_remote(&ctx, &remote, &remote_assets)) != CASI_OK)
        goto done;

    for (i = 0; i < local.len; i++) {
        casi_entry *l = &local.items[i];
        casi_entry *r = casi_entry_list_find(&remote, l->session_id);
        const char *relation = "local_only";
        const char *origin = r != NULL && r->origin_machine != NULL
                           ? r->origin_machine : "";
        uint64_t rbytes = 0, rupdated = 0;
        size_t rchunks = 0, common = 0;
        size_t kind = 4;

        if (r != NULL) {
            casi_sync_relation rel;

            if ((rc = casi_sync_compare(ctx.repo, l, r, &rel)) != CASI_OK)
                goto done;
            switch (rel) {
            case CASI_SYNC_SAME:         relation = "same";         kind = 0; break;
            case CASI_SYNC_LOCAL_AHEAD:  relation = "local_ahead";  kind = 1; break;
            case CASI_SYNC_REMOTE_AHEAD: relation = "remote_ahead"; kind = 2; break;
            case CASI_SYNC_DIVERGED:     relation = "diverged";     kind = 3; break;
            }
            rbytes = r->bytes;
            rchunks = r->chunk_count;
            rupdated = r->updated_at;
            common = casi_sync_common_prefix(l, r);
        }

        counts[kind]++;
        emit(relation, l->session_id, l->project_path, origin,
             l->bytes, rbytes, l->chunk_count, rchunks, common,
             l->updated_at, rupdated);
    }

    for (i = 0; i < remote.len; i++) {
        casi_entry *r = &remote.items[i];

        if (casi_entry_list_find(&local, r->session_id) != NULL)
            continue;
        counts[5]++;
        emit("remote_only", r->session_id, r->project_path,
             r->origin_machine != NULL ? r->origin_machine : "",
             0, r->bytes, 0, r->chunk_count, 0, 0, r->updated_at);
    }

    printf("summary\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n",
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
    rc = CASI_OK;

done:
    casi_entry_list_dispose(&local);
    casi_entry_list_dispose(&remote);
    casi_asset_list_dispose(&local_assets);
    casi_asset_list_dispose(&remote_assets);
    casi_ctx_dispose(&ctx);
    return rc;
}
