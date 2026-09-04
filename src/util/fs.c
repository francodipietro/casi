/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/fs.h"
#include "casi/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Windows port lands here: _wstat64 / CreateFileW / MoveFileExW / FindFirstFileW,
 * with UTF-8 <-> UTF-16 conversion confined to this file. */

int casi_fs_stat(const char *path, casi_stat *out)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return casi_error_set(CASI_ENOTFOUND, "no such file: %s", path);
        return casi_error_set(CASI_EIO, "cannot stat %s: %s", path, strerror(errno));
    }

    out->size   = (uint64_t)st.st_size;
    out->ino    = (uint64_t)st.st_ino;
    out->is_dir = S_ISDIR(st.st_mode) ? true : false;

#if defined(__APPLE__)
    out->mtime_sec  = (int64_t)st.st_mtimespec.tv_sec;
    out->mtime_nsec = (uint32_t)st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    out->mtime_sec  = (int64_t)st.st_mtim.tv_sec;
    out->mtime_nsec = (uint32_t)st.st_mtim.tv_nsec;
#else
    out->mtime_sec  = (int64_t)st.st_mtime;
    out->mtime_nsec = 0;
#endif

    return CASI_OK;
}

bool casi_fs_exists(const char *path)
{
    casi_stat st;
    return casi_fs_stat(path, &st) == CASI_OK;
}

bool casi_fs_is_dir(const char *path)
{
    casi_stat st;
    return casi_fs_stat(path, &st) == CASI_OK && st.is_dir;
}

int casi_fs_mkdir_p(const char *path)
{
    casi_buf work = CASI_BUF_INIT;
    size_t i;
    int rc;

    if (path[0] == '\0')
        return casi_error_set(CASI_EINVAL, "empty path");

    if ((rc = casi_buf_puts(&work, path)) != CASI_OK)
        return rc;

    /* Walk the components, creating each in turn. Start at 1 so a leading
     * '/' is never treated as a component of its own. */
    for (i = 1; i <= work.len; i++) {
        char saved;

        if (work.ptr[i] != '/' && work.ptr[i] != '\0')
            continue;
        if (work.ptr[i - 1] == '/')  /* collapse "//" */
            continue;

        saved = work.ptr[i];
        work.ptr[i] = '\0';

        if (mkdir(work.ptr, 0777) != 0) {
            casi_stat st;

            if (errno != EEXIST) {
                rc = casi_error_set(CASI_EIO, "cannot create directory %s: %s",
                                    work.ptr, strerror(errno));
                casi_buf_dispose(&work);
                return rc;
            }

            /* Something is already there. It has to be a directory (or a
             * symlink to one, which stat follows): otherwise the walk would
             * blunder on and fail several components later with a far less
             * obvious message. */
            if (casi_fs_stat(work.ptr, &st) != CASI_OK || !st.is_dir) {
                rc = casi_error_set(CASI_EEXISTS,
                                    "cannot create directory %s: path exists "
                                    "and is not a directory", work.ptr);
                casi_buf_dispose(&work);
                return rc;
            }
        }

        work.ptr[i] = saved;
    }

    casi_buf_dispose(&work);
    return CASI_OK;
}

int casi_fs_dirname(casi_buf *out, const char *path)
{
    const char *slash = strrchr(path, '/');

    casi_buf_clear(out);
    if (slash == NULL)
        return casi_buf_puts(out, ".");
    if (slash == path)  /* "/foo" -> "/" */
        return casi_buf_puts(out, "/");

    return casi_buf_put(out, path, (size_t)(slash - path));
}

int casi_fs_mkdir_parent(const char *path)
{
    casi_buf dir = CASI_BUF_INIT;
    int rc;

    if ((rc = casi_fs_dirname(&dir, path)) != CASI_OK)
        goto out;

    rc = casi_fs_mkdir_p(casi_buf_cstr(&dir));

out:
    casi_buf_dispose(&dir);
    return rc;
}

int casi_fs_read_file(const char *path, casi_buf *out)
{
    FILE *f;
    casi_stat st;
    int rc;

    if ((rc = casi_fs_stat(path, &st)) != CASI_OK)
        return rc;
    if (st.is_dir)
        return casi_error_set(CASI_EINVAL, "is a directory: %s", path);

    if ((f = fopen(path, "rb")) == NULL)
        return casi_error_set(CASI_EIO, "cannot open %s: %s", path, strerror(errno));

    casi_buf_clear(out);
    if ((rc = casi_buf_grow(out, (size_t)st.size)) != CASI_OK) {
        fclose(f);
        return rc;
    }

    /* stat's size is only a hint (the file may grow under us), so keep
     * reading until EOF rather than trusting it. */
    for (;;) {
        size_t got;

        if ((rc = casi_buf_grow(out, 65536)) != CASI_OK) {
            fclose(f);
            return rc;
        }

        got = fread(out->ptr + out->len, 1, out->cap - out->len - 1, f);
        out->len += got;
        out->ptr[out->len] = '\0';

        if (got == 0)
            break;
    }

    if (ferror(f)) {
        fclose(f);
        return casi_error_set(CASI_EIO, "error reading %s", path);
    }

    fclose(f);
    return CASI_OK;
}

int casi_fs_read_file_prefix(const char *path, size_t max, casi_buf *out)
{
    FILE *f;
    size_t got;
    int rc;

    if ((f = fopen(path, "rb")) == NULL) {
        if (errno == ENOENT)
            return casi_error_set(CASI_ENOTFOUND, "no such file: %s", path);
        return casi_error_set(CASI_EIO, "cannot open %s: %s", path, strerror(errno));
    }

    casi_buf_clear(out);
    if ((rc = casi_buf_grow(out, max)) != CASI_OK) {
        fclose(f);
        return rc;
    }

    got = fread(out->ptr, 1, max, f);
    out->len = got;
    out->ptr[got] = '\0';

    if (ferror(f)) {
        fclose(f);
        return casi_error_set(CASI_EIO, "error reading %s", path);
    }

    fclose(f);
    return CASI_OK;
}

int casi_fs_realpath(const char *path, casi_buf *out)
{
    char *resolved;
    int rc;

    if ((resolved = realpath(path, NULL)) == NULL) {
        if (errno == ENOENT || errno == ENOTDIR)
            return casi_error_set(CASI_ENOTFOUND, "no such directory: %s", path);
        return casi_error_set(CASI_EIO, "cannot resolve %s: %s",
                              path, strerror(errno));
    }

    casi_buf_clear(out);
    rc = casi_buf_puts(out, resolved);
    free(resolved);
    return rc;
}

int casi_fs_rename_replace(const char *from, const char *to)
{
    /* POSIX rename() already replaces an existing destination atomically.
     * Windows needs MoveFileExW(..., MOVEFILE_REPLACE_EXISTING). */
    if (rename(from, to) != 0)
        return casi_error_set(CASI_EIO, "cannot rename %s to %s: %s",
                              from, to, strerror(errno));
    return CASI_OK;
}

int casi_fs_write_file_atomic(const char *path, const void *data, size_t len)
{
    casi_buf tmp = CASI_BUF_INIT;
    FILE *f = NULL;
    int rc;

    if ((rc = casi_fs_mkdir_parent(path)) != CASI_OK)
        goto out;

    /* Sibling temp file, so the rename stays within one filesystem. */
    if ((rc = casi_buf_printf(&tmp, "%s.casi-tmp-%ld", path, (long)getpid())) != CASI_OK)
        goto out;

    if ((f = fopen(casi_buf_cstr(&tmp), "wb")) == NULL) {
        rc = casi_error_set(CASI_EIO, "cannot create %s: %s",
                            casi_buf_cstr(&tmp), strerror(errno));
        goto out;
    }

    if (len > 0 && fwrite(data, 1, len, f) != len) {
        rc = casi_error_set(CASI_EIO, "short write to %s", casi_buf_cstr(&tmp));
        goto out;
    }

    if (fclose(f) != 0) {
        f = NULL;
        rc = casi_error_set(CASI_EIO, "cannot flush %s: %s",
                            casi_buf_cstr(&tmp), strerror(errno));
        goto out;
    }
    f = NULL;

    rc = casi_fs_rename_replace(casi_buf_cstr(&tmp), path);

out:
    if (f != NULL)
        fclose(f);
    if (rc != CASI_OK && tmp.len > 0)
        remove(casi_buf_cstr(&tmp));
    casi_buf_dispose(&tmp);
    return rc;
}

int casi_fs_remove_file(const char *path)
{
    if (remove(path) != 0) {
        if (errno == ENOENT)
            return CASI_OK;
        return casi_error_set(CASI_EIO, "cannot remove %s: %s", path, strerror(errno));
    }
    return CASI_OK;
}

int casi_fs_listdir(const char *path, casi_strvec *out)
{
    DIR *d;
    struct dirent *ent;
    int rc = CASI_OK;

    if ((d = opendir(path)) == NULL) {
        if (errno == ENOENT)
            return casi_error_set(CASI_ENOTFOUND, "no such directory: %s", path);
        return casi_error_set(CASI_EIO, "cannot open directory %s: %s",
                              path, strerror(errno));
    }

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if ((rc = casi_strvec_push(out, ent->d_name)) != CASI_OK)
            break;
    }

    closedir(d);

    /* readdir order is filesystem-dependent; sort so every machine walks the
     * tree the same way and produces the same commit. */
    if (rc == CASI_OK)
        casi_strvec_sort(out);

    return rc;
}

int casi_fs_join(casi_buf *out, const char *base, const char *rest)
{
    size_t blen = strlen(base);
    int rc;

    if (blen > 0) {
        while (blen > 0 && base[blen - 1] == '/')
            blen--;
        if ((rc = casi_buf_put(out, base, blen)) != CASI_OK)
            return rc;
    }

    while (*rest == '/')
        rest++;

    if (*rest == '\0')
        return CASI_OK;

    if ((rc = casi_buf_putc(out, '/')) != CASI_OK)
        return rc;

    return casi_buf_puts(out, rest);
}

bool casi_fs_stdout_is_tty(void)
{
    return isatty(STDOUT_FILENO) ? true : false;
}

int casi_fs_hostname(casi_buf *out)
{
    char host[256];
    char *dot;

    casi_buf_clear(out);
    if (gethostname(host, sizeof(host)) != 0)
        return casi_buf_puts(out, "unnamed");

    host[sizeof(host) - 1] = '\0';
    if (host[0] == '\0')
        return casi_buf_puts(out, "unnamed");

    dot = strchr(host, '.');
    if (dot != NULL)       /* "mac-air.local" -> "mac-air" */
        *dot = '\0';

    return casi_buf_puts(out, host);
}

bool casi_fs_ssh_hostkey_is_known(const char *host, const char *fingerprint)
{
    char line[512], cmd[512];
    FILE *pipe;
    int command_len, found = 0;

    /* `host` reaches a shell and is also an ssh-keygen argument. Reject an
     * empty/option-looking value plus anything outside a hostname's character
     * set instead of trying to quote it. */
    if (host == NULL || fingerprint == NULL || host[0] == '\0' || host[0] == '-')
        return false;

    for (const char *p = host; *p != '\0'; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_';
        if (!ok)
            return false;
    }

    command_len = snprintf(cmd, sizeof(cmd),
                           "ssh-keygen -l -F %s 2>/dev/null", host);
    if (command_len < 0 || (size_t)command_len >= sizeof(cmd))
        return false;
    if ((pipe = popen(cmd, "r")) == NULL)
        return false;

    while (fgets(line, sizeof(line), pipe) != NULL) {
        if (strstr(line, fingerprint) != NULL) {
            found = 1;
            break;
        }
    }

    pclose(pipe);
    return found ? true : false;
}

const char *casi_fs_home(void)
{
    const char *home = getenv("HOME");

    /* Windows port: fall back to %USERPROFILE%, then %HOMEDRIVE%%HOMEPATH%. */
    if (home != NULL && home[0] != '\0')
        return home;

    return NULL;
}
