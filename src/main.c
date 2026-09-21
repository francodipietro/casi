/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include <stdio.h>
#include <string.h>

const casi_command casi_commands[] = {
    { "init",    "create the store and point it at a remote",
      "casi init [--remote <url>] [--machine <name>] [--encrypt [--keyfile <path>]]",
      casi_cmd_init,    false },
    { "push",    "send this machine's sessions",
      "casi push [--dry-run]",
      casi_cmd_push,    false },
    { "pull",    "bring in the other machines' sessions",
      "casi pull [--dry-run] [--theirs <session-id>] [--theirs-all]",
      casi_cmd_pull,    false },
    { "sync",    "pull, then push",
      "casi sync",
      casi_cmd_sync,    false },
    { "status",  "what is out of date, and where",
      "casi status [--porcelain]",
      casi_cmd_status,  false },
    { "exclude", "stop syncing a project",
      "casi exclude <path>",
      casi_cmd_exclude, false },
    { "include", "resume syncing a project",
      "casi include <path>",
      casi_cmd_include, false },
    { "config",  "get and set casi options",
      "casi config <key> [<value>] | --list | --unset <key>",
      casi_cmd_config,  false },
    { "doctor",  "diagnose the store, remote, and root mappings",
      "casi doctor",
      casi_cmd_doctor,  true  },
    { "conflicts", "list copies parked by pull",
      "casi conflicts",
      casi_cmd_conflicts, true },
    { "gc", "repack and prune the local store",
      "casi gc",
      casi_cmd_gc, true },
    { "diagnose", "classify every session local vs remote",
      "casi diagnose",
      casi_cmd_diagnose, true },
    { "migrate", "rewrite foreign home paths to this machine's home",
      "casi migrate <home1> [<home2> ...]",
      casi_cmd_migrate, true },
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

    /*
     * CASI_ECONFLICT and CASI_EUNMAPPED are documented exit statuses (3 and
     * 5), not unhandled failures: the command that returns either one has
     * already told the user everything via casi_warn()/casi_info() before
     * returning. Printing casi_error_last() on top would either duplicate
     * that report or, once something else has called casi_error_clear() in
     * the meantime, print the meaningless "unknown error" placeholder over a
     * perfectly good report -- both observed while testing pull's conflict
     * path. Any future command that returns one of these two codes must
     * follow the same rule: report it in full before returning, because
     * nothing here will.
     */
    if (rc != CASI_OK && rc != CASI_ECONFLICT && rc != CASI_EUNMAPPED)
        casi_err("%s", casi_error_last());

    casi_shutdown();
    return casi_exit_code(rc);
}
