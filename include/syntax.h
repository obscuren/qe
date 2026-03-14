// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SYNTAX_H
#define SYNTAX_H

#include "buf.h"

typedef enum {
    HL_NORMAL  = 0,
    HL_COMMENT,
    HL_KEYWORD,
    HL_TYPE,
    HL_STRING,
    HL_NUMBER,
    HL_ESCAPE,         /* escape sequence inside string (\n, \t, \\, …)    */
    HL_PREPROC,        /* preprocessor directive (#include, #define, …)     */
    HL_BRACKET1,       /* rainbow bracket depth 1                          */
    HL_BRACKET2,       /* rainbow bracket depth 2                          */
    HL_BRACKET3,       /* rainbow bracket depth 3                          */
    HL_BRACKET4,       /* rainbow bracket depth 4                          */
    HL_SEARCH,         /* render-only: search match overrides syntax        */
    HL_BRACKET_MATCH,  /* render-only: matching bracket pair                */
    HL_VISUAL,         /* render-only: visual selection                     */
} HlType;

typedef struct {
    char **filetypes;        /* NULL-terminated list of extensions (no '.') */
    int    num_filetypes;
    char **keywords;         /* NULL-terminated */
    int    num_keywords;
    char **types;            /* NULL-terminated */
    int    num_types;
    char  *comment_single;   /* e.g. "//"   — NULL if none */
    char  *comment_ml_start; /* e.g. slash-star — NULL if none */
    char  *comment_ml_end;   /* e.g. star-slash — NULL if none */
} SyntaxDef;

void             syntax_register(SyntaxDef *def);
const SyntaxDef *syntax_detect(const char *filename);

/* Fill row->hl; return new open_comment state (1 = still in ML comment). */
int  syntax_highlight_row(const SyntaxDef *def, Row *row, int open_comment);

/* Light scan (no hl output): track comment state only. */
int  syntax_scan_row(const SyntaxDef *def, const Row *row, int open_comment);

/* Recompute hl_open_comment for rows[from..numrows-1] until stable. */
void syntax_update_open_comments(const SyntaxDef *def, Buffer *buf, int from);

#endif
