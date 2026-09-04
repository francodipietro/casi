/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_PROVIDER_H
#define CASI_PROVIDER_H

#include "casi/buf.h"
#include "casi/roots.h"

/*
 * The extensibility seam. Everything above this line -- chunking, the prefix
 * rule, path normalisation, the transport -- knows nothing about which
 * assistant it is syncing.
 *
 * The remote tree namespaces by provider name, so a second assistant is a new
 * vtable and a new namespace, not a change to the sync core. v1 registers one.
 */

/* One syncable session as it exists on this machine. */
typedef struct {
    char *session_id;    /* the assistant's own id; names the file           */
    char *local_path;    /* absolute path of the transcript on this machine  */
    char *project_path;  /* canonical project path, e.g. "casi://src/bookit" */
    char *project_id;    /* stable hex id derived from project_path          */
} casi_session;

typedef struct {
    casi_session *items;
    size_t len;
    size_t cap;
} casi_session_list;

void casi_session_list_dispose(casi_session_list *list);
int  casi_session_list_push(casi_session_list *list, const casi_session *session);

typedef struct casi_provider {
    const char *name;

    /* Enumerates every session this machine holds, with paths already
     * normalised through `roots`. */
    int (*discover)(const casi_roots *roots, casi_session_list *out);

    /* Absolute local path where a session with this canonical project path
     * and id belongs on this machine. */
    int (*local_path_for)(const casi_roots *roots, const char *project_path,
                          const char *session_id, casi_buf *out);
} casi_provider;

const casi_provider *casi_provider_default(void);
const casi_provider *casi_provider_lookup(const char *name);

/* Stable identifier for a canonical project path: the hex object id libgit2
 * would give its bytes. Hex keeps the tree free of characters a filesystem
 * might reject, reserved Windows names, and case-folding surprises; the
 * human-readable path travels in the session's meta.json instead. */
int casi_project_id(const char *project_path, casi_buf *out);

#endif /* CASI_PROVIDER_H */
