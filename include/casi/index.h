/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_INDEX_H
#define CASI_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include <git2.h>

#include "casi/fs.h"
#include "casi/roots.h"

/*
 * A local, disposable stat cache for transcript scans.  It is deliberately
 * outside the git store: entries name local paths and local root mappings,
 * neither of which belongs on another machine.
 *
 * The cache is valid only for the exact root-table snapshot it was written
 * with.  A hit requires path, provider, size, mtime (including nanoseconds),
 * and inode to match.  When a transcript has grown on the same inode, the
 * scanner can reuse every chunk before `tail_raw_offset` and reread only its
 * final chunk plus the newly appended bytes.  That offset is always directly
 * after a newline, so path normalisation has no cross-boundary state there.
 */

typedef struct casi_index casi_index;

typedef struct {
    const char    *path;
    const char    *provider;
    casi_stat      stat;
    uint64_t       tail_raw_offset;
    uint64_t       tail_normalized_offset;
    uint64_t       normalized_bytes;
    const git_oid *chunks;
    size_t         chunk_count;
} casi_index_entry;

/* Missing, malformed, or obsolete cache files are treated as an empty cache.
 * `crypto_key_id` separates otherwise identical local scans encrypted with
 * different keys; NULL means no encryption. */
int  casi_index_open(const casi_roots *roots, const char *crypto_key_id,
                     casi_index **out);
void casi_index_free(casi_index *index);

const casi_index_entry *casi_index_find(const casi_index *index,
                                        const char *provider, const char *path);
bool casi_index_entry_matches(const casi_index_entry *entry, const casi_stat *stat);
bool casi_index_entry_can_resume(const casi_index_entry *entry, const casi_stat *stat);
/* Structural validation for untrusted bytes read from the local cache file. */
bool casi_index_entry_is_well_formed(const casi_index_entry *entry);

/* Replaces one entry. Copies all pointed-to data. */
int casi_index_update(casi_index *index, const char *provider, const char *path,
                      const casi_stat *stat, uint64_t tail_raw_offset,
                      uint64_t tail_normalized_offset, uint64_t normalized_bytes,
                      const git_oid *chunks, size_t chunk_count);

/* Writes only when updates were made, through an atomic replacement. */
int casi_index_flush(casi_index *index);

#endif /* CASI_INDEX_H */
