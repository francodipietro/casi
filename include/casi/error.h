/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_ERROR_H
#define CASI_ERROR_H

/*
 * Error model: functions return 0 on success and a negative casi_result on
 * failure, having first recorded a human-readable message via
 * casi_error_set(). Callers propagate the code upward unchanged; only the
 * command layer turns it into an exit status via casi_exit_code().
 *
 * casi_error_set() returns its own code argument so call sites can write:
 *     return casi_error_set(CASI_EIO, "cannot read %s", path);
 */

typedef enum {
    CASI_OK        =  0,
    CASI_ERROR     = -1,  /* unclassified failure                       */
    CASI_ENOTFOUND = -2,  /* the thing asked for does not exist         */
    CASI_EEXISTS   = -3,  /* it already exists and that is a problem    */
    CASI_EINVAL    = -4,  /* bad argument or malformed input            */
    CASI_EIO       = -5,  /* filesystem or syscall failure              */
    CASI_ENOMEM    = -6,  /* allocation failure                         */
    CASI_ECONFLICT = -7,  /* a session diverged between machines        */
    CASI_ENETWORK  = -8,  /* transport or authentication failure        */
    CASI_EUNMAPPED = -9,  /* remote references a root this machine lacks*/
    CASI_ERETRY    = -10  /* optimistic shared-state update lost a race */
} casi_result;

/* Exit statuses, as documented in the CLI contract. */
#define CASI_EXIT_OK        0
#define CASI_EXIT_ERROR     1
#define CASI_EXIT_USAGE     2
#define CASI_EXIT_CONFLICT  3
#define CASI_EXIT_NETWORK   4
#define CASI_EXIT_UNMAPPED  5

int         casi_error_set(int code, const char *fmt, ...);
/* Same, but appends libgit2's current error message when it has one. */
int         casi_error_set_git(int code, const char *fmt, ...);
const char *casi_error_last(void);
int         casi_error_last_code(void);
void        casi_error_clear(void);

/* Maps a casi_result onto the process exit status. */
int casi_exit_code(int rc);

#endif /* CASI_ERROR_H */
