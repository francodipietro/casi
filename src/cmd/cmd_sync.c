/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

int casi_cmd_sync(int argc, char **argv)
{
    int rc;

    (void)argv;

    if (argc != 0)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("sync")->usage);

    /*
     * Pull first, then push. Bringing the other machines' work in before
     * publishing this machine's means a session continued elsewhere is
     * already accounted for, so the push that follows cannot look like a
     * divergence it caused itself.
     *
     * A conflict does not abort the push: the local transcripts are still
     * worth publishing, and the parked copies are still there to inspect.
     */
    rc = casi_cmd_pull(0, NULL);
    if (rc != CASI_OK && rc != CASI_ECONFLICT)
        return rc;

    if (rc == CASI_ECONFLICT)
        casi_error_clear();

    {
        int push_rc = casi_cmd_push(0, NULL);

        if (push_rc != CASI_OK)
            return push_rc;
    }

    return rc;  /* keeps exit 3 when something diverged */
}
