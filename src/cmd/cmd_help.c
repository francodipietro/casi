/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include <stdio.h>
#include <string.h>

static void print_usage(void)
{
    const casi_command *cmd;

    puts("casi -- sync coding assistant sessions between machines\n");
    puts("usage: casi <command> [<args>]\n");
    puts("commands:");

    for (cmd = casi_commands; cmd->name != NULL; cmd++) {
        if (cmd->hidden)
            continue;
        printf("   %-9s %s\n", cmd->name, cmd->summary);
    }

    puts("\noptions:");
    puts("   -v, --verbose   more detail (-vv for debug)");
    puts("   -q, --quiet     errors only");
    puts("       --no-color  never colourise output");
    puts("       --version   print version and exit");
    puts("   -h, --help      print this help and exit");
}

int casi_cmd_help(int argc, char **argv)
{
    const casi_command *cmd;

    if (argc < 1) {
        print_usage();
        return CASI_OK;
    }

    if ((cmd = casi_command_lookup(argv[0])) == NULL) {
        casi_err("unknown command '%s'", argv[0]);
        return CASI_EINVAL;
    }

    printf("usage: %s\n\n%s\n", cmd->usage, cmd->summary);
    return CASI_OK;
}
