/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include <string.h>

int casi_cmd_push(int argc, char **argv)
{
    casi_ctx ctx;
    casi_entry_list local;
    casi_asset_list local_assets;
    casi_buf refname = CASI_BUF_INIT, message = CASI_BUF_INIT;
    git_oid tree, existing_tree, commit;
    bool dry_run = false, unchanged = false;
    size_t excluded = 0;
    int i, rc;

    memset(&local, 0, sizeof(local));
    memset(&local_assets, 0, sizeof(local_assets));

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = true;
        else
            return casi_error_set(CASI_EINVAL, "usage: %s",
                                  casi_command_lookup("push")->usage);
    }

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        return rc;

    /* Publish/migrate config before scanning. In particular, a legacy local
     * exclusion reaches the remote before this command can upload sessions. */
    if (!dry_run && (rc = casi_ctx_publish_shared_config(&ctx)) != CASI_OK)
        goto done;

    if ((rc = casi_ops_scan_local(&ctx, &local, &local_assets, &excluded)) != CASI_OK)
        goto done;

    if ((rc = casi_store_write_tree(ctx.repo, ctx.provider->name, &local,
                                    &local_assets, &tree)) != CASI_OK)
        goto done;

    if ((rc = casi_ctx_local_ref(&ctx, &refname)) != CASI_OK)
        goto done;

    /* An identical tree means nothing changed since the last push. Recording
     * a commit anyway would grow the history with noise and make every run
     * look like it did something. */
    if (casi_repo_ref_tree(ctx.repo, casi_buf_cstr(&refname), &existing_tree) == CASI_OK &&
        git_oid_equal(&tree, &existing_tree))
        unchanged = true;
    else
        casi_error_clear();

    casi_verbose("%zu session(s), %zu auxiliary file(s) scanned, %zu excluded",
                 local.len, local_assets.len, excluded);

    if (dry_run) {
        if (unchanged)
            casi_info("nothing to push");
        else
            casi_info("would push %zu session(s)", local.len);
        rc = CASI_OK;
        goto done;
    }

    if (!unchanged) {
        if ((rc = casi_buf_printf(&message, "%s: %zu session(s)",
                                  ctx.machine, local.len)) != CASI_OK)
            goto done;
        if ((rc = casi_repo_commit(ctx.repo, casi_buf_cstr(&refname), &tree,
                                   ctx.machine, casi_buf_cstr(&message),
                                   &commit)) != CASI_OK)
            goto done;
    }

    if ((rc = casi_repo_push(ctx.repo, casi_buf_cstr(&refname))) != CASI_OK)
        goto done;

    if (unchanged)
        casi_info("already up to date");
    else
        casi_info("pushed %zu session(s)", local.len);

done:
    casi_buf_dispose(&refname);
    casi_buf_dispose(&message);
    casi_entry_list_dispose(&local);
    casi_asset_list_dispose(&local_assets);
    casi_ctx_dispose(&ctx);
    return rc;
}
