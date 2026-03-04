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

/* Width of the line-number gutter: digits in numrows + 1 trailing space.
   Returns 0 when line numbers are off or the buffer is empty. */
static int gutter_width(void) {
    if (!E.opts.line_numbers || E.buf.numrows == 0) return 0;
    int n = E.buf.numrows, w = 0;
    while (n > 0) { w++; n /= 10; }
    return w + 1;  /* e.g. 3-digit file → "999 " = width 4 */
}

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
    "Copyright (C) 2026 - Jeff",
    "Licensed under GPL",
};
#define SPLASH_LINES ((int)(sizeof(SPLASH) / sizeof(SPLASH[0])))

/* Draw one splash line: '~' always appears at column 0, text is centred
   in the remaining width by letting '~' absorb the first padding space. */
static void draw_splash_line(AppendBuf *ab, int y) {
    int top = (E.screenrows - SPLASH_LINES) / 2;
    int idx = y - top;

    ab_append(ab, "~", 1);

    if (idx < 0 || idx >= SPLASH_LINES)
        return;

    int len = (int)strlen(SPLASH[idx]);
    if (len > E.screencols) len = E.screencols;

    /* '~' already occupies the first column, so subtract 1 from width. */
    int pad = (E.screencols - 1 - len) / 2;
    while (pad-- > 0)
        ab_append(ab, " ", 1);

    ab_append(ab, SPLASH[idx], len);
}

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
   Returns 1 if the row is (at least partially) selected, 0 otherwise.
   *c1 == INT_MAX means "to end of row" and must be resolved by the caller. */
static int visual_col_range(int filerow, int *c0, int *c1) {
    if (E.mode != MODE_VISUAL && E.mode != MODE_VISUAL_LINE) return 0;

    int ar = E.visual_anchor_row, ac = E.visual_anchor_col;
    int cr = E.cy,               cc = E.cx;
    int r0 = ar < cr ? ar : cr;
    int r1 = ar > cr ? ar : cr;
    if (filerow < r0 || filerow > r1) return 0;

    if (E.mode == MODE_VISUAL_LINE) {
        *c0 = 0; *c1 = INT_MAX;
        return 1;
    }

    /* Charwise: determine the ordered start/end positions. */
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

/* Priority (lowest → highest): syntax, bracket match, visual, search.
   bm0/bm1: bracket-match file-column positions on this row (-1 = none).
   vis_c0/vis_c1: visual selection file-column range (-1 = none on this row). */
static void render_row_content(AppendBuf *ab, const Row *row,
                               int coloff, int visible_len,
                               const SearchQuery *q, int bm0, int bm1,
                               int vis_c0, int vis_c1) {
    if (visible_len <= 0) return;

    /* Build a per-character final highlight array for the visible slice. */
    unsigned char final_hl[visible_len];
    if (row->hl)
        memcpy(final_hl, &row->hl[coloff], visible_len);
    else
        memset(final_hl, HL_NORMAL, visible_len);

    /* Overlay bracket match positions. */
    int bm[2] = {bm0, bm1};
    for (int i = 0; i < 2; i++) {
        int col = bm[i] - coloff;
        if (col >= 0 && col < visible_len)
            final_hl[col] = HL_BRACKET_MATCH;
    }

    /* Overlay visual selection (overrides bracket match). */
    if (vis_c0 >= 0) {
        int vc0 = vis_c0 - coloff;
        int vc1 = (vis_c1 == INT_MAX) ? visible_len : vis_c1 - coloff;
        if (vc0 < 0)            vc0 = 0;
        if (vc1 > visible_len)  vc1 = visible_len;
        for (int i = vc0; i < vc1; i++)
            final_hl[i] = HL_VISUAL;
    }

    /* Overlay search matches (highest priority). */
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

    /* Render in same-color runs. */
    unsigned char prev  = HL_NORMAL;
    int           any   = 0;
    int           i     = 0;

    while (i < visible_len) {
        unsigned char cur = final_hl[i];

        /* Find end of this run. */
        int j = i + 1;
        while (j < visible_len && final_hl[j] == cur) j++;

        /* Emit color change when needed. */
        if (cur != prev) {
            ab_append(ab, "\x1b[m", 3);            /* reset first */
            const char *esc = hl_to_escape(cur);
            if (esc) ab_append(ab, esc, (int)strlen(esc));
            prev = cur;
            any  = 1;
        }

        ab_append(ab, &row->chars[coloff + i], j - i);
        i = j;
    }

    if (any) ab_append(ab, "\x1b[m", 3);  /* final reset */
}

/* Find the bracket that matches the one under the cursor (if any).
   Result is stored in E.match_bracket_{valid,row,col}.
   Only active in Normal mode; no syntax awareness (string/comment skipping). */
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

static void editor_draw_rows(AppendBuf *ab) {
    int gw           = gutter_width();
    int content_cols = E.screencols - gw;

    editor_find_bracket_match();

    /* Cascade hl_open_comment for any rows dirtied since last draw. */
    if (E.syntax && E.buf.hl_dirty_from < E.buf.numrows) {
        syntax_update_open_comments(E.syntax, &E.buf, E.buf.hl_dirty_from);
        E.buf.hl_dirty_from = E.buf.numrows;  /* mark clean */
    }

    /* Active search query for match highlighting (NULL = no highlight). */
    SearchQuery        live_q;
    const SearchQuery *hl_query = NULL;
    if (E.mode == MODE_SEARCH && E.searchlen > 0) {
        search_query_init_literal(&live_q, E.searchbuf, E.searchlen);
        hl_query = &live_q;
    } else if (E.last_search_valid) {
        hl_query = &E.last_query;
    }

    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;

        if (E.buf.numrows == 0) {
            /* Empty buffer: splash screen, no gutter. */
            draw_splash_line(ab, y);

        } else if (filerow >= E.buf.numrows) {
            /* Past end of file: '~' at column 0, no gutter (Vim style). */
            ab_append(ab, "~", 1);

        } else {
            /* Content row: gutter then text. */
            if (gw > 0) {
                /* Right-align the line number, then a trailing space. */
                char num[16];
                int  nlen = snprintf(num, sizeof(num), "%d", filerow + 1);
                int  pad  = gw - 1 - nlen;
                ab_append(ab, (filerow == E.cy) ? "\x1b[1m" : "\x1b[2m", 4);
                while (pad-- > 0) ab_append(ab, " ", 1);
                ab_append(ab, num, nlen);
                ab_append(ab, " ", 1);
                ab_append(ab, "\x1b[m", 3);
            }

            Row *row = &E.buf.rows[filerow];
            int  len = row->len - E.coloff;
            if (len < 0) len = 0;
            if (len > content_cols) len = content_cols;

            if (E.syntax)
                syntax_highlight_row(E.syntax, row, row->hl_open_comment);

            /* Bracket match columns for this row (-1 = not on this row). */
            int bm0 = -1, bm1 = -1;
            if (E.match_bracket_valid) {
                if (filerow == E.cy)
                    bm0 = E.cx;
                if (filerow == E.match_bracket_row)
                    bm1 = E.match_bracket_col;
            }

            /* Visual selection range for this row (-1 = not selected). */
            int vis_c0 = -1, vis_c1 = -1;
            {
                int vc0, vc1;
                if (visual_col_range(filerow, &vc0, &vc1)) {
                    vis_c0 = vc0;
                    vis_c1 = (vc1 == INT_MAX) ? row->len : vc1;
                }
            }

            if (len > 0) {
                if (hl_query || row->hl || bm0 != -1 || bm1 != -1 || vis_c0 != -1)
                    render_row_content(ab, row, E.coloff, len, hl_query,
                                       bm0, bm1, vis_c0, vis_c1);
                else
                    ab_append(ab, &row->chars[E.coloff], len);
            }
        }

        ab_append(ab, "\x1b[K", 3);  /* erase to end of line */
        ab_append(ab, "\r\n", 2);
    }
}

static void editor_draw_status_bar(AppendBuf *ab) {
    ab_append(ab, "\x1b[7m", 4);  /* reverse video on */

    char left[128], right[32];
    const char *name = E.buf.filename ? E.buf.filename : "[No Name]";
    int llen = snprintf(left,  sizeof(left),  " %.30s%s",
                        name, E.buf.dirty ? " [+]" : "");
    char prefix[16] = "";
    if (E.count > 0 && E.pending_op)
        snprintf(prefix, sizeof(prefix), "%d%c", E.count, E.pending_op);
    else if (E.count > 0)
        snprintf(prefix, sizeof(prefix), "%d", E.count);
    else if (E.pending_op)
        snprintf(prefix, sizeof(prefix), "%c", E.pending_op);
    int rlen = prefix[0]
        ? snprintf(right, sizeof(right), "%s    %d,%d ", prefix, E.cy + 1, E.cx + 1)
        : snprintf(right, sizeof(right), "%d,%d ", E.cy + 1, E.cx + 1);

    if (llen > E.screencols) llen = E.screencols;
    ab_append(ab, left, llen);

    int gap = E.screencols - llen - rlen;
    while (gap-- > 0)
        ab_append(ab, " ", 1);

    if (llen + rlen <= E.screencols)
        ab_append(ab, right, rlen);

    ab_append(ab, "\x1b[m", 3);   /* reverse video off */
    ab_append(ab, "\r\n", 2);
}

static void editor_draw_command_bar(AppendBuf *ab) {
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
            if (len > E.screencols) len = E.screencols;
            ab_append(ab, line, len);
            break;
        }

        case MODE_SEARCH: {
            char line[258];
            int len = snprintf(line, sizeof(line), "/%s", E.searchbuf);
            if (len > E.screencols) len = E.screencols;
            ab_append(ab, line, len);
            break;
        }

        case MODE_NORMAL:
            if (E.statusmsg[0]) {
                int len = (int)strlen(E.statusmsg);
                if (len > E.screencols) len = E.screencols;
                if (E.statusmsg_is_error)
                    ab_append(ab, "\x1b[41;97m", 8);   /* red bg + white fg */
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

    ab_append(&ab, "\x1b[?25l", 6);  /* hide cursor while drawing */
    ab_append(&ab, "\x1b[H",    3);  /* move to top-left */

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_command_bar(&ab);

    /* Set cursor shape: block in normal, bar in insert/command/search. */
    if (E.mode == MODE_NORMAL)
        ab_append(&ab, "\x1b[1 q", 5);  /* blinking block */
    else
        ab_append(&ab, "\x1b[6 q", 5);  /* steady bar */

    /* Reposition cursor. */
    char buf[32];
    int  len;
    if (E.mode == MODE_COMMAND) {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       E.screenrows + 2, E.cmdlen + 2);
    } else if (E.mode == MODE_SEARCH) {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       E.screenrows + 2, E.searchlen + 2);
    } else {
        len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
                       (E.cy - E.rowoff) + 1,
                       (E.cx - E.coloff) + 1 + gutter_width());
    }
    ab_append(&ab, buf, len);

    ab_append(&ab, "\x1b[?25h", 6);  /* show cursor */

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}
