// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UNDOFILE_H
#define UNDOFILE_H

#include "undo.h"

/* Write tree to .qe/undo/<hash>.undo — returns 0 on success */
int  undofile_save(const char *filepath, const UndoTree *tree);

/* Read tree from .qe/undo/<hash>.undo — returns 0 on success */
int  undofile_load(const char *filepath, UndoTree *tree);

/* Delete the undo file for a given filepath */
void undofile_remove(const char *filepath);

#endif
