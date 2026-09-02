/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_PATHS_H
#define CASI_PATHS_H

/*
 * Where casi keeps its own state, and where it expects to find the assistant
 * it syncs.
 *
 * Layout (XDG, with the usual ~ fallbacks):
 *   config      $XDG_CONFIG_HOME/casi/config   ~/.config/casi/config
 *   data dir    $XDG_DATA_HOME/casi            ~/.local/share/casi
 *     repo.git    the internal bare repository
 *     index       the stat-cache
 *     conflicts/  remote copies parked by a diverged pull
 *
 * Two environment overrides exist so the integration tests can run several
 * fully isolated "machines" inside one process tree:
 *   CASI_HOME         puts config *and* data under one directory
 *   CASI_CLAUDE_HOME  replaces ~/.claude
 *
 * The returned pointers are owned by this module and stay valid until
 * casi_paths_reset(). They are NULL only if the home directory cannot be
 * resolved at all.
 */

const char *casi_paths_config_file(void);
const char *casi_paths_data_dir(void);
const char *casi_paths_repo(void);
const char *casi_paths_index(void);
const char *casi_paths_conflicts_dir(void);
const char *casi_paths_claude_home(void);

/* Frees the cached strings and forces the next call to re-read the
 * environment. Called at exit, and by tests between cases. */
void casi_paths_reset(void);

#endif /* CASI_PATHS_H */
