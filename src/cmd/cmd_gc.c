/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include "casi/ctx.h"
#include "casi/repo.h"

int casi_cmd_gc(int argc, char **argv)
{
    casi_ctx ctx;
    int rc;

    (void)argv;

    if (argc != 0)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("gc")->usage);
    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        return rc;

    casi_info("optimising the local casi store");
    rc = casi_repo_gc(ctx.repo);
    if (rc == CASI_OK)
        casi_info("local store optimised");

    casi_ctx_dispose(&ctx);
    return rc;
}
