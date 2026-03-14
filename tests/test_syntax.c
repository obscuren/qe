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

/* ── preprocessor highlighting ────────────────────────────────────────── */

void test_highlight_preproc_include_quoted(void) {
    Row r = make_row("#include \"foo.h\"");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    /* #include  = HL_PREPROC (indices 0-8) */
    for (int i = 0; i < 9; i++)
        TEST_ASSERT_EQUAL_INT(HL_PREPROC, r.hl[i]);
    /* "foo.h" = HL_STRING (indices 9-14) */
    for (int i = 9; i < 15; i++)
        TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[i]);
    free(r.hl);
}

void test_highlight_preproc_include_angle(void) {
    Row r = make_row("#include <stdio.h>");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    /* #include  = HL_PREPROC (indices 0-8) */
    for (int i = 0; i < 9; i++)
        TEST_ASSERT_EQUAL_INT(HL_PREPROC, r.hl[i]);
    /* <stdio.h> = HL_STRING (indices 9-17) */
    for (int i = 9; i < 18; i++)
        TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[i]);
    free(r.hl);
}

void test_highlight_preproc_define(void) {
    Row r = make_row("#define FOO");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    /* #define  = HL_PREPROC (indices 0-7, including trailing space) */
    for (int i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_INT(HL_PREPROC, r.hl[i]);
    free(r.hl);
}

/* ── escape sequence highlighting ────────────────────────────────────── */

void test_highlight_escape_in_string(void) {
    /* "hi\nbye" — 9 chars: " h i \ n b y e " */
    Row r = make_row("\"hi\\nbye\"");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[0]);  /* " */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[1]);  /* h */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[2]);  /* i */
    TEST_ASSERT_EQUAL_INT(HL_ESCAPE, r.hl[3]);  /* \ */
    TEST_ASSERT_EQUAL_INT(HL_ESCAPE, r.hl[4]);  /* n */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[5]);  /* b */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[6]);  /* y */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[7]);  /* e */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[8]);  /* " */
    free(r.hl);
}

void test_highlight_escape_backslash(void) {
    /* "\\" — 4 chars: " \ \ " */
    Row r = make_row("\"\\\\\"");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[0]);  /* " */
    TEST_ASSERT_EQUAL_INT(HL_ESCAPE, r.hl[1]);  /* \ */
    TEST_ASSERT_EQUAL_INT(HL_ESCAPE, r.hl[2]);  /* \ */
    TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[3]);  /* " */
    free(r.hl);
}

/* ── rainbow bracket highlighting ────────────────────────────────────── */

void test_highlight_rainbow_brackets(void) {
    /* (([]))  — depths: ( =0, ( =1, [ =2, ] =2, ) =1, ) =0 */
    Row r = make_row("(([]))");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    TEST_ASSERT_EQUAL_INT(HL_BRACKET1, r.hl[0]);  /* ( depth 0 */
    TEST_ASSERT_EQUAL_INT(HL_BRACKET2, r.hl[1]);  /* ( depth 1 */
    TEST_ASSERT_EQUAL_INT(HL_BRACKET3, r.hl[2]);  /* [ depth 2 */
    TEST_ASSERT_EQUAL_INT(HL_BRACKET3, r.hl[3]);  /* ] depth 2 */
    TEST_ASSERT_EQUAL_INT(HL_BRACKET2, r.hl[4]);  /* ) depth 1 */
    TEST_ASSERT_EQUAL_INT(HL_BRACKET1, r.hl[5]);  /* ) depth 0 */
    free(r.hl);
}

void test_highlight_brackets_in_string(void) {
    /* "()" — brackets inside string remain HL_STRING */
    Row r = make_row("\"()\"");
    r.hl = NULL;
    syntax_highlight_row(&c_def, &r, 0);
    TEST_ASSERT_NOT_NULL(r.hl);
    for (int i = 0; i < r.len; i++)
        TEST_ASSERT_EQUAL_INT(HL_STRING, r.hl[i]);
    free(r.hl);
}

/* ── multiline comment highlighting ──────────────────────────────────── */

void test_multiline_comment_two_rows(void) {
    Row r1 = make_row("/* start");
    Row r2 = make_row("end */");
    r1.hl = NULL; r2.hl = NULL;
    int oc1 = syntax_highlight_row(&c_def, &r1, 0);
    TEST_ASSERT_EQUAL_INT(1, oc1);  /* still in comment */
    for (int i = 0; i < r1.len; i++)
        TEST_ASSERT_EQUAL_INT(HL_COMMENT, r1.hl[i]);
    int oc2 = syntax_highlight_row(&c_def, &r2, oc1);
    TEST_ASSERT_EQUAL_INT(0, oc2);  /* comment closed */
    for (int i = 0; i < r2.len; i++)
        TEST_ASSERT_EQUAL_INT(HL_COMMENT, r2.hl[i]);
    free(r1.hl); free(r2.hl);
}

void test_multiline_comment_three_rows(void) {
    Row r1 = make_row("/* start");
    Row r2 = make_row("middle");
    Row r3 = make_row("end */");
    r1.hl = NULL; r2.hl = NULL; r3.hl = NULL;
    int oc1 = syntax_highlight_row(&c_def, &r1, 0);
    int oc2 = syntax_highlight_row(&c_def, &r2, oc1);
    TEST_ASSERT_EQUAL_INT(1, oc2);  /* still in comment */
    for (int i = 0; i < r2.len; i++)
        TEST_ASSERT_EQUAL_INT(HL_COMMENT, r2.hl[i]);
    int oc3 = syntax_highlight_row(&c_def, &r3, oc2);
    TEST_ASSERT_EQUAL_INT(0, oc3);
    for (int i = 0; i < r3.len; i++)
        TEST_ASSERT_EQUAL_INT(HL_COMMENT, r3.hl[i]);
    free(r1.hl); free(r2.hl); free(r3.hl);
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

    /* preprocessor */
    RUN_TEST(test_highlight_preproc_include_quoted);
    RUN_TEST(test_highlight_preproc_include_angle);
    RUN_TEST(test_highlight_preproc_define);

    /* escape sequences */
    RUN_TEST(test_highlight_escape_in_string);
    RUN_TEST(test_highlight_escape_backslash);

    /* rainbow brackets */
    RUN_TEST(test_highlight_rainbow_brackets);
    RUN_TEST(test_highlight_brackets_in_string);

    /* multiline comments */
    RUN_TEST(test_multiline_comment_two_rows);
    RUN_TEST(test_multiline_comment_three_rows);

    return UNITY_END();
}
