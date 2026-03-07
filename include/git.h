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

/* Compute diff signs for both old (HEAD) and new (working) sides simultaneously.
   Writes both to temp files, diffs them, and fills both sign arrays.
   Caller must free both *out_new_signs and *out_old_signs. */
void git_diff_signs_both(const char *filename,
                         const char *const *new_chars, const int *new_lens,
                         int new_numrows,
                         const char *const *old_chars, const int *old_lens,
                         int old_numrows,
                         char **out_new_signs, char **out_old_signs);

/* Retrieve the HEAD version of a file via `git show HEAD:<filename>`.
   Returns a malloc'd array of line strings (one per line, no trailing newline).
   Sets *out_count to the number of lines.  Returns NULL on error.
   Caller must free each string and the array. */
char **git_show_head(const char *filename, int *out_count);

/* Run `git blame --date=short` and return an array of blame-prefix strings
   (one per source line: "abcdef12 Author     2024-01-15").
   Returns NULL on error.  Caller must free each string and the array. */
char **git_blame(const char *filename, int *out_count);

/* Stage a file via `git add`.  Returns 1 on success. */
int git_add(const char *filename);

/* Run `git commit` with the given message.  Returns 1 on success.
   output[] receives the first line of git's output (for status message). */
int git_commit(const char *message, char *output, int outlen);

/* Return a malloc'd string with `git diff --cached --stat` output, or NULL. */
char *git_staged_summary(void);

/* A single diff hunk (line ranges are 1-based). */
typedef struct {
    int old_start, old_count;  /* range in old (HEAD) file */
    int new_start, new_count;  /* range in new (working) file */
} DiffHunk;

/* Get all diff hunks between HEAD and the in-memory buffer.
   Returns malloc'd array of DiffHunk; caller must free().
   Sets *out_count.  Returns NULL on error or no diff. */
DiffHunk *git_get_hunks(const char *filename,
                        const char *const *row_chars, const int *row_lens,
                        int numrows, int *out_count);

/* Stage (add to index) a single hunk from the buffer.
   hunk_idx is an index into the array returned by git_get_hunks.
   Returns 1 on success, 0 on failure. */
int git_stage_hunk(const char *filename,
                   const char *const *row_chars, const int *row_lens,
                   int numrows, int hunk_idx);

#endif
