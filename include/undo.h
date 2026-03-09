// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UNDO_H
#define UNDO_H

#define UNDO_TREE_MAX  200
#define UNDO_DESC_MAX   64

/* Snapshot of buffer content and cursor position at a point in time. */
typedef struct {
    char **row_chars;
    int   *row_lens;
    int    numrows;
    int    cx, cy;
} UndoState;

/* A node in the undo tree. Each node owns its snapshot. */
typedef struct UndoNode {
    UndoState         state;
    char              desc[UNDO_DESC_MAX];
    unsigned int      seq;           /* monotonic sequence number            */
    struct UndoNode  *parent;
    struct UndoNode **children;
    int               num_children;
    int               cap_children;
} UndoNode;

/* The undo tree replaces the old linear undo/redo stacks. */
typedef struct {
    UndoNode    *root;           /* initial state (file open)              */
    UndoNode    *current;        /* node matching the live buffer          */
    unsigned int next_seq;       /* monotonic counter for new nodes        */
    int          total_nodes;    /* live node count (for GC threshold)     */
} UndoTree;

void undo_state_free(UndoState *s);

/* ── Tree API ─────────────────────────────────────────────────────── */

/* Initialise an empty tree (zero everything). */
void      undo_tree_init(UndoTree *tree);

/* Create the root node with the given state. */
void      undo_tree_set_root(UndoTree *tree, UndoState state, const char *desc);

/* Add a new child of current, make it current. */
UndoNode *undo_tree_push(UndoTree *tree, UndoState state, const char *desc);

/* Move current to parent. Returns new current, or NULL if at root. */
UndoNode *undo_tree_undo(UndoTree *tree);

/* Move current to most-recent child. Returns new current, or NULL if leaf. */
UndoNode *undo_tree_redo(UndoTree *tree);

/* Chronological: move to the node with the highest seq < current. */
UndoNode *undo_tree_earlier(UndoTree *tree);

/* Chronological: move to the node with the lowest seq > current. */
UndoNode *undo_tree_later(UndoTree *tree);

/* Free entire tree (all nodes and their snapshots). */
void      undo_tree_free(UndoTree *tree);

/* GC: prune oldest leaf nodes not on the current path until within budget. */
void      undo_tree_gc(UndoTree *tree);

/* Flatten tree into a seq-sorted array of node pointers.
   Returns count; caller must free(*out) (the array, not the nodes). */
int       undo_tree_flatten(const UndoTree *tree, UndoNode ***out);

/* ── Legacy compatibility (used by test_undo until migrated) ──────── */
typedef struct {
    UndoState entries[100];
    int       count;
} UndoStack;

void undo_push(UndoStack *stack, UndoState state);
int  undo_pop(UndoStack *stack, UndoState *out);
void undo_stack_clear(UndoStack *stack);

#endif
