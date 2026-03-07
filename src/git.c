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
static int parse_hunk(const char *line, int *old_start, int *old_count,
                      int *new_start, int *new_count) {
    /* Expect "@@ -" */
    const char *p = line;
    if (p[0] != '@' || p[1] != '@' || p[2] != ' ' || p[3] != '-') return 0;
    p += 4;

    /* old_start[,old_count] */
    *old_start = (int)strtol(p, (char **)&p, 10);
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

/* Apply parsed hunk headers to the new-side signs array. */
static void apply_hunks(FILE *fp, char *signs, int numrows) {
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] != '@' || line[1] != '@') continue;

        int old_start, old_count, new_start, new_count;
        if (!parse_hunk(line, &old_start, &old_count, &new_start, &new_count))
            continue;

        if (old_count == 0 && new_count > 0) {
            for (int i = new_start; i < new_start + new_count && i <= numrows; i++)
                if (i >= 1) signs[i - 1] = GIT_SIGN_ADD;
        } else if (new_count == 0 && old_count > 0) {
            int mark = new_start - 1;
            if (mark < 0) mark = 0;
            if (mark < numrows) signs[mark] = GIT_SIGN_DEL;
        } else {
            for (int i = new_start; i < new_start + new_count && i <= numrows; i++)
                if (i >= 1) signs[i - 1] = GIT_SIGN_MOD;
        }
    }
}

/* Apply parsed hunk headers to both old-side and new-side signs arrays. */
static void apply_hunks_both(FILE *fp,
                             char *old_signs, int old_numrows,
                             char *new_signs, int new_numrows) {
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] != '@' || line[1] != '@') continue;

        int old_start, old_count, new_start, new_count;
        if (!parse_hunk(line, &old_start, &old_count, &new_start, &new_count))
            continue;

        if (old_count == 0 && new_count > 0) {
            /* Pure addition in new: green on new side, mark deletion point on old. */
            for (int i = new_start; i < new_start + new_count && i <= new_numrows; i++)
                if (i >= 1) new_signs[i - 1] = GIT_SIGN_ADD;
            int mark = old_start;  /* line after which insertion happened */
            if (mark < 1) mark = 1;
            if (mark <= old_numrows) old_signs[mark - 1] = GIT_SIGN_DEL;
        } else if (new_count == 0 && old_count > 0) {
            /* Pure deletion from old: red on old side, mark deletion point on new. */
            for (int i = old_start; i < old_start + old_count && i <= old_numrows; i++)
                if (i >= 1) old_signs[i - 1] = GIT_SIGN_DEL;
            int mark = new_start - 1;
            if (mark < 0) mark = 0;
            if (mark < new_numrows) new_signs[mark] = GIT_SIGN_DEL;
        } else {
            /* Modification: yellow on both sides. */
            for (int i = old_start; i < old_start + old_count && i <= old_numrows; i++)
                if (i >= 1) old_signs[i - 1] = GIT_SIGN_MOD;
            for (int i = new_start; i < new_start + new_count && i <= new_numrows; i++)
                if (i >= 1) new_signs[i - 1] = GIT_SIGN_MOD;
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

void git_diff_signs_both(const char *filename,
                         const char *const *new_chars, const int *new_lens,
                         int new_numrows,
                         const char *const *old_chars, const int *old_lens,
                         int old_numrows,
                         char **out_new_signs, char **out_old_signs) {
    *out_new_signs = NULL;
    *out_old_signs = NULL;
    if (!filename || new_numrows <= 0 || old_numrows <= 0) return;

    /* Write both versions to temp files. */
    char tmp_new[] = "/tmp/qe_dnew_XXXXXX";
    char tmp_old[] = "/tmp/qe_dold_XXXXXX";
    int fd_new = mkstemp(tmp_new);
    int fd_old = mkstemp(tmp_old);
    if (fd_new < 0 || fd_old < 0) {
        if (fd_new >= 0) { close(fd_new); unlink(tmp_new); }
        if (fd_old >= 0) { close(fd_old); unlink(tmp_old); }
        return;
    }
    for (int i = 0; i < old_numrows; i++) {
        if (old_lens[i] > 0) write(fd_old, old_chars[i], old_lens[i]);
        write(fd_old, "\n", 1);
    }
    close(fd_old);
    for (int i = 0; i < new_numrows; i++) {
        if (new_lens[i] > 0) write(fd_new, new_chars[i], new_lens[i]);
        write(fd_new, "\n", 1);
    }
    close(fd_new);

    char q_old[256], q_new[256];
    shell_quote(tmp_old, q_old, sizeof(q_old));
    shell_quote(tmp_new, q_new, sizeof(q_new));

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "diff -U0 -- %s %s 2>/dev/null", q_old, q_new);

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmp_old); unlink(tmp_new); return; }

    char *ns = malloc(new_numrows);
    char *os = malloc(old_numrows);
    if (!ns || !os) { free(ns); free(os); pclose(fp); unlink(tmp_old); unlink(tmp_new); return; }
    memset(ns, GIT_SIGN_NONE, new_numrows);
    memset(os, GIT_SIGN_NONE, old_numrows);

    apply_hunks_both(fp, os, old_numrows, ns, new_numrows);

    pclose(fp);
    unlink(tmp_old);
    unlink(tmp_new);
    *out_new_signs = ns;
    *out_old_signs = os;
}

/* ── Commit ──────────────────────────────────────────────────────────── */

int git_commit(const char *message, char *output, int outlen) {
    if (!message || !*message) return 0;

    /* Write message to temp file to avoid shell quoting issues. */
    char msgpath[] = "/tmp/qe_cmsg_XXXXXX";
    int fd = mkstemp(msgpath);
    if (fd < 0) return 0;
    write(fd, message, strlen(message));
    close(fd);

    char qmsg[256];
    shell_quote(msgpath, qmsg, sizeof(qmsg));

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "git commit -F %s 2>&1", qmsg);

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(msgpath); return 0; }

    /* Read first line of output. */
    if (output && outlen > 0) {
        output[0] = '\0';
        if (fgets(output, outlen, fp)) {
            int len = (int)strlen(output);
            while (len > 0 && (output[len-1] == '\n' || output[len-1] == '\r'))
                output[--len] = '\0';
        }
    }

    int rc = pclose(fp);
    unlink(msgpath);
    return (rc == 0) ? 1 : 0;
}

char *git_staged_summary(void) {
    FILE *fp = popen("git diff --cached --stat 2>/dev/null", "r");
    if (!fp) return NULL;

    size_t cap = 512, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        size_t ll = strlen(line);
        if (len + ll + 1 > cap) {
            cap = (len + ll + 1) * 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        memcpy(buf + len, line, ll);
        len += ll;
    }
    pclose(fp);

    if (len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

/* ── Hunk operations ─────────────────────────────────────────────────── */

/* Helper: write buffer rows to a temp file, return fd (caller closes). */
static int write_buf_to_tmp(const char *template,
                            const char *const *chars, const int *lens,
                            int numrows, char *path, int pathlen) {
    snprintf(path, pathlen, "%s", template);
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    for (int i = 0; i < numrows; i++) {
        if (lens[i] > 0) write(fd, chars[i], lens[i]);
        write(fd, "\n", 1);
    }
    close(fd);
    return 0;
}

/* Helper: write HEAD version to a temp file via git show. */
static int write_head_to_tmp(const char *filename, char *path, int pathlen) {
    snprintf(path, pathlen, "/tmp/qe_hold_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);

    char qfile[1024], qpath[256];
    shell_quote(filename, qfile, sizeof(qfile));
    shell_quote(path, qpath, sizeof(qpath));

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "git show HEAD:%s > %s 2>/dev/null", qfile, qpath);
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}

DiffHunk *git_get_hunks(const char *filename,
                        const char *const *row_chars, const int *row_lens,
                        int numrows, int *out_count) {
    *out_count = 0;
    if (!filename || numrows <= 0) return NULL;

    char tmp_new[64] = "/tmp/qe_hnew_XXXXXX";
    char tmp_old[64];
    if (write_buf_to_tmp(tmp_new, row_chars, row_lens, numrows,
                         tmp_new, sizeof(tmp_new)) < 0)
        return NULL;
    if (write_head_to_tmp(filename, tmp_old, sizeof(tmp_old)) < 0) {
        unlink(tmp_new); return NULL;
    }

    char q_old[256], q_new[256];
    shell_quote(tmp_old, q_old, sizeof(q_old));
    shell_quote(tmp_new, q_new, sizeof(q_new));
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "diff -U0 -- %s %s 2>/dev/null", q_old, q_new);

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmp_old); unlink(tmp_new); return NULL; }

    int cap = 16, count = 0;
    DiffHunk *hunks = malloc(sizeof(DiffHunk) * cap);
    if (!hunks) { pclose(fp); unlink(tmp_old); unlink(tmp_new); return NULL; }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] != '@' || line[1] != '@') continue;
        DiffHunk h;
        if (!parse_hunk(line, &h.old_start, &h.old_count,
                        &h.new_start, &h.new_count))
            continue;
        if (count >= cap) {
            cap *= 2;
            DiffHunk *tmp = realloc(hunks, sizeof(DiffHunk) * cap);
            if (!tmp) break;
            hunks = tmp;
        }
        hunks[count++] = h;
    }
    pclose(fp);
    unlink(tmp_old);
    unlink(tmp_new);

    if (count == 0) { free(hunks); return NULL; }
    *out_count = count;
    return hunks;
}

int git_stage_hunk(const char *filename,
                   const char *const *row_chars, const int *row_lens,
                   int numrows, int hunk_idx) {
    if (!filename || numrows <= 0) return 0;

    /* Write buffer and HEAD to temp files. */
    char tmp_new[64] = "/tmp/qe_snew_XXXXXX";
    char tmp_old[64];
    if (write_buf_to_tmp(tmp_new, row_chars, row_lens, numrows,
                         tmp_new, sizeof(tmp_new)) < 0)
        return 0;
    if (write_head_to_tmp(filename, tmp_old, sizeof(tmp_old)) < 0) {
        unlink(tmp_new); return 0;
    }

    /* Get full diff with context=0 and line content. */
    char q_old[256], q_new[256];
    shell_quote(tmp_old, q_old, sizeof(q_old));
    shell_quote(tmp_new, q_new, sizeof(q_new));
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "diff -U0 -- %s %s 2>/dev/null", q_old, q_new);

    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(tmp_old); unlink(tmp_new); return 0; }

    /* Collect full diff output. */
    char *diffbuf = NULL;
    size_t diffsz = 0;
    {
        char chunk[4096];
        size_t cap = 0;
        while (fgets(chunk, sizeof(chunk), fp)) {
            size_t len = strlen(chunk);
            if (diffsz + len + 1 > cap) {
                cap = (diffsz + len + 1) * 2;
                char *tmp = realloc(diffbuf, cap);
                if (!tmp) break;
                diffbuf = tmp;
            }
            memcpy(diffbuf + diffsz, chunk, len);
            diffsz += len;
        }
        if (diffbuf) diffbuf[diffsz] = '\0';
    }
    pclose(fp);

    if (!diffbuf) { unlink(tmp_old); unlink(tmp_new); return 0; }

    /* Parse to find the target hunk and extract its lines. */
    int cur_hunk = -1;
    const char *hunk_start = NULL;
    const char *hunk_end   = NULL;

    const char *p = diffbuf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (!nl) nl = p + strlen(p);

        if (p[0] == '@' && p[1] == '@') {
            /* New hunk header — close previous hunk. */
            if (cur_hunk == hunk_idx) {
                hunk_end = p;
                break;
            }
            cur_hunk++;
            if (cur_hunk == hunk_idx) hunk_start = p;
        }

        p = (*nl) ? nl + 1 : nl;
    }
    if (cur_hunk == hunk_idx && !hunk_end) hunk_end = diffbuf + diffsz;

    int ok = 0;
    if (hunk_start && hunk_end && hunk_end > hunk_start) {
        /* Build a patch file:
           --- a/filename
           +++ b/filename
           <hunk header + lines> */
        char patch_path[] = "/tmp/qe_patch_XXXXXX";
        int pfd = mkstemp(patch_path);
        if (pfd >= 0) {
            dprintf(pfd, "--- a/%s\n+++ b/%s\n", filename, filename);
            write(pfd, hunk_start, hunk_end - hunk_start);
            close(pfd);

            char qpatch[256];
            shell_quote(patch_path, qpatch, sizeof(qpatch));
            char acmd[1024];
            snprintf(acmd, sizeof(acmd),
                     "git apply --cached --unidiff-zero %s 2>/dev/null", qpatch);
            ok = (system(acmd) == 0);
            unlink(patch_path);
        }
    }

    free(diffbuf);
    unlink(tmp_old);
    unlink(tmp_new);
    return ok;
}

/* ── Show HEAD ───────────────────────────────────────────────────────── */

char **git_show_head(const char *filename, int *out_count) {
    *out_count = 0;
    if (!filename) return NULL;

    char qfile[1024];
    shell_quote(filename, qfile, sizeof(qfile));

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "git show HEAD:%s 2>/dev/null", qfile);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    int cap = 256, count = 0;
    char **lines = malloc(sizeof(char *) * cap);
    if (!lines) { pclose(fp); return NULL; }

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(lines, sizeof(char *) * cap);
            if (!tmp) break;
            lines = tmp;
        }
        lines[count++] = strdup(line);
    }
    pclose(fp);

    if (count == 0) { free(lines); return NULL; }
    *out_count = count;
    return lines;
}

/* ── Blame ───────────────────────────────────────────────────────────── */

char **git_blame(const char *filename, int *out_count) {
    *out_count = 0;
    if (!filename) return NULL;

    char qfile[1024];
    shell_quote(filename, qfile, sizeof(qfile));

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "git blame --date=short -- %s 2>/dev/null", qfile);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    int cap = 256, count = 0;
    char **lines = malloc(sizeof(char *) * cap);
    if (!lines) { pclose(fp); return NULL; }

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline. */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* git blame format: "hash (Author  Date  lineno) content"
           Extract everything up to and including ')'. */
        char *paren = strchr(line, ')');
        if (!paren) continue;

        int prefix_len = (int)(paren - line + 1);

        /* Build a trimmed blame string: "hash Author Date" */
        char buf[128];
        int  blen = prefix_len < (int)sizeof(buf) - 1
                    ? prefix_len : (int)sizeof(buf) - 1;
        memcpy(buf, line, blen);
        buf[blen] = '\0';

        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(lines, sizeof(char *) * cap);
            if (!tmp) break;
            lines = tmp;
        }
        lines[count++] = strdup(buf);
    }
    pclose(fp);

    *out_count = count;
    return lines;
}
