// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery.h"
#include "editor.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Binary format:
 *   "QER\x01"    (4 bytes magic)
 *   timestamp     (8 bytes, time_t of when recovery was written)
 *   filepath_len  (4 bytes)
 *   filepath      (filepath_len bytes, original absolute path)
 *   cx            (4 bytes)
 *   cy            (4 bytes)
 *   numrows       (4 bytes)
 *   Per row: len (4 bytes) + chars (len bytes)
 */

#define REC_MAGIC     "QER\x01"
#define REC_MAGIC_LEN 4

/* ── Path helpers ────────────────────────────────────────────────── */

static void path_to_hash(const char *filepath, char *out, size_t outsz) {
    char abspath[PATH_MAX];
    if (realpath(filepath, abspath))
        filepath = abspath;

    unsigned long h = 5381;
    for (const char *p = filepath; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;

    snprintf(out, outsz, "%016lx", h);
}

static void rec_filepath(const char *filepath, char *out, size_t outsz) {
    char hash[32];
    path_to_hash(filepath, hash, sizeof(hash));
    snprintf(out, outsz, ".qe/recovery/%s.rec", hash);
}

static int ensure_rec_dir(void) {
    mkdir(".qe", 0755);
    return mkdir(".qe/recovery", 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* ── Write/read helpers ──────────────────────────────────────────── */

static int write_u32(FILE *fp, uint32_t v) {
    return fwrite(&v, 4, 1, fp) == 1 ? 0 : -1;
}

static int write_i32(FILE *fp, int32_t v) {
    return fwrite(&v, 4, 1, fp) == 1 ? 0 : -1;
}

static int read_u32(FILE *fp, uint32_t *v) {
    return fread(v, 4, 1, fp) == 1 ? 0 : -1;
}

static int read_i32(FILE *fp, int32_t *v) {
    return fread(v, 4, 1, fp) == 1 ? 0 : -1;
}

/* ── Public API ──────────────────────────────────────────────────── */

int recovery_save(const char *filepath, const Buffer *b, int cx, int cy) {
    if (!filepath) return -1;
    if (ensure_rec_dir() != 0) return -1;

    char path[PATH_MAX];
    rec_filepath(filepath, path, sizeof(path));

    /* Resolve to absolute path for embedding in the file. */
    char abspath[PATH_MAX];
    const char *stored = filepath;
    if (realpath(filepath, abspath))
        stored = abspath;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    /* Header */
    fwrite(REC_MAGIC, 1, REC_MAGIC_LEN, fp);

    /* Timestamp */
    int64_t ts = (int64_t)time(NULL);
    fwrite(&ts, 8, 1, fp);

    /* Embedded filepath */
    uint32_t flen = (uint32_t)strlen(stored);
    write_u32(fp, flen);
    fwrite(stored, 1, flen, fp);

    /* Cursor + rows */
    write_i32(fp, cx);
    write_i32(fp, cy);
    write_i32(fp, b->numrows);

    for (int i = 0; i < b->numrows; i++) {
        int32_t len = b->rows[i].len;
        write_i32(fp, len);
        if (len > 0)
            fwrite(b->rows[i].chars, 1, len, fp);
    }

    fclose(fp);
    return 0;
}

int recovery_exists(const char *filepath) {
    if (!filepath) return 0;

    char path[PATH_MAX];
    rec_filepath(filepath, path, sizeof(path));

    struct stat rec_st;
    if (stat(path, &rec_st) != 0) return 0;

    struct stat file_st;
    if (stat(filepath, &file_st) != 0) return 1; /* file gone, recovery exists */

    /* Recovery file must be newer than the file on disk. */
    return rec_st.st_mtime > file_st.st_mtime;
}

int recovery_load(const char *filepath, Buffer *b, int *cx, int *cy) {
    if (!filepath) return -1;

    char path[PATH_MAX];
    rec_filepath(filepath, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    /* Validate magic */
    char magic[REC_MAGIC_LEN];
    if (fread(magic, 1, REC_MAGIC_LEN, fp) != REC_MAGIC_LEN ||
        memcmp(magic, REC_MAGIC, REC_MAGIC_LEN) != 0) {
        fclose(fp);
        return -1;
    }

    /* Skip timestamp */
    int64_t ts;
    if (fread(&ts, 8, 1, fp) != 1) { fclose(fp); return -1; }

    /* Skip embedded filepath */
    uint32_t flen;
    if (read_u32(fp, &flen) || flen > PATH_MAX) { fclose(fp); return -1; }
    if (fseek(fp, flen, SEEK_CUR) != 0) { fclose(fp); return -1; }

    /* Cursor */
    int32_t rcx, rcy, numrows;
    if (read_i32(fp, &rcx) || read_i32(fp, &rcy) || read_i32(fp, &numrows)) {
        fclose(fp);
        return -1;
    }
    if (numrows < 0 || numrows > 10000000) { fclose(fp); return -1; }

    /* Clear existing buffer content and load recovery rows. */
    buf_clear_rows(b);
    b->numrows = 0;
    b->rows = NULL;

    if (numrows > 0) {
        b->rows = malloc(sizeof(Row) * numrows);
        if (!b->rows) { fclose(fp); return -1; }

        for (int i = 0; i < numrows; i++) {
            int32_t len;
            if (read_i32(fp, &len) || len < 0 || len > 10000000) {
                /* Clean up partially-read rows */
                for (int k = 0; k < i; k++) {
                    free(b->rows[k].chars);
                    free(b->rows[k].hl);
                }
                free(b->rows);
                b->rows = NULL;
                fclose(fp);
                return -1;
            }
            b->rows[i].chars = malloc(len + 1);
            if (!b->rows[i].chars) {
                for (int k = 0; k < i; k++) {
                    free(b->rows[k].chars);
                    free(b->rows[k].hl);
                }
                free(b->rows);
                b->rows = NULL;
                fclose(fp);
                return -1;
            }
            if (len > 0 && (int)fread(b->rows[i].chars, 1, len, fp) != len) {
                free(b->rows[i].chars);
                for (int k = 0; k < i; k++) {
                    free(b->rows[k].chars);
                    free(b->rows[k].hl);
                }
                free(b->rows);
                b->rows = NULL;
                fclose(fp);
                return -1;
            }
            b->rows[i].chars[len] = '\0';
            b->rows[i].len = len;
            b->rows[i].hl = NULL;
            b->rows[i].hl_open_comment = 0;
        }
        b->numrows = numrows;
    }

    b->dirty = 1;
    b->hl_dirty_from = 0;

    *cx = rcx;
    *cy = rcy;

    fclose(fp);
    return 0;
}

void recovery_remove(const char *filepath) {
    if (!filepath) return;
    char path[PATH_MAX];
    rec_filepath(filepath, path, sizeof(path));
    unlink(path);
}

#ifndef QE_TEST
void recovery_tick(void) {
    for (int i = 0; i < E.num_buftabs; i++) {
        if (buftab_is_special(&E.buftabs[i])) continue;
        Buffer *b = (i == E.cur_buftab) ? &E.buf : &E.buftabs[i].buf;
        if (!b->filename || !b->dirty) continue;
        int cx = (i == E.cur_buftab) ? E.cx : E.buftabs[i].cx;
        int cy = (i == E.cur_buftab) ? E.cy : E.buftabs[i].cy;
        recovery_save(b->filename, b, cx, cy);
    }
}
#endif
