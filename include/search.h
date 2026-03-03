// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SEARCH_H
#define SEARCH_H

#include "buf.h"

typedef int (*MatchFn)(const char *line, int line_len,
                       const char *pattern, int pat_len,
                       int start_col,
                       int *out_col, int *out_len);

typedef struct {
    char    pattern[256];
    int     pat_len;
    MatchFn match_fn;   /* pluggable strategy: literal, regex, fuzzy, … */
} SearchQuery;

/* Built-in strategies */
int search_match_literal(const char *line, int line_len,
                         const char *pattern, int pat_len,
                         int start_col, int *out_col, int *out_len);

/* Initialise a query with the literal strategy */
void search_query_init_literal(SearchQuery *q, const char *pattern, int pat_len);

/* Find next/prev match starting from (from_row, from_col), wrapping.
   Returns 1 and sets out_row/out_col on success, 0 if nothing found. */
int search_find_next(const SearchQuery *q, const Buffer *buf,
                     int from_row, int from_col,
                     int *out_row, int *out_col);
int search_find_prev(const SearchQuery *q, const Buffer *buf,
                     int from_row, int from_col,
                     int *out_row, int *out_col);

#endif
