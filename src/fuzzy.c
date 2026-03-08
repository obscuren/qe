// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuzzy.h"
#include "editor.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── File scanning ───────────────────────────────────────────────────── */

static const char *SKIP_DIRS[] = {
    "node_modules", "target", "build", ".cache", ".idea", NULL
};

static int should_skip_dir(const char *name) {
    if (name[0] == '.') return 1;   /* all hidden dirs */
    for (int i = 0; SKIP_DIRS[i]; i++)
        if (strcmp(name, SKIP_DIRS[i]) == 0) return 1;
    return 0;
}

static void scan_dir(const char *rel, int rel_len) {
    FuzzyState *f = &E.fuzzy;

    DIR *d = opendir(rel_len > 0 ? rel : ".");
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char entry[512];
        int  elen;
        if (rel_len > 0)
            elen = snprintf(entry, sizeof(entry), "%s/%s", rel, de->d_name);
        else
            elen = snprintf(entry, sizeof(entry), "%s", de->d_name);

        if (elen >= (int)sizeof(entry)) continue;

        struct stat st;
        if (stat(entry, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (!should_skip_dir(de->d_name) && f->all_count < FUZZY_MAX_FILES)
                scan_dir(entry, elen);
        } else if (S_ISREG(st.st_mode)) {
            if (f->all_count < FUZZY_MAX_FILES)
                f->all_files[f->all_count++] = strdup(entry);
        }
    }
    closedir(d);
}

/* ── Scoring ─────────────────────────────────────────────────────────── */

/* Returns score >= 0 if query is a subsequence of path, -1 otherwise.
   Matched byte positions (in path) are written to pos[0..qlen-1]. */
static int fuzzy_score(const char *path, const char *query, int qlen, int *pos) {
    if (qlen == 0) return 100;

    int plen = (int)strlen(path);
    int qi = 0, pi = 0, count = 0;

    while (qi < qlen && pi < plen) {
        char pc = path[pi], qc = query[qi];
        if (pc >= 'A' && pc <= 'Z') pc += 32;
        if (qc >= 'A' && qc <= 'Z') qc += 32;
        if (pc == qc) { pos[count++] = pi; qi++; }
        pi++;
    }
    if (qi < qlen) return -1;

    int score = 0;
    for (int i = 0; i < count; i++) {
        score += 10;
        if (i > 0 && pos[i] == pos[i-1] + 1) score += 15;  /* consecutive */
        char prev = (pos[i] > 0) ? path[pos[i]-1] : '/';
        if (prev == '/' || prev == '_' || prev == '-' || prev == '.')
            score += 20;  /* word boundary */
    }
    score -= plen / 10;    /* prefer shorter paths */
    return score;
}

static int match_cmp(const void *a, const void *b) {
    return ((const FuzzyMatch *)b)->score - ((const FuzzyMatch *)a)->score;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void fuzzy_filter(void) {
    FuzzyState *f = &E.fuzzy;
    f->match_count = 0;

    int tmp[256];
    for (int i = 0; i < f->all_count; i++) {
        int score = fuzzy_score(f->all_files[i], f->query, f->query_len, tmp);
        if (score < 0) continue;
        FuzzyMatch *m = &f->matches[f->match_count++];
        strncpy(m->path, f->all_files[i], sizeof(m->path) - 1);
        m->path[sizeof(m->path)-1] = '\0';
        m->score = score;
        m->orig_idx = i;
        m->match_count = f->query_len < 256 ? f->query_len : 255;
        memcpy(m->match_pos, tmp, sizeof(int) * m->match_count);
    }
    qsort(f->matches, f->match_count, sizeof(FuzzyMatch), match_cmp);

    if (f->selected >= f->match_count)
        f->selected = f->match_count > 0 ? f->match_count - 1 : 0;
    if (f->selected < f->scroll)
        f->scroll = f->selected;
    if (f->selected >= f->scroll + FUZZY_MAX_VIS)
        f->scroll = f->selected - FUZZY_MAX_VIS + 1;
}

void fuzzy_open(void) {
    FuzzyState *f = &E.fuzzy;
    memset(f, 0, sizeof(*f));
    f->all_files = malloc(sizeof(char *) * FUZZY_MAX_FILES);
    f->matches   = malloc(sizeof(FuzzyMatch) * FUZZY_MAX_FILES);
    if (!f->all_files || !f->matches) { fuzzy_close(); return; }
    scan_dir("", 0);
    fuzzy_filter();
    E.mode = MODE_FUZZY;
}

void fuzzy_open_buffers(void) {
    FuzzyState *f = &E.fuzzy;
    memset(f, 0, sizeof(*f));
    f->buf_mode   = 1;
    f->all_files  = malloc(sizeof(char *) * FUZZY_MAX_FILES);
    f->matches    = malloc(sizeof(FuzzyMatch) * FUZZY_MAX_FILES);
    f->buf_indices = malloc(sizeof(int) * FUZZY_MAX_FILES);
    if (!f->all_files || !f->matches || !f->buf_indices) { fuzzy_close(); return; }

    for (int i = 0; i < E.num_buftabs && f->all_count < FUZZY_MAX_FILES; i++) {
        /* Skip tree, quickfix, and other special buffers. */
        BufTab *bt = &E.buftabs[i];
        if (buftab_is_special(bt)) continue;

        const char *fn = (i == E.cur_buftab)
            ? (E.buf.filename ? E.buf.filename : "[No Name]")
            : (bt->buf.filename ? bt->buf.filename : "[No Name]");
        int dirty = (i == E.cur_buftab) ? E.buf.dirty : bt->buf.dirty;

        char label[520];
        snprintf(label, sizeof(label), "%s%s", fn, dirty ? " [+]" : "");
        f->buf_indices[f->all_count] = i;
        f->all_files[f->all_count++] = strdup(label);
    }
    fuzzy_filter();
    E.mode = MODE_FUZZY;
}

void fuzzy_close(void) {
    FuzzyState *f = &E.fuzzy;
    if (f->all_files) {
        for (int i = 0; i < f->all_count; i++) free(f->all_files[i]);
        free(f->all_files);
    }
    free(f->matches);
    free(f->buf_indices);
    memset(f, 0, sizeof(*f));
    E.mode = MODE_NORMAL;
}
