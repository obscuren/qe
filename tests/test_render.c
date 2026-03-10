// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "editor.h"
#include "render.h"

#include <string.h>
#include <unistd.h>

/* ── setUp / tearDown ────────────────────────────────────────────────── */

void setUp(void) {
    editor_init();
    /* Override to known dimensions regardless of terminal availability. */
    E.term_rows  = 24;
    E.term_cols  = 80;
    E.screenrows = E.term_rows - 2;
    E.screencols = E.term_cols;
    E.panes[0].height = E.screenrows;
    E.panes[0].width  = E.screencols;
}

void tearDown(void) {
    buf_free(&E.buf);
    undo_tree_free(&E.undo_tree);
}

/* ── test_no_ed2_in_refresh ──────────────────────────────────────────── */

void test_no_ed2_in_refresh(void) {
    /* Redirect STDOUT_FILENO to a pipe so we can capture the output. */
    int pipefd[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(pipefd));

    int saved_stdout = dup(STDOUT_FILENO);
    TEST_ASSERT_NOT_EQUAL(-1, saved_stdout);

    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    editor_refresh_screen();

    /* Restore real stdout before any assertions. */
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    /* Read whatever was written. */
    char buf[65536];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';

    /* \x1b[2J must NOT appear. */
    TEST_ASSERT_NULL(memmem(buf, (size_t)n, "\x1b[2J", 4));

    /* \x1b[H and \x1b[J must appear. */
    TEST_ASSERT_NOT_NULL(memmem(buf, (size_t)n, "\x1b[H", 3));
    TEST_ASSERT_NOT_NULL(memmem(buf, (size_t)n, "\x1b[J", 3));
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_no_ed2_in_refresh);
    return UNITY_END();
}
