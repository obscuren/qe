// SPDX-License-Identifier: GPL-3.0-or-later
#include "tree.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

typedef struct { char name[TREE_NAME_MAX]; char full[TREE_PATH_MAX]; int is_dir; } DirEntry;

static int de_cmp(const void *a, const void *b) {
    const DirEntry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;  /* dirs first */
    return strcmp(x->name, y->name);
}

static void tree_scan(TreeState *ts, const char *path, int depth,
                      const char (*exp)[TREE_PATH_MAX], int nexp) {
    DIR *d = opendir(path);
    if (!d) return;

    DirEntry *local = malloc(sizeof(DirEntry) * 512);
    if (!local) { closedir(d); return; }
    int lcount = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && lcount < 512) {
        const char *nm = de->d_name;
        if (nm[0] == '.') {
            if (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')) continue;
            if (!ts->show_hidden) continue;
        }
        snprintf(local[lcount].full, TREE_PATH_MAX, "%s/%s", path, nm);
        snprintf(local[lcount].name, TREE_NAME_MAX, "%s", nm);
        int is_dir = 0;
        if (de->d_type == DT_DIR) {
            is_dir = 1;
        } else if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
            struct stat st;
            if (stat(local[lcount].full, &st) == 0 && S_ISDIR(st.st_mode))
                is_dir = 1;
        }
        local[lcount].is_dir = is_dir;
        lcount++;
    }
    closedir(d);

    qsort(local, lcount, sizeof(DirEntry), de_cmp);

    for (int i = 0; i < lcount && ts->count < TREE_MAX_ENTRIES; i++) {
        TreeEntry *e = &ts->entries[ts->count++];
        strncpy(e->path, local[i].full, TREE_PATH_MAX - 1);
        e->path[TREE_PATH_MAX - 1] = '\0';
        strncpy(e->name, local[i].name, TREE_NAME_MAX - 1);
        e->name[TREE_NAME_MAX - 1] = '\0';
        e->is_dir   = local[i].is_dir;
        e->depth    = depth;
        e->expanded = 0;

        if (e->is_dir) {
            for (int j = 0; j < nexp; j++) {
                if (strcmp(exp[j], e->path) == 0) { e->expanded = 1; break; }
            }
            if (e->expanded)
                tree_scan(ts, e->path, depth + 1, exp, nexp);
        }
    }
    free(local);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void tree_refresh(TreeState *ts) {
    /* Save currently expanded paths before clearing. */
    char saved[128][TREE_PATH_MAX];
    int  nsaved = 0;
    for (int i = 0; i < ts->count && nsaved < 128; i++) {
        if (ts->entries[i].is_dir && ts->entries[i].expanded)
            strncpy(saved[nsaved++], ts->entries[i].path, TREE_PATH_MAX - 1);
    }
    ts->count = 0;
    tree_scan(ts, ts->root, 0, (const char (*)[TREE_PATH_MAX])saved, nsaved);
}

void tree_toggle(TreeState *ts, int idx) {
    if (idx < 0 || idx >= ts->count) return;
    if (!ts->entries[idx].is_dir) return;
    ts->entries[idx].expanded ^= 1;
    tree_refresh(ts);
}

void tree_render_to_buf(const TreeState *ts, Buffer *buf) {
    /* Free all existing rows but preserve filename. */
    for (int i = 0; i < buf->numrows; i++) {
        free(buf->rows[i].chars);
        free(buf->rows[i].hl);
    }
    free(buf->rows);
    buf->rows          = NULL;
    buf->numrows       = 0;
    buf->dirty         = 0;
    buf->hl_dirty_from = 2147483647; /* INT_MAX without including limits.h */

    /* Root line: ▾ dirname/  (▾ = U+25BE = \xe2\x96\xbe) */
    const char *rname = strrchr(ts->root, '/');
    if (rname && rname[1]) rname++;
    else                   rname = ts->root;
    char line[TREE_PATH_MAX + 8];
    int  len = snprintf(line, sizeof(line), "\xe2\x96\xbe %s/", rname);
    buf_insert_row(buf, 0, line, len);

    /* One row per entry. */
    for (int i = 0; i < ts->count; i++) {
        const TreeEntry *e = &ts->entries[i];
        char row[TREE_PATH_MAX + 16];
        int  pos    = 0;
        int  indent = 2 + e->depth * 2;
        for (int d = 0; d < indent; d++) row[pos++] = ' ';
        if (e->is_dir) {
            /* ▾ (expanded) or ▸ (collapsed); each is 3 bytes + space = 4 bytes */
            const char *icon = e->expanded ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ";
            memcpy(row + pos, icon, 4); pos += 4;
            int nlen = (int)strlen(e->name);
            memcpy(row + pos, e->name, nlen); pos += nlen;
            row[pos++] = '/';
        } else {
            int nlen = (int)strlen(e->name);
            memcpy(row + pos, e->name, nlen); pos += nlen;
        }
        row[pos] = '\0';
        buf_insert_row(buf, buf->numrows, row, pos);
    }
    buf->dirty = 0;
}
