/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "cmd/sync_ops.h"

#include "casi/fs.h"
#include "casi/jsonl.h"
#include "casi/paths.h"

#include <string.h>

static bool has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);

    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* A parked transcript carries a sibling `<name>.meta.json` written at park time
 * with enough context to choose a side without reopening the transcript. If the
 * sidecar is missing (an asset, or a copy parked before this existed), fall
 * back to the raw path. */
static int describe(const char *dir, const char *name)
{
    casi_buf path = CASI_BUF_INIT, meta = CASI_BUF_INIT;
    casi_buf project = CASI_BUF_INIT, origin = CASI_BUF_INIT, side = CASI_BUF_INIT;
    casi_buf sid = CASI_BUF_INIT, size = CASI_BUF_INIT, when = CASI_BUF_INIT;
    const char *origin_s, *sid_s;
    uint64_t bytes, updated_at;
    int rc = CASI_OK;

    if ((rc = casi_buf_printf(&path, "%s/%s.meta.json", dir, name)) != CASI_OK)
        goto done;
    rc = casi_fs_read_file(casi_buf_cstr(&path), &meta);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
        goto fallback;
    }
    if (rc != CASI_OK)
        goto done;

    if (!casi_json_find_string(meta.ptr, meta.len, "project", &project) ||
        !casi_json_find_string(meta.ptr, meta.len, "side", &side)) {
        rc = CASI_OK;
        goto fallback;
    }
    casi_json_find_string(meta.ptr, meta.len, "sessionId", &sid);
    casi_json_find_string(meta.ptr, meta.len, "originMachine", &origin);
    bytes = casi_json_find_uint(meta.ptr, meta.len, "bytes");
    updated_at = casi_json_find_uint(meta.ptr, meta.len, "updatedAt");
    if ((rc = casi_ops_human_size(bytes, &size)) != CASI_OK ||
        (rc = casi_ops_format_time(updated_at, &when)) != CASI_OK)
        goto done;

    sid_s = sid.len > 0 ? casi_buf_cstr(&sid) : "";
    origin_s = origin.len > 0 ? casi_buf_cstr(&origin) : "unknown machine";

    casi_info("  %s / %.*s  [%s]  from %s, %s, modified %s",
              casi_buf_cstr(&project), (int)8, sid_s, casi_buf_cstr(&side),
              origin_s, casi_buf_cstr(&size), casi_buf_cstr(&when));
    goto done;

fallback:
    casi_info("  %s/%s", dir, name);

done:
    casi_buf_dispose(&path);
    casi_buf_dispose(&meta);
    casi_buf_dispose(&project);
    casi_buf_dispose(&origin);
    casi_buf_dispose(&side);
    casi_buf_dispose(&sid);
    casi_buf_dispose(&size);
    casi_buf_dispose(&when);
    return rc;
}

int casi_cmd_conflicts(int argc, char **argv)
{
    casi_strvec names = CASI_STRVEC_INIT;
    const char *dir;
    size_t i;
    int rc;

    (void)argv;

    if (argc != 0)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("conflicts")->usage);
    if ((dir = casi_paths_conflicts_dir()) == NULL)
        return casi_error_last_code();

    rc = casi_fs_listdir(dir, &names);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
    }
    if (rc != CASI_OK)
        goto done;

    if (names.len == 0) {
        casi_info("no parked conflicts");
        goto done;
    }

    casi_info("parked conflicts:");
    for (i = 0; i < names.len; i++) {
        if (has_suffix(names.items[i], ".meta.json"))
            continue;
        if ((rc = describe(dir, names.items[i])) != CASI_OK)
            goto done;
    }

done:
    casi_strvec_dispose(&names);
    return rc;
}
