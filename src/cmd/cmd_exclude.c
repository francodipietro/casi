/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"
#include "casi/shared_config.h"

#include <string.h>

/*
 * Excluding works on the canonical project path, not the literal argument, so
 * that "~/src/bookit" and "/Users/me/src/bookit" and a path given from inside
 * the project all resolve to the same entry -- and so the entry still means
 * something on a machine that keeps the project somewhere else.
 */
static int canonical_of(casi_ctx *ctx, const char *arg, casi_buf *out)
{
    casi_buf absolute = CASI_BUF_INIT;
    int rc;

    if ((rc = casi_fs_realpath(arg, &absolute)) != CASI_OK)
        goto done;

    rc = casi_roots_normalize_path(ctx->roots, casi_buf_cstr(&absolute), out);

done:
    casi_buf_dispose(&absolute);
    return rc;
}

static int run(int argc, char **argv, bool excluding)
{
    casi_ctx ctx;
    casi_buf canonical = CASI_BUF_INIT;
    int rc;

    if (argc != 1)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup(excluding ? "exclude" : "include")->usage);

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        return rc;

    if ((rc = canonical_of(&ctx, argv[0], &canonical)) != CASI_OK)
        goto done;

    {
        unsigned int attempt;

        rc = CASI_ERETRY;
        for (attempt = 0; attempt < 3; attempt++) {
            casi_shared_config shared = { 0 };
            bool present;

            if ((rc = casi_repo_fetch(ctx.repo)) != CASI_OK)
                goto update_done;
            if ((rc = casi_shared_config_load_remote(ctx.repo, &ctx.crypto,
                                                      &shared, &present)) != CASI_OK)
                goto update_done;

            /* Upgrade local-only exclusions as one atomic first shared write.
             * Once the ref exists, remote state is authoritative. */
            if (!present) {
                casi_strvec legacy = CASI_STRVEC_INIT;
                size_t i;

                rc = casi_config_get_multivar(ctx.cfg, "sync.exclude", &legacy);
                for (i = 0; rc == CASI_OK && i < legacy.len; i++)
                    rc = casi_shared_config_add_exclude(&shared, legacy.items[i]);
                casi_strvec_dispose(&legacy);
                if (rc != CASI_OK)
                    goto update_done;
            }

            rc = excluding
                 ? casi_shared_config_add_exclude(&shared, casi_buf_cstr(&canonical))
                 : casi_shared_config_remove_exclude(&shared, casi_buf_cstr(&canonical));
            if (rc == CASI_ENOTFOUND && !excluding) {
                casi_error_clear();
                rc = CASI_OK;
            }
            if (rc == CASI_OK)
                rc = casi_shared_config_commit_push(ctx.repo, &shared, &ctx.crypto,
                                                     ctx.machine);

update_done:
            casi_shared_config_dispose(&shared);
            if (rc != CASI_ERETRY)
                break;
            casi_error_clear();
        }
        if (rc == CASI_ERETRY)
            rc = casi_error_set(CASI_ERROR,
                                "shared configuration changed concurrently; try again");
    }

    if (rc == CASI_OK && excluding)
        casi_info("%s is excluded -- its sessions will not be pushed on any machine",
                  casi_buf_cstr(&canonical));
    else if (rc == CASI_OK)
        casi_info("%s is included -- its sessions may be pushed again on every machine",
                  casi_buf_cstr(&canonical));

done:
    casi_buf_dispose(&canonical);
    casi_ctx_dispose(&ctx);
    return rc;
}

int casi_cmd_exclude(int argc, char **argv)
{
    return run(argc, argv, true);
}

int casi_cmd_include(int argc, char **argv)
{
    return run(argc, argv, false);
}
