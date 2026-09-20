/* SPDX-License-Identifier: AGPL-3.0-only */
#include "casi/log.h"
#include "casi/fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static casi_log_level g_level = CASI_LOG_NORMAL;
static bool           g_color;
static bool           g_progress_active;

void casi_log_set_level(casi_log_level level) { g_level = level; }
casi_log_level casi_log_get_level(void)       { return g_level; }
void casi_log_set_color(bool enabled)         { g_color = enabled; }
bool casi_log_color(void)                     { return g_color; }

void casi_log_detect_color(void)
{
    const char *term = getenv("TERM");

    /* https://no-color.org: any non-empty value disables colour. */
    if (getenv("NO_COLOR") != NULL) {
        g_color = false;
        return;
    }
    if (term != NULL && strcmp(term, "dumb") == 0) {
        g_color = false;
        return;
    }

    g_color = casi_fs_stdout_is_tty();
}

static void emit(FILE *out, const char *prefix, const char *fmt, va_list ap)
{
    if (prefix != NULL)
        fputs(prefix, out);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
}

#define LOG_BODY(stream, minlevel, prefix)      \
    va_list ap;                                 \
    if (g_level < (minlevel))                   \
        return;                                 \
    va_start(ap, fmt);                          \
    emit((stream), (prefix), fmt, ap);          \
    va_end(ap)

void casi_info(const char *fmt, ...)    { LOG_BODY(stdout, CASI_LOG_NORMAL,  NULL); }
void casi_verbose(const char *fmt, ...) { LOG_BODY(stdout, CASI_LOG_VERBOSE, NULL); }
void casi_debug(const char *fmt, ...)   { LOG_BODY(stdout, CASI_LOG_DEBUG,   NULL); }

void casi_warn(const char *fmt, ...)
{
    LOG_BODY(stderr, CASI_LOG_NORMAL,
             g_color ? "\033[33mwarning:\033[0m " : "warning: ");
}

void casi_err(const char *fmt, ...)
{
    /* Errors survive --quiet: a silent failure is worse than noise. */
    LOG_BODY(stderr, CASI_LOG_QUIET,
             g_color ? "\033[31merror:\033[0m " : "error: ");
}

void casi_progress(const char *fmt, ...)
{
    va_list ap;

    if (g_level < CASI_LOG_NORMAL || !casi_fs_stderr_is_tty())
        return;
    va_start(ap, fmt);
    fputs("\r", stderr);
    vfprintf(stderr, fmt, ap);
    fputs("\033[K", stderr);   /* clear to end of line */
    fflush(stderr);
    va_end(ap);
    g_progress_active = true;
}

void casi_progress_done(void)
{
    if (!g_progress_active)
        return;
    fputs("\n", stderr);
    fflush(stderr);
    g_progress_active = false;
}
