/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/provider.h"
#include "casi/casi.h"
#include "casi/encoding.h"
#include "casi/jsonl.h"

#include <stdlib.h>
#include <string.h>

/*
 * Claude Code stores one session per file:
 *
 *   ~/.claude/projects/<encoded-cwd>/<session-uuid>.jsonl
 *   ~/.claude/projects/<encoded-cwd>/<session-uuid>/subagents/...
 *   ~/.claude/projects/<encoded-cwd>/memory/ (markdown notes)
 *
 * Transcripts, per-session subagents, and per-project memory are discovered
 * separately because only the transcript participates in append comparison.
 */

/* How much of a transcript to read when all that is wanted is the project it
 * belongs to. The first record carries `cwd`, and transcripts reach hundreds
 * of megabytes, so reading the whole file to identify it would make `casi
 * status` unusable. */
#define IDENTIFY_PREFIX_BYTES (64 * 1024)

static int projects_dir(casi_buf *out)
{
    const char *home = casi_paths_claude_home();
    int rc;

    if (home == NULL)
        return casi_error_last_code();

    casi_buf_clear(out);
    if ((rc = casi_buf_puts(out, home)) != CASI_OK)
        return rc;

    return casi_fs_join(out, "", "projects");
}

/*
 * The real project path for a transcript.
 *
 * Read from the file's own `cwd` field rather than decoded from the directory
 * name: that encoding replaces every non-alphanumeric character with '-' and
 * cannot be inverted. The *first* record is what counts -- `cwd` moves as the
 * session runs, so later records reflect wherever it wandered to.
 */
static int project_path_of(const char *jsonl_path, casi_buf *out)
{
    casi_buf head = CASI_BUF_INIT;
    int rc;

    if ((rc = casi_fs_read_file_prefix(jsonl_path, IDENTIFY_PREFIX_BYTES, &head)) != CASI_OK)
        goto done;

    if (!casi_jsonl_first_string(casi_buf_cstr(&head), head.len, "cwd", out))
        rc = casi_error_set(CASI_ENOTFOUND,
                            "no cwd recorded in %s", jsonl_path);

done:
    casi_buf_dispose(&head);
    return rc;
}

static bool is_session_file(const char *name)
{
    /* "<uuid>.jsonl" at the top of a project directory. */
    return casi_str_has_suffix(name, ".jsonl");
}

static bool is_subagent_file(const char *name)
{
    return casi_str_has_prefix(name, "agent-") &&
           (casi_str_has_suffix(name, ".jsonl") ||
            casi_str_has_suffix(name, ".meta.json"));
}

static int add_aux_file(casi_aux_file_list *out, casi_aux_kind kind,
                        const casi_session *session, const char *path,
                        const char *name)
{
    casi_aux_file file;
    int rc;

    memset(&file, 0, sizeof(file));
    file.kind = kind;
    file.local_path = casi_strdup(path);
    file.project_path = casi_strdup(session->project_path);
    file.project_id = casi_strdup(session->project_id);
    file.session_id = kind == CASI_AUX_SUBAGENT ? casi_strdup(session->session_id) : NULL;
    file.name = casi_strdup(name);
    if (file.local_path == NULL || file.project_path == NULL ||
        file.project_id == NULL || file.name == NULL ||
        (kind == CASI_AUX_SUBAGENT && file.session_id == NULL)) {
        free(file.local_path);
        free(file.project_path);
        free(file.project_id);
        free(file.session_id);
        free(file.name);
        return casi_error_set(CASI_ENOMEM, "out of memory listing auxiliary file");
    }

    rc = casi_aux_file_list_push(out, &file);
    if (rc != CASI_OK) {
        free(file.local_path);
        free(file.project_path);
        free(file.project_id);
        free(file.session_id);
        free(file.name);
    }
    return rc;
}

static int discover_subagents(const char *project_dir, const casi_session *session,
                              casi_aux_file_list *out)
{
    casi_buf dir = CASI_BUF_INIT, path = CASI_BUF_INIT;
    casi_strvec names = CASI_STRVEC_INIT;
    size_t i;
    int rc;

    if ((rc = casi_buf_printf(&dir, "%s/%s/subagents", project_dir,
                              session->session_id)) != CASI_OK)
        goto done;
    rc = casi_fs_listdir(casi_buf_cstr(&dir), &names);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    for (i = 0; i < names.len; i++) {
        casi_stat st;

        if (!is_subagent_file(names.items[i]))
            continue;
        casi_buf_clear(&path);
        if ((rc = casi_fs_join(&path, casi_buf_cstr(&dir), names.items[i])) != CASI_OK)
            goto done;
        if ((rc = casi_fs_stat(casi_buf_cstr(&path), &st)) != CASI_OK)
            goto done;
        if (!st.is_dir &&
            (rc = add_aux_file(out, CASI_AUX_SUBAGENT, session,
                               casi_buf_cstr(&path), names.items[i])) != CASI_OK)
            goto done;
    }

    rc = CASI_OK;
done:
    casi_strvec_dispose(&names);
    casi_buf_dispose(&dir);
    casi_buf_dispose(&path);
    return rc;
}

static int discover_memory(const char *project_dir, const casi_session *session,
                           casi_aux_file_list *out)
{
    casi_buf dir = CASI_BUF_INIT, path = CASI_BUF_INIT;
    casi_strvec names = CASI_STRVEC_INIT;
    size_t i;
    int rc;

    if ((rc = casi_buf_printf(&dir, "%s/memory", project_dir)) != CASI_OK)
        goto done;
    rc = casi_fs_listdir(casi_buf_cstr(&dir), &names);
    if (rc == CASI_ENOTFOUND) {
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    for (i = 0; i < names.len; i++) {
        casi_stat st;

        casi_buf_clear(&path);
        if ((rc = casi_fs_join(&path, casi_buf_cstr(&dir), names.items[i])) != CASI_OK)
            goto done;
        if ((rc = casi_fs_stat(casi_buf_cstr(&path), &st)) != CASI_OK)
            goto done;
        if (!st.is_dir &&
            (rc = add_aux_file(out, CASI_AUX_MEMORY, session,
                               casi_buf_cstr(&path), names.items[i])) != CASI_OK)
            goto done;
    }

    rc = CASI_OK;
done:
    casi_strvec_dispose(&names);
    casi_buf_dispose(&dir);
    casi_buf_dispose(&path);
    return rc;
}

static int discover_one_project(casi_roots *roots, const char *projects,
                                const char *dir_name, casi_session_list *out,
                                casi_aux_file_list *aux_out)
{
    casi_buf dir = CASI_BUF_INIT, file = CASI_BUF_INIT;
    casi_buf raw_path = CASI_BUF_INIT, canonical = CASI_BUF_INIT, pid = CASI_BUF_INIT;
    casi_strvec entries = CASI_STRVEC_INIT;
    size_t i, first_session;
    int rc;

    if ((rc = casi_fs_join(&dir, projects, dir_name)) != CASI_OK)
        goto done;
    if ((rc = casi_fs_listdir(casi_buf_cstr(&dir), &entries)) != CASI_OK)
        goto done;

    first_session = out->len;

    for (i = 0; i < entries.len; i++) {
        casi_session session;

        if (!is_session_file(entries.items[i]))
            continue;

        casi_buf_clear(&file);
        if ((rc = casi_fs_join(&file, casi_buf_cstr(&dir), entries.items[i])) != CASI_OK)
            goto done;

        /* A transcript with no cwd yet -- an empty session, or one killed
         * before its first record -- is skipped rather than fatal. */
        if (project_path_of(casi_buf_cstr(&file), &raw_path) != CASI_OK) {
            casi_verbose("skipping %s: %s", entries.items[i], casi_error_last());
            casi_error_clear();
            continue;
        }

        /* Register the project as an auto root keyed by its basename, then
         * normalise through the (now complete) root table. This is what lets
         * two machines with different layouts agree on one canonical name
         * without either declaring a root by hand. */
        if ((rc = casi_roots_canonicalize_project(roots, casi_buf_cstr(&raw_path),
                                                  &canonical)) != CASI_OK)
            goto done;
        if ((rc = casi_project_id(casi_buf_cstr(&canonical), &pid)) != CASI_OK)
            goto done;

        memset(&session, 0, sizeof(session));
        session.session_id  = casi_strndup(entries.items[i],
                                           strlen(entries.items[i]) - strlen(".jsonl"));
        session.local_path  = casi_strdup(casi_buf_cstr(&file));
        session.project_path = casi_strdup(casi_buf_cstr(&canonical));
        session.project_id  = casi_strdup(casi_buf_cstr(&pid));

        if (session.session_id == NULL || session.local_path == NULL ||
            session.project_path == NULL || session.project_id == NULL) {
            free(session.session_id);
            free(session.local_path);
            free(session.project_path);
            free(session.project_id);
            rc = CASI_ENOMEM;
            goto done;
        }

        if ((rc = casi_session_list_push(out, &session)) != CASI_OK) {
            free(session.session_id);
            free(session.local_path);
            free(session.project_path);
            free(session.project_id);
            goto done;
        }
        if ((rc = discover_subagents(casi_buf_cstr(&dir), &session, aux_out)) != CASI_OK)
            goto done;
    }

    /* A Claude project directory has no invertible name. A transcript is the
     * authoritative source of its path, so memory with no transcript yet is
     * intentionally left local until Claude has written that first record. */
    if (out->len > first_session &&
        (rc = discover_memory(casi_buf_cstr(&dir), &out->items[first_session], aux_out)) != CASI_OK)
        goto done;

    rc = CASI_OK;

done:
    casi_strvec_dispose(&entries);
    casi_buf_dispose(&dir);
    casi_buf_dispose(&file);
    casi_buf_dispose(&raw_path);
    casi_buf_dispose(&canonical);
    casi_buf_dispose(&pid);
    return rc;
}

static int claude_discover(casi_roots *roots, casi_session_list *out,
                           casi_aux_file_list *aux_out)
{
    casi_buf projects = CASI_BUF_INIT;
    casi_strvec dirs = CASI_STRVEC_INIT;
    size_t i;
    int rc;

    if ((rc = projects_dir(&projects)) != CASI_OK)
        goto done;

    rc = casi_fs_listdir(casi_buf_cstr(&projects), &dirs);
    if (rc == CASI_ENOTFOUND) {
        /* No Claude Code data on this machine at all: an empty inventory, not
         * an error. Pulling onto a fresh machine has to work. */
        casi_error_clear();
        rc = CASI_OK;
        goto done;
    }
    if (rc != CASI_OK)
        goto done;

    for (i = 0; i < dirs.len; i++) {
        casi_buf full = CASI_BUF_INIT;

        rc = casi_fs_join(&full, casi_buf_cstr(&projects), dirs.items[i]);
        if (rc == CASI_OK && casi_fs_is_dir(casi_buf_cstr(&full)))
            rc = discover_one_project(roots, casi_buf_cstr(&projects),
                                      dirs.items[i], out, aux_out);
        casi_buf_dispose(&full);

        if (rc != CASI_OK)
            goto done;
    }

done:
    casi_strvec_dispose(&dirs);
    casi_buf_dispose(&projects);
    return rc;
}

static int claude_project_dir_for(const casi_roots *roots, const char *project_path,
                                  casi_buf *out)
{
    casi_buf projects = CASI_BUF_INIT, local = CASI_BUF_INIT, encoded = CASI_BUF_INIT;
    int rc;

    if ((rc = projects_dir(&projects)) != CASI_OK)
        goto done;

    /* Canonical path back to this machine's layout, then re-encoded with the
     * provider's own rule. The origin machine's directory name is never
     * reused: it encodes *its* paths, not ours. */
    if ((rc = casi_roots_denormalize_path(roots, project_path, &local)) != CASI_OK)
        goto done;
    if ((rc = casi_encode_project_dir(casi_buf_cstr(&local), &encoded)) != CASI_OK)
        goto done;

    casi_buf_clear(out);
    if ((rc = casi_buf_puts(out, casi_buf_cstr(&projects))) != CASI_OK)
        goto done;
    rc = casi_fs_join(out, "", casi_buf_cstr(&encoded));

done:
    casi_buf_dispose(&projects);
    casi_buf_dispose(&local);
    casi_buf_dispose(&encoded);
    return rc;
}

static int claude_local_path_for(const casi_roots *roots, const char *project_path,
                                 const char *session_id, casi_buf *out)
{
    int rc;

    if ((rc = claude_project_dir_for(roots, project_path, out)) != CASI_OK)
        return rc;
    if ((rc = casi_fs_join(out, "", session_id)) != CASI_OK)
        return rc;
    return casi_buf_puts(out, ".jsonl");
}

static int claude_aux_local_path_for(const casi_roots *roots, casi_aux_kind kind,
                                     const char *project_path, const char *session_id,
                                     const char *name, casi_buf *out)
{
    int rc;

    if ((rc = claude_project_dir_for(roots, project_path, out)) != CASI_OK)
        return rc;
    if (kind == CASI_AUX_SUBAGENT) {
        if (session_id == NULL)
            return casi_error_set(CASI_EINVAL, "subagent without a session id");
        if ((rc = casi_fs_join(out, "", session_id)) != CASI_OK ||
            (rc = casi_fs_join(out, "", "subagents")) != CASI_OK)
            return rc;
    } else if (kind == CASI_AUX_MEMORY) {
        if ((rc = casi_fs_join(out, "", "memory")) != CASI_OK)
            return rc;
    } else {
        return casi_error_set(CASI_EINVAL, "unknown auxiliary file kind");
    }

    return casi_fs_join(out, "", name);
}

const casi_provider casi_provider_claude_code = {
    "claude-code",
    claude_discover,
    claude_local_path_for,
    claude_aux_local_path_for
};
