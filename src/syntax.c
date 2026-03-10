// SPDX-License-Identifier: GPL-3.0-or-later
#include "syntax.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ── Registry ────────────────────────────────────────────────────────── */

#define MAX_SYNTAXES 64

static SyntaxDef *registry[MAX_SYNTAXES];
static int        num_syntaxes;

void syntax_register(SyntaxDef *def) {
    if (num_syntaxes < MAX_SYNTAXES)
        registry[num_syntaxes++] = def;
}

const SyntaxDef *syntax_detect(const char *filename) {
    if (!filename) return NULL;

    /* Find the last '.' to get the extension. */
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return NULL;
    const char *ext = dot + 1;

    for (int i = num_syntaxes - 1; i >= 0; i--) {   /* newest first: user defs win */
        SyntaxDef *def = registry[i];
        for (int j = 0; j < def->num_filetypes; j++) {
            if (strcmp(def->filetypes[j], ext) == 0)
                return def;
        }
    }
    return NULL;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Returns 1 if line[pos..] starts with the given prefix. */
static int starts_with(const char *line, int len, int pos,
                        const char *prefix, int prefix_len) {
    if (prefix_len <= 0 || pos + prefix_len > len) return 0;
    return memcmp(&line[pos], prefix, prefix_len) == 0;
}

/* ── Light scan ──────────────────────────────────────────────────────── */

int syntax_scan_row(const SyntaxDef *def, const Row *row, int open_comment) {
    if (!def) return open_comment;

    const char *line = row->chars;
    int         len  = row->len;

    int ml_sl = def->comment_ml_start ? (int)strlen(def->comment_ml_start) : 0;
    int ml_el = def->comment_ml_end   ? (int)strlen(def->comment_ml_end)   : 0;
    int sl_l  = def->comment_single   ? (int)strlen(def->comment_single)   : 0;

    int in_ml     = open_comment;
    int in_string = 0;
    char sc       = 0;

    for (int pos = 0; pos < len; ) {
        char c = line[pos];

        if (in_ml) {
            if (ml_el > 0 && starts_with(line, len, pos, def->comment_ml_end, ml_el)) {
                in_ml = 0;
                pos  += ml_el;
            } else {
                pos++;
            }
            continue;
        }

        if (in_string) {
            if (c == '\\' && pos + 1 < len) { pos += 2; continue; }
            if (c == sc) in_string = 0;
            pos++;
            continue;
        }

        /* Single-line comment ends the scan. */
        if (sl_l > 0 && starts_with(line, len, pos, def->comment_single, sl_l))
            break;

        /* Multi-line comment start. */
        if (ml_sl > 0 && starts_with(line, len, pos, def->comment_ml_start, ml_sl)) {
            in_ml = 1;
            pos  += ml_sl;
            continue;
        }

        if (c == '"' || c == '\'') { in_string = 1; sc = c; }
        pos++;
    }

    return in_ml;
}

/* ── Open-comment cascade ────────────────────────────────────────────── */

void syntax_update_open_comments(const SyntaxDef *def, Buffer *buf, int from) {
    if (!def || !def->comment_ml_start || buf->numrows == 0) return;
    if (from < 0) from = 0;

    /* Reseed the starting row's hl_open_comment from the previous row's
       end-state.  This is needed when a row is inserted or deleted so that
       the new row gets the correct incoming state. */
    if (from > 0) {
        int prev_end = syntax_scan_row(def, &buf->rows[from - 1],
                                        buf->rows[from - 1].hl_open_comment);
        buf->rows[from].hl_open_comment = prev_end;
    }

    for (int i = from; i < buf->numrows; i++) {
        int state_after = syntax_scan_row(def, &buf->rows[i],
                                           buf->rows[i].hl_open_comment);
        if (i + 1 >= buf->numrows) break;

        /* Early exit: once the next row's state is already correct we're done. */
        if (i > from && buf->rows[i + 1].hl_open_comment == state_after)
            break;

        buf->rows[i + 1].hl_open_comment = state_after;
    }
}

/* ── Full-row tokeniser ───────────────────────────────────────────────── */

int syntax_highlight_row(const SyntaxDef *def, Row *row, int open_comment) {
    if (!def || row->len == 0) return open_comment;

    unsigned char *hl = realloc(row->hl, row->len);
    if (!hl) return open_comment;
    row->hl = hl;
    memset(hl, HL_NORMAL, row->len);

    const char *line = row->chars;
    int         len  = row->len;

    int ml_sl = def->comment_ml_start ? (int)strlen(def->comment_ml_start) : 0;
    int ml_el = def->comment_ml_end   ? (int)strlen(def->comment_ml_end)   : 0;
    int sl_l  = def->comment_single   ? (int)strlen(def->comment_single)   : 0;

    int  in_ml     = open_comment;
    int  in_string = 0;
    char sc        = 0;

    int pos = 0;
    while (pos < len) {
        char c = line[pos];

        /* ── Inside a multi-line comment ─────────────────────────────── */
        if (in_ml) {
            hl[pos] = HL_COMMENT;
            if (ml_el > 0 && starts_with(line, len, pos, def->comment_ml_end, ml_el)) {
                for (int k = 1; k < ml_el && pos + k < len; k++)
                    hl[pos + k] = HL_COMMENT;
                in_ml = 0;
                pos  += ml_el;
            } else {
                pos++;
            }
            continue;
        }

        /* ── Inside a string literal ─────────────────────────────────── */
        if (in_string) {
            hl[pos] = HL_STRING;
            if (c == '\\' && pos + 1 < len) {
                hl[pos + 1] = HL_STRING;
                pos += 2;
                continue;
            }
            if (c == sc) in_string = 0;
            pos++;
            continue;
        }

        /* ── Single-line comment ─────────────────────────────────────── */
        if (sl_l > 0 && starts_with(line, len, pos, def->comment_single, sl_l)) {
            memset(&hl[pos], HL_COMMENT, len - pos);
            break;
        }

        /* ── Multi-line comment open ─────────────────────────────────── */
        if (ml_sl > 0 && starts_with(line, len, pos, def->comment_ml_start, ml_sl)) {
            for (int k = 0; k < ml_sl && pos + k < len; k++)
                hl[pos + k] = HL_COMMENT;
            in_ml = 1;
            pos  += ml_sl;
            continue;
        }

        /* ── String / char literal ───────────────────────────────────── */
        if (c == '"' || c == '\'') {
            hl[pos]    = HL_STRING;
            in_string  = 1;
            sc         = c;
            pos++;
            continue;
        }

        /* ── Number: digit not preceded by identifier char ───────────── */
        if (isdigit((unsigned char)c)) {
            int prev_alnum = pos > 0 &&
                (isalnum((unsigned char)line[pos - 1]) || line[pos - 1] == '_');
            if (!prev_alnum) {
                while (pos < len &&
                       (isalnum((unsigned char)line[pos]) || line[pos] == '.'))
                    hl[pos++] = HL_NUMBER;
                continue;
            }
        }

        /* ── Identifier → keyword or type ───────────────────────────── */
        if (isalpha((unsigned char)c) || c == '_') {
            int start = pos;
            while (pos < len &&
                   (isalnum((unsigned char)line[pos]) || line[pos] == '_'))
                pos++;
            int ident_len = pos - start;

            HlType t = HL_NORMAL;
            for (int k = 0; k < def->num_keywords && t == HL_NORMAL; k++) {
                if ((int)strlen(def->keywords[k]) == ident_len &&
                    memcmp(&line[start], def->keywords[k], ident_len) == 0)
                    t = HL_KEYWORD;
            }
            for (int k = 0; k < def->num_types && t == HL_NORMAL; k++) {
                if ((int)strlen(def->types[k]) == ident_len &&
                    memcmp(&line[start], def->types[k], ident_len) == 0)
                    t = HL_TYPE;
            }
            memset(&hl[start], t, ident_len);
            continue;
        }

        pos++;
    }

    return in_ml;
}
