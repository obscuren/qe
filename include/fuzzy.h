// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FUZZY_H
#define FUZZY_H

#define FUZZY_MAX_FILES  8192
#define FUZZY_MAX_VIS    15    /* visible result rows in panel */

typedef struct {
    char  path[512];
    int   score;
    int   match_pos[256];    /* byte offsets of matched query chars in path */
    int   match_count;
} FuzzyMatch;

typedef struct {
    char        query[256];
    int         query_len;
    char      **all_files;   /* heap: all scanned paths                   */
    int         all_count;
    FuzzyMatch *matches;     /* heap: filtered + scored results            */
    int         match_count;
    int         selected;    /* highlighted result row (0-based)          */
    int         scroll;      /* first visible result row                  */
} FuzzyState;

void fuzzy_open(void);
void fuzzy_close(void);
void fuzzy_filter(void);

#endif
