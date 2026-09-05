/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_STORE_H
#define CASI_STORE_H

#include <stdint.h>

#include "casi/provider.h"
#include "casi/repo.h"
#include "casi/roots.h"

/*
 * The mapping between sessions on disk and the shape they take in the git
 * tree:
 *
 *   casi.json
 *   sessions/<provider>/<project-id>/<session-id>/meta.json
 *   sessions/<provider>/<project-id>/<session-id>/chunks/000000 ...
 *   sessions/<provider>/<project-id>/<session-id>/subagents/<file>
 *   projects/<provider>/<project-id>/memory/<file>
 *
 * Content is normalised through the roots table BEFORE it is chunked, so two
 * machines with different local layouts produce byte-identical blobs for the
 * same session. Deduplication and the prefix rule both depend on that
 * ordering.
 */

#define CASI_FORMAT_VERSION 1

/* A session reduced to what sync actually compares: the ordered list of chunk
 * object ids. Everything else is metadata for humans. */
typedef struct {
    char    *session_id;
    char    *project_path;   /* canonical */
    char    *project_id;
    git_oid *chunks;
    size_t   chunk_count;
    uint64_t bytes;          /* normalised size, not the on-disk size */
} casi_entry;

typedef struct {
    casi_entry *items;
    size_t len;
    size_t cap;
} casi_entry_list;

typedef struct {
    casi_aux_kind kind;
    char         *project_path;
    char         *project_id;
    char         *session_id;  /* set only for CASI_AUX_SUBAGENT */
    char         *name;
    git_oid       oid;
    uint64_t      bytes;
} casi_asset;

typedef struct {
    casi_asset *items;
    size_t      len;
    size_t      cap;
} casi_asset_list;

void casi_entry_list_dispose(casi_entry_list *list);
void casi_entry_dispose_one(casi_entry *entry);
/* Takes ownership of the entry's allocations; the caller's copy is emptied. */
int  casi_entry_list_push_owned(casi_entry_list *list, casi_entry *entry);
void casi_entry_swap(casi_entry *a, casi_entry *b);
/* Linear lookup by session id; NULL when absent. */
casi_entry *casi_entry_list_find(casi_entry_list *list, const char *session_id);

void casi_asset_list_dispose(casi_asset_list *list);
/* Takes ownership of the asset's allocations; the caller's copy is emptied. */
int  casi_asset_list_push_owned(casi_asset_list *list, casi_asset *asset);
casi_asset *casi_asset_list_find(casi_asset_list *list, casi_aux_kind kind,
                                 const char *project_id, const char *session_id,
                                 const char *name);

/*
 * Reads every local session, normalises it, chunks it, and writes the blobs
 * into the store. The returned entries describe what a push would upload.
 * `exclude` may be NULL; when given, project paths listed in it are skipped
 * and `excluded_out`, if given, receives how many sessions that removed --
 * so status can say what it is not showing rather than quietly showing less.
 */
int casi_store_scan_local(casi_repo *repo, const casi_roots *roots,
                          const casi_provider *provider,
                          const casi_strvec *exclude,
                          casi_entry_list *out, casi_asset_list *assets_out,
                          size_t *excluded_out);

/* Reads the entries recorded in a tree written by a previous push. */
int casi_store_read_tree(casi_repo *repo, const git_oid *tree,
                         const char *provider_name, casi_entry_list *out,
                         casi_asset_list *assets_out);

/* Builds the tree for a set of entries. */
int casi_store_write_tree(casi_repo *repo, const char *provider_name,
                          const casi_entry_list *entries,
                          const casi_asset_list *assets, git_oid *tree_out);

/*
 * Reassembles an entry's chunks, translates the paths back to this machine,
 * and writes the transcript where the provider says it belongs.
 *
 * Returns CASI_EUNMAPPED, without writing anything, when the content names a
 * root this machine has not declared -- `unmapped_out` then holds the root's
 * name so the caller can print the exact config line that is missing.
 */
int casi_store_materialize(casi_repo *repo, const casi_roots *roots,
                           const casi_provider *provider, const casi_entry *entry,
                           casi_buf *unmapped_out);

/* Same reassembly, but written to an explicit path -- used to park the remote
 * side of a conflict instead of overwriting the local file. */
int casi_store_materialize_to(casi_repo *repo, const casi_roots *roots,
                              const casi_entry *entry, const char *path,
                              casi_buf *unmapped_out);

/* Rewrites paths in an auxiliary blob and materialises it where the provider
 * expects the sidecar or memory file to live. */
int casi_store_materialize_asset(casi_repo *repo, const casi_roots *roots,
                                 const casi_provider *provider,
                                 const casi_asset *asset, casi_buf *unmapped_out);
/* Same, but writes an auxiliary file to an explicit conflict-parking path. */
int casi_store_materialize_asset_to(casi_repo *repo, const casi_roots *roots,
                                    const casi_asset *asset, const char *path,
                                    casi_buf *unmapped_out);

#endif /* CASI_STORE_H */
