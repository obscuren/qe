// SPDX-License-Identifier: GPL-3.0-or-later
#include "unity.h"
#include "undofile.h"
#include "undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Create a temp directory, chdir into it, so .qe/undo/ is isolated. */
static char orig_cwd[1024];

static void enter_tmpdir(void) {
    getcwd(orig_cwd, sizeof(orig_cwd));
    char tmpl[] = "/tmp/qe_test_undofile_XXXXXX";
    char *dir = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(dir);
    chdir(dir);
}

static void leave_tmpdir(void) {
    /* Clean up .qe/undo/ */
    system("rm -rf .qe");
    chdir(orig_cwd);
}

/* Create a dummy file so realpath() works during hashing. */
static void create_dummy_file(const char *name) {
    FILE *fp = fopen(name, "w");
    if (fp) { fprintf(fp, "dummy\n"); fclose(fp); }
}

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

/* Build a simple linear tree: root -> child1 -> child2.
   Caller must free with undo_tree_free(). */
static void build_linear_tree(UndoTree *t) {
    undo_tree_init(t);
    const char *a[] = {"line one", "line two"};
    const char *b[] = {"edited one", "line two"};
    const char *c[] = {"edited one", "edited two"};
    undo_tree_set_root(t, make_state(a, 2, 0, 0), "open");
    undo_tree_push(t, make_state(b, 2, 5, 0), "edit1");
    undo_tree_push(t, make_state(c, 2, 7, 1), "edit2");
}

/* Build a branching tree:
       root
      /    \
   br1      br2  (current) */
static void build_branching_tree(UndoTree *t) {
    undo_tree_init(t);
    const char *r[] = {"root"};
    const char *b1[] = {"branch1"};
    const char *b2[] = {"branch2"};
    undo_tree_set_root(t, make_state(r, 1, 0, 0), "initial");
    undo_tree_push(t, make_state(b1, 1, 3, 0), "br1");
    undo_tree_undo(t); /* back to root */
    undo_tree_push(t, make_state(b2, 1, 4, 0), "br2");
    /* current = br2 */
}

/* ── setUp / tearDown ────────────────────────────────────────────────── */

void setUp(void)    { enter_tmpdir(); }
void tearDown(void) { leave_tmpdir(); }

/* ── Save / load round-trip: linear tree ────────────────────────────── */

void test_save_load_linear(void) {
    create_dummy_file("test.c");
    UndoTree orig;
    build_linear_tree(&orig);

    TEST_ASSERT_EQUAL_INT(0, undofile_save("test.c", &orig));

    /* Verify the .qe/undo directory was created. */
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(".qe/undo", &st));
    TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));

    UndoTree loaded = {0};
    TEST_ASSERT_EQUAL_INT(0, undofile_load("test.c", &loaded));

    /* Structural checks. */
    TEST_ASSERT_NOT_NULL(loaded.root);
    TEST_ASSERT_NOT_NULL(loaded.current);
    TEST_ASSERT_EQUAL_INT(orig.total_nodes, loaded.total_nodes);
    TEST_ASSERT_EQUAL_INT(orig.next_seq, loaded.next_seq);

    /* Root content. */
    TEST_ASSERT_EQUAL_INT(2, loaded.root->state.numrows);
    TEST_ASSERT_EQUAL_STRING("line one", loaded.root->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("line two", loaded.root->state.row_chars[1]);
    TEST_ASSERT_EQUAL_STRING("open", loaded.root->desc);

    /* Current should be the last node (edit2, seq=2). */
    TEST_ASSERT_EQUAL_INT(orig.current->seq, loaded.current->seq);
    TEST_ASSERT_EQUAL_STRING("edited one", loaded.current->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("edited two", loaded.current->state.row_chars[1]);
    TEST_ASSERT_EQUAL_INT(7, loaded.current->state.cx);
    TEST_ASSERT_EQUAL_INT(1, loaded.current->state.cy);

    undo_tree_free(&orig);
    undo_tree_free(&loaded);
}

/* ── Save / load round-trip: branching tree ─────────────────────────── */

void test_save_load_branching(void) {
    create_dummy_file("branch.c");
    UndoTree orig;
    build_branching_tree(&orig);

    TEST_ASSERT_EQUAL_INT(0, undofile_save("branch.c", &orig));

    UndoTree loaded = {0};
    TEST_ASSERT_EQUAL_INT(0, undofile_load("branch.c", &loaded));

    TEST_ASSERT_EQUAL_INT(3, loaded.total_nodes);

    /* Root should have 2 children. */
    TEST_ASSERT_EQUAL_INT(2, loaded.root->num_children);
    TEST_ASSERT_EQUAL_STRING("root", loaded.root->state.row_chars[0]);

    /* Both branches present. */
    TEST_ASSERT_EQUAL_STRING("branch1",
                             loaded.root->children[0]->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("branch2",
                             loaded.root->children[1]->state.row_chars[0]);

    /* Current should be on branch2. */
    TEST_ASSERT_EQUAL_STRING("branch2", loaded.current->state.row_chars[0]);

    /* Parent links correct. */
    TEST_ASSERT_EQUAL_PTR(loaded.root, loaded.root->children[0]->parent);
    TEST_ASSERT_EQUAL_PTR(loaded.root, loaded.root->children[1]->parent);
    TEST_ASSERT_NULL(loaded.root->parent);

    undo_tree_free(&orig);
    undo_tree_free(&loaded);
}

/* ── Cursor and description survive round-trip ──────────────────────── */

void test_save_load_preserves_cursor_and_desc(void) {
    create_dummy_file("cur.c");
    UndoTree t;
    undo_tree_init(&t);
    const char *lines[] = {"hello world"};
    undo_tree_set_root(&t, make_state(lines, 1, 42, 99), "my desc");

    undofile_save("cur.c", &t);

    UndoTree loaded = {0};
    undofile_load("cur.c", &loaded);

    TEST_ASSERT_EQUAL_INT(42, loaded.root->state.cx);
    TEST_ASSERT_EQUAL_INT(99, loaded.root->state.cy);
    TEST_ASSERT_EQUAL_STRING("my desc", loaded.root->desc);

    undo_tree_free(&t);
    undo_tree_free(&loaded);
}

/* ── Empty tree: save returns error ─────────────────────────────────── */

void test_save_empty_tree_fails(void) {
    UndoTree t;
    undo_tree_init(&t);
    TEST_ASSERT_EQUAL_INT(-1, undofile_save("nofile.c", &t));
}

/* ── Load nonexistent file returns error ────────────────────────────── */

void test_load_nonexistent_fails(void) {
    UndoTree t = {0};
    TEST_ASSERT_EQUAL_INT(-1, undofile_load("doesnotexist.c", &t));
    TEST_ASSERT_NULL(t.root);
}

/* ── Load corrupt file (bad magic) returns error ────────────────────── */

void test_load_bad_magic_fails(void) {
    create_dummy_file("bad.c");
    /* Write a file with wrong magic. */
    mkdir(".qe", 0755);
    mkdir(".qe/undo", 0755);

    /* We need the undo path — just save a valid file first, then overwrite. */
    UndoTree t;
    undo_tree_init(&t);
    const char *lines[] = {"x"};
    undo_tree_set_root(&t, make_state(lines, 1, 0, 0), "x");
    undofile_save("bad.c", &t);

    /* Now corrupt the magic bytes by re-saving with bad header. */
    /* Find the undo file path by scanning the dir. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ls .qe/undo/*.undo");
    FILE *ls = popen(cmd, "r");
    char path[256] = {0};
    if (ls) { fgets(path, sizeof(path), ls); pclose(ls); }
    /* Trim newline */
    int len = (int)strlen(path);
    if (len > 0 && path[len-1] == '\n') path[len-1] = '\0';

    if (path[0]) {
        FILE *fp = fopen(path, "wb");
        if (fp) { fprintf(fp, "BAAD"); fclose(fp); }
    }

    UndoTree loaded = {0};
    TEST_ASSERT_EQUAL_INT(-1, undofile_load("bad.c", &loaded));
    TEST_ASSERT_NULL(loaded.root);

    undo_tree_free(&t);
}

/* ── undofile_remove deletes the file ───────────────────────────────── */

void test_remove_deletes_file(void) {
    create_dummy_file("rm.c");
    UndoTree t;
    undo_tree_init(&t);
    const char *lines[] = {"data"};
    undo_tree_set_root(&t, make_state(lines, 1, 0, 0), "init");
    undofile_save("rm.c", &t);

    /* File should exist. */
    UndoTree probe = {0};
    TEST_ASSERT_EQUAL_INT(0, undofile_load("rm.c", &probe));
    undo_tree_free(&probe);

    undofile_remove("rm.c");

    /* Now load should fail. */
    UndoTree gone = {0};
    TEST_ASSERT_EQUAL_INT(-1, undofile_load("rm.c", &gone));

    undo_tree_free(&t);
}

/* ── Undo/redo works on a loaded tree ───────────────────────────────── */

void test_undo_redo_after_load(void) {
    create_dummy_file("ur.c");
    UndoTree orig;
    build_linear_tree(&orig);
    undofile_save("ur.c", &orig);
    undo_tree_free(&orig);

    UndoTree loaded = {0};
    undofile_load("ur.c", &loaded);

    /* Current is at edit2 (seq=2). Undo should go to edit1. */
    UndoNode *n = undo_tree_undo(&loaded);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_STRING("edit1", n->desc);
    TEST_ASSERT_EQUAL_STRING("edited one", n->state.row_chars[0]);

    /* Undo again -> root. */
    n = undo_tree_undo(&loaded);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_STRING("open", n->desc);
    TEST_ASSERT_EQUAL_STRING("line one", n->state.row_chars[0]);

    /* Undo at root -> NULL. */
    TEST_ASSERT_NULL(undo_tree_undo(&loaded));

    /* Redo back to edit1. */
    n = undo_tree_redo(&loaded);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_STRING("edit1", n->desc);

    /* Redo to edit2. */
    n = undo_tree_redo(&loaded);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_STRING("edit2", n->desc);

    /* Redo at leaf -> NULL. */
    TEST_ASSERT_NULL(undo_tree_redo(&loaded));

    undo_tree_free(&loaded);
}

/* ── Push new nodes onto a loaded tree ──────────────────────────────── */

void test_push_after_load(void) {
    create_dummy_file("push.c");
    UndoTree orig;
    build_linear_tree(&orig);
    unsigned int orig_next_seq = orig.next_seq;
    undofile_save("push.c", &orig);
    undo_tree_free(&orig);

    UndoTree loaded = {0};
    undofile_load("push.c", &loaded);

    /* next_seq should have been restored, so new pushes don't collide. */
    TEST_ASSERT_EQUAL_UINT32(orig_next_seq, loaded.next_seq);

    const char *new_lines[] = {"new content"};
    UndoNode *pushed = undo_tree_push(&loaded, make_state(new_lines, 1, 0, 0), "edit3");
    TEST_ASSERT_NOT_NULL(pushed);
    TEST_ASSERT_EQUAL_INT(4, loaded.total_nodes);
    TEST_ASSERT_EQUAL_STRING("new content", loaded.current->state.row_chars[0]);

    /* Seq should be monotonically increasing. */
    TEST_ASSERT_TRUE(pushed->seq >= orig_next_seq);

    undo_tree_free(&loaded);
}

/* ── Overwrite: save twice, second save replaces first ──────────────── */

void test_save_overwrites(void) {
    create_dummy_file("ow.c");

    UndoTree t1;
    undo_tree_init(&t1);
    const char *a[] = {"first"};
    undo_tree_set_root(&t1, make_state(a, 1, 0, 0), "v1");
    undofile_save("ow.c", &t1);
    undo_tree_free(&t1);

    UndoTree t2;
    undo_tree_init(&t2);
    const char *b[] = {"second"};
    undo_tree_set_root(&t2, make_state(b, 1, 0, 0), "v2");
    undofile_save("ow.c", &t2);
    undo_tree_free(&t2);

    UndoTree loaded = {0};
    undofile_load("ow.c", &loaded);
    TEST_ASSERT_EQUAL_STRING("second", loaded.root->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("v2", loaded.root->desc);
    undo_tree_free(&loaded);
}

/* ── Empty row content survives round-trip ──────────────────────────── */

void test_save_load_empty_rows(void) {
    create_dummy_file("empty.c");
    UndoTree t;
    undo_tree_init(&t);
    const char *lines[] = {"hello", "", "world"};
    undo_tree_set_root(&t, make_state(lines, 3, 0, 1), "with empty");

    undofile_save("empty.c", &t);

    UndoTree loaded = {0};
    undofile_load("empty.c", &loaded);

    TEST_ASSERT_EQUAL_INT(3, loaded.root->state.numrows);
    TEST_ASSERT_EQUAL_STRING("hello", loaded.root->state.row_chars[0]);
    TEST_ASSERT_EQUAL_STRING("", loaded.root->state.row_chars[1]);
    TEST_ASSERT_EQUAL_INT(0, loaded.root->state.row_lens[1]);
    TEST_ASSERT_EQUAL_STRING("world", loaded.root->state.row_chars[2]);

    undo_tree_free(&t);
    undo_tree_free(&loaded);
}

/* ── Single-node tree (root only) ───────────────────────────────────── */

void test_save_load_single_node(void) {
    create_dummy_file("single.c");
    UndoTree t;
    undo_tree_init(&t);
    const char *lines[] = {"only line"};
    undo_tree_set_root(&t, make_state(lines, 1, 3, 0), "root only");

    undofile_save("single.c", &t);

    UndoTree loaded = {0};
    undofile_load("single.c", &loaded);

    TEST_ASSERT_EQUAL_INT(1, loaded.total_nodes);
    TEST_ASSERT_EQUAL_PTR(loaded.root, loaded.current);
    TEST_ASSERT_NULL(loaded.root->parent);
    TEST_ASSERT_EQUAL_INT(0, loaded.root->num_children);
    TEST_ASSERT_EQUAL_STRING("only line", loaded.root->state.row_chars[0]);

    undo_tree_free(&t);
    undo_tree_free(&loaded);
}

/* ── Current in the middle of the tree ──────────────────────────────── */

void test_save_load_current_mid_tree(void) {
    create_dummy_file("mid.c");
    UndoTree t;
    build_linear_tree(&t);  /* root -> edit1 -> edit2, current=edit2 */
    undo_tree_undo(&t);     /* current = edit1 */

    unsigned int mid_seq = t.current->seq;
    undofile_save("mid.c", &t);

    UndoTree loaded = {0};
    undofile_load("mid.c", &loaded);

    /* Current should be restored to the middle node. */
    TEST_ASSERT_EQUAL_INT(mid_seq, loaded.current->seq);
    TEST_ASSERT_EQUAL_STRING("edit1", loaded.current->desc);

    /* Should still be able to undo and redo from here. */
    UndoNode *parent = undo_tree_undo(&loaded);
    TEST_ASSERT_NOT_NULL(parent);
    TEST_ASSERT_EQUAL_STRING("open", parent->desc);

    undo_tree_redo(&loaded); /* back to edit1 */
    UndoNode *child = undo_tree_redo(&loaded);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQUAL_STRING("edit2", child->desc);

    undo_tree_free(&t);
    undo_tree_free(&loaded);
}

/* ── Branching undo/redo after load ─────────────────────────────────── */

void test_branch_undo_redo_after_load(void) {
    create_dummy_file("bur.c");
    UndoTree orig;
    build_branching_tree(&orig);  /* root -> br1, root -> br2 (current) */
    undofile_save("bur.c", &orig);
    undo_tree_free(&orig);

    UndoTree loaded = {0};
    undofile_load("bur.c", &loaded);

    /* Current = br2. Undo to root. */
    UndoNode *r = undo_tree_undo(&loaded);
    TEST_ASSERT_EQUAL_STRING("root", r->state.row_chars[0]);

    /* Redo should follow the most-recent branch (br2 has higher seq). */
    UndoNode *n = undo_tree_redo(&loaded);
    TEST_ASSERT_EQUAL_STRING("branch2", n->state.row_chars[0]);

    undo_tree_free(&loaded);
}

/* ── Row lengths preserved exactly ──────────────────────────────────── */

void test_save_load_row_lengths(void) {
    create_dummy_file("len.c");
    UndoTree t;
    undo_tree_init(&t);
    const char *lines[] = {"abc", "de", "fghij"};
    undo_tree_set_root(&t, make_state(lines, 3, 0, 0), "lens");
    undofile_save("len.c", &t);

    UndoTree loaded = {0};
    undofile_load("len.c", &loaded);

    TEST_ASSERT_EQUAL_INT(3, loaded.root->state.row_lens[0]);
    TEST_ASSERT_EQUAL_INT(2, loaded.root->state.row_lens[1]);
    TEST_ASSERT_EQUAL_INT(5, loaded.root->state.row_lens[2]);

    undo_tree_free(&t);
    undo_tree_free(&loaded);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* round-trip */
    RUN_TEST(test_save_load_linear);
    RUN_TEST(test_save_load_branching);
    RUN_TEST(test_save_load_preserves_cursor_and_desc);
    RUN_TEST(test_save_load_empty_rows);
    RUN_TEST(test_save_load_single_node);
    RUN_TEST(test_save_load_current_mid_tree);
    RUN_TEST(test_save_load_row_lengths);

    /* error cases */
    RUN_TEST(test_save_empty_tree_fails);
    RUN_TEST(test_load_nonexistent_fails);
    RUN_TEST(test_load_bad_magic_fails);

    /* operations on loaded tree */
    RUN_TEST(test_undo_redo_after_load);
    RUN_TEST(test_push_after_load);
    RUN_TEST(test_branch_undo_redo_after_load);

    /* misc */
    RUN_TEST(test_remove_deletes_file);
    RUN_TEST(test_save_overwrites);

    return UNITY_END();
}
