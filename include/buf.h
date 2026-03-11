// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BUF_H
#define BUF_H

#include <stdint.h>

typedef struct {
    char          *chars;
    int            len;
    unsigned char *hl;             /* per-char HlType; NULL until syntax active */
    int            hl_open_comment;/* 1 if multi-line comment open at row start */
} Row;

typedef struct {
    Row   *rows;
    int    numrows;
    char  *filename;
    int    dirty;           /* non-zero when there are unsaved changes */
    int    hl_dirty_from;   /* rows >= this need hl_open_comment recomputed */
    char  *git_signs;       /* per-row diff sign (GIT_SIGN_*), or NULL      */
    int    git_signs_count; /* length of git_signs array                    */
    int8_t *folds;          /* per-row: 1 = fold closed at this row         */
    int     folds_cap;      /* allocated size of folds array                */
} Buffer;

void buf_init(Buffer *b);
void buf_clear_rows(Buffer *b);
void buf_free(Buffer *b);

void buf_insert_row(Buffer *b, int at, const char *s, int len);
void buf_delete_row(Buffer *b, int at);

void buf_row_insert_char(Row *row, int at, char c);
void buf_row_delete_char(Row *row, int at);
void buf_row_append_string(Row *row, const char *s, int len);

int  buf_open(Buffer *b, const char *filename);
int  buf_save(Buffer *b);

void buf_mark_hl_dirty(Buffer *b, int row);

/* Convert byte column → visual column (tabs expand to tabwidth stops). */
int col_to_vcol(const Row *row, int col, int tabwidth);

/* Convert visual column → nearest byte column. */
int vcol_to_col(const Row *row, int vcol, int tabwidth);

/* Fold helpers. */
int  buf_row_indent(const Row *row, int tabwidth);
int  buf_fold_end(const Buffer *buf, int start, int tabwidth);
void buf_ensure_folds(Buffer *buf);

#endif
