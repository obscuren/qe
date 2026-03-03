// SPDX-License-Identifier: GPL-3.0-or-later
#include "search.h"

#include <string.h>

int search_match_literal(const char *line, int line_len,
                         const char *pattern, int pat_len,
                         int start_col, int *out_col, int *out_len) {
    if (pat_len <= 0 || pat_len > line_len) return 0;
    for (int i = start_col; i <= line_len - pat_len; i++) {
        if (memcmp(line + i, pattern, (size_t)pat_len) == 0) {
            *out_col = i;
            *out_len = pat_len;
            return 1;
        }
    }
    return 0;
}

void search_query_init_literal(SearchQuery *q, const char *pattern, int pat_len) {
    if (pat_len > (int)sizeof(q->pattern) - 1)
        pat_len = (int)sizeof(q->pattern) - 1;
    memcpy(q->pattern, pattern, (size_t)pat_len);
    q->pattern[pat_len] = '\0';
    q->pat_len  = pat_len;
    q->match_fn = search_match_literal;
}

int search_find_next(const SearchQuery *q, const Buffer *buf,
                     int from_row, int from_col,
                     int *out_row, int *out_col) {
    if (buf->numrows == 0 || q->pat_len == 0) return 0;
    int n = buf->numrows;
    for (int i = 0; i < n; i++) {
        int row   = (from_row + i) % n;
        int start = (i == 0) ? from_col + 1 : 0;
        Row *r = &buf->rows[row];
        int col, len;
        if (q->match_fn(r->chars, r->len, q->pattern, q->pat_len, start, &col, &len)) {
            *out_row = row;
            *out_col = col;
            return 1;
        }
    }
    return 0;
}

/* Find the rightmost match in line with col strictly < limit. */
static int find_last_match_before(const SearchQuery *q,
                                  const char *line, int line_len,
                                  int limit, int *out_col, int *out_len) {
    int found = 0;
    int start = 0;
    int col, len;
    while (q->match_fn(line, line_len, q->pattern, q->pat_len, start, &col, &len)) {
        if (col >= limit) break;
        *out_col = col;
        *out_len = len;
        found    = 1;
        start    = col + 1;
    }
    return found;
}

int search_find_prev(const SearchQuery *q, const Buffer *buf,
                     int from_row, int from_col,
                     int *out_row, int *out_col) {
    if (buf->numrows == 0 || q->pat_len == 0) return 0;
    int n = buf->numrows;
    for (int i = 0; i < n; i++) {
        int row = ((from_row - i) % n + n) % n;
        Row *r  = &buf->rows[row];
        /* For the starting row, only accept matches before from_col.
           For all other rows, accept any match (limit beyond line end). */
        int limit = (i == 0) ? from_col : r->len + 1;
        int col, len;
        if (find_last_match_before(q, r->chars, r->len, limit, &col, &len)) {
            *out_row = row;
            *out_col = col;
            return 1;
        }
    }
    return 0;
}
