/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/paths.h"
#include "casi/buf.h"
#include "casi/error.h"
#include "casi/fs.h"

#include <stdlib.h>
#include <string.h>

static casi_buf g_config_file    = CASI_BUF_INIT;
static casi_buf g_data_dir       = CASI_BUF_INIT;
static casi_buf g_repo           = CASI_BUF_INIT;
static casi_buf g_index          = CASI_BUF_INIT;
static casi_buf g_conflicts_dir  = CASI_BUF_INIT;
static casi_buf g_claude_home    = CASI_BUF_INIT;

static const char *env_or_null(const char *name)
{
    const char *v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : NULL;
}

/* Resolves <xdg_var>/<suffix>, falling back to <home>/<fallback>/<suffix>. */
static const char *resolve_xdg(casi_buf *cache, const char *xdg_var,
                               const char *fallback, const char *suffix)
{
    const char *base, *home;

    if (cache->len > 0)
        return casi_buf_cstr(cache);

    if ((base = env_or_null(xdg_var)) != NULL) {
        if (casi_buf_puts(cache, base) != CASI_OK)
            return NULL;
    } else {
        if ((home = casi_fs_home()) == NULL) {
            casi_error_set(CASI_ENOTFOUND,
                           "cannot determine home directory (HOME is unset)");
            return NULL;
        }
        if (casi_buf_puts(cache, home) != CASI_OK)
            return NULL;
        if (casi_fs_join(cache, "", fallback) != CASI_OK)
            return NULL;
    }

    if (suffix != NULL && casi_fs_join(cache, "", suffix) != CASI_OK)
        return NULL;

    return casi_buf_cstr(cache);
}

const char *casi_paths_data_dir(void)
{
    const char *override;

    if (g_data_dir.len > 0)
        return casi_buf_cstr(&g_data_dir);

    if ((override = env_or_null("CASI_HOME")) != NULL) {
        if (casi_buf_puts(&g_data_dir, override) != CASI_OK)
            return NULL;
        return casi_buf_cstr(&g_data_dir);
    }

    return resolve_xdg(&g_data_dir, "XDG_DATA_HOME", ".local/share", "casi");
}

const char *casi_paths_config_file(void)
{
    const char *override, *dir;

    if (g_config_file.len > 0)
        return casi_buf_cstr(&g_config_file);

    if ((override = env_or_null("CASI_HOME")) != NULL) {
        if (casi_buf_puts(&g_config_file, override) != CASI_OK)
            return NULL;
        if (casi_fs_join(&g_config_file, "", "config") != CASI_OK)
            return NULL;
        return casi_buf_cstr(&g_config_file);
    }

    dir = resolve_xdg(&g_config_file, "XDG_CONFIG_HOME", ".config", "casi");
    if (dir == NULL)
        return NULL;
    if (casi_fs_join(&g_config_file, "", "config") != CASI_OK)
        return NULL;

    return casi_buf_cstr(&g_config_file);
}

/* Derives <data dir>/<name> into `cache`. */
static const char *under_data_dir(casi_buf *cache, const char *name)
{
    const char *data;

    if (cache->len > 0)
        return casi_buf_cstr(cache);

    if ((data = casi_paths_data_dir()) == NULL)
        return NULL;
    if (casi_buf_puts(cache, data) != CASI_OK)
        return NULL;
    if (casi_fs_join(cache, "", name) != CASI_OK)
        return NULL;

    return casi_buf_cstr(cache);
}

const char *casi_paths_repo(void)
{
    return under_data_dir(&g_repo, "repo.git");
}

const char *casi_paths_index(void)
{
    return under_data_dir(&g_index, "index");
}

const char *casi_paths_conflicts_dir(void)
{
    return under_data_dir(&g_conflicts_dir, "conflicts");
}

const char *casi_paths_claude_home(void)
{
    const char *override, *home;

    if (g_claude_home.len > 0)
        return casi_buf_cstr(&g_claude_home);

    if ((override = env_or_null("CASI_CLAUDE_HOME")) != NULL) {
        if (casi_buf_puts(&g_claude_home, override) != CASI_OK)
            return NULL;
        return casi_buf_cstr(&g_claude_home);
    }

    if ((home = casi_fs_home()) == NULL) {
        casi_error_set(CASI_ENOTFOUND,
                       "cannot determine home directory (HOME is unset)");
        return NULL;
    }
    if (casi_buf_puts(&g_claude_home, home) != CASI_OK)
        return NULL;
    if (casi_fs_join(&g_claude_home, "", ".claude") != CASI_OK)
        return NULL;

    return casi_buf_cstr(&g_claude_home);
}

void casi_paths_reset(void)
{
    casi_buf_dispose(&g_config_file);
    casi_buf_dispose(&g_data_dir);
    casi_buf_dispose(&g_repo);
    casi_buf_dispose(&g_index);
    casi_buf_dispose(&g_conflicts_dir);
    casi_buf_dispose(&g_claude_home);
}
