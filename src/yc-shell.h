/* SPDX-License-Identifier: 0BSD */
/* Turning one line of shell script into a YcChildCreateInfo, without
 * running a shell.
 *
 * The point is to spawn the job directly when we can: 'sh -c' doubles
 * the process count and, for a short-lived job, can cost more than the
 * job itself.  So this handles the constructs that actually show up in
 * a file of commands --
 *
 *     quoting          'literal'  "with $interpolation"  back\slash
 *     $VAR, ${VAR}     from our own environment
 *     VAR=VALUE ...    leading assignments (added to the child's env)
 *     redirection      < > >> 2> 2>&1 3<file ...
 *
 * -- and nothing else.  Anything beyond that (pipelines, ;, &&, glob
 * characters, $(...), ~, backticks) is not guessed at: with
 * FALLBACK_TO_SH_C the whole line goes to 'sh -c' verbatim, and
 * without it you get YC_SHELL_ERROR_UNSUPPORTED naming the construct.
 *
 * Note that fd 0/1/2 default to MODE_INHERIT here -- shell semantics,
 * not yc_child_new()'s zeroed default of /dev/null.  A job runner that
 * wants children off the terminal should overwrite fd_infos[0] after
 * parsing.  'callbacks' and 'user_data' are left for the caller.
 */

#ifndef YC_SHELL_H_
#define YC_SHELL_H_

#include "yc-child.h"

typedef enum
{
  YC_SHELL_FLAGS_FALLBACK_TO_SH_C = (1<<0)
} YcShellFlags;

typedef enum
{
  YC_SHELL_ERROR_NONE = 0,
  YC_SHELL_ERROR_EMPTY,        /* blank, or nothing but a '#' comment */
  YC_SHELL_ERROR_NO_COMMAND,   /* assignments and/or redirections only */
  YC_SHELL_ERROR_SYNTAX,       /* broken for any shell: unclosed quote... */
  YC_SHELL_ERROR_UNSUPPORTED   /* needs a real shell (see the flag above) */
} YcShellErrorCode;

/* By-value, like YcChildCreateError: nothing to clean up. */
typedef struct YcShellError {
  YcShellErrorCode code;
  size_t position;             /* byte offset into 'cmdline' */
  char message[256];
} YcShellError;

/* NULL on failure, with *error set.  EMPTY and NO_COMMAND are reported
 * even with FALLBACK_TO_SH_C: a line with no command is a mistake in a
 * job file, not something to spend a shell on.  So is SYNTAX, which we
 * can point at a column. */
YcChildCreateInfo *yc_shell_parse(const char *cmdline,
                                  YcShellFlags flags,
                                  YcShellError *error);
void               yc_shell_free (YcChildCreateInfo *cci);

#endif
