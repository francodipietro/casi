/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_CTX_H
#define CASI_CTX_H

#include "casi/config.h"
#include "casi/provider.h"
#include "casi/repo.h"
#include "casi/roots.h"

/* Everything a command needs, opened once. */
typedef struct {
    casi_config         *cfg;
    casi_roots          *roots;
    casi_repo           *repo;
    const casi_provider *provider;
    char                *machine;   /* this machine's label; names its branch */
} casi_ctx;

/* Fails with CASI_ENOTFOUND when `casi init` has not run here. */
int  casi_ctx_open(casi_ctx *ctx);
void casi_ctx_dispose(casi_ctx *ctx);

/* refs/heads/casi/<machine> -- the branch this machine owns and alone writes. */
int casi_ctx_local_ref(const casi_ctx *ctx, casi_buf *out);

/* The excluded canonical project paths, from sync.exclude. */
int casi_ctx_exclude_list(const casi_ctx *ctx, casi_strvec *out);

/* Fetches and publishes the authoritative shared config before a push. On a
 * legacy remote with no config ref yet, local sync.exclude is migrated first
 * so no existing exclusion is bypassed during the transition. */
int casi_ctx_publish_shared_config(casi_ctx *ctx);

#endif /* CASI_CTX_H */
