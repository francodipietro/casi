/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"
#include "casi/shared_config.h"

#include <string.h>

static const char *local_root_path(const casi_ctx *ctx, const char *name)
{
    size_t i;

    for (i = 0; i < casi_roots_count(ctx->roots); i++)
        if (strcmp(casi_roots_name_at(ctx->roots, i), name) == 0)
            return casi_roots_path_at(ctx->roots, i);

    return NULL;
}

/* The implicit "~" root means a session under the home directory needs no
 * declared root on either machine. This is the common simple case a first
 * push should not make the user think about. */
static bool is_under_home_root(const char *project_path)
{
    return strncmp(project_path, "casi://~/", 9) == 0 ||
           strcmp(project_path, "casi://~") == 0;
}

/* Discover only lists directories, it does not chunk anything, so it is cheap
 * enough for a diagnostic. It answers "do I actually need to declare roots?" */
static int session_layout(const casi_ctx *ctx, size_t *total_out,
                          size_t *outside_home_out)
{
    casi_session_list sessions;
    casi_aux_file_list aux;
    size_t i;
    int rc;

    memset(&sessions, 0, sizeof(sessions));
    memset(&aux, 0, sizeof(aux));
    *total_out = 0;
    *outside_home_out = 0;

    if ((rc = ctx->provider->discover(ctx->roots, &sessions, &aux)) != CASI_OK)
        goto done;
    for (i = 0; i < sessions.len; i++) {
        (*total_out)++;
        if (!is_under_home_root(sessions.items[i].project_path))
            (*outside_home_out)++;
    }
    rc = CASI_OK;

done:
    casi_session_list_dispose(&sessions);
    casi_aux_file_list_dispose(&aux);
    return rc;
}

int casi_cmd_doctor(int argc, char **argv)
{
    casi_ctx ctx;
    casi_shared_config shared = { 0 };
    casi_buf backend = CASI_BUF_INIT, remote = CASI_BUF_INIT;
    casi_strvec machines = CASI_STRVEC_INIT;
    bool present, unmapped = false;
    size_t i;
    int rc;

    (void)argv;

    if (argc != 0)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("doctor")->usage);

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        goto done;
    if ((rc = casi_backend_describe(&backend)) != CASI_OK)
        goto done;
    if ((rc = casi_repo_remote_url(ctx.repo, &remote)) != CASI_OK)
        goto done;

    casi_info("backend: %s", casi_buf_cstr(&backend));
    casi_info("remote: %s", casi_buf_cstr(&remote));

    if ((rc = casi_repo_fetch(ctx.repo)) != CASI_OK)
        goto done;
    if ((rc = casi_repo_list_machines(ctx.repo, &machines)) != CASI_OK)
        goto done;
    if (machines.len == 0)
        casi_info("remote: reachable (empty -- no machine has pushed yet)");
    else
        casi_info("remote: reachable (%zu machine branch(es))", machines.len);

    if ((rc = casi_shared_config_load_remote(ctx.repo, &ctx.crypto, &shared,
                                             &present)) != CASI_OK)
        goto done;
    if (!present) {
        size_t total = 0, outside = 0;

        if ((rc = session_layout(&ctx, &total, &outside)) != CASI_OK)
            goto done;
        if (total == 0)
            casi_info("sessions: none found yet");
        else if (outside == 0)
            casi_info("sessions: %zu found, all under your home directory "
                      "(no roots to declare)", total);
        else
            casi_info("sessions: %zu found, %zu outside your home "
                      "(these will need a root on the other machine)",
                      total, outside);
        casi_info("state: no shared configuration yet -- expected before "
                  "your first `casi push`");
        casi_info("next: `casi push`");
        rc = CASI_OK;
        goto done;
    }

    if (shared.roots.len == 0)
        casi_info("shared roots: none declared");

    for (i = 0; i < shared.roots.len; i++) {
        const char *name = shared.roots.items[i];
        const char *path = local_root_path(&ctx, name);
        casi_stat st;

        if (path == NULL) {
            casi_warn("root \"%s\" is not mapped locally", name);
            casi_info("  declare it with: casi config root.%s.path <local path>", name);
            unmapped = true;
            continue;
        }

        if ((rc = casi_fs_stat(path, &st)) != CASI_OK || !st.is_dir) {
            if (rc == CASI_ENOTFOUND)
                casi_error_clear();
            else if (rc != CASI_OK)
                goto done;
            casi_warn("root \"%s\" maps to a missing directory: %s", name, path);
            unmapped = true;
            continue;
        }

        casi_info("root \"%s\": %s", name, path);
    }

    if (unmapped) {
        casi_warn("some shared roots cannot be materialised on this machine");
        casi_info("state: roots still need a local mapping");
        rc = CASI_EUNMAPPED;
    } else {
        casi_info("state: ready -- next `casi push`");
        rc = CASI_OK;
    }

done:
    casi_shared_config_dispose(&shared);
    casi_strvec_dispose(&machines);
    casi_buf_dispose(&backend);
    casi_buf_dispose(&remote);
    casi_ctx_dispose(&ctx);
    return rc;
}
