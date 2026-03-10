// SPDX-License-Identifier: GPL-3.0-or-later
#include "render.h"
#include "editor.h"
#include "fuzzy.h"
#include "git.h"
#include "qf.h"
#include "search.h"
#include "syntax.h"
#include "term_emu.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Append buffer ───────────────────────────────────────────────────── */

typedef struct {
    char *b;
    int   len;
} AppendBuf;

#define ABUF_INIT {NULL, 0}

static void ab_append(AppendBuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (!new) return;
    memcpy(&new[ab->len], s, len);
    ab->b   = new;
    ab->len += len;
}

static void ab_free(AppendBuf *ab) {
    free(ab->b);
}

/* ── Gutter ──────────────────────────────────────────────────────────── */

/* Returns 1 if any mark exists for the given buffer slot. */
static int buf_has_marks(int buf_idx) {
    for (int i = 0; i < MARK_MAX; i++)
        if (E.marks[i].valid && E.marks[i].buf_idx == buf_idx) return 1;
    return 0;
}

/* Returns the mark letter for a given row/buf, or 0 if none. */
static char mark_char_at(int buf_idx, int row) {
    for (int i = 0; i < MARK_MAX; i++)
        if (E.marks[i].valid && E.marks[i].buf_idx == buf_idx && E.marks[i].row == row)
            return 'a' + i;
    return 0;
}

/* Width of the line-number gutter for a specific buffer.
   When marks exist for that buffer, prepends 2 extra columns (mark + space).
   When git signs exist, prepends 1 extra column. */
static int gutter_width_for(const Buffer *buf, int buf_idx) {
    if (!E.opts.line_numbers || buf->numrows == 0) return 0;
    int n = buf->numrows, w = 0;
    while (n > 0) { w++; n /= 10; }
    w++;  /* trailing space */
    if (buf_has_marks(buf_idx)) w += 2;
    if (buf->git_signs) w += 1;
    return w;
}

/* Gutter width for the active (live) buffer. */
static int gutter_width(void) { return gutter_width_for(&E.buf, E.cur_buftab); }

/* ── Scroll ──────────────────────────────────────────────────────────── */

/* Adjust rowoff/coloff so the cursor stays within the visible viewport.
   Horizontal scroll uses the content width (screen minus gutter). */
/* Count visible screen rows between file rows `from` and `to` (inclusive of from,
   exclusive of to).  Closed folds count as 1 visible row. */
static int visible_rows_between(int from, int to) {
    int count = 0;
    int r = from;
    while (r < to && r < E.buf.numrows) {
        count++;
        if (E.buf.folds && r < E.buf.folds_cap && E.buf.folds[r]) {
            r = buf_fold_end(&E.buf, r, E.opts.tabwidth) + 1;
        } else {
            r++;
        }
    }
    return count;
}

/* Walk `n` visible rows backward from `start`, returning the file row. */
static int visible_row_back(int start, int n) {
    int r = start;
    while (n > 0 && r > 0) {
        r--;
        /* If r is inside a fold, jump to the fold header. */
        if (E.buf.folds) {
            for (int f = r; f >= 0; f--) {
                if (f < E.buf.folds_cap && E.buf.folds[f]) {
                    int end = buf_fold_end(&E.buf, f, E.opts.tabwidth);
                    if (r <= end) { r = f; break; }
                    break;
                }
            }
        }
        n--;
    }
    return r;
}

static void editor_scroll(void) {
    int content_cols = E.screencols - gutter_width();
    int so = E.opts.scrolloff;
    /* Clamp scrolloff so it can't exceed half the screen. */
    if (so > E.screenrows / 2) so = E.screenrows / 2;

    if (E.cy < E.rowoff + so)
        E.rowoff = E.cy - so;
    if (E.rowoff < 0) E.rowoff = 0;

    /* Count visible rows from rowoff to cy; if > screenrows-scrolloff, scroll down. */
    int vis = visible_rows_between(E.rowoff, E.cy);
    if (vis >= E.screenrows - so)
        E.rowoff = visible_row_back(E.cy, E.screenrows - 1 - so);

    /* Horizontal scroll uses visual column so tabs scroll correctly. */
    int vcx = 0;
    if (E.buf.numrows > 0 && E.cy < E.buf.numrows)
        vcx = col_to_vcol(&E.buf.rows[E.cy], E.cx, E.opts.tabwidth);

    if (vcx < E.coloff)
        E.coloff = vcx;
    if (vcx >= E.coloff + content_cols)
        E.coloff = vcx - content_cols + 1;
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

static const char *SPLASH[] = {
    "Quick Ed",
    "",
    "Version 0.1",
    "By Jeff & Claude",
    "Qe is open source and freely distributable",
    "",
    "Type  :q<Enter>             to exit",
    "Type  :w <File>             to write"
};
#define SPLASH_LINES ((int)(sizeof(SPLASH) / sizeof(SPLASH[0])))

/* Map a HlType to its ANSI escape code, or NULL for HL_NORMAL. */
static const char *hl_to_escape(unsigned char hl) {
    switch ((HlType)hl) {
        case HL_COMMENT: return "\x1b[2;36m";  /* dim cyan    */
        case HL_KEYWORD: return "\x1b[1;33m";  /* bold yellow */
        case HL_TYPE:    return "\x1b[36m";    /* cyan        */
        case HL_STRING:  return "\x1b[32m";    /* green       */
        case HL_NUMBER:  return "\x1b[35m";    /* magenta     */
        case HL_SEARCH:        return "\x1b[7m";    /* reverse      */
        case HL_BRACKET_MATCH: return "\x1b[104;97m"; /* bright blue bg + white fg */
        case HL_VISUAL:        return "\x1b[44m";     /* blue background           */
        default:               return NULL;
    }
}

/* For a given file row, compute the visual-selection column range [*c0, *c1).
   vis_anchor_row/col and cur_row/col are the selection bounds (from active pane).
   Returns 1 if the row is (at least partially) selected, 0 otherwise. */
static int visual_col_range_for(int filerow, int vis_anchor_row, int vis_anchor_col,
                                 int cur_row, int cur_col,
                                 EditorMode mode, int *c0, int *c1) {
    if (mode != MODE_VISUAL && mode != MODE_VISUAL_LINE
        && mode != MODE_VISUAL_BLOCK) return 0;

    int ar = vis_anchor_row, ac = vis_anchor_col;
    int cr = cur_row,        cc = cur_col;
    int r0 = ar < cr ? ar : cr;
    int r1 = ar > cr ? ar : cr;
    if (filerow < r0 || filerow > r1) return 0;

    if (mode == MODE_VISUAL_LINE) {
        *c0 = 0; *c1 = INT_MAX;
        return 1;
    }

    if (mode == MODE_VISUAL_BLOCK) {
        /* Block mode: same column range on every row. */
        int lc = ac < cc ? ac : cc;
        int rc = ac > cc ? ac : cc;
        *c0 = lc; *c1 = rc + 1;
        return 1;
    }

    int sr, sc, er, ec;
    if (ar < cr || (ar == cr && ac <= cc)) {
        sr = ar; sc = ac; er = cr; ec = cc;
    } else {
        sr = cr; sc = cc; er = ar; ec = ac;
    }

    if      (filerow == sr && filerow == er) { *c0 = sc;  *c1 = ec + 1;   }
    else if (filerow == sr)                  { *c0 = sc;  *c1 = INT_MAX;  }
    else if (filerow == er)                  { *c0 = 0;   *c1 = ec + 1;   }
    else                                     { *c0 = 0;   *c1 = INT_MAX;  }
    return 1;
}

/* Diff background escape (bg-only, RGB): dark opaque tints. */
static const char *diff_bg_escape(char sign) {
    switch (sign) {
        case GIT_SIGN_ADD: return "\x1b[48;2;30;50;30m";
        case GIT_SIGN_MOD: return "\x1b[48;2;50;50;20m";
        case GIT_SIGN_DEL: return "\x1b[48;2;50;30;30m";
        default:           return NULL;
    }
}

/* Render a row's content within a visual-column viewport.
   vcol_start / vcol_count are in visual columns (tabs expanded).
   All highlight coordinates (bm0, bm1, vis_c0, vis_c1, cursor_col)
   remain byte offsets into row->chars.
   cursor_col (-1 = none) is exempted from overlays so the terminal
   cursor block always has a plain cell to blink over. */
static void render_row_content(AppendBuf *ab, const Row *row,
                               int vcol_start, int vcol_count, int tabwidth,
                               const SearchQuery *q, int bm0, int bm1,
                               int vis_c0, int vis_c1, int cursor_col,
                               char diff_bg, int cursorline) {
    if (vcol_count <= 0 || row->len == 0) return;

    /* Build final_hl[row->len] — byte-indexed highlight array. */
    unsigned char fhl[row->len];
    if (row->hl)
        memcpy(fhl, row->hl, row->len);
    else
        memset(fhl, HL_NORMAL, row->len);

    /* Bracket match */
    int bm[2] = { bm0, bm1 };
    for (int k = 0; k < 2; k++) {
        if (bm[k] >= 0 && bm[k] < row->len)
            fhl[bm[k]] = HL_BRACKET_MATCH;
    }

    /* Visual selection */
    if (vis_c0 >= 0) {
        int vc0 = vis_c0;
        int vc1 = (vis_c1 == INT_MAX) ? row->len : vis_c1;
        if (vc1 > row->len) vc1 = row->len;
        for (int k = vc0; k < vc1; k++) fhl[k] = HL_VISUAL;
    }

    /* Search highlight */
    if (q) {
        int pos = 0;
        while (pos < row->len) {
            int mc, ml;
            if (!q->match_fn(row->chars, row->len,
                             q->pattern, q->pat_len, pos, &mc, &ml))
                break;
            int hs = mc, he = mc + ml;
            if (he > row->len) he = row->len;
            memset(&fhl[hs], HL_SEARCH, he - hs);
            pos = (mc + ml > pos) ? mc + ml : pos + 1;
        }
    }

    /* Cursor: revert to syntax colour so terminal cursor blinks visibly */
    if (cursor_col >= 0 && cursor_col < row->len)
        fhl[cursor_col] = row->hl ? row->hl[cursor_col] : HL_NORMAL;

    /* Row background: diff tint or cursorline. */
    const char *bg_esc = diff_bg_escape(diff_bg);
    if (!bg_esc && cursorline)
        bg_esc = "\x1b[48;5;236m";
    int bg_esc_len = bg_esc ? (int)strlen(bg_esc) : 0;

    /* Render char by char, expanding tabs, clipped to the vcol viewport. */
    unsigned char prev_hl = HL_NORMAL;
    int any_esc = 0;
    int vcol    = 0;   /* visual column of the current byte */

    /* Emit diff background at the start of the line. */
    if (bg_esc) { ab_append(ab, bg_esc, bg_esc_len); any_esc = 1; }

    for (int i = 0; i < row->len; i++) {
        unsigned char ch  = (unsigned char)row->chars[i];
        unsigned char cur = fhl[i];
        int cw = (ch == '\t') ? (tabwidth - (vcol % tabwidth)) : 1;
        int char_end = vcol + cw;

        if (char_end <= vcol_start) { vcol = char_end; continue; }
        if (vcol >= vcol_start + vcol_count) break;

        /* Visual columns of this character that fall in the viewport */
        int out_s = (vcol < vcol_start) ? vcol_start : vcol;
        int out_e = (char_end > vcol_start + vcol_count)
                    ? vcol_start + vcol_count : char_end;
        int out_n = out_e - out_s;

        if (cur != prev_hl) {
            ab_append(ab, "\x1b[m", 3);
            if (bg_esc) ab_append(ab, bg_esc, bg_esc_len);
            const char *esc = hl_to_escape(cur);
            if (esc) ab_append(ab, esc, (int)strlen(esc));
            prev_hl = cur;
            any_esc = 1;
        }

        if (ch == '\t') {
            for (int j = 0; j < out_n; j++) ab_append(ab, " ", 1);
        } else {
            ab_append(ab, &row->chars[i], 1);
        }

        vcol = char_end;
    }

    if (any_esc) ab_append(ab, "\x1b[m", 3);
}

/* Find the bracket that matches the one under the cursor (if any).
   Result is stored in E.match_bracket_{valid,row,col}. */
static void editor_find_bracket_match(void) {
    E.match_bracket_valid = 0;
    if (E.mode != MODE_NORMAL || E.buf.numrows == 0 || E.cy >= E.buf.numrows)
        return;

    const Row *cur = &E.buf.rows[E.cy];
    if (E.cx >= cur->len) return;

    char ch = cur->chars[E.cx];
    char open_ch, close_ch;
    int  forward;

    switch (ch) {
        case '(': open_ch = '('; close_ch = ')'; forward = 1; break;
        case '[': open_ch = '['; close_ch = ']'; forward = 1; break;
        case '{': open_ch = '{'; close_ch = '}'; forward = 1; break;
        case ')': open_ch = '('; close_ch = ')'; forward = 0; break;
        case ']': open_ch = '['; close_ch = ']'; forward = 0; break;
        case '}': open_ch = '{'; close_ch = '}'; forward = 0; break;
        default:  return;
    }

    int depth = 1;
    int r = E.cy;
    int c = E.cx;

    if (forward) {
        c++;
        while (r < E.buf.numrows) {
            const char *line = E.buf.rows[r].chars;
            int          len  = E.buf.rows[r].len;
            while (c < len) {
                if      (line[c] == open_ch)  depth++;
                else if (line[c] == close_ch) { if (--depth == 0) goto found; }
                c++;
            }
            r++; c = 0;
        }
    } else {
        c--;
        while (r >= 0) {
            const char *line = E.buf.rows[r].chars;
            while (c >= 0) {
                if      (line[c] == close_ch) depth++;
                else if (line[c] == open_ch)  { if (--depth == 0) goto found; }
                c--;
            }
            r--; if (r >= 0) c = E.buf.rows[r].len - 1;
        }
    }
    return;
found:
    E.match_bracket_valid = 1;
    E.match_bracket_row   = r;
    E.match_bracket_col   = c;
}

/* ── Per-pane drawing ────────────────────────────────────────────────── */

static void draw_pane_rows(AppendBuf *ab, const Pane *p,
                           int pcx, int pcy, int pro, int pco,
                           Buffer *buf, const SyntaxDef *syn,
                           EditorMode mode,
                           const SearchQuery *hl_q,
                           int vis_ar, int vis_ac,
                           int bm_valid, int bm_row, int bm_col) {
    /* Terminal pane: render directly from cell grid. */
    if (E.buftabs[p->buf_idx].kind == BT_TERM) {
        TermState *ts = E.buftabs[p->buf_idx].term;
        if (!ts) return;
        char mv[24];
        for (int y = 0; y < p->height; y++) {
            int mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;%dH", p->top + y, p->left);
            ab_append(ab, mv, mvlen);
            char erase[16];
            int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
            ab_append(ab, erase, elen);

            uint8_t prev_fg = 0, prev_bg = 0;
            uint8_t prev_bold = 0, prev_dim = 0, prev_ul = 0, prev_rev = 0;
            int attr_set = 0;

            int cols = (p->width < ts->cols) ? p->width : ts->cols;
            for (int x = 0; x < cols; x++) {
                TermCell tc = term_emu_cell(ts, y, x);
                /* Emit SGR only when attributes change. */
                if (!attr_set || tc.fg != prev_fg || tc.bg != prev_bg ||
                    tc.bold != prev_bold || tc.dim != prev_dim ||
                    tc.underline != prev_ul || tc.reverse != prev_rev) {
                    char sgr[64];
                    int slen = 0;
                    slen += snprintf(sgr + slen, sizeof(sgr) - slen, "\x1b[0");
                    if (tc.bold) slen += snprintf(sgr + slen, sizeof(sgr) - slen, ";1");
                    if (tc.dim)  slen += snprintf(sgr + slen, sizeof(sgr) - slen, ";2");
                    if (tc.underline) slen += snprintf(sgr + slen, sizeof(sgr) - slen, ";4");
                    if (tc.reverse)   slen += snprintf(sgr + slen, sizeof(sgr) - slen, ";7");
                    if (tc.fg > 0) slen += snprintf(sgr + slen, sizeof(sgr) - slen,
                                                     ";38;5;%d", tc.fg - 1);
                    if (tc.bg > 0) slen += snprintf(sgr + slen, sizeof(sgr) - slen,
                                                     ";48;5;%d", tc.bg - 1);
                    slen += snprintf(sgr + slen, sizeof(sgr) - slen, "m");
                    ab_append(ab, sgr, slen);
                    prev_fg = tc.fg; prev_bg = tc.bg;
                    prev_bold = tc.bold; prev_dim = tc.dim;
                    prev_ul = tc.underline; prev_rev = tc.reverse;
                    attr_set = 1;
                }
                char ch = tc.ch ? tc.ch : ' ';
                ab_append(ab, &ch, 1);
            }
            ab_append(ab, "\x1b[m", 3);
        }
        return;
    }

    int is_tree      = E.buftabs[p->buf_idx].kind == BT_TREE;
    int is_qf        = E.buftabs[p->buf_idx].kind == BT_QF;
    int is_blame     = E.buftabs[p->buf_idx].kind == BT_BLAME;
    int is_diff      = E.buftabs[p->buf_idx].kind == BT_DIFF;
    int is_commit    = E.buftabs[p->buf_idx].kind == BT_COMMIT;
    int is_log       = E.buftabs[p->buf_idx].kind == BT_LOG;
    int is_show      = E.buftabs[p->buf_idx].kind == BT_SHOW;
    int is_rev       = E.buftabs[p->buf_idx].kind == BT_REVISIONS;
    int no_gutter    = is_tree || is_qf || is_blame || is_commit || is_log || is_show || is_rev;
    int gw           = no_gutter ? 0 : gutter_width_for(buf, p->buf_idx);
    int has_marks    = no_gutter ? 0 : buf_has_marks(p->buf_idx);
    int content_cols = p->width - gw;

    /* Determine if this pane should show diff background highlighting.
       True for the diff pane itself, and for the source pane of an active diff. */
    int show_diff_bg = is_diff || is_show;
    if (!show_diff_bg) {
        for (int di = 0; di < E.num_panes; di++) {
            BufTab *dbt = &E.buftabs[E.panes[di].buf_idx];
            if (dbt->kind == BT_DIFF && dbt->diff_source_buf == p->buf_idx) {
                show_diff_bg = 1; break;
            }
        }
    }

    /* Cascade hl_open_comment for dirty rows. */
    if (syn && buf->hl_dirty_from < buf->numrows) {
        syntax_update_open_comments(syn, buf, buf->hl_dirty_from);
        buf->hl_dirty_from = buf->numrows;
    }

    /* Build filerow mapping: walk forward from pro, skipping folded rows. */
    int fr = pro;
    char mv[24];
    for (int y = 0; y < p->height; y++) {
        int filerow = fr;

        /* Move to absolute terminal position. */
        int mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;%dH", p->top + y, p->left);
        ab_append(ab, mv, mvlen);

        /* Diff background: set before erase so whole row gets the tint. */
        char row_dbg = 0;
        const char *row_bg = NULL;
        if (show_diff_bg && buf->git_signs && filerow < buf->git_signs_count
            && filerow < buf->numrows) {
            row_dbg = buf->git_signs[filerow];
            row_bg  = diff_bg_escape(row_dbg);
        }

        /* Cursorline: subtle background on the cursor row (content panes only). */
        int is_cursorline = 0;
        if (!row_bg && E.opts.cursorline && !no_gutter
            && filerow == pcy && filerow < buf->numrows) {
            row_bg = "\x1b[48;5;236m";
            is_cursorline = 1;
        }

        if (row_bg) ab_append(ab, row_bg, (int)strlen(row_bg));

        /* Erase this pane row (only within pane width).
           When row_bg is active, ECH fills cells with the diff tint. */
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);

        /* Quickfix pane: custom colour rendering per entry. */
        if (is_qf) {
            QfList *ql = E.buftabs[p->buf_idx].qf;
            if (!ql || filerow >= ql->count) {
                ab_append(ab, "~", 1);
            } else {
                QfEntry *e  = &ql->entries[filerow];
                int is_sel  = (filerow == pcy);
                int avail   = p->width;

                if (is_sel) ab_append(ab, "\x1b[7m", 4);

                /* ▶ / spaces */
                if (is_sel) ab_append(ab, "▶ ", 4);
                else        ab_append(ab, "  ", 2);
                avail -= 2;

                /* Dir prefix (dim) + filename (normal). */
                const char *slash  = strrchr(e->path, '/');
                int   dir_len  = slash ? (int)(slash - e->path + 1) : 0;
                const char *fname  = e->path + dir_len;
                int   fname_len = (int)strlen(fname);

                if (dir_len > 0 && avail > 0) {
                    int dl = dir_len < avail ? dir_len : avail;
                    ab_append(ab, is_sel ? "\x1b[2;7m" : "\x1b[2m", is_sel ? 6 : 4);
                    ab_append(ab, e->path, dl);
                    avail -= dl;
                    ab_append(ab, is_sel ? "\x1b[7m" : "\x1b[m", is_sel ? 4 : 3);
                }
                if (fname_len > 0 && avail > 0) {
                    int fl = fname_len < avail ? fname_len : avail;
                    ab_append(ab, fname, fl);
                    avail -= fl;
                }

                /* Line number (yellow). */
                if (avail > 0) {
                    char lnum[16];
                    int  llen = snprintf(lnum, sizeof(lnum), ":%d:", e->line);
                    if (llen > avail) llen = avail;
                    ab_append(ab, is_sel ? "\x1b[33;7m" : "\x1b[33m", is_sel ? 8 : 5);
                    ab_append(ab, lnum, llen);
                    avail -= llen;
                    ab_append(ab, is_sel ? "\x1b[7m" : "\x1b[m", is_sel ? 4 : 3);
                }

                /* Match text. */
                if (avail > 1) {
                    ab_append(ab, " ", 1);
                    avail--;
                    int tlen = (int)strlen(e->text);
                    if (tlen > avail) tlen = avail;
                    ab_append(ab, e->text, tlen);
                }

                ab_append(ab, "\x1b[m", 3);
            }
            fr++;
            continue;
        }

        /* Tree pane: colour filenames by git status. */
        if (is_tree && filerow < buf->numrows) {
            TreeState *ts = E.buftabs[p->buf_idx].tree;
            int is_sel = (filerow == pcy);
            if (is_sel) ab_append(ab, "\x1b[7m", 4);

            Row *row = &buf->rows[filerow];
            int avail = p->width;
            int rlen = row->len < avail ? row->len : avail;

            /* Entry index: row 0 = root line, row i+1 = entries[i]. */
            int eidx = filerow - 1;
            char gs = ' ';
            if (ts && eidx >= 0 && eidx < ts->count)
                gs = ts->entries[eidx].git_status;

            if (gs == '?' || gs == 'A')
                ab_append(ab, "\x1b[32m", 5);      /* green */
            else if (gs == 'M')
                ab_append(ab, "\x1b[33m", 5);      /* yellow */
            else if (gs == 'D')
                ab_append(ab, "\x1b[31m", 5);      /* red */

            ab_append(ab, row->chars, rlen);
            ab_append(ab, "\x1b[m", 3);
            fr++;
            continue;
        }

        /* Revisions pane: tree structure in dim, current node highlighted. */
        if (is_rev && filerow < buf->numrows) {
            Row *row = &buf->rows[filerow];
            int avail = p->width;
            int rlen = row->len < avail ? row->len : avail;
            int is_sel = (filerow == pcy);
            if (is_sel) ab_append(ab, "\x1b[7m", 4);

            /* Check if this row contains the current marker (◀). */
            int is_current = 0;
            for (int ci = 0; ci + 2 < rlen; ci++) {
                if ((unsigned char)row->chars[ci] == 0xe2 &&
                    (unsigned char)row->chars[ci+1] == 0x97 &&
                    (unsigned char)row->chars[ci+2] == 0x80) {
                    is_current = 1; break;
                }
            }

            if (is_current && !is_sel)
                ab_append(ab, "\x1b[1;33m", 6);    /* bold yellow for current */
            else if (!is_sel)
                ab_append(ab, "\x1b[36m", 5);      /* cyan for tree lines */

            ab_append(ab, row->chars, rlen);
            ab_append(ab, "\x1b[m", 3);
            fr++;
            continue;
        }

        /* Blame pane: hash in dark yellow, rest in dark cyan. */
        if (is_blame && filerow < buf->numrows) {
            Row *row = &buf->rows[filerow];
            int avail = p->width;
            int rlen = row->len < avail ? row->len : avail;
            int is_sel = (filerow == pcy);
            if (is_sel) ab_append(ab, "\x1b[7m", 4);

            /* Find the end of the hash (first space). */
            int hash_end = 0;
            while (hash_end < rlen && row->chars[hash_end] != ' ') hash_end++;

            /* Hash: dark yellow (\x1b[33m). */
            ab_append(ab, "\x1b[33m", 5);
            ab_append(ab, row->chars, hash_end);

            /* Rest: dark cyan (\x1b[36m). */
            if (hash_end < rlen) {
                ab_append(ab, "\x1b[36m", 5);
                ab_append(ab, row->chars + hash_end, rlen - hash_end);
            }

            ab_append(ab, "\x1b[m", 3);
            fr++;
            continue;
        }

        /* Commit buffer: dim comment lines starting with #. */
        if (is_commit && filerow < buf->numrows) {
            Row *row = &buf->rows[filerow];
            int avail = p->width;
            int rlen = row->len < avail ? row->len : avail;
            if (rlen > 0 && row->chars[0] == '#')
                ab_append(ab, "\x1b[2m", 4);  /* dim */
            ab_append(ab, row->chars, rlen);
            if (rlen > 0 && row->chars[0] == '#')
                ab_append(ab, "\x1b[m", 3);
            fr++;
            continue;
        }

        /* Git log pane: colored fields — hash|date|author|subject. */
        if (is_log && filerow < buf->numrows) {
            BufTab *lbt = &E.buftabs[p->buf_idx];
            int is_sel = (filerow == pcy);
            if (is_sel) ab_append(ab, "\x1b[7m", 4);

            if (filerow < lbt->log_count) {
                GitLogEntry *e = &lbt->log_entries[filerow];
                int avail = p->width;
                int col = 0;

                /* ▶ / space */
                if (is_sel) { ab_append(ab, "\xe2\x96\xb6 ", 4); col += 2; }
                else        { ab_append(ab, "  ", 2); col += 2; }

                /* Hash: yellow */
                int hlen = (int)strlen(e->hash);
                if (col + hlen <= avail) {
                    ab_append(ab, is_sel ? "\x1b[33;7m" : "\x1b[33m", is_sel ? 7 : 5);
                    ab_append(ab, e->hash, hlen);
                    col += hlen;
                }

                /* Space */
                if (col < avail) { ab_append(ab, " ", 1); col++; }

                /* Date: dim */
                int dlen = (int)strlen(e->date);
                if (col + dlen <= avail) {
                    ab_append(ab, is_sel ? "\x1b[2;7m" : "\x1b[2m", is_sel ? 5 : 4);
                    ab_append(ab, e->date, dlen);
                    col += dlen;
                }

                /* Space */
                if (col < avail) { ab_append(ab, " ", 1); col++; }

                /* Author: cyan */
                int alen = (int)strlen(e->author);
                if (alen > 12) alen = 12;
                if (col + alen <= avail) {
                    ab_append(ab, is_sel ? "\x1b[36;7m" : "\x1b[36m", is_sel ? 7 : 5);
                    ab_append(ab, e->author, alen);
                    col += alen;
                    /* Pad to 12. */
                    int pad = 12 - alen;
                    while (pad-- > 0 && col < avail) {
                        ab_append(ab, " ", 1); col++;
                    }
                }

                /* Space */
                if (col < avail) { ab_append(ab, " ", 1); col++; }

                /* Subject: default */
                int slen = (int)strlen(e->subject);
                if (slen > avail - col) slen = avail - col;
                if (slen > 0) {
                    ab_append(ab, is_sel ? "\x1b[m\x1b[7m" : "\x1b[m", is_sel ? 7 : 3);
                    ab_append(ab, e->subject, slen);
                    col += slen;
                }
            }

            ab_append(ab, "\x1b[m", 3);
            fr++;
            continue;
        }

        if (buf->numrows == 0) {
            /* Empty buffer: splash only for single-pane active. */
            if (p->width > 1) {
                if (E.num_panes == 1) {
                    int top = (p->height - SPLASH_LINES) / 2;
                    int idx = y - top;
                    ab_append(ab, "~", 1);
                    if (idx >= 0 && idx < SPLASH_LINES) {
                        int len = (int)strlen(SPLASH[idx]);
                        if (len > p->width) len = p->width;
                        int pad = (p->width - 1 - len) / 2;
                        while (pad-- > 0) ab_append(ab, " ", 1);
                        ab_append(ab, SPLASH[idx], len);
                    }
                } else {
                    ab_append(ab, "~", 1);
                }
            }
        } else if (filerow >= buf->numrows) {
            if (row_bg) ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "~", 1);
        } else {
            /* Gutter */
            if (gw > 0) {
                int has_git = (buf->git_signs != NULL);
                char num[16];
                int display_num = (E.opts.relative_line_numbers && filerow != pcy)
                                  ? (filerow > pcy ? filerow - pcy : pcy - filerow)
                                  : filerow + 1;
                int  nlen = snprintf(num, sizeof(num), "%d", display_num);
                int  num_gw = gw - (has_marks ? 2 : 0) - (has_git ? 1 : 0);
                int  pad  = num_gw - 1 - nlen;

                /* Git sign column (single char: +/~/- or space). */
                if (has_git) {
                    char gs = (filerow < buf->git_signs_count)
                              ? buf->git_signs[filerow] : ' ';
                    if (gs == '+') {
                        ab_append(ab, "\x1b[32m+", 6);
                    } else if (gs == '~') {
                        ab_append(ab, "\x1b[33m~", 6);
                    } else if (gs == '-') {
                        ab_append(ab, "\x1b[31m-", 6);
                    } else {
                        ab_append(ab, " ", 1);
                    }
                    /* Reset fg but keep bg. */
                    ab_append(ab, "\x1b[m", 3);
                    if (row_bg) ab_append(ab, row_bg, (int)strlen(row_bg));
                }

                /* Line number: diff-aware coloring. */
                if (row_bg) {
                    if (row_dbg == GIT_SIGN_ADD)
                        ab_append(ab, "\x1b[38;5;77m", 11);  /* light green */
                    else if (row_dbg == GIT_SIGN_DEL)
                        ab_append(ab, "\x1b[38;5;167m", 12); /* light red */
                    else if (row_dbg == GIT_SIGN_MOD)
                        ab_append(ab, "\x1b[38;5;186m", 12); /* light yellow */
                    else
                        ab_append(ab, (filerow == pcy) ? "\x1b[1m" : "\x1b[2m", 4);
                } else {
                    ab_append(ab, (filerow == pcy) ? "\x1b[1m" : "\x1b[2m", 4);
                }
                if (has_marks) {
                    char mc = mark_char_at(p->buf_idx, filerow);
                    if (mc) {
                        ab_append(ab, "\x1b[33m", 5); /* yellow mark char */
                        ab_append(ab, &mc, 1);
                        if (row_bg) {
                            ab_append(ab, "\x1b[m", 3);
                            ab_append(ab, row_bg, (int)strlen(row_bg));
                        }
                        ab_append(ab, (filerow == pcy) ? "\x1b[1m" : "\x1b[2m", 4);
                    } else {
                        ab_append(ab, " ", 1);
                    }
                    ab_append(ab, " ", 1);
                }
                while (pad-- > 0) ab_append(ab, " ", 1);
                ab_append(ab, num, nlen);
                ab_append(ab, " ", 1);
                ab_append(ab, "\x1b[m", 3);
                if (row_bg) ab_append(ab, row_bg, (int)strlen(row_bg));
            }

            Row *row = &buf->rows[filerow];

            if (syn)
                syntax_highlight_row(syn, row, row->hl_open_comment);

            int bm0 = -1, bm1 = -1;
            if (bm_valid) {
                if (filerow == pcy)    bm0 = pcx;
                if (filerow == bm_row) bm1 = bm_col;
            }

            int vis_c0 = -1, vis_c1 = -1;
            if (vis_ar >= 0) {
                int vc0, vc1;
                if (visual_col_range_for(filerow, vis_ar, vis_ac, pcy, pcx,
                                         mode, &vc0, &vc1)) {
                    vis_c0 = vc0;
                    vis_c1 = (vc1 == INT_MAX) ? row->len : vc1;
                }
            }

            /* Exempt cursor cell from overlays (active pane cursor row only). */
            int cur_col = (vis_ar >= 0 && filerow == pcy) ? pcx : -1;

            /* Diff background sign for this row. */
            char dbg = 0;
            if (show_diff_bg && buf->git_signs && filerow < buf->git_signs_count)
                dbg = buf->git_signs[filerow];

            render_row_content(ab, row, pco, content_cols,
                               E.opts.tabwidth, hl_q,
                               bm0, bm1, vis_c0, vis_c1, cur_col,
                               dbg, is_cursorline);

            /* Fold indicator: show "[N lines]" after fold header. */
            if (buf->folds && filerow < buf->folds_cap && buf->folds[filerow]) {
                int fend = buf_fold_end(buf, filerow, E.opts.tabwidth);
                int hidden = fend - filerow;
                if (hidden > 0) {
                    char finfo[32];
                    int flen = snprintf(finfo, sizeof(finfo),
                                        " \x1b[2;36m[%d lines]\x1b[m", hidden);
                    ab_append(ab, finfo, flen);
                }
            }

            /* Reset attributes after row content to prevent background
               leaking to the next line (e.g. cursorline on empty rows). */
            if (row_bg) ab_append(ab, "\x1b[m", 3);
        }

        /* Advance fr: if this row is a closed fold, skip its body. */
        if (filerow < buf->numrows && buf->folds
            && filerow < buf->folds_cap && buf->folds[filerow]) {
            fr = buf_fold_end(buf, filerow, E.opts.tabwidth) + 1;
        } else {
            fr++;
        }
    }
}

static void draw_pane_status(AppendBuf *ab, const Pane *p,
                             const Buffer *buf, int pcx, int pcy,
                             int is_active) {
    /* Position at status bar row for this pane. */
    char mv[24];
    int mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;%dH", p->top + p->height, p->left);
    ab_append(ab, mv, mvlen);

    /* Tab-completion overlay for active pane in command mode. */
    if (is_active && E.completion_idx >= 0 && E.mode == MODE_COMMAND) {
        ab_append(ab, "\x1b[107;30m", 9);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        int col = 0;
        for (int i = 0; i < E.completion_count; i++) {
            int len = (int)strlen(E.completion_matches[i]);
            if (col + len + 1 > p->width) break;
            if (i == E.completion_idx)
                ab_append(ab, "\x1b[40;97m", 8);
            ab_append(ab, E.completion_matches[i], len);
            if (i == E.completion_idx)
                ab_append(ab, "\x1b[107;30m", 9);
            ab_append(ab, " ", 1);
            col += len + 1;
        }
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Terminal pane: compact status. */
    if (E.buftabs[p->buf_idx].kind == BT_TERM) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        TermState *ts = E.buftabs[p->buf_idx].term;
        char left[128], right[32];
        const char *state = (ts && ts->exited) ? "finished" : "running";
        int llen = snprintf(left, sizeof(left), " [Terminal] %s", state);
        int rlen = 0;
        if (ts && ts->exited)
            rlen = snprintf(right, sizeof(right), "exit %d", ts->exit_status);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (rlen > 0 && llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Quickfix pane: compact status. */
    if (E.buftabs[p->buf_idx].kind == BT_QF) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        QfList *ql = E.buftabs[p->buf_idx].qf;
        char left[128], right[16];
        int llen = ql
            ? snprintf(left,  sizeof(left),  " [Quickfix] %d results: \"%s\"",
                       ql->count, ql->pattern)
            : snprintf(left,  sizeof(left),  " [Quickfix]");
        int rlen = snprintf(right, sizeof(right), "%d", pcy + 1);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Blame pane: compact status. */
    if (E.buftabs[p->buf_idx].kind == BT_BLAME) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        int src = E.buftabs[p->buf_idx].blame_source_buf;
        const char *fname = E.buftabs[src].buf.filename;
        if (!fname) fname = "[No Name]";
        char left[128], right[16];
        int llen = snprintf(left, sizeof(left), " [Blame] %.30s", fname);
        int rlen = snprintf(right, sizeof(right), "%d", pcy + 1);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Diff pane: compact status. */
    if (E.buftabs[p->buf_idx].kind == BT_DIFF) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        const char *fname = buf->filename ? buf->filename : "[No Name]";
        char left[128], right[16];
        int llen = snprintf(left, sizeof(left), " [HEAD] %.30s", fname);
        int rlen = snprintf(right, sizeof(right), "%d", pcy + 1);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Commit buffer: status bar. */
    if (E.buftabs[p->buf_idx].kind == BT_COMMIT) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        char left[128], right[16];
        int llen = snprintf(left, sizeof(left),
                            " [Commit] :wq to commit, :q to abort");
        int rlen = snprintf(right, sizeof(right), "%d", pcy + 1);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Log pane: status bar. */
    if (E.buftabs[p->buf_idx].kind == BT_LOG) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        int lc = E.buftabs[p->buf_idx].log_count;
        char left[128], right[16];
        int llen = snprintf(left, sizeof(left),
                            " [Git Log] %d commits", lc);
        int rlen = snprintf(right, sizeof(right), "%d", pcy + 1);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Git show (commit diff) pane: status bar. */
    if (E.buftabs[p->buf_idx].kind == BT_SHOW) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        char left[128], right[16];
        const char *fn = buf->filename ? buf->filename : "";
        int llen = snprintf(left, sizeof(left), " %s", fn);
        int rlen = snprintf(right, sizeof(right), "%d/%d", pcy + 1, buf->numrows);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Tree pane: special compact status. */
    if (E.buftabs[p->buf_idx].kind == BT_TREE) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        char left[32], right[16];
        int llen = snprintf(left,  sizeof(left),  " [Tree]");
        int rlen = snprintf(right, sizeof(right), "%d", pcy + 1);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Revisions pane: compact status bar. */
    if (E.buftabs[p->buf_idx].kind == BT_REVISIONS) {
        ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);
        char left[64], right[16];
        int llen = snprintf(left,  sizeof(left),  " [Local Revisions]");
        int rlen = snprintf(right, sizeof(right), "%d", pcy + 1);
        if (llen > p->width) llen = p->width;
        ab_append(ab, left, llen);
        int gap = p->width - llen - rlen;
        while (gap-- > 0) ab_append(ab, " ", 1);
        if (llen + rlen <= p->width) ab_append(ab, right, rlen);
        ab_append(ab, "\x1b[m", 3);
        return;
    }

    /* Active = full reverse video; inactive = dim reverse. */
    ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);

    /* Erase pane width with current attributes. */
    char erase[16];
    int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
    ab_append(ab, erase, elen);

    char left[196], right[32];
    const char *name = buf->filename ? buf->filename : "[No Name]";

    int llen, rlen;
    if (is_active) {
        char bufnum[32] = "";
        if (E.num_buftabs > 1)
            snprintf(bufnum, sizeof(bufnum), " [%d/%d]",
                     E.cur_buftab + 1, E.num_buftabs);
        char branch[80] = "";
        if (E.git_branch[0])
            snprintf(branch, sizeof(branch), "  %s", E.git_branch);
        const char *mod = buf->dirty ? " [+]" : "";
        const char *ro  = E.readonly ? " [RO]" : "";
        llen = snprintf(left, sizeof(left), " %.30s%s%s%s%s",
                        name, mod, ro, bufnum, branch);
        char prefix[24] = "";
        char regstr[6]  = "";
        if (E.pending_reg >= 1 && E.pending_reg <= 26)
            snprintf(regstr, sizeof(regstr), "\"%c", 'a' + E.pending_reg - 1);
        else if (E.pending_reg == REG_CLIPBOARD)
            snprintf(regstr, sizeof(regstr), "\"+");
        if (E.count > 0 && E.pending_op)
            snprintf(prefix, sizeof(prefix), "%s%d%c", regstr, E.count, E.pending_op);
        else if (E.count > 0)
            snprintf(prefix, sizeof(prefix), "%s%d", regstr, E.count);
        else if (E.pending_op)
            snprintf(prefix, sizeof(prefix), "%s%c", regstr, E.pending_op);
        else if (regstr[0])
            snprintf(prefix, sizeof(prefix), "%s", regstr);

        char pos[16];
        if (buf->numrows <= 1 || pcy == 0)
            strcpy(pos, "Top");
        else if (pcy >= buf->numrows - 1)
            strcpy(pos, "Bot");
        else
            snprintf(pos, sizeof(pos), "%d%%", (pcy * 100) / (buf->numrows - 1));

        int vpcx = (buf->numrows > 0 && pcy < buf->numrows)
                   ? col_to_vcol(&buf->rows[pcy], pcx, E.opts.tabwidth) : pcx;
        rlen = prefix[0]
            ? snprintf(right, sizeof(right), "%s    %d,%-3d   %-4s",
                       prefix, pcy + 1, vpcx + 1, pos)
            : snprintf(right, sizeof(right), "%d,%-3d   %-4s",
                       pcy + 1, vpcx + 1, pos);
    } else {
        char pos[16];
        if (buf->numrows <= 1 || pcy == 0)
            strcpy(pos, "Top");
        else if (pcy >= buf->numrows - 1)
            strcpy(pos, "Bot");
        else
            snprintf(pos, sizeof(pos), "%d%%", (pcy * 100) / (buf->numrows - 1));

        int vpcx = (buf->numrows > 0 && pcy < buf->numrows)
                   ? col_to_vcol(&buf->rows[pcy], pcx, E.opts.tabwidth) : pcx;
        llen = snprintf(left,  sizeof(left),  " %.30s%s", name, buf->dirty ? " [+]" : "");
        rlen = snprintf(right, sizeof(right), "%d,%-3d   %-4s", pcy + 1, vpcx + 1, pos);
    }

    if (llen > p->width) llen = p->width;
    ab_append(ab, left, llen);

    int gap = p->width - llen - rlen;
    while (gap-- > 0) ab_append(ab, " ", 1);
    if (llen + rlen <= p->width) ab_append(ab, right, rlen);

    ab_append(ab, "\x1b[m", 3);
}

/* Draw a 1-column white-background divider between side-by-side panes.
   A divider exists at column (p->left + p->width) when another pane
   starts immediately one column to the right. */
static void draw_dividers(AppendBuf *ab) {
    for (int i = 0; i < E.num_panes; i++) {
        Pane *p = &E.panes[i];
        int div_col = p->left + p->width;   /* 1-based divider column */
        if (div_col > E.term_cols) continue;
        /* Check if a right-neighbour pane starts right after the gap. */
        int has_right = 0;
        for (int j = 0; j < E.num_panes && !has_right; j++) {
            if (j != i && E.panes[j].left == div_col + 1) has_right = 1;
        }
        if (!has_right) continue;
        /* Draw a white-background space for each content row only (status bar
           is covered by the combined group status bar drawn separately). */
        char mv[24];
        for (int row = p->top; row < p->top + p->height; row++) {
            int mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;%dH", row, div_col);
            ab_append(ab, mv, mvlen);
            ab_append(ab, "\x1b[107m \x1b[m", 10);
        }
    }
}

/* ── Fuzzy finder overlay ─────────────────────────────────────────────── */

static void fuzzy_draw(AppendBuf *ab) {
    FuzzyState *f = &E.fuzzy;

    /* Panel dimensions. */
    int width = E.term_cols * E.opts.fuzzy_width_pct / 100;
    if (width < 40)              width = 40;
    if (width > E.term_cols - 2) width = E.term_cols - 2;
    int inner = width - 2;  /* visual columns between the side borders */

    /* height: top border + search + separator + results + separator + footer + bottom */
    int height = FUZZY_MAX_VIS + 6;
    if (height > E.term_rows - 2) height = E.term_rows - 2;

    int top  = (E.term_rows - height) / 2 + 1;  /* 1-based */
    int left = (E.term_cols - width)  / 2 + 1;  /* 1-based */

    char mv[32];
    int  mvlen;

    for (int y = 0; y < height; y++) {
        mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;%dH", top + y, left);
        ab_append(ab, mv, mvlen);

        if (y == 0) {
            /* ── Top border ─────────────────────────────────────── */
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "┌", 3);
            for (int x = 0; x < inner; x++) ab_append(ab, "─", 3);
            ab_append(ab, "┐", 3);

        } else if (y == 1) {
            /* ── Search input row ───────────────────────────────── */
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "│", 3);
            ab_append(ab, " ", 1);
            const char *label = f->buf_mode ? "buffer:" : "search:";
            ab_append(ab, "\x1b[33m",   5);  /* dark yellow */
            ab_append(ab, label,    7);
            ab_append(ab, "\x1b[1;37m", 7);  /* bold white: " > " */
            ab_append(ab, " > ",        3);
            ab_append(ab, "\x1b[m",     3);

            /* Query text — show tail if it overflows. */
            int avail  = inner - 1 - 7 - 3;  /* space + label + " > " */
            int qstart = (f->query_len > avail) ? f->query_len - avail : 0;
            int qshow  = f->query_len - qstart;
            ab_append(ab, f->query + qstart, qshow);
            int pad = avail - qshow;
            while (pad-- > 0) ab_append(ab, " ", 1);
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "│", 3);

        } else if (y == 2 || y == height - 3) {
            /* ── Separator ──────────────────────────────────────── */
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "├", 3);
            for (int x = 0; x < inner; x++) ab_append(ab, "─", 3);
            ab_append(ab, "┤", 3);

        } else if (y == height - 1) {
            /* ── Bottom border ──────────────────────────────────── */
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "└", 3);
            for (int x = 0; x < inner; x++) ab_append(ab, "─", 3);
            ab_append(ab, "┘", 3);

        } else if (y == height - 2) {
            /* ── Footer ─────────────────────────────────────────── */
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "│", 3);
            ab_append(ab, " ", 1);
            ab_append(ab, "\x1b[2m", 4);  /* dim */
            char count_str[32];
            int  clen = snprintf(count_str, sizeof(count_str),
                                 "%d / %d", f->match_count, f->all_count);
            ab_append(ab, count_str, clen);
            /* Hints right-aligned. */
            const char *hints = f->buf_mode
                ? "<Enter> switch  <Esc> close"
                : "<Enter> open  <C-x> sp  <C-v> vsp  <Esc> close";
            int hlen = (int)strlen(hints);
            int gap  = inner - 1 - clen - hlen;
            if (gap > 0) {
                while (gap-- > 0) ab_append(ab, " ", 1);
                ab_append(ab, hints, hlen);
            } else {
                int p = inner - 1 - clen;
                while (p-- > 0) ab_append(ab, " ", 1);
            }
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "│", 3);

        } else {
            /* ── Result row ─────────────────────────────────────── */
            int ridx = (y - 3) + f->scroll;   /* y==3 → first result */
            ab_append(ab, "\x1b[m", 3);
            ab_append(ab, "│", 3);
            ab_append(ab, " ", 1);

            int avail = inner - 2;  /* 1 leading space + 1 trailing space */
            if (ridx < f->match_count) {
                FuzzyMatch *m  = &f->matches[ridx];
                int is_sel = (ridx == f->selected);
                if (is_sel) ab_append(ab, "\x1b[7m", 4);

                /* ▶ / spaces (2 visual cols). */
                if (is_sel) ab_append(ab, "▶ ", 4);   /* 3-byte UTF-8 + space */
                else        ab_append(ab, "  ", 2);
                avail -= 2;

                /* Split path into dir prefix + filename. */
                const char *slash = strrchr(m->path, '/');
                int   dir_len  = slash ? (int)(slash - m->path + 1) : 0;
                const char *fname = m->path + dir_len;
                int   fname_len   = (int)strlen(fname);

                /* Dir prefix — dim. */
                if (dir_len > 0 && avail > 0) {
                    int dl = dir_len < avail ? dir_len : avail;
                    ab_append(ab, is_sel ? "\x1b[2;7m" : "\x1b[2m",
                              is_sel ? 6 : 4);
                    ab_append(ab, m->path, dl);
                    avail -= dl;
                    if (is_sel) ab_append(ab, "\x1b[7m",  4);
                    else        ab_append(ab, "\x1b[m",   3);
                }

                /* Filename — char by char, highlight matched positions. */
                int prev_hi = 0;
                for (int i = 0; i < fname_len && avail > 0; i++, avail--) {
                    int abs_pos = dir_len + i;
                    int is_match = 0;
                    for (int j = 0; j < m->match_count; j++)
                        if (m->match_pos[j] == abs_pos) { is_match = 1; break; }
                    if (is_match != prev_hi) {
                        if (is_sel)
                            ab_append(ab, is_match ? "\x1b[1;33;7m" : "\x1b[7m",
                                      is_match ? 9 : 4);
                        else
                            ab_append(ab, is_match ? "\x1b[1;33m" : "\x1b[m",
                                      is_match ? 7 : 3);
                        prev_hi = is_match;
                    }
                    ab_append(ab, &fname[i], 1);
                }
                ab_append(ab, "\x1b[m", 3);
                while (avail-- > 0) ab_append(ab, " ", 1);
            } else {
                for (int x = 0; x < avail; x++) ab_append(ab, " ", 1);
            }

            ab_append(ab, " \x1b[m│", 7);
        }
    }
}

static void draw_global_command_bar(AppendBuf *ab) {
    char mv[24];
    int mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;1H", E.term_rows);
    ab_append(ab, mv, mvlen);

    ab_append(ab, "\x1b[K", 3);  /* erase line */

    switch (E.mode) {
        case MODE_INSERT:
            if (E.buftabs[E.panes[E.cur_pane].buf_idx].kind == BT_TERM)
                ab_append(ab, "-- TERMINAL --", 14);
            else
                ab_append(ab, "-- INSERT --", 12);
            break;

        case MODE_VISUAL:
            ab_append(ab, "-- VISUAL --", 12);
            break;

        case MODE_VISUAL_LINE:
            ab_append(ab, "-- VISUAL LINE --", 17);
            break;

        case MODE_VISUAL_BLOCK:
            ab_append(ab, "-- VISUAL BLOCK --", 18);
            break;

        case MODE_COMMAND: {
            char line[258];
            int len = snprintf(line, sizeof(line), ":%s", E.cmdbuf);
            if (len > E.term_cols) len = E.term_cols;
            ab_append(ab, line, len);
            break;
        }

        case MODE_SEARCH: {
            char line[258];
            int len = snprintf(line, sizeof(line), "/%s", E.searchbuf);
            if (len > E.term_cols) len = E.term_cols;
            ab_append(ab, line, len);
            break;
        }

        case MODE_NORMAL:
        case MODE_FUZZY:
            if (E.statusmsg[0]) {
                int len = (int)strlen(E.statusmsg);
                if (len > E.term_cols) len = E.term_cols;
                if (E.statusmsg_is_error)
                    ab_append(ab, "\x1b[41;97m", 8);
                ab_append(ab, E.statusmsg, len);
                if (E.statusmsg_is_error)
                    ab_append(ab, "\x1b[m", 3);
            }
            break;
    }

    /* Show recording indicator on the right side of the command bar. */
    if (E.recording_reg >= 0) {
        char rec[20];
        int rlen = snprintf(rec, sizeof(rec), "recording @%c",
                            'a' + E.recording_reg);
        /* Position at the right edge. */
        int col = E.term_cols - rlen;
        if (col > 1) {
            char pos[16];
            int plen = snprintf(pos, sizeof(pos), "\x1b[%d;%dH",
                                E.term_rows, col);
            ab_append(ab, pos, plen);
            ab_append(ab, "\x1b[31m", 5);  /* red */
            ab_append(ab, rec, rlen);
            ab_append(ab, "\x1b[m", 3);
        }
    }
}

/* ── Main refresh ────────────────────────────────────────────────────── */

/* Sync scroll between a linked pane (blame or diff) and its source pane. */
static void linked_pane_sync_scroll(void) {
    for (int i = 0; i < E.num_panes; i++) {
        BufTab *bt = &E.buftabs[E.panes[i].buf_idx];
        int src_buf;
        if (bt->kind == BT_BLAME)     src_buf = bt->blame_source_buf;
        else if (bt->kind == BT_DIFF) src_buf = bt->diff_source_buf;
        else continue;
        /* Find the source pane. */
        for (int j = 0; j < E.num_panes; j++) {
            if (E.panes[j].buf_idx != src_buf) continue;

            /* Determine the live rowoff/cy of source and blame panes. */
            int src_ro, src_cy, blm_ro, blm_cy;
            if (j == E.cur_pane) {
                src_ro = E.rowoff; src_cy = E.cy;
            } else {
                src_ro = E.panes[j].rowoff; src_cy = E.panes[j].cy;
            }
            if (i == E.cur_pane) {
                blm_ro = E.rowoff; blm_cy = E.cy;
            } else {
                blm_ro = E.panes[i].rowoff; blm_cy = E.panes[i].cy;
            }

            /* Active pane is the master. Sync the other. */
            if (j == E.cur_pane) {
                E.panes[i].rowoff = src_ro;
                E.panes[i].cy     = src_cy;
            } else if (i == E.cur_pane) {
                E.panes[j].rowoff = blm_ro;
                E.panes[j].cy     = blm_cy;
            }
            break;
        }
    }
}

void editor_refresh_screen(void) {
    editor_scroll();
    linked_pane_sync_scroll();

    AppendBuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?2026h", 8); /* begin synchronized update */
    ab_append(&ab, "\x1b[?25l",   6); /* hide cursor */
    ab_append(&ab, "\x1b[m",      3); /* reset attributes before clear */
    ab_append(&ab, "\x1b[H",      3); /* cursor to home      */
    ab_append(&ab, "\x1b[J",      3); /* erase to end of screen (no scrollback push on macOS) */

    /* Draw inactive panes first, then active on top (for shared buffers). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < E.num_panes; i++) {
            int active = (i == E.cur_pane);
            if ((pass == 0) == active) continue;

            Pane   *p    = &E.panes[i];
            Buffer *pbuf = (p->buf_idx == E.cur_buftab) ? &E.buf
                                                        : &E.buftabs[p->buf_idx].buf;
            const SyntaxDef *psyn = active ? E.syntax
                                           : E.buftabs[p->buf_idx].syntax;
            int pcx = active ? E.cx      : p->cx;
            int pcy = active ? E.cy      : p->cy;
            int pro = active ? E.rowoff  : p->rowoff;
            int pco = active ? E.coloff  : p->coloff;

            /* Bracket match: only recompute for active pane. */
            if (active) editor_find_bracket_match();

            /* Search highlight query. */
            SearchQuery        live_q;
            const SearchQuery *hl_q = NULL;
            if (active) {
                if (E.mode == MODE_SEARCH && E.searchlen > 0) {
                    search_query_init_literal(&live_q, E.searchbuf, E.searchlen);
                    hl_q = &live_q;
                } else if (E.last_search_valid) {
                    hl_q = &E.last_query;
                }
            }

            draw_pane_rows(&ab, p, pcx, pcy, pro, pco, pbuf, psyn, E.mode, hl_q,
                           active ? E.visual_anchor_row : -1,
                           active ? E.visual_anchor_col : -1,
                           active && E.match_bracket_valid,
                           E.match_bracket_row, E.match_bracket_col);
        }
    }

    /* Draw one status bar per pane, each within its own column range. */
    for (int i = 0; i < E.num_panes; i++) {
        Pane *p       = &E.panes[i];
        int is_active = (i == E.cur_pane);
        const Buffer *dbuf;
        int dcx, dcy;
        if (is_active) {
            dbuf = &E.buf; dcx = E.cx; dcy = E.cy;
        } else {
            dbuf = (p->buf_idx == E.cur_buftab) ? &E.buf
                                                : &E.buftabs[p->buf_idx].buf;
            dcx = p->cx; dcy = p->cy;
        }
        draw_pane_status(&ab, p, dbuf, dcx, dcy, is_active);
    }

    draw_dividers(&ab);
    draw_global_command_bar(&ab);

    /* Fuzzy overlay drawn on top of everything else. */
    if (E.mode == MODE_FUZZY) fuzzy_draw(&ab);

    /* Cursor shape: terminal panes use a steady bar. */
    int is_term_pane = E.buftabs[E.panes[E.cur_pane].buf_idx].kind == BT_TERM;
    if (is_term_pane)
        ab_append(&ab, "\x1b[6 q", 5);
    else
        ab_append(&ab, (E.mode == MODE_NORMAL || E.mode == MODE_VISUAL
                       || E.mode == MODE_VISUAL_LINE || E.mode == MODE_VISUAL_BLOCK)
                      ? "\x1b[1 q" : "\x1b[6 q", 5);

    /* Cursor position. */
    char buf[32];
    int  len;
    Pane *ap = &E.panes[E.cur_pane];
    if (is_term_pane && E.buftabs[ap->buf_idx].term) {
        TermState *ts = E.buftabs[ap->buf_idx].term;
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       ap->top + ts->c_row, ap->left + ts->c_col);
    } else if (E.mode == MODE_FUZZY) {
        /* Cursor sits in the search input row, right after the query text. */
        int width = E.term_cols * E.opts.fuzzy_width_pct / 100;
        if (width < 40)              width = 40;
        if (width > E.term_cols - 2) width = E.term_cols - 2;
        int height = FUZZY_MAX_VIS + 6;
        if (height > E.term_rows - 2) height = E.term_rows - 2;
        int top  = (E.term_rows - height) / 2 + 1;
        int left = (E.term_cols - width)  / 2 + 1;
        /* col: │(1) + space(1) + "search:"(7) + " > "(3) + query + 1 */
        int avail = width - 2 - 1 - 7 - 3;
        int qshow = (E.fuzzy.query_len > avail) ? avail : E.fuzzy.query_len;
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       top + 1, left + 1 + 7 + 3 + qshow + 1);
    } else if (E.mode == MODE_COMMAND) {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       E.term_rows, E.cmdlen + 2);
    } else if (E.mode == MODE_SEARCH) {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       E.term_rows, E.searchlen + 2);
    } else {
        int vcx = (E.buf.numrows > 0 && E.cy < E.buf.numrows)
                  ? col_to_vcol(&E.buf.rows[E.cy], E.cx, E.opts.tabwidth) : 0;
        int screen_row = visible_rows_between(E.rowoff, E.cy);
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       ap->top  + screen_row,
                       ap->left + (vcx - E.coloff) + gutter_width());
    }
    ab_append(&ab, buf, len);
    ab_append(&ab, "\x1b[?25h",   6); /* show cursor */
    ab_append(&ab, "\x1b[?2026l", 8); /* end synchronized update */

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}
