// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GIT_H
#define GIT_H

/* Per-line diff sign values (stored in Buffer.git_signs). */
#define GIT_SIGN_NONE  ' '
#define GIT_SIGN_ADD   '+'
#define GIT_SIGN_MOD   '~'
#define GIT_SIGN_DEL   '-'   /* deletion: placed on the line above (or line 0) */

/* Compute per-line diff signs for a buffer against its HEAD version.
   Writes the buffer rows to a temp file and diffs against git show HEAD:filename.
   Returns a malloc'd array of `numrows` chars, or NULL on error / not in git.
   Caller must free() the result. */
char *git_diff_signs(const char *filename, const char *const *row_chars,
                     const int *row_lens, int numrows);

/* Read the current branch name into out[outlen].
   For detached HEAD, writes the short SHA.  Returns 1 on success, 0 if not a git repo. */
int git_current_branch(char *out, int outlen);

#endif
