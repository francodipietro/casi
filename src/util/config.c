/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/config.h"
#include "casi/error.h"
#include "casi/fs.h"
#include "casi/paths.h"

#include <stdlib.h>
#include <string.h>

#include <git2.h>

struct casi_config {
    git_config *cfg;
};

int casi_config_open_path(casi_config **out, const char *path)
{
    casi_config *self;
    int rc;

    /* libgit2 will happily read a missing file as empty, but writing to one
     * whose directory does not exist fails. Settle both up front so every
     * later get/set is a plain operation. */
    if ((rc = casi_fs_mkdir_parent(path)) != CASI_OK)
        return rc;
    if (!casi_fs_exists(path) &&
        (rc = casi_fs_write_file_atomic(path, "", 0)) != CASI_OK)
        return rc;

    if ((self = calloc(1, sizeof(*self))) == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory opening config");

    if (git_config_open_ondisk(&self->cfg, path) != 0) {
        free(self);
        return casi_error_set_git(CASI_EIO, "cannot open config %s", path);
    }

    *out = self;
    return CASI_OK;
}

int casi_config_open(casi_config **out)
{
    const char *path = casi_paths_config_file();

    if (path == NULL)
        return casi_error_last_code();

    return casi_config_open_path(out, path);
}

void casi_config_free(casi_config *cfg)
{
    if (cfg == NULL)
        return;
    git_config_free(cfg->cfg);
    free(cfg);
}

int casi_config_get_string(casi_config *cfg, const char *key, casi_buf *out)
{
    git_buf value = GIT_BUF_INIT;
    int err, rc;

    err = git_config_get_string_buf(&value, cfg->cfg, key);
    if (err == GIT_ENOTFOUND)
        return casi_error_set(CASI_ENOTFOUND, "config key not set: %s", key);
    if (err != 0)
        return casi_error_set_git(CASI_ERROR, "cannot read config key %s", key);

    rc = casi_buf_set(out, value.ptr, value.size);
    git_buf_dispose(&value);
    return rc;
}

int casi_config_get_bool(casi_config *cfg, const char *key, bool fallback, bool *out)
{
    int value, err;

    err = git_config_get_bool(&value, cfg->cfg, key);
    if (err == GIT_ENOTFOUND) {
        *out = fallback;
        return CASI_OK;
    }
    if (err != 0)
        return casi_error_set_git(CASI_EINVAL, "config key %s is not a boolean", key);

    *out = value != 0;
    return CASI_OK;
}

int casi_config_set_string(casi_config *cfg, const char *key, const char *value)
{
    if (git_config_set_string(cfg->cfg, key, value) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot set config key %s", key);
    return CASI_OK;
}

int casi_config_unset(casi_config *cfg, const char *key)
{
    int err = git_config_delete_entry(cfg->cfg, key);

    if (err == GIT_ENOTFOUND)
        return casi_error_set(CASI_ENOTFOUND, "config key not set: %s", key);
    if (err != 0)
        return casi_error_set_git(CASI_ERROR, "cannot unset config key %s", key);

    return CASI_OK;
}

struct multivar_ctx {
    casi_strvec *out;
    int rc;
};

static int multivar_collect(const git_config_entry *entry, void *payload)
{
    struct multivar_ctx *ctx = payload;

    ctx->rc = casi_strvec_push(ctx->out, entry->value != NULL ? entry->value : "");
    return ctx->rc != CASI_OK ? -1 : 0;
}

int casi_config_get_multivar(casi_config *cfg, const char *key, casi_strvec *out)
{
    struct multivar_ctx ctx = { out, CASI_OK };
    int err;

    err = git_config_get_multivar_foreach(cfg->cfg, key, NULL, multivar_collect, &ctx);

    /* No entries is the ordinary state, not a failure: nothing is excluded. */
    if (err == GIT_ENOTFOUND)
        return CASI_OK;
    if (ctx.rc != CASI_OK)
        return ctx.rc;
    if (err != 0)
        return casi_error_set_git(CASI_ERROR, "cannot read config key %s", key);

    return CASI_OK;
}

int casi_config_add_multivar(casi_config *cfg, const char *key, const char *value)
{
    if (git_config_set_multivar(cfg->cfg, key, "^$", value) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot add %s = %s", key, value);
    return CASI_OK;
}

int casi_config_remove_multivar(casi_config *cfg, const char *key, const char *value)
{
    casi_buf pattern = CASI_BUF_INIT;
    const char *p;
    int err, rc;

    /* git matches multivar values by regex, so anchor and escape the literal
     * path -- a project path is full of dots and dashes. */
    if ((rc = casi_buf_putc(&pattern, '^')) != CASI_OK)
        goto done;
    for (p = value; *p != '\0'; p++) {
        if (strchr(".^$*+?()[]{}|\\", *p) != NULL &&
            (rc = casi_buf_putc(&pattern, '\\')) != CASI_OK)
            goto done;
        if ((rc = casi_buf_putc(&pattern, *p)) != CASI_OK)
            goto done;
    }
    if ((rc = casi_buf_putc(&pattern, '$')) != CASI_OK)
        goto done;

    err = git_config_delete_multivar(cfg->cfg, key, casi_buf_cstr(&pattern));
    if (err == GIT_ENOTFOUND)
        rc = casi_error_set(CASI_ENOTFOUND, "not in %s: %s", key, value);
    else if (err != 0)
        rc = casi_error_set_git(CASI_ERROR, "cannot remove %s from %s", value, key);
    else
        rc = CASI_OK;

done:
    casi_buf_dispose(&pattern);
    return rc;
}

struct foreach_ctx {
    casi_config_cb cb;
    void *payload;
    int rc;
};

static int foreach_trampoline(const git_config_entry *entry, void *payload)
{
    struct foreach_ctx *ctx = payload;

    ctx->rc = ctx->cb(entry->name, entry->value != NULL ? entry->value : "",
                      ctx->payload);
    /* Any non-zero stops libgit2's walk; we carry the real code in ctx. */
    return ctx->rc != CASI_OK ? -1 : 0;
}

int casi_config_foreach(casi_config *cfg, casi_config_cb cb, void *payload)
{
    struct foreach_ctx ctx = { cb, payload, CASI_OK };
    int err = git_config_foreach(cfg->cfg, foreach_trampoline, &ctx);

    if (ctx.rc != CASI_OK)
        return ctx.rc;
    if (err != 0)
        return casi_error_set_git(CASI_ERROR, "cannot iterate config");

    return CASI_OK;
}
