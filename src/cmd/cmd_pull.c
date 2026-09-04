/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include <string.h>
#include <time.h>

static void report_unmapped(const casi_buf *root)
{
    casi_err("%s", casi_error_last());
    casi_err("declare it with: casi config root.%s.path <local path>",
             casi_buf_cstr(root));
}

/*
 * Parks the remote side of a divergence instead of overwriting the local
 * transcript. Nothing is destroyed and nothing prompts, which is what keeps
 * `casi sync` safe to run unattended at login.
 */
static int park_conflict(casi_ctx *ctx, const casi_entry *remote)
{
    casi_buf path = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;
    const char *dir = casi_paths_conflicts_dir();
    int rc;

    if (dir == NULL)
        return casi_error_last_code();

    if ((rc = casi_buf_printf(&path, "%s/%s-%lld.jsonl", dir,
                              remote->session_id, (long long)time(NULL))) != CASI_OK)
        goto done;

    rc = casi_store_materialize_to(ctx->repo, ctx->roots, remote,
                                   casi_buf_cstr(&path), &unmapped);
    if (rc == CASI_EUNMAPPED)
        report_unmapped(&unmapped);
    if (rc == CASI_OK)
        casi_warn("diverged: %s", remote->session_id);
    if (rc == CASI_OK)
        casi_info("    remote copy parked at %s", casi_buf_cstr(&path));

done:
    casi_buf_dispose(&path);
    casi_buf_dispose(&unmapped);
    return rc;
}

int casi_cmd_pull(int argc, char **argv)
{
    casi_ctx ctx;
    casi_entry_list local, remote;
    casi_buf unmapped = CASI_BUF_INIT;
    const char *take_theirs = NULL;
    bool dry_run = false;
    size_t pulled = 0, conflicts = 0, i;
    int rc;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));

    for (i = 0; (int)i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--theirs") == 0 && (int)i + 1 < argc) {
            take_theirs = argv[++i];
        } else {
            return casi_error_set(CASI_EINVAL, "usage: %s",
                                  casi_command_lookup("pull")->usage);
        }
    }

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        return rc;

    if (!dry_run && (rc = casi_repo_fetch(ctx.repo)) != CASI_OK)
        goto done;

    if ((rc = casi_ops_scan_local(&ctx, &local, NULL)) != CASI_OK)
        goto done;
    if ((rc = casi_ops_load_remote(&ctx, &remote)) != CASI_OK)
        goto done;

    for (i = 0; i < remote.len; i++) {
        casi_entry *r = &remote.items[i];
        casi_entry *l = casi_entry_list_find(&local, r->session_id);
        casi_sync_relation relation;

        relation = (l == NULL) ? CASI_SYNC_REMOTE_AHEAD : casi_sync_compare(l, r);

        if (relation == CASI_SYNC_SAME || relation == CASI_SYNC_LOCAL_AHEAD)
            continue;

        if (relation == CASI_SYNC_DIVERGED) {
            bool override = take_theirs != NULL &&
                            strcmp(take_theirs, r->session_id) == 0;

            if (!override) {
                conflicts++;
                if (!dry_run && (rc = park_conflict(&ctx, r)) != CASI_OK)
                    goto done;
                continue;
            }
            casi_info("taking the remote copy of %s", r->session_id);
        }

        if (dry_run) {
            casi_info("would update %s", r->session_id);
            pulled++;
            continue;
        }

        casi_buf_clear(&unmapped);
        rc = casi_store_materialize(ctx.repo, ctx.roots, ctx.provider, r, &unmapped);
        if (rc == CASI_EUNMAPPED) {
            report_unmapped(&unmapped);
            goto done;
        }
        if (rc != CASI_OK)
            goto done;

        casi_verbose("updated %s", r->session_id);
        pulled++;
    }

    casi_info("%s %zu session(s)", dry_run ? "would update" : "updated", pulled);

    if (conflicts > 0) {
        casi_warn("%zu session(s) diverged and were left untouched locally", conflicts);
        casi_info("  take the remote copy with: casi pull --theirs <session-id>");
        rc = CASI_ECONFLICT;
    } else {
        rc = CASI_OK;
    }

done:
    casi_buf_dispose(&unmapped);
    casi_entry_list_dispose(&local);
    casi_entry_list_dispose(&remote);
    casi_ctx_dispose(&ctx);
    return rc;
}
