// SPDX-License-Identifier: GPL-3.0-or-later
#include "undo.h"

#include <stdlib.h>
#include <string.h>

void undo_state_free(UndoState *s) {
    for (int i = 0; i < s->numrows; i++)
        free(s->row_chars[i]);
    free(s->row_chars);
    free(s->row_lens);
    s->row_chars = NULL;
    s->row_lens  = NULL;
    s->numrows   = 0;
}

void undo_push(UndoStack *stack, UndoState state) {
    if (stack->count == UNDO_MAX) {
        /* Drop the oldest entry to make room. */
        undo_state_free(&stack->entries[0]);
        memmove(&stack->entries[0], &stack->entries[1],
                sizeof(UndoState) * (UNDO_MAX - 1));
        stack->count--;
    }
    stack->entries[stack->count++] = state;
}

int undo_pop(UndoStack *stack, UndoState *out) {
    if (stack->count == 0) return -1;
    *out = stack->entries[--stack->count];
    return 0;
}

void undo_stack_clear(UndoStack *stack) {
    for (int i = 0; i < stack->count; i++)
        undo_state_free(&stack->entries[i]);
    stack->count = 0;
}
