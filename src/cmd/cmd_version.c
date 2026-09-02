/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include <stdio.h>

int casi_cmd_version(int argc, char **argv)
{
    casi_buf backend = CASI_BUF_INIT;
    int rc;

    (void)argc;
    (void)argv;

    printf("casi %s\n", CASI_VERSION_STRING);

    if ((rc = casi_backend_describe(&backend)) != CASI_OK) {
        casi_buf_dispose(&backend);
        return rc;
    }

    printf("%s\n", casi_buf_cstr(&backend));
    casi_buf_dispose(&backend);
    return CASI_OK;
}
