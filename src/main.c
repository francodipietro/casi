/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include <stdio.h>
#include <string.h>

const casi_command casi_commands[] = {
    { "config",  "get and set casi options",
      "casi config <key> [<value>] | --list | --unset <key>",
      casi_cmd_config,  false },
    { "help",    "show help for a command",
      "casi help [<command>]",
      casi_cmd_help,    true  },
    { "version", "print the casi version",
      "casi version",
      casi_cmd_version, true  },
    { NULL, NULL, NULL, NULL, false }
};

const casi_command *casi_command_lookup(const char *name)
{
    const casi_command *cmd;

    for (cmd = casi_commands; cmd->name != NULL; cmd++)
        if (strcmp(cmd->name, name) == 0)
            return cmd;

    return NULL;
}

/*
 * Global options are consumed before the verb, git-style: `casi -v push`, not
 * `casi push -v`. Anything after the verb belongs to the verb.
 */
static int parse_global_opts(int *argc, char ***argv)
{
    int verbosity = 0;

    while (*argc > 0) {
        const char *arg = (*argv)[0];

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbosity++;
        } else if (strcmp(arg, "-vv") == 0) {
            verbosity += 2;
        } else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            casi_log_set_level(CASI_LOG_QUIET);
        } else if (strcmp(arg, "--no-color") == 0 || strcmp(arg, "--no-colour") == 0) {
            casi_log_set_color(false);
        } else {
            break;
        }

        (*argc)--;
        (*argv)++;
    }

    if (verbosity > 0 && casi_log_get_level() != CASI_LOG_QUIET)
        casi_log_set_level(verbosity >= 2 ? CASI_LOG_DEBUG : CASI_LOG_VERBOSE);

    return CASI_OK;
}

int main(int argc, char **argv)
{
    const casi_command *cmd;
    int rc;

    argc--;
    argv++;

    if ((rc = casi_init()) != CASI_OK) {
        fprintf(stderr, "error: %s\n", casi_error_last());
        return CASI_EXIT_ERROR;
    }

    parse_global_opts(&argc, &argv);

    if (argc == 0) {
        rc = casi_cmd_help(0, NULL);
        casi_shutdown();
        return casi_exit_code(rc);
    }

    /* --version / --help are spelled both ways, as users expect. */
    if (strcmp(argv[0], "--version") == 0) {
        rc = casi_cmd_version(0, NULL);
        casi_shutdown();
        return casi_exit_code(rc);
    }
    if (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
        rc = casi_cmd_help(argc - 1, argv + 1);
        casi_shutdown();
        return casi_exit_code(rc);
    }

    if ((cmd = casi_command_lookup(argv[0])) == NULL) {
        casi_err("unknown command '%s'", argv[0]);
        casi_err("run 'casi --help' for the command list");
        casi_shutdown();
        return CASI_EXIT_USAGE;
    }

    rc = cmd->run(argc - 1, argv + 1);
    if (rc != CASI_OK)
        casi_err("%s", casi_error_last());

    casi_shutdown();
    return casi_exit_code(rc);
}
