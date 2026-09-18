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

int casi_cmd_doctor(int argc, char **argv)
{
    casi_ctx ctx;
    casi_shared_config shared = { 0 };
    casi_buf backend = CASI_BUF_INIT, remote = CASI_BUF_INIT;
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
    casi_info("remote: reachable");

    if ((rc = casi_shared_config_load_remote(ctx.repo, &ctx.crypto, &shared, &present)) != CASI_OK)
        goto done;
    if (!present) {
        casi_info("roots: no shared configuration yet");
        rc = CASI_OK;
        goto done;
    }

    if (shared.roots.len == 0)
        casi_info("roots: no named roots declared");

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
        rc = CASI_EUNMAPPED;
    } else {
        casi_info("roots: all shared roots are mapped");
        rc = CASI_OK;
    }

done:
    casi_shared_config_dispose(&shared);
    casi_buf_dispose(&backend);
    casi_buf_dispose(&remote);
    casi_ctx_dispose(&ctx);
    return rc;
}
