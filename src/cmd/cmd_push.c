/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include <string.h>

int casi_cmd_push(int argc, char **argv)
{
    casi_ctx ctx;
    casi_entry_list local;
    casi_buf refname = CASI_BUF_INIT, message = CASI_BUF_INIT;
    git_oid tree, existing_tree, commit;
    bool dry_run = false, unchanged = false;
    size_t excluded = 0;
    int i, rc;

    memset(&local, 0, sizeof(local));

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = true;
        else
            return casi_error_set(CASI_EINVAL, "usage: %s",
                                  casi_command_lookup("push")->usage);
    }

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        return rc;

    if ((rc = casi_ops_scan_local(&ctx, &local, &excluded)) != CASI_OK)
        goto done;

    if ((rc = casi_store_write_tree(ctx.repo, ctx.provider->name, &local, &tree)) != CASI_OK)
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

    casi_verbose("%zu session(s) scanned, %zu excluded", local.len, excluded);

    if (dry_run) {
        casi_info(unchanged ? "nothing to push" : "would push %zu session(s)", local.len);
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

    casi_info(unchanged ? "already up to date" : "pushed %zu session(s)", local.len);

done:
    casi_buf_dispose(&refname);
    casi_buf_dispose(&message);
    casi_entry_list_dispose(&local);
    casi_ctx_dispose(&ctx);
    return rc;
}
