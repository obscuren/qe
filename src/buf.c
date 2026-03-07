// SPDX-License-Identifier: GPL-3.0-or-later
#include "buf.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int col_to_vcol(const Row *row, int col, int tabwidth) {
    int vcol = 0;
    for (int i = 0; i < col && i < row->len; i++) {
        if ((unsigned char)row->chars[i] == '\t')
            vcol += tabwidth - (vcol % tabwidth);
        else
            vcol++;
    }
    return vcol;
}

int vcol_to_col(const Row *row, int vcol, int tabwidth) {
    int v = 0;
    for (int i = 0; i < row->len; i++) {
        if (v >= vcol) return i;
        if ((unsigned char)row->chars[i] == '\t')
            v += tabwidth - (v % tabwidth);
        else
            v++;
    }
    return row->len;
}

void buf_mark_hl_dirty(Buffer *b, int row) {
    if (row < b->hl_dirty_from)
        b->hl_dirty_from = row;
}

void buf_init(Buffer *b) {
    b->rows          = NULL;
    b->numrows       = 0;
    b->filename      = NULL;
    b->dirty         = 0;
    b->hl_dirty_from = INT_MAX;
}

void buf_free(Buffer *b) {
    for (int i = 0; i < b->numrows; i++) {
        free(b->rows[i].chars);
        free(b->rows[i].hl);
    }
    free(b->rows);
    free(b->filename);
    buf_init(b);
}

/* Insert a new row at position `at` initialised with s[0..len-1]. */
void buf_insert_row(Buffer *b, int at, const char *s, int len) {
    if (at < 0 || at > b->numrows) return;

    b->rows = realloc(b->rows, sizeof(Row) * (b->numrows + 1));
    memmove(&b->rows[at + 1], &b->rows[at], sizeof(Row) * (b->numrows - at));

    b->rows[at].chars          = malloc(len + 1);
    memcpy(b->rows[at].chars, s, len);
    b->rows[at].chars[len]     = '\0';
    b->rows[at].len            = len;
    b->rows[at].hl             = NULL;
    b->rows[at].hl_open_comment = 0;

    b->numrows++;
    b->dirty++;
    buf_mark_hl_dirty(b, at);
}

void buf_delete_row(Buffer *b, int at) {
    if (at < 0 || at >= b->numrows) return;
    free(b->rows[at].chars);
    free(b->rows[at].hl);
    memmove(&b->rows[at], &b->rows[at + 1],
            sizeof(Row) * (b->numrows - at - 1));
    b->numrows--;
    b->dirty++;
    buf_mark_hl_dirty(b, at);
}

/* Insert character c before position `at` in the row. */
void buf_row_insert_char(Row *row, int at, char c) {
    if (at < 0 || at > row->len) at = row->len;
    row->chars = realloc(row->chars, row->len + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->len - at + 1);
    row->chars[at] = c;
    row->len++;
}

/* Delete the character at position `at`. */
void buf_row_delete_char(Row *row, int at) {
    if (at < 0 || at >= row->len) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->len - at);
    row->len--;
}

/* Append s[0..len-1] to the end of the row. */
void buf_row_append_string(Row *row, const char *s, int len) {
    row->chars = realloc(row->chars, row->len + len + 1);
    memcpy(&row->chars[row->len], s, len);
    row->len += len;
    row->chars[row->len] = '\0';
}

/* Open a file into the buffer. Non-existent file is treated as a new file. */
int buf_open(Buffer *b, const char *filename) {
    free(b->filename);
    b->filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;  /* new file — not an error */

    char   *line = NULL;
    size_t  cap  = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &cap, fp)) != -1) {
        /* strip trailing CR/LF */
        while (linelen > 0 &&
               (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        buf_insert_row(b, b->numrows, line, (int)linelen);
    }

    free(line);
    fclose(fp);
    b->dirty = 0;   /* opening resets the dirty flag */
    return 0;
}

/* Write the buffer to its filename. Returns 0 on success, -1 on error. */
int buf_save(Buffer *b) {
    if (!b->filename) return -1;

    FILE *fp = fopen(b->filename, "w");
    if (!fp) return -1;

    for (int i = 0; i < b->numrows; i++) {
        fwrite(b->rows[i].chars, 1, b->rows[i].len, fp);
        fputc('\n', fp);
    }

    fclose(fp);
    b->dirty = 0;
    return 0;
}
