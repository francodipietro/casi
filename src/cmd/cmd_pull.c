/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"
#include "casi/jsonl.h"

#include <inttypes.h>
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

static void report_asset_conflict(const casi_asset *asset)
{
    if (asset->session_id != NULL) {
        casi_warn("auxiliary file differs and was left untouched: project %s, "
                  "session %s, subagent %s",
                  asset->project_path, asset->session_id, asset->name);
    } else {
        casi_warn("auxiliary file differs and was left untouched: project %s, "
                  "project memory %s", asset->project_path, asset->name);
    }
}

/* A parked transcript gets a sibling `<name>.meta.json` with the context the
 * user needs to choose a side later: which project, which machine wrote it,
 * how big, and when. Best-effort: the parked copy is the safety guarantee, the
 * sidecar only makes `casi conflicts` legible. */
static int write_conflict_meta(const char *path, const casi_entry *entry,
                               const char *machine, const char *side)
{
    casi_buf meta_path = CASI_BUF_INIT, json = CASI_BUF_INIT;
    casi_buf sid = CASI_BUF_INIT, project = CASI_BUF_INIT, m = CASI_BUF_INIT;
    int rc;

    if ((rc = casi_buf_printf(&meta_path, "%s.meta.json", path)) != CASI_OK ||
        (rc = casi_json_escape_string(entry->session_id, &sid)) != CASI_OK ||
        (rc = casi_json_escape_string(entry->project_path, &project)) != CASI_OK ||
        (rc = casi_json_escape_string(machine, &m)) != CASI_OK)
        goto done;
    rc = casi_buf_printf(&json,
                         "{\"sessionId\":\"%s\",\"project\":\"%s\","
                         "\"originMachine\":\"%s\",\"side\":\"%s\","
                         "\"bytes\":%" PRIu64 ",\"updatedAt\":%" PRIu64 "}\n",
                         casi_buf_cstr(&sid), casi_buf_cstr(&project),
                         casi_buf_cstr(&m), side, entry->bytes, entry->updated_at);
    if (rc == CASI_OK)
        rc = casi_fs_write_file_atomic(casi_buf_cstr(&meta_path), json.ptr, json.len);

done:
    casi_buf_dispose(&meta_path);
    casi_buf_dispose(&json);
    casi_buf_dispose(&sid);
    casi_buf_dispose(&project);
    casi_buf_dispose(&m);
    return rc;
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
    if (rc == CASI_OK) {
        const char *machine = entry->origin_machine != NULL
                            ? entry->origin_machine : ctx->machine;
        int mrc = write_conflict_meta(casi_buf_cstr(&path), entry, machine, side);

        if (mrc != CASI_OK) {
            casi_error_clear();
            casi_warn("could not record conflict metadata beside %s",
                      casi_buf_cstr(&path));
        }
        casi_info("    %s copy parked at %s", side, casi_buf_cstr(&path));
    }

done:
    casi_buf_dispose(&path);
    casi_buf_dispose(&unmapped);
    return rc;
}

/* The decision-relevant facts about a divergence: which project, how big each
 * side is, which machine the remote copy came from, and how far they agree.
 * Kept beside park_conflict so the report and the action never drift apart. */
static int report_diverged(const casi_ctx *ctx, const casi_entry *l,
                           const casi_entry *r)
{
    const char *origin = r->origin_machine != NULL ? r->origin_machine : "unknown machine";
    size_t total = l->chunk_count > r->chunk_count ? l->chunk_count : r->chunk_count;
    casi_buf lsize = CASI_BUF_INIT, rsize = CASI_BUF_INIT;
    casi_buf ltime = CASI_BUF_INIT, rtime = CASI_BUF_INIT;
    int rc;

    if ((rc = casi_ops_human_size(l->bytes, &lsize)) != CASI_OK ||
        (rc = casi_ops_human_size(r->bytes, &rsize)) != CASI_OK ||
        (rc = casi_ops_format_time(l->updated_at, &ltime)) != CASI_OK ||
        (rc = casi_ops_format_time(r->updated_at, &rtime)) != CASI_OK)
        goto done;

    casi_warn("%s / %.*s diverged", l->project_path, (int)8, l->session_id);
    casi_info("    local  (%s): %s, %zu chunks, modified %s", ctx->machine,
              casi_buf_cstr(&lsize), l->chunk_count, casi_buf_cstr(&ltime));
    casi_info("    remote (%s): %s, %zu chunks, modified %s", origin,
              casi_buf_cstr(&rsize), r->chunk_count, casi_buf_cstr(&rtime));
    casi_info("    share %zu of %zu chunks", casi_sync_common_prefix(l, r), total);

done:
    casi_buf_dispose(&lsize);
    casi_buf_dispose(&rsize);
    casi_buf_dispose(&ltime);
    casi_buf_dispose(&rtime);
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
                if ((rc = report_diverged(&ctx, l, r)) != CASI_OK)
                    goto done;
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
            report_asset_conflict(remote_asset);
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
