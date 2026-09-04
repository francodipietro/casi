/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/repo.h"
#include "casi/casi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CASI_REMOTE_NAME "origin"
#define CASI_REFSPEC_FETCH \
    "+refs/heads/casi/*:refs/remotes/" CASI_REMOTE_NAME "/casi/*"

struct casi_repo {
    git_repository *git;
};

struct casi_tree {
    casi_repo *repo;
    git_index *index;
};

/* --- open / init ------------------------------------------------------ */

static int repo_wrap(casi_repo **out, git_repository *git)
{
    casi_repo *repo = calloc(1, sizeof(*repo));

    if (repo == NULL) {
        git_repository_free(git);
        return casi_error_set(CASI_ENOMEM, "out of memory opening store");
    }
    repo->git = git;
    *out = repo;
    return CASI_OK;
}

int casi_repo_open(casi_repo **out)
{
    const char *path = casi_paths_repo();
    git_repository *git = NULL;

    if (path == NULL)
        return casi_error_last_code();

    if (!casi_fs_exists(path))
        return casi_error_set(CASI_ENOTFOUND,
                              "no casi store here yet -- run `casi init` first");

    if (git_repository_open_bare(&git, path) != 0)
        return casi_error_set_git(CASI_EIO, "cannot open store at %s", path);

    return repo_wrap(out, git);
}

int casi_repo_init(casi_repo **out)
{
    const char *path = casi_paths_repo();
    git_repository *git = NULL;

    if (path == NULL)
        return casi_error_last_code();

    if (casi_fs_exists(path))
        return casi_repo_open(out);

    if (git_repository_init(&git, path, 1 /* bare */) != 0)
        return casi_error_set_git(CASI_EIO, "cannot create store at %s", path);

    return repo_wrap(out, git);
}

void casi_repo_free(casi_repo *repo)
{
    if (repo == NULL)
        return;
    git_repository_free(repo->git);
    free(repo);
}

git_repository *casi_repo_git(casi_repo *repo)
{
    return repo->git;
}

int casi_repo_validate_machine_name(const char *machine)
{
    casi_buf refname = CASI_BUF_INIT;
    int valid = 0;
    int rc;

    /* One label owns one branch directly below casi/. Allowing '/' would
     * create nested namespaces and possible directory/file ref collisions. */
    if (machine == NULL || machine[0] == '\0' || strchr(machine, '/') != NULL)
        return casi_error_set(CASI_EINVAL,
                              "invalid machine name \"%s\": expected one valid Git ref component",
                              machine != NULL ? machine : "");

    if ((rc = casi_buf_printf(&refname, "refs/heads/casi/%s", machine)) != CASI_OK)
        goto done;

    if (git_reference_name_is_valid(&valid, casi_buf_cstr(&refname)) != 0) {
        rc = casi_error_set_git(CASI_EINVAL, "cannot validate machine name \"%s\"",
                                machine);
        goto done;
    }
    if (!valid) {
        rc = casi_error_set(CASI_EINVAL,
                            "invalid machine name \"%s\": expected one valid Git ref component",
                            machine);
        goto done;
    }

    rc = CASI_OK;

done:
    casi_buf_dispose(&refname);
    return rc;
}

/* --- objects ---------------------------------------------------------- */

int casi_repo_write_blob(casi_repo *repo, const void *data, size_t len, git_oid *out)
{
    if (git_blob_create_from_buffer(out, repo->git, data, len) != 0)
        return casi_error_set_git(CASI_EIO, "cannot write %zu bytes to the store", len);
    return CASI_OK;
}

int casi_repo_read_blob(casi_repo *repo, const git_oid *oid, casi_buf *out)
{
    git_blob *blob = NULL;
    int rc;

    if (git_blob_lookup(&blob, repo->git, oid) != 0)
        return casi_error_set_git(CASI_ENOTFOUND, "object missing from the store");

    rc = casi_buf_set(out, git_blob_rawcontent(blob), (size_t)git_blob_rawsize(blob));
    git_blob_free(blob);
    return rc;
}

int casi_repo_has_object(casi_repo *repo, const git_oid *oid)
{
    git_odb *odb = NULL;
    int found;

    if (git_repository_odb(&odb, repo->git) != 0)
        return 0;

    found = git_odb_exists(odb, oid);
    git_odb_free(odb);
    return found;
}

/* --- trees ------------------------------------------------------------ */

int casi_tree_new(casi_repo *repo, casi_tree **out)
{
    casi_tree *tree = calloc(1, sizeof(*tree));

    if (tree == NULL)
        return casi_error_set(CASI_ENOMEM, "out of memory building tree");

    if (git_index_new(&tree->index) != 0) {
        free(tree);
        return casi_error_set_git(CASI_ERROR, "cannot create in-memory index");
    }

    tree->repo = repo;
    *out = tree;
    return CASI_OK;
}

int casi_tree_add(casi_tree *tree, const char *path, const git_oid *blob)
{
    git_index_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.path = path;
    git_oid_cpy(&entry.id, blob);
    /* Always a plain non-executable file. casi stores content, never Unix
     * permissions -- there is nothing here to chmod, and a mode bit that
     * differed between machines would be a spurious diff. */
    entry.mode = GIT_FILEMODE_BLOB;

    if (git_index_add(tree->index, &entry) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot add %s to tree", path);

    return CASI_OK;
}

int casi_tree_add_text(casi_tree *tree, const char *path, const char *text)
{
    git_oid oid;
    int rc;

    if ((rc = casi_repo_write_blob(tree->repo, text, strlen(text), &oid)) != CASI_OK)
        return rc;

    return casi_tree_add(tree, path, &oid);
}

int casi_tree_write(casi_tree *tree, git_oid *tree_out)
{
    if (git_index_write_tree_to(tree_out, tree->index, tree->repo->git) != 0)
        return casi_error_set_git(CASI_EIO, "cannot write tree");
    return CASI_OK;
}

void casi_tree_free(casi_tree *tree)
{
    if (tree == NULL)
        return;
    git_index_free(tree->index);
    free(tree);
}

/* --- refs and commits ------------------------------------------------- */

int casi_repo_ref_tree(casi_repo *repo, const char *refname, git_oid *out)
{
    git_object *obj = NULL;
    git_commit *commit = NULL;

    if (git_revparse_single(&obj, repo->git, refname) != 0)
        return casi_error_set(CASI_ENOTFOUND, "no such ref: %s", refname);

    if (git_object_type(obj) != GIT_OBJECT_COMMIT) {
        git_object_free(obj);
        return casi_error_set(CASI_ERROR, "%s does not point at a commit", refname);
    }

    commit = (git_commit *)obj;
    git_oid_cpy(out, git_commit_tree_id(commit));
    git_object_free(obj);
    return CASI_OK;
}

int casi_repo_commit(casi_repo *repo, const char *refname, const git_oid *tree_oid,
                     const char *machine, const char *message, git_oid *out)
{
    git_signature *sig = NULL;
    git_tree *tree = NULL;
    git_commit *parent = NULL;
    const git_commit *parents[1];
    size_t parent_count = 0;
    git_oid parent_oid;
    int rc = CASI_OK;

    /* The machine label is the author: internal history nobody is meant to
     * read, but when something goes wrong it should say which machine wrote
     * it. The email is a placeholder -- casi has no notion of identity. */
    if (git_signature_now(&sig, machine, "casi@localhost") != 0) {
        rc = casi_error_set_git(CASI_ERROR, "cannot build commit signature");
        goto out;
    }

    if (git_tree_lookup(&tree, repo->git, tree_oid) != 0) {
        rc = casi_error_set_git(CASI_ERROR, "cannot look up tree");
        goto out;
    }

    if (git_reference_name_to_id(&parent_oid, repo->git, refname) == 0) {
        if (git_commit_lookup(&parent, repo->git, &parent_oid) != 0) {
            rc = casi_error_set_git(CASI_ERROR, "cannot look up parent commit");
            goto out;
        }
        parents[0] = parent;
        parent_count = 1;
    }

    if (git_commit_create(out, repo->git, refname, sig, sig, "UTF-8",
                          message, tree, parent_count, parents) != 0) {
        rc = casi_error_set_git(CASI_EIO, "cannot record commit on %s", refname);
        goto out;
    }

out:
    git_commit_free(parent);
    git_tree_free(tree);
    git_signature_free(sig);
    return rc;
}

int casi_repo_tree_entry_oid(casi_repo *repo, const git_oid *tree_oid,
                             const char *path, git_oid *out)
{
    git_tree *tree = NULL;
    git_tree_entry *entry = NULL;

    if (git_tree_lookup(&tree, repo->git, tree_oid) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot look up tree");

    if (git_tree_entry_bypath(&entry, tree, path) != 0) {
        git_tree_free(tree);
        return casi_error_set(CASI_ENOTFOUND, "not in the store: %s", path);
    }

    git_oid_cpy(out, git_tree_entry_id(entry));

    git_tree_entry_free(entry);
    git_tree_free(tree);
    return CASI_OK;
}

int casi_repo_tree_entry_blob(casi_repo *repo, const git_oid *tree_oid,
                              const char *path, casi_buf *out)
{
    git_tree *tree = NULL;
    git_tree_entry *entry = NULL;
    int rc;

    if (git_tree_lookup(&tree, repo->git, tree_oid) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot look up tree");

    if (git_tree_entry_bypath(&entry, tree, path) != 0) {
        git_tree_free(tree);
        return casi_error_set(CASI_ENOTFOUND, "not in the store: %s", path);
    }

    rc = casi_repo_read_blob(repo, git_tree_entry_id(entry), out);

    git_tree_entry_free(entry);
    git_tree_free(tree);
    return rc;
}

int casi_repo_tree_list(casi_repo *repo, const git_oid *tree_oid, const char *path,
                        casi_strvec *out)
{
    git_tree *root = NULL, *dir = NULL;
    git_tree_entry *entry = NULL;
    size_t i, count;
    int rc = CASI_OK;

    if (git_tree_lookup(&root, repo->git, tree_oid) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot look up tree");

    if (path == NULL || path[0] == '\0') {
        dir = root;
    } else {
        if (git_tree_entry_bypath(&entry, root, path) != 0) {
            git_tree_free(root);
            return casi_error_set(CASI_ENOTFOUND, "not in the store: %s", path);
        }
        if (git_tree_lookup(&dir, repo->git, git_tree_entry_id(entry)) != 0) {
            git_tree_entry_free(entry);
            git_tree_free(root);
            return casi_error_set(CASI_ENOTFOUND, "not a directory: %s", path);
        }
    }

    count = git_tree_entrycount(dir);
    for (i = 0; i < count; i++) {
        const git_tree_entry *e = git_tree_entry_byindex(dir, i);

        if ((rc = casi_strvec_push(out, git_tree_entry_name(e))) != CASI_OK)
            break;
    }

    /* git already stores entries in sorted order, but say so explicitly:
     * every machine must walk the tree in the same sequence. */
    if (rc == CASI_OK)
        casi_strvec_sort(out);

    if (dir != root)
        git_tree_free(dir);
    git_tree_entry_free(entry);
    git_tree_free(root);
    return rc;
}

/* --- transport -------------------------------------------------------- */

int casi_repo_set_remote(casi_repo *repo, const char *url)
{
    git_remote *remote = NULL;

    if (git_remote_lookup(&remote, repo->git, CASI_REMOTE_NAME) == 0) {
        git_remote_free(remote);
        if (git_remote_set_url(repo->git, CASI_REMOTE_NAME, url) != 0)
            return casi_error_set_git(CASI_ERROR, "cannot set remote url");
        return CASI_OK;
    }

    if (git_remote_create(&remote, repo->git, CASI_REMOTE_NAME, url) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot add remote %s", url);

    git_remote_free(remote);
    return CASI_OK;
}

int casi_repo_remote_url(casi_repo *repo, casi_buf *out)
{
    git_remote *remote = NULL;
    const char *url;
    int rc;

    if (git_remote_lookup(&remote, repo->git, CASI_REMOTE_NAME) != 0)
        return casi_error_set(CASI_ENOTFOUND,
                              "no remote configured -- run `casi init --remote <url>`");

    url = git_remote_url(remote);
    rc = casi_buf_set(out, url != NULL ? url : "", url != NULL ? strlen(url) : 0);
    git_remote_free(remote);
    return rc;
}

/*
 * Credentials. The exec SSH backend never reaches here -- it hands the
 * connection to the system ssh, which uses the user's agent and config as
 * always. This exists for the libssh2 backend, which distribution packages
 * commonly link (Homebrew's libgit2 does).
 */
static int credential_cb(git_credential **out, const char *url,
                         const char *username_from_url,
                         unsigned int allowed_types, void *payload)
{
    (void)url;
    (void)payload;

    if (allowed_types & GIT_CREDENTIAL_SSH_KEY)
        return git_credential_ssh_key_from_agent(
            out, username_from_url != NULL ? username_from_url : "git");

    if (allowed_types & GIT_CREDENTIAL_DEFAULT)
        return git_credential_default_new(out);

    /* No username/password path: casi never prompts for or stores a secret.
     * For HTTPS remotes the user should use a credential helper via a URL
     * that carries a token, or use SSH. */
    /* Do not use git_error_set_str() here: it is an internal libgit2 API
     * whose header is absent from the supported 1.7 development package.
     * Returning GIT_EAUTH lets libgit2 retain the transport error, which
     * transport_error() reports to the user. */
    return GIT_EAUTH;
}

/* Base64 in OpenSSH's fingerprint style: standard alphabet, no padding. */
static void base64_no_pad(const unsigned char *in, size_t len, char *out, size_t out_cap)
{
    static const char abc[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;

    while (i < len && o + 4 < out_cap) {
        unsigned int v = in[i] << 16;
        size_t have = 1;

        if (i + 1 < len) { v |= in[i + 1] << 8; have++; }
        if (i + 2 < len) { v |= in[i + 2];      have++; }

        out[o++] = abc[(v >> 18) & 0x3F];
        out[o++] = abc[(v >> 12) & 0x3F];
        if (have > 1) out[o++] = abc[(v >> 6) & 0x3F];
        if (have > 2) out[o++] = abc[v & 0x3F];

        i += 3;
    }
    out[o] = '\0';
}

/*
 * Host key verification for the libssh2 backend.
 *
 * libgit2 exposes no known_hosts support, so without this callback the only
 * options are trusting every host blindly or refusing SSH entirely. Neither is
 * acceptable, so casi asks OpenSSH itself: `ssh-keygen -l -F <host>` prints the
 * SHA-256 fingerprints already trusted for that host, including entries stored
 * under hashed hostnames, and honours the user's real known_hosts files.
 *
 * The exec backend never gets here; ssh does this itself.
 */
static int hostkey_is_known(const git_cert_hostkey *key, const char *host)
{
    char want[64];

    if ((key->type & GIT_CERT_SSH_SHA256) == 0)
        return 0;

    base64_no_pad(key->hash_sha256, sizeof(key->hash_sha256), want, sizeof(want));
    return casi_fs_ssh_hostkey_is_known(host, want) ? 1 : 0;
}

static int certificate_cb(git_cert *cert, int valid, const char *host, void *payload)
{
    (void)payload;

    if (cert->cert_type == GIT_CERT_HOSTKEY_LIBSSH2) {
        if (hostkey_is_known((const git_cert_hostkey *)cert, host))
            return 0;

        /* See credential_cb(): libgit2 1.7 does not expose its error setter
         * publicly. GIT_ECERTIFICATE still makes the transport fail closed. */
        return GIT_ECERTIFICATE;
    }

    /* TLS is validated by the TLS stack itself; respect its verdict. */
    return valid ? 0 : GIT_ECERTIFICATE;
}

static void init_callbacks(git_remote_callbacks *cb)
{
    git_remote_init_callbacks(cb, GIT_REMOTE_CALLBACKS_VERSION);
    cb->credentials = credential_cb;
    cb->certificate_check = certificate_cb;
}

/* Turns a libgit2 transport failure into casi's network/auth code, so the
 * process exits 4 rather than a generic 1. */
static int transport_error(const char *what)
{
    return casi_error_set_git(CASI_ENETWORK, "%s", what);
}

int casi_repo_fetch(casi_repo *repo)
{
    git_remote *remote = NULL;
    git_fetch_options opts;
    char *spec = (char *)CASI_REFSPEC_FETCH;
    git_strarray refspecs = { &spec, 1 };
    int rc = CASI_OK;

    if (git_remote_lookup(&remote, repo->git, CASI_REMOTE_NAME) != 0)
        return casi_error_set(CASI_ENOTFOUND,
                              "no remote configured -- run `casi init --remote <url>`");

    git_fetch_options_init(&opts, GIT_FETCH_OPTIONS_VERSION);
    init_callbacks(&opts.callbacks);
    /* Nothing here is a working tree, so there are no tags worth chasing. */
    opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
    /* Drop remote-tracking refs for branches that vanished upstream. */
    opts.prune = GIT_FETCH_PRUNE;

    if (git_remote_fetch(remote, &refspecs, &opts, NULL) != 0)
        rc = transport_error("cannot fetch from the remote");

    git_remote_free(remote);
    return rc;
}

int casi_repo_push(casi_repo *repo, const char *refname)
{
    git_remote *remote = NULL;
    git_push_options opts;
    casi_buf spec = CASI_BUF_INIT;
    char *specs[1];
    git_strarray refspecs = { specs, 1 };
    int rc = CASI_OK;

    if (git_remote_lookup(&remote, repo->git, CASI_REMOTE_NAME) != 0)
        return casi_error_set(CASI_ENOTFOUND,
                              "no remote configured -- run `casi init --remote <url>`");

    /* No leading '+': each machine owns its branch, so a push that would not
     * fast-forward means something is wrong and should be reported, not forced. */
    if ((rc = casi_buf_printf(&spec, "%s:%s", refname, refname)) != CASI_OK)
        goto out;

    specs[0] = spec.ptr;

    git_push_options_init(&opts, GIT_PUSH_OPTIONS_VERSION);
    init_callbacks(&opts.callbacks);

    if (git_remote_push(remote, &refspecs, &opts) != 0)
        rc = transport_error("cannot push to the remote");

out:
    casi_buf_dispose(&spec);
    git_remote_free(remote);
    return rc;
}

int casi_repo_list_machines(casi_repo *repo, casi_strvec *out)
{
    git_reference_iterator *iter = NULL;
    git_reference *ref = NULL;
    static const char prefix[] = "refs/remotes/" CASI_REMOTE_NAME "/casi/";
    int rc = CASI_OK;

    if (git_reference_iterator_new(&iter, repo->git) != 0)
        return casi_error_set_git(CASI_ERROR, "cannot list refs");

    while (git_reference_next(&ref, iter) == 0) {
        const char *name = git_reference_name(ref);

        if (casi_str_has_prefix(name, prefix))
            rc = casi_strvec_push(out, name + sizeof(prefix) - 1);

        git_reference_free(ref);
        if (rc != CASI_OK)
            break;
    }

    git_reference_iterator_free(iter);
    if (rc == CASI_OK)
        casi_strvec_sort(out);
    return rc;
}
