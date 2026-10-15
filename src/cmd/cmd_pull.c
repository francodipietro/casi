/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"
#include "casi/index.h"
#include "casi/jsonl.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

static void report_unmapped(const casi_buf *root)
{
    (void)root;
    /* The error itself names the project and explains it is not here yet; the
     * old "declare a root" hint no longer applies now that projects match by
     * basename automatically. */
    casi_err("%s", casi_error_last());
}

static bool is_excluded(const casi_strvec *exclude, const char *project_path)
{
    size_t i;

    for (i = 0; i < exclude->len; i++)
        if (strcmp(exclude->items[i], project_path) == 0)
            return true;
    return false;
}

/* Conflict copies are addressed by the materialised content id, rather than
 * by retry order. Re-running sync therefore reuses the same archive entry;
 * a genuinely new revision gets a distinct, inspectable copy. */
static int archive_content_id(casi_repo *repo, const casi_buf *content,
                              char out[GIT_OID_HEXSZ + 1])
{
    git_oid oid;
    int rc;

    if ((rc = casi_repo_write_blob(repo, content->ptr, content->len, &oid)) != CASI_OK)
        return rc;
    git_oid_tostr(out, GIT_OID_HEXSZ + 1, &oid);
    return CASI_OK;
}

static int conflict_path(casi_ctx *ctx, const casi_entry *entry, const char *side,
                         const casi_buf *content, casi_buf *path, bool *already_out)
{
    const char *dir = casi_paths_conflicts_dir();
    char id[GIT_OID_HEXSZ + 1];
    casi_stat st;
    int rc;

    if (dir == NULL)
        return casi_error_last_code();

    casi_buf_clear(path);
    if ((rc = archive_content_id(ctx->repo, content, id)) != CASI_OK ||
        (rc = casi_buf_printf(path, "%s/%s-%s-%s.jsonl", dir,
                              entry->session_id, side, id)) != CASI_OK)
        return rc;
    rc = casi_fs_stat(casi_buf_cstr(path), &st);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        *already_out = false;
        return CASI_OK;
    }
    if (rc != CASI_OK)
        return rc;
    *already_out = true;
    return CASI_OK;
}

static int asset_conflict_path(casi_ctx *ctx, const casi_asset *asset, const char *side,
                               const casi_buf *content, casi_buf *path,
                               bool *already_out)
{
    const char *dir = casi_paths_conflicts_dir();
    const char *session = asset->session_id != NULL ? asset->session_id : "project";
    char id[GIT_OID_HEXSZ + 1];
    casi_stat st;
    int rc;

    if (dir == NULL)
        return casi_error_last_code();

    casi_buf_clear(path);
    if ((rc = archive_content_id(ctx->repo, content, id)) != CASI_OK ||
        (rc = casi_buf_printf(path, "%s/aux-%s-%s-%s-%s-%s", dir,
                              asset->project_id, session, asset->name, side, id)) != CASI_OK)
        return rc;
    rc = casi_fs_stat(casi_buf_cstr(path), &st);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        *already_out = false;
        return CASI_OK;
    }
    if (rc != CASI_OK)
        return rc;
    *already_out = true;
    return CASI_OK;
}

static int park_asset_conflict(casi_ctx *ctx, const casi_asset *asset,
                               const char *side, casi_buf *path,
                               casi_buf *unmapped)
{
    casi_buf content = CASI_BUF_INIT;
    bool already;
    int rc = casi_store_materialize_asset_content(ctx->repo, ctx->roots,
                                                   asset, &content, unmapped);

    if (rc == CASI_OK)
        rc = asset_conflict_path(ctx, asset, side, &content, path, &already);
    if (rc == CASI_OK && !already)
        rc = casi_fs_write_file_atomic(casi_buf_cstr(path), content.ptr, content.len);
    casi_buf_dispose(&content);
    return rc;
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

static void report_auto_asset_selection(bool dry_run, bool remote_wins,
                                        const casi_asset *asset)
{
    const char *when = dry_run ? "would auto-resolve with" : "auto-resolved with";
    const char *side = remote_wins ? "remote" : "local";

    if (asset->session_id != NULL)
        casi_info("%s %s auxiliary file %s for session %s", when, side,
                  asset->name, asset->session_id);
    else
        casi_info("%s %s project memory %s", when, side, asset->name);
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
                         "\"bytes\":%" PRIu64 ",\"updatedAt\":%" PRIu64 ","
                         "\"parkedAt\":%" PRIu64 "}\n",
                         casi_buf_cstr(&sid), casi_buf_cstr(&project),
                         casi_buf_cstr(&m), side, entry->bytes, entry->updated_at,
                         (uint64_t)time(NULL));
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
    casi_buf path = CASI_BUF_INIT, unmapped = CASI_BUF_INIT, content = CASI_BUF_INIT;
    bool already;
    int rc;

    rc = casi_store_materialize_import_content(ctx->repo, ctx->roots, entry,
                                               &content, &unmapped);
    if (rc == CASI_EUNMAPPED)
        report_unmapped(&unmapped);
    if (rc != CASI_OK)
        goto done;
    if ((rc = conflict_path(ctx, entry, side, &content, &path, &already)) != CASI_OK)
        goto done;
    if (!already)
        rc = casi_fs_write_file_atomic(casi_buf_cstr(&path),
                                       content.ptr, content.len);
    if (rc == CASI_OK) {
        const char *machine = entry->origin_machine != NULL
                            ? entry->origin_machine : ctx->machine;
        int mrc = already ? CASI_OK :
            write_conflict_meta(casi_buf_cstr(&path), entry, machine, side);

        if (mrc != CASI_OK) {
            casi_error_clear();
            casi_warn("could not record conflict metadata beside %s",
                      casi_buf_cstr(&path));
        }
        casi_info("    %s copy %s at %s", side,
                  already ? "already parked" : "parked",
                  casi_buf_cstr(&path));
    }

done:
    casi_buf_dispose(&path);
    casi_buf_dispose(&unmapped);
    casi_buf_dispose(&content);
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

/* A transcript's own event timestamps travel with it; filesystem mtimes do
 * not. ISO-8601 UTC strings compare chronologically byte-for-byte. If an old
 * or malformed transcript has no usable timestamp, choose by its immutable
 * chunk ids so every machine reaches the same answer and archives the other
 * copy. */
static int entry_tiebreak(const casi_entry *a, const casi_entry *b)
{
    size_t n = a->chunk_count < b->chunk_count ? a->chunk_count : b->chunk_count;
    size_t i;

    for (i = 0; i < n; i++) {
        int cmp = git_oid_cmp(&a->chunks[i], &b->chunks[i]);

        if (cmp != 0)
            return cmp;
    }
    if (a->chunk_count != b->chunk_count)
        return a->chunk_count < b->chunk_count ? -1 : 1;
    return strcmp(a->project_path, b->project_path);
}

static int auto_session_winner(casi_ctx *ctx, const casi_entry *local,
                               const casi_entry *remote, bool *remote_out)
{
    casi_buf l = CASI_BUF_INIT, r = CASI_BUF_INIT;
    casi_buf ltime = CASI_BUF_INIT, rtime = CASI_BUF_INIT, unmapped = CASI_BUF_INIT;
    bool have_l, have_r;
    int rc;

    if ((rc = casi_store_materialize_import_content(ctx->repo, ctx->roots, local,
                                                     &l, &unmapped)) != CASI_OK ||
        (rc = casi_store_materialize_import_content(ctx->repo, ctx->roots, remote,
                                                     &r, &unmapped)) != CASI_OK)
        goto done;
    have_l = casi_jsonl_last_string(l.ptr, l.len, "timestamp", &ltime);
    have_r = casi_jsonl_last_string(r.ptr, r.len, "timestamp", &rtime);
    if (have_l && have_r && strcmp(casi_buf_cstr(&ltime), casi_buf_cstr(&rtime)) != 0)
        *remote_out = strcmp(casi_buf_cstr(&rtime), casi_buf_cstr(&ltime)) > 0;
    else if (have_r != have_l)
        *remote_out = have_r;
    else
        *remote_out = entry_tiebreak(remote, local) < 0;
    rc = CASI_OK;

done:
    casi_buf_dispose(&l);
    casi_buf_dispose(&r);
    casi_buf_dispose(&ltime);
    casi_buf_dispose(&rtime);
    casi_buf_dispose(&unmapped);
    return rc;
}

/* Session sidecars form one conversation with their parent transcript. When
 * that parent genuinely forked, choosing a sidecar by an unrelated blob hash
 * could mix two branches of the same conversation. Follow the parent's
 * timestamp decision instead. Project memory has no parent session, so it
 * retains the deterministic object-id fallback. */
static int auto_asset_remote_wins(casi_ctx *ctx, const casi_asset *local,
                                  const casi_asset *remote,
                                  casi_entry_list *local_sessions,
                                  casi_entry_list *remote_sessions,
                                  bool *remote_out)
{
    casi_buf local_content = CASI_BUF_INIT, remote_content = CASI_BUF_INIT;
    casi_buf local_time = CASI_BUF_INIT, remote_time = CASI_BUF_INIT;
    casi_buf unmapped = CASI_BUF_INIT;
    bool have_local, have_remote;
    int rc = CASI_OK;

    if (local->session_id != NULL && remote->session_id != NULL &&
        strcmp(local->session_id, remote->session_id) == 0) {
        casi_entry *local_session = casi_entry_list_find(local_sessions, local->session_id);
        casi_entry *remote_session = casi_entry_list_find(remote_sessions, remote->session_id);

        if (local_session != NULL && remote_session != NULL) {
            casi_sync_relation relation;
            rc = casi_sync_compare(ctx->repo, ctx->roots, local_session,
                                   remote_session, &relation);

            if (rc != CASI_OK)
                goto done;
            if (relation == CASI_SYNC_DIVERGED)
                rc = auto_session_winner(ctx, local_session, remote_session,
                                         remote_out);
            if (rc != CASI_OK || relation == CASI_SYNC_DIVERGED)
                goto done;
        }
    }

    /* Claude subagent transcripts are JSONL too. If their parent did not
     * fork, the sidecar's own last event is the most useful independent
     * signal; metadata and old formats simply fall through to the stable
     * object-id choice. */
    if ((rc = casi_store_materialize_asset_content(ctx->repo, ctx->roots, local,
                                                   &local_content, &unmapped)) != CASI_OK ||
        (rc = casi_store_materialize_asset_content(ctx->repo, ctx->roots, remote,
                                                   &remote_content, &unmapped)) != CASI_OK)
        goto done;
    have_local = casi_jsonl_last_string(local_content.ptr, local_content.len,
                                        "timestamp", &local_time);
    have_remote = casi_jsonl_last_string(remote_content.ptr, remote_content.len,
                                         "timestamp", &remote_time);
    if (have_local && have_remote &&
        strcmp(casi_buf_cstr(&local_time), casi_buf_cstr(&remote_time)) != 0) {
        *remote_out = strcmp(casi_buf_cstr(&remote_time), casi_buf_cstr(&local_time)) > 0;
        goto done;
    }
    if (have_local != have_remote) {
        *remote_out = have_remote;
        goto done;
    }
    *remote_out = git_oid_cmp(&remote->oid, &local->oid) < 0;

done:
    casi_buf_dispose(&local_content);
    casi_buf_dispose(&remote_content);
    casi_buf_dispose(&local_time);
    casi_buf_dispose(&remote_time);
    casi_buf_dispose(&unmapped);
    return rc;
}

/* Validate the complete set before parking or replacing any file. A missing
 * root inside a later transcript used to leave earlier conflicts parked on
 * every retry, producing -remote-1, -remote-2, and so on. */
static int preflight_pull(casi_ctx *ctx, casi_entry_list *local,
                          casi_entry_list *remote, casi_asset_list *local_assets,
                          casi_asset_list *remote_assets,
                          const casi_strvec *exclude)
{
    casi_buf unmapped = CASI_BUF_INIT;
    size_t i;
    int rc = CASI_OK;

    for (i = 0; i < remote->len; i++) {
        casi_entry *r = &remote->items[i];
        casi_entry *l = casi_entry_list_find(local, r->session_id);
        casi_sync_relation relation = CASI_SYNC_REMOTE_AHEAD;
        if (is_excluded(exclude, r->project_path))
            continue;
        if (l != NULL &&
            (rc = casi_sync_compare(ctx->repo, ctx->roots, l, r, &relation)) != CASI_OK)
            goto done;
        if (relation == CASI_SYNC_SAME || relation == CASI_SYNC_LOCAL_AHEAD)
            continue;

        rc = casi_store_validate_entry(ctx->repo, ctx->roots, ctx->provider,
                                       r, &unmapped);
        if (rc != CASI_OK)
            goto done;
        if (relation == CASI_SYNC_DIVERGED)
            rc = casi_store_validate_entry(ctx->repo, ctx->roots, ctx->provider,
                                           l, &unmapped);
        if (rc != CASI_OK)
            goto done;
    }

    for (i = 0; i < remote_assets->len; i++) {
        casi_asset *r = &remote_assets->items[i];
        casi_asset *l;

        if (is_excluded(exclude, r->project_path))
            continue;
        l = casi_asset_list_find(local_assets, r->kind, r->project_id,
                                 r->session_id, r->name);
        if (l != NULL) {
            bool equal;

            if ((rc = casi_store_asset_equal(ctx->repo, ctx->roots,
                                             l, r, &equal)) != CASI_OK)
                goto done;
            if (equal)
                continue;
        }
        rc = casi_store_validate_asset(ctx->repo, ctx->roots, ctx->provider,
                                       r, &unmapped);
        if (rc != CASI_OK)
            goto done;
        if (l != NULL)
            rc = casi_store_validate_asset(ctx->repo, ctx->roots, ctx->provider,
                                           l, &unmapped);
        if (rc != CASI_OK)
            goto done;
    }

done:
    if (rc == CASI_EUNMAPPED)
        report_unmapped(&unmapped);
    casi_buf_dispose(&unmapped);
    return rc;
}

int casi_cmd_pull(int argc, char **argv)
{
    casi_ctx ctx, scan_ctx;
    casi_entry_list local, remote;
    casi_asset_list local_assets, remote_assets;
    casi_repo *scratch_repo = NULL;
    casi_strvec exclude = CASI_STRVEC_INIT;
    casi_buf unmapped = CASI_BUF_INIT, parked = CASI_BUF_INIT;
    const char *take_theirs = NULL;
    bool dry_run = false;
    bool theirs_all = false;
    bool materialized = false;
    bool strict = false;
    size_t pulled = 0, assets_pulled = 0, conflicts = 0, asset_conflicts = 0;
    size_t auto_sessions = 0, auto_assets = 0, i;
    int rc;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    memset(&local_assets, 0, sizeof(local_assets));
    memset(&remote_assets, 0, sizeof(remote_assets));

    for (i = 0; (int)i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (strcmp(argv[i], "--theirs-all") == 0) {
            theirs_all = true;
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

    /* Scanning normally writes newly seen chunks and refreshes the stat cache.
     * A preview must not even do that: use a throwaway object database whose
     * alternate points at the real, read-only store. */
    scan_ctx = ctx;
    if (dry_run) {
        if ((rc = casi_repo_scratch(&scratch_repo, ctx.repo)) != CASI_OK)
            goto done;
        scan_ctx.repo = scratch_repo;
    }

    if ((rc = (dry_run ? casi_ops_scan_local_read_only(&scan_ctx, &local,
                                                        &local_assets, NULL)
                       : casi_ops_scan_local(&scan_ctx, &local, &local_assets,
                                             NULL))) != CASI_OK)
        goto done;
    if ((rc = casi_ops_load_remote(&ctx, &remote, &remote_assets)) != CASI_OK)
        goto done;
    if ((rc = casi_ctx_exclude_list(&ctx, &exclude)) != CASI_OK)
        goto done;
    if ((rc = preflight_pull(&scan_ctx, &local, &remote, &local_assets, &remote_assets,
                             &exclude)) != CASI_OK)
        goto done;

    for (i = 0; i < remote.len; i++) {
        casi_entry *r = &remote.items[i];
        casi_entry *l = casi_entry_list_find(&local, r->session_id);
        casi_sync_relation relation;

        if (is_excluded(&exclude, r->project_path))
            continue;

        if (l == NULL) {
            relation = CASI_SYNC_REMOTE_AHEAD;
        } else if ((rc = casi_sync_compare(scan_ctx.repo, ctx.roots, l, r, &relation)) != CASI_OK) {
            goto done;
        }

        if (relation == CASI_SYNC_SAME || relation == CASI_SYNC_LOCAL_AHEAD)
            continue;

        if (relation == CASI_SYNC_DIVERGED) {
            bool override = theirs_all ||
                            (take_theirs != NULL &&
                             strcmp(take_theirs, r->session_id) == 0);
            bool remote_wins = override;

            if (!override && !strict &&
                (rc = auto_session_winner(&scan_ctx, l, r, &remote_wins)) != CASI_OK)
                goto done;

            if (!override && strict) {
                conflicts++;
                if ((rc = report_diverged(&ctx, l, r)) != CASI_OK)
                    goto done;
                if (!dry_run && (rc = park_conflict(&ctx, r, "remote")) != CASI_OK)
                    goto done;
                continue;
            }
            if (remote_wins) {
                const char *side = override ? "local" : "archived-local";

                if (!dry_run && (rc = park_conflict(&ctx, l, side)) != CASI_OK)
                    goto done;
                if (override)
                    casi_info("%s remote copy of %s", dry_run ? "would take" : "taking",
                              r->session_id);
                else
                    casi_info("%s remote copy of %s",
                              dry_run ? "would auto-resolve with" : "auto-resolved with",
                              r->session_id);
                if (!override)
                    auto_sessions++;
            } else {
                if (!dry_run && (rc = park_conflict(&ctx, r, "archived-remote")) != CASI_OK)
                    goto done;
                casi_info("%s local copy of %s",
                          dry_run ? "would auto-resolve with" : "auto-resolved with",
                          r->session_id);
                auto_sessions++;
                continue;
            }
        }

        if (dry_run) {
            casi_info("would update %s", r->session_id);
            pulled++;
            continue;
        }

        casi_progress("materializing session %zu/%zu", i + 1, remote.len);
        casi_buf_clear(&unmapped);
        rc = casi_store_materialize(scan_ctx.repo, ctx.roots, ctx.provider, r, &unmapped);
        if (rc == CASI_EUNMAPPED) {
            report_unmapped(&unmapped);
            goto done;
        }
        if (rc != CASI_OK)
            goto done;

        casi_verbose("updated %s", r->session_id);
        materialized = true;
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
        if (local_asset != NULL) {
            bool equal;

            if ((rc = casi_store_asset_equal(scan_ctx.repo, ctx.roots, local_asset,
                                             remote_asset, &equal)) != CASI_OK)
                goto done;
            if (equal)
                continue;
        }
        if (local_asset != NULL) {
            bool remote_wins = theirs_all;

            if (!theirs_all && !strict &&
                (rc = auto_asset_remote_wins(&scan_ctx, local_asset, remote_asset,
                                             &local, &remote, &remote_wins)) != CASI_OK)
                goto done;
            if (remote_wins) {
                /* Park the local copy, then take the remote one. */
                if (!dry_run) {
                    casi_buf_clear(&unmapped);
                    rc = park_asset_conflict(&ctx, local_asset,
                                             theirs_all ? "local" : "archived-local", &parked,
                                             &unmapped);
                    if (rc == CASI_EUNMAPPED) {
                        report_unmapped(&unmapped);
                        goto done;
                    }
                    if (rc != CASI_OK)
                        goto done;

                    casi_buf_clear(&unmapped);
                    rc = casi_store_materialize_asset(scan_ctx.repo, ctx.roots, ctx.provider,
                                                      remote_asset, &unmapped);
                    if (rc == CASI_EUNMAPPED) {
                        report_unmapped(&unmapped);
                        goto done;
                    }
                    if (rc != CASI_OK)
                        goto done;
                }
                assets_pulled++;
                materialized = true;
                if (!theirs_all) {
                    report_auto_asset_selection(dry_run, true, remote_asset);
                    auto_assets++;
                }
                continue;
            }
            if (!strict) {
                if (!dry_run) {
                    casi_buf_clear(&unmapped);
                    rc = park_asset_conflict(&ctx, remote_asset, "archived-remote",
                                             &parked, &unmapped);
                    if (rc == CASI_EUNMAPPED) {
                        report_unmapped(&unmapped);
                        goto done;
                    }
                    if (rc != CASI_OK)
                        goto done;
                }
                report_auto_asset_selection(dry_run, false, remote_asset);
                auto_assets++;
                continue;
            }
            report_asset_conflict(remote_asset);
            if (!dry_run) {
                casi_buf_clear(&unmapped);
                rc = park_asset_conflict(&ctx, remote_asset, "remote", &parked,
                                         &unmapped);
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
        rc = casi_store_materialize_asset(scan_ctx.repo, ctx.roots, ctx.provider,
                                          remote_asset, &unmapped);
        if (rc == CASI_EUNMAPPED) {
            report_unmapped(&unmapped);
            goto done;
        }
        if (rc != CASI_OK)
            goto done;
        casi_verbose("updated auxiliary file %s", remote_asset->name);
        materialized = true;
        assets_pulled++;
    }

    if (materialized && !dry_run && (rc = casi_index_invalidate()) != CASI_OK)
        goto done;

    casi_progress_done();
    casi_info("%s %zu session(s)", dry_run ? "would update" : "updated", pulled);
    if (assets_pulled > 0)
        casi_info("%s %zu auxiliary file(s)", dry_run ? "would update" : "updated",
                  assets_pulled);
    if (auto_sessions > 0 || auto_assets > 0) {
        if (dry_run)
            casi_info("would auto-resolve %zu session(s) and %zu auxiliary file(s); "
                      "losing copies would be archived", auto_sessions, auto_assets);
        else
            casi_info("auto-resolved %zu session(s) and %zu auxiliary file(s); "
                      "previous copies archived", auto_sessions, auto_assets);
    }

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
    casi_repo_free(scratch_repo);
    casi_ctx_dispose(&ctx);
    return rc;
}
