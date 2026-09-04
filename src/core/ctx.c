/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/ctx.h"
#include "casi/casi.h"

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
    return casi_config_get_multivar(ctx->cfg, "sync.exclude", out);
}
