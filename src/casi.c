/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/casi.h"

#include <git2.h>

int casi_init(void)
{
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

    backend = git_libgit2_feature_backend(GIT_FEATURE_SSH);
    return (backend != NULL && backend[0] != '\0') ? backend : "unknown";
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
