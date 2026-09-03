/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include <stdio.h>
#include <string.h>

static int print_entry(const char *key, const char *value, void *payload)
{
    (void)payload;
    printf("%s=%s\n", key, value);
    return CASI_OK;
}

static int config_list(casi_config *cfg)
{
    return casi_config_foreach(cfg, print_entry, NULL);
}

static int config_get(casi_config *cfg, const char *key)
{
    casi_buf value = CASI_BUF_INIT;
    int rc;

    if ((rc = casi_config_get_string(cfg, key, &value)) == CASI_OK)
        printf("%s\n", casi_buf_cstr(&value));

    casi_buf_dispose(&value);
    return rc;
}

int casi_cmd_config(int argc, char **argv)
{
    casi_config *cfg = NULL;
    int rc;

    /* Report through the error channel, never directly: main() prints
     * casi_error_last() for any non-zero return, so printing here too would
     * emit the message twice. */
    if (argc < 1)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("config")->usage);

    if ((rc = casi_config_open(&cfg)) != CASI_OK)
        return rc;

    if (strcmp(argv[0], "--list") == 0 || strcmp(argv[0], "-l") == 0) {
        rc = (argc == 1) ? config_list(cfg)
                         : casi_error_set(CASI_EINVAL, "--list takes no arguments");
    } else if (strcmp(argv[0], "--unset") == 0) {
        rc = (argc == 2) ? casi_config_unset(cfg, argv[1])
                         : casi_error_set(CASI_EINVAL, "--unset takes exactly one key");
    } else if (argc == 1) {
        rc = config_get(cfg, argv[0]);
    } else if (argc == 2) {
        rc = casi_config_set_string(cfg, argv[0], argv[1]);
    } else {
        rc = casi_error_set(CASI_EINVAL, "too many arguments");
    }

    casi_config_free(cfg);
    return rc;
}
