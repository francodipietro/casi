/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/ctx.h"
#include "casi/casi.h"
#include "casi/shared_config.h"

#include <stdlib.h>
#include <string.h>

int casi_ctx_open(casi_ctx *ctx)
{
    casi_buf value = CASI_BUF_INIT;
    int rc;

    memset(ctx, 0, sizeof(*ctx));

    if ((rc = casi_config_open(&ctx->cfg)) != CASI_OK)
        goto fail;

    if ((rc = casi_config_get_string(ctx->cfg, "core.machine", &value)) != CASI_OK) {
        rc = casi_error_set(CASI_ENOTFOUND,
                            "this machine has no casi store yet -- run `casi init`");
        goto fail;
    }
    if ((ctx->machine = casi_strdup(casi_buf_cstr(&value))) == NULL) {
        rc = CASI_ENOMEM;
        goto fail;
    }
    if ((rc = casi_repo_validate_machine_name(ctx->machine)) != CASI_OK)
        goto fail;

    if ((rc = casi_roots_new(&ctx->roots)) != CASI_OK)
        goto fail;
    if ((rc = casi_roots_load(ctx->roots, ctx->cfg)) != CASI_OK)
        goto fail;

    /* One provider today; the config key exists so a second one slots in
     * without changing how commands are invoked. */
    casi_buf_clear(&value);
    if (casi_config_get_string(ctx->cfg, "core.provider", &value) == CASI_OK) {
        ctx->provider = casi_provider_lookup(casi_buf_cstr(&value));
        if (ctx->provider == NULL) {
            rc = casi_error_set(CASI_EINVAL, "unknown provider \"%s\"",
                                casi_buf_cstr(&value));
            goto fail;
        }
    } else {
        casi_error_clear();
        ctx->provider = casi_provider_default();
    }

    if ((rc = casi_repo_open(&ctx->repo)) != CASI_OK)
        goto fail;

    casi_buf_dispose(&value);
    return CASI_OK;

fail:
    casi_buf_dispose(&value);
    casi_ctx_dispose(ctx);
    return rc;
}

void casi_ctx_dispose(casi_ctx *ctx)
{
    casi_repo_free(ctx->repo);
    casi_roots_free(ctx->roots);
    casi_config_free(ctx->cfg);
    free(ctx->machine);
    memset(ctx, 0, sizeof(*ctx));
}

int casi_ctx_local_ref(const casi_ctx *ctx, casi_buf *out)
{
    casi_buf_clear(out);
    return casi_buf_printf(out, "refs/heads/casi/%s", ctx->machine);
}

int casi_ctx_exclude_list(const casi_ctx *ctx, casi_strvec *out)
{
    casi_shared_config shared = { 0 };
    bool present;
    size_t i;
    int rc;

    if ((rc = casi_shared_config_load_remote(ctx->repo, &shared, &present)) != CASI_OK)
        goto done;

    if (!present) {
        rc = casi_config_get_multivar(ctx->cfg, "sync.exclude", out);
        goto done;
    }

    casi_strvec_dispose(out);
    for (i = 0; i < shared.exclude.len; i++)
        if ((rc = casi_strvec_push(out, shared.exclude.items[i])) != CASI_OK)
            goto done;
    rc = CASI_OK;

done:
    casi_shared_config_dispose(&shared);
    return rc;
}

static int add_local_root_names(casi_ctx *ctx, casi_shared_config *shared)
{
    size_t i;
    int rc;

    for (i = 0; i < casi_roots_count(ctx->roots); i++) {
        const char *name = casi_roots_name_at(ctx->roots, i);

        if (strcmp(name, CASI_HOME_ROOT) != 0 &&
            (rc = casi_shared_config_add_root(shared, name)) != CASI_OK)
            return rc;
    }
    return CASI_OK;
}

int casi_ctx_publish_shared_config(casi_ctx *ctx)
{
    unsigned int attempt;
    int rc = CASI_ERETRY;

    for (attempt = 0; attempt < 3; attempt++) {
        casi_shared_config shared = { 0 };
        casi_strvec legacy = CASI_STRVEC_INIT;
        bool present;
        size_t i;

        if ((rc = casi_repo_fetch(ctx->repo)) != CASI_OK)
            goto done;
        if ((rc = casi_shared_config_load_remote(ctx->repo, &shared, &present)) != CASI_OK)
            goto done;

        /* The old local-only list must be brought across before the first
         * session push; otherwise upgrading would expose an excluded project. */
        if (!present &&
            (rc = casi_config_get_multivar(ctx->cfg, "sync.exclude", &legacy)) != CASI_OK)
            goto done;
        for (i = 0; rc == CASI_OK && i < legacy.len; i++)
            rc = casi_shared_config_add_exclude(&shared, legacy.items[i]);
        if (rc == CASI_OK)
            rc = add_local_root_names(ctx, &shared);
        if (rc == CASI_OK)
            rc = casi_shared_config_commit_push(ctx->repo, &shared, ctx->machine);

done:
        casi_strvec_dispose(&legacy);
        casi_shared_config_dispose(&shared);
        if (rc != CASI_ERETRY)
            return rc;
        casi_error_clear();
    }

    return casi_error_set(CASI_ERROR,
                          "shared configuration changed concurrently; try again");
}
