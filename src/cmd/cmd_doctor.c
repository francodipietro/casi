/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"
#include "casi/shared_config.h"

#include <string.h>

/* Discover only lists directories, it does not chunk anything, so it is cheap
 * enough for a diagnostic. Projects are matched by basename automatically, so
 * there is nothing for the user to declare; report what was found instead. */
static int session_layout(const casi_ctx *ctx, size_t *sessions_out,
                          size_t *projects_out)
{
    casi_session_list sessions;
    casi_aux_file_list aux;
    casi_strvec projects = CASI_STRVEC_INIT;
    size_t i;
    int rc;

    memset(&sessions, 0, sizeof(sessions));
    memset(&aux, 0, sizeof(aux));
    *sessions_out = 0;
    *projects_out = 0;

    if ((rc = ctx->provider->discover(ctx->roots, &sessions, &aux)) != CASI_OK)
        goto done;
    for (i = 0; i < sessions.len; i++) {
        size_t j;
        bool seen = false;

        (*sessions_out)++;
        for (j = 0; j < projects.len; j++)
            if (strcmp(projects.items[j], sessions.items[i].project_path) == 0) {
                seen = true;
                break;
            }
        if (!seen &&
            (rc = casi_strvec_push(&projects, sessions.items[i].project_path)) != CASI_OK)
            goto done;
    }
    *projects_out = projects.len;
    rc = CASI_OK;

done:
    casi_strvec_dispose(&projects);
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
    bool present;
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
        size_t total = 0, projects = 0;

        if ((rc = session_layout(&ctx, &total, &projects)) != CASI_OK)
            goto done;
        if (total == 0)
            casi_info("sessions: none found yet");
        else
            casi_info("sessions: %zu found across %zu project(s), "
                      "matched by name automatically", total, projects);
        casi_info("state: no shared configuration yet -- expected before "
                  "your first `casi push`");
        casi_info("next: `casi push`");
        rc = CASI_OK;
        goto done;
    }

    if (shared.exclude.len > 0)
        casi_info("excluded projects: %zu", shared.exclude.len);
    casi_info("state: ready -- next `casi push`");
    rc = CASI_OK;

done:
    casi_shared_config_dispose(&shared);
    casi_strvec_dispose(&machines);
    casi_buf_dispose(&backend);
    casi_buf_dispose(&remote);
    casi_ctx_dispose(&ctx);
    return rc;
}
