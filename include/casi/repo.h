/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_REPO_H
#define CASI_REPO_H

#include <git2.h>

#include "casi/buf.h"
#include "casi/crypto.h"
#include "casi/str.h"

/*
 * The internal git repository, and the only place libgit2 is driven.
 *
 * It is bare and stays bare. casi writes blobs and trees straight into the
 * object database and materialises files itself, so git_checkout_* and the
 * index-on-disk are never involved. That is what keeps line-ending
 * translation, Unix mode bits, symlinks and checkout semantics out of the
 * picture entirely -- and those are exactly the four things that would make a
 * Windows port painful.
 *
 * Branch layout: each machine owns refs/heads/casi/<machine> and only ever
 * writes its own. refs/heads/casi/config is the sole shared branch and its
 * optimistic update protocol retries a non-fast-forward push. A pull reads
 * every machine branch under refs/heads/casi/ and takes their union.
 */

typedef struct casi_repo casi_repo;

/* Opens the store; CASI_ENOTFOUND if `casi init` has not run. */
int  casi_repo_open(casi_repo **out);
/* Opens it, creating the bare repository if needed. */
int  casi_repo_init(casi_repo **out);
void casi_repo_free(casi_repo *repo);

git_repository *casi_repo_git(casi_repo *repo);

/* Installs a copy of the local crypto context for all ordinary content blobs. */
void casi_repo_set_crypto(casi_repo *repo, const casi_crypto *crypto);
bool casi_repo_crypto_enabled(const casi_repo *repo);
/* Encodes one logical tree component when the local store is encrypted. */
int casi_repo_path_component(casi_repo *repo, const char *component, casi_buf *out);
/* A stable cache domain; it changes when the configured encryption key does. */
int casi_repo_crypto_key_id(casi_repo *repo, casi_buf *out);

/* A machine label becomes the final component of refs/heads/casi/<machine>.
 * Validate that invariant before persisting it or trying to create a ref. */
int casi_repo_validate_machine_name(const char *machine);

int casi_repo_write_blob(casi_repo *repo, const void *data, size_t len, git_oid *out);
int casi_repo_read_blob(casi_repo *repo, const git_oid *oid, casi_buf *out);
int casi_repo_blob_size(casi_repo *repo, const git_oid *oid, uint64_t *out);
/* Raw blobs are reserved for the non-secret encrypted-config header. */
int casi_repo_write_blob_raw(casi_repo *repo, const void *data, size_t len, git_oid *out);
int casi_repo_read_blob_raw(casi_repo *repo, const git_oid *oid, casi_buf *out);
int casi_repo_has_object(casi_repo *repo, const git_oid *oid);

/*
 * Tree building goes through an in-memory index rather than nested
 * git_treebuilders: libgit2 then handles the whole directory nesting, and
 * "in-memory" means no index file ever touches disk.
 */
typedef struct casi_tree casi_tree;

int  casi_tree_new(casi_repo *repo, casi_tree **out);
int  casi_tree_add(casi_tree *tree, const char *path, const git_oid *blob);
int  casi_tree_add_text(casi_tree *tree, const char *path, const char *text);
int  casi_tree_add_text_raw(casi_tree *tree, const char *path, const char *text);
int  casi_tree_write(casi_tree *tree, git_oid *tree_out);
void casi_tree_free(casi_tree *tree);

/* Commits `tree` onto `refname`, parented on whatever it points at now. */
int casi_repo_commit(casi_repo *repo, const char *refname, const git_oid *tree,
                     const char *machine, const char *message, git_oid *out);

/* Tree a ref currently points at. CASI_ENOTFOUND when the ref does not exist,
 * which is the ordinary state before the first push. */
int casi_repo_ref_tree(casi_repo *repo, const char *refname, git_oid *out);

/* Makes `refname` point at `source_refname` when the latter exists. Used by
 * the shared configuration transaction to parent its next commit on the
 * freshly fetched authoritative state. A missing source is ordinary before
 * the first configuration push. */
int casi_repo_reset_ref_from(casi_repo *repo, const char *refname,
                             const char *source_refname);

/* Object id of a tree entry addressed by path. */
int casi_repo_tree_entry_oid(casi_repo *repo, const git_oid *tree,
                             const char *path, git_oid *out);

/* Reads one blob addressed by path within a tree. */
int casi_repo_tree_entry_blob(casi_repo *repo, const git_oid *tree,
                              const char *path, casi_buf *out);
int casi_repo_tree_entry_blob_raw(casi_repo *repo, const git_oid *tree,
                                  const char *path, casi_buf *out);
/* Names of the entries directly under `path` in `tree` (sorted, no recursion).
 * CASI_ENOTFOUND when the path is absent -- an empty store, usually. */
int casi_repo_tree_list(casi_repo *repo, const git_oid *tree, const char *path,
                        casi_strvec *out);

/* --- transport ------------------------------------------------------- */

int casi_repo_set_remote(casi_repo *repo, const char *url);
int casi_repo_remote_url(casi_repo *repo, casi_buf *out);

/* Fetches every branch under refs/heads/casi/ into the matching
 * refs/remotes/origin/casi/ tracking ref. */
int casi_repo_fetch(casi_repo *repo);
/* Pushes one local ref. */
int casi_repo_push(casi_repo *repo, const char *refname);

/* Remote-tracking branch names under refs/remotes/origin/casi/, i.e. the
 * machines this store has seen. */
int casi_repo_list_machines(casi_repo *repo, casi_strvec *out);

/* Prunes unreachable objects and repacks the local bare store. */
int casi_repo_gc(casi_repo *repo);

#endif /* CASI_REPO_H */
