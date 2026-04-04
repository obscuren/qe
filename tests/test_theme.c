// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "theme.h"
#include "syntax.h"

#include <stdio.h>
#include <string.h>

void setUp(void)    { theme_reset(); }
void tearDown(void) { theme_reset(); }

/* ── theme_init ─────────────────────────────────────────────────────── */

void test_init_sets_default_theme(void) {
    theme_init();
    /* After init, "default" is the current theme — accessors should work. */
    TEST_ASSERT_NOT_NULL(theme_hl_escape(HL_COMMENT));
    TEST_ASSERT_NOT_NULL(theme_statusbar_escape(1));
    TEST_ASSERT_NOT_NULL(theme_cursorline_bg());
}

void test_init_default_bg_fg_null(void) {
    theme_init();
    /* Default theme has no explicit bg/fg (terminal default). */
    TEST_ASSERT_NULL(theme_bg());
    TEST_ASSERT_NULL(theme_fg());
}

/* ── theme_hl_escape ────────────────────────────────────────────────── */

void test_hl_escape_comment(void) {
    theme_init();
    const char *esc = theme_hl_escape(HL_COMMENT);
    TEST_ASSERT_NOT_NULL(esc);
    /* Default comment colour is dim cyan. */
    TEST_ASSERT_EQUAL_STRING("\x1b[2;36m", esc);
}

void test_hl_escape_keyword(void) {
    theme_init();
    const char *esc = theme_hl_escape(HL_KEYWORD);
    TEST_ASSERT_NOT_NULL(esc);
    TEST_ASSERT_EQUAL_STRING("\x1b[1;33m", esc);
}

void test_hl_escape_out_of_range(void) {
    theme_init();
    /* Out-of-range highlight type should return NULL. */
    TEST_ASSERT_NULL(theme_hl_escape(255));
}

void test_hl_escape_no_theme(void) {
    /* No theme_init — current_theme is NULL. */
    TEST_ASSERT_NULL(theme_hl_escape(HL_COMMENT));
}

/* ── theme_statusbar_escape ─────────────────────────────────────────── */

void test_statusbar_active_vs_inactive(void) {
    theme_init();
    const char *active   = theme_statusbar_escape(1);
    const char *inactive = theme_statusbar_escape(0);
    TEST_ASSERT_NOT_NULL(active);
    TEST_ASSERT_NOT_NULL(inactive);
    /* They should differ. */
    TEST_ASSERT_TRUE(strcmp(active, inactive) != 0);
}

void test_statusbar_fallback_when_no_theme(void) {
    /* No theme_init — should return hardcoded fallbacks. */
    const char *active   = theme_statusbar_escape(1);
    const char *inactive = theme_statusbar_escape(0);
    TEST_ASSERT_EQUAL_STRING("\x1b[7m",    active);
    TEST_ASSERT_EQUAL_STRING("\x1b[2;7m",  inactive);
}

/* ── theme_cursorline_bg ────────────────────────────────────────────── */

void test_cursorline_bg_default(void) {
    theme_init();
    const char *bg = theme_cursorline_bg();
    TEST_ASSERT_NOT_NULL(bg);
    TEST_ASSERT_EQUAL_STRING("\x1b[48;5;236m", bg);
}

void test_cursorline_bg_fallback(void) {
    /* No init — should return hardcoded fallback. */
    TEST_ASSERT_EQUAL_STRING("\x1b[48;5;236m", theme_cursorline_bg());
}

/* ── theme_set ──────────────────────────────────────────────────────── */

void test_set_unknown_returns_error(void) {
    theme_init();
    TEST_ASSERT_EQUAL_INT(-1, theme_set("nonexistent"));
}

void test_set_default_returns_zero(void) {
    theme_init();
    TEST_ASSERT_EQUAL_INT(0, theme_set("default"));
}

/* ── theme_register (overwrite existing) ────────────────────────────── */

void test_register_overwrite(void) {
    theme_init();
    /* Register a theme with the name "default" — should overwrite. */
    Theme t = {0};
    strncpy(t.name, "default", sizeof(t.name) - 1);
    t.hl_colors[HL_COMMENT] = "\x1b[31m"; /* red */
    t.bg = "custom_bg";
    theme_register(&t);

    /* theme_register copies strings, so the original escape is replaced. */
    TEST_ASSERT_EQUAL_STRING("\x1b[31m", theme_hl_escape(HL_COMMENT));
    TEST_ASSERT_NOT_NULL(theme_bg());
    TEST_ASSERT_EQUAL_STRING("custom_bg", theme_bg());
}

/* ── theme_register (new theme + switch) ────────────────────────────── */

void test_register_new_and_switch(void) {
    theme_init();

    Theme t = {0};
    strncpy(t.name, "monokai", sizeof(t.name) - 1);
    t.hl_colors[HL_STRING] = "\x1b[33m"; /* yellow */
    t.fg = "mono_fg";
    theme_register(&t);

    /* Default still active. */
    TEST_ASSERT_EQUAL_STRING("\x1b[32m", theme_hl_escape(HL_STRING));

    /* Switch to monokai. */
    TEST_ASSERT_EQUAL_INT(0, theme_set("monokai"));
    TEST_ASSERT_EQUAL_STRING("\x1b[33m", theme_hl_escape(HL_STRING));
    TEST_ASSERT_NOT_NULL(theme_fg());
    TEST_ASSERT_EQUAL_STRING("mono_fg", theme_fg());

    /* Switch back to default. */
    TEST_ASSERT_EQUAL_INT(0, theme_set("default"));
    TEST_ASSERT_EQUAL_STRING("\x1b[32m", theme_hl_escape(HL_STRING));
}

/* ── MAX_THEMES boundary ────────────────────────────────────────────── */

void test_max_themes_boundary(void) {
    /* Fill all slots. */
    for (int i = 0; i < MAX_THEMES; i++) {
        Theme t = {0};
        snprintf(t.name, sizeof(t.name), "theme_%d", i);
        theme_register(&t);
    }
    /* One more should be silently ignored. */
    Theme overflow = {0};
    strncpy(overflow.name, "overflow", sizeof(overflow.name) - 1);
    overflow.hl_colors[HL_COMMENT] = "\x1b[31m";
    theme_register(&overflow);

    /* "overflow" should not be reachable. */
    TEST_ASSERT_EQUAL_INT(-1, theme_set("overflow"));
}

/* ── Runner ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_init_sets_default_theme);
    RUN_TEST(test_init_default_bg_fg_null);

    RUN_TEST(test_hl_escape_comment);
    RUN_TEST(test_hl_escape_keyword);
    RUN_TEST(test_hl_escape_out_of_range);
    RUN_TEST(test_hl_escape_no_theme);

    RUN_TEST(test_statusbar_active_vs_inactive);
    RUN_TEST(test_statusbar_fallback_when_no_theme);

    RUN_TEST(test_cursorline_bg_default);
    RUN_TEST(test_cursorline_bg_fallback);

    RUN_TEST(test_set_unknown_returns_error);
    RUN_TEST(test_set_default_returns_zero);

    RUN_TEST(test_register_overwrite);
    RUN_TEST(test_register_new_and_switch);
    RUN_TEST(test_max_themes_boundary);

    return UNITY_END();
}
