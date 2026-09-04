/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"

#include <string.h>

/* A default machine label, so `casi init` needs no arguments to be useful.
 * The hostname is what the user already calls this machine. */
static int default_machine(casi_buf *out)
{
    return casi_fs_hostname(out);
}

int casi_cmd_init(int argc, char **argv)
{
    casi_config *cfg = NULL;
    casi_repo *repo = NULL;
    casi_buf machine = CASI_BUF_INIT, existing = CASI_BUF_INIT;
    const char *remote = NULL, *machine_arg = NULL;
    int i, rc;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--remote") == 0 && i + 1 < argc) {
            remote = argv[++i];
        } else if (strcmp(argv[i], "--machine") == 0 && i + 1 < argc) {
            machine_arg = argv[++i];
        } else {
            rc = casi_error_set(CASI_EINVAL, "usage: %s",
                                casi_command_lookup("init")->usage);
            goto done;
        }
    }

    if ((rc = casi_config_open(&cfg)) != CASI_OK)
        goto done;

    /* Re-running init must not silently rename the machine and orphan its
     * branch, so an existing label is kept unless --machine says otherwise. */
    if (machine_arg != NULL) {
        if ((rc = casi_buf_puts(&machine, machine_arg)) != CASI_OK)
            goto done;
    } else if (casi_config_get_string(cfg, "core.machine", &existing) == CASI_OK) {
        if ((rc = casi_buf_put(&machine, existing.ptr, existing.len)) != CASI_OK)
            goto done;
    } else {
        casi_error_clear();
        if ((rc = default_machine(&machine)) != CASI_OK)
            goto done;
    }

    if ((rc = casi_config_set_string(cfg, "core.machine",
                                     casi_buf_cstr(&machine))) != CASI_OK)
        goto done;
    if ((rc = casi_config_set_string(cfg, "core.provider",
                                     casi_provider_default()->name)) != CASI_OK)
        goto done;

    if ((rc = casi_repo_init(&repo)) != CASI_OK)
        goto done;

    if (remote != NULL && (rc = casi_repo_set_remote(repo, remote)) != CASI_OK)
        goto done;

    casi_info("casi store ready for machine \"%s\"", casi_buf_cstr(&machine));
    if (remote != NULL)
        casi_info("  remote: %s", remote);
    else
        casi_info("  no remote yet -- re-run with `casi init --remote <url>`");

    rc = CASI_OK;

done:
    casi_repo_free(repo);
    casi_config_free(cfg);
    casi_buf_dispose(&machine);
    casi_buf_dispose(&existing);
    return rc;
}
