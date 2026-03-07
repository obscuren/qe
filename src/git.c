// SPDX-License-Identifier: GPL-3.0-or-later
#include "git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Branch name ─────────────────────────────────────────────────────── */

/* Walk up from cwd to find the .git directory.  Returns 1 and fills
   git_dir (absolute path) on success, 0 if not inside a git repo. */
static int find_git_dir(char *git_dir, int gdlen) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) return 0;

    char *p = cwd;
    for (;;) {
        int n = snprintf(git_dir, gdlen, "%s/.git/HEAD", p);
        if (n >= gdlen) return 0;
        FILE *f = fopen(git_dir, "r");
        if (f) {
            fclose(f);
            snprintf(git_dir, gdlen, "%s/.git", p);
            return 1;
        }
        char *slash = strrchr(p, '/');
        if (!slash || slash == p) break;
        *slash = '\0';
    }
    return 0;
}

int git_current_branch(char *out, int outlen) {
    char git_dir[1024];
    if (!find_git_dir(git_dir, sizeof(git_dir))) return 0;

    char head_path[1088];
    snprintf(head_path, sizeof(head_path), "%s/HEAD", git_dir);

    FILE *f = fopen(head_path, "r");
    if (!f) return 0;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    fclose(f);

    /* Strip newline. */
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

    /* "ref: refs/heads/branchname" → branchname */
    if (strncmp(line, "ref: refs/heads/", 16) == 0) {
        snprintf(out, outlen, "%s", line + 16);
        return 1;
    }

    /* Detached HEAD — show short SHA. */
    if (len >= 7) {
        int slen = len < 8 ? len : 8;
        if (slen >= outlen) slen = outlen - 1;
        memcpy(out, line, slen);
        out[slen] = '\0';
        return 1;
    }
    return 0;
}

/* ── Diff signs ──────────────────────────────────────────────────────── */

/* Shell-quote s into out[outlen] with single quotes. */
static void shell_quote(const char *s, char *out, int outlen) {
    int j = 0;
    if (j < outlen - 1) out[j++] = '\'';
    for (int i = 0; s[i] && j < outlen - 5; i++) {
        if (s[i] == '\'') {
            out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\'';
        } else {
            out[j++] = s[i];
        }
    }
    if (j < outlen - 1) out[j++] = '\'';
    out[j] = '\0';
}

/* Parse a hunk header line: "@@ -old_start[,old_count] +new_start[,new_count] @@"
   Returns 1 on success. */
static int parse_hunk(const char *line, int *old_count, int *new_start, int *new_count) {
    /* Expect "@@ -" */
    const char *p = line;
    if (p[0] != '@' || p[1] != '@' || p[2] != ' ' || p[3] != '-') return 0;
    p += 4;

    /* old_start[,old_count] */
    /* int old_start = */ (void)strtol(p, (char **)&p, 10);
    if (*p == ',') { p++; *old_count = (int)strtol(p, (char **)&p, 10); }
    else           *old_count = 1;

    /* " +" */
    if (*p != ' ' || *(p+1) != '+') return 0;
    p += 2;

    *new_start = (int)strtol(p, (char **)&p, 10);
    if (*p == ',') { p++; *new_count = (int)strtol(p, (char **)&p, 10); }
    else           *new_count = 1;

    return 1;
}

/* Apply parsed hunk headers to the signs array. */
static void apply_hunks(FILE *fp, char *signs, int numrows) {
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] != '@' || line[1] != '@') continue;

        int old_count, new_start, new_count;
        if (!parse_hunk(line, &old_count, &new_start, &new_count)) continue;

        if (old_count == 0 && new_count > 0) {
            /* Pure addition. */
            for (int i = new_start; i < new_start + new_count && i <= numrows; i++)
                if (i >= 1) signs[i - 1] = GIT_SIGN_ADD;
        } else if (new_count == 0 && old_count > 0) {
            /* Pure deletion — mark the line just above. */
            int mark = new_start - 1;   /* new_start is line *after* deletion */
            if (mark < 0) mark = 0;
            if (mark < numrows) signs[mark] = GIT_SIGN_DEL;
        } else {
            /* Modification. */
            for (int i = new_start; i < new_start + new_count && i <= numrows; i++)
                if (i >= 1) signs[i - 1] = GIT_SIGN_MOD;
        }
    }
}

char *git_diff_signs(const char *filename, const char *const *row_chars,
                     const int *row_lens, int numrows) {
    if (!filename || numrows <= 0) return NULL;

    /* Write buffer content to a temp file. */
    char tmppath[] = "/tmp/qe_git_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return NULL;
    for (int i = 0; i < numrows; i++) {
        if (row_lens[i] > 0) write(fd, row_chars[i], row_lens[i]);
        write(fd, "\n", 1);
    }
    close(fd);

    /* Diff HEAD version against the buffer temp file.
       git show HEAD:path writes the committed version to stdout;
       diff -U0 compares it against our temp file. */
    char qfile[1024], qtmp[256];
    shell_quote(filename, qfile, sizeof(qfile));
    shell_quote(tmppath, qtmp,  sizeof(qtmp));

    char cmd[3072];
    snprintf(cmd, sizeof(cmd),
             "git show HEAD:%s 2>/dev/null | diff -U0 -- - %s 2>/dev/null",
             qfile, qtmp);

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmppath); return NULL; }

    char *signs = malloc(numrows);
    if (!signs) { pclose(fp); unlink(tmppath); return NULL; }
    memset(signs, GIT_SIGN_NONE, numrows);

    apply_hunks(fp, signs, numrows);

    pclose(fp);
    unlink(tmppath);
    return signs;
}
