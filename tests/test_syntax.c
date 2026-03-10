// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "syntax.h"
#include "buf.h"

#include <stdlib.h>
#include <string.h>

/* Minimal C-like syntax definition used by all tests. */
static char *filetypes[]  = { "c", "h" };
static char *keywords[]   = { "if", "return", NULL };
static char *types[]      = { "int", "void", NULL };
static SyntaxDef c_def = {
    .filetypes        = filetypes,
    .num_filetypes    = 2,
    .keywords         = keywords,
    .num_keywords     = 2,
    .types            = types,
    .num_types        = 2,
    .comment_single   = "//",
    .comment_ml_start = "/*",
    .comment_ml_end   = "*/",
};

void setUp(void)    { syntax_register(&c_def); }
void tearDown(void) { /* registry persists; tests are additive */ }

/* Helper: build a Row from a string literal (no heap allocation of chars). */
static Row make_row(const char *s) {
    Row r;
    r.len            = (int)strlen(s);
    r.chars          = (char *)s;  /* borrow; tests never free this */
    r.hl             = NULL;
    r.hl_open_comment = 0;
    return r;
}

/* ── syntax_detect ───────────────────────────────────────────────────── */

void test_detect_known_extension(void) {
    const SyntaxDef *d = syntax_detect("main.c");
    TEST_ASSERT_NOT_NULL(d);
}

void test_detect_second_extension(void) {
    const SyntaxDef *d = syntax_detect("defs.h");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_PTR(&c_def, d);
}

void test_detect_unknown_extension(void) {
    TEST_ASSERT_NULL(syntax_detect("file.py"));
}

void test_detect_no_extension(void) {
    TEST_ASSERT_NULL(syntax_detect("Makefile"));
}

void test_detect_null_filename(void) {
    TEST_ASSERT_NULL(syntax_detect(NULL));
}

void test_detect_case_sensitive(void) {
    /* "TEST.C" should not match "c" (case-sensitive). */
    TEST_ASSERT_NULL(syntax_detect("TEST.C"));
}

/* ── syntax_scan_row ─────────────────────────────────────────────────── */

void test_scan_no_comment_markers(void) {
    Row r = make_row("int x = 42;");
    TEST_ASSERT_EQUAL_INT(0, syntax_scan_row(&c_def, &r, 0));
}

void test_scan_single_line_comment(void) {
    Row r = make_row("// this is a comment");
    TEST_ASSERT_EQUAL_INT(0, syntax_scan_row(&c_def, &r, 0));
}

void test_scan_ml_open_without_close(void) {
    Row r = make_row("/* start of comment");
    TEST_ASSERT_EQUAL_INT(1, syntax_scan_row(&c_def, &r, 0));
}

void test_scan_ml_open_and_close(void) {
    Row r = make_row("/* comment */ code");
    TEST_ASSERT_EQUAL_INT(0, syntax_scan_row(&c_def, &r, 0));
}

void test_scan_continues_open_ml_blank(void) {
    Row r = make_row("   ");
    TEST_ASSERT_EQUAL_INT(1, syntax_scan_row(&c_def, &r, 1));
}

void test_scan_close_ends_ml(void) {
    Row r = make_row("   */ code");
    TEST_ASSERT_EQUAL_INT(0, syntax_scan_row(&c_def, &r, 1));
}

void test_scan_comment_inside_string_not_counted(void) {
    /* The slash-star is inside a string literal; should not open a ML comment. */
    Row r = make_row("\"/*\" code");
    TEST_ASSERT_EQUAL_INT(0, syntax_scan_row(&c_def, &r, 0));
}

void test_scan_single_comment_mid_line(void) {
    Row r = make_row("x = 1; // comment");
    TEST_ASSERT_EQUAL_INT(0, syntax_scan_row(&c_def, &r, 0));
}

/* ── syntax_highlight_row ────────────────────────────────────────────── */

void test_highlight_null_def_leaves_hl_null(void) {
    Row r = make_row("hello");
    int oc = syntax_highlight_row(NULL, &r, 0);
    TEST_ASSERT_NULL(r.hl);
    TEST_ASSERT_EQUAL_INT(0, oc);
}

void test_highlight_plain_text_all_normal(void) {
    Row r = make_row("xyz");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    for (int i = 0; i < r.len; i++)
        TEST_ASSERT_EQUAL_INT(HL_NORMAL, r.hl[i]);
    free(r.hl);
}

void test_highlight_keyword(void) {
    Row r = make_row("if");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    TEST_ASSERT_EQUAL_INT(HL_KEYWORD, r.hl[0]);
    TEST_ASSERT_EQUAL_INT(HL_KEYWORD, r.hl[1]);
    free(r.hl);
}

void test_highlight_string_literal(void) {
    Row r = make_row("\"hi\"");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    /* bytes 0-3: opening quote, h, i, closing quote → all HL_STRING */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[0]);
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[1]);
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[2]);
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[3]);
    free(r.hl);
}

void test_highlight_single_line_comment(void) {
    Row r = make_row("// note");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    for (int i = 0; i < r.len; i++)
        TEST_ASSERT_EQUAL_INT(HL_COMMENT, r.hl[i]);
    free(r.hl);
}

void test_highlight_number(void) {
    Row r = make_row("42");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    TEST_ASSERT_EQUAL_INT(HL_NUMBER, r.hl[0]);
    TEST_ASSERT_EQUAL_INT(HL_NUMBER, r.hl[1]);
    free(r.hl);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* syntax_detect */
    RUN_TEST(test_detect_known_extension);
    RUN_TEST(test_detect_second_extension);
    RUN_TEST(test_detect_unknown_extension);
    RUN_TEST(test_detect_no_extension);
    RUN_TEST(test_detect_null_filename);
    RUN_TEST(test_detect_case_sensitive);

    /* syntax_scan_row */
    RUN_TEST(test_scan_no_comment_markers);
    RUN_TEST(test_scan_single_line_comment);
    RUN_TEST(test_scan_ml_open_without_close);
    RUN_TEST(test_scan_ml_open_and_close);
    RUN_TEST(test_scan_continues_open_ml_blank);
    RUN_TEST(test_scan_close_ends_ml);
    RUN_TEST(test_scan_comment_inside_string_not_counted);
    RUN_TEST(test_scan_single_comment_mid_line);

    /* syntax_highlight_row */
    RUN_TEST(test_highlight_null_def_leaves_hl_null);
    RUN_TEST(test_highlight_plain_text_all_normal);
    RUN_TEST(test_highlight_keyword);
    RUN_TEST(test_highlight_string_literal);
    RUN_TEST(test_highlight_single_line_comment);
    RUN_TEST(test_highlight_number);

    return UNITY_END();
}
