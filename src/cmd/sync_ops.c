/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/sync_ops.h"
#include "casi/casi.h"

#include <string.h>

int casi_ops_scan_local(casi_ctx *ctx, casi_entry_list *out,
                         casi_asset_list *assets_out, size_t *excluded_out)
{
    casi_strvec exclude = CASI_STRVEC_INIT;
    int rc;

    if ((rc = casi_ctx_exclude_list(ctx, &exclude)) == CASI_OK)
        rc = casi_store_scan_local(ctx->repo, ctx->roots, ctx->provider,
                                   &exclude, out, assets_out, excluded_out);

    casi_strvec_dispose(&exclude);
    return rc;
}

static int merge_assets(casi_asset_list *into, casi_asset_list *from)
{
    size_t i;
    int rc;

    for (i = 0; i < from->len; i++) {
        casi_asset *incoming = &from->items[i];
        casi_asset *have = casi_asset_list_find(into, incoming->kind,
                                                incoming->project_id,
                                                incoming->session_id,
                                                incoming->name);

        if (have == NULL) {
            casi_asset moved = *incoming;

            memset(incoming, 0, sizeof(*incoming));
            if ((rc = casi_asset_list_push_owned(into, &moved)) != CASI_OK)
                return rc;
            continue;
        }

        /* Auxiliary files have no safe append rule. Never choose one of two
         * different remote versions arbitrarily: pull will keep the local
         * file and report the conflict explicitly instead. */
        if (!git_oid_equal(&have->oid, &incoming->oid)) {
            if (incoming->session_id != NULL) {
                casi_warn("auxiliary file has conflicting remote versions: project %s, "
                          "session %s, subagent %s", incoming->project_path,
                          incoming->session_id, incoming->name);
                return casi_error_set(CASI_ECONFLICT,
                                      "subagent %s in project %s session %s has conflicting remote versions",
                                      incoming->name, incoming->project_path,
                                      incoming->session_id);
            }
            casi_warn("auxiliary file has conflicting remote versions: project %s, "
                      "project memory %s", incoming->project_path, incoming->name);
            return casi_error_set(CASI_ECONFLICT,
                                  "project memory %s in project %s has conflicting remote versions",
                                  incoming->name, incoming->project_path);
        }
    }

    return CASI_OK;
}

/* Folds one machine's entries into the accumulating union. */
static int merge_branch(casi_repo *repo, casi_entry_list *into, casi_entry_list *from)
{
    size_t i;
    int rc;

    for (i = 0; i < from->len; i++) {
        casi_entry *incoming = &from->items[i];
        casi_entry *have = casi_entry_list_find(into, incoming->session_id);

        if (have == NULL) {
            /* Move it across wholesale; `from` gives up ownership. */
            casi_entry moved = *incoming;

            memset(incoming, 0, sizeof(*incoming));
            if ((rc = casi_entry_list_push_owned(into, &moved)) != CASI_OK)
                return rc;
            continue;
        }

        /* Two machines both have it. Keep whichever is further along; if they
         * genuinely diverged, keep what we have and let the comparison against
         * local surface it as a conflict. */
        casi_sync_relation relation;

        if ((rc = casi_sync_compare(repo, incoming, have, &relation)) != CASI_OK)
            return rc;
        if (relation == CASI_SYNC_LOCAL_AHEAD) {
            casi_entry moved = *incoming;

            memset(incoming, 0, sizeof(*incoming));
            casi_entry_swap(have, &moved);
            casi_entry_dispose_one(&moved);
        }
    }

    return CASI_OK;
}

int casi_ops_load_remote(casi_ctx *ctx, casi_entry_list *out,
                          casi_asset_list *assets_out)
{
    casi_strvec machines = CASI_STRVEC_INIT;
    casi_buf refname = CASI_BUF_INIT;
    size_t i;
    int rc;

    if ((rc = casi_repo_list_machines(ctx->repo, &machines)) != CASI_OK)
        goto done;

    for (i = 0; i < machines.len; i++) {
        casi_entry_list branch;
        casi_asset_list branch_assets;
        git_oid tree;

        /* Our own branch is what we are comparing against, not part of the
         * other side. */
        if (strcmp(machines.items[i], ctx->machine) == 0)
            continue;

        casi_buf_clear(&refname);
        if ((rc = casi_buf_printf(&refname, "refs/remotes/origin/casi/%s",
                                  machines.items[i])) != CASI_OK)
            goto done;

        if (casi_repo_ref_tree(ctx->repo, casi_buf_cstr(&refname), &tree) != CASI_OK) {
            casi_error_clear();
            continue;
        }

        memset(&branch, 0, sizeof(branch));
        memset(&branch_assets, 0, sizeof(branch_assets));
        rc = casi_store_read_tree(ctx->repo, &tree, ctx->provider->name, &branch,
                                  &branch_assets);
        if (rc == CASI_OK)
            rc = merge_branch(ctx->repo, out, &branch);
        if (rc == CASI_OK)
            rc = merge_assets(assets_out, &branch_assets);
        casi_entry_list_dispose(&branch);
        casi_asset_list_dispose(&branch_assets);

        if (rc != CASI_OK)
            goto done;
    }

    rc = CASI_OK;

done:
    casi_strvec_dispose(&machines);
    casi_buf_dispose(&refname);
    return rc;
}

int casi_ops_summarise(casi_repo *repo, casi_entry_list *local,
                       casi_entry_list *remote, casi_asset_list *local_assets,
                       casi_asset_list *remote_assets, casi_sync_summary *out)
{
    size_t i;
    int rc;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < local->len; i++) {
        casi_entry *l = &local->items[i];
        casi_entry *r = casi_entry_list_find(remote, l->session_id);

        if (r == NULL) {
            out->to_push++;
            out->push_bytes += l->bytes;
            continue;
        }

        casi_sync_relation relation;

        if ((rc = casi_sync_compare(repo, l, r, &relation)) != CASI_OK)
            return rc;

        switch (relation) {
        case CASI_SYNC_SAME:
            out->up_to_date++;
            break;
        case CASI_SYNC_LOCAL_AHEAD:
            out->to_push++;
            out->push_bytes += l->bytes;
            break;
        case CASI_SYNC_REMOTE_AHEAD:
            out->to_pull++;
            out->pull_bytes += r->bytes;
            break;
        case CASI_SYNC_DIVERGED:
            out->conflicts++;
            break;
        }
    }

    /* Anything the remote has that this machine has never seen. */
    for (i = 0; i < remote->len; i++)
        if (casi_entry_list_find(local, remote->items[i].session_id) == NULL) {
            out->to_pull++;
            out->pull_bytes += remote->items[i].bytes;
        }

    for (i = 0; i < local_assets->len; i++) {
        casi_asset *local_asset = &local_assets->items[i];
        casi_asset *remote_asset = casi_asset_list_find(remote_assets, local_asset->kind,
                                                         local_asset->project_id,
                                                         local_asset->session_id,
                                                         local_asset->name);

        if (remote_asset == NULL) {
            out->auxiliary_to_push++;
            out->auxiliary_push_bytes += local_asset->bytes;
        } else if (!git_oid_equal(&local_asset->oid, &remote_asset->oid)) {
            out->auxiliary_conflicts++;
        }
    }
    for (i = 0; i < remote_assets->len; i++) {
        casi_asset *remote_asset = &remote_assets->items[i];

        if (casi_asset_list_find(local_assets, remote_asset->kind,
                                 remote_asset->project_id, remote_asset->session_id,
                                 remote_asset->name) == NULL) {
            out->auxiliary_to_pull++;
            out->auxiliary_pull_bytes += remote_asset->bytes;
        }
    }

    return CASI_OK;
}
