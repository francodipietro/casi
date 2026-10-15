/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"

#include "casi/ctx.h"
#include "casi/repo.h"
#include "casi/fs.h"
#include "casi/jsonl.h"
#include "casi/paths.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define RETENTION_SECONDS (30ULL * 24ULL * 60ULL * 60ULL)

static bool suffix(const char *s, const char *tail)
{
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && strcmp(s + n - m, tail) == 0;
}

/* Never follow entries placed in the conflict directory. */
static bool regular_file(const char *path, uint64_t *size)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    *size = (uint64_t)st.st_size;
    return true;
}

static int conflict_plan(bool remove, uint64_t *eligible, uint64_t *skipped,
                         uint64_t *bytes, uint64_t *removed)
{
    casi_strvec names = CASI_STRVEC_INIT;
    casi_buf path = CASI_BUF_INIT, meta_path = CASI_BUF_INIT, meta = CASI_BUF_INIT;
    const char *dir = casi_paths_conflicts_dir();
    uint64_t now = (uint64_t)time(NULL);
    size_t i;
    int rc;

    *eligible = *skipped = *bytes = *removed = 0;
    rc = casi_fs_listdir(dir, &names);
    if (rc == CASI_ENOTFOUND) { casi_error_clear(); return CASI_OK; }
    if (rc != CASI_OK) goto done;
    for (i = 0; i < names.len; i++) {
        uint64_t size, parked_at;
        if (suffix(names.items[i], ".meta.json")) continue;
        casi_buf_clear(&path); casi_buf_clear(&meta_path); casi_buf_clear(&meta);
        if ((rc = casi_fs_join(&path, dir, names.items[i])) != CASI_OK ||
            (rc = casi_buf_printf(&meta_path, "%s.meta.json", casi_buf_cstr(&path))) != CASI_OK)
            goto done;
        if (!regular_file(casi_buf_cstr(&path), &size) ||
            !regular_file(casi_buf_cstr(&meta_path), &size) ||
            casi_fs_read_file(casi_buf_cstr(&meta_path), &meta) != CASI_OK) {
            casi_error_clear(); (*skipped)++; continue;
        }
        parked_at = casi_json_find_uint(meta.ptr, meta.len, "parkedAt");
        if (parked_at == 0 || parked_at > now || now - parked_at < RETENTION_SECONDS) {
            (*skipped)++; continue;
        }
        if (!regular_file(casi_buf_cstr(&path), &size)) { (*skipped)++; continue; }
        (*eligible)++; *bytes += size;
        if (remove) {
            if ((rc = casi_fs_remove_file(casi_buf_cstr(&path))) != CASI_OK) {
                casi_warn("could not remove parked conflict %s", names.items[i]);
                casi_error_clear(); continue;
            }
            if ((rc = casi_fs_remove_file(casi_buf_cstr(&meta_path))) != CASI_OK) {
                casi_warn("removed parked conflict but not its metadata %s", names.items[i]);
                casi_error_clear();
            }
            (*removed)++;
        }
    }
    rc = CASI_OK;
done:
    casi_strvec_dispose(&names); casi_buf_dispose(&path);
    casi_buf_dispose(&meta_path); casi_buf_dispose(&meta);
    return rc;
}

static bool confirmed(void)
{
    char answer[16];
    if (!casi_fs_stdin_is_tty()) return false;
    fputs("Delete these local archived conflicts? [y/N] ", stderr);
    return fgets(answer, sizeof(answer), stdin) != NULL &&
           (answer[0] == 'y' || answer[0] == 'Y');
}

int casi_cmd_gc(int argc, char **argv)
{
    casi_ctx ctx;
    bool yes = argc == 1 && strcmp(argv[0], "--yes") == 0;
    uint64_t eligible, skipped, bytes, removed;
    int rc;

    if (argc > 1 || (argc == 1 && !yes))
        return casi_error_set(CASI_EINVAL, "usage: casi gc [--yes]");
    if ((rc = casi_ctx_open(&ctx)) != CASI_OK) return rc;
    casi_info("optimising the local casi store");
    if ((rc = casi_repo_gc(ctx.repo)) != CASI_OK) goto done;
    casi_info("local store optimised");
    if ((rc = conflict_plan(false, &eligible, &skipped, &bytes, &removed)) != CASI_OK) goto done;
    casi_info("conflict cleanup: %" PRIu64 " eligible, %" PRIu64
              " skipped, %" PRIu64 " bytes reclaimable", eligible, skipped, bytes);
    if (eligible == 0) goto done;
    if (!yes && !confirmed()) {
        casi_info("no conflicts removed; rerun interactively or use `casi gc --yes`");
        goto done;
    }
    if ((rc = conflict_plan(true, &eligible, &skipped, &bytes, &removed)) != CASI_OK) goto done;
    casi_info("conflict cleanup: %" PRIu64 " removed, %" PRIu64 " bytes freed", removed, bytes);
done:
    casi_ctx_dispose(&ctx);
    return rc;
}
