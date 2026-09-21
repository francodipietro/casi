/* SPDX-License-Identifier: AGPL-3.0-only */
#include "cmd/cmd.h"
#include "casi/ctx.h"
#include "casi/shared_config.h"

#include <string.h>

/* A default machine label, so `casi init` needs no arguments to be useful.
 * The hostname is what the user already calls this machine. */
static int default_machine(casi_buf *out)
{
    return casi_fs_hostname(out);
}

/* A pasted Markdown link ("[text](url)") or a URL with stray whitespace is the
 * most common first-run mistake. Reject it here, before a store or remote is
 * created, so `casi init` never announces success for a URL it never tested. */
static int validate_remote_url(const char *url)
{
    const char *p;

    if (url == NULL || url[0] == '\0')
        return casi_error_set(CASI_EINVAL, "remote URL is empty");

    if (strstr(url, "](") != NULL)
        return casi_error_set(CASI_EINVAL,
                              "this looks like a pasted Markdown link; "
                              "run `casi init --remote <url>` with the plain URL only");

    for (p = url; *p != '\0'; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            return casi_error_set(CASI_EINVAL,
                                  "remote URL contains spaces; paste the URL alone, "
                                  "without surrounding text");

    return CASI_OK;
}

int casi_cmd_init(int argc, char **argv)
{
    casi_config *cfg = NULL;
    casi_repo *repo = NULL;
    casi_buf machine = CASI_BUF_INIT, existing = CASI_BUF_INIT;
    casi_buf configured = CASI_BUF_INIT;
    const char *remote = NULL, *machine_arg = NULL, *keyfile_arg = NULL;
    bool encrypt = false;
    int i, rc;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--remote") == 0 && i + 1 < argc) {
            remote = argv[++i];
        } else if (strcmp(argv[i], "--machine") == 0 && i + 1 < argc) {
            machine_arg = argv[++i];
        } else if (strcmp(argv[i], "--encrypt") == 0) {
            encrypt = true;
        } else if (strcmp(argv[i], "--keyfile") == 0 && i + 1 < argc) {
            keyfile_arg = argv[++i];
        } else {
            rc = casi_error_set(CASI_EINVAL, "usage: %s",
                                casi_command_lookup("init")->usage);
            goto done;
        }
    }

    if ((rc = casi_config_open(&cfg)) != CASI_OK)
        goto done;

    /* Re-running init must not silently rename the machine and orphan its
     * branch, so an existing label is kept unless --machine says otherwise. */
    if (machine_arg != NULL) {
        if ((rc = casi_buf_puts(&machine, machine_arg)) != CASI_OK)
            goto done;
    } else if (casi_config_get_string(cfg, "core.machine", &existing) == CASI_OK) {
        if ((rc = casi_buf_put(&machine, existing.ptr, existing.len)) != CASI_OK)
            goto done;
    } else {
        casi_error_clear();
        if ((rc = default_machine(&machine)) != CASI_OK)
            goto done;
    }

    if ((rc = casi_repo_validate_machine_name(casi_buf_cstr(&machine))) != CASI_OK)
        goto done;

    if (keyfile_arg != NULL && !encrypt) {
        rc = casi_error_set(CASI_EINVAL, "--keyfile requires --encrypt");
        goto done;
    }
    if (encrypt && remote == NULL) {
        rc = casi_error_set(CASI_EINVAL,
                            "--encrypt requires --remote so casi can protect a new remote");
        goto done;
    }

    if (remote != NULL && (rc = validate_remote_url(remote)) != CASI_OK)
        goto done;

    if ((rc = casi_repo_init(&repo)) != CASI_OK)
        goto done;

    if (remote != NULL && (rc = casi_repo_set_remote(repo, remote)) != CASI_OK)
        goto done;

    if (encrypt) {
        const char *keyfile = keyfile_arg != NULL ? keyfile_arg : casi_paths_key_file();
        casi_crypto crypto = { 0 };
        casi_strvec machines = CASI_STRVEC_INIT;
        casi_shared_config shared = { 0 };
        git_oid config_tree;
        bool config_present = false;

        if (keyfile == NULL) {
            rc = casi_error_last_code();
            goto done;
        }
        if ((rc = casi_repo_fetch(repo)) != CASI_OK)
            goto encrypt_done;
        if ((rc = casi_repo_list_machines(repo, &machines)) != CASI_OK)
            goto encrypt_done;
        /* Joining a populated encrypted remote requires a key copied by the
         * user first. Do not manufacture an unrelated default key and then
         * strand it when the remote key-id check fails. */
        if (!casi_fs_exists(keyfile) && machines.len > 0) {
            rc = casi_error_set(CASI_EINVAL,
                                "encrypted remote requires an existing keyfile; copy it before init");
            goto encrypt_done;
        }
        rc = casi_repo_ref_tree(repo, CASI_SHARED_CONFIG_REMOTE_REF, &config_tree);
        if (rc == CASI_OK && !casi_fs_exists(keyfile)) {
            rc = casi_error_set(CASI_EINVAL,
                                "encrypted remote requires an existing keyfile; copy it before init");
            goto encrypt_done;
        }
        if (rc != CASI_OK && rc != CASI_ENOTFOUND)
            goto encrypt_done;
        casi_error_clear();
        rc = CASI_OK;
        if (!casi_fs_exists(keyfile) && (rc = casi_crypto_generate_key_file(keyfile)) != CASI_OK)
            goto encrypt_done;
        if ((rc = casi_crypto_load_key_file(keyfile, &crypto)) != CASI_OK)
            goto encrypt_done;
        if ((rc = casi_shared_config_load_remote(repo, &crypto, &shared,
                                                 &config_present)) != CASI_OK)
            goto encrypt_done;
        if (!config_present && machines.len > 0) {
            rc = casi_error_set(CASI_EINVAL,
                                "--encrypt only supports a remote with no existing casi data");
            goto encrypt_done;
        }
        /* A present configuration made it through the encrypted-header and
         * key-id checks above, so this is a new machine joining the same
         * encrypted remote rather than an unsafe migration. */
        if ((rc = casi_config_set_string(cfg, "crypto.mode", CASI_CRYPTO_MODE_CONVERGENT)) != CASI_OK ||
            (rc = casi_config_set_string(cfg, "crypto.keyfile", keyfile)) != CASI_OK)
            goto encrypt_done;
        casi_info("  encryption: enabled (key kept at %s)", keyfile);

encrypt_done:
        casi_shared_config_dispose(&shared);
        casi_crypto_dispose(&crypto);
        casi_strvec_dispose(&machines);
        if (rc != CASI_OK)
            goto done;
    } else if (remote != NULL) {
        /* A plain init used to save the URL without testing it, so a mistyped
         * or unauthenticated remote only surfaced at the first push. Verify
         * reachability now so "store ready" means what it says. */
        if ((rc = casi_repo_fetch(repo)) != CASI_OK) {
            casi_error_clear();
            rc = casi_error_set(CASI_ENETWORK,
                                "cannot reach \"%s\" -- check the URL and your SSH/HTTPS credentials",
                                remote);
            goto done;
        }
    }

    if ((rc = casi_config_set_string(cfg, "core.machine",
                                     casi_buf_cstr(&machine))) != CASI_OK ||
        (rc = casi_config_set_string(cfg, "core.provider",
                                     casi_provider_default()->name)) != CASI_OK)
        goto done;

    casi_info("casi store ready for machine \"%s\"", casi_buf_cstr(&machine));
    if (remote != NULL) {
        casi_info("  remote verified: %s", remote);
        if (!encrypt)
            casi_info("  next: `casi push` to upload this machine's sessions");
    } else if (casi_repo_remote_url(repo, &configured) == CASI_OK) {
        casi_info("  remote: %s", casi_buf_cstr(&configured));
    } else {
        casi_error_clear();
        casi_info("  no remote yet -- run `casi init --remote <url>` to connect one");
    }

    rc = CASI_OK;

done:
    casi_repo_free(repo);
    casi_config_free(cfg);
    casi_buf_dispose(&machine);
    casi_buf_dispose(&existing);
    casi_buf_dispose(&configured);
    return rc;
}
