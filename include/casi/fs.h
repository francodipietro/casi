/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_FS_H
#define CASI_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "casi/buf.h"
#include "casi/str.h"

/*
 * Every direct operating-system interaction casi makes goes through this
 * file. That is deliberate: it is the single place the eventual Windows port
 * has to touch.
 *
 * Two rules hold everywhere above this layer:
 *   - paths are UTF-8 and use '/' as the separator, on every platform;
 *   - file I/O is always binary, so no CRT text-mode newline translation can
 *     ever corrupt a session transcript.
 */

typedef struct {
    uint64_t size;
    int64_t  mtime_sec;
    uint32_t mtime_nsec;
    uint64_t ino;
    bool     is_dir;
} casi_stat;

int  casi_fs_stat(const char *path, casi_stat *out);
bool casi_fs_exists(const char *path);
bool casi_fs_is_dir(const char *path);

/* Creates `path` and any missing parents. Succeeds if it already exists. */
int  casi_fs_mkdir_p(const char *path);
/* Same, for the directory containing `path`. */
int  casi_fs_mkdir_parent(const char *path);

/* Replaces the contents of `out`. */
int  casi_fs_read_file(const char *path, casi_buf *out);
/* Replaces `out` with bytes from `offset` through EOF. */
int  casi_fs_read_file_from(const char *path, uint64_t offset, casi_buf *out);
/* First `max` bytes only. Transcripts reach hundreds of megabytes and the
 * fields casi reads to identify one live in the first record, so scanning a
 * session should never mean reading all of it. */
int  casi_fs_read_file_prefix(const char *path, size_t max, casi_buf *out);
/* Resolves `path` to an absolute, symlink-free path in `out`. */
int  casi_fs_realpath(const char *path, casi_buf *out);

/*
 * Writes via a sibling temp file plus an atomic replace, so an interrupted
 * casi never leaves a half-written session on disk. Creates parent dirs.
 */
int  casi_fs_write_file_atomic(const char *path, const void *data, size_t len);
/* Same atomic replacement, but the resulting file is readable only by its
 * owner. Used exclusively for locally held cryptographic keys. */
int  casi_fs_write_file_atomic_private(const char *path, const void *data, size_t len);
/* Reads a regular file only when group/other permissions are absent. */
int  casi_fs_read_file_private(const char *path, casi_buf *out);
int  casi_fs_rename_replace(const char *from, const char *to);
/* Creates a symbolic link at `path` pointing to `target`. */
int  casi_fs_symlink(const char *target, const char *path);
int  casi_fs_remove_file(const char *path);

/* Runs `git -C <bare-repo> gc --prune=now` without invoking a shell. This is
 * kept here with the other process/filesystem boundary for the eventual
 * Windows port. */
int  casi_fs_git_gc(const char *bare_repo_path);

/* Entry names only, without "." and "..", sorted. */
int  casi_fs_listdir(const char *path, casi_strvec *out);

/* Appends "base/rest" to `out`, collapsing the separator. */
int  casi_fs_join(casi_buf *out, const char *base, const char *rest);
/* Writes the directory part of `path` into `out` ("." when there is none). */
int  casi_fs_dirname(casi_buf *out, const char *path);

/* Whether stdout is a terminal. Lives here rather than in the logger because
 * isatty() is a syscall, and on Windows becomes _isatty()/GetConsoleMode(). */
bool casi_fs_stdout_is_tty(void);

/* Whether stderr is a terminal -- where a self-overwriting progress line is
 * meaningful. Progress stays silent when piped or redirected. */
bool casi_fs_stderr_is_tty(void);

/* A short, user-facing hostname; falls back to "unnamed" when unavailable. */
int casi_fs_hostname(casi_buf *out);

/* Whether OpenSSH's known_hosts contains `fingerprint` for `host`. */
bool casi_fs_ssh_hostkey_is_known(const char *host, const char *fingerprint);

/* The user's home directory. NULL only if the environment has none. */
const char *casi_fs_home(void);

#endif /* CASI_FS_H */
