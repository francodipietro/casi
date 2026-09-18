/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_SHARED_CONFIG_H
#define CASI_SHARED_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "casi/buf.h"
#include "casi/crypto.h"
#include "casi/repo.h"
#include "casi/str.h"

/*
 * State stored authoritatively in the remote's casi.json. Local machine
 * configuration still maps root names to local paths; this file records only
 * names that exist in the shared namespace and project exclusions that must
 * apply before a newly configured machine performs its first push.
 *
 * The on-wire representation is deliberately compact and deterministic:
 *
 *   {"format":1,"roots":["bookit","src"],"sync":{"exclude":[...]}}
 */
#define CASI_SHARED_CONFIG_FORMAT 1
#define CASI_SHARED_CONFIG_REF "refs/heads/casi/config"
#define CASI_SHARED_CONFIG_REMOTE_REF "refs/remotes/origin/casi/config"

typedef struct {
    casi_strvec roots;    /* logical names; never the implicit "~" root */
    casi_strvec exclude;  /* canonical casi:// project paths */
} casi_shared_config;

void casi_shared_config_dispose(casi_shared_config *cfg);

/* Replaces cfg from a casi.json blob. A missing file is represented by an
 * empty buffer and yields the empty, current-format configuration. */
int casi_shared_config_parse(casi_shared_config *cfg, const char *data, size_t len);
int casi_shared_config_serialize(const casi_shared_config *cfg, casi_buf *out);

/* Set-like mutations: additions are idempotent and vectors stay sorted for a
 * stable Git object id. Removing an absent exclusion returns CASI_ENOTFOUND. */
int  casi_shared_config_add_root(casi_shared_config *cfg, const char *name);
int  casi_shared_config_add_exclude(casi_shared_config *cfg, const char *project_path);
int  casi_shared_config_remove_exclude(casi_shared_config *cfg, const char *project_path);
bool casi_shared_config_is_excluded(const casi_shared_config *cfg,
                                    const char *project_path);

/* Loads the fetched authoritative config ref. `present_out` distinguishes a
 * first use from an existing, intentionally empty configuration. */
int casi_shared_config_load_remote(casi_repo *repo, const casi_crypto *crypto,
                                   casi_shared_config *cfg,
                                   bool *present_out);
/* Commits cfg onto the freshly fetched config ref and pushes it. A concurrent
 * update returns CASI_ERETRY so the caller can refetch, replay its mutation,
 * and try again. */
int casi_shared_config_commit_push(casi_repo *repo, const casi_shared_config *cfg,
                                   const casi_crypto *crypto, const char *machine);

#endif /* CASI_SHARED_CONFIG_H */
