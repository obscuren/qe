// SPDX-License-Identifier: GPL-3.0-or-later
#include "render.h"
#include "editor.h"
#include "search.h"
#include "syntax.h"

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

/* Width of the line-number gutter for a specific buffer. */
static int gutter_width_for(const Buffer *buf) {
    if (!E.opts.line_numbers || buf->numrows == 0) return 0;
    int n = buf->numrows, w = 0;
    while (n > 0) { w++; n /= 10; }
    return w + 1;
}

/* Gutter width for the active (live) buffer. */
static int gutter_width(void) { return gutter_width_for(&E.buf); }

/* ── Scroll ──────────────────────────────────────────────────────────── */

/* Adjust rowoff/coloff so the cursor stays within the visible viewport.
   Horizontal scroll uses the content width (screen minus gutter). */
static void editor_scroll(void) {
    int content_cols = E.screencols - gutter_width();

    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1;

    if (E.cx < E.coloff)
        E.coloff = E.cx;
    if (E.cx >= E.coloff + content_cols)
        E.coloff = E.cx - content_cols + 1;
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
    if (mode != MODE_VISUAL && mode != MODE_VISUAL_LINE) return 0;

    int ar = vis_anchor_row, ac = vis_anchor_col;
    int cr = cur_row,        cc = cur_col;
    int r0 = ar < cr ? ar : cr;
    int r1 = ar > cr ? ar : cr;
    if (filerow < r0 || filerow > r1) return 0;

    if (mode == MODE_VISUAL_LINE) {
        *c0 = 0; *c1 = INT_MAX;
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

/* Priority (lowest → highest): syntax, bracket match, visual, search. */
static void render_row_content(AppendBuf *ab, const Row *row,
                               int coloff, int visible_len,
                               const SearchQuery *q, int bm0, int bm1,
                               int vis_c0, int vis_c1) {
    if (visible_len <= 0) return;

    unsigned char final_hl[visible_len];
    if (row->hl)
        memcpy(final_hl, &row->hl[coloff], visible_len);
    else
        memset(final_hl, HL_NORMAL, visible_len);

    int bm[2] = {bm0, bm1};
    for (int i = 0; i < 2; i++) {
        int col = bm[i] - coloff;
        if (col >= 0 && col < visible_len)
            final_hl[col] = HL_BRACKET_MATCH;
    }

    if (vis_c0 >= 0) {
        int vc0 = vis_c0 - coloff;
        int vc1 = (vis_c1 == INT_MAX) ? visible_len : vis_c1 - coloff;
        if (vc0 < 0)            vc0 = 0;
        if (vc1 > visible_len)  vc1 = visible_len;
        for (int i = vc0; i < vc1; i++)
            final_hl[i] = HL_VISUAL;
    }

    if (q) {
        int col_end = coloff + visible_len;
        int pos     = coloff;
        while (pos < col_end) {
            int mc, ml;
            if (!q->match_fn(row->chars, row->len,
                             q->pattern, q->pat_len, pos,
                             &mc, &ml) || mc >= col_end)
                break;
            int hl_s = mc - coloff;
            int hl_e = (mc + ml) - coloff;
            if (hl_s < 0)            hl_s = 0;
            if (hl_e > visible_len)  hl_e = visible_len;
            memset(&final_hl[hl_s], HL_SEARCH, hl_e - hl_s);
            pos = mc + ml;
        }
    }

    unsigned char prev  = HL_NORMAL;
    int           any   = 0;
    int           i     = 0;

    while (i < visible_len) {
        unsigned char cur = final_hl[i];
        int j = i + 1;
        while (j < visible_len && final_hl[j] == cur) j++;
        if (cur != prev) {
            ab_append(ab, "\x1b[m", 3);
            const char *esc = hl_to_escape(cur);
            if (esc) ab_append(ab, esc, (int)strlen(esc));
            prev = cur;
            any  = 1;
        }
        ab_append(ab, &row->chars[coloff + i], j - i);
        i = j;
    }

    if (any) ab_append(ab, "\x1b[m", 3);
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
    int is_tree      = E.buftabs[p->buf_idx].is_tree;
    int gw           = is_tree ? 0 : gutter_width_for(buf);
    int content_cols = p->width - gw;

    /* Cascade hl_open_comment for dirty rows. */
    if (syn && buf->hl_dirty_from < buf->numrows) {
        syntax_update_open_comments(syn, buf, buf->hl_dirty_from);
        buf->hl_dirty_from = buf->numrows;
    }

    char mv[24];
    for (int y = 0; y < p->height; y++) {
        int filerow = y + pro;

        /* Move to absolute terminal position. */
        int mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;%dH", p->top + y, p->left);
        ab_append(ab, mv, mvlen);

        /* Erase this pane row (only within pane width). */
        char erase[16];
        int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
        ab_append(ab, erase, elen);

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
            ab_append(ab, "~", 1);
        } else {
            /* Gutter */
            if (gw > 0) {
                char num[16];
                int  nlen = snprintf(num, sizeof(num), "%d", filerow + 1);
                int  pad  = gw - 1 - nlen;
                ab_append(ab, (filerow == pcy) ? "\x1b[1m" : "\x1b[2m", 4);
                while (pad-- > 0) ab_append(ab, " ", 1);
                ab_append(ab, num, nlen);
                ab_append(ab, " ", 1);
                ab_append(ab, "\x1b[m", 3);
            }

            Row *row = &buf->rows[filerow];
            int  len = row->len - pco;
            if (len < 0) len = 0;
            if (len > content_cols) len = content_cols;

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

            if (len > 0) {
                if (hl_q || row->hl || bm0 != -1 || bm1 != -1 || vis_c0 != -1)
                    render_row_content(ab, row, pco, len, hl_q,
                                       bm0, bm1, vis_c0, vis_c1);
                else
                    ab_append(ab, &row->chars[pco], len);
            }
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

    /* Tree pane: special compact status. */
    if (E.buftabs[p->buf_idx].is_tree) {
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

    /* Active = full reverse video; inactive = dim reverse. */
    ab_append(ab, is_active ? "\x1b[7m" : "\x1b[2;7m", is_active ? 4 : 6);

    /* Erase pane width with current attributes. */
    char erase[16];
    int elen = snprintf(erase, sizeof(erase), "\x1b[%dX", p->width);
    ab_append(ab, erase, elen);

    char left[128], right[32];
    const char *name = buf->filename ? buf->filename : "[No Name]";

    int llen, rlen;
    if (is_active) {
        char bufnum[32] = "";
        if (E.num_buftabs > 1)
            snprintf(bufnum, sizeof(bufnum), " [%d/%d]",
                     E.cur_buftab + 1, E.num_buftabs);
        llen = snprintf(left, sizeof(left), " %.30s%s%s",
                        name, buf->dirty ? " [+]" : "", bufnum);
        char prefix[16] = "";
        if (E.count > 0 && E.pending_op)
            snprintf(prefix, sizeof(prefix), "%d%c", E.count, E.pending_op);
        else if (E.count > 0)
            snprintf(prefix, sizeof(prefix), "%d", E.count);
        else if (E.pending_op)
            snprintf(prefix, sizeof(prefix), "%c", E.pending_op);

        char pos[16];
        if (buf->numrows <= 1 || pcy == 0)
            strcpy(pos, "Top");
        else if (pcy >= buf->numrows - 1)
            strcpy(pos, "Bot");
        else
            snprintf(pos, sizeof(pos), "%d%%", (pcy * 100) / (buf->numrows - 1));

        rlen = prefix[0]
            ? snprintf(right, sizeof(right), "%s    %d,%-3d   %-4s",
                       prefix, pcy + 1, pcx + 1, pos)
            : snprintf(right, sizeof(right), "%d,%-3d   %-4s",
                       pcy + 1, pcx + 1, pos);
    } else {
        char pos[16];
        if (buf->numrows <= 1 || pcy == 0)
            strcpy(pos, "Top");
        else if (pcy >= buf->numrows - 1)
            strcpy(pos, "Bot");
        else
            snprintf(pos, sizeof(pos), "%d%%", (pcy * 100) / (buf->numrows - 1));

        llen = snprintf(left,  sizeof(left),  " %.30s%s", name, buf->dirty ? " [+]" : "");
        rlen = snprintf(right, sizeof(right), "%d,%-3d   %-4s", pcy + 1, pcx + 1, pos);
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

static void draw_global_command_bar(AppendBuf *ab) {
    char mv[24];
    int mvlen = snprintf(mv, sizeof(mv), "\x1b[%d;1H", E.term_rows);
    ab_append(ab, mv, mvlen);

    ab_append(ab, "\x1b[K", 3);  /* erase line */

    switch (E.mode) {
        case MODE_INSERT:
            ab_append(ab, "-- INSERT --", 12);
            break;

        case MODE_VISUAL:
            ab_append(ab, "-- VISUAL --", 12);
            break;

        case MODE_VISUAL_LINE:
            ab_append(ab, "-- VISUAL LINE --", 17);
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
}

/* ── Main refresh ────────────────────────────────────────────────────── */

void editor_refresh_screen(void) {
    editor_scroll();

    AppendBuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6);  /* hide cursor */
    ab_append(&ab, "\x1b[m",    3);  /* reset attributes before clear */
    ab_append(&ab, "\x1b[2J",   4);  /* clear entire screen */

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

    /* Cursor shape. */
    ab_append(&ab, (E.mode == MODE_NORMAL) ? "\x1b[1 q" : "\x1b[6 q", 5);

    /* Cursor position. */
    char buf[32];
    int  len;
    Pane *ap = &E.panes[E.cur_pane];
    if (E.mode == MODE_COMMAND) {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       E.term_rows, E.cmdlen + 2);
    } else if (E.mode == MODE_SEARCH) {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       E.term_rows, E.searchlen + 2);
    } else {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       ap->top  + (E.cy - E.rowoff),
                       ap->left + (E.cx - E.coloff) + gutter_width());
    }
    ab_append(&ab, buf, len);
    ab_append(&ab, "\x1b[?25h", 6);  /* show cursor */

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}
