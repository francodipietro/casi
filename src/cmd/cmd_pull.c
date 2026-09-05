/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include <string.h>

static void report_unmapped(const casi_buf *root)
{
    casi_err("%s", casi_error_last());
    casi_err("declare it with: casi config root.%s.path <local path>",
             casi_buf_cstr(root));
}

/* Conflict copies carry their side in the filename. A numeric suffix preserves
 * earlier copies when the same session is parked more than once. */
static int conflict_path(const casi_entry *entry, const char *side, casi_buf *path)
{
    const char *dir = casi_paths_conflicts_dir();
    size_t suffix = 0;
    int rc;

    if (dir == NULL)
        return casi_error_last_code();

    for (;;) {
        casi_stat st;

        casi_buf_clear(path);
        if (suffix == 0)
            rc = casi_buf_printf(path, "%s/%s-%s.jsonl", dir,
                                 entry->session_id, side);
        else
            rc = casi_buf_printf(path, "%s/%s-%s-%zu.jsonl", dir,
                                 entry->session_id, side, suffix);
        if (rc != CASI_OK)
            return rc;

        rc = casi_fs_stat(casi_buf_cstr(path), &st);
        if (rc == CASI_ENOTFOUND) {
            casi_error_clear();
            return CASI_OK;
        }
        if (rc != CASI_OK)
            return rc;
        suffix++;
    }
}

static int park_conflict(casi_ctx *ctx, const casi_entry *entry, const char *side)
{
    casi_buf path = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;
    int rc;

    if ((rc = conflict_path(entry, side, &path)) != CASI_OK)
        goto done;

    rc = casi_store_materialize_to(ctx->repo, ctx->roots, entry,
                                   casi_buf_cstr(&path), &unmapped);
    if (rc == CASI_EUNMAPPED)
        report_unmapped(&unmapped);
    if (rc == CASI_OK)
        casi_warn("diverged: %s", entry->session_id);
    if (rc == CASI_OK)
        casi_info("    %s copy parked at %s", side, casi_buf_cstr(&path));

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

        if (l == NULL) {
            relation = CASI_SYNC_REMOTE_AHEAD;
        } else if ((rc = casi_sync_compare(ctx.repo, l, r, &relation)) != CASI_OK) {
            goto done;
        }

        if (relation == CASI_SYNC_SAME || relation == CASI_SYNC_LOCAL_AHEAD)
            continue;

        if (relation == CASI_SYNC_DIVERGED) {
            bool override = take_theirs != NULL &&
                            strcmp(take_theirs, r->session_id) == 0;

            if (!override) {
                conflicts++;
                if (!dry_run && (rc = park_conflict(&ctx, r, "remote")) != CASI_OK)
                    goto done;
                continue;
            }
            if (!dry_run && (rc = park_conflict(&ctx, l, "local")) != CASI_OK)
                goto done;
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
