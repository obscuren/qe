// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef QF_H
#define QF_H

#include "buf.h"

#define QF_MAX 2048

typedef struct {
    char path[512];
    int  line, col;
    char text[256];
} QfEntry;

typedef struct {
    QfEntry *entries;
    int      count;
    int      selected;
    char     pattern[256];  /* last search pattern (for status bar) */
} QfList;

void qf_run(QfList *ql, const char *pattern, const char *path);
void qf_render_to_buf(QfList *ql, Buffer *buf);
void qf_free(QfList *ql);

#endif
