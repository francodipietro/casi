/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_H
#define CASI_H

#include "casi/buf.h"
#include "casi/config.h"
#include "casi/error.h"
#include "casi/fs.h"
#include "casi/log.h"
#include "casi/paths.h"
#include "casi/str.h"
#include "casi/version.h"

/* Brings up libgit2 and the process-wide state. Must bracket every use of
 * the library, including from the tests. */
int  casi_init(void);
void casi_shutdown(void);

/* Which SSH backend libgit2 was built with ("exec", "libssh2"), or NULL when
 * SSH support is missing entirely. */
const char *casi_ssh_backend(void);

/* Human-readable description of the libgit2 build behind this binary:
 * version plus which transports it can speak. Reported by `casi --version`
 * and `casi doctor`, because which SSH backend is in play decides whether
 * ~/.ssh/config aliases resolve at all. */
int casi_backend_describe(casi_buf *out);

#endif /* CASI_H */
