/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* `casi_ops_load_remote()` deliberately omits our own machine branch: it is
 * the input for pull.  Status needs a second comparison for push, though:
 * comparing local content to the other machines made already-published,
 * machine-local sessions look perpetually "to push". */
static int publication_pending(casi_ctx *ctx, casi_entry_list *local,
                               casi_asset_list *local_assets,
                               casi_sync_summary *out)
{
    casi_entry_list published = { 0 };
    casi_asset_list published_assets = { 0 };
    casi_buf refname = CASI_BUF_INIT;
    git_oid tree;
    size_t i;
    int rc;

    memset(out, 0, sizeof(*out));
    /* The local machine-owned ref is the snapshot that `push` commits before
     * publishing.  Unlike refs/remotes/origin it is updated immediately by a
     * successful push, so status does not need another fetch just to forget
     * work it has already sent. */
    if ((rc = casi_buf_printf(&refname, "refs/heads/casi/%s", ctx->machine)) != CASI_OK)
        goto done;
    rc = casi_repo_ref_tree(ctx->repo, casi_buf_cstr(&refname), &tree);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
    } else if (rc != CASI_OK) {
        goto done;
    } else if ((rc = casi_store_read_tree(ctx->repo, &tree, ctx->provider->name,
                                          &published, &published_assets)) != CASI_OK) {
        goto done;
    }

    for (i = 0; i < local->len; i++) {
        casi_entry *remote = casi_entry_list_find(&published, local->items[i].session_id);
        casi_sync_relation relation;

        if (remote == NULL ||
            (rc = casi_sync_compare(ctx->repo, ctx->roots, &local->items[i], remote,
                                    &relation)) != CASI_OK) {
            if (rc != CASI_OK && remote != NULL)
                goto done;
            out->to_push++;
            out->push_bytes += local->items[i].bytes;
        } else if (relation != CASI_SYNC_SAME) {
            out->to_push++;
            out->push_bytes += local->items[i].bytes;
        }
    }
    for (i = 0; i < local_assets->len; i++) {
        casi_asset *remote = casi_asset_list_find(&published_assets, local_assets->items[i].kind,
                                                    local_assets->items[i].project_id,
                                                    local_assets->items[i].session_id,
                                                    local_assets->items[i].name);
        bool equal;

        if (remote == NULL) {
            out->auxiliary_to_push++;
            out->auxiliary_push_bytes += local_assets->items[i].bytes;
        } else if ((rc = casi_store_asset_equal(ctx->repo, ctx->roots, &local_assets->items[i],
                                                  remote, &equal)) != CASI_OK) {
            goto done;
        } else if (!equal) {
            out->auxiliary_to_push++;
            out->auxiliary_push_bytes += local_assets->items[i].bytes;
        }
    }
    rc = CASI_OK;

done:
    casi_buf_dispose(&refname);
    casi_entry_list_dispose(&published);
    casi_asset_list_dispose(&published_assets);
    return rc;
}

static int status_porcelain(const casi_sync_summary *s,
                            const casi_conflict_list *conflicts)
{
    size_t i;

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

    for (i = 0; i < conflicts->len; i++) {
        const casi_conflict *c = &conflicts->items[i];

        printf("diverge\t%s\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\t%zu\t%zu\t%zu\t%" PRIu64 "\t%" PRIu64 "\n",
               c->local->session_id, c->local->project_path,
               c->remote->origin_machine != NULL ? c->remote->origin_machine : "?",
               c->local->bytes, c->remote->bytes,
               c->local->chunk_count, c->remote->chunk_count, c->common_chunks,
               c->local->updated_at, c->remote->updated_at);
    }

    /* Session divergence requires a user-visible non-zero status in strict
     * workflows. Auxiliary files are resolved automatically by `pull`; their
     * presence is informational and must not make `status` look broken. */
    return s->conflicts > 0 ? CASI_ECONFLICT : CASI_OK;
}

/* One human-readable block per divergent session, so the user can tell the two
 * versions apart without decoding a session id. Parking still preserves both
 * copies; this only supplies the context needed to choose between them. */
static int report_conflicts(const casi_ctx *ctx,
                            const casi_conflict_list *conflicts)
{
    size_t i;

    for (i = 0; i < conflicts->len; i++) {
        const casi_conflict *c = &conflicts->items[i];
        const char *origin = c->remote->origin_machine != NULL
                           ? c->remote->origin_machine : "unknown machine";
        size_t total = c->local->chunk_count > c->remote->chunk_count
                     ? c->local->chunk_count : c->remote->chunk_count;
        casi_buf lsize = CASI_BUF_INIT, rsize = CASI_BUF_INIT;
        casi_buf ltime = CASI_BUF_INIT, rtime = CASI_BUF_INIT;
        int rc;

        if ((rc = casi_ops_human_size(c->local->bytes, &lsize)) != CASI_OK ||
            (rc = casi_ops_human_size(c->remote->bytes, &rsize)) != CASI_OK ||
            (rc = casi_ops_format_time(c->local->updated_at, &ltime)) != CASI_OK ||
            (rc = casi_ops_format_time(c->remote->updated_at, &rtime)) != CASI_OK) {
            casi_buf_dispose(&lsize);
            casi_buf_dispose(&rsize);
            casi_buf_dispose(&ltime);
            casi_buf_dispose(&rtime);
            return rc;
        }

        casi_warn("%s / %.*s diverged", c->local->project_path,
                  (int)8, c->local->session_id);
        casi_info("    local  (%s): %s, %zu chunks, modified %s", ctx->machine,
                  casi_buf_cstr(&lsize), c->local->chunk_count, casi_buf_cstr(&ltime));
        casi_info("    remote (%s): %s, %zu chunks, modified %s", origin,
                  casi_buf_cstr(&rsize), c->remote->chunk_count, casi_buf_cstr(&rtime));
        casi_info("    share %zu of %zu chunks", c->common_chunks, total);

        casi_buf_dispose(&lsize);
        casi_buf_dispose(&rsize);
        casi_buf_dispose(&ltime);
        casi_buf_dispose(&rtime);
    }

    return CASI_OK;
}

int casi_cmd_status(int argc, char **argv)
{
    casi_ctx ctx;
    casi_entry_list local, remote;
    casi_asset_list local_assets, remote_assets;
    casi_sync_summary summary;
    casi_conflict_list conflicts;
    casi_buf url = CASI_BUF_INIT, size = CASI_BUF_INIT;
    bool porcelain = false;
    size_t excluded;
    int i, rc;

    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    memset(&local_assets, 0, sizeof(local_assets));
    memset(&remote_assets, 0, sizeof(remote_assets));
    memset(&conflicts, 0, sizeof(conflicts));

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

    if ((rc = casi_ops_summarise(ctx.repo, ctx.roots, &local, &remote, &local_assets,
                                 &remote_assets, &summary)) != CASI_OK)
        goto done;
    {
        casi_sync_summary publication;

        if ((rc = publication_pending(&ctx, &local, &local_assets, &publication)) != CASI_OK)
            goto done;
        summary.to_push = publication.to_push;
        summary.push_bytes = publication.push_bytes;
        summary.auxiliary_to_push = publication.auxiliary_to_push;
        summary.auxiliary_push_bytes = publication.auxiliary_push_bytes;
    }
    summary.excluded = excluded;

    if (summary.conflicts > 0 &&
        (rc = casi_ops_list_conflicts(ctx.repo, ctx.roots, &local, &remote, &conflicts)) != CASI_OK)
        goto done;

    if (porcelain) {
        rc = status_porcelain(&summary, &conflicts);
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

    if ((rc = casi_ops_human_size(summary.push_bytes, &size)) != CASI_OK)
        goto done;
    casi_info("  to push      %4zu sessions   %s", summary.to_push, casi_buf_cstr(&size));
    if ((rc = casi_ops_human_size(summary.pull_bytes, &size)) != CASI_OK)
        goto done;
    casi_info("  to pull      %4zu sessions   %s", summary.to_pull, casi_buf_cstr(&size));
    casi_info("  up to date   %4zu sessions", summary.up_to_date);
    if (summary.excluded > 0)
        casi_info("  excluded     %4zu sessions", summary.excluded);
    if (summary.auxiliary_to_push > 0) {
        if ((rc = casi_ops_human_size(summary.auxiliary_push_bytes, &size)) != CASI_OK)
            goto done;
        casi_info("  aux to push  %4zu files      %s", summary.auxiliary_to_push,
                  casi_buf_cstr(&size));
    }
    if (summary.auxiliary_to_pull > 0) {
        if ((rc = casi_ops_human_size(summary.auxiliary_pull_bytes, &size)) != CASI_OK)
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
            if ((rc = report_conflicts(&ctx, &conflicts)) != CASI_OK)
                goto done;
            casi_info("  run `casi pull` to park the remote copies for inspection");
        }
        if (summary.auxiliary_conflicts > 0)
            casi_info("  %zu auxiliary file version(s) differ between machines; "
                      "`casi pull` resolves them automatically and archives prior copies",
                      summary.auxiliary_conflicts);
        if (summary.conflicts > 0)
            rc = CASI_ECONFLICT;
    }

done:
    casi_conflict_list_dispose(&conflicts);
    casi_buf_dispose(&url);
    casi_buf_dispose(&size);
    casi_entry_list_dispose(&local);
    casi_entry_list_dispose(&remote);
    casi_asset_list_dispose(&local_assets);
    casi_asset_list_dispose(&remote_assets);
    casi_ctx_dispose(&ctx);
    return rc;
}
