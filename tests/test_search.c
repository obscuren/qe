// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "search.h"
#include "buf.h"

#include <stdlib.h>
#include <string.h>

/* ── Test helpers ────────────────────────────────────────────────────── */

static Buffer buf;

/* Build a Buffer from a NULL-terminated array of C strings. */
static void buf_from_lines(Buffer *b, const char **lines) {
    buf_init(b);
    for (int i = 0; lines[i] != NULL; i++)
        buf_insert_row(b, b->numrows, lines[i], (int)strlen(lines[i]));
    b->dirty = 0;
}

void setUp(void)    { buf_init(&buf); }
void tearDown(void) { buf_free(&buf); }

/* ── search_match_literal ────────────────────────────────────────────── */

void test_literal_found(void) {
    const char *line = "hello world";
    int col, len;
    int r = search_match_literal(line, 11, "world", 5, 0, &col, &len);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(6, col);
    TEST_ASSERT_EQUAL_INT(5, len);
}

void test_literal_not_found(void) {
    const char *line = "hello world";
    int col, len;
    int r = search_match_literal(line, 11, "foo", 3, 0, &col, &len);
    TEST_ASSERT_EQUAL_INT(0, r);
}

void test_literal_match_at_start(void) {
    const char *line = "abcdef";
    int col, len;
    int r = search_match_literal(line, 6, "abc", 3, 0, &col, &len);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(0, col);
    TEST_ASSERT_EQUAL_INT(3, len);
}

void test_literal_match_at_end(void) {
    const char *line = "abcdef";
    int col, len;
    int r = search_match_literal(line, 6, "def", 3, 0, &col, &len);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(3, col);
    TEST_ASSERT_EQUAL_INT(3, len);
}

void test_literal_start_col_skips_earlier_match(void) {
    const char *line = "abab";
    int col, len;
    /* "ab" appears at 0 and 2; start_col=1 should skip the first */
    int r = search_match_literal(line, 4, "ab", 2, 1, &col, &len);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(2, col);
}

void test_literal_pat_len_exceeds_line_len(void) {
    const char *line = "hi";
    int col, len;
    int r = search_match_literal(line, 2, "hello", 5, 0, &col, &len);
    TEST_ASSERT_EQUAL_INT(0, r);
}

/* ── search_find_next ────────────────────────────────────────────────── */

void test_find_next_same_row_after_cursor(void) {
    const char *lines[] = { "foo bar foo", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "foo", 3);
    int row, col;
    /* cursor at (0,0) → start_col = 1, should find match at col 8 */
    int r = search_find_next(&q, &buf, 0, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(0, row);
    TEST_ASSERT_EQUAL_INT(8, col);
}

void test_find_next_different_row(void) {
    const char *lines[] = { "hello", "world", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "world", 5);
    int row, col;
    int r = search_find_next(&q, &buf, 0, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(1, row);
    TEST_ASSERT_EQUAL_INT(0, col);
}

void test_find_next_wraps_around(void) {
    const char *lines[] = { "target", "nothing", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "target", 6);
    int row, col;
    /* Start on last row with no match there → should wrap to row 0 */
    int r = search_find_next(&q, &buf, 1, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(0, row);
    TEST_ASSERT_EQUAL_INT(0, col);
}

void test_find_next_not_found(void) {
    const char *lines[] = { "hello", "world", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "zzz", 3);
    int row, col;
    int r = search_find_next(&q, &buf, 0, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(0, r);
}

void test_find_next_empty_buffer(void) {
    SearchQuery q;
    search_query_init_literal(&q, "foo", 3);
    int row, col;
    int r = search_find_next(&q, &buf, 0, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(0, r);
}

/* ── search_find_prev ────────────────────────────────────────────────── */

void test_find_prev_same_row_before_cursor(void) {
    const char *lines[] = { "foo bar foo", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "foo", 3);
    int row, col;
    /* cursor at (0,8) → should find the earlier match at col 0 */
    int r = search_find_prev(&q, &buf, 0, 8, &row, &col);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(0, row);
    TEST_ASSERT_EQUAL_INT(0, col);
}

void test_find_prev_previous_row(void) {
    const char *lines[] = { "target", "cursor here", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "target", 6);
    int row, col;
    /* cursor at start of row 1 → no match on row 1 before col 0; should go to row 0 */
    int r = search_find_prev(&q, &buf, 1, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(0, row);
    TEST_ASSERT_EQUAL_INT(0, col);
}

void test_find_prev_wraps_around(void) {
    const char *lines[] = { "nothing", "target", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "target", 6);
    int row, col;
    /* cursor at row 0 col 0; no prev match on row 0; wrap to row 1 */
    int r = search_find_prev(&q, &buf, 0, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(1, r);
    TEST_ASSERT_EQUAL_INT(1, row);
    TEST_ASSERT_EQUAL_INT(0, col);
}

void test_find_prev_not_found(void) {
    const char *lines[] = { "hello", "world", NULL };
    buf_from_lines(&buf, lines);
    SearchQuery q;
    search_query_init_literal(&q, "zzz", 3);
    int row, col;
    int r = search_find_prev(&q, &buf, 0, 0, &row, &col);
    TEST_ASSERT_EQUAL_INT(0, r);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* search_match_literal */
    RUN_TEST(test_literal_found);
    RUN_TEST(test_literal_not_found);
    RUN_TEST(test_literal_match_at_start);
    RUN_TEST(test_literal_match_at_end);
    RUN_TEST(test_literal_start_col_skips_earlier_match);
    RUN_TEST(test_literal_pat_len_exceeds_line_len);

    /* search_find_next */
    RUN_TEST(test_find_next_same_row_after_cursor);
    RUN_TEST(test_find_next_different_row);
    RUN_TEST(test_find_next_wraps_around);
    RUN_TEST(test_find_next_not_found);
    RUN_TEST(test_find_next_empty_buffer);

    /* search_find_prev */
    RUN_TEST(test_find_prev_same_row_before_cursor);
    RUN_TEST(test_find_prev_previous_row);
    RUN_TEST(test_find_prev_wraps_around);
    RUN_TEST(test_find_prev_not_found);

    return UNITY_END();
}
