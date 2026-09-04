/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_CHUNK_H
#define CASI_CHUNK_H

#include <stddef.h>

/*
 * Session transcripts are append-only JSONL and can reach hundreds of
 * megabytes. Git stores blobs whole, so a 137 MB session that grows by 100 KB
 * would produce a second 137 MB blob on every push.
 *
 * Splitting deterministically from byte 0, always cutting just after a
 * newline, means every earlier chunk stays byte-identical as the file grows
 * and hashes to the same object id. A push then adds exactly one new blob.
 * No rolling window, no content-defined chunking, no delta logic.
 *
 * The boundary rule: a chunk runs to at least CASI_CHUNK_TARGET bytes and
 * then to the end of the line it lands in. Only the final chunk may be
 * shorter, and only it may lack a trailing newline. A single line longer than
 * the target simply becomes an oversized chunk of its own.
 *
 * Chunking must happen AFTER path normalisation, so that two machines with
 * different local layouts produce identical blobs for the same session.
 */

#define CASI_CHUNK_TARGET (1024 * 1024)

/* Returning non-zero stops the walk and is propagated to the caller. */
typedef int (*casi_chunk_cb)(const void *data, size_t len, size_t index, void *payload);

int casi_chunk_split(const void *data, size_t len, size_t target,
                     casi_chunk_cb cb, void *payload);

/* How many chunks `len` bytes would produce, without emitting them. */
size_t casi_chunk_count(const void *data, size_t len, size_t target);

#endif /* CASI_CHUNK_H */
