/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"

#include <git2.h>

#include <stdio.h>

int casi_init(void)
{
    /*
     * Line-buffer stdout unconditionally, even when it is not a tty. Without
     * this, stdout (casi_info) sits in a full buffer until exit while stderr
     * (casi_warn/casi_err) flushes immediately, so anything piped or
     * redirected -- a log file, `| less`, a cron job -- shows warnings before
     * the report they refer to, even though the code emits them after.
     * Caught by hand testing `casi status` with a pending conflict outside a
     * terminal, which is exactly how `casi sync` runs from the sync-all.sh
     * -style setup this tool is meant to replace.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (git_libgit2_init() < 0)
        return casi_error_set_git(CASI_ERROR, "cannot initialise libgit2");

    casi_log_detect_color();
    return CASI_OK;
}

void casi_shutdown(void)
{
    casi_paths_reset();
    git_libgit2_shutdown();
}

const char *casi_ssh_backend(void)
{
    const char *backend;

    if ((git_libgit2_features() & GIT_FEATURE_SSH) == 0)
        return NULL;

#if LIBGIT2_VERSION_MAJOR > 1 || \
    (LIBGIT2_VERSION_MAJOR == 1 && LIBGIT2_VERSION_MINOR >= 9)
    backend = git_libgit2_feature_backend(GIT_FEATURE_SSH);
    return (backend != NULL && backend[0] != '\0') ? backend : "unknown";
#else
    /* git_libgit2_feature_backend() arrived in 1.9. Before 1.8 there was only
     * one SSH backend, and CMake rejects the 1.8.x window outright, so
     * reaching here means libssh2. */
    (void)backend;
    return "libssh2";
#endif
}

int casi_backend_describe(casi_buf *out)
{
    int major = 0, minor = 0, rev = 0;
    int caps = git_libgit2_features();
    const char *ssh = casi_ssh_backend();
    int rc;

    git_libgit2_version(&major, &minor, &rev);

    if ((rc = casi_buf_printf(out, "libgit2 %d.%d.%d", major, minor, rev)) != CASI_OK)
        return rc;

    /*
     * The two SSH backends are not interchangeable for us. "exec" shells out
     * to the system ssh, so ~/.ssh/config aliases, known_hosts and the agent
     * all work as the user already has them configured. "libssh2" understands
     * none of that, so an alias-only host like `nemo-pc` will not resolve.
     * `casi doctor` reports which one is in play for exactly that reason.
     */
    return casi_buf_printf(out, " [%sssh: %s]",
                           (caps & GIT_FEATURE_HTTPS) ? "https, " : "",
                           ssh != NULL ? ssh : "unavailable");
}
