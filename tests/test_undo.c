// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "undo.h"
#include "editor.h"
#include "buf.h"

#include <stdlib.h>
#include <string.h>

/* ── Test helpers ────────────────────────────────────────────────────── */

/* Build a minimal UndoState without going through editor_capture_state(). */
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
    /* Initialise E manually — avoids editor_init() and the TTY ioctl. */
    E.cx = E.cy = 0;
    E.rowoff = E.coloff = 0;
    E.screenrows = 24; E.screencols = 80;
    E.mode             = MODE_NORMAL;
    E.cmdbuf[0]        = '\0';
    E.cmdlen           = 0;
    E.statusmsg[0]     = '\0';
    E.opts.line_numbers = 1;
    E.undo_stack.count = 0;
    E.redo_stack.count = 0;
    E.has_pre_insert   = 0;
    buf_init(&E.buf);
}

void tearDown(void) {
    buf_free(&E.buf);
    undo_stack_clear(&E.undo_stack);
    undo_stack_clear(&E.redo_stack);
}

/* ── undo_push / undo_pop ────────────────────────────────────────────── */

void test_push_and_pop_single_entry(void) {
    const char *lines[] = {"hello"};
    UndoState   in  = make_state(lines, 1, 3, 0);
    UndoState   out;

    undo_push(&E.undo_stack, in);
    TEST_ASSERT_EQUAL_INT(1, E.undo_stack.count);
    TEST_ASSERT_EQUAL_INT(0, undo_pop(&E.undo_stack, &out));
    TEST_ASSERT_EQUAL_INT(0, E.undo_stack.count);

    TEST_ASSERT_EQUAL_INT(1,       out.numrows);
    TEST_ASSERT_EQUAL_INT(3,       out.cx);
    TEST_ASSERT_EQUAL_INT(0,       out.cy);
    TEST_ASSERT_EQUAL_STRING("hello", out.row_chars[0]);

    undo_state_free(&out);
}

void test_push_and_pop_preserves_order(void) {
    const char *a[] = {"first"};
    const char *b[] = {"second"};

    undo_push(&E.undo_stack, make_state(a, 1, 0, 0));
    undo_push(&E.undo_stack, make_state(b, 1, 0, 0));
    TEST_ASSERT_EQUAL_INT(2, E.undo_stack.count);

    UndoState out;
    undo_pop(&E.undo_stack, &out);
    TEST_ASSERT_EQUAL_STRING("second", out.row_chars[0]);
    undo_state_free(&out);

    undo_pop(&E.undo_stack, &out);
    TEST_ASSERT_EQUAL_STRING("first", out.row_chars[0]);
    undo_state_free(&out);
}

void test_pop_empty_stack_returns_minus_one(void) {
    UndoState out;
    TEST_ASSERT_EQUAL_INT(-1, undo_pop(&E.undo_stack, &out));
}

void test_push_drops_oldest_when_full(void) {
    /* Fill the stack to capacity. */
    for (int i = 0; i < UNDO_MAX; i++) {
        const char *line = (i == 0) ? "oldest" : "other";
        const char *lines[] = {line};
        undo_push(&E.undo_stack, make_state(lines, 1, 0, 0));
    }
    TEST_ASSERT_EQUAL_INT(UNDO_MAX, E.undo_stack.count);

    /* Pushing one more should drop "oldest". */
    const char *new_lines[] = {"newest"};
    undo_push(&E.undo_stack, make_state(new_lines, 1, 0, 0));
    TEST_ASSERT_EQUAL_INT(UNDO_MAX, E.undo_stack.count);

    /* The bottom entry must NOT be "oldest" any more. */
    TEST_ASSERT_NOT_EQUAL(0, strcmp(E.undo_stack.entries[0].row_chars[0], "oldest"));
    /* The top entry must be "newest". */
    TEST_ASSERT_EQUAL_STRING("newest",
        E.undo_stack.entries[E.undo_stack.count - 1].row_chars[0]);
}

/* ── undo_stack_clear ────────────────────────────────────────────────── */

void test_stack_clear_empties_stack(void) {
    const char *lines[] = {"x"};
    undo_push(&E.undo_stack, make_state(lines, 1, 0, 0));
    undo_push(&E.undo_stack, make_state(lines, 1, 0, 0));
    undo_stack_clear(&E.undo_stack);
    TEST_ASSERT_EQUAL_INT(0, E.undo_stack.count);
}

void test_stack_clear_on_empty_is_safe(void) {
    undo_stack_clear(&E.undo_stack);  /* must not crash */
    TEST_ASSERT_EQUAL_INT(0, E.undo_stack.count);
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
    undo_state_free(&s);  /* must not crash */
    TEST_ASSERT_EQUAL_INT(0, s.numrows);
}

/* ── editor_capture_state ────────────────────────────────────────────── */

void test_capture_preserves_row_content(void) {
    buf_insert_row(&E.buf, 0, "line one", 8);
    buf_insert_row(&E.buf, 1, "line two", 8);

    UndoState s = editor_capture_state();

    TEST_ASSERT_EQUAL_INT(2, s.numrows);
    TEST_ASSERT_EQUAL_STRING("line one", s.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("line two", s.row_chars[1]);
    TEST_ASSERT_EQUAL_INT(8, s.row_lens[0]);
    TEST_ASSERT_EQUAL_INT(8, s.row_lens[1]);

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

void test_capture_makes_independent_copy(void) {
    buf_insert_row(&E.buf, 0, "original", 8);
    UndoState s = editor_capture_state();

    /* Mutate the buffer after capture. */
    buf_row_insert_char(&E.buf.rows[0], 0, 'X');

    /* Snapshot must be unchanged. */
    TEST_ASSERT_EQUAL_STRING("original", s.row_chars[0]);
    undo_state_free(&s);
}

/* ── editor_restore_state ────────────────────────────────────────────── */

void test_restore_restores_rows(void) {
    buf_insert_row(&E.buf, 0, "before", 6);
    UndoState s = editor_capture_state();

    /* Change the buffer. */
    buf_insert_row(&E.buf, 1, "extra", 5);
    TEST_ASSERT_EQUAL_INT(2, E.buf.numrows);

    editor_restore_state(&s);

    TEST_ASSERT_EQUAL_INT(1, E.buf.numrows);
    TEST_ASSERT_EQUAL_STRING("before", E.buf.rows[0].chars);

    undo_state_free(&s);
}

void test_restore_restores_cursor(void) {
    buf_insert_row(&E.buf, 0, "hello", 5);
    E.cx = 2; E.cy = 0;
    UndoState s = editor_capture_state();

    E.cx = 5; E.cy = 99;
    editor_restore_state(&s);

    TEST_ASSERT_EQUAL_INT(2, E.cx);
    TEST_ASSERT_EQUAL_INT(0, E.cy);

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

void test_restore_empty_state_clears_buffer(void) {
    buf_insert_row(&E.buf, 0, "something", 9);
    UndoState s = editor_capture_state();  /* state with 1 row */

    buf_delete_row(&E.buf, 0);            /* buffer now empty */
    UndoState empty = editor_capture_state();

    editor_restore_state(&s);           /* restore 1-row state */
    TEST_ASSERT_EQUAL_INT(1, E.buf.numrows);

    editor_restore_state(&empty);       /* restore empty state */
    TEST_ASSERT_EQUAL_INT(0, E.buf.numrows);
    TEST_ASSERT_NULL(E.buf.rows);

    undo_state_free(&s);
    undo_state_free(&empty);
}

/* ── Full undo / redo flow ───────────────────────────────────────────── */

void test_undo_restores_previous_state(void) {
    buf_insert_row(&E.buf, 0, "original", 8);

    /* Simulate what enter_insert_mode() + Escape does. */
    undo_push(&E.undo_stack, editor_capture_state());

    /* Modify the buffer (simulated insert session). */
    buf_insert_row(&E.buf, 1, "added", 5);
    E.buf.dirty++;

    /* Undo. */
    UndoState prev;
    undo_pop(&E.undo_stack, &prev);
    undo_push(&E.redo_stack, editor_capture_state());
    editor_restore_state(&prev);
    undo_state_free(&prev);

    TEST_ASSERT_EQUAL_INT(1, E.buf.numrows);
    TEST_ASSERT_EQUAL_STRING("original", E.buf.rows[0].chars);
}

void test_redo_restores_undone_state(void) {
    buf_insert_row(&E.buf, 0, "original", 8);
    undo_push(&E.undo_stack, editor_capture_state());

    buf_insert_row(&E.buf, 1, "added", 5);
    E.buf.dirty++;

    /* Undo. */
    UndoState prev;
    undo_pop(&E.undo_stack, &prev);
    undo_push(&E.redo_stack, editor_capture_state());
    editor_restore_state(&prev);
    undo_state_free(&prev);

    TEST_ASSERT_EQUAL_INT(1, E.buf.numrows);

    /* Redo. */
    UndoState next;
    undo_pop(&E.redo_stack, &next);
    undo_push(&E.undo_stack, editor_capture_state());
    editor_restore_state(&next);
    undo_state_free(&next);

    TEST_ASSERT_EQUAL_INT(2, E.buf.numrows);
    TEST_ASSERT_EQUAL_STRING("added", E.buf.rows[1].chars);
}

void test_new_change_clears_redo_stack(void) {
    buf_insert_row(&E.buf, 0, "original", 8);
    undo_push(&E.undo_stack, editor_capture_state());
    buf_insert_row(&E.buf, 1, "added", 5);

    /* Undo. */
    UndoState prev;
    undo_pop(&E.undo_stack, &prev);
    undo_push(&E.redo_stack, editor_capture_state());
    editor_restore_state(&prev);
    undo_state_free(&prev);

    TEST_ASSERT_EQUAL_INT(1, E.redo_stack.count);

    /* New change clears redo. */
    undo_push(&E.undo_stack, editor_capture_state());
    undo_stack_clear(&E.redo_stack);

    TEST_ASSERT_EQUAL_INT(0, E.redo_stack.count);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* push / pop */
    RUN_TEST(test_push_and_pop_single_entry);
    RUN_TEST(test_push_and_pop_preserves_order);
    RUN_TEST(test_pop_empty_stack_returns_minus_one);
    RUN_TEST(test_push_drops_oldest_when_full);

    /* stack clear */
    RUN_TEST(test_stack_clear_empties_stack);
    RUN_TEST(test_stack_clear_on_empty_is_safe);

    /* state free */
    RUN_TEST(test_state_free_zeroes_fields);
    RUN_TEST(test_state_free_empty_state_is_safe);

    /* capture */
    RUN_TEST(test_capture_preserves_row_content);
    RUN_TEST(test_capture_preserves_cursor);
    RUN_TEST(test_capture_empty_buffer);
    RUN_TEST(test_capture_makes_independent_copy);

    /* restore */
    RUN_TEST(test_restore_restores_rows);
    RUN_TEST(test_restore_restores_cursor);
    RUN_TEST(test_restore_sets_dirty);
    RUN_TEST(test_restore_empty_state_clears_buffer);

    /* full undo/redo flow */
    RUN_TEST(test_undo_restores_previous_state);
    RUN_TEST(test_redo_restores_undone_state);
    RUN_TEST(test_new_change_clears_redo_stack);

    return UNITY_END();
}
