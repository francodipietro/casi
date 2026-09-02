/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <git2/errors.h>

/* casi is a single-threaded CLI: one process-wide slot is enough. */
static char g_msg[1024];
static int  g_code;

static void error_vset(int code, const char *fmt, va_list ap)
{
    g_code = code;
    if (fmt == NULL) {
        g_msg[0] = '\0';
        return;
    }
    vsnprintf(g_msg, sizeof(g_msg), fmt, ap);
}

int casi_error_set(int code, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    error_vset(code, fmt, ap);
    va_end(ap);

    return code;
}

int casi_error_set_git(int code, const char *fmt, ...)
{
    const git_error *ge;
    va_list ap;
    size_t used;

    va_start(ap, fmt);
    error_vset(code, fmt, ap);
    va_end(ap);

    ge = git_error_last();
    if (ge == NULL || ge->message == NULL || ge->message[0] == '\0')
        return code;

    used = strlen(g_msg);
    if (used + 3 < sizeof(g_msg))
        snprintf(g_msg + used, sizeof(g_msg) - used, ": %s", ge->message);

    return code;
}

const char *casi_error_last(void)
{
    return g_msg[0] != '\0' ? g_msg : "unknown error";
}

int casi_error_last_code(void)
{
    return g_code;
}

void casi_error_clear(void)
{
    g_msg[0] = '\0';
    g_code = CASI_OK;
}

int casi_exit_code(int rc)
{
    switch (rc) {
    case CASI_OK:        return CASI_EXIT_OK;
    case CASI_EINVAL:    return CASI_EXIT_USAGE;
    case CASI_ECONFLICT: return CASI_EXIT_CONFLICT;
    case CASI_ENETWORK:  return CASI_EXIT_NETWORK;
    case CASI_EUNMAPPED: return CASI_EXIT_UNMAPPED;
    default:             return CASI_EXIT_ERROR;
    }
}
