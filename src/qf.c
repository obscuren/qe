// SPDX-License-Identifier: GPL-3.0-or-later
#include "qf.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse one line of rg --vimgrep output: path:line:col:text
   Falls back to grep -n format:            path:line:text     */
static int parse_line(const char *line, QfEntry *e) {
    /* Strip trailing newline. */
    char buf[1024];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf)-1] = '\0';
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    if (n == 0) return 0;

    /* Find first colon that is NOT part of a Windows drive letter (pos > 1). */
    /* Format: path:line:col:text  or  path:line:text */
    char *p = buf;

    /* path: scan to first ':' at offset > 1 */
    char *colon1 = NULL;
    for (int i = 1; buf[i]; i++) {
        if (buf[i] == ':') { colon1 = &buf[i]; break; }
    }
    if (!colon1) return 0;
    *colon1 = '\0';
    strncpy(e->path, p, sizeof(e->path) - 1);
    p = colon1 + 1;

    /* line number */
    char *colon2 = strchr(p, ':');
    if (!colon2) return 0;
    *colon2 = '\0';
    e->line = atoi(p);
    if (e->line <= 0) return 0;
    p = colon2 + 1;

    /* Check if next field is a column number (all digits before next ':').
       If so, skip it; otherwise the rest is the text. */
    char *colon3 = strchr(p, ':');
    if (colon3) {
        int all_digits = 1;
        for (char *c = p; c < colon3; c++)
            if (*c < '0' || *c > '9') { all_digits = 0; break; }
        if (all_digits) {
            e->col = atoi(p);
            p = colon3 + 1;
        } else {
            e->col = 1;
        }
    } else {
        e->col = 1;
    }

    /* Rest is match text — trim leading whitespace. */
    while (*p == ' ' || *p == '\t') p++;
    strncpy(e->text, p, sizeof(e->text) - 1);
    e->text[sizeof(e->text)-1] = '\0';
    return 1;
}

void qf_run(QfList *ql, const char *pattern, const char *path) {
    if (!ql->entries)
        ql->entries = malloc(sizeof(QfEntry) * QF_MAX);
    if (!ql->entries) return;
    ql->count    = 0;
    ql->selected = 0;
    strncpy(ql->pattern, pattern, sizeof(ql->pattern) - 1);

    char qpat[1024], qpath[1024];
    shell_quote(pattern, qpat,  sizeof(qpat));
    shell_quote(path ? path : ".", qpath, sizeof(qpath));

    /* Try rg first; fall back to grep -rn. */
    char cmd[5120];
    snprintf(cmd, sizeof(cmd),
             "rg --vimgrep --no-heading -e %s %s 2>/dev/null"
             " || grep -rn --with-filename -- %s %s 2>/dev/null",
             qpat, qpath, qpat, qpath);

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp) && ql->count < QF_MAX) {
        QfEntry e;
        memset(&e, 0, sizeof(e));
        if (parse_line(line, &e))
            ql->entries[ql->count++] = e;
    }
    pclose(fp);
}

void qf_render_to_buf(QfList *ql, Buffer *buf) {
    /* Clear existing rows. */
    buf_clear_rows(buf);

    /* One plain-text row per entry (used for navigation / cursor clamping).
       Visual rendering is overridden in draw_pane_rows with colours. */
    for (int i = 0; i < ql->count; i++) {
        QfEntry *e = &ql->entries[i];
        char row[800];
        int  len = snprintf(row, sizeof(row), "%s:%d: %s",
                            e->path, e->line, e->text);
        buf_insert_row(buf, buf->numrows, row, len);
    }
    buf->dirty = 0;
}

void qf_free(QfList *ql) {
    free(ql->entries);
    memset(ql, 0, sizeof(*ql));
}
