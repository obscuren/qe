// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "undo.h"
#include "editor.h"
#include "buf.h"

#include <stdlib.h>
#include <string.h>

/* ── Test helpers ────────────────────────────────────────────────────── */

static UndoState make_state(const char **lines, int numrows, int cx, int cy) {
    UndoState s;
    s.numrows   = numrows;
    s.cx        = cx;
    s.cy        = cy;
    s.row_chars = malloc(sizeof(char *) * (numrows > 0 ? numrows : 1));
    s.row_lens  = malloc(sizeof(int)    * (numrows > 0 ? numrows : 1));
    for (int i = 0; i < numrows; i++) {
        s.row_chars[i] = strdup(lines[i]);
        s.row_lens[i]  = (int)strlen(lines[i]);
    }
    return s;
}

/* ── setUp / tearDown ────────────────────────────────────────────────── */

void setUp(void) {
    E.cx = E.cy = 0;
    E.rowoff = E.coloff = 0;
    E.screenrows = 24; E.screencols = 80;
    E.mode             = MODE_NORMAL;
    E.cmdbuf[0]        = '\0';
    E.cmdlen           = 0;
    E.statusmsg[0]     = '\0';
    E.opts.line_numbers = 1;
    undo_tree_init(&E.undo_tree);
    E.has_pre_insert   = 0;
    buf_init(&E.buf);
}

void tearDown(void) {
    buf_free(&E.buf);
    undo_tree_free(&E.undo_tree);
}

/* ── undo_state_free ─────────────────────────────────────────────────── */

void test_state_free_zeroes_fields(void) {
    const char *lines[] = {"hello"};
    UndoState s = make_state(lines, 1, 0, 0);
    undo_state_free(&s);
    TEST_ASSERT_NULL(s.row_chars);
    TEST_ASSERT_NULL(s.row_lens);
    TEST_ASSERT_EQUAL_INT(0, s.numrows);
}

void test_state_free_empty_state_is_safe(void) {
    const char **no_lines = NULL;
    UndoState s = make_state(no_lines, 0, 0, 0);
    undo_state_free(&s);
    TEST_ASSERT_EQUAL_INT(0, s.numrows);
}

/* ── undo_tree basic ─────────────────────────────────────────────────── */

void test_tree_init_empty(void) {
    UndoTree t;
    undo_tree_init(&t);
    TEST_ASSERT_NULL(t.root);
    TEST_ASSERT_NULL(t.current);
    TEST_ASSERT_EQUAL_INT(0, t.total_nodes);
}

void test_tree_set_root(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *lines[] = {"root"};
    undo_tree_set_root(&t, make_state(lines, 1, 0, 0), "initial");
    TEST_ASSERT_NOT_NULL(t.root);
    TEST_ASSERT_EQUAL_PTR(t.root, t.current);
    TEST_ASSERT_EQUAL_INT(1, t.total_nodes);
    TEST_ASSERT_EQUAL_STRING("root", t.root->state.row_chars[0]);
    undo_tree_free(&t);
}

void test_tree_push_creates_child(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"root"};
    const char *b[] = {"child"};
    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "initial");
    undo_tree_push(&t, make_state(b, 1, 0, 0), "edit");

    TEST_ASSERT_EQUAL_INT(2, t.total_nodes);
    TEST_ASSERT_NOT_NULL(t.current);
    TEST_ASSERT_EQUAL_STRING("child", t.current->state.row_chars[0]);
    TEST_ASSERT_EQUAL_PTR(t.root, t.current->parent);
    TEST_ASSERT_EQUAL_INT(1, t.root->num_children);
    undo_tree_free(&t);
}

/* ── undo / redo ─────────────────────────────────────────────────────── */

void test_tree_undo(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"root"};
    const char *b[] = {"child"};
    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "initial");
    undo_tree_push(&t, make_state(b, 1, 0, 0), "edit");

    UndoNode *prev = undo_tree_undo(&t);
    TEST_ASSERT_NOT_NULL(prev);
    TEST_ASSERT_EQUAL_PTR(t.root, prev);
    TEST_ASSERT_EQUAL_STRING("root", prev->state.row_chars[0]);
    undo_tree_free(&t);
}

void test_tree_undo_at_root_returns_null(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"root"};
    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "initial");

    TEST_ASSERT_NULL(undo_tree_undo(&t));
    undo_tree_free(&t);
}

void test_tree_redo(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"root"};
    const char *b[] = {"child"};
    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "initial");
    undo_tree_push(&t, make_state(b, 1, 0, 0), "edit");
    undo_tree_undo(&t);

    UndoNode *next = undo_tree_redo(&t);
    TEST_ASSERT_NOT_NULL(next);
    TEST_ASSERT_EQUAL_STRING("child", next->state.row_chars[0]);
    undo_tree_free(&t);
}

void test_tree_redo_at_leaf_returns_null(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"root"};
    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "initial");

    TEST_ASSERT_NULL(undo_tree_redo(&t));
    undo_tree_free(&t);
}

/* ── branching ───────────────────────────────────────────────────────── */

void test_tree_branch_preserves_old_branch(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"root"};
    const char *b[] = {"branch1"};
    const char *c[] = {"branch2"};

    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "initial");
    undo_tree_push(&t, make_state(b, 1, 0, 0), "edit1");
    undo_tree_undo(&t);  /* back to root */
    undo_tree_push(&t, make_state(c, 1, 0, 0), "edit2");

    /* Root should now have 2 children (both branches preserved). */
    TEST_ASSERT_EQUAL_INT(2, t.root->num_children);
    TEST_ASSERT_EQUAL_STRING("branch1", t.root->children[0]->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("branch2", t.root->children[1]->state.row_chars[0]);
    /* Current should be on the new branch. */
    TEST_ASSERT_EQUAL_STRING("branch2", t.current->state.row_chars[0]);
    undo_tree_free(&t);
}

void test_tree_redo_follows_most_recent_branch(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"root"};
    const char *b[] = {"old"};
    const char *c[] = {"new"};

    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "initial");
    undo_tree_push(&t, make_state(b, 1, 0, 0), "old");
    undo_tree_undo(&t);
    undo_tree_push(&t, make_state(c, 1, 0, 0), "new");
    undo_tree_undo(&t);  /* back to root */

    /* Redo should follow the most recent branch (highest seq = "new"). */
    UndoNode *next = undo_tree_redo(&t);
    TEST_ASSERT_NOT_NULL(next);
    TEST_ASSERT_EQUAL_STRING("new", next->state.row_chars[0]);
    undo_tree_free(&t);
}

/* ── chronological traversal ─────────────────────────────────────────── */

void test_tree_earlier_later(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"s0"};
    const char *b[] = {"s1"};
    const char *c[] = {"s2"};

    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "s0");
    undo_tree_push(&t, make_state(b, 1, 0, 0), "s1");
    undo_tree_push(&t, make_state(c, 1, 0, 0), "s2");
    /* current = s2 (seq=2) */

    UndoNode *prev = undo_tree_earlier(&t);
    TEST_ASSERT_NOT_NULL(prev);
    TEST_ASSERT_EQUAL_STRING("s1", prev->state.row_chars[0]);

    UndoNode *next = undo_tree_later(&t);
    TEST_ASSERT_NOT_NULL(next);
    TEST_ASSERT_EQUAL_STRING("s2", next->state.row_chars[0]);
    undo_tree_free(&t);
}

/* ── flatten ─────────────────────────────────────────────────────────── */

void test_tree_flatten(void) {
    UndoTree t;
    undo_tree_init(&t);
    const char *a[] = {"s0"};
    const char *b[] = {"s1"};
    const char *c[] = {"s2"};

    undo_tree_set_root(&t, make_state(a, 1, 0, 0), "s0");
    undo_tree_push(&t, make_state(b, 1, 0, 0), "s1");
    undo_tree_push(&t, make_state(c, 1, 0, 0), "s2");

    UndoNode **arr = NULL;
    int count = undo_tree_flatten(&t, &arr);
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_EQUAL_STRING("s0", arr[0]->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("s1", arr[1]->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("s2", arr[2]->state.row_chars[0]);
    free(arr);
    undo_tree_free(&t);
}

/* ── editor capture / restore ────────────────────────────────────────── */

void test_capture_preserves_row_content(void) {
    buf_insert_row(&E.buf, 0, "line one", 8);
    buf_insert_row(&E.buf, 1, "line two", 8);
    UndoState s = editor_capture_state();
    TEST_ASSERT_EQUAL_INT(2, s.numrows);
    TEST_ASSERT_EQUAL_STRING("line one", s.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("line two", s.row_chars[1]);
    undo_state_free(&s);
}

void test_capture_preserves_cursor(void) {
    buf_insert_row(&E.buf, 0, "hello", 5);
    E.cx = 3; E.cy = 0;
    UndoState s = editor_capture_state();
    TEST_ASSERT_EQUAL_INT(3, s.cx);
    TEST_ASSERT_EQUAL_INT(0, s.cy);
    undo_state_free(&s);
}

void test_capture_empty_buffer(void) {
    UndoState s = editor_capture_state();
    TEST_ASSERT_EQUAL_INT(0, s.numrows);
    undo_state_free(&s);
}

void test_restore_restores_rows(void) {
    buf_insert_row(&E.buf, 0, "before", 6);
    UndoState s = editor_capture_state();
    buf_insert_row(&E.buf, 1, "extra", 5);
    TEST_ASSERT_EQUAL_INT(2, E.buf.numrows);
    editor_restore_state(&s);
    TEST_ASSERT_EQUAL_INT(1, E.buf.numrows);
    TEST_ASSERT_EQUAL_STRING("before", E.buf.rows[0].chars);
    undo_state_free(&s);
}

void test_restore_sets_dirty(void) {
    buf_insert_row(&E.buf, 0, "x", 1);
    UndoState s = editor_capture_state();
    E.buf.dirty = 0;
    editor_restore_state(&s);
    TEST_ASSERT_EQUAL_INT(1, E.buf.dirty);
    undo_state_free(&s);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* state free */
    RUN_TEST(test_state_free_zeroes_fields);
    RUN_TEST(test_state_free_empty_state_is_safe);

    /* tree basic */
    RUN_TEST(test_tree_init_empty);
    RUN_TEST(test_tree_set_root);
    RUN_TEST(test_tree_push_creates_child);

    /* undo / redo */
    RUN_TEST(test_tree_undo);
    RUN_TEST(test_tree_undo_at_root_returns_null);
    RUN_TEST(test_tree_redo);
    RUN_TEST(test_tree_redo_at_leaf_returns_null);

    /* branching */
    RUN_TEST(test_tree_branch_preserves_old_branch);
    RUN_TEST(test_tree_redo_follows_most_recent_branch);

    /* chronological */
    RUN_TEST(test_tree_earlier_later);

    /* flatten */
    RUN_TEST(test_tree_flatten);

    /* editor capture / restore */
    RUN_TEST(test_capture_preserves_row_content);
    RUN_TEST(test_capture_preserves_cursor);
    RUN_TEST(test_capture_empty_buffer);
    RUN_TEST(test_restore_restores_rows);
    RUN_TEST(test_restore_sets_dirty);

    return UNITY_END();
}
