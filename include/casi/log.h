/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_LOG_H
#define CASI_LOG_H

#include <stdbool.h>

#if defined(__GNUC__) || defined(__clang__)
#define CASI_PRINTF_FORMAT(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define CASI_PRINTF_FORMAT(format_index, first_arg)
#endif

typedef enum {
    CASI_LOG_QUIET   = 0,  /* errors only                          */
    CASI_LOG_NORMAL  = 1,  /* the default                          */
    CASI_LOG_VERBOSE = 2,  /* -v: per-session detail               */
    CASI_LOG_DEBUG   = 3   /* -vv: object ids, transport internals */
} casi_log_level;

void           casi_log_set_level(casi_log_level level);
casi_log_level casi_log_get_level(void);

/* Auto-detected from isatty()/NO_COLOR/TERM at init; --no-color overrides. */
void casi_log_set_color(bool enabled);
bool casi_log_color(void);
void casi_log_detect_color(void);

/* Progress and results go to stdout; anything the user must notice goes to
 * stderr, so `casi status --porcelain | ...` stays clean. */
void casi_info(const char *fmt, ...) CASI_PRINTF_FORMAT(1, 2);
void casi_verbose(const char *fmt, ...) CASI_PRINTF_FORMAT(1, 2);
void casi_debug(const char *fmt, ...) CASI_PRINTF_FORMAT(1, 2);
void casi_warn(const char *fmt, ...) CASI_PRINTF_FORMAT(1, 2);
void casi_err(const char *fmt, ...) CASI_PRINTF_FORMAT(1, 2);

#undef CASI_PRINTF_FORMAT

#endif /* CASI_LOG_H */
