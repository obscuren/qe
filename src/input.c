// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include "editor.h"
#include "lua_bridge.h"
#include "search.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Key reading ─────────────────────────────────────────────────────── */

int editor_read_key(void) {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (c != '\x1b')
        return c;

    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                }
            }
        } else {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
        }
    }

    return '\x1b';
}

/* ── Editing helpers ─────────────────────────────────────────────────── */

/* Measure leading whitespace on a row; copy up to `cap-1` chars into `buf`.
   Returns the indent length (number of leading space/tab chars). */
static int row_indent(const Row *row, char *buf, int cap) {
    int n = 0;
    while (n < row->len && n < cap - 1 &&
           (row->chars[n] == ' ' || row->chars[n] == '\t')) {
        if (buf) buf[n] = row->chars[n];
        n++;
    }
    if (buf) buf[n] = '\0';
    return n;
}

/* Return the last non-whitespace character in row->chars[0..up_to_cx),
   or 0 if none exists. Used to detect opening brackets. */
static char last_nonws(const Row *row, int up_to_cx) {
    for (int i = up_to_cx - 1; i >= 0; i--) {
        char c = row->chars[i];
        if (c != ' ' && c != '\t') return c;
    }
    return 0;
}

static int is_open_bracket(char c) {
    return c == '{' || c == '(' || c == '[';
}

static int is_close_bracket(char c) {
    return c == '}' || c == ')' || c == ']';
}

static void editor_insert_char(char c) {
    /* If cursor is past the last row (e.g. empty file), add a row first. */
    if (E.cy == E.buf.numrows)
        buf_insert_row(&E.buf, E.buf.numrows, "", 0);

    buf_row_insert_char(&E.buf.rows[E.cy], E.cx, c);
    E.buf.dirty++;
    E.cx++;
    buf_mark_hl_dirty(&E.buf, E.cy);
}

static void editor_insert_newline(void) {
    if (E.buf.numrows == 0) {
        /* Empty file: create two empty rows, land on the second. */
        buf_insert_row(&E.buf, 0, "", 0);
        buf_insert_row(&E.buf, 1, "", 0);
        E.cy = 1;
        E.cx = 0;
        return;
    }

    if (E.cx == 0) {
        /* Insert a blank line above the current row. */
        buf_insert_row(&E.buf, E.cy, "", 0);
    } else {
        /* Capture indentation BEFORE the row array is reallocated. */
        char ind[256];
        int  ind_len = 0;
        int  extra   = 0;
        if (E.opts.autoindent) {
            ind_len = row_indent(&E.buf.rows[E.cy], ind, sizeof(ind));
            if (is_open_bracket(last_nonws(&E.buf.rows[E.cy], E.cx)))
                extra = E.opts.tabwidth;
        }

        /* Split the current row at the cursor. */
        Row *row = &E.buf.rows[E.cy];
        buf_insert_row(&E.buf, E.cy + 1,
                       &row->chars[E.cx], row->len - E.cx);
        /* Truncate the current row at cx (pointer may have moved). */
        row = &E.buf.rows[E.cy];
        row->len = E.cx;
        row->chars[row->len] = '\0';
        E.buf.dirty++;
        buf_mark_hl_dirty(&E.buf, E.cy);

        /* Prepend indentation + any extra bracket indent to the new row. */
        int total = ind_len + extra;
        if (total > 0) {
            /* Insert extra spaces first (they'll end up after the copied indent). */
            for (int i = 0; i < extra; i++)
                buf_row_insert_char(&E.buf.rows[E.cy + 1], 0, ' ');
            /* Then prepend the copied indent in reverse. */
            for (int i = ind_len - 1; i >= 0; i--)
                buf_row_insert_char(&E.buf.rows[E.cy + 1], 0, ind[i]);
            E.buf.dirty++;
        }
        E.cy++;
        E.cx = total;
        return;
    }
    E.cy++;
    E.cx = 0;
}

/* Backspace: delete character to the left of the cursor. */
static void editor_delete_char(void) {
    if (E.cy >= E.buf.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    if (E.cx > 0) {
        buf_row_delete_char(&E.buf.rows[E.cy], E.cx - 1);
        E.buf.dirty++;
        E.cx--;
        buf_mark_hl_dirty(&E.buf, E.cy);
    } else {
        /* Join this row onto the previous one. */
        E.cx = E.buf.rows[E.cy - 1].len;
        buf_row_append_string(&E.buf.rows[E.cy - 1],
                              E.buf.rows[E.cy].chars,
                              E.buf.rows[E.cy].len);
        buf_delete_row(&E.buf, E.cy);
        E.cy--;
        buf_mark_hl_dirty(&E.buf, E.cy);
    }
}

/* ── Word motion ─────────────────────────────────────────────────────── */

/* Character class for word-motion purposes:
     0 = whitespace
     1 = alphanumeric / underscore  (word)
     2 = other printable            (punctuation run) */
static int char_class(char c) {
    if (c == ' ' || c == '\t') return 0;
    if (isalnum((unsigned char)c) || c == '_') return 1;
    return 2;
}

/* Position-computing helpers — modify (*r, *c) in place without touching E.
   Used by both cursor-motion keys and operators. */

/* Advance to the last char of the next token (inclusive). */
static void pos_word_end(int *r, int *c) {
    (*c)++;
    for (;;) {
        if (*r >= E.buf.numrows) return;
        const char *line = E.buf.rows[*r].chars;
        int         len  = E.buf.rows[*r].len;
        while (*c < len && char_class(line[*c]) == 0) (*c)++;
        if (*c >= len) { (*r)++; *c = 0; continue; }
        int cls = char_class(line[*c]);
        while (*c + 1 < len && char_class(line[*c + 1]) == cls) (*c)++;
        return;
    }
}

/* Retreat to the first char of the previous token (inclusive). */
static void pos_word_start(int *r, int *c) {
    (*c)--;
    for (;;) {
        if (*r < 0) return;
        const char *line = E.buf.rows[*r].chars;
        while (*c >= 0 && char_class(line[*c]) == 0) (*c)--;
        if (*c < 0) { (*r)--; if (*r >= 0) *c = E.buf.rows[*r].len - 1; continue; }
        int cls = char_class(line[*c]);
        while (*c - 1 >= 0 && char_class(line[*c - 1]) == cls) (*c)--;
        return;
    }
}

/* Advance to the first char of the next token (skip current token + whitespace). */
static void pos_word_next(int *r, int *c) {
    if (*r >= E.buf.numrows) return;
    const char *line = E.buf.rows[*r].chars;
    int         len  = E.buf.rows[*r].len;
    /* Skip current token. */
    if (*c < len && char_class(line[*c]) != 0) {
        int cls = char_class(line[*c]);
        while (*c < len && char_class(line[*c]) == cls) (*c)++;
    }
    /* Skip whitespace, crossing lines. */
    for (;;) {
        if (*r >= E.buf.numrows) return;
        line = E.buf.rows[*r].chars;
        len  = E.buf.rows[*r].len;
        while (*c < len && char_class(line[*c]) == 0) (*c)++;
        if (*c < len) return;
        (*r)++; *c = 0;
    }
}

/* Cursor-motion wrappers. */
static void editor_move_word_end(void)   { if (E.buf.numrows) pos_word_end(&E.cy, &E.cx);   }
static void editor_move_word_start(void) { if (E.buf.numrows) pos_word_start(&E.cy, &E.cx); }
static void editor_move_word_next(void)  { if (E.buf.numrows) pos_word_next(&E.cy, &E.cx);  }

/* ── Yank register ───────────────────────────────────────────────────── */

static void yank_free(void) {
    for (int i = 0; i < E.yank_numrows; i++) free(E.yank_rows[i]);
    free(E.yank_rows);
    E.yank_rows    = NULL;
    E.yank_numrows = 0;
}

static void yank_set_lines(int r0, int r1) {
    yank_free();
    E.yank_linewise = 1;
    E.yank_numrows  = r1 - r0 + 1;
    E.yank_rows     = malloc(sizeof(char *) * E.yank_numrows);
    for (int i = 0; i < E.yank_numrows; i++)
        E.yank_rows[i] = strdup(E.buf.rows[r0 + i].chars);
}

static void yank_set_chars(int row, int c0, int c1) {
    yank_free();
    E.yank_linewise = 0;
    E.yank_numrows  = 1;
    E.yank_rows     = malloc(sizeof(char *));
    int len         = c1 - c0;
    E.yank_rows[0]  = malloc(len + 1);
    memcpy(E.yank_rows[0], E.buf.rows[row].chars + c0, len);
    E.yank_rows[0][len] = '\0';
}

/* ── Operator + motion ───────────────────────────────────────────────── */

static void push_undo(void) {
    undo_push(&E.undo_stack, editor_capture_state());
    undo_stack_clear(&E.redo_stack);
}

/* Apply operator op ('d', 'y', or 'c') with the given motion key, repeated n times. */
static void editor_apply_op(char op, int motion_key, int n) {
    if (E.buf.numrows == 0) return;

    /* Doubled operator: linewise on n lines starting at cursor (dd/yy/cc + count). */
    if (motion_key == (int)op) {
        int r0 = E.cy;
        int r1 = E.cy + n - 1;
        if (r1 >= E.buf.numrows) r1 = E.buf.numrows - 1;
        yank_set_lines(r0, r1);
        if (op == 'd') {
            push_undo();
            for (int i = r1; i >= r0; i--)
                buf_delete_row(&E.buf, i);
            if (E.cy >= E.buf.numrows && E.cy > 0) E.cy--;
            E.cx = 0;
        } else if (op == 'c') {
            /* cc: delete n lines, keep one empty row, enter Insert. */
            E.pre_insert_snapshot = editor_capture_state();
            E.pre_insert_dirty    = E.buf.dirty;
            E.has_pre_insert      = 1;
            for (int i = r1; i > r0; i--)
                buf_delete_row(&E.buf, i);
            Row *row  = &E.buf.rows[r0];
            row->len      = 0;
            row->chars[0] = '\0';
            E.buf.dirty++;
            buf_mark_hl_dirty(&E.buf, r0);
            E.cy   = r0;
            E.cx   = 0;
            E.mode = MODE_INSERT;
        } else {  /* 'y' */
            int lines = r1 - r0 + 1;
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "%d line%s yanked", lines, lines != 1 ? "s" : "");
        }
        return;
    }

    /* Compute target position by applying the motion n times. */
    int tr = E.cy, tc = E.cx;
    for (int i = 0; i < n; i++) {
        switch (motion_key) {
            case 'w': pos_word_next(&tr, &tc);  break;
            case 'e': pos_word_end(&tr, &tc);   break;
            case 'b': pos_word_start(&tr, &tc); break;
            case '0': tr = E.cy; tc = 0; i = n; break;  /* stop after first */
            case '$': tr = E.cy; tc = E.buf.rows[E.cy].len; i = n; break;
            default:  return;   /* unknown motion: cancel silently */
        }
    }
    /* For 'e': convert inclusive end to exclusive. */
    if (motion_key == 'e' && tr == E.cy) tc++;

    /* Clamp cross-line motions to the current line boundary. */
    if (tr != E.cy) {
        tc = (tr > E.cy) ? E.buf.rows[E.cy].len : 0;
        tr = E.cy;
    }

    /* Build ordered range [c0, c1) on the current row. */
    int c0   = (tc < E.cx) ? tc : E.cx;
    int c1   = (tc < E.cx) ? E.cx : tc;
    int rlen = E.buf.rows[E.cy].len;
    if (c0 > rlen) c0 = rlen;
    if (c1 > rlen) c1 = rlen;
    if (c0 >= c1)  return;

    yank_set_chars(E.cy, c0, c1);
    if (op == 'd' || op == 'c') {
        if (op == 'c') {
            /* Snapshot before the delete so one 'u' restores the pre-change state. */
            E.pre_insert_snapshot = editor_capture_state();
            E.pre_insert_dirty    = E.buf.dirty;
            E.has_pre_insert      = 1;
        } else {
            push_undo();
        }
        Row *row = &E.buf.rows[E.cy];
        for (int i = c1 - 1; i >= c0; i--)
            buf_row_delete_char(row, i);
        E.buf.dirty++;
        buf_mark_hl_dirty(&E.buf, E.cy);
        E.cx = c0;
        if (E.cx > E.buf.rows[E.cy].len) E.cx = E.buf.rows[E.cy].len;
        if (op == 'c') E.mode = MODE_INSERT;
    }
}

/* Paste the yank register. before=1 pastes before cursor/line, 0 = after. */
static void editor_paste(int before) {
    if (!E.yank_rows || E.yank_numrows == 0) return;
    push_undo();

    if (E.yank_linewise) {
        int at = before ? E.cy : E.cy + 1;
        for (int i = 0; i < E.yank_numrows; i++)
            buf_insert_row(&E.buf, at + i,
                           E.yank_rows[i], (int)strlen(E.yank_rows[i]));
        E.cy = at;
        E.cx = 0;
    } else {
        if (E.buf.numrows == 0)
            buf_insert_row(&E.buf, 0, "", 0);
        Row        *row  = &E.buf.rows[E.cy];
        int         ins  = before ? E.cx
                                  : (E.cx < row->len ? E.cx + 1 : row->len);
        const char *text = E.yank_rows[0];
        int         tlen = (int)strlen(text);
        for (int i = tlen - 1; i >= 0; i--)
            buf_row_insert_char(row, ins, text[i]);
        E.buf.dirty++;
        buf_mark_hl_dirty(&E.buf, E.cy);
        E.cx = ins + tlen - 1;
        if (E.cx < 0) E.cx = 0;
    }
}

/* ── Visual mode ─────────────────────────────────────────────────────── */

static void editor_move_cursor(int key);  /* forward declaration */

/* Return ordered selection bounds: [r0,c0] start, [r1,c1] end (inclusive). */
static void visual_get_bounds(int *r0, int *c0, int *r1, int *c1) {
    int ar = E.visual_anchor_row, ac = E.visual_anchor_col;
    int cr = E.cy,               cc = E.cx;
    if (ar < cr || (ar == cr && ac <= cc)) {
        *r0 = ar; *c0 = ac; *r1 = cr; *c1 = cc;
    } else {
        *r0 = cr; *c0 = cc; *r1 = ar; *c1 = ac;
    }
}

static void visual_op_yank(void) {
    int r0, c0, r1, c1;
    visual_get_bounds(&r0, &c0, &r1, &c1);
    int linewise = (E.mode == MODE_VISUAL_LINE) || (r0 != r1);
    E.mode = MODE_NORMAL;
    E.cy = r0; E.cx = c0;
    if (linewise) {
        yank_set_lines(r0, r1);
        snprintf(E.statusmsg, sizeof(E.statusmsg),
                 "%d line%s yanked", r1 - r0 + 1, r1 - r0 ? "s" : "");
    } else {
        int end = c1 + 1;
        if (end > E.buf.rows[r0].len) end = E.buf.rows[r0].len;
        yank_set_chars(r0, c0, end);
    }
}

static void visual_op_delete(void) {
    int r0, c0, r1, c1;
    visual_get_bounds(&r0, &c0, &r1, &c1);
    int linewise = (E.mode == MODE_VISUAL_LINE) || (r0 != r1);
    E.mode = MODE_NORMAL;
    if (linewise) {
        yank_set_lines(r0, r1);
        push_undo();
        for (int i = r1; i >= r0; i--)
            buf_delete_row(&E.buf, i);
        E.cy = r0;
        if (E.cy >= E.buf.numrows && E.cy > 0) E.cy--;
        E.cx = 0;
    } else {
        int end = c1 + 1;
        if (end > E.buf.rows[r0].len) end = E.buf.rows[r0].len;
        yank_set_chars(r0, c0, end);
        push_undo();
        Row *row = &E.buf.rows[r0];
        for (int i = end - 1; i >= c0; i--)
            buf_row_delete_char(row, i);
        E.buf.dirty++;
        buf_mark_hl_dirty(&E.buf, r0);
        E.cy = r0;
        E.cx = c0;
        if (E.cx > row->len) E.cx = row->len;
    }
}

static void visual_op_change(void) {
    int r0, c0, r1, c1;
    visual_get_bounds(&r0, &c0, &r1, &c1);
    int linewise = (E.mode == MODE_VISUAL_LINE) || (r0 != r1);
    /* Snapshot before modification so one 'u' restores pre-change state. */
    E.pre_insert_snapshot = editor_capture_state();
    E.pre_insert_dirty    = E.buf.dirty;
    E.has_pre_insert      = 1;
    E.mode = MODE_INSERT;
    if (linewise) {
        yank_set_lines(r0, r1);
        for (int i = r1; i >= r0; i--)
            buf_delete_row(&E.buf, i);
        buf_insert_row(&E.buf, r0, "", 0);
        E.cy = r0;
        E.cx = 0;
    } else {
        int end = c1 + 1;
        if (end > E.buf.rows[r0].len) end = E.buf.rows[r0].len;
        yank_set_chars(r0, c0, end);
        Row *row = &E.buf.rows[r0];
        for (int i = end - 1; i >= c0; i--)
            buf_row_delete_char(row, i);
        E.buf.dirty++;
        buf_mark_hl_dirty(&E.buf, r0);
        E.cy = r0;
        E.cx = c0;
        if (E.cx > row->len) E.cx = row->len;
    }
}

static void editor_process_visual(int c) {
    if (lua_bridge_call_key(E.mode, c)) return;

    switch (c) {
        case '\x1b':
            E.mode = MODE_NORMAL;
            break;

        case 'v':
            /* Toggle charwise; if already charwise, cancel. */
            E.mode = (E.mode == MODE_VISUAL) ? MODE_NORMAL : MODE_VISUAL;
            break;

        case 'V':
            /* Toggle linewise; if already linewise, cancel. */
            E.mode = (E.mode == MODE_VISUAL_LINE) ? MODE_NORMAL : MODE_VISUAL_LINE;
            break;

        case 'd':
        case 'x':
            visual_op_delete();
            break;

        case 'y':
            visual_op_yank();
            break;

        case 'c':
            visual_op_change();
            break;

        /* Cursor movement — same keys as normal mode. */
        case 'w': editor_move_word_next(); break;
        case 'e': editor_move_word_end();  break;
        case 'b': editor_move_word_start(); break;
        case '0': E.cx = 0; break;
        case '$':
            if (E.cy < E.buf.numrows) {
                int len = E.buf.rows[E.cy].len;
                E.cx = len > 0 ? len - 1 : 0;
            }
            break;
        case 'G':
            if (E.buf.numrows > 0) E.cy = E.buf.numrows - 1;
            break;
        case ARROW_UP:    case 'k':
        case ARROW_DOWN:  case 'j':
        case ARROW_LEFT:  case 'h':
        case ARROW_RIGHT: case 'l':
        case HOME_KEY:
        case END_KEY:
        case PAGE_UP:
        case PAGE_DOWN:
            editor_move_cursor(c);
            break;
    }
}

/* ── Cursor movement ─────────────────────────────────────────────────── */

static void editor_move_cursor(int key) {
    int rowlen = (E.cy < E.buf.numrows) ? E.buf.rows[E.cy].len : 0;

    switch (key) {
        case ARROW_LEFT:
        case 'h':
            if (E.cx > 0) E.cx--;
            break;
        case ARROW_RIGHT:
        case 'l':
            if (E.cx < rowlen) E.cx++;
            break;
        case ARROW_UP:
        case 'k':
            if (E.cy > 0) E.cy--;
            break;
        case ARROW_DOWN:
        case 'j':
            if (E.cy < E.buf.numrows - 1) E.cy++;
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = rowlen;
            break;
        case PAGE_UP:
            E.cy -= E.screenrows;
            if (E.cy < 0) E.cy = 0;
            break;
        case PAGE_DOWN:
            E.cy += E.screenrows;
            if (E.buf.numrows > 0 && E.cy >= E.buf.numrows)
                E.cy = E.buf.numrows - 1;
            else if (E.buf.numrows == 0)
                E.cy = 0;
            break;
    }

    /* Clamp cx to the (possibly new) row length after vertical moves. */
    rowlen = (E.cy < E.buf.numrows) ? E.buf.rows[E.cy].len : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

/* ── Command execution ───────────────────────────────────────────────── */

static void editor_quit(void) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H",  3);
    write(STDOUT_FILENO, "\x1b[0 q", 5);  /* reset cursor shape */
    exit(EXIT_SUCCESS);
}

void editor_execute_command(void) {
    const char *cmd = E.cmdbuf;

    if (strcmp(cmd, "q") == 0) {
        if (E.buf.dirty) {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "Unsaved changes (use :q! to override)");
        } else {
            editor_quit();
        }

    } else if (strcmp(cmd, "q!") == 0) {
        editor_quit();

    } else if (strcmp(cmd, "w") == 0 ||
               (cmd[0] == 'w' && cmd[1] == ' ' && cmd[2])) {
        /* :w [filename] */
        if (cmd[1] == ' ' && cmd[2]) {
            free(E.buf.filename);
            E.buf.filename = strdup(cmd + 2);
            editor_detect_syntax();
        }
        if (!E.buf.filename) {
            snprintf(E.statusmsg, sizeof(E.statusmsg), "No filename");
        } else if (buf_save(&E.buf) == 0) {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "\"%s\" written", E.buf.filename);
        } else {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "Cannot write \"%s\"", E.buf.filename);
        }

    } else if (strcmp(cmd, "wq") == 0) {
        if (!E.buf.filename) {
            snprintf(E.statusmsg, sizeof(E.statusmsg), "No filename");
        } else if (buf_save(&E.buf) == 0) {
            editor_quit();
        } else {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "Cannot write \"%s\"", E.buf.filename);
        }

    } else if (strcmp(cmd, "set nu") == 0) {
        E.opts.line_numbers = 1;
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Line numbers on");

    } else if (strcmp(cmd, "set nonu") == 0) {
        E.opts.line_numbers = 0;
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Line numbers off");

    } else if (cmd[0] != '\0') {
        snprintf(E.statusmsg, sizeof(E.statusmsg),
                 "Not a command: %.100s", cmd);
    }

    E.mode = MODE_NORMAL;
    E.cmdbuf[0] = '\0';
    E.cmdlen    = 0;
}

/* ── Normal mode ─────────────────────────────────────────────────────── */

/* Begin an insert session: snapshot state for undo, then switch mode. */
static void enter_insert_mode(void) {
    E.pre_insert_snapshot = editor_capture_state();
    E.pre_insert_dirty    = E.buf.dirty;
    E.has_pre_insert      = 1;
    E.mode                = MODE_INSERT;
}

static void editor_undo(void) {
    UndoState prev;
    if (undo_pop(&E.undo_stack, &prev) == -1) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Already at oldest change");
        return;
    }
    undo_push(&E.redo_stack, editor_capture_state());
    editor_restore_state(&prev);
    undo_state_free(&prev);
}

static void editor_redo(void) {
    UndoState next;
    if (undo_pop(&E.redo_stack, &next) == -1) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Already at newest change");
        return;
    }
    undo_push(&E.undo_stack, editor_capture_state());
    editor_restore_state(&next);
    undo_state_free(&next);
}

static void editor_process_normal(int c) {
    E.statusmsg[0] = '\0';  /* clear one-shot message on any keypress */
    if (lua_bridge_call_key(MODE_NORMAL, c)) return;

    /* Accumulate count prefix digits: 1-9 always; 0 only if count already started. */
    if ((c >= '1' && c <= '9') || (c == '0' && E.count > 0)) {
        E.count = E.count * 10 + (c - '0');
        return;
    }

    /* If an operator is pending, this key is the motion. */
    if (E.pending_op) {
        char op      = E.pending_op;
        E.pending_op = '\0';
        int n        = E.count > 0 ? E.count : 1;
        E.count      = 0;
        editor_apply_op(op, c, n);
        return;
    }

    /* Operators: set pending_op WITHOUT consuming count (e.g. 3dd needs it). */
    if (c == 'd' || c == 'y' || c == 'c') {
        E.pending_op = (char)c;
        return;
    }

    /* Consume count for all other keys. */
    int raw_count = E.count;
    int n         = raw_count > 0 ? raw_count : 1;
    E.count       = 0;

    switch (c) {
        /* --- cancel / ESC --- */
        case '\x1b':
            break;

        /* --- mode switches (count ignored) --- */
        case ':':
            E.mode = MODE_COMMAND;
            E.cmdbuf[0] = '\0';
            E.cmdlen    = 0;
            break;

        case '/':
            E.mode = MODE_SEARCH;
            E.searchbuf[0] = '\0';
            E.searchlen    = 0;
            break;

        case 'i':
            enter_insert_mode();
            break;

        case 'a':
            if (E.cy < E.buf.numrows && E.cx < E.buf.rows[E.cy].len)
                E.cx++;
            enter_insert_mode();
            break;

        case 'A':
            if (E.cy < E.buf.numrows)
                E.cx = E.buf.rows[E.cy].len;
            enter_insert_mode();
            break;

        case 'o': {
            char ind[256]; int ind_len = 0, extra = 0;
            if (E.opts.autoindent && E.cy < E.buf.numrows) {
                const Row *r = &E.buf.rows[E.cy];
                ind_len = row_indent(r, ind, sizeof(ind));
                if (is_open_bracket(last_nonws(r, r->len)))
                    extra = E.opts.tabwidth;
            }
            enter_insert_mode();
            int at = E.cy + 1;
            if (at > E.buf.numrows) at = E.buf.numrows;
            char row_init[512];
            memcpy(row_init, ind, ind_len);
            memset(row_init + ind_len, ' ', extra);
            buf_insert_row(&E.buf, at, row_init, ind_len + extra);
            E.cy = at;
            E.cx = ind_len + extra;
            break;
        }

        case 'O': {
            char ind[256]; int ind_len = 0;
            if (E.opts.autoindent && E.cy < E.buf.numrows)
                ind_len = row_indent(&E.buf.rows[E.cy], ind, sizeof(ind));
            enter_insert_mode();
            buf_insert_row(&E.buf, E.cy, ind, ind_len);
            E.cx = ind_len;
            break;
        }

        /* --- x: delete n chars under/after cursor --- */
        case 'x':
            if (E.cy < E.buf.numrows) {
                Row *row = &E.buf.rows[E.cy];
                if (E.cx < row->len) {
                    push_undo();
                    int del = n;
                    if (E.cx + del > row->len) del = row->len - E.cx;
                    for (int i = 0; i < del; i++)
                        buf_row_delete_char(row, E.cx);
                    E.buf.dirty++;
                    buf_mark_hl_dirty(&E.buf, E.cy);
                    if (E.cx > 0 && E.cx >= row->len) E.cx--;
                }
            }
            break;

        /* --- undo / redo (n times) --- */
        case 'u':
            for (int i = 0; i < n; i++) editor_undo();
            break;

        case 'r':
            for (int i = 0; i < n; i++) editor_redo();
            break;

        /* --- search repeat (n times) --- */
        case 'n': {
            if (!E.last_search_valid) break;
            for (int i = 0; i < n; i++) {
                int row, col;
                if (search_find_next(&E.last_query, &E.buf, E.cy, E.cx, &row, &col)) {
                    E.cy = row; E.cx = col;
                } else {
                    snprintf(E.statusmsg, sizeof(E.statusmsg), "Pattern not found");
                    break;
                }
            }
            break;
        }

        case 'N': {
            if (!E.last_search_valid) break;
            for (int i = 0; i < n; i++) {
                int row, col;
                if (search_find_prev(&E.last_query, &E.buf, E.cy, E.cx, &row, &col)) {
                    E.cy = row; E.cx = col;
                } else {
                    snprintf(E.statusmsg, sizeof(E.statusmsg), "Pattern not found");
                    break;
                }
            }
            break;
        }

        /* --- visual mode (count ignored) --- */
        case 'v':
            E.visual_anchor_row = E.cy;
            E.visual_anchor_col = E.cx;
            E.mode = MODE_VISUAL;
            break;

        case 'V':
            E.visual_anchor_row = E.cy;
            E.visual_anchor_col = 0;
            E.mode = MODE_VISUAL_LINE;
            break;

        /* --- paste n times --- */
        case 'p':
            for (int i = 0; i < n; i++) editor_paste(0);
            break;

        case 'P':
            for (int i = 0; i < n; i++) editor_paste(1);
            break;

        /* --- word motions (n times) --- */
        case 'w':
            for (int i = 0; i < n; i++) editor_move_word_next();
            break;

        case 'e':
            for (int i = 0; i < n; i++) editor_move_word_end();
            break;

        case 'E':
            if (E.cy < E.buf.numrows) {
                int len = E.buf.rows[E.cy].len;
                E.cx = len > 0 ? len - 1 : 0;
            }
            break;

        case 'b':
            for (int i = 0; i < n; i++) editor_move_word_start();
            break;

        case 'B':
            E.cx = 0;
            break;

        /* --- line motions (count ignored) --- */
        case '0':
            E.cx = 0;
            break;

        case '$':
            if (E.cy < E.buf.numrows) {
                int len = E.buf.rows[E.cy].len;
                E.cx = len > 0 ? len - 1 : 0;
            }
            break;

        /* --- G: nG = go to line n, G = last line --- */
        case 'G':
            if (raw_count > 0) {
                E.cy = raw_count - 1;  /* 1-based input → 0-based index */
                if (E.cy >= E.buf.numrows && E.buf.numrows > 0)
                    E.cy = E.buf.numrows - 1;
            } else {
                if (E.buf.numrows > 0) E.cy = E.buf.numrows - 1;
            }
            break;

        /* --- cursor movement (n times) --- */
        case ARROW_UP:    case 'k':
        case ARROW_DOWN:  case 'j':
        case ARROW_LEFT:  case 'h':
        case ARROW_RIGHT: case 'l':
        case PAGE_UP:
        case PAGE_DOWN:
            for (int i = 0; i < n; i++) editor_move_cursor(c);
            break;

        case HOME_KEY:
        case END_KEY:
            editor_move_cursor(c);
            break;
    }
}

/* ── Insert mode ─────────────────────────────────────────────────────── */

/* Insert a closing bracket, dedenting the current line by one level first
   if the cursor has only whitespace to its left. */
static void editor_close_bracket(char c) {
    if (E.cy < E.buf.numrows) {
        Row *row = &E.buf.rows[E.cy];
        int all_ws = 1;
        for (int i = 0; i < E.cx; i++) {
            if (row->chars[i] != ' ' && row->chars[i] != '\t') {
                all_ws = 0;
                break;
            }
        }
        if (all_ws && E.cx > 0) {
            int remove = E.cx < E.opts.tabwidth ? E.cx : E.opts.tabwidth;
            for (int i = 0; i < remove; i++)
                buf_row_delete_char(row, 0);
            E.cx -= remove;
            E.buf.dirty++;
            buf_mark_hl_dirty(&E.buf, E.cy);
        }
    }
    editor_insert_char(c);
}

static void editor_process_insert(int c) {
    if (lua_bridge_call_key(MODE_INSERT, c)) return;
    switch (c) {
        case '\x1b':
            if (E.cx > 0) E.cx--;  /* nudge left on Escape, like Vim */
            E.mode = MODE_NORMAL;
            /* Push the pre-insert snapshot if anything changed. */
            if (E.has_pre_insert) {
                if (E.buf.dirty != E.pre_insert_dirty) {
                    undo_push(&E.undo_stack, E.pre_insert_snapshot);
                    undo_stack_clear(&E.redo_stack);
                } else {
                    undo_state_free(&E.pre_insert_snapshot);
                }
                E.has_pre_insert = 0;
            }
            break;

        case '\r':
            editor_insert_newline();
            break;

        case 127:  /* Backspace */
            editor_delete_char();
            break;

        case '\t': {
            /* Insert spaces up to the next tab stop. */
            int spaces = E.opts.tabwidth - (E.cx % E.opts.tabwidth);
            for (int i = 0; i < spaces; i++)
                editor_insert_char(' ');
            break;
        }

        default:
            if (c >= 32 && c < 127) {
                if (E.opts.autoindent && is_close_bracket((char)c))
                    editor_close_bracket((char)c);
                else
                    editor_insert_char((char)c);
            }
            break;
    }
}

/* ── Command mode ────────────────────────────────────────────────────── */

static void editor_process_command(int c) {
    if (lua_bridge_call_key(MODE_COMMAND, c)) return;
    switch (c) {
        case '\x1b':
            E.mode = MODE_NORMAL;
            E.cmdbuf[0] = '\0';
            E.cmdlen    = 0;
            break;

        case '\r':
            editor_execute_command();
            break;

        case 127:  /* Backspace */
            if (E.cmdlen > 0) {
                E.cmdbuf[--E.cmdlen] = '\0';
                if (E.cmdlen == 0)
                    E.mode = MODE_NORMAL;
            }
            break;

        default:
            if (c >= 32 && c < 127 &&
                E.cmdlen < (int)sizeof(E.cmdbuf) - 1) {
                E.cmdbuf[E.cmdlen++] = (char)c;
                E.cmdbuf[E.cmdlen]   = '\0';
            }
            break;
    }
}

/* ── Search mode ─────────────────────────────────────────────────────── */

static void editor_process_search(int c) {
    if (lua_bridge_call_key(MODE_SEARCH, c)) return;
    switch (c) {
        case '\x1b':
            E.mode = MODE_NORMAL;
            E.searchbuf[0] = '\0';
            E.searchlen    = 0;
            break;

        case '\r': {
            E.mode = MODE_NORMAL;
            if (E.searchlen > 0) {
                search_query_init_literal(&E.last_query,
                                          E.searchbuf, E.searchlen);
                E.last_search_valid = 1;
                int row, col;
                if (search_find_next(&E.last_query, &E.buf,
                                     E.cy, E.cx, &row, &col)) {
                    E.cy = row;
                    E.cx = col;
                } else {
                    snprintf(E.statusmsg, sizeof(E.statusmsg),
                             "Pattern not found");
                }
            }
            break;
        }

        case 127:  /* Backspace */
            if (E.searchlen > 0) {
                E.searchbuf[--E.searchlen] = '\0';
                if (E.searchlen == 0)
                    E.mode = MODE_NORMAL;
            }
            break;

        default:
            if (c >= 32 && c < 127 &&
                E.searchlen < (int)sizeof(E.searchbuf) - 1) {
                E.searchbuf[E.searchlen++] = (char)c;
                E.searchbuf[E.searchlen]   = '\0';
            }
            break;
    }
}

/* ── Dispatch ────────────────────────────────────────────────────────── */

void editor_process_keypress(void) {
    int c = editor_read_key();

    switch (E.mode) {
        case MODE_NORMAL:      editor_process_normal(c);  break;
        case MODE_INSERT:      editor_process_insert(c);  break;
        case MODE_COMMAND:     editor_process_command(c); break;
        case MODE_SEARCH:      editor_process_search(c);  break;
        case MODE_VISUAL:
        case MODE_VISUAL_LINE: editor_process_visual(c);  break;
    }
}
