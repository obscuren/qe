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

/* ── Node helpers ─────────────────────────────────────────────────── */

static UndoNode *node_new(UndoState state, const char *desc, unsigned int seq) {
    UndoNode *n = calloc(1, sizeof(UndoNode));
    if (!n) return NULL;
    n->state = state;
    if (desc) strncpy(n->desc, desc, UNDO_DESC_MAX - 1);
    n->seq = seq;
    return n;
}

static void node_add_child(UndoNode *parent, UndoNode *child) {
    if (parent->num_children >= parent->cap_children) {
        int newcap = parent->cap_children < 2 ? 2 : parent->cap_children * 2;
        parent->children = realloc(parent->children,
                                   sizeof(UndoNode *) * newcap);
        parent->cap_children = newcap;
    }
    child->parent = parent;
    parent->children[parent->num_children++] = child;
}

static void node_free_recursive(UndoNode *n) {
    if (!n) return;
    for (int i = 0; i < n->num_children; i++)
        node_free_recursive(n->children[i]);
    free(n->children);
    undo_state_free(&n->state);
    free(n);
}

/* ── Tree API ─────────────────────────────────────────────────────── */

void undo_tree_init(UndoTree *tree) {
    memset(tree, 0, sizeof(UndoTree));
}

void undo_tree_set_root(UndoTree *tree, UndoState state, const char *desc) {
    if (tree->root) undo_tree_free(tree);
    UndoNode *r = node_new(state, desc, tree->next_seq++);
    tree->root = r;
    tree->current = r;
    tree->total_nodes = 1;
}

UndoNode *undo_tree_push(UndoTree *tree, UndoState state, const char *desc) {
    if (!tree->root) {
        undo_tree_set_root(tree, state, desc);
        return tree->current;
    }
    UndoNode *n = node_new(state, desc, tree->next_seq++);
    if (!n) return NULL;
    node_add_child(tree->current, n);
    tree->current = n;
    tree->total_nodes++;

    if (tree->total_nodes > UNDO_TREE_MAX)
        undo_tree_gc(tree);

    return n;
}

UndoNode *undo_tree_undo(UndoTree *tree) {
    if (!tree->current || !tree->current->parent) return NULL;
    tree->current = tree->current->parent;
    return tree->current;
}

UndoNode *undo_tree_redo(UndoTree *tree) {
    if (!tree->current || tree->current->num_children == 0) return NULL;
    /* Follow the most recent branch (highest seq). */
    UndoNode *best = tree->current->children[0];
    for (int i = 1; i < tree->current->num_children; i++) {
        if (tree->current->children[i]->seq > best->seq)
            best = tree->current->children[i];
    }
    tree->current = best;
    return tree->current;
}

/* ── Chronological traversal ──────────────────────────────────────── */

/* Walk the tree collecting all nodes into a flat list. */
static void collect_nodes(UndoNode *n, UndoNode ***arr, int *count, int *cap) {
    if (!n) return;
    if (*count >= *cap) {
        *cap = (*cap < 16) ? 16 : *cap * 2;
        *arr = realloc(*arr, sizeof(UndoNode *) * (*cap));
    }
    (*arr)[(*count)++] = n;
    for (int i = 0; i < n->num_children; i++)
        collect_nodes(n->children[i], arr, count, cap);
}

static int cmp_seq(const void *a, const void *b) {
    unsigned int sa = (*(UndoNode **)a)->seq;
    unsigned int sb = (*(UndoNode **)b)->seq;
    return (sa > sb) - (sa < sb);
}

UndoNode *undo_tree_earlier(UndoTree *tree) {
    if (!tree->root || !tree->current) return NULL;
    unsigned int cur_seq = tree->current->seq;

    UndoNode **arr = NULL;
    int count = 0, cap = 0;
    collect_nodes(tree->root, &arr, &count, &cap);

    UndoNode *best = NULL;
    for (int i = 0; i < count; i++) {
        if (arr[i]->seq < cur_seq) {
            if (!best || arr[i]->seq > best->seq)
                best = arr[i];
        }
    }
    free(arr);

    if (best) tree->current = best;
    return best;
}

UndoNode *undo_tree_later(UndoTree *tree) {
    if (!tree->root || !tree->current) return NULL;
    unsigned int cur_seq = tree->current->seq;

    UndoNode **arr = NULL;
    int count = 0, cap = 0;
    collect_nodes(tree->root, &arr, &count, &cap);

    UndoNode *best = NULL;
    for (int i = 0; i < count; i++) {
        if (arr[i]->seq > cur_seq) {
            if (!best || arr[i]->seq < best->seq)
                best = arr[i];
        }
    }
    free(arr);

    if (best) tree->current = best;
    return best;
}

void undo_tree_free(UndoTree *tree) {
    node_free_recursive(tree->root);
    memset(tree, 0, sizeof(UndoTree));
}

/* ── GC ───────────────────────────────────────────────────────────── */

/* Mark nodes on the path from root to current so GC never prunes them. */
static void mark_path(UndoNode *current, UndoNode **path, int *path_len) {
    *path_len = 0;
    for (UndoNode *n = current; n; n = n->parent)
        path[(*path_len)++] = n;
}

static int on_path(UndoNode *n, UndoNode **path, int path_len) {
    for (int i = 0; i < path_len; i++)
        if (path[i] == n) return 1;
    return 0;
}

void undo_tree_gc(UndoTree *tree) {
    if (!tree->root) return;

    /* Build protected path. */
    UndoNode *path[UNDO_TREE_MAX + 1];
    int path_len = 0;
    mark_path(tree->current, path, &path_len);

    while (tree->total_nodes > UNDO_TREE_MAX) {
        /* Find the leaf with the lowest seq that's not on the protected path. */
        UndoNode **arr = NULL;
        int count = 0, cap = 0;
        collect_nodes(tree->root, &arr, &count, &cap);

        UndoNode *victim = NULL;
        for (int i = 0; i < count; i++) {
            UndoNode *n = arr[i];
            if (n->num_children > 0) continue;  /* not a leaf */
            if (on_path(n, path, path_len)) continue;
            if (!victim || n->seq < victim->seq)
                victim = n;
        }
        free(arr);

        if (!victim) break;  /* only protected nodes remain */

        /* Remove victim from parent's children array. */
        UndoNode *p = victim->parent;
        if (p) {
            for (int i = 0; i < p->num_children; i++) {
                if (p->children[i] == victim) {
                    memmove(&p->children[i], &p->children[i + 1],
                            sizeof(UndoNode *) * (p->num_children - i - 1));
                    p->num_children--;
                    break;
                }
            }
        }

        undo_state_free(&victim->state);
        free(victim->children);
        free(victim);
        tree->total_nodes--;
    }
}

int undo_tree_flatten(const UndoTree *tree, UndoNode ***out) {
    if (!tree->root) { *out = NULL; return 0; }

    UndoNode **arr = NULL;
    int count = 0, cap = 0;
    collect_nodes(tree->root, &arr, &count, &cap);
    qsort(arr, count, sizeof(UndoNode *), cmp_seq);

    *out = arr;
    return count;
}

/* ── Legacy stack API (kept for backward compat) ──────────────────── */

void undo_push(UndoStack *stack, UndoState state) {
    if (stack->count == 100) {
        undo_state_free(&stack->entries[0]);
        memmove(&stack->entries[0], &stack->entries[1],
                sizeof(UndoState) * 99);
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
