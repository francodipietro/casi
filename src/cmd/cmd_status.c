/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int human_size(uint64_t bytes, casi_buf *out)
{
    casi_buf_clear(out);
    if (bytes >= 1024ULL * 1024 * 1024)
        return casi_buf_printf(out, "%.1f GB", (double)bytes / (1024 * 1024 * 1024));
    if (bytes >= 1024 * 1024)
        return casi_buf_printf(out, "%.1f MB", (double)bytes / (1024 * 1024));
    if (bytes >= 1024)
        return casi_buf_printf(out, "%.0f KB", (double)bytes / 1024);
    return casi_buf_printf(out, "%" PRIu64 " B", bytes);
}

static int status_porcelain(const casi_sync_summary *s)
{
    printf("push\t%zu\t%" PRIu64 "\n", s->to_push, s->push_bytes);
    printf("pull\t%zu\t%" PRIu64 "\n", s->to_pull, s->pull_bytes);
    printf("current\t%zu\n", s->up_to_date);
    printf("conflict\t%zu\n", s->conflicts);
    printf("excluded\t%zu\n", s->excluded);
    printf("aux-push\t%zu\t%" PRIu64 "\n", s->auxiliary_to_push,
           s->auxiliary_push_bytes);
    printf("aux-pull\t%zu\t%" PRIu64 "\n", s->auxiliary_to_pull,
           s->auxiliary_pull_bytes);
    printf("aux-conflict\t%zu\n", s->auxiliary_conflicts);
    return s->conflicts > 0 || s->auxiliary_conflicts > 0 ? CASI_ECONFLICT : CASI_OK;
}

int casi_cmd_status(int argc, char **argv)
{
    casi_ctx ctx;
    casi_entry_list local, remote;
    casi_asset_list local_assets, remote_assets;
    casi_sync_summary summary;
    casi_buf url = CASI_BUF_INIT, size = CASI_BUF_INIT;
    bool porcelain = false;
    size_t excluded;
    int i, rc;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    memset(&local_assets, 0, sizeof(local_assets));
    memset(&remote_assets, 0, sizeof(remote_assets));

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--porcelain") == 0)
            porcelain = true;
        else
            return casi_error_set(CASI_EINVAL, "usage: %s",
                                  casi_command_lookup("status")->usage);
    }

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        return rc;

    if ((rc = casi_ops_scan_local(&ctx, &local, &local_assets, &excluded)) != CASI_OK)
        goto done;
    if ((rc = casi_ops_load_remote(&ctx, &remote, &remote_assets)) != CASI_OK)
        goto done;

    if ((rc = casi_ops_summarise(ctx.repo, &local, &remote, &local_assets,
                                 &remote_assets, &summary)) != CASI_OK)
        goto done;
    summary.excluded = excluded;

    if (porcelain) {
        rc = status_porcelain(&summary);
        goto done;
    }

    rc = casi_repo_remote_url(ctx.repo, &url);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        if ((rc = casi_buf_puts(&url, "(none configured)")) != CASI_OK)
            goto done;
    } else if (rc != CASI_OK) {
        goto done;
    }

    casi_info("machine: %s   remote: %s", ctx.machine, casi_buf_cstr(&url));
    casi_info("%s", "");

    if ((rc = human_size(summary.push_bytes, &size)) != CASI_OK)
        goto done;
    casi_info("  to push      %4zu sessions   %s", summary.to_push, casi_buf_cstr(&size));
    if ((rc = human_size(summary.pull_bytes, &size)) != CASI_OK)
        goto done;
    casi_info("  to pull      %4zu sessions   %s", summary.to_pull, casi_buf_cstr(&size));
    casi_info("  up to date   %4zu sessions", summary.up_to_date);
    if (summary.excluded > 0)
        casi_info("  excluded     %4zu sessions", summary.excluded);
    if (summary.auxiliary_to_push > 0) {
        if ((rc = human_size(summary.auxiliary_push_bytes, &size)) != CASI_OK)
            goto done;
        casi_info("  aux to push  %4zu files      %s", summary.auxiliary_to_push,
                  casi_buf_cstr(&size));
    }
    if (summary.auxiliary_to_pull > 0) {
        if ((rc = human_size(summary.auxiliary_pull_bytes, &size)) != CASI_OK)
            goto done;
        casi_info("  aux to pull  %4zu files      %s", summary.auxiliary_to_pull,
                  casi_buf_cstr(&size));
    }

    /* "to pull" counts what the last fetch saw, not what is on the remote
     * right now: status stays offline and instant, like git's. */
    casi_info("%s", "");
    casi_info("  (remote figures are from the last fetch; run `casi pull` to refresh)");

    if (summary.conflicts > 0 || summary.auxiliary_conflicts > 0) {
        casi_info("%s", "");
        if (summary.conflicts > 0) {
            casi_warn("%zu session(s) diverged between machines", summary.conflicts);
            casi_info("  run `casi pull` to park the remote copies for inspection");
        }
        if (summary.auxiliary_conflicts > 0)
            casi_warn("%zu auxiliary file(s) differ between machines",
                      summary.auxiliary_conflicts);
        rc = CASI_ECONFLICT;
    }

done:
    casi_buf_dispose(&url);
    casi_buf_dispose(&size);
    casi_entry_list_dispose(&local);
    casi_entry_list_dispose(&remote);
    casi_asset_list_dispose(&local_assets);
    casi_asset_list_dispose(&remote_assets);
    casi_ctx_dispose(&ctx);
    return rc;
}
