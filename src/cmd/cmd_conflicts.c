/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include "casi/fs.h"
#include "casi/paths.h"

int casi_cmd_conflicts(int argc, char **argv)
{
    casi_strvec names = CASI_STRVEC_INIT;
    const char *dir;
    size_t i;
    int rc;

    (void)argv;

    if (argc != 0)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("conflicts")->usage);
    if ((dir = casi_paths_conflicts_dir()) == NULL)
        return casi_error_last_code();

    rc = casi_fs_listdir(dir, &names);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
    }
    if (rc != CASI_OK)
        goto done;

    if (names.len == 0) {
        casi_info("no parked conflicts");
        goto done;
    }

    casi_info("parked conflicts:");
    for (i = 0; i < names.len; i++)
        casi_info("  %s/%s", dir, names.items[i]);

done:
    casi_strvec_dispose(&names);
    return rc;
}
