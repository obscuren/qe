// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TREE_H
#define TREE_H

#include "buf.h"

#define TREE_WIDTH       30
#define TREE_MAX_ENTRIES 4096
#define TREE_PATH_MAX    512
#define TREE_NAME_MAX    256

typedef struct {
    char path[TREE_PATH_MAX];   /* full absolute path */
    char name[TREE_NAME_MAX];   /* basename only      */
    int  is_dir;
    int  depth;
    int  expanded;
    char git_status;            /* ' '=clean, 'M'=modified, '?'=untracked,
                                   'A'=added, 'D'=deleted                  */
} TreeEntry;

typedef struct {
    char      root[TREE_PATH_MAX];
    TreeEntry entries[TREE_MAX_ENTRIES];
    int       count;
    int       show_hidden;
} TreeState;

/* Rebuild entries from disk (preserves expanded states). */
void tree_refresh(TreeState *ts);

/* Toggle expand/collapse for entry at idx (0-based into ts->entries). */
void tree_toggle(TreeState *ts, int idx);

/* Render ts into buf rows (clears previous rows; preserves buf->filename). */
void tree_render_to_buf(const TreeState *ts, Buffer *buf);

/* Populate git_status for every entry using `git status --porcelain`.
   Also propagates status up to parent directories. */
void tree_update_git_status(TreeState *ts);

#endif
