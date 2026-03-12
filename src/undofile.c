// SPDX-License-Identifier: GPL-3.0-or-later
#include "undofile.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Binary format:
 *   Header:  "QEU\x01" (4B) | next_seq (4B) | n_nodes (4B) | cur_seq (4B)
 *   Per node (seq-sorted):
 *     seq (4B) | parent_seq (4B, 0xFFFFFFFF=root) | desc (64B) |
 *     cx (4B) | cy (4B) | numrows (4B) |
 *     per row: len (4B) + chars (len bytes)
 */

#define UNDO_MAGIC  "QEU\x01"
#define UNDO_MAGIC_LEN 4
#define NO_PARENT   0xFFFFFFFFu

/* ── Path helpers ────────────────────────────────────────────────── */

/* DJB2 hash of a string, returned as hex. */
static void path_to_hash(const char *filepath, char *out, size_t outsz) {
    /* Resolve to absolute path. */
    char abspath[PATH_MAX];
    if (realpath(filepath, abspath))
        filepath = abspath;

    unsigned long h = 5381;
    for (const char *p = filepath; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;

    snprintf(out, outsz, "%016lx", h);
}

static void undo_filepath(const char *filepath, char *out, size_t outsz) {
    char hash[32];
    path_to_hash(filepath, hash, sizeof(hash));
    snprintf(out, outsz, ".qe/undo/%s.undo", hash);
}

static int ensure_undo_dir(void) {
    mkdir(".qe", 0755);
    return mkdir(".qe/undo", 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* ── Write helpers ───────────────────────────────────────────────── */

static int write_u32(FILE *fp, uint32_t v) {
    return fwrite(&v, 4, 1, fp) == 1 ? 0 : -1;
}

static int write_i32(FILE *fp, int32_t v) {
    return fwrite(&v, 4, 1, fp) == 1 ? 0 : -1;
}

/* ── Read helpers ────────────────────────────────────────────────── */

static int read_u32(FILE *fp, uint32_t *v) {
    return fread(v, 4, 1, fp) == 1 ? 0 : -1;
}

static int read_i32(FILE *fp, int32_t *v) {
    return fread(v, 4, 1, fp) == 1 ? 0 : -1;
}

/* ── Public API ──────────────────────────────────────────────────── */

int undofile_save(const char *filepath, const UndoTree *tree) {
    if (!tree->root) return -1;

    if (ensure_undo_dir() != 0) return -1;

    char path[PATH_MAX];
    undo_filepath(filepath, path, sizeof(path));

    UndoNode **nodes = NULL;
    int count = undo_tree_flatten(tree, &nodes);
    if (count <= 0) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(nodes); return -1; }

    /* Header */
    fwrite(UNDO_MAGIC, 1, UNDO_MAGIC_LEN, fp);
    write_u32(fp, tree->next_seq);
    write_u32(fp, (uint32_t)count);
    write_u32(fp, tree->current ? tree->current->seq : 0);

    /* Nodes */
    for (int i = 0; i < count; i++) {
        UndoNode *n = nodes[i];
        write_u32(fp, n->seq);
        write_u32(fp, n->parent ? n->parent->seq : NO_PARENT);

        /* desc: fixed 64 bytes, null-padded */
        char desc[UNDO_DESC_MAX];
        memset(desc, 0, sizeof(desc));
        memcpy(desc, n->desc, strlen(n->desc));
        fwrite(desc, 1, UNDO_DESC_MAX, fp);

        write_i32(fp, n->state.cx);
        write_i32(fp, n->state.cy);
        write_i32(fp, n->state.numrows);

        for (int r = 0; r < n->state.numrows; r++) {
            int32_t len = n->state.row_lens[r];
            write_i32(fp, len);
            if (len > 0)
                fwrite(n->state.row_chars[r], 1, len, fp);
        }
    }

    free(nodes);
    fclose(fp);
    return 0;
}

int undofile_load(const char *filepath, UndoTree *tree) {
    char path[PATH_MAX];
    undo_filepath(filepath, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    /* Validate magic */
    char magic[UNDO_MAGIC_LEN];
    if (fread(magic, 1, UNDO_MAGIC_LEN, fp) != UNDO_MAGIC_LEN ||
        memcmp(magic, UNDO_MAGIC, UNDO_MAGIC_LEN) != 0) {
        fclose(fp);
        return -1;
    }

    uint32_t next_seq, n_nodes, cur_seq;
    if (read_u32(fp, &next_seq) || read_u32(fp, &n_nodes) || read_u32(fp, &cur_seq)) {
        fclose(fp);
        return -1;
    }

    if (n_nodes == 0 || n_nodes > 10000) { fclose(fp); return -1; }

    /* Read all nodes into temporary arrays. */
    uint32_t *seqs       = calloc(n_nodes, sizeof(uint32_t));
    uint32_t *parent_seqs = calloc(n_nodes, sizeof(uint32_t));
    UndoNode **nodes     = calloc(n_nodes, sizeof(UndoNode *));
    if (!seqs || !parent_seqs || !nodes) goto fail;

    for (uint32_t i = 0; i < n_nodes; i++) {
        if (read_u32(fp, &seqs[i]) || read_u32(fp, &parent_seqs[i]))
            goto fail;

        char desc[UNDO_DESC_MAX];
        if (fread(desc, 1, UNDO_DESC_MAX, fp) != UNDO_DESC_MAX) goto fail;

        int32_t cx, cy, numrows;
        if (read_i32(fp, &cx) || read_i32(fp, &cy) || read_i32(fp, &numrows))
            goto fail;

        if (numrows < 0 || numrows > 1000000) goto fail;

        UndoState state;
        state.cx = cx;
        state.cy = cy;
        state.numrows = numrows;
        state.row_chars = malloc(sizeof(char *) * (numrows > 0 ? numrows : 1));
        state.row_lens  = malloc(sizeof(int)    * (numrows > 0 ? numrows : 1));
        if (!state.row_chars || !state.row_lens) {
            free(state.row_chars);
            free(state.row_lens);
            goto fail;
        }

        int row_ok = 1;
        for (int r = 0; r < numrows; r++) {
            int32_t len;
            if (read_i32(fp, &len) || len < 0 || len > 10000000) {
                /* Clean up partially-read rows */
                for (int k = 0; k < r; k++) free(state.row_chars[k]);
                free(state.row_chars);
                free(state.row_lens);
                row_ok = 0;
                break;
            }
            state.row_lens[r] = len;
            state.row_chars[r] = malloc(len + 1);
            if (!state.row_chars[r]) {
                for (int k = 0; k < r; k++) free(state.row_chars[k]);
                free(state.row_chars);
                free(state.row_lens);
                row_ok = 0;
                break;
            }
            if (len > 0 && (int)fread(state.row_chars[r], 1, len, fp) != len) {
                for (int k = 0; k <= r; k++) free(state.row_chars[k]);
                free(state.row_chars);
                free(state.row_lens);
                row_ok = 0;
                break;
            }
            state.row_chars[r][len] = '\0';
        }
        if (!row_ok) goto fail;

        /* Create the node. */
        UndoNode *n = calloc(1, sizeof(UndoNode));
        if (!n) {
            undo_state_free(&state);
            goto fail;
        }
        n->state = state;
        n->seq = seqs[i];
        strncpy(n->desc, desc, UNDO_DESC_MAX - 1);
        nodes[i] = n;
    }

    /* Rebuild parent-child links. */
    UndoNode *root = NULL;
    UndoNode *current = NULL;

    for (uint32_t i = 0; i < n_nodes; i++) {
        if (parent_seqs[i] == NO_PARENT) {
            root = nodes[i];
        } else {
            /* Find parent by seq. */
            UndoNode *parent = NULL;
            for (uint32_t j = 0; j < n_nodes; j++) {
                if (seqs[j] == parent_seqs[i]) {
                    parent = nodes[j];
                    break;
                }
            }
            if (!parent) goto fail;

            /* Add child. */
            if (nodes[i]) {
                int nc = parent->num_children;
                if (nc >= parent->cap_children) {
                    int newcap = parent->cap_children < 2 ? 2 : parent->cap_children * 2;
                    parent->children = realloc(parent->children,
                                               sizeof(UndoNode *) * newcap);
                    parent->cap_children = newcap;
                }
                nodes[i]->parent = parent;
                parent->children[nc] = nodes[i];
                parent->num_children++;
            }
        }
        if (seqs[i] == cur_seq) current = nodes[i];
    }

    if (!root) goto fail;

    fclose(fp);
    free(seqs);
    free(parent_seqs);
    free(nodes);

    undo_tree_init(tree);
    tree->root = root;
    tree->current = current ? current : root;
    tree->next_seq = next_seq;
    tree->total_nodes = (int)n_nodes;
    return 0;

fail:
    fclose(fp);
    /* Free any nodes that were created. */
    if (nodes) {
        for (uint32_t i = 0; i < n_nodes; i++) {
            if (nodes[i]) {
                undo_state_free(&nodes[i]->state);
                free(nodes[i]->children);
                free(nodes[i]);
            }
        }
    }
    free(seqs);
    free(parent_seqs);
    free(nodes);
    return -1;
}

void undofile_remove(const char *filepath) {
    char path[PATH_MAX];
    undo_filepath(filepath, path, sizeof(path));
    unlink(path);
}
