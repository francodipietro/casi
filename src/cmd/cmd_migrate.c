/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"

#include <string.h>

/*
 * One-off migration helper. A previous sync tool (claude-sync) rewrote paths
 * incompletely, leaving other machines' home directories embedded in the
 * transcripts. casi matches projects by basename, so those foreign homes
 * normalise differently on each machine and re-diverge forever.
 *
 * `casi migrate <home1> [<home2> ...]` rewrites every occurrence of the given
 * foreign homes to THIS machine's home, in every transcript and sidecar, with
 * the originals backed up first. Run it once on each machine and the migration
 * is over; normal sync then works.
 */

static int rewrite_one(const char *in, size_t len, const char *from, const char *to,
                       casi_buf *out)
{
    size_t from_len = strlen(from);
    size_t i, span = 0;
    int rc;

    if (from_len == 0)
        return casi_error_set(CASI_EINVAL, "foreign home cannot be empty");

    casi_buf_clear(out);
    if ((rc = casi_buf_grow(out, len)) != CASI_OK)
        return rc;

    for (i = 0; i + from_len <= len; i++) {
        char next;

        if (memcmp(in + i, from, from_len) != 0)
            continue;
        /* Only at a path boundary: followed by '/', a closing quote, or end. */
        next = (i + from_len < len) ? in[i + from_len] : '\0';
        if (next != '/' && next != '"' && next != '\'')
            continue;

        if ((rc = casi_buf_put(out, in + span, i - span)) != CASI_OK ||
            (rc = casi_buf_puts(out, to)) != CASI_OK)
            return rc;
        i += from_len - 1;
        span = i + 1;
    }
    return casi_buf_put(out, in + span, len - span);
}

static int migrate_file(const char *path, int homes_argc, char **homes,
                        const char *local_home, const char *backup_dir)
{
    casi_buf content = CASI_BUF_INIT, cur = CASI_BUF_INIT, next = CASI_BUF_INIT;
    int rc = CASI_OK, i;

    if ((rc = casi_fs_read_file(path, &content)) != CASI_OK)
        goto done;
    if ((rc = casi_buf_set(&cur, content.ptr, content.len)) != CASI_OK)
        goto done;

    for (i = 0; i < homes_argc; i++) {
        if ((rc = rewrite_one(cur.ptr, cur.len, homes[i], local_home, &next)) != CASI_OK)
            goto done;
        casi_buf_dispose(&cur);
        cur = next;
        next = (casi_buf){ 0 };
    }

    if (cur.len == content.len && memcmp(cur.ptr, content.ptr, content.len) == 0)
        goto done;

    {
        casi_buf backup = CASI_BUF_INIT;
        size_t j;

        if ((rc = casi_fs_mkdir_p(backup_dir)) != CASI_OK)
            goto done;
        if ((rc = casi_buf_printf(&backup, "%s/", backup_dir)) != CASI_OK)
            goto done;
        for (j = 0; path[j] != '\0'; j++)
            if ((rc = casi_buf_putc(&backup, path[j] == '/' ? '-' : path[j])) != CASI_OK)
                goto done;
        if ((rc = casi_fs_write_file_atomic(casi_buf_cstr(&backup),
                                            content.ptr, content.len)) != CASI_OK)
            goto done;
        if ((rc = casi_fs_write_file_atomic(path, cur.ptr, cur.len)) != CASI_OK)
            goto done;
        casi_info("migrated %s", path);
        casi_buf_dispose(&backup);
    }

done:
    casi_buf_dispose(&content);
    casi_buf_dispose(&cur);
    casi_buf_dispose(&next);
    return rc;
}

int casi_cmd_migrate(int argc, char **argv)
{
    casi_ctx ctx;
    casi_session_list sessions;
    casi_aux_file_list aux;
    casi_buf backup_dir = CASI_BUF_INIT;
    const char *local_home;
    size_t i;
    int rc;

    if (argc < 1)
        return casi_error_set(CASI_EINVAL, "usage: %s",
                              casi_command_lookup("migrate")->usage);

    for (i = 0; i < (size_t)argc; i++)
        if (argv[i][0] == '\0')
            return casi_error_set(CASI_EINVAL, "foreign home cannot be empty");

    if ((local_home = casi_fs_home()) == NULL)
        return casi_error_set(CASI_ERROR, "cannot resolve the home directory");

    if ((rc = casi_ctx_open(&ctx)) != CASI_OK)
        return rc;

    if ((rc = casi_buf_printf(&backup_dir, "%s/migrate-backup",
                              casi_paths_data_dir())) != CASI_OK)
        goto done;

    memset(&sessions, 0, sizeof(sessions));
    memset(&aux, 0, sizeof(aux));
    if ((rc = ctx.provider->discover(ctx.roots, &sessions, &aux)) != CASI_OK)
        goto done;

    for (i = 0; i < sessions.len; i++)
        if ((rc = migrate_file(sessions.items[i].local_path, argc, argv, local_home,
                               casi_buf_cstr(&backup_dir))) != CASI_OK)
            goto done;
    for (i = 0; i < aux.len; i++)
        if ((rc = migrate_file(aux.items[i].local_path, argc, argv, local_home,
                               casi_buf_cstr(&backup_dir))) != CASI_OK)
            goto done;

    casi_info("migration complete; originals backed up at %s",
              casi_buf_cstr(&backup_dir));
    rc = CASI_OK;

done:
    casi_session_list_dispose(&sessions);
    casi_aux_file_list_dispose(&aux);
    casi_buf_dispose(&backup_dir);
    casi_ctx_dispose(&ctx);
    return rc;
}
