/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"

#include <stdlib.h>
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
    char *resolved;
    int rc;

    resolved = realpath(arg, NULL);
    if (resolved == NULL)
        return casi_error_set(CASI_ENOTFOUND, "no such directory: %s", arg);

    rc = casi_buf_puts(&absolute, resolved);
    free(resolved);
    if (rc != CASI_OK)
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

    if (excluding) {
        rc = casi_config_add_multivar(ctx.cfg, "sync.exclude",
                                      casi_buf_cstr(&canonical));
        if (rc == CASI_OK)
            casi_info("excluded %s -- its sessions will not be pushed",
                      casi_buf_cstr(&canonical));
    } else {
        rc = casi_config_remove_multivar(ctx.cfg, "sync.exclude",
                                         casi_buf_cstr(&canonical));
        if (rc == CASI_ENOTFOUND) {
            casi_error_clear();
            casi_info("%s was not excluded; nothing to do",
                      casi_buf_cstr(&canonical));
            rc = CASI_OK;
        } else if (rc == CASI_OK) {
            casi_info("included %s -- its sessions will be pushed again",
                      casi_buf_cstr(&canonical));
        }
    }

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
