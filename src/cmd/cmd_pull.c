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

static bool is_excluded(const casi_strvec *exclude, const char *project_path)
{
    size_t i;

    for (i = 0; i < exclude->len; i++)
        if (strcmp(exclude->items[i], project_path) == 0)
            return true;
    return false;
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

static int asset_conflict_path(const casi_asset *asset, casi_buf *path)
{
    const char *dir = casi_paths_conflicts_dir();
    const char *session = asset->session_id != NULL ? asset->session_id : "project";
    size_t suffix = 0;
    int rc;

    if (dir == NULL)
        return casi_error_last_code();

    for (;;) {
        casi_stat st;

        casi_buf_clear(path);
        if (suffix == 0)
            rc = casi_buf_printf(path, "%s/aux-%s-%s-%s-remote", dir,
                                 asset->project_id, session, asset->name);
        else
            rc = casi_buf_printf(path, "%s/aux-%s-%s-%s-remote-%zu", dir,
                                 asset->project_id, session, asset->name, suffix);
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
    casi_asset_list local_assets, remote_assets;
    casi_strvec exclude = CASI_STRVEC_INIT;
    casi_buf unmapped = CASI_BUF_INIT, parked = CASI_BUF_INIT;
    const char *take_theirs = NULL;
    bool dry_run = false;
    size_t pulled = 0, assets_pulled = 0, conflicts = 0, asset_conflicts = 0, i;
    int rc;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    memset(&local_assets, 0, sizeof(local_assets));
    memset(&remote_assets, 0, sizeof(remote_assets));

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

    if ((rc = casi_ops_scan_local(&ctx, &local, &local_assets, NULL)) != CASI_OK)
        goto done;
    if ((rc = casi_ops_load_remote(&ctx, &remote, &remote_assets)) != CASI_OK)
        goto done;
    if ((rc = casi_ctx_exclude_list(&ctx, &exclude)) != CASI_OK)
        goto done;

    for (i = 0; i < remote.len; i++) {
        casi_entry *r = &remote.items[i];
        casi_entry *l = casi_entry_list_find(&local, r->session_id);
        casi_sync_relation relation;

        if (is_excluded(&exclude, r->project_path))
            continue;

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

    for (i = 0; i < remote_assets.len; i++) {
        casi_asset *remote_asset = &remote_assets.items[i];
        casi_asset *local_asset;

        if (is_excluded(&exclude, remote_asset->project_path))
            continue;
        local_asset = casi_asset_list_find(&local_assets, remote_asset->kind,
                                           remote_asset->project_id,
                                           remote_asset->session_id,
                                           remote_asset->name);
        if (local_asset != NULL && git_oid_equal(&local_asset->oid, &remote_asset->oid))
            continue;
        if (local_asset != NULL) {
            casi_warn("auxiliary file differs and was left untouched: %s", remote_asset->name);
            if (!dry_run) {
                if ((rc = asset_conflict_path(remote_asset, &parked)) != CASI_OK)
                    goto done;
                casi_buf_clear(&unmapped);
                rc = casi_store_materialize_asset_to(ctx.repo, ctx.roots, remote_asset,
                                                     casi_buf_cstr(&parked), &unmapped);
                if (rc == CASI_EUNMAPPED) {
                    report_unmapped(&unmapped);
                    goto done;
                }
                if (rc != CASI_OK)
                    goto done;
                casi_info("    remote copy parked at %s", casi_buf_cstr(&parked));
            }
            asset_conflicts++;
            continue;
        }
        if (dry_run) {
            casi_info("would update auxiliary file %s", remote_asset->name);
            assets_pulled++;
            continue;
        }

        casi_buf_clear(&unmapped);
        rc = casi_store_materialize_asset(ctx.repo, ctx.roots, ctx.provider,
                                          remote_asset, &unmapped);
        if (rc == CASI_EUNMAPPED) {
            report_unmapped(&unmapped);
            goto done;
        }
        if (rc != CASI_OK)
            goto done;
        casi_verbose("updated auxiliary file %s", remote_asset->name);
        assets_pulled++;
    }

    casi_info("%s %zu session(s)", dry_run ? "would update" : "updated", pulled);
    if (assets_pulled > 0)
        casi_info("%s %zu auxiliary file(s)", dry_run ? "would update" : "updated",
                  assets_pulled);

    if (conflicts > 0 || asset_conflicts > 0) {
        if (asset_conflicts > 0)
            casi_warn("%zu auxiliary file(s) differ and were left untouched locally",
                      asset_conflicts);
        if (conflicts > 0) {
        casi_warn("%zu session(s) diverged and were left untouched locally", conflicts);
        casi_info("  take the remote copy with: casi pull --theirs <session-id>");
        }
        rc = CASI_ECONFLICT;
    } else {
        rc = CASI_OK;
    }

done:
    casi_buf_dispose(&unmapped);
    casi_buf_dispose(&parked);
    casi_entry_list_dispose(&local);
    casi_entry_list_dispose(&remote);
    casi_asset_list_dispose(&local_assets);
    casi_asset_list_dispose(&remote_assets);
    casi_strvec_dispose(&exclude);
    casi_ctx_dispose(&ctx);
    return rc;
}
