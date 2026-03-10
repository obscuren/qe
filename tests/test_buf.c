// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Buffer buf;

void setUp(void)    { buf_init(&buf); }
void tearDown(void) { buf_free(&buf); }

/* ── buf_init ────────────────────────────────────────────────────────── */

void test_init_rows_null(void) {
    TEST_ASSERT_NULL(buf.rows);
}
void test_init_numrows_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, buf.numrows);
}
void test_init_filename_null(void) {
    TEST_ASSERT_NULL(buf.filename);
}
void test_init_dirty_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, buf.dirty);
}

/* ── buf_insert_row ──────────────────────────────────────────────────── */

void test_insert_row_into_empty(void) {
    buf_insert_row(&buf, 0, "hello", 5);
    TEST_ASSERT_EQUAL_INT(1, buf.numrows);
    TEST_ASSERT_EQUAL_STRING("hello", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(5, buf.rows[0].len);
}

void test_insert_row_increments_dirty(void) {
    buf_insert_row(&buf, 0, "a", 1);
    TEST_ASSERT_EQUAL_INT(1, buf.dirty);
}

void test_insert_row_at_end(void) {
    buf_insert_row(&buf, 0, "first",  5);
    buf_insert_row(&buf, 1, "second", 6);
    TEST_ASSERT_EQUAL_INT(2, buf.numrows);
    TEST_ASSERT_EQUAL_STRING("first",  buf.rows[0].chars);
    TEST_ASSERT_EQUAL_STRING("second", buf.rows[1].chars);
}

void test_insert_row_in_middle(void) {
    buf_insert_row(&buf, 0, "first",  5);
    buf_insert_row(&buf, 1, "third",  5);
    buf_insert_row(&buf, 1, "second", 6);
    TEST_ASSERT_EQUAL_INT(3, buf.numrows);
    TEST_ASSERT_EQUAL_STRING("first",  buf.rows[0].chars);
    TEST_ASSERT_EQUAL_STRING("second", buf.rows[1].chars);
    TEST_ASSERT_EQUAL_STRING("third",  buf.rows[2].chars);
}

void test_insert_row_empty_string(void) {
    buf_insert_row(&buf, 0, "", 0);
    TEST_ASSERT_EQUAL_INT(1, buf.numrows);
    TEST_ASSERT_EQUAL_INT(0, buf.rows[0].len);
    TEST_ASSERT_EQUAL_STRING("", buf.rows[0].chars);
}

void test_insert_row_out_of_bounds_is_noop(void) {
    buf_insert_row(&buf, 5, "nope", 4);
    TEST_ASSERT_EQUAL_INT(0, buf.numrows);
}

/* ── buf_delete_row ──────────────────────────────────────────────────── */

void test_delete_row_only_row(void) {
    buf_insert_row(&buf, 0, "hello", 5);
    buf_delete_row(&buf, 0);
    TEST_ASSERT_EQUAL_INT(0, buf.numrows);
}

void test_delete_row_increments_dirty(void) {
    buf_insert_row(&buf, 0, "a", 1);
    buf.dirty = 0;
    buf_delete_row(&buf, 0);
    TEST_ASSERT_EQUAL_INT(1, buf.dirty);
}

void test_delete_row_first(void) {
    buf_insert_row(&buf, 0, "first",  5);
    buf_insert_row(&buf, 1, "second", 6);
    buf_delete_row(&buf, 0);
    TEST_ASSERT_EQUAL_INT(1, buf.numrows);
    TEST_ASSERT_EQUAL_STRING("second", buf.rows[0].chars);
}

void test_delete_row_middle(void) {
    buf_insert_row(&buf, 0, "a", 1);
    buf_insert_row(&buf, 1, "b", 1);
    buf_insert_row(&buf, 2, "c", 1);
    buf_delete_row(&buf, 1);
    TEST_ASSERT_EQUAL_INT(2, buf.numrows);
    TEST_ASSERT_EQUAL_STRING("a", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_STRING("c", buf.rows[1].chars);
}

void test_delete_row_last(void) {
    buf_insert_row(&buf, 0, "first",  5);
    buf_insert_row(&buf, 1, "second", 6);
    buf_delete_row(&buf, 1);
    TEST_ASSERT_EQUAL_INT(1, buf.numrows);
    TEST_ASSERT_EQUAL_STRING("first", buf.rows[0].chars);
}

void test_delete_row_out_of_bounds_is_noop(void) {
    buf_insert_row(&buf, 0, "a", 1);
    buf_delete_row(&buf, 5);
    TEST_ASSERT_EQUAL_INT(1, buf.numrows);
}

/* ── buf_row_insert_char ─────────────────────────────────────────────── */

void test_row_insert_char_at_start(void) {
    buf_insert_row(&buf, 0, "bc", 2);
    buf_row_insert_char(&buf.rows[0], 0, 'a');
    TEST_ASSERT_EQUAL_STRING("abc", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(3, buf.rows[0].len);
}

void test_row_insert_char_at_end(void) {
    buf_insert_row(&buf, 0, "ab", 2);
    buf_row_insert_char(&buf.rows[0], 2, 'c');
    TEST_ASSERT_EQUAL_STRING("abc", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(3, buf.rows[0].len);
}

void test_row_insert_char_in_middle(void) {
    buf_insert_row(&buf, 0, "ac", 2);
    buf_row_insert_char(&buf.rows[0], 1, 'b');
    TEST_ASSERT_EQUAL_STRING("abc", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(3, buf.rows[0].len);
}

void test_row_insert_char_into_empty_row(void) {
    buf_insert_row(&buf, 0, "", 0);
    buf_row_insert_char(&buf.rows[0], 0, 'x');
    TEST_ASSERT_EQUAL_STRING("x", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(1, buf.rows[0].len);
}

void test_row_insert_char_out_of_bounds_clamps_to_end(void) {
    buf_insert_row(&buf, 0, "ab", 2);
    buf_row_insert_char(&buf.rows[0], 99, 'c');
    TEST_ASSERT_EQUAL_STRING("abc", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(3, buf.rows[0].len);
}

/* ── buf_row_delete_char ─────────────────────────────────────────────── */

void test_row_delete_char_first(void) {
    buf_insert_row(&buf, 0, "abc", 3);
    buf_row_delete_char(&buf.rows[0], 0);
    TEST_ASSERT_EQUAL_STRING("bc", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(2, buf.rows[0].len);
}

void test_row_delete_char_last(void) {
    buf_insert_row(&buf, 0, "abc", 3);
    buf_row_delete_char(&buf.rows[0], 2);
    TEST_ASSERT_EQUAL_STRING("ab", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(2, buf.rows[0].len);
}

void test_row_delete_char_middle(void) {
    buf_insert_row(&buf, 0, "abc", 3);
    buf_row_delete_char(&buf.rows[0], 1);
    TEST_ASSERT_EQUAL_STRING("ac", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(2, buf.rows[0].len);
}

void test_row_delete_char_out_of_bounds_is_noop(void) {
    buf_insert_row(&buf, 0, "abc", 3);
    buf_row_delete_char(&buf.rows[0], 5);
    TEST_ASSERT_EQUAL_INT(3, buf.rows[0].len);
    TEST_ASSERT_EQUAL_STRING("abc", buf.rows[0].chars);
}

void test_row_delete_char_until_empty(void) {
    buf_insert_row(&buf, 0, "a", 1);
    buf_row_delete_char(&buf.rows[0], 0);
    TEST_ASSERT_EQUAL_INT(0, buf.rows[0].len);
    TEST_ASSERT_EQUAL_STRING("", buf.rows[0].chars);
}

/* ── buf_row_append_string ───────────────────────────────────────────── */

void test_row_append_string_basic(void) {
    buf_insert_row(&buf, 0, "hello", 5);
    buf_row_append_string(&buf.rows[0], " world", 6);
    TEST_ASSERT_EQUAL_STRING("hello world", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(11, buf.rows[0].len);
}

void test_row_append_string_to_empty_row(void) {
    buf_insert_row(&buf, 0, "", 0);
    buf_row_append_string(&buf.rows[0], "hello", 5);
    TEST_ASSERT_EQUAL_STRING("hello", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(5, buf.rows[0].len);
}

void test_row_append_empty_string_is_noop(void) {
    buf_insert_row(&buf, 0, "hello", 5);
    buf_row_append_string(&buf.rows[0], "", 0);
    TEST_ASSERT_EQUAL_STRING("hello", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_INT(5, buf.rows[0].len);
}

/* ── buf_open ────────────────────────────────────────────────────────── */

void test_open_nonexistent_file_sets_filename(void) {
    buf_open(&buf, "/tmp/qe_no_such_file_xyz.txt");
    TEST_ASSERT_EQUAL_STRING("/tmp/qe_no_such_file_xyz.txt", buf.filename);
    TEST_ASSERT_EQUAL_INT(0, buf.numrows);
}

void test_open_strips_newlines(void) {
    const char *path = "/tmp/qe_test_open.txt";
    FILE *fp = fopen(path, "w");
    fputs("hello\n", fp);
    fputs("world\n", fp);
    fclose(fp);

    buf_open(&buf, path);
    TEST_ASSERT_EQUAL_INT(2, buf.numrows);
    TEST_ASSERT_EQUAL_STRING("hello", buf.rows[0].chars);
    TEST_ASSERT_EQUAL_STRING("world", buf.rows[1].chars);

    remove(path);
}

void test_open_resets_dirty(void) {
    const char *path = "/tmp/qe_test_dirty.txt";
    FILE *fp = fopen(path, "w");
    fputs("hi\n", fp);
    fclose(fp);

    buf_open(&buf, path);
    TEST_ASSERT_EQUAL_INT(0, buf.dirty);

    remove(path);
}

void test_open_empty_file(void) {
    const char *path = "/tmp/qe_test_empty.txt";
    FILE *fp = fopen(path, "w");
    fclose(fp);

    buf_open(&buf, path);
    TEST_ASSERT_EQUAL_INT(0, buf.numrows);

    remove(path);
}

/* ── buf_save ────────────────────────────────────────────────────────── */

void test_save_without_filename_returns_error(void) {
    TEST_ASSERT_EQUAL_INT(-1, buf_save(&buf));
}

void test_save_resets_dirty(void) {
    const char *path = "/tmp/qe_test_save_dirty.txt";
    buf_insert_row(&buf, 0, "hi", 2);
    buf.filename = strdup(path);

    buf_save(&buf);
    TEST_ASSERT_EQUAL_INT(0, buf.dirty);

    remove(path);
}

void test_save_and_reload(void) {
    const char *path = "/tmp/qe_test_roundtrip.txt";
    buf_insert_row(&buf, 0, "line one", 8);
    buf_insert_row(&buf, 1, "line two", 8);
    buf.filename = strdup(path);

    TEST_ASSERT_EQUAL_INT(0, buf_save(&buf));

    Buffer buf2;
    buf_init(&buf2);
    buf_open(&buf2, path);

    TEST_ASSERT_EQUAL_INT(2, buf2.numrows);
    TEST_ASSERT_EQUAL_STRING("line one", buf2.rows[0].chars);
    TEST_ASSERT_EQUAL_STRING("line two", buf2.rows[1].chars);
    TEST_ASSERT_EQUAL_INT(0, buf2.dirty);

    buf_free(&buf2);
    remove(path);
}

void test_save_preserves_empty_lines(void) {
    const char *path = "/tmp/qe_test_empty_lines.txt";
    buf_insert_row(&buf, 0, "first", 5);
    buf_insert_row(&buf, 1, "",      0);
    buf_insert_row(&buf, 2, "third", 5);
    buf.filename = strdup(path);

    buf_save(&buf);

    Buffer buf2;
    buf_init(&buf2);
    buf_open(&buf2, path);

    TEST_ASSERT_EQUAL_INT(3, buf2.numrows);
    TEST_ASSERT_EQUAL_STRING("first", buf2.rows[0].chars);
    TEST_ASSERT_EQUAL_STRING("",      buf2.rows[1].chars);
    TEST_ASSERT_EQUAL_STRING("third", buf2.rows[2].chars);

    buf_free(&buf2);
    remove(path);
}

/* ── col_to_vcol ─────────────────────────────────────────────────────── */

void test_col_to_vcol_no_tabs(void) {
    buf_insert_row(&buf, 0, "hello", 5);
    /* No tabs: byte col == visual col */
    TEST_ASSERT_EQUAL_INT(0, col_to_vcol(&buf.rows[0], 0, 4));
    TEST_ASSERT_EQUAL_INT(3, col_to_vcol(&buf.rows[0], 3, 4));
    TEST_ASSERT_EQUAL_INT(5, col_to_vcol(&buf.rows[0], 5, 4));
}

void test_col_to_vcol_tab_at_col0(void) {
    buf_insert_row(&buf, 0, "\thello", 6);
    /* Tab at col 0 with tabwidth 4 → vcol 4 */
    TEST_ASSERT_EQUAL_INT(4, col_to_vcol(&buf.rows[0], 1, 4));
}

void test_col_to_vcol_tab_mid_row(void) {
    /* "ab\tcd": tab at byte 2, aligns to next tabstop at vcol 4 */
    buf_insert_row(&buf, 0, "ab\tcd", 5);
    TEST_ASSERT_EQUAL_INT(2, col_to_vcol(&buf.rows[0], 2, 4));
    TEST_ASSERT_EQUAL_INT(4, col_to_vcol(&buf.rows[0], 3, 4)); /* after tab */
}

void test_col_to_vcol_multiple_tabs(void) {
    /* "\t\t": two tabs each width 4 → vcol 8 after col 2 */
    buf_insert_row(&buf, 0, "\t\t", 2);
    TEST_ASSERT_EQUAL_INT(8, col_to_vcol(&buf.rows[0], 2, 4));
}

void test_col_to_vcol_past_end(void) {
    buf_insert_row(&buf, 0, "abc", 3);
    /* col == len returns sum without going out of bounds */
    TEST_ASSERT_EQUAL_INT(3, col_to_vcol(&buf.rows[0], 3, 4));
}

void test_col_to_vcol_tabwidth8(void) {
    buf_insert_row(&buf, 0, "\tx", 2);
    TEST_ASSERT_EQUAL_INT(8, col_to_vcol(&buf.rows[0], 1, 8));
}

/* ── vcol_to_col ─────────────────────────────────────────────────────── */

void test_vcol_to_col_roundtrip(void) {
    buf_insert_row(&buf, 0, "ab\tcd", 5);
    for (int c = 0; c <= 5; c++) {
        int v = col_to_vcol(&buf.rows[0], c, 4);
        TEST_ASSERT_EQUAL_INT(c, vcol_to_col(&buf.rows[0], v, 4));
    }
}

void test_vcol_to_col_before_tab(void) {
    /* "ab\t": vcol 1 → col 1 (before the tab) */
    buf_insert_row(&buf, 0, "ab\t", 3);
    TEST_ASSERT_EQUAL_INT(1, vcol_to_col(&buf.rows[0], 1, 4));
}

void test_vcol_to_col_inside_tab_snaps(void) {
    /* "\thi": vcol 2 is inside the first tab expansion (0-3).
       vcol_to_col should return col 1 (byte after tab). */
    buf_insert_row(&buf, 0, "\thi", 3);
    TEST_ASSERT_EQUAL_INT(1, vcol_to_col(&buf.rows[0], 2, 4));
}

void test_vcol_to_col_exact_tabstop(void) {
    buf_insert_row(&buf, 0, "\thi", 3);
    /* vcol == tabwidth: the tab consumed vcols 0-3, so vcol 4 → col 1 */
    TEST_ASSERT_EQUAL_INT(1, vcol_to_col(&buf.rows[0], 4, 4));
}

void test_vcol_to_col_past_end_clamps(void) {
    buf_insert_row(&buf, 0, "abc", 3);
    TEST_ASSERT_EQUAL_INT(3, vcol_to_col(&buf.rows[0], 999, 4));
}

void test_vcol_to_col_no_tabs_identity(void) {
    buf_insert_row(&buf, 0, "hello", 5);
    TEST_ASSERT_EQUAL_INT(2, vcol_to_col(&buf.rows[0], 2, 4));
}

/* ── buf_row_indent ──────────────────────────────────────────────────── */

void test_row_indent_no_indent(void) {
    buf_insert_row(&buf, 0, "hello", 5);
    TEST_ASSERT_EQUAL_INT(0, buf_row_indent(&buf.rows[0], 4));
}

void test_row_indent_spaces(void) {
    buf_insert_row(&buf, 0, "   x", 4);
    TEST_ASSERT_EQUAL_INT(3, buf_row_indent(&buf.rows[0], 4));
}

void test_row_indent_tabs(void) {
    buf_insert_row(&buf, 0, "\tx", 2);
    TEST_ASSERT_EQUAL_INT(4, buf_row_indent(&buf.rows[0], 4));
}

void test_row_indent_mixed(void) {
    /* " \tx": 1 space + 1 tab (aligns to next stop at 4) = 4 */
    buf_insert_row(&buf, 0, " \tx", 3);
    TEST_ASSERT_EQUAL_INT(4, buf_row_indent(&buf.rows[0], 4));
}

/* ── buf_fold_end ────────────────────────────────────────────────────── */

void test_fold_end_single_row(void) {
    buf_insert_row(&buf, 0, "only", 4);
    TEST_ASSERT_EQUAL_INT(0, buf_fold_end(&buf, 0, 4));
}

void test_fold_end_deeper_block(void) {
    /* row 0: base; rows 1-2: indented → fold_end == 2 */
    buf_insert_row(&buf, 0, "if x:", 5);
    buf_insert_row(&buf, 1, "    a", 5);
    buf_insert_row(&buf, 2, "    b", 5);
    buf_insert_row(&buf, 3, "end", 3);
    TEST_ASSERT_EQUAL_INT(2, buf_fold_end(&buf, 0, 4));
}

void test_fold_end_includes_blank_lines_inside(void) {
    /* blank line between indented rows should be included */
    buf_insert_row(&buf, 0, "if x:", 5);
    buf_insert_row(&buf, 1, "    a", 5);
    buf_insert_row(&buf, 2, "",      0);
    buf_insert_row(&buf, 3, "    b", 5);
    buf_insert_row(&buf, 4, "end",   3);
    TEST_ASSERT_EQUAL_INT(3, buf_fold_end(&buf, 0, 4));
}

void test_fold_end_zero_indent_stops_at_next_nonblank(void) {
    /* All lines at indent 0: fold covers nothing beyond start */
    buf_insert_row(&buf, 0, "a", 1);
    buf_insert_row(&buf, 1, "b", 1);
    TEST_ASSERT_EQUAL_INT(0, buf_fold_end(&buf, 0, 4));
}

void test_fold_end_start_is_last_row(void) {
    buf_insert_row(&buf, 0, "x", 1);
    buf_insert_row(&buf, 1, "y", 1);
    TEST_ASSERT_EQUAL_INT(1, buf_fold_end(&buf, 1, 4));
}

void test_fold_end_nested_deeper_block(void) {
    /* All three indented rows belong to the outer fold */
    buf_insert_row(&buf, 0, "outer:",   6);
    buf_insert_row(&buf, 1, "    mid:", 8);
    buf_insert_row(&buf, 2, "        deep", 12);
    buf_insert_row(&buf, 3, "done",    4);
    TEST_ASSERT_EQUAL_INT(2, buf_fold_end(&buf, 0, 4));
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* buf_init */
    RUN_TEST(test_init_rows_null);
    RUN_TEST(test_init_numrows_zero);
    RUN_TEST(test_init_filename_null);
    RUN_TEST(test_init_dirty_zero);

    /* buf_insert_row */
    RUN_TEST(test_insert_row_into_empty);
    RUN_TEST(test_insert_row_increments_dirty);
    RUN_TEST(test_insert_row_at_end);
    RUN_TEST(test_insert_row_in_middle);
    RUN_TEST(test_insert_row_empty_string);
    RUN_TEST(test_insert_row_out_of_bounds_is_noop);

    /* buf_delete_row */
    RUN_TEST(test_delete_row_only_row);
    RUN_TEST(test_delete_row_increments_dirty);
    RUN_TEST(test_delete_row_first);
    RUN_TEST(test_delete_row_middle);
    RUN_TEST(test_delete_row_last);
    RUN_TEST(test_delete_row_out_of_bounds_is_noop);

    /* buf_row_insert_char */
    RUN_TEST(test_row_insert_char_at_start);
    RUN_TEST(test_row_insert_char_at_end);
    RUN_TEST(test_row_insert_char_in_middle);
    RUN_TEST(test_row_insert_char_into_empty_row);
    RUN_TEST(test_row_insert_char_out_of_bounds_clamps_to_end);

    /* buf_row_delete_char */
    RUN_TEST(test_row_delete_char_first);
    RUN_TEST(test_row_delete_char_last);
    RUN_TEST(test_row_delete_char_middle);
    RUN_TEST(test_row_delete_char_out_of_bounds_is_noop);
    RUN_TEST(test_row_delete_char_until_empty);

    /* buf_row_append_string */
    RUN_TEST(test_row_append_string_basic);
    RUN_TEST(test_row_append_string_to_empty_row);
    RUN_TEST(test_row_append_empty_string_is_noop);

    /* buf_open */
    RUN_TEST(test_open_nonexistent_file_sets_filename);
    RUN_TEST(test_open_strips_newlines);
    RUN_TEST(test_open_resets_dirty);
    RUN_TEST(test_open_empty_file);

    /* buf_save */
    RUN_TEST(test_save_without_filename_returns_error);
    RUN_TEST(test_save_resets_dirty);
    RUN_TEST(test_save_and_reload);
    RUN_TEST(test_save_preserves_empty_lines);

    /* col_to_vcol */
    RUN_TEST(test_col_to_vcol_no_tabs);
    RUN_TEST(test_col_to_vcol_tab_at_col0);
    RUN_TEST(test_col_to_vcol_tab_mid_row);
    RUN_TEST(test_col_to_vcol_multiple_tabs);
    RUN_TEST(test_col_to_vcol_past_end);
    RUN_TEST(test_col_to_vcol_tabwidth8);

    /* vcol_to_col */
    RUN_TEST(test_vcol_to_col_roundtrip);
    RUN_TEST(test_vcol_to_col_before_tab);
    RUN_TEST(test_vcol_to_col_inside_tab_snaps);
    RUN_TEST(test_vcol_to_col_exact_tabstop);
    RUN_TEST(test_vcol_to_col_past_end_clamps);
    RUN_TEST(test_vcol_to_col_no_tabs_identity);

    /* buf_row_indent */
    RUN_TEST(test_row_indent_no_indent);
    RUN_TEST(test_row_indent_spaces);
    RUN_TEST(test_row_indent_tabs);
    RUN_TEST(test_row_indent_mixed);

    /* buf_fold_end */
    RUN_TEST(test_fold_end_single_row);
    RUN_TEST(test_fold_end_deeper_block);
    RUN_TEST(test_fold_end_includes_blank_lines_inside);
    RUN_TEST(test_fold_end_zero_indent_stops_at_next_nonblank);
    RUN_TEST(test_fold_end_start_is_last_row);
    RUN_TEST(test_fold_end_nested_deeper_block);

    return UNITY_END();
}
