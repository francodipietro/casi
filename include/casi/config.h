/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_CONFIG_H
#define CASI_CONFIG_H

#include <stdbool.h>

#include "casi/buf.h"
#include "casi/str.h"

/*
 * casi's own config file is git's INI format, parsed by libgit2's own config
 * machinery. That is not laziness: it means `casi config remote.origin.url
 * <url>` behaves exactly like its git counterpart -- same key grammar, same
 * subsection quoting, same include handling -- with no parser of our own to
 * get wrong.
 *
 *   [core]
 *       machine = mac-air
 *   [remote "origin"]
 *       url = git@github.com:me/my-sessions.git
 *   [root "bookit"]
 *       path = /Users/fdipietro/src/bookit
 */

typedef struct casi_config casi_config;

/* Opens (creating if needed) the config file for this machine. */
int  casi_config_open(casi_config **out);
/* Same, at an explicit path. Used by the tests. */
int  casi_config_open_path(casi_config **out, const char *path);
void casi_config_free(casi_config *cfg);

/* CASI_ENOTFOUND when the key is absent; `out` is left untouched. */
int  casi_config_get_string(casi_config *cfg, const char *key, casi_buf *out);
/* Returns `fallback` rather than failing when the key is absent. */
int  casi_config_get_bool(casi_config *cfg, const char *key, bool fallback, bool *out);

int  casi_config_set_string(casi_config *cfg, const char *key, const char *value);
int  casi_config_unset(casi_config *cfg, const char *key);

/*
 * A key that legitimately holds several values, git's own multivar. The
 * exclude list uses it: one `sync.exclude = <path>` line per excluded
 * project, exactly as .gitconfig would spell it.
 */
int  casi_config_get_multivar(casi_config *cfg, const char *key, casi_strvec *out);
int  casi_config_add_multivar(casi_config *cfg, const char *key, const char *value);
/* Removes every entry equal to `value`. CASI_ENOTFOUND when there was none. */
int  casi_config_remove_multivar(casi_config *cfg, const char *key, const char *value);

/* Iterates every key in file order. A non-zero return from `cb` stops the
 * walk and is passed back to the caller. */
typedef int (*casi_config_cb)(const char *key, const char *value, void *payload);
int  casi_config_foreach(casi_config *cfg, casi_config_cb cb, void *payload);

#endif /* CASI_CONFIG_H */
