// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UNDO_H
#define UNDO_H

#define UNDO_MAX 100

/* Snapshot of buffer content and cursor position at a point in time. */
typedef struct {
    char **row_chars;
    int   *row_lens;
    int    numrows;
    int    cx, cy;
} UndoState;

/* Fixed-size stack of snapshots. */
typedef struct {
    UndoState entries[UNDO_MAX];
    int       count;
} UndoStack;

void undo_state_free(UndoState *s);

/* Push a state onto the stack; drops the oldest entry if full. */
void undo_push(UndoStack *stack, UndoState state);

/* Pop the top state into *out. Returns 0 on success, -1 if empty. */
int  undo_pop(UndoStack *stack, UndoState *out);

/* Free every entry and reset the stack to empty. */
void undo_stack_clear(UndoStack *stack);

#endif
