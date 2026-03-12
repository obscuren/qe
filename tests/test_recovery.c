// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "recovery.h"
#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static char orig_cwd[1024];

static void enter_tmpdir(void) {
    getcwd(orig_cwd, sizeof(orig_cwd));
    char tmpl[] = "/tmp/qe_test_recovery_XXXXXX";
    char *dir = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(dir);
    chdir(dir);
}

static void leave_tmpdir(void) {
    system("rm -rf .qe");
    system("rm -f testfile.txt");
    chdir(orig_cwd);
}

static void create_file(const char *name, const char *content) {
    FILE *fp = fopen(name, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs(content, fp);
    fclose(fp);
}

/* Set the file's mtime to 10 seconds in the past so recovery files are newer. */
static void backdate_file(const char *name) {
    struct stat st;
    stat(name, &st);
    struct utimbuf ut;
    ut.actime  = st.st_atime;
    ut.modtime = st.st_mtime - 10;
    utime(name, &ut);
}

static Buffer make_buf(const char *filename, const char **lines, int numrows) {
    Buffer b;
    buf_init(&b);
    b.filename = filename ? strdup(filename) : NULL;
    b.numrows = numrows;
    b.rows = malloc(sizeof(Row) * (numrows > 0 ? numrows : 1));
    for (int i = 0; i < numrows; i++) {
        b.rows[i].len = (int)strlen(lines[i]);
        b.rows[i].chars = strdup(lines[i]);
        b.rows[i].hl = NULL;
        b.rows[i].hl_open_comment = 0;
    }
    b.dirty = 1;
    return b;
}

void setUp(void) { enter_tmpdir(); }
void tearDown(void) { leave_tmpdir(); }

/* ── Tests ───────────────────────────────────────────────────────────── */

void test_save_and_load_roundtrip(void) {
    create_file("testfile.txt", "original\n");
    backdate_file("testfile.txt");

    const char *lines[] = {"hello world", "second line", "third"};
    Buffer b = make_buf("testfile.txt", lines, 3);

    TEST_ASSERT_EQUAL_INT(0, recovery_save("testfile.txt", &b, 5, 1));

    /* Verify the recovery file exists. */
    TEST_ASSERT_EQUAL_INT(1, recovery_exists("testfile.txt"));

    /* Load into a fresh buffer. */
    Buffer b2;
    buf_init(&b2);
    b2.filename = strdup("testfile.txt");
    int cx, cy;
    TEST_ASSERT_EQUAL_INT(0, recovery_load("testfile.txt", &b2, &cx, &cy));

    TEST_ASSERT_EQUAL_INT(3, b2.numrows);
    TEST_ASSERT_EQUAL_INT(5, cx);
    TEST_ASSERT_EQUAL_INT(1, cy);
    TEST_ASSERT_EQUAL_STRING("hello world", b2.rows[0].chars);
    TEST_ASSERT_EQUAL_STRING("second line", b2.rows[1].chars);
    TEST_ASSERT_EQUAL_STRING("third", b2.rows[2].chars);
    TEST_ASSERT_EQUAL_INT(1, b2.dirty);

    buf_free(&b);
    buf_free(&b2);
}

void test_exists_returns_false_without_recovery(void) {
    create_file("testfile.txt", "content\n");
    TEST_ASSERT_EQUAL_INT(0, recovery_exists("testfile.txt"));
}

void test_exists_returns_false_when_file_is_newer(void) {
    create_file("testfile.txt", "old\n");
    backdate_file("testfile.txt");

    const char *lines[] = {"recovered"};
    Buffer b = make_buf("testfile.txt", lines, 1);
    recovery_save("testfile.txt", &b, 0, 0);

    /* Rewrite original file — sleep ensures it gets a newer mtime. */
    sleep(1);
    create_file("testfile.txt", "updated\n");

    TEST_ASSERT_EQUAL_INT(0, recovery_exists("testfile.txt"));

    buf_free(&b);
}

void test_remove_deletes_recovery_file(void) {
    create_file("testfile.txt", "content\n");
    backdate_file("testfile.txt");

    const char *lines[] = {"data"};
    Buffer b = make_buf("testfile.txt", lines, 1);
    recovery_save("testfile.txt", &b, 0, 0);

    TEST_ASSERT_EQUAL_INT(1, recovery_exists("testfile.txt"));
    recovery_remove("testfile.txt");
    TEST_ASSERT_EQUAL_INT(0, recovery_exists("testfile.txt"));

    buf_free(&b);
}

void test_load_nonexistent_fails(void) {
    Buffer b;
    buf_init(&b);
    int cx, cy;
    TEST_ASSERT_EQUAL_INT(-1, recovery_load("nonexistent.txt", &b, &cx, &cy));
}

void test_save_empty_buffer(void) {
    create_file("testfile.txt", "");
    backdate_file("testfile.txt");

    const char **lines = NULL;
    Buffer b;
    buf_init(&b);
    b.filename = strdup("testfile.txt");
    b.numrows = 0;
    b.rows = NULL;
    b.dirty = 1;
    (void)lines;

    TEST_ASSERT_EQUAL_INT(0, recovery_save("testfile.txt", &b, 0, 0));

    Buffer b2;
    buf_init(&b2);
    b2.filename = strdup("testfile.txt");
    int cx, cy;
    TEST_ASSERT_EQUAL_INT(0, recovery_load("testfile.txt", &b2, &cx, &cy));
    TEST_ASSERT_EQUAL_INT(0, b2.numrows);

    buf_free(&b);
    buf_free(&b2);
}

void test_null_filepath(void) {
    TEST_ASSERT_EQUAL_INT(0, recovery_exists(NULL));
    TEST_ASSERT_EQUAL_INT(-1, recovery_save(NULL, NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(-1, recovery_load(NULL, NULL, NULL, NULL));
    /* recovery_remove(NULL) should not crash */
    recovery_remove(NULL);
}

/* ── Runner ──────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_save_and_load_roundtrip);
    RUN_TEST(test_exists_returns_false_without_recovery);
    RUN_TEST(test_exists_returns_false_when_file_is_newer);
    RUN_TEST(test_remove_deletes_recovery_file);
    RUN_TEST(test_load_nonexistent_fails);
    RUN_TEST(test_save_empty_buffer);
    RUN_TEST(test_null_filepath);
    return UNITY_END();
}
