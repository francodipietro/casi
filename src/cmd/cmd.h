/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_CMD_H
#define CASI_CMD_H

#include "casi/casi.h"

/*
 * One entry per user-facing verb. `hidden` keeps plumbing (doctor, gc,
 * conflicts) out of the main help without hiding it from the shell: the UX
 * contract is that `casi --help` shows five commands and nothing more.
 */
typedef struct {
    const char *name;
    const char *summary;
    const char *usage;
    int       (*run)(int argc, char **argv);
    bool        hidden;
} casi_command;

extern const casi_command casi_commands[];

const casi_command *casi_command_lookup(const char *name);

int casi_cmd_init(int argc, char **argv);
int casi_cmd_status(int argc, char **argv);
int casi_cmd_push(int argc, char **argv);
int casi_cmd_pull(int argc, char **argv);
int casi_cmd_sync(int argc, char **argv);
int casi_cmd_exclude(int argc, char **argv);
int casi_cmd_include(int argc, char **argv);
int casi_cmd_version(int argc, char **argv);
int casi_cmd_help(int argc, char **argv);
int casi_cmd_config(int argc, char **argv);
int casi_cmd_doctor(int argc, char **argv);
int casi_cmd_conflicts(int argc, char **argv);
int casi_cmd_gc(int argc, char **argv);
int casi_cmd_diagnose(int argc, char **argv);

#endif /* CASI_CMD_H */
