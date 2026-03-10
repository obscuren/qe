// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"
#include "editor.h"
#include "fuzzy.h"
#include "git.h"
#include "qf.h"
#include "lua_bridge.h"
#include "search.h"
#include "term_emu.h"
#include "tree.h"
#include "utils.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Status message helpers ──────────────────────────────────────────── */

static void status_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_is_error = 0;
}

static void status_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_is_error = 1;
}

/* ── Key reading ─────────────────────────────────────────────────────── */

int editor_read_key(void) {
    int nread;
    char c;

again:
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
        /* SGR mouse: \x1b[<btn;x;yM (press) or \x1b[<btn;x;ym (release) */
        if (seq[1] == '<') {
            char buf[32]; int len = 0; char ch = 0;
            while (len < 31 && read(STDIN_FILENO, &ch, 1) == 1
                   && ch != 'M' && ch != 'm')
                buf[len++] = ch;
            buf[len] = '\0';
            int btn = 0, mx = 0, my = 0;
            sscanf(buf, "%d;%d;%d", &btn, &mx, &my);
            if (ch == 'M') {  /* press / drag */
                E.mouse_x = mx;
                E.mouse_y = my;
                if (btn == 0)  return MOUSE_PRESS;
                if (btn == 64) return MOUSE_SCROLL_UP;
                if (btn == 65) return MOUSE_SCROLL_DOWN;
            }
            goto again;  /* release or unhandled button — ignore */
        }

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

/* Auto-pair helpers. */
static char autopair_close(char c) {
    switch (c) {
        case '(': return ')';
        case '{': return '}';
        case '[': return ']';
        case '"': return '"';
        case '\'': return '\'';
        default: return 0;
    }
}

static int is_autopair_open(char c) {
    return autopair_close(c) != 0;
}

/* Return the char at cursor position, or 0 if at/past end of line. */
static char char_at_cursor(void) {
    if (E.cy >= E.buf.numrows) return 0;
    Row *row = &E.buf.rows[E.cy];
    if (E.cx >= row->len) return 0;
    return row->chars[E.cx];
}

/* Return the char before cursor, or 0. */
static char char_before_cursor(void) {
    if (E.cy >= E.buf.numrows || E.cx <= 0) return 0;
    return E.buf.rows[E.cy].chars[E.cx - 1];
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

/* ── Fold helpers ────────────────────────────────────────────────────── */

/* Can this row be a fold header? (has subsequent rows with greater indent) */
static int fold_can_fold(int row) {
    if (row >= E.buf.numrows - 1) return 0;
    return buf_fold_end(&E.buf, row, E.opts.tabwidth) > row;
}

static void fold_close(int row) {
    if (!fold_can_fold(row)) return;
    buf_ensure_folds(&E.buf);
    E.buf.folds[row] = 1;
}

static void fold_open(int row) {
    if (!E.buf.folds || row >= E.buf.folds_cap) return;
    E.buf.folds[row] = 0;
}

/* Is the given row hidden inside a closed fold? */
static int fold_row_hidden(int row) {
    if (!E.buf.folds || row <= 0) return 0;
    for (int r = 0; r < row; r++) {
        if (r < E.buf.folds_cap && E.buf.folds[r]) {
            int end = buf_fold_end(&E.buf, r, E.opts.tabwidth);
            if (row <= end) return 1;
            r = end; /* skip past this fold */
        }
    }
    return 0;
}

/* Find the fold header that hides the given row, or -1. */
static int fold_parent(int row) {
    if (!E.buf.folds || row <= 0) return -1;
    for (int r = row - 1; r >= 0; r--) {
        if (r < E.buf.folds_cap && E.buf.folds[r]) {
            int end = buf_fold_end(&E.buf, r, E.opts.tabwidth);
            if (row <= end) return r;
        }
    }
    return -1;
}

/* Move cursor to the next visible row (forward). */
static void fold_skip_forward(void) {
    while (E.cy < E.buf.numrows - 1 && fold_row_hidden(E.cy))
        E.cy++;
}

/* Ensure E.cy is on a visible row after a fold toggle. */
static void fold_fix_cursor(void) {
    if (fold_row_hidden(E.cy)) {
        int p = fold_parent(E.cy);
        if (p >= 0) E.cy = p;
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

/* Find the nth occurrence of target on the current line using f/F/t/T semantics.
   Updates *c in place; *r is unchanged (f/F/t/T are line-bound).
   Returns 1 if found, 0 if not. */
static int pos_find_char(int *r, int *c, char key, char target, int n) {
    if (*r >= E.buf.numrows) return 0;
    const char *line = E.buf.rows[*r].chars;
    int len = E.buf.rows[*r].len;
    int cnt = 0;

    if (key == 'f' || key == 't') {
        for (int i = *c + 1; i < len; i++) {
            if (line[i] == target && ++cnt == n) {
                *c = (key == 'f') ? i : i - 1;
                return 1;
            }
        }
    } else {  /* F or T */
        for (int i = *c - 1; i >= 0; i--) {
            if (line[i] == target && ++cnt == n) {
                *c = (key == 'F') ? i : i + 1;
                return 1;
            }
        }
    }
    return 0;
}

/* ── Yank registers ──────────────────────────────────────────────────── */

/* Consume pending_reg and return the target register index (0 = unnamed). */
static int reg_consume(void) {
    int r = E.pending_reg;
    E.pending_reg = -1;
    if (r == REG_CLIPBOARD) return REG_CLIPBOARD;
    return (r >= 1 && r <= 26) ? r : 0;
}

/* Free a single register. */
static void reg_free(int ri) {
    for (int i = 0; i < E.regs[ri].numrows; i++) free(E.regs[ri].rows[i]);
    free(E.regs[ri].rows);
    E.regs[ri].rows    = NULL;
    E.regs[ri].numrows = 0;
}

/* Copy register src into dst (deep copy). */
static void reg_copy(int dst, int src) {
    reg_free(dst);
    E.regs[dst].linewise = E.regs[src].linewise;
    E.regs[dst].numrows  = E.regs[src].numrows;
    E.regs[dst].rows     = malloc(sizeof(char *) * E.regs[src].numrows);
    for (int i = 0; i < E.regs[src].numrows; i++)
        E.regs[dst].rows[i] = strdup(E.regs[src].rows[i]);
}

/* Copy register contents to system clipboard via available tool. */
static void clipboard_copy(int ri) {
    if (!E.regs[ri].rows || E.regs[ri].numrows == 0) return;
    /* Try wl-copy (Wayland), then xclip, then xsel. */
    const char *cmds[] = {
        "wl-copy 2>/dev/null",
        "xclip -selection clipboard 2>/dev/null",
        "xsel --clipboard --input 2>/dev/null",
        NULL
    };
    for (int ci = 0; cmds[ci]; ci++) {
        FILE *fp = popen(cmds[ci], "w");
        if (!fp) continue;
        for (int i = 0; i < E.regs[ri].numrows; i++) {
            fputs(E.regs[ri].rows[i], fp);
            if (i < E.regs[ri].numrows - 1 || E.regs[ri].linewise)
                fputc('\n', fp);
        }
        if (pclose(fp) == 0) return;
    }
}

/* Fetch system clipboard into a register. */
static void clipboard_paste(int ri) {
    const char *cmds[] = {
        "wl-paste -n 2>/dev/null",
        "xclip -selection clipboard -o 2>/dev/null",
        "xsel --clipboard --output 2>/dev/null",
        NULL
    };
    for (int ci = 0; cmds[ci]; ci++) {
        FILE *fp = popen(cmds[ci], "r");
        if (!fp) continue;
        char *buf = NULL; size_t cap = 0; ssize_t len;
        int nrows = 0;
        char **rows = NULL;
        while ((len = getline(&buf, &cap, fp)) > 0) {
            if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
            rows = realloc(rows, sizeof(char *) * (nrows + 1));
            rows[nrows++] = strdup(buf);
        }
        free(buf);
        if (pclose(fp) == 0 && nrows > 0) {
            reg_free(ri);
            E.regs[ri].rows     = rows;
            E.regs[ri].numrows  = nrows;
            E.regs[ri].linewise = 0;
            return;
        }
        for (int i = 0; i < nrows; i++) free(rows[i]);
        free(rows);
    }
}

/* Write to a register + always mirror into unnamed (reg 0).
   If writing to clipboard register, also push to system clipboard. */
static void reg_write(int ri, int linewise, char **rows, int numrows) {
    reg_free(ri);
    E.regs[ri].linewise = linewise;
    E.regs[ri].numrows  = numrows;
    E.regs[ri].rows     = rows;
    if (ri != 0) reg_copy(0, ri);
    if (ri == REG_CLIPBOARD) clipboard_copy(ri);
}

static void yank_set_lines(int r0, int r1) {
    int ri = reg_consume();
    int n  = r1 - r0 + 1;
    char **rows = malloc(sizeof(char *) * n);
    for (int i = 0; i < n; i++)
        rows[i] = strdup(E.buf.rows[r0 + i].chars);
    reg_write(ri, 1, rows, n);
}

static void yank_set_chars(int row, int c0, int c1) {
    int ri  = reg_consume();
    int len = c1 - c0;
    char **rows = malloc(sizeof(char *));
    rows[0] = malloc(len + 1);
    memcpy(rows[0], E.buf.rows[row].chars + c0, len);
    rows[0][len] = '\0';
    reg_write(ri, 0, rows, 1);
}

/* Charwise yank spanning multiple rows: row sr cols [sc..end),
   full rows sr+1..er-1, row er cols [0..ec). */
static void yank_set_multiline_chars(int sr, int sc, int er, int ec) {
    int ri = reg_consume();
    int n  = er - sr + 1;
    char **rows = malloc(sizeof(char *) * n);
    for (int i = 0; i < n; i++) {
        int r   = sr + i;
        int len = E.buf.rows[r].len;
        int c0  = (i == 0)       ? sc : 0;
        int c1  = (i == n - 1)   ? (ec < len ? ec : len) : len;
        int seg = c1 - c0; if (seg < 0) seg = 0;
        rows[i] = malloc(seg + 1);
        if (seg > 0) memcpy(rows[i], E.buf.rows[r].chars + c0, seg);
        rows[i][seg] = '\0';
    }
    reg_write(ri, 0, rows, n);
}

static void editor_close_bracket(char c);  /* forward declaration */

/* ── Macro recording / playback ──────────────────────────────────────── */

static void macro_record_key(int c) {
    if (E.macro_len >= E.macro_cap) {
        E.macro_cap = E.macro_cap ? E.macro_cap * 2 : 64;
        E.macro_buf = realloc(E.macro_buf, sizeof(int) * E.macro_cap);
    }
    E.macro_buf[E.macro_len++] = c;
}

static void macro_stop(void) {
    int ri = E.recording_reg;
    E.recording_reg = -1;
    if (ri < 0 || ri >= MACRO_REGS) return;
    free(E.macros[ri].keys);
    E.macros[ri].keys = E.macro_buf;
    E.macros[ri].len  = E.macro_len;
    E.macro_buf = NULL;
    E.macro_len = 0;
    E.macro_cap = 0;
    status_msg("Recorded @%c (%d keys)", 'a' + ri, E.macros[ri].len);
}

/* macro_play() defined just before editor_process_keypress(). */
static void macro_play(int ri, int count);

/* ── . repeat helpers ────────────────────────────────────────────────── */

static void la_free(void) {
    free(E.last_action.text);
    E.last_action.text        = NULL;
    E.last_action.text_len    = 0;
    E.last_action.find_target = '\0';
    E.last_action.type        = LA_NONE;
}

static void insert_rec_reset(void) {
    E.insert_rec_len = 0;
}

static void insert_rec_append(char c) {
    if (E.insert_rec_len + 1 > E.insert_rec_cap) {
        int cap = E.insert_rec_cap > 0 ? E.insert_rec_cap * 2 : 64;
        E.insert_rec     = realloc(E.insert_rec, cap);
        E.insert_rec_cap = cap;
    }
    if (E.insert_rec) E.insert_rec[E.insert_rec_len++] = c;
}

/* Replay previously recorded insert text without changing mode or recording. */
static void replay_insert_text(const char *text, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r') {
            editor_insert_newline();
        } else if (c == 127) {
            editor_delete_char();
        } else if (c == '\t') {
            int spaces = E.opts.tabwidth - (E.cx % E.opts.tabwidth);
            for (int j = 0; j < spaces; j++) editor_insert_char(' ');
        } else if (c >= 32 && c < 127) {
            if (E.opts.autoindent && is_close_bracket((char)c))
                editor_close_bracket((char)c);
            else if (E.opts.autopairs && is_autopair_open((char)c)) {
                if ((c == '"' || c == '\'') && char_at_cursor() == (char)c)
                    E.cx++;
                else {
                    editor_insert_char((char)c);
                    editor_insert_char(autopair_close((char)c));
                    E.cx--;
                }
            } else if (E.opts.autopairs && is_close_bracket((char)c)
                       && char_at_cursor() == (char)c) {
                E.cx++;
            } else {
                editor_insert_char((char)c);
            }
        }
    }
}

/* ── Operator + motion ───────────────────────────────────────────────── */

static void push_undo(const char *desc) {
    if (!E.undo_tree.root)
        undo_tree_set_root(&E.undo_tree, editor_capture_state(), "initial");
    else
        undo_tree_push(&E.undo_tree, editor_capture_state(), desc);
}

/* Indent (dir='>') or outdent (dir='<') lines r0..r1 by one tabwidth. */
static void indent_lines(int r0, int r1, char dir) {
    int tw = E.opts.tabwidth;
    if (r1 >= E.buf.numrows) r1 = E.buf.numrows - 1;
    for (int r = r0; r <= r1; r++) {
        Row *row = &E.buf.rows[r];
        if (dir == '>') {
            for (int i = 0; i < tw; i++)
                buf_row_insert_char(row, 0, ' ');
            E.buf.dirty++;
        } else {
            int remove = 0;
            while (remove < tw && remove < row->len && row->chars[remove] == ' ')
                remove++;
            if (remove > 0) {
                for (int i = 0; i < remove; i++)
                    buf_row_delete_char(row, 0);
                E.buf.dirty++;
            }
        }
        buf_mark_hl_dirty(&E.buf, r);
    }
}

/* ── Text objects ────────────────────────────────────────────────────── */

/* iw/aw: word under cursor.  Half-open range [c0,c1) on row E.cy. */
static int text_obj_word(char ia, int *r0, int *c0, int *r1, int *c1) {
    if (E.buf.numrows == 0) return 0;
    Row *rw = &E.buf.rows[E.cy];
    if (rw->len == 0) return 0;
    int cx = E.cx < rw->len ? E.cx : rw->len - 1;
    int cc = char_class(rw->chars[cx]);
    int start = cx, end = cx;
    while (start > 0 && char_class(rw->chars[start - 1]) == cc) start--;
    while (end + 1 < rw->len && char_class(rw->chars[end + 1]) == cc) end++;
    if (ia == 'a') {
        /* prefer trailing whitespace; fall back to leading */
        if (end + 1 < rw->len && char_class(rw->chars[end + 1]) == 0)
            while (end + 1 < rw->len && char_class(rw->chars[end + 1]) == 0) end++;
        else
            while (start > 0 && char_class(rw->chars[start - 1]) == 0) start--;
    }
    *r0 = E.cy; *c0 = start; *r1 = E.cy; *c1 = end + 1;
    return 1;
}

/* i"/a" (also i' a' i` a`): quote pair on the same line.
   Scans left-to-right for consecutive pairs; uses the first one that
   contains or follows the cursor. */
static int text_obj_quotes(char ia, char q,
                           int *r0, int *c0, int *r1, int *c1) {
    if (E.buf.numrows == 0) return 0;
    Row *rw = &E.buf.rows[E.cy];
    int cx = E.cx;
    int left = -1, right = -1;
    int i = 0;
    while (i < rw->len) {
        if (rw->chars[i] != q) { i++; continue; }
        int j = i + 1;
        while (j < rw->len && rw->chars[j] != q) j++;
        if (j >= rw->len) break;          /* unmatched */
        if (i <= cx && cx <= j) { left = i; right = j; break; }   /* inside */
        if (i > cx)             { left = i; right = j; break; }   /* to the right */
        i = j + 1;
    }
    if (left == -1 || right == -1) return 0;
    *r0 = E.cy; *r1 = E.cy;
    if (ia == 'i') { *c0 = left + 1; *c1 = right; }
    else           { *c0 = left;     *c1 = right + 1; }
    return (*c0 < *c1);
}

/* i(/a( etc.: enclosing bracket pair.  Multi-line aware. */
static int text_obj_bracket(char ia, char open, char close,
                            int *r0, int *c0, int *r1, int *c1) {
    int or_ = -1, oc = -1, cr_ = -1, cc_ = -1;

    /* Search backward for the opening bracket. */
    {
        int depth = 0;
        /* Cursor on the open bracket: use it directly. */
        if (E.cy < E.buf.numrows && E.cx < E.buf.rows[E.cy].len &&
                E.buf.rows[E.cy].chars[E.cx] == open) {
            or_ = E.cy; oc = E.cx;
        } else {
            for (int r = E.cy; r >= 0 && or_ == -1; r--) {
                Row *rw = &E.buf.rows[r];
                int c_end = (r == E.cy) ? E.cx - 1 : rw->len - 1;
                for (int k = c_end; k >= 0 && or_ == -1; k--) {
                    if      (rw->chars[k] == close) depth++;
                    else if (rw->chars[k] == open) {
                        if (depth == 0) { or_ = r; oc = k; }
                        else            depth--;
                    }
                }
            }
        }
    }
    if (or_ == -1) return 0;

    /* Search forward for the matching close bracket. */
    {
        int depth = 0;
        for (int r = or_; r < E.buf.numrows && cr_ == -1; r++) {
            Row *rw = &E.buf.rows[r];
            int c_start = (r == or_) ? oc + 1 : 0;
            for (int k = c_start; k < rw->len && cr_ == -1; k++) {
                if      (rw->chars[k] == open)  depth++;
                else if (rw->chars[k] == close) {
                    if (depth == 0) { cr_ = r; cc_ = k; }
                    else            depth--;
                }
            }
        }
    }
    if (cr_ == -1) return 0;

    if (ia == 'i') { *r0 = or_; *c0 = oc + 1; *r1 = cr_; *c1 = cc_; }
    else           { *r0 = or_; *c0 = oc;     *r1 = cr_; *c1 = cc_ + 1; }
    return (*r0 != *r1 || *c0 < *c1);
}

/* Dispatcher: fills half-open range [r0,c0)..[r1,c1). */
static int text_object_range(char ia, char obj,
                             int *r0, int *c0, int *r1, int *c1) {
    switch (obj) {
        case 'w':
            return text_obj_word(ia, r0, c0, r1, c1);
        case '"': case '\'': case '`':
            return text_obj_quotes(ia, obj, r0, c0, r1, c1);
        case '(': case ')': case 'b':
            return text_obj_bracket(ia, '(', ')', r0, c0, r1, c1);
        case '[': case ']':
            return text_obj_bracket(ia, '[', ']', r0, c0, r1, c1);
        case '{': case '}': case 'B':
            return text_obj_bracket(ia, '{', '}', r0, c0, r1, c1);
        case '<': case '>':
            return text_obj_bracket(ia, '<', '>', r0, c0, r1, c1);
        default: return 0;
    }
}

/* Apply op ('d','y','c') over half-open char range [r0,c0]..[r1,c1).
   Sets up last_action / insert_motion for dot repeat. */
static void apply_textobj_op(char op, int r0, int c0, int r1, int c1,
                              char ia, char obj) {
    if (r0 > r1 || (r0 == r1 && c0 >= c1)) return;
    if (r1 >= E.buf.numrows) r1 = E.buf.numrows - 1;
    if (c1 > E.buf.rows[r1].len) c1 = E.buf.rows[r1].len;
    if (c0 > E.buf.rows[r0].len) c0 = E.buf.rows[r0].len;
    if (r0 == r1 && c0 >= c1) return;

    if (r0 == r1) {
        /* Same line. */
        yank_set_chars(r0, c0, c1);
        if (op == 'd' || op == 'c') {
            if (op == 'c') {
                E.pre_insert_snapshot = editor_capture_state();
                E.pre_insert_dirty    = E.buf.dirty;
                E.has_pre_insert      = 1;
            } else {
                push_undo("delete");
            }
            Row *rw = &E.buf.rows[r0];
            for (int i = c1 - 1; i >= c0; i--)
                buf_row_delete_char(rw, i);
            E.buf.dirty++;
            buf_mark_hl_dirty(&E.buf, r0);
            E.cy = r0; E.cx = c0;
            if (E.cx > rw->len) E.cx = rw->len;
            if (op == 'c') {
                E.mode               = MODE_INSERT;
                E.insert_entry       = 'c';
                E.insert_motion      = (int)ia;
                E.insert_find_target = (int)obj;
                E.insert_count       = 1;
                insert_rec_reset();
            }
        } else {
            E.cy = r0; E.cx = c0;
        }
    } else {
        /* Multi-line. */
        yank_set_multiline_chars(r0, c0, r1, c1);
        if (op == 'd' || op == 'c') {
            if (op == 'c') {
                E.pre_insert_snapshot = editor_capture_state();
                E.pre_insert_dirty    = E.buf.dirty;
                E.has_pre_insert      = 1;
            } else {
                push_undo("delete");
            }
            int   tail_len = E.buf.rows[r1].len - c1;
            char *tail     = malloc(tail_len + 1);
            memcpy(tail, E.buf.rows[r1].chars + c1, tail_len);
            tail[tail_len] = '\0';
            E.buf.rows[r0].len       = c0;
            E.buf.rows[r0].chars[c0] = '\0';
            for (int i = 0; i < tail_len; i++)
                buf_row_insert_char(&E.buf.rows[r0], c0 + i, tail[i]);
            free(tail);
            for (int r = r1; r > r0; r--)
                buf_delete_row(&E.buf, r);
            E.buf.dirty++;
            buf_mark_hl_dirty(&E.buf, r0);
            E.cy = r0; E.cx = c0;
            if (E.cx > E.buf.rows[r0].len) E.cx = E.buf.rows[r0].len;
            if (op == 'c') {
                E.mode               = MODE_INSERT;
                E.insert_entry       = 'c';
                E.insert_motion      = (int)ia;
                E.insert_find_target = (int)obj;
                E.insert_count       = 1;
                insert_rec_reset();
            }
        } else {
            E.cy = r0; E.cx = c0;
        }
    }
    if (op == 'd' && !E.is_replaying) {
        la_free();
        E.last_action.type        = LA_DELETE;
        E.last_action.count       = 1;
        E.last_action.motion      = (int)ia;
        E.last_action.find_target = obj;
    }
    /* 'c' last_action is finalised in the insert-mode Escape handler. */
}

/* Apply operator op ('d', 'y', or 'c') with the given motion key, repeated n times. */
static void editor_apply_op(char op, int motion_key, int n) {
    if (E.buf.numrows == 0) return;

    /* Text objects: i<obj> or a<obj>. */
    if (motion_key == 'i' || motion_key == 'a') {
        char ia = (char)motion_key;
        char obj;
        if (E.is_replaying) {
            obj = E.last_action.find_target;
        } else {
            int nk = editor_read_key();
            if (nk == '\x1b') return;
            obj = (char)nk;
        }
        int r0, c0, r1, c1;
        if (!text_object_range(ia, obj, &r0, &c0, &r1, &c1)) return;
        apply_textobj_op(op, r0, c0, r1, c1, ia, obj);
        return;
    }

    /* Doubled operator: linewise on n lines starting at cursor (dd/yy/cc/>>/<< + count). */
    if (motion_key == (int)op) {
        int r0 = E.cy;
        int r1 = E.cy + n - 1;
        if (r1 >= E.buf.numrows) r1 = E.buf.numrows - 1;

        if (op == '>' || op == '<') {
            push_undo("indent");
            indent_lines(r0, r1, (char)op);
            E.cx = 0;
            if (!E.is_replaying) {
                la_free();
                E.last_action.type   = LA_INDENT;
                E.last_action.count  = n;
                E.last_action.motion = op;
            }
            return;
        }

        yank_set_lines(r0, r1);
        if (op == 'd') {
            push_undo("delete lines");
            for (int i = r1; i >= r0; i--)
                buf_delete_row(&E.buf, i);
            if (E.cy >= E.buf.numrows && E.cy > 0) E.cy--;
            E.cx = 0;
            if (!E.is_replaying) {
                la_free();
                E.last_action.type   = LA_DELETE;
                E.last_action.count  = n;
                E.last_action.motion = (int)op;  /* doubled: motion == op */
            }
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
            E.insert_entry  = 'c';
            E.insert_motion = (int)op;  /* doubled */
            E.insert_count  = n;
            insert_rec_reset();
        } else {  /* 'y' */
            int lines = r1 - r0 + 1;
            status_msg("%d line%s yanked", lines, lines != 1 ? "s" : "");
        }
        return;
    }

    /* Compute target position for the motion. */
    int tr = E.cy, tc = E.cx;

    /* f/F/t/T and ;/, are line-bound — handled before the word-motion loop. */
    if (motion_key == 'f' || motion_key == 'F' ||
        motion_key == 't' || motion_key == 'T' ||
        motion_key == ';' || motion_key == ',') {

        char fkey, ftarget;

        if (motion_key == ';' || motion_key == ',') {
            if (!E.last_find_key) return;
            fkey = (motion_key == ';') ? E.last_find_key
                 : (E.last_find_key == 'f') ? 'F'
                 : (E.last_find_key == 'F') ? 'f'
                 : (E.last_find_key == 't') ? 'T' : 't';
            ftarget = E.last_find_target;
            motion_key = (int)fkey;  /* normalise for last_action recording */
        } else {
            fkey = (char)motion_key;
            if (E.is_replaying) {
                ftarget = E.last_action.find_target;
            } else {
                int tk = editor_read_key();
                if (tk == '\x1b') return;
                ftarget = (char)tk;
                E.last_find_key    = fkey;
                E.last_find_target = ftarget;
            }
        }

        if (!pos_find_char(&tr, &tc, fkey, ftarget, n)) return;
        if (fkey == 'f' || fkey == 'F') tc++;  /* inclusive → exclusive */

        /* Store find target in last_action (set after the delete/yank below). */
        if (!E.is_replaying) E.last_action.find_target = ftarget;

    } else {
        /* Word motions: apply n times. */
        for (int i = 0; i < n; i++) {
            switch (motion_key) {
                case 'w': pos_word_next(&tr, &tc);  break;
                case 'e': pos_word_end(&tr, &tc);   break;
                case 'b': pos_word_start(&tr, &tc); break;
                case '0': tr = E.cy; tc = 0; i = n; break;
                case '$': tr = E.cy; tc = E.buf.rows[E.cy].len; i = n; break;
                case '_': case '^': {
                    int col = 0;
                    Row *row = &E.buf.rows[tr];
                    while (col < row->len && (row->chars[col] == ' ' || row->chars[col] == '\t'))
                        col++;
                    tc = col; i = n;
                    break;
                }
                case '{': {
                    int r = tr - 1;
                    while (r > 0 && E.buf.rows[r].len == 0) r--;
                    while (r > 0 && E.buf.rows[r].len > 0) r--;
                    tr = r; tc = 0;
                    break;
                }
                case '}': {
                    int last = E.buf.numrows > 0 ? E.buf.numrows - 1 : 0;
                    int r = tr + 1;
                    while (r < last && E.buf.rows[r].len == 0) r++;
                    while (r < last && E.buf.rows[r].len > 0) r++;
                    tr = r; tc = 0;
                    break;
                }
                default:  return;
            }
        }
        /* For 'e': convert inclusive end to exclusive (works for any row). */
        if (motion_key == 'e') {
            int rlen = (tr < E.buf.numrows) ? E.buf.rows[tr].len : 0;
            if (tc < rlen) tc++;
        }
    }

    /* ── Cross-line charwise operation ─────────────────────────────────── */
    if (tr != E.cy) {
        /* Ordered start/end. */
        int sr, sc, er, ec;
        if (tr > E.cy || (tr == E.cy && tc >= E.cx))
            { sr = E.cy; sc = E.cx; er = tr;   ec = tc;   }
        else
            { sr = tr;   sc = tc;   er = E.cy; ec = E.cx; }
        if (er >= E.buf.numrows) er = E.buf.numrows - 1;
        if (ec > E.buf.rows[er].len) ec = E.buf.rows[er].len;

        yank_set_multiline_chars(sr, sc, er, ec);

        if (op == 'd' || op == 'c') {
            if (op == 'c') {
                E.pre_insert_snapshot = editor_capture_state();
                E.pre_insert_dirty    = E.buf.dirty;
                E.has_pre_insert      = 1;
            } else {
                push_undo("delete");
            }
            /* Save tail of end row, then merge: row sr[0..sc) + row er[ec..end). */
            int   tail_len = E.buf.rows[er].len - ec;
            char *tail     = malloc(tail_len + 1);
            memcpy(tail, E.buf.rows[er].chars + ec, tail_len);
            tail[tail_len] = '\0';

            /* Truncate start row at sc. */
            E.buf.rows[sr].len      = sc;
            E.buf.rows[sr].chars[sc] = '\0';
            /* Append tail to start row. */
            for (int i = 0; i < tail_len; i++)
                buf_row_insert_char(&E.buf.rows[sr], sc + i, tail[i]);
            free(tail);

            /* Delete rows er down to sr+1. */
            for (int r = er; r > sr; r--)
                buf_delete_row(&E.buf, r);

            E.buf.dirty++;
            buf_mark_hl_dirty(&E.buf, sr);
            E.cy = sr;
            E.cx = sc;
            if (E.cx > E.buf.rows[sr].len) E.cx = E.buf.rows[sr].len;

            if (op == 'd' && !E.is_replaying) {
                la_free();
                E.last_action.type   = LA_DELETE;
                E.last_action.count  = n;
                E.last_action.motion = motion_key;
            } else if (op == 'c') {
                E.mode          = MODE_INSERT;
                E.insert_entry  = 'c';
                E.insert_motion = motion_key;
                E.insert_count  = n;
                insert_rec_reset();
            }
        }
        return;
    }

    /* ── Same-line charwise operation ───────────────────────────────────── */
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
            push_undo("delete");
        }
        Row *row = &E.buf.rows[E.cy];
        for (int i = c1 - 1; i >= c0; i--)
            buf_row_delete_char(row, i);
        E.buf.dirty++;
        buf_mark_hl_dirty(&E.buf, E.cy);
        E.cx = c0;
        if (E.cx > E.buf.rows[E.cy].len) E.cx = E.buf.rows[E.cy].len;
        if (op == 'd' && !E.is_replaying) {
            la_free();
            E.last_action.type   = LA_DELETE;
            E.last_action.count  = n;
            E.last_action.motion = motion_key;
        } else if (op == 'c') {
            E.mode          = MODE_INSERT;
            E.insert_entry  = 'c';
            E.insert_motion = motion_key;
            E.insert_count  = n;
            insert_rec_reset();
        }
    }
}

/* Paste from a register. before=1 pastes before cursor/line, 0 = after. */
static void editor_paste(int before) {
    int ri = reg_consume();
    if (ri == REG_CLIPBOARD) clipboard_paste(ri);
    if (!E.regs[ri].rows || E.regs[ri].numrows == 0) return;
    push_undo("paste");

    char **yr = E.regs[ri].rows;
    int    yn = E.regs[ri].numrows;

    if (E.regs[ri].linewise) {
        int at = before ? E.cy : E.cy + 1;
        for (int i = 0; i < yn; i++)
            buf_insert_row(&E.buf, at + i, yr[i], (int)strlen(yr[i]));
        E.cy = at;
        E.cx = 0;
    } else {
        if (E.buf.numrows == 0)
            buf_insert_row(&E.buf, 0, "", 0);
        Row *row = &E.buf.rows[E.cy];
        int  ins = before ? E.cx : (E.cx < row->len ? E.cx + 1 : row->len);

        if (yn == 1) {
            const char *text = yr[0];
            int         tlen = (int)strlen(text);
            for (int i = tlen - 1; i >= 0; i--)
                buf_row_insert_char(row, ins, text[i]);
            E.buf.dirty++;
            buf_mark_hl_dirty(&E.buf, E.cy);
            E.cx = ins + tlen - 1;
            if (E.cx < 0) E.cx = 0;
        } else {
            int   tail_len = row->len - ins;
            char *tail     = malloc(tail_len + 1);
            memcpy(tail, row->chars + ins, tail_len);
            tail[tail_len] = '\0';

            row->len        = ins;
            row->chars[ins] = '\0';
            const char *first = yr[0];
            int          flen = (int)strlen(first);
            for (int i = flen - 1; i >= 0; i--)
                buf_row_insert_char(&E.buf.rows[E.cy], ins, first[i]);

            for (int i = 1; i < yn - 1; i++)
                buf_insert_row(&E.buf, E.cy + i, yr[i], (int)strlen(yr[i]));

            const char *last = yr[yn - 1];
            int          llen = (int)strlen(last);
            int       new_row = E.cy + yn - 1;
            char    *combined = malloc(llen + tail_len + 1);
            memcpy(combined,        last, llen);
            memcpy(combined + llen, tail, tail_len);
            combined[llen + tail_len] = '\0';
            buf_insert_row(&E.buf, new_row, combined, llen + tail_len);
            free(combined);
            free(tail);

            E.buf.dirty++;
            buf_mark_hl_dirty(&E.buf, E.cy);
            E.cy = new_row;
            E.cx = llen > 0 ? llen - 1 : 0;
        }
    }
}

/* ── Visual mode ─────────────────────────────────────────────────────── */

static void editor_move_cursor(int key);  /* forward declaration */
static void enter_insert_mode(char entry); /* forward declaration */

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
        status_msg("%d line%s yanked", r1 - r0 + 1, r1 - r0 ? "s" : "");
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
        push_undo("delete lines");
        for (int i = r1; i >= r0; i--)
            buf_delete_row(&E.buf, i);
        E.cy = r0;
        if (E.cy >= E.buf.numrows && E.cy > 0) E.cy--;
        E.cx = 0;
    } else {
        int end = c1 + 1;
        if (end > E.buf.rows[r0].len) end = E.buf.rows[r0].len;
        yank_set_chars(r0, c0, end);
        push_undo("delete");
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

/* Block visual: get block bounds (rows r0..r1, columns lc..rc inclusive). */
static void visual_block_bounds(int *r0, int *r1, int *lc, int *rc) {
    int ar = E.visual_anchor_row, ac = E.visual_anchor_col;
    int cr = E.cy, cc = E.cx;
    *r0 = ar < cr ? ar : cr;
    *r1 = ar > cr ? ar : cr;
    *lc = ac < cc ? ac : cc;
    *rc = ac > cc ? ac : cc;
}

static void visual_block_delete(void) {
    int r0, r1, lc, rc;
    visual_block_bounds(&r0, &r1, &lc, &rc);
    push_undo("block delete");
    E.mode = MODE_NORMAL;
    for (int r = r0; r <= r1 && r < E.buf.numrows; r++) {
        Row *row = &E.buf.rows[r];
        int c0 = lc < row->len ? lc : row->len;
        int c1 = (rc + 1) < row->len ? (rc + 1) : row->len;
        for (int i = c1 - 1; i >= c0; i--)
            buf_row_delete_char(row, i);
        E.buf.dirty++;
        buf_mark_hl_dirty(&E.buf, r);
    }
    E.cy = r0; E.cx = lc;
    status_msg("%d lines block-deleted", r1 - r0 + 1);
}

static void visual_block_insert(int append) {
    int r0, r1, lc, rc;
    visual_block_bounds(&r0, &r1, &lc, &rc);
    int col = append ? rc + 1 : lc;

    /* Capture undo before any changes. */
    E.pre_insert_snapshot = editor_capture_state();
    E.pre_insert_dirty    = E.buf.dirty;
    E.has_pre_insert      = 1;

    /* Position cursor at the insert column of the first row. */
    E.cy = r0; E.cx = col;
    if (E.cy < E.buf.numrows && E.cx > E.buf.rows[E.cy].len)
        E.cx = E.buf.rows[E.cy].len;

    /* Save block info for applying to remaining rows on ESC. */
    E.block_insert_r0 = r0;
    E.block_insert_r1 = r1;
    E.block_insert_col = col;
    E.block_insert_active = 1;

    E.mode = MODE_INSERT;
    enter_insert_mode('i');
}

static void editor_process_visual(int c) {
    if (lua_bridge_call_key(E.mode, c)) return;

    /* Register prefix in visual mode. */
    if (c == '"') {
        int r = editor_read_key();
        if (r >= 'a' && r <= 'z')
            E.pending_reg = r - 'a' + 1;
        else if (r == '+')
            E.pending_reg = REG_CLIPBOARD;
        return;
    }

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

        case 0x16:  /* Ctrl-V: toggle block visual */
            E.mode = (E.mode == MODE_VISUAL_BLOCK) ? MODE_NORMAL : MODE_VISUAL_BLOCK;
            break;

        case 'd':
        case 'x':
            if (E.mode == MODE_VISUAL_BLOCK)
                visual_block_delete();
            else
                visual_op_delete();
            break;

        case 'y':
            visual_op_yank();
            break;

        case 'c':
            if (E.mode == MODE_VISUAL_BLOCK)
                visual_block_delete();   /* delete block, then fall into insert */
            else
                visual_op_change();
            break;

        case 'I':  /* Block insert at left edge. */
            if (E.mode == MODE_VISUAL_BLOCK)
                visual_block_insert(0);
            break;

        case 'A':  /* Block append at right edge. */
            if (E.mode == MODE_VISUAL_BLOCK)
                visual_block_insert(1);
            break;

        case '>':
        case '<': {
            int ar = E.visual_anchor_row, cr = E.cy;
            int r0 = ar < cr ? ar : cr;
            int r1 = ar > cr ? ar : cr;
            push_undo("indent");
            indent_lines(r0, r1, (char)c);
            E.cx = 0;
            E.mode = MODE_NORMAL;
            if (!E.is_replaying) {
                la_free();
                E.last_action.type   = LA_INDENT;
                E.last_action.count  = r1 - r0 + 1;
                E.last_action.motion = c;
            }
            break;
        }

        /* Cursor movement — same keys as normal mode. */
        case 'f': case 'F': case 't': case 'T': {
            int tk = editor_read_key();
            if (tk == '\x1b') break;
            char fkey = (char)c, ftarget = (char)tk;
            E.last_find_key    = fkey;
            E.last_find_target = ftarget;
            int r = E.cy, col = E.cx;
            if (pos_find_char(&r, &col, fkey, ftarget, 1))
                E.cx = col;
            break;
        }
        case ';': {
            if (!E.last_find_key) break;
            int r = E.cy, col = E.cx;
            if (pos_find_char(&r, &col, E.last_find_key, E.last_find_target, 1))
                E.cx = col;
            break;
        }
        case ',': {
            if (!E.last_find_key) break;
            char rev = (E.last_find_key == 'f') ? 'F'
                     : (E.last_find_key == 'F') ? 'f'
                     : (E.last_find_key == 't') ? 'T' : 't';
            int r = E.cy, col = E.cx;
            if (pos_find_char(&r, &col, rev, E.last_find_target, 1))
                E.cx = col;
            break;
        }
        case 'i':
        case 'a': {
            int nk = editor_read_key();
            if (nk == '\x1b') break;
            int r0, c0, r1, c1;
            if (!text_object_range((char)c, (char)nk, &r0, &c0, &r1, &c1)) break;
            if (E.mode == MODE_VISUAL_LINE || E.mode == MODE_VISUAL_BLOCK) E.mode = MODE_VISUAL;
            E.visual_anchor_row = r0;
            E.visual_anchor_col = c0;
            E.cy = r1;
            E.cx = c1 > 0 ? c1 - 1 : 0;
            break;
        }
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

/* Set by vertical-move operations; suppresses preferred_col update at end of
   each keypress so the user's intended column is remembered across short lines. */
static int s_vertical_move = 0;

/* Restore cx from preferred_col (visual column) on the current row.
   Finds the byte offset whose visual column is closest to preferred_col. */
static void apply_preferred_col(void) {
    if (E.buf.numrows == 0 || E.cy >= E.buf.numrows) { E.cx = 0; return; }
    Row *row = &E.buf.rows[E.cy];
    int col = vcol_to_col(row, E.preferred_col, E.opts.tabwidth);
    /* Clamp to last character (not past the newline). */
    int maxcol = row->len > 0 ? row->len - 1 : 0;
    E.cx = col <= maxcol ? col : maxcol;
}

static void editor_move_cursor(int key) {
    int rowlen = (E.cy < E.buf.numrows) ? E.buf.rows[E.cy].len : 0;

    switch (key) {
        case ARROW_LEFT:
        case 'h':
            if (E.cx > 0) E.cx--;
            break;
        case ARROW_RIGHT:
        case 'l':
            if (rowlen > 0 && E.cx < rowlen - 1) E.cx++;
            break;
        case ARROW_UP:
        case 'k':
            if (E.cy > 0) {
                E.cy--;
                /* Skip over folded rows (land on the fold header). */
                if (fold_row_hidden(E.cy)) {
                    int p = fold_parent(E.cy);
                    if (p >= 0) E.cy = p;
                }
            }
            s_vertical_move = 1;
            break;
        case ARROW_DOWN:
        case 'j':
            if (E.cy < E.buf.numrows - 1) {
                /* If on a closed fold header, skip past the fold. */
                if (E.buf.folds && E.cy < E.buf.folds_cap && E.buf.folds[E.cy]) {
                    int end = buf_fold_end(&E.buf, E.cy, E.opts.tabwidth);
                    E.cy = (end + 1 < E.buf.numrows) ? end + 1 : E.cy;
                } else {
                    E.cy++;
                    fold_skip_forward();
                }
            }
            s_vertical_move = 1;
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = rowlen > 0 ? rowlen - 1 : 0;
            break;
        case PAGE_UP:
            E.cy -= E.screenrows;
            if (E.cy < 0) E.cy = 0;
            s_vertical_move = 1;
            break;
        case PAGE_DOWN:
            E.cy += E.screenrows;
            if (E.buf.numrows > 0 && E.cy >= E.buf.numrows)
                E.cy = E.buf.numrows - 1;
            else if (E.buf.numrows == 0)
                E.cy = 0;
            s_vertical_move = 1;
            break;
    }

    rowlen = (E.cy < E.buf.numrows) ? E.buf.rows[E.cy].len : 0;
    if (s_vertical_move) {
        apply_preferred_col();
    } else {
        /* Clamp to last character (len-1), not past the newline. */
        int maxcol = rowlen > 0 ? rowlen - 1 : 0;
        if (E.cx > maxcol) E.cx = maxcol;
    }
}

/* ── Pane helpers ────────────────────────────────────────────────────── */

static void pane_save_cursor(void) {
    Pane *p  = &E.panes[E.cur_pane];
    p->cx    = E.cx;     p->cy    = E.cy;
    p->rowoff = E.rowoff; p->coloff = E.coloff;
}

static void pane_activate(int idx) {
    pane_save_cursor();
    /* Track the last content pane we're leaving. */
    if (!buftab_is_special(&E.buftabs[E.panes[E.cur_pane].buf_idx]))
        E.last_content_pane = E.cur_pane;
    int old_buf = E.panes[E.cur_pane].buf_idx;
    int new_buf = E.panes[idx].buf_idx;
    if (old_buf != new_buf) editor_buf_save(old_buf);
    E.cur_pane   = idx;
    Pane *p      = &E.panes[idx];
    E.screenrows = p->height;
    E.screencols = p->width;
    E.cx = p->cx; E.cy = p->cy;
    E.rowoff = p->rowoff; E.coloff = p->coloff;
    E.cur_buftab = new_buf;
    if (old_buf != new_buf) editor_buf_restore(new_buf);
    E.mode = E.buftabs[new_buf].kind == BT_TERM ? MODE_INSERT : MODE_NORMAL;
    E.match_bracket_valid = 0;
}

static int pane_split_h(int idx) {
    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return 0; }
    Pane *p = &E.panes[idx];
    int H = p->height;
    if (H < 3) { status_err("Pane too small to split"); return 0; }
    int h_a = (H - 1) / 2, h_b = H - 1 - h_a;
    for (int i = E.num_panes; i > idx + 1; i--) E.panes[i] = E.panes[i-1];
    E.num_panes++;
    p->height = h_a;
    Pane *np  = &E.panes[idx + 1];
    *np       = *p;
    np->top   = p->top + h_a + 1;  /* skip status bar row */
    np->height = h_b;
    return 1;
}

static int pane_split_v(int idx) {
    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return 0; }
    Pane *p = &E.panes[idx];
    if (p->width < 11) { status_err("Pane too narrow to split"); return 0; }
    int w_a = p->width / 2, w_b = p->width - w_a - 1;  /* -1 for divider col */
    for (int i = E.num_panes; i > idx + 1; i--) E.panes[i] = E.panes[i-1];
    E.num_panes++;
    p->width  = w_a;
    Pane *np  = &E.panes[idx + 1];
    *np       = *p;
    np->left  = p->left + w_a + 1;  /* skip divider column */
    np->width = w_b;
    return 1;
}

static void pane_close(int idx) {
    if (E.num_panes == 1) { status_err("Only one window"); return; }

    /* Save cursor of the currently active pane before any changes. */
    pane_save_cursor();

    /* Track last content pane when leaving one. */
    if (!buftab_is_special(&E.buftabs[E.panes[E.cur_pane].buf_idx]))
        E.last_content_pane = E.cur_pane;

    Pane *p = &E.panes[idx];
    int donor = -1;
    /* Horizontal neighbors (same left+width). */
    for (int i = 0; i < E.num_panes && donor == -1; i++) {
        if (i == idx) continue;
        Pane *q = &E.panes[i];
        if (q->left == p->left && q->width == p->width) {
            if (q->top + q->height + 1 == p->top)
                { q->height += p->height + 1; donor = i; }
            else if (p->top + p->height + 1 == q->top)
                { q->top = p->top; q->height += p->height + 1; donor = i; }
        }
    }
    /* Vertical neighbors (same top+height). */
    for (int i = 0; i < E.num_panes && donor == -1; i++) {
        if (i == idx) continue;
        Pane *q = &E.panes[i];
        if (q->top == p->top && q->height == p->height) {
            if (q->left + q->width + 1 == p->left)          /* +1 = divider col */
                { q->width += p->width + 1; donor = i; }
            else if (p->left + p->width + 1 == q->left)
                { q->left = p->left; q->width += p->width + 1; donor = i; }
        }
    }
    if (donor == -1) { status_err("Cannot close pane"); return; }

    for (int i = idx; i < E.num_panes - 1; i++) E.panes[i] = E.panes[i+1];
    E.num_panes--;
    if (donor > idx) donor--;

    /* Switch to the donor pane.  Use E.cur_buftab (what is actually live in
       E.buf) to decide whether a save+restore is needed — NOT E.panes[].buf_idx,
       which may have been pre-set by the caller before we got here. */
    Pane *dp      = &E.panes[donor];
    int donor_buf = dp->buf_idx;
    if (E.cur_buftab != donor_buf) {
        editor_buf_save(E.cur_buftab);
        editor_buf_restore(donor_buf);
        E.cur_buftab = donor_buf;
    }
    E.cur_pane   = donor;
    E.screenrows = dp->height;
    E.screencols = dp->width;
    E.cx = dp->cx; E.cy = dp->cy;
    E.rowoff = dp->rowoff; E.coloff = dp->coloff;
    E.mode = E.buftabs[donor_buf].kind == BT_TERM ? MODE_INSERT : MODE_NORMAL;
    E.match_bracket_valid = 0;
}

static void pane_navigate(int dir) {
    Pane *cur = &E.panes[E.cur_pane];
    int best = -1, best_dist = INT_MAX;
    for (int i = 0; i < E.num_panes; i++) {
        if (i == E.cur_pane) continue;
        Pane *p = &E.panes[i];
        int ok = 0;
        if      (dir == 'h') ok = (p->left + p->width  <= cur->left);
        else if (dir == 'l') ok = (p->left >= cur->left + cur->width);
        else if (dir == 'k') ok = (p->top  + p->height <= cur->top);
        else if (dir == 'j') ok = (p->top  >= cur->top  + cur->height);
        if (!ok) continue;
        int d = abs((p->top  + p->height / 2) - (cur->top  + cur->height / 2))
              + abs((p->left + p->width  / 2) - (cur->left + cur->width  / 2));
        if (d < best_dist) { best_dist = d; best = i; }
    }
    if (best == -1) status_err("No window in that direction");
    else            pane_activate(best);
}

/* ── Tree helpers ────────────────────────────────────────────────────── */

/* Forward declarations for helpers used by tree. */
static void completion_free(void);
static int  open_new_buf(const char *filename);

static void tree_activate_entry(void) {
    if (E.cy == 0) return;  /* root line — do nothing */
    TreeState *ts = E.buftabs[E.cur_buftab].tree;
    int entry_idx = E.cy - 1;  /* buf row 0 = root, row N+1 = entry N */
    if (!ts || entry_idx < 0 || entry_idx >= ts->count) return;
    TreeEntry *e = &ts->entries[entry_idx];

    if (e->is_dir) {
        tree_toggle(ts, entry_idx);
        tree_render_to_buf(ts, &E.buf);
        /* Clamp cursor in case rows were removed. */
        if (E.cy >= E.buf.numrows) E.cy = E.buf.numrows - 1;
        if (E.cy < 0) E.cy = 0;
    } else {
        /* Find a content pane to open the file in. */
        int cpane = E.last_content_pane;
        if (cpane >= E.num_panes || buftab_is_special(&E.buftabs[E.panes[cpane].buf_idx])) {
            cpane = -1;
            for (int i = 0; i < E.num_panes; i++) {
                if (!buftab_is_special(&E.buftabs[E.panes[i].buf_idx])) { cpane = i; break; }
            }
        }
        if (cpane < 0) { status_err("No content pane"); return; }

        /* Activate the content pane, then open the file. */
        pane_activate(cpane);
        /* last_content_pane now set by pane_activate leaving tree pane */

        char *fname = strdup(e->path);
        /* Check if another pane also shows this buffer. */
        int buf_shared = 0;
        for (int i = 0; i < E.num_panes; i++) {
            if (i != E.cur_pane && E.panes[i].buf_idx == E.cur_buftab)
                { buf_shared = 1; break; }
        }
        if (buf_shared) {
            if (open_new_buf(fname))
                E.panes[E.cur_pane].buf_idx = E.cur_buftab;
        } else {
            buf_free(&E.buf);
            undo_tree_free(&E.undo_tree);
            if (E.has_pre_insert) {
                undo_state_free(&E.pre_insert_snapshot);
                E.has_pre_insert = 0;
            }
            la_free();
            E.cx = 0; E.cy = 0; E.rowoff = 0; E.coloff = 0;
            buf_open(&E.buf, fname);
            editor_detect_syntax();
            editor_update_git_signs();
            filewatcher_add(E.cur_buftab);
            status_msg("\"%s\"", E.buf.filename ? E.buf.filename : fname);
        }
        free(fname);
    }
}

static void tree_handle_key(int c) {
    TreeState *ts = E.buftabs[E.cur_buftab].tree;

    switch (c) {
        case ARROW_UP:
        case 'k':
            if (E.cy > 0) E.cy--;
            break;
        case ARROW_DOWN:
        case 'j':
            if (E.cy < E.buf.numrows - 1) E.cy++;
            break;
        case '\r':
            tree_activate_entry();
            break;
        case 'I':   /* toggle hidden files */
            if (ts) {
                ts->show_hidden ^= 1;
                tree_refresh(ts);
                tree_update_git_status(ts);
                tree_render_to_buf(ts, &E.buf);
                if (E.cy >= E.buf.numrows) E.cy = E.buf.numrows - 1;
            }
            break;
        case 'r':   /* refresh from disk */
            if (ts) {
                tree_refresh(ts);
                tree_update_git_status(ts);
                tree_render_to_buf(ts, &E.buf);
                if (E.cy >= E.buf.numrows) E.cy = E.buf.numrows - 1;
            }
            break;
        case 'a':   /* git add file/dir under cursor */
            if (ts) {
                int eidx = E.cy - 1;  /* row 0 = root, row i+1 = entries[i] */
                if (eidx >= 0 && eidx < ts->count) {
                    if (git_add(ts->entries[eidx].path)) {
                        status_msg("Staged: %s", ts->entries[eidx].name);
                        tree_update_git_status(ts);
                        tree_render_to_buf(ts, &E.buf);
                    } else {
                        status_err("git add failed");
                    }
                }
            }
            break;
        case 'u':   /* git reset (unstage) file/dir under cursor */
            if (ts) {
                int eidx = E.cy - 1;
                if (eidx >= 0 && eidx < ts->count) {
                    if (git_reset(ts->entries[eidx].path)) {
                        status_msg("Unstaged: %s", ts->entries[eidx].name);
                        tree_update_git_status(ts);
                        tree_render_to_buf(ts, &E.buf);
                    } else {
                        status_err("git reset failed");
                    }
                }
            }
            break;
        case ':':
            E.mode = MODE_COMMAND;
            E.cmdbuf[0] = '\0';
            E.cmdlen    = 0;
            break;
        case 'q':
        case '\x1b':
            editor_open_tree();   /* toggle closed */
            break;
        default:
            break;   /* silently ignore all other keys */
    }
}

/* Open the file-tree sidebar (or close it if already open). */
static void editor_open_tree_pane(void) {
    /* Check if a tree pane is already open — toggle it closed. */
    int tree_pane = -1;
    for (int i = 0; i < E.num_panes; i++) {
        if (E.buftabs[E.panes[i].buf_idx].kind == BT_TREE) { tree_pane = i; break; }
    }
    if (tree_pane >= 0) {
        Pane *tp = &E.panes[tree_pane];
        int tree_right = tp->left + tp->width + 1; /* first col of content panes */

        /* Find the content pane to activate after removal. */
        int cpane = E.last_content_pane;
        if (cpane >= E.num_panes || cpane == tree_pane ||
            buftab_is_special(&E.buftabs[E.panes[cpane].buf_idx])) {
            cpane = -1;
            for (int i = 0; i < E.num_panes; i++) {
                if (i != tree_pane && !buftab_is_special(&E.buftabs[E.panes[i].buf_idx]))
                    { cpane = i; break; }
            }
        }

        /* Expand every pane immediately right of the tree to reclaim its width. */
        for (int i = 0; i < E.num_panes; i++) {
            if (i == tree_pane) continue;
            if (E.panes[i].left == tree_right) {
                E.panes[i].left  = tp->left;
                E.panes[i].width += tp->width + 1;
            }
        }

        /* Remove tree pane from array. */
        for (int i = tree_pane; i < E.num_panes - 1; i++)
            E.panes[i] = E.panes[i + 1];
        E.num_panes--;
        if (cpane > tree_pane) cpane--;

        /* Switch to the content pane. */
        if (cpane >= 0) {
            int donor_buf = E.panes[cpane].buf_idx;
            if (E.cur_buftab != donor_buf) {
                editor_buf_save(E.cur_buftab);
                editor_buf_restore(donor_buf);
                E.cur_buftab = donor_buf;
            }
            E.cur_pane   = cpane;
            E.screenrows = E.panes[cpane].height;
            E.screencols = E.panes[cpane].width;
            E.cx = E.panes[cpane].cx; E.cy = E.panes[cpane].cy;
            E.rowoff = E.panes[cpane].rowoff; E.coloff = E.panes[cpane].coloff;
        }
        E.mode = MODE_NORMAL;
        E.match_bracket_valid = 0;
        return;
    }

    /* Not open: create tree pane on the far left, spanning the full height. */
    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return; }
    if (E.num_buftabs >= MAX_BUFS) { status_err("Too many buffers"); return; }

    /* Verify every pane in the left zone will still have enough width. */
    for (int i = 0; i < E.num_panes; i++) {
        if (E.panes[i].left < TREE_WIDTH + 2) {
            int new_width = E.panes[i].width - (TREE_WIDTH + 2 - E.panes[i].left);
            if (new_width < 5) { status_err("Pane too narrow for tree"); return; }
        }
    }

    int cp_idx = E.cur_pane;
    int cp_buf = E.panes[cp_idx].buf_idx;
    E.last_content_pane = cp_idx;

    /* Allocate a new buftab for the tree buffer. */
    int tidx = E.num_buftabs++;
    memset(&E.buftabs[tidx], 0, sizeof(BufTab));
    E.buftabs[tidx].kind = BT_TREE;
    E.buftabs[tidx].tree    = malloc(sizeof(TreeState));
    if (!E.buftabs[tidx].tree) { E.num_buftabs--; status_err("Out of memory"); return; }
    memset(E.buftabs[tidx].tree, 0, sizeof(TreeState));

    TreeState *ts = E.buftabs[tidx].tree;
    if (getcwd(ts->root, TREE_PATH_MAX) == NULL)
        strncpy(ts->root, ".", TREE_PATH_MAX - 1);
    ts->show_hidden = 0;

    /* Populate the parked tree buffer (before it becomes live). */
    buf_init(&E.buftabs[tidx].buf);
    E.buftabs[tidx].buf.filename = strdup(ts->root);
    tree_refresh(ts);
    tree_update_git_status(ts);
    tree_render_to_buf(ts, &E.buftabs[tidx].buf);

    /* Save the current live content buffer. */
    pane_save_cursor();
    editor_buf_save(cp_buf);
    la_free();
    insert_rec_reset();
    completion_free();
    E.pending_op = '\0';
    E.count      = 0;

    /* Push all panes that overlap the tree zone to the right of it. */
    for (int i = 0; i < E.num_panes; i++) {
        if (E.panes[i].left < TREE_WIDTH + 2) {
            int shift = TREE_WIDTH + 2 - E.panes[i].left;
            E.panes[i].width -= shift;
            E.panes[i].left   = TREE_WIDTH + 2;
        }
    }

    /* Insert tree pane at index 0 (all others shift up by one). */
    for (int i = E.num_panes; i > 0; i--) E.panes[i] = E.panes[i-1];
    E.num_panes++;
    E.last_content_pane = cp_idx + 1;  /* shifted by insertion */

    /* Tree pane: far left, full terminal height. */
    E.panes[0].top     = 1;
    E.panes[0].left    = 1;
    E.panes[0].height  = E.term_rows - 2;
    E.panes[0].width   = TREE_WIDTH;
    E.panes[0].buf_idx = tidx;
    E.panes[0].cx      = 0;
    E.panes[0].cy      = 0;
    E.panes[0].rowoff  = 0;
    E.panes[0].coloff  = 0;

    /* Activate tree pane (buffer managed manually above). */
    E.cur_pane   = 0;
    E.cur_buftab = tidx;
    E.screenrows = E.term_rows - 2;
    E.screencols = TREE_WIDTH;
    E.cx = 0; E.cy = 0; E.rowoff = 0; E.coloff = 0;
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;

    /* Load the tree buffer into live state. */
    editor_buf_restore(tidx);
}

void editor_open_tree(void) {
    editor_open_tree_pane();
}

void editor_open_fuzzy(void) {
    fuzzy_open();
}

/* ── Blame pane ──────────────────────────────────────────────────────── */

#define BLAME_WIDTH 40

static int blame_pane_idx(void) {
    for (int i = 0; i < E.num_panes; i++)
        if (E.buftabs[E.panes[i].buf_idx].kind == BT_BLAME) return i;
    return -1;
}

static void blame_close(void) {
    int bpi = blame_pane_idx();
    if (bpi < 0) return;

    Pane *bp = &E.panes[bpi];
    int buf_idx = bp->buf_idx;
    int blame_left  = bp->left;
    int blame_width = bp->width;

    /* Find content pane to the right and expand it. */
    int cpane = -1;
    for (int i = 0; i < E.num_panes; i++) {
        if (i != bpi && E.panes[i].left == blame_left + blame_width + 1) {
            cpane = i; break;
        }
    }
    if (cpane >= 0) {
        E.panes[cpane].left   = blame_left;
        E.panes[cpane].width += blame_width + 1;
    }

    /* Free blame buffer. */
    if (E.cur_pane == bpi) {
        for (int i = 0; i < E.buf.numrows; i++) {
            free(E.buf.rows[i].chars);
            free(E.buf.rows[i].hl);
        }
        free(E.buf.rows);
        memset(&E.buf, 0, sizeof(Buffer));
        E.buf.hl_dirty_from = INT_MAX;
    } else {
        pane_save_cursor();
        buf_free(&E.buftabs[buf_idx].buf);
    }
    memset(&E.buftabs[buf_idx], 0, sizeof(BufTab));

    /* Remove blame pane. */
    for (int i = bpi; i < E.num_panes - 1; i++) E.panes[i] = E.panes[i + 1];
    E.num_panes--;
    if (cpane > bpi) cpane--;

    /* Activate the content pane. */
    if (cpane >= 0) {
        int donor_buf = E.panes[cpane].buf_idx;
        if (E.cur_buftab != donor_buf) {
            editor_buf_restore(donor_buf);
            E.cur_buftab = donor_buf;
        }
        E.cur_pane   = cpane;
        E.screenrows = E.panes[cpane].height;
        E.screencols = E.panes[cpane].width;
        E.cx     = E.panes[cpane].cx;
        E.cy     = E.panes[cpane].cy;
        E.rowoff = E.panes[cpane].rowoff;
        E.coloff = E.panes[cpane].coloff;
    }
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;
}

static void blame_open(void) {
    /* Toggle off if already open. */
    if (blame_pane_idx() >= 0) { blame_close(); return; }

    if (!E.buf.filename) { status_err("No filename"); return; }
    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return; }
    if (E.num_buftabs >= MAX_BUFS) { status_err("Too many buffers"); return; }

    /* Run git blame. */
    int blame_count = 0;
    char **blame_lines = git_blame(E.buf.filename, &blame_count);
    if (!blame_lines || blame_count == 0) {
        status_err("git blame failed (file committed?)");
        return;
    }

    /* Determine blame pane width (capped to available space). */
    int bw = BLAME_WIDTH;
    Pane *sp = &E.panes[E.cur_pane];
    if (sp->width < bw + 15) { /* need room for source pane */
        for (int i = 0; i < blame_count; i++) free(blame_lines[i]);
        free(blame_lines);
        status_err("Pane too narrow for blame");
        return;
    }

    /* Allocate blame buftab. */
    int bidx = E.num_buftabs++;
    memset(&E.buftabs[bidx], 0, sizeof(BufTab));
    E.buftabs[bidx].kind = BT_BLAME;
    E.buftabs[bidx].blame_source_buf = E.cur_buftab;
    buf_init(&E.buftabs[bidx].buf);

    /* Populate blame buffer rows. */
    for (int i = 0; i < blame_count; i++) {
        int len = (int)strlen(blame_lines[i]);
        buf_insert_row(&E.buftabs[bidx].buf, i, blame_lines[i], len);
        free(blame_lines[i]);
    }
    free(blame_lines);
    E.buftabs[bidx].buf.dirty = 0;

    /* Shrink source pane and insert blame pane to its left. */
    int src_idx = E.cur_pane;
    pane_save_cursor();
    editor_buf_save(E.cur_buftab);

    sp = &E.panes[src_idx];  /* re-fetch after potential pane_save_cursor */
    int old_left = sp->left;
    sp->left  = old_left + bw + 1;
    sp->width -= (bw + 1);

    /* Insert blame pane before the source pane. */
    for (int i = E.num_panes; i > src_idx; i--) E.panes[i] = E.panes[i - 1];
    E.num_panes++;
    src_idx++;  /* source pane shifted right */

    Pane *bp = &E.panes[src_idx - 1];
    bp->top     = E.panes[src_idx].top;
    bp->left    = old_left;
    bp->height  = E.panes[src_idx].height;
    bp->width   = bw;
    bp->buf_idx = bidx;
    bp->cx      = 0;
    bp->cy      = E.cy;       /* start at same line as source */
    bp->rowoff  = E.rowoff;
    bp->coloff  = 0;

    /* Keep focus on source pane. */
    E.cur_pane   = src_idx;
    E.screenrows = E.panes[src_idx].height;
    E.screencols = E.panes[src_idx].width;
    E.cur_buftab = E.panes[src_idx].buf_idx;
    editor_buf_restore(E.cur_buftab);
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;

    /* Update last_content_pane since indices shifted. */
    if (E.last_content_pane >= src_idx - 1) E.last_content_pane++;
}

static void blame_handle_key(int c) {
    if (c == 'q' || c == '\x1b') { blame_close(); return; }

    int bpi = blame_pane_idx();
    if (bpi < 0) return;
    int src_buf = E.buftabs[E.panes[bpi].buf_idx].blame_source_buf;

    if ((c == ARROW_DOWN || c == 'j') && E.cy < E.buf.numrows - 1) {
        E.cy++;
        if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    } else if ((c == ARROW_UP || c == 'k') && E.cy > 0) {
        E.cy--;
        if (E.cy < E.rowoff) E.rowoff = E.cy;
    }

    /* Sync scroll to source pane. */
    for (int i = 0; i < E.num_panes; i++) {
        if (E.panes[i].buf_idx == src_buf) {
            E.panes[i].cy     = E.cy;
            E.panes[i].rowoff = E.rowoff;
            break;
        }
    }
}

/* ── Git log pane ────────────────────────────────────────────────────── */

static int log_pane_idx(void) {
    for (int i = 0; i < E.num_panes; i++)
        if (E.buftabs[E.panes[i].buf_idx].kind == BT_LOG) return i;
    return -1;
}

static void log_close(void) {
    int lpi = log_pane_idx();
    if (lpi < 0) return;

    Pane *lp = &E.panes[lpi];
    int buf_idx = lp->buf_idx;

    /* Expand the pane above. */
    for (int i = 0; i < E.num_panes; i++) {
        if (i == lpi) continue;
        Pane *q = &E.panes[i];
        if (q->left == lp->left && q->width == lp->width &&
            q->top + q->height + 1 == lp->top) {
            q->height += lp->height + 1;
            break;
        }
    }

    /* Free log data. */
    free(E.buftabs[buf_idx].log_entries);
    if (E.cur_pane == lpi) {
        for (int i = 0; i < E.buf.numrows; i++) {
            free(E.buf.rows[i].chars);
            free(E.buf.rows[i].hl);
        }
        free(E.buf.rows);
        free(E.buf.git_signs);
        memset(&E.buf, 0, sizeof(Buffer));
        E.buf.hl_dirty_from = INT_MAX;
    } else {
        buf_free(&E.buftabs[buf_idx].buf);
    }
    memset(&E.buftabs[buf_idx], 0, sizeof(BufTab));

    /* Remove pane. */
    for (int i = lpi; i < E.num_panes - 1; i++) E.panes[i] = E.panes[i + 1];
    E.num_panes--;

    /* Activate last content pane. */
    int cpane = E.last_content_pane;
    if (cpane >= E.num_panes || cpane < 0) cpane = 0;
    if (E.cur_pane == lpi || E.cur_pane >= E.num_panes) {
        int donor_buf = E.panes[cpane].buf_idx;
        if (E.cur_buftab != donor_buf) {
            editor_buf_restore(donor_buf);
            E.cur_buftab = donor_buf;
        }
        E.cur_pane   = cpane;
        E.screenrows = E.panes[cpane].height;
        E.screencols = E.panes[cpane].width;
        E.cx     = E.panes[cpane].cx;
        E.cy     = E.panes[cpane].cy;
        E.rowoff = E.panes[cpane].rowoff;
        E.coloff = E.panes[cpane].coloff;
    }
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;
}

static void log_open(void) {
    if (log_pane_idx() >= 0) { log_close(); return; }

    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return; }
    if (E.num_buftabs >= MAX_BUFS) { status_err("Too many buffers"); return; }

    int log_count = 0;
    GitLogEntry *entries = git_log(200, &log_count);
    if (!entries || log_count == 0) {
        status_err("git log failed (any commits?)");
        return;
    }

    /* Allocate log buftab. */
    int lidx = E.num_buftabs++;
    memset(&E.buftabs[lidx], 0, sizeof(BufTab));
    E.buftabs[lidx].kind = BT_LOG;
    E.buftabs[lidx].log_entries = entries;
    E.buftabs[lidx].log_count   = log_count;
    buf_init(&E.buftabs[lidx].buf);

    /* Populate buffer rows (one per commit, for scrolling). */
    for (int i = 0; i < log_count; i++) {
        GitLogEntry *e = &entries[i];
        char line[256];
        int len = snprintf(line, sizeof(line), "%s %s %-12.12s %s",
                           e->hash, e->date, e->author, e->subject);
        buf_insert_row(&E.buftabs[lidx].buf, i, line, len);
    }
    E.buftabs[lidx].buf.dirty = 0;

    /* Create bottom pane (same pattern as quickfix). */
    int height = E.opts.qf_height_rows;
    if (height < 3) height = 3;

    pane_save_cursor();
    editor_buf_save(E.cur_buftab);

    Pane *sp = &E.panes[E.cur_pane];
    if (sp->height < height + 3) {
        /* Not enough room — use half. */
        height = sp->height / 2;
        if (height < 3) height = 3;
    }
    sp->height -= (height + 1);

    /* Insert log pane after current. */
    int lpi = E.cur_pane + 1;
    for (int i = E.num_panes; i > lpi; i--) E.panes[i] = E.panes[i - 1];
    E.num_panes++;

    Pane *lp = &E.panes[lpi];
    lp->top    = sp->top + sp->height + 1;
    lp->left   = sp->left;
    lp->height = height;
    lp->width  = sp->width;
    lp->buf_idx = lidx;
    lp->cx = 0; lp->cy = 0;
    lp->rowoff = 0; lp->coloff = 0;

    /* Activate the log pane. */
    E.panes[E.cur_pane].buf_idx = E.cur_buftab;  /* ensure source pane slot is correct */
    pane_activate(lpi);
}

/* Open a scratch buffer showing `git show <hash>`. */
static void log_show_commit(const char *hash) {
    if (!hash || !*hash) return;

    int line_count = 0;
    char **lines = git_show_commit(hash, &line_count);
    if (!lines || line_count == 0) {
        status_err("git show failed");
        return;
    }

    /* Close log pane first, then open the commit in the main pane. */
    log_close();

    if (E.num_buftabs >= MAX_BUFS) {
        for (int i = 0; i < line_count; i++) free(lines[i]);
        free(lines);
        status_err("Too many buffers");
        return;
    }

    /* Create a new buffer for the commit. */
    int cidx = E.num_buftabs++;
    memset(&E.buftabs[cidx], 0, sizeof(BufTab));
    buf_init(&E.buftabs[cidx].buf);

    for (int i = 0; i < line_count; i++) {
        int len = (int)strlen(lines[i]);
        buf_insert_row(&E.buftabs[cidx].buf, i, lines[i], len);
        free(lines[i]);
    }
    free(lines);
    E.buftabs[cidx].buf.dirty = 0;
    E.buftabs[cidx].kind = BT_SHOW;

    /* Build git_signs from diff line prefixes for background highlighting. */
    Buffer *cb = &E.buftabs[cidx].buf;
    cb->git_signs = calloc(cb->numrows, 1);
    cb->git_signs_count = cb->numrows;
    int in_diff = 0;
    for (int i = 0; i < cb->numrows; i++) {
        const char *ln = cb->rows[i].chars;
        int len = cb->rows[i].len;
        if (len >= 4 && strncmp(ln, "diff ", 5) == 0) in_diff = 1;
        if (!in_diff) continue;
        if (len > 0 && ln[0] == '+' && !(len >= 3 && ln[1] == '+' && ln[2] == '+'))
            cb->git_signs[i] = GIT_SIGN_ADD;
        else if (len > 0 && ln[0] == '-' && !(len >= 3 && ln[1] == '-' && ln[2] == '-'))
            cb->git_signs[i] = GIT_SIGN_DEL;
        else if (len >= 2 && ln[0] == '@' && ln[1] == '@')
            cb->git_signs[i] = GIT_SIGN_MOD;
    }

    /* Set a descriptive filename. */
    char name[64];
    snprintf(name, sizeof(name), "[commit %s]", hash);
    E.buftabs[cidx].buf.filename = strdup(name);

    /* Switch to it. */
    pane_save_cursor();
    editor_buf_save(E.cur_buftab);
    E.panes[E.cur_pane].buf_idx = cidx;
    E.cur_buftab = cidx;
    editor_buf_restore(cidx);
    E.syntax = NULL;
    E.mode = MODE_NORMAL;
    E.cy = 0; E.cx = 0;
    E.rowoff = 0; E.coloff = 0;
}

static void log_handle_key(int c) {
    if (c == 'q' || c == '\x1b') { log_close(); return; }

    BufTab *bt = &E.buftabs[E.cur_buftab];

    if ((c == ARROW_DOWN || c == 'j') && E.cy < E.buf.numrows - 1) {
        E.cy++;
        if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    } else if ((c == ARROW_UP || c == 'k') && E.cy > 0) {
        E.cy--;
        if (E.cy < E.rowoff) E.rowoff = E.cy;
    } else if (c == 'G') {
        E.cy = E.buf.numrows - 1;
    } else if (c == 'g') {
        E.cy = 0; E.rowoff = 0;
    } else if (c == '\r') {
        /* Enter: show the selected commit. */
        if (E.cy >= 0 && E.cy < bt->log_count) {
            char hash[12];
            snprintf(hash, sizeof(hash), "%s", bt->log_entries[E.cy].hash);
            log_show_commit(hash);
        }
    }
}

/* ── Git commit buffer ───────────────────────────────────────────────── */

static void commit_open(void) {
    /* Check for staged changes. */
    char *summary = git_staged_summary();
    if (!summary) { status_err("Nothing staged to commit"); return; }

    if (E.num_buftabs >= MAX_BUFS) { free(summary); status_err("Too many buffers"); return; }

    /* Allocate commit buftab. */
    int cidx = E.num_buftabs++;
    memset(&E.buftabs[cidx], 0, sizeof(BufTab));
    E.buftabs[cidx].kind = BT_COMMIT;
    buf_init(&E.buftabs[cidx].buf);

    /* Populate with template. */
    Buffer *cb = &E.buftabs[cidx].buf;
    buf_insert_row(cb, 0, "", 0);  /* blank line for message */
    const char *sep = "# --- Staged changes ---";
    buf_insert_row(cb, 1, sep, (int)strlen(sep));

    /* Add staged diff as comment lines. */
    int row = 2;
    const char *p = summary;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char line[512];
        int llen = snprintf(line, sizeof(line), "# %.*s", len, p);
        buf_insert_row(cb, row++, line, llen);
        if (!nl) break;
        p = nl + 1;
    }
    free(summary);
    cb->dirty = 0;

    /* Switch to commit buffer. */
    pane_save_cursor();
    editor_buf_save(E.cur_buftab);
    E.panes[E.cur_pane].buf_idx = cidx;
    E.cur_buftab = cidx;
    editor_buf_restore(cidx);
    E.syntax = NULL;
    E.mode = MODE_INSERT;  /* start in insert mode for convenience */
    E.cy = 0; E.cx = 0;
    E.rowoff = 0; E.coloff = 0;
}

/* Extract non-comment, non-empty lines from the commit buffer as the message. */
static char *commit_extract_message(void) {
    size_t cap = 256, len = 0;
    char *msg = malloc(cap);
    if (!msg) return NULL;

    for (int i = 0; i < E.buf.numrows; i++) {
        Row *r = &E.buf.rows[i];
        if (r->len > 0 && r->chars[0] == '#') continue;  /* skip comments */
        /* Include the line (even if blank, for paragraph separation). */
        size_t need = len + r->len + 2;
        if (need > cap) {
            cap = need * 2;
            char *tmp = realloc(msg, cap);
            if (!tmp) { free(msg); return NULL; }
            msg = tmp;
        }
        if (r->len > 0) memcpy(msg + len, r->chars, r->len);
        len += r->len;
        msg[len++] = '\n';
    }

    /* Trim trailing whitespace. */
    while (len > 0 && (msg[len-1] == '\n' || msg[len-1] == ' '))
        len--;
    msg[len] = '\0';

    if (len == 0) { free(msg); return NULL; }
    return msg;
}

static void commit_execute(void) {
    char *msg = commit_extract_message();
    if (!msg) { status_err("Empty commit message — aborting"); return; }

    char output[128];
    int ok = git_commit(msg, output, sizeof(output));
    free(msg);

    if (ok) {
        /* Close commit buffer and return to previous buffer. */
        for (int i = 0; i < E.buf.numrows; i++) {
            free(E.buf.rows[i].chars);
            free(E.buf.rows[i].hl);
        }
        free(E.buf.rows);
        free(E.buf.git_signs);
        memset(&E.buf, 0, sizeof(Buffer));
        E.buf.hl_dirty_from = INT_MAX;
        memset(&E.buftabs[E.cur_buftab], 0, sizeof(BufTab));

        /* Switch back to previous buffer (buftab 0 as fallback). */
        int prev = 0;
        for (int i = E.num_buftabs - 1; i >= 0; i--) {
            if (i != E.cur_buftab && (E.buftabs[i].buf.rows ||
                i == 0)) { prev = i; break; }
        }
        E.num_buftabs--;
        E.cur_buftab = prev;
        E.panes[E.cur_pane].buf_idx = prev;
        editor_buf_restore(prev);
        editor_detect_syntax();
        E.mode = MODE_NORMAL;

        /* Update git branch (might have changed) and gutter. */
        git_current_branch(E.git_branch, sizeof(E.git_branch));
        editor_update_git_signs();

        snprintf(E.statusmsg, sizeof(E.statusmsg), "%s", output);
        E.statusmsg_is_error = 0;
    } else {
        status_err("Commit failed: %s", output);
    }
}

/* ── Diff pane (HEAD vs working copy) ────────────────────────────────── */

static int diff_pane_idx(void) {
    for (int i = 0; i < E.num_panes; i++)
        if (E.buftabs[E.panes[i].buf_idx].kind == BT_DIFF) return i;
    return -1;
}

static void diff_close(void) {
    int dpi = diff_pane_idx();
    if (dpi < 0) return;

    Pane *dp = &E.panes[dpi];
    int buf_idx = dp->buf_idx;
    int diff_left  = dp->left;
    int diff_width = dp->width;

    /* Find the source pane to the right and expand it. */
    int cpane = -1;
    for (int i = 0; i < E.num_panes; i++) {
        if (i != dpi && E.panes[i].left == diff_left + diff_width + 1) {
            cpane = i; break;
        }
    }
    if (cpane >= 0) {
        E.panes[cpane].left   = diff_left;
        E.panes[cpane].width += diff_width + 1;
    }

    /* Free diff buffer. */
    if (E.cur_pane == dpi) {
        for (int i = 0; i < E.buf.numrows; i++) {
            free(E.buf.rows[i].chars);
            free(E.buf.rows[i].hl);
        }
        free(E.buf.rows);
        free(E.buf.git_signs);
        memset(&E.buf, 0, sizeof(Buffer));
        E.buf.hl_dirty_from = INT_MAX;
    } else {
        buf_free(&E.buftabs[buf_idx].buf);
    }
    memset(&E.buftabs[buf_idx], 0, sizeof(BufTab));

    /* Remove diff pane. */
    for (int i = dpi; i < E.num_panes - 1; i++) E.panes[i] = E.panes[i + 1];
    E.num_panes--;
    if (cpane > dpi) cpane--;

    /* Activate the source pane. */
    if (cpane >= 0) {
        int donor_buf = E.panes[cpane].buf_idx;
        if (E.cur_buftab != donor_buf) {
            editor_buf_restore(donor_buf);
            E.cur_buftab = donor_buf;
        }
        E.cur_pane   = cpane;
        E.screenrows = E.panes[cpane].height;
        E.screencols = E.panes[cpane].width;
        E.cx     = E.panes[cpane].cx;
        E.cy     = E.panes[cpane].cy;
        E.rowoff = E.panes[cpane].rowoff;
        E.coloff = E.panes[cpane].coloff;
    }
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;
}

static void diff_open(void) {
    /* Toggle off if already open. */
    if (diff_pane_idx() >= 0) { diff_close(); return; }

    if (!E.buf.filename) { status_err("No filename"); return; }
    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return; }
    if (E.num_buftabs >= MAX_BUFS) { status_err("Too many buffers"); return; }

    /* Get HEAD version. */
    int head_count = 0;
    char **head_lines = git_show_head(E.buf.filename, &head_count);
    if (!head_lines || head_count == 0) {
        status_err("git show HEAD failed (file committed?)");
        return;
    }

    /* Need room for two panes. */
    Pane *sp = &E.panes[E.cur_pane];
    if (sp->width < 30) {
        for (int i = 0; i < head_count; i++) free(head_lines[i]);
        free(head_lines);
        status_err("Pane too narrow for diff");
        return;
    }

    /* Allocate diff buftab. */
    int didx = E.num_buftabs++;
    memset(&E.buftabs[didx], 0, sizeof(BufTab));
    E.buftabs[didx].kind = BT_DIFF;
    E.buftabs[didx].diff_source_buf = E.cur_buftab;
    E.buftabs[didx].syntax = syntax_detect(E.buf.filename);
    buf_init(&E.buftabs[didx].buf);

    /* Populate diff buffer rows. */
    for (int i = 0; i < head_count; i++) {
        int len = (int)strlen(head_lines[i]);
        buf_insert_row(&E.buftabs[didx].buf, i, head_lines[i], len);
        free(head_lines[i]);
    }
    free(head_lines);
    E.buftabs[didx].buf.dirty = 0;
    /* Copy filename so gutter git signs and syntax work. */
    E.buftabs[didx].buf.filename = strdup(E.buf.filename);

    /* Compute diff signs for both sides (background highlighting). */
    {
        Buffer *hbuf = &E.buftabs[didx].buf;
        GitLines new_gl = git_lines_from_buf(&E.buf);
        GitLines old_gl = git_lines_from_buf(hbuf);
        char *ns = NULL, *os = NULL;
        git_diff_signs_both(E.buf.filename, &new_gl, &old_gl, &ns, &os);
        git_lines_free(&new_gl);
        git_lines_free(&old_gl);
        free(E.buf.git_signs);
        E.buf.git_signs = ns;
        E.buf.git_signs_count = ns ? E.buf.numrows : 0;
        free(hbuf->git_signs);
        hbuf->git_signs = os;
        hbuf->git_signs_count = os ? hbuf->numrows : 0;
    }

    /* Split: left = HEAD (diff), right = working copy. */
    int src_idx = E.cur_pane;
    pane_save_cursor();
    editor_buf_save(E.cur_buftab);

    sp = &E.panes[src_idx];
    int old_left  = sp->left;
    int old_width = sp->width;
    int half = old_width / 2;

    sp->left  = old_left + half + 1;
    sp->width = old_width - half - 1;

    /* Insert diff pane before the source pane. */
    for (int i = E.num_panes; i > src_idx; i--) E.panes[i] = E.panes[i - 1];
    E.num_panes++;
    src_idx++;  /* source pane shifted right */

    Pane *dp = &E.panes[src_idx - 1];
    dp->top     = E.panes[src_idx].top;
    dp->left    = old_left;
    dp->height  = E.panes[src_idx].height;
    dp->width   = half;
    dp->buf_idx = didx;
    dp->cx      = 0;
    dp->cy      = E.cy;
    dp->rowoff  = E.rowoff;
    dp->coloff  = 0;

    /* Keep focus on source (working copy) pane. */
    E.cur_pane   = src_idx;
    E.screenrows = E.panes[src_idx].height;
    E.screencols = E.panes[src_idx].width;
    E.cur_buftab = E.panes[src_idx].buf_idx;
    editor_buf_restore(E.cur_buftab);
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;

    if (E.last_content_pane >= src_idx - 1) E.last_content_pane++;
}

/* ── Hunk operations ─────────────────────────────────────────────────── */

/* Find which hunk (0-based) contains cursor line cy (0-based).
   Returns -1 if cursor is not in any hunk. */
static int find_hunk_at_cursor(DiffHunk *hunks, int nhunks, int cy) {
    int line1 = cy + 1;  /* 1-based */
    for (int i = 0; i < nhunks; i++) {
        DiffHunk *h = &hunks[i];
        int start = h->new_start;
        int end   = start + h->new_count - 1;
        if (h->new_count == 0) {
            /* Pure deletion: cursor on the line just before insertion point. */
            if (line1 == h->new_start || line1 == h->new_start - 1)
                return i;
        } else if (line1 >= start && line1 <= end) {
            return i;
        }
    }
    return -1;
}

static void hunk_stage(void) {
    if (!E.buf.filename || E.buf.numrows <= 0) {
        status_err("No file to stage"); return;
    }
    GitLines gl = git_lines_from_buf(&E.buf);

    int nhunks = 0;
    DiffHunk *hunks = git_get_hunks(E.buf.filename, &gl, &nhunks);
    if (!hunks) {
        git_lines_free(&gl);
        status_err("No changes to stage"); return;
    }

    int idx = find_hunk_at_cursor(hunks, nhunks, E.cy);
    if (idx < 0) {
        free(hunks); git_lines_free(&gl);
        status_err("Cursor not on a changed hunk"); return;
    }

    int ok = git_stage_hunk(E.buf.filename, &gl, idx);
    free(hunks); git_lines_free(&gl);

    if (ok) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Hunk staged");
        E.statusmsg_is_error = 0;
        editor_update_git_signs();
    } else {
        status_err("Failed to stage hunk");
    }
}

static void hunk_revert(void) {
    if (!E.buf.filename || E.buf.numrows <= 0) {
        status_err("No file to revert"); return;
    }
    GitLines gl = git_lines_from_buf(&E.buf);
    int nhunks = 0;
    DiffHunk *hunks = git_get_hunks(E.buf.filename, &gl, &nhunks);
    git_lines_free(&gl);
    if (!hunks) { status_err("No changes to revert"); return; }

    int idx = find_hunk_at_cursor(hunks, nhunks, E.cy);
    if (idx < 0) {
        free(hunks); status_err("Cursor not on a changed hunk"); return;
    }

    DiffHunk h = hunks[idx];
    free(hunks);

    /* Get HEAD version of the file. */
    int head_count = 0;
    char **head_lines = git_show_head(E.buf.filename, &head_count);
    if (!head_lines) { status_err("Cannot read HEAD version"); return; }

    /* Push undo before modifying buffer. */
    push_undo("sort");

    /* Delete new lines from buffer (new_start is 1-based). */
    int del_from = h.new_start - 1;  /* 0-based */
    for (int i = 0; i < h.new_count && del_from < E.buf.numrows; i++)
        buf_delete_row(&E.buf, del_from);

    /* Insert old lines from HEAD. */
    for (int i = 0; i < h.old_count; i++) {
        int src = h.old_start - 1 + i;  /* 0-based index into head_lines */
        if (src >= 0 && src < head_count) {
            int len = (int)strlen(head_lines[src]);
            buf_insert_row(&E.buf, del_from + i, head_lines[src], len);
        }
    }

    for (int i = 0; i < head_count; i++) free(head_lines[i]);
    free(head_lines);

    /* Reposition cursor. */
    if (E.cy >= E.buf.numrows) E.cy = E.buf.numrows - 1;
    if (E.cy < 0) E.cy = 0;

    editor_update_git_signs();
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Hunk reverted");
    E.statusmsg_is_error = 0;
}

/* ── Quickfix pane ───────────────────────────────────────────────────── */

static int qf_pane_idx(void) {
    for (int i = 0; i < E.num_panes; i++)
        if (E.buftabs[E.panes[i].buf_idx].kind == BT_QF) return i;
    return -1;
}

/* Jump to the selected quickfix entry in the last content pane. */
static void qf_jump(int idx) {
    int qpi = qf_pane_idx();
    if (qpi < 0) return;
    QfList *ql = E.buftabs[E.panes[qpi].buf_idx].qf;
    if (!ql || idx < 0 || idx >= ql->count) return;

    ql->selected = idx;

    /* Also update cy in the qf pane's buffer / pane slot. */
    if (qpi == E.cur_pane) {
        E.cy = idx;
        if (E.cy < E.rowoff) E.rowoff = E.cy;
        if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    } else {
        E.panes[qpi].cy = idx;
    }

    QfEntry *e = &ql->entries[idx];

    /* Find a content pane to display in. */
    int cpane = E.last_content_pane;
    if (cpane < 0 || cpane >= E.num_panes ||
        buftab_is_special(&E.buftabs[E.panes[cpane].buf_idx])) {
        cpane = -1;
        for (int i = 0; i < E.num_panes; i++) {
            if (!buftab_is_special(&E.buftabs[E.panes[i].buf_idx]))
                { cpane = i; break; }
        }
    }
    if (cpane < 0) return;

    /* Activate content pane, open file. */
    pane_activate(cpane);
    open_new_buf(e->path);
    E.panes[E.cur_pane].buf_idx = E.cur_buftab;

    /* Jump to line (cy/rowoff saved into content pane slot by pane_activate below). */
    int row = e->line - 1;
    if (row < 0) row = 0;
    if (row >= E.buf.numrows) row = E.buf.numrows > 0 ? E.buf.numrows - 1 : 0;
    E.cy = row;
    E.cx = (e->col > 1) ? e->col - 1 : 0;
    if (E.cx >= E.buf.rows[E.cy].len) E.cx = 0;
    /* Adjust rowoff so the target line is visible in the inactive pane rendering.
       Inactive panes use p->rowoff directly — editor_scroll() only runs for the
       active pane, so we must set rowoff correctly before saving via pane_activate. */
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    else if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows / 2;

    /* Return focus to qf pane; pane_activate saves the content cursor first. */
    pane_activate(qpi);
}

static void qf_close_pane(void) {
    int qpi = qf_pane_idx();
    if (qpi < 0) return;

    int qf_top    = E.panes[qpi].top;
    int qf_h      = E.panes[qpi].height;
    int buf_idx   = E.panes[qpi].buf_idx;
    int qf_active = (E.cur_pane == qpi);

    /* Find the content pane to switch to after removal. */
    int cpane = E.last_content_pane;
    if (cpane < 0 || cpane >= E.num_panes || cpane == qpi ||
        buftab_is_special(&E.buftabs[E.panes[cpane].buf_idx])) {
        cpane = -1;
        for (int i = 0; i < E.num_panes; i++) {
            if (i != qpi && !buftab_is_special(&E.buftabs[E.panes[i].buf_idx])) {
                cpane = i; break;
            }
        }
    }

    /* Expand panes whose status bar row is directly above the qf pane. */
    for (int i = 0; i < E.num_panes; i++) {
        if (i == qpi) continue;
        Pane *p = &E.panes[i];
        if (p->top + p->height + 1 == qf_top)
            p->height += qf_h + 1;
    }

    /* Free the qf buffer.
       When the qf pane is active, the buffer lives in E.buf — free it directly
       without saving, so the qf content is never written over any content buftab.
       When inactive, it is parked in the buftab. */
    if (qf_active) {
        for (int i = 0; i < E.buf.numrows; i++) {
            free(E.buf.rows[i].chars);
            free(E.buf.rows[i].hl);
        }
        free(E.buf.rows);
        memset(&E.buf, 0, sizeof(Buffer));
        E.buf.hl_dirty_from = INT_MAX;
    } else {
        pane_save_cursor();   /* preserve live content cursor in its pane slot */
        buf_free(&E.buftabs[buf_idx].buf);
    }

    QfList *ql = E.buftabs[buf_idx].qf;
    if (ql) { qf_free(ql); free(ql); }
    memset(&E.buftabs[buf_idx], 0, sizeof(BufTab));
    /* No buftab compaction — all other pane buf_idx values remain valid. */

    /* Remove the qf pane from the array. */
    for (int i = qpi; i < E.num_panes - 1; i++) E.panes[i] = E.panes[i + 1];
    E.num_panes--;
    if (cpane > qpi) cpane--;

    /* Manually activate the content pane (avoids pane_activate's stale-index
       pane_save_cursor + buffer save that would clobber the content buffer). */
    if (cpane >= 0) {
        int donor_buf = E.panes[cpane].buf_idx;
        if (E.cur_buftab != donor_buf) {
            editor_buf_restore(donor_buf);
            E.cur_buftab = donor_buf;
        }
        E.cur_pane   = cpane;
        E.screenrows = E.panes[cpane].height;
        E.screencols = E.panes[cpane].width;
        E.cx     = E.panes[cpane].cx;
        E.cy     = E.panes[cpane].cy;
        E.rowoff = E.panes[cpane].rowoff;
        E.coloff = E.panes[cpane].coloff;
    }
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;
}

static void qf_open_pane(QfList *ql) {
    /* Close existing qf pane first (replace with new results). */
    if (qf_pane_idx() >= 0) qf_close_pane();

    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return; }
    if (E.num_buftabs >= MAX_BUFS) { status_err("Too many buffers"); return; }

    int qh  = E.opts.qf_height_rows;
    /* qf content top: leaves room for qf status bar + command bar at bottom */
    int qf_top = E.term_rows - 1 - qh;   /* status bar at term_rows-1 */

    /* Ensure content panes don't overlap the qf zone. */
    for (int i = 0; i < E.num_panes; i++) {
        Pane *p = &E.panes[i];
        if (p->top + p->height >= qf_top) {
            int new_h = qf_top - 1 - p->top;
            if (new_h < 2) { status_err("Not enough space for quickfix"); return; }
            p->height = new_h;
        }
    }

    /* Remember which pane is the current content pane. */
    if (!buftab_is_special(&E.buftabs[E.panes[E.cur_pane].buf_idx]))
        E.last_content_pane = E.cur_pane;

    /* Allocate buftab for qf buffer. */
    int qidx = E.num_buftabs++;
    memset(&E.buftabs[qidx], 0, sizeof(BufTab));
    E.buftabs[qidx].kind = BT_QF;
    E.buftabs[qidx].qf    = ql;

    buf_init(&E.buftabs[qidx].buf);
    qf_render_to_buf(ql, &E.buftabs[qidx].buf);

    /* Append qf pane at the bottom. */
    int qpi = E.num_panes++;
    E.panes[qpi].top    = qf_top;
    E.panes[qpi].left   = 1;
    E.panes[qpi].height = qh;
    E.panes[qpi].width  = E.term_cols;
    E.panes[qpi].buf_idx = qidx;
    E.panes[qpi].cx = E.panes[qpi].cy = 0;
    E.panes[qpi].rowoff = E.panes[qpi].coloff = 0;

    /* Activate the qf pane. */
    pane_save_cursor();
    editor_buf_save(E.cur_buftab);
    E.cur_pane   = qpi;
    E.cur_buftab = qidx;
    E.cx = E.cy = E.rowoff = E.coloff = 0;
    E.screenrows = qh;
    E.screencols = E.term_cols;
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;
    editor_buf_restore(qidx);
}

static void qf_handle_key(int c) {
    int qpi = qf_pane_idx();
    if (qpi < 0) return;
    QfList *ql = E.buftabs[E.panes[qpi].buf_idx].qf;

    if (c == 'q' || c == '\x1b') { qf_close_pane(); return; }

    if ((c == ARROW_DOWN || c == 'j') && ql && E.cy < ql->count - 1) {
        E.cy++;
        if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    } else if ((c == ARROW_UP || c == 'k') && E.cy > 0) {
        E.cy--;
        if (E.cy < E.rowoff) E.rowoff = E.cy;
    } else if (c == '\r' && ql && ql->count > 0) {
        qf_jump(E.cy);
    }
}

/* ── Local revisions (undo tree browser) ─────────────────────────────── */

/* Saved original state for preview — restored on close, discarded on Enter. */
static UndoState rev_saved_state;
static int       rev_saved_valid;
static int       rev_saved_src_buf;  /* buftab index of the source buffer */

static int rev_pane_idx(void) {
    for (int i = 0; i < E.num_panes; i++)
        if (E.buftabs[E.panes[i].buf_idx].kind == BT_REVISIONS) return i;
    return -1;
}

/* Render the undo tree into buffer rows for display.
   Format: indented tree with markers for current node and branch points.
   Example:
     ● initial                 [root]
     ├─ insert                 seq 1
     │  └─ delete              seq 3
     └─ insert                 seq 2  ◀
*/
static void rev_render_node(UndoNode *n, Buffer *buf, UndoNode *current,
                            const char *prefix, int is_last, int is_root) {
    char line[256];
    int pos = 0;

    if (is_root) {
        pos += snprintf(line + pos, sizeof(line) - pos, "● %s", n->desc[0] ? n->desc : "(root)");
    } else {
        pos += snprintf(line + pos, sizeof(line) - pos, "%s", prefix);
        pos += snprintf(line + pos, sizeof(line) - pos, "%s ", is_last ? "└─" : "├─");
        pos += snprintf(line + pos, sizeof(line) - pos, "%s", n->desc[0] ? n->desc : "(edit)");
    }

    /* Mark current node. */
    if (n == current)
        pos += snprintf(line + pos, sizeof(line) - pos, "  ◀");

    /* Lines/cursor info. */
    pos += snprintf(line + pos, sizeof(line) - pos, "  (%d lines, %d:%d)",
                    n->state.numrows, n->state.cy + 1, n->state.cx);

    buf_insert_row(buf, buf->numrows, line, pos);

    /* Build child prefix. */
    char child_prefix[128];
    if (is_root) {
        child_prefix[0] = '\0';
    } else {
        snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix,
                 is_last ? "   " : "│  ");
    }

    for (int i = 0; i < n->num_children; i++) {
        rev_render_node(n->children[i], buf, current, child_prefix,
                        i == n->num_children - 1, 0);
    }
}

static void rev_render_to_buf(const UndoTree *tree, Buffer *buf, UndoNode *current) {
    /* Clear existing rows. */
    for (int i = 0; i < buf->numrows; i++) {
        free(buf->rows[i].chars);
        free(buf->rows[i].hl);
    }
    free(buf->rows);
    buf->rows = NULL;
    buf->numrows = 0;

    if (!tree->root) {
        buf_insert_row(buf, 0, "(no undo history)", 17);
        return;
    }

    rev_render_node(tree->root, buf, current, "", 1, 1);
    buf->dirty = 0;
}

/* Map a buffer row index to the corresponding UndoNode (DFS order). */
static UndoNode *rev_node_at_row(UndoNode *n, int *row, int target) {
    if (*row == target) return n;
    (*row)++;
    for (int i = 0; i < n->num_children; i++) {
        UndoNode *found = rev_node_at_row(n->children[i], row, target);
        if (found) return found;
    }
    return NULL;
}

static void rev_close_pane(void);

/* Update the source content pane to preview a given undo node's state.
   Also compute line-level diff against the saved original for coloring. */
static void rev_update_preview(UndoNode *node) {
    if (!node || !rev_saved_valid) return;

    int src_buf = rev_saved_src_buf;
    Buffer *buf = &E.buftabs[src_buf].buf;

    /* Replace the parked buffer rows with the node's state. */
    for (int i = 0; i < buf->numrows; i++) {
        free(buf->rows[i].chars);
        free(buf->rows[i].hl);
    }
    free(buf->rows);
    buf->rows = NULL;
    buf->numrows = 0;

    for (int i = 0; i < node->state.numrows; i++) {
        buf_insert_row(buf, i, node->state.row_chars[i], node->state.row_lens[i]);
    }

    /* Compute line-level diff signs against the saved original. */
    free(buf->git_signs);
    int nrows = buf->numrows;
    buf->git_signs = malloc(nrows > 0 ? nrows : 1);
    buf->git_signs_count = nrows;

    UndoState *orig = &rev_saved_state;
    for (int i = 0; i < nrows; i++) {
        if (i >= orig->numrows) {
            /* Line beyond original = added. */
            buf->git_signs[i] = GIT_SIGN_ADD;
        } else if (node->state.row_lens[i] != orig->row_lens[i] ||
                   memcmp(node->state.row_chars[i], orig->row_chars[i],
                          node->state.row_lens[i]) != 0) {
            /* Line differs from original = modified. */
            buf->git_signs[i] = GIT_SIGN_MOD;
        } else {
            buf->git_signs[i] = GIT_SIGN_NONE;
        }
    }
    /* Mark a deletion indicator if the original had more lines. */
    if (orig->numrows > nrows && nrows > 0) {
        buf->git_signs[nrows - 1] = GIT_SIGN_DEL;
    }

    /* Force syntax re-highlight. */
    buf->hl_dirty_from = 0;

    /* Update the parked cursor to match the node's cursor position. */
    E.buftabs[src_buf].cx = node->state.cx;
    E.buftabs[src_buf].cy = node->state.cy;
}

/* Restore the source buffer from the saved original state. */
static void rev_restore_original(void) {
    if (!rev_saved_valid) return;

    int src_buf = rev_saved_src_buf;
    Buffer *buf = &E.buftabs[src_buf].buf;

    /* Replace rows with original. */
    for (int i = 0; i < buf->numrows; i++) {
        free(buf->rows[i].chars);
        free(buf->rows[i].hl);
    }
    free(buf->rows);
    buf->rows = NULL;
    buf->numrows = 0;

    for (int i = 0; i < rev_saved_state.numrows; i++) {
        buf_insert_row(buf, i, rev_saved_state.row_chars[i],
                       rev_saved_state.row_lens[i]);
    }

    /* Restore cursor. */
    E.buftabs[src_buf].cx = rev_saved_state.cx;
    E.buftabs[src_buf].cy = rev_saved_state.cy;

    /* Clear diff signs. */
    free(buf->git_signs);
    buf->git_signs = NULL;
    buf->git_signs_count = 0;

    buf->hl_dirty_from = 0;
}

static void rev_activate_entry(void) {
    int rpi = rev_pane_idx();
    if (rpi < 0) return;

    BufTab *rbt = &E.buftabs[E.panes[rpi].buf_idx];
    int src_buf = rbt->rev_source_buf;

    /* Find the undo tree for the source buffer (it's parked). */
    UndoTree *tree = &E.buftabs[src_buf].undo_tree;
    if (!tree->root) return;

    int row = 0;
    UndoNode *target = rev_node_at_row(tree->root, &row, E.cy);
    if (!target) return;

    /* Save the original state into its undo node before we accept. */
    if (rev_saved_valid && tree->current) {
        undo_state_free(&tree->current->state);
        tree->current->state = rev_saved_state;
        rev_saved_valid = 0;  /* consumed, don't free */
    }

    /* The preview is already showing this node's content in the parked buffer.
       Just update the undo tree pointer and clear diff signs. */
    tree->current = target;
    Buffer *buf = &E.buftabs[src_buf].buf;
    free(buf->git_signs);
    buf->git_signs = NULL;
    buf->git_signs_count = 0;
    buf->dirty = 1;

    /* Close the revisions pane (switches focus to content). */
    rev_close_pane();
}

static void rev_close_pane(void) {
    int rpi = rev_pane_idx();
    if (rpi < 0) return;

    int buf_idx    = E.panes[rpi].buf_idx;
    int rev_active = (E.cur_pane == rpi);
    int rev_right  = E.panes[rpi].left + E.panes[rpi].width + 1;

    /* Find content pane to return to. */
    int cpane = E.last_content_pane;
    if (cpane < 0 || cpane >= E.num_panes || cpane == rpi ||
        buftab_is_special(&E.buftabs[E.panes[cpane].buf_idx])) {
        cpane = -1;
        for (int i = 0; i < E.num_panes; i++) {
            if (i != rpi && !buftab_is_special(&E.buftabs[E.panes[i].buf_idx])) {
                cpane = i; break;
            }
        }
    }

    /* Restore the original buffer content if we're closing without accepting. */
    if (rev_saved_valid) {
        rev_restore_original();
        undo_state_free(&rev_saved_state);
        rev_saved_valid = 0;
    }

    /* Expand panes immediately right of the revisions pane to reclaim width. */
    for (int i = 0; i < E.num_panes; i++) {
        if (i == rpi) continue;
        if (E.panes[i].left == rev_right) {
            E.panes[i].left  = E.panes[rpi].left;
            E.panes[i].width += E.panes[rpi].width + 1;
        }
    }

    /* Free the revisions buffer. */
    if (rev_active) {
        for (int i = 0; i < E.buf.numrows; i++) {
            free(E.buf.rows[i].chars);
            free(E.buf.rows[i].hl);
        }
        free(E.buf.rows);
        memset(&E.buf, 0, sizeof(Buffer));
        E.buf.hl_dirty_from = INT_MAX;
    } else {
        pane_save_cursor();
        buf_free(&E.buftabs[buf_idx].buf);
    }

    memset(&E.buftabs[buf_idx], 0, sizeof(BufTab));

    /* Remove pane. */
    for (int i = rpi; i < E.num_panes - 1; i++) E.panes[i] = E.panes[i + 1];
    E.num_panes--;
    if (cpane > rpi) cpane--;

    /* Activate content pane. */
    if (cpane >= 0) {
        int donor_buf = E.panes[cpane].buf_idx;
        if (E.cur_buftab != donor_buf) {
            editor_buf_restore(donor_buf);
            E.cur_buftab = donor_buf;
        }
        E.cur_pane   = cpane;
        E.screenrows = E.panes[cpane].height;
        E.screencols = E.panes[cpane].width;
        E.cx     = E.panes[cpane].cx;
        E.cy     = E.panes[cpane].cy;
        E.rowoff = E.panes[cpane].rowoff;
        E.coloff = E.panes[cpane].coloff;
    }
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;
}

#define REV_WIDTH 40

static void rev_open_pane(void) {
    /* Close existing revisions pane if open (toggle). */
    if (rev_pane_idx() >= 0) { rev_close_pane(); return; }

    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return; }
    if (E.num_buftabs >= MAX_BUFS) { status_err("Too many buffers"); return; }

    /* The source is the current content buffer. */
    int src_buf = E.cur_buftab;
    if (buftab_is_special(&E.buftabs[src_buf])) {
        status_err("No undo tree for special buffers");
        return;
    }

    /* Save current state into the undo tree's current node so the display
       reflects the live buffer accurately. */
    if (E.undo_tree.current) {
        undo_state_free(&E.undo_tree.current->state);
        E.undo_tree.current->state = editor_capture_state();
    }

    /* Save the original state for preview revert on close. */
    rev_saved_state   = editor_capture_state();
    rev_saved_valid   = 1;
    rev_saved_src_buf = src_buf;

    /* Verify panes will still have enough width. */
    int rev_zone = REV_WIDTH + 2;  /* pane + separator col */
    for (int i = 0; i < E.num_panes; i++) {
        if (E.panes[i].left < rev_zone) {
            int new_width = E.panes[i].width - (rev_zone - E.panes[i].left);
            if (new_width < 5) { status_err("Pane too narrow for revisions"); return; }
        }
    }

    int cp_idx = E.cur_pane;
    if (!buftab_is_special(&E.buftabs[E.panes[cp_idx].buf_idx]))
        E.last_content_pane = cp_idx;

    /* Allocate buftab for revisions buffer. */
    int ridx = E.num_buftabs++;
    memset(&E.buftabs[ridx], 0, sizeof(BufTab));
    E.buftabs[ridx].kind = BT_REVISIONS;
    E.buftabs[ridx].rev_source_buf = src_buf;

    buf_init(&E.buftabs[ridx].buf);
    rev_render_to_buf(&E.undo_tree, &E.buftabs[ridx].buf, E.undo_tree.current);

    /* Save current live content buffer. */
    pane_save_cursor();
    editor_buf_save(E.cur_buftab);
    la_free();
    insert_rec_reset();
    completion_free();
    E.pending_op = '\0';
    E.count      = 0;

    /* Push all panes that overlap the revisions zone to the right. */
    for (int i = 0; i < E.num_panes; i++) {
        if (E.panes[i].left < rev_zone) {
            int shift = rev_zone - E.panes[i].left;
            E.panes[i].width -= shift;
            E.panes[i].left   = rev_zone;
        }
    }

    /* Insert revisions pane at index 0 (shift all others). */
    for (int i = E.num_panes; i > 0; i--) E.panes[i] = E.panes[i-1];
    E.num_panes++;
    E.last_content_pane = cp_idx + 1;  /* shifted by insertion */

    /* Revisions pane: far left, full terminal height. */
    E.panes[0].top     = 1;
    E.panes[0].left    = 1;
    E.panes[0].height  = E.term_rows - 2;
    E.panes[0].width   = REV_WIDTH;
    E.panes[0].buf_idx = ridx;
    E.panes[0].cx      = 0;
    E.panes[0].cy      = 0;
    E.panes[0].rowoff  = 0;
    E.panes[0].coloff  = 0;

    /* Find the row of the current undo node to position cursor there. */
    int rh = E.term_rows - 2;
    if (E.undo_tree.root && E.undo_tree.current) {
        int row = 0;
        for (int r = 0; r < E.buftabs[ridx].buf.numrows; r++) {
            UndoNode *n = rev_node_at_row(E.undo_tree.root, &row, r);
            if (n == E.undo_tree.current) {
                E.panes[0].cy = r;
                if (r >= rh) E.panes[0].rowoff = r - rh / 2;
                break;
            }
            row = 0;
        }
    }

    /* Activate revisions pane. */
    E.cur_pane   = 0;
    E.cur_buftab = ridx;
    E.screenrows = rh;
    E.screencols = REV_WIDTH;
    E.cx = 0;
    E.cy = E.panes[0].cy;
    E.rowoff = E.panes[0].rowoff;
    E.coloff = 0;
    E.mode = MODE_NORMAL;
    E.match_bracket_valid = 0;
    editor_buf_restore(ridx);
}

static void rev_handle_key(int c) {
    int rpi = rev_pane_idx();
    if (rpi < 0) return;

    if (c == 'q' || c == '\x1b') { rev_close_pane(); return; }

    int moved = 0;
    if ((c == ARROW_DOWN || c == 'j') && E.cy < E.buf.numrows - 1) {
        E.cy++;
        if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
        moved = 1;
    } else if ((c == ARROW_UP || c == 'k') && E.cy > 0) {
        E.cy--;
        if (E.cy < E.rowoff) E.rowoff = E.cy;
        moved = 1;
    } else if (c == 'G') {
        E.cy = E.buf.numrows - 1;
        if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
        moved = 1;
    } else if (c == 'g') {
        E.cy = 0; E.rowoff = 0;
        moved = 1;
    } else if (c == '\r') {
        rev_activate_entry();
        return;
    }

    /* Update preview in content pane after cursor movement. */
    if (moved && rev_saved_valid) {
        BufTab *rbt = &E.buftabs[E.panes[rpi].buf_idx];
        UndoTree *tree = &E.buftabs[rbt->rev_source_buf].undo_tree;
        if (tree->root) {
            int row = 0;
            UndoNode *node = rev_node_at_row(tree->root, &row, E.cy);
            if (node) rev_update_preview(node);
        }
    }
}

/* ── Command execution ───────────────────────────────────────────────── */

static void editor_quit(void) {
    write(STDOUT_FILENO, "\x1b[0 q", 5);  /* reset cursor shape */
    exit(EXIT_SUCCESS);
}

/* ── Multi-buffer helpers ────────────────────────────────────────────── */

static void completion_free(void);   /* forward declaration — defined below */

/* Clear transient editing state and switch the live slot to idx. */
static void jump_push(void);  /* forward declaration — defined after switch_to_buf */

static void switch_to_buf(int idx) {
    jump_push();
    editor_buf_save(E.cur_buftab);
    la_free();
    insert_rec_reset();
    completion_free();
    E.pending_op          = '\0';
    E.count               = 0;
    E.match_bracket_valid = 0;
    E.statusmsg[0]        = '\0';
    E.statusmsg_is_error  = 0;
    E.mode                = MODE_NORMAL;
    E.cur_buftab                      = idx;
    E.panes[E.cur_pane].buf_idx       = idx;
    editor_buf_restore(idx);
}

/* ── Jump list (Ctrl-O / Ctrl-I) ─────────────────────────────────────── */

/* Save current position before a jump.  Truncates any forward history. */
static void jump_push(void) {
    int buf = E.cur_buftab, row = E.cy, col = E.cx;

    /* Truncate forward history if we navigated back and are now jumping. */
    if (E.jump_cur < E.jump_count)
        E.jump_count = E.jump_cur;

    /* Skip duplicate of last entry. */
    if (E.jump_count > 0) {
        JumpEntry *t = &E.jump_list[E.jump_count - 1];
        if (t->buf_idx == buf && t->row == row && t->col == col) {
            E.jump_cur = E.jump_count;
            return;
        }
    }

    /* Evict oldest entry on overflow. */
    if (E.jump_count >= JUMP_MAX) {
        memmove(E.jump_list, E.jump_list + 1,
                (JUMP_MAX - 1) * sizeof(JumpEntry));
        E.jump_count = JUMP_MAX - 1;
        if (E.jump_cur > 0) E.jump_cur--;
    }

    E.jump_list[E.jump_count++] = (JumpEntry){ buf, row, col };
    E.jump_cur = E.jump_count;
}

static void jump_navigate(const JumpEntry *e) {
    if (e->buf_idx != E.cur_buftab)
        switch_to_buf(e->buf_idx);
    E.cy = (E.buf.numrows > 0)
           ? (e->row < E.buf.numrows ? e->row : E.buf.numrows - 1)
           : 0;
    int len = (E.buf.numrows > 0) ? E.buf.rows[E.cy].len : 0;
    E.cx = (e->col <= len) ? e->col : len;
    E.preferred_col = E.cx;
}

static void jump_back(void) {
    if (E.jump_cur == E.jump_count) {
        /* At live position: save it first so Ctrl-I can return. */
        int buf = E.cur_buftab, row = E.cy, col = E.cx;
        int dup = E.jump_count > 0
               && E.jump_list[E.jump_count - 1].buf_idx == buf
               && E.jump_list[E.jump_count - 1].row     == row
               && E.jump_list[E.jump_count - 1].col     == col;
        if (!dup) {
            if (E.jump_count >= JUMP_MAX) {
                memmove(E.jump_list, E.jump_list + 1,
                        (JUMP_MAX - 1) * sizeof(JumpEntry));
                E.jump_count = JUMP_MAX - 1;
            }
            E.jump_list[E.jump_count++] = (JumpEntry){ buf, row, col };
        }
        E.jump_cur = E.jump_count;
        if (E.jump_count < 2) { status_err("Already at oldest jump"); return; }
        E.jump_cur -= 2;
    } else {
        if (E.jump_cur == 0) { status_err("Already at oldest jump"); return; }
        E.jump_cur--;
    }
    jump_navigate(&E.jump_list[E.jump_cur]);
}

static void jump_forward(void) {
    if (E.jump_cur >= E.jump_count - 1) {
        status_err("Already at newest jump");
        return;
    }
    E.jump_cur++;
    jump_navigate(&E.jump_list[E.jump_cur]);
}

/* Strip a leading "./" from a path for comparison purposes. */
static const char *norm_path(const char *p) {
    return (p[0] == '.' && p[1] == '/') ? p + 2 : p;
}

/* Open a new buffer (empty if filename==NULL) and switch to it.
   If filename is already open in any buffer, switch the current pane to
   that buffer instead of creating a duplicate (per-pane: we stay in the
   current pane — we never jump focus to another pane). */
static int open_new_buf(const char *filename) {
    if (filename) {
        const char *fn = norm_path(filename);
        for (int i = 0; i < E.num_buftabs; i++) {
            if (buftab_is_special(&E.buftabs[i])) continue;
            Buffer *b = (i == E.cur_buftab) ? &E.buf : &E.buftabs[i].buf;
            if (!b->filename) continue;
            if (strcmp(norm_path(b->filename), fn) == 0) {
                /* Already open — make current pane show this buffer. */
                if (i != E.cur_buftab) switch_to_buf(i);
                E.panes[E.cur_pane].buf_idx = i;
                /* Use E.buf.filename: if we just switched, b now points into
                   the zeroed buftabs slot; E.buf holds the live filename. */
                status_msg("\"%s\" (already open)", E.buf.filename);
                return 1;
            }
        }
    }

    if (E.num_buftabs >= MAX_BUFS) {
        status_err("Too many open buffers (max %d)", MAX_BUFS);
        return 0;
    }
    int idx = E.num_buftabs++;
    memset(&E.buftabs[idx], 0, sizeof(BufTab));
    E.buftabs[idx].watch_handle = -1;
    switch_to_buf(idx);   /* saves current, clears transient, activates new slot */
    buf_init(&E.buf);
    if (filename) {
        buf_open(&E.buf, filename);
        editor_detect_syntax();
        editor_update_git_signs();
        filewatcher_add(E.cur_buftab);
        status_msg("\"%s\"", E.buf.filename ? E.buf.filename : filename);
    }
    return 1;
}

/* Open an embedded terminal in a horizontal split below the current pane. */
/* Close terminal buffer at buftab index `bi` and its pane (if multi-pane).
   If it's the last pane, quits the editor. */
static void term_close_buf(int bi) {
    term_emu_close(E.buftabs[bi].term);
    E.buftabs[bi].term = NULL;

    /* Find the pane showing this buffer. */
    int pane_idx = -1;
    for (int pi = 0; pi < E.num_panes; pi++)
        if (E.panes[pi].buf_idx == bi) { pane_idx = pi; break; }

    if (E.num_panes > 1 && pane_idx >= 0) {
        /* If the terminal isn't the active pane, make it active first
           so pane_close operates on it. */
        if (pane_idx != E.cur_pane)
            pane_activate(pane_idx);
        int closing_buf = E.cur_buftab;
        pane_close(E.cur_pane);
        /* Remove the terminal buffer slot. */
        buf_free(&E.buftabs[closing_buf].buf);
        for (int si = closing_buf; si < E.num_buftabs - 1; si++)
            E.buftabs[si] = E.buftabs[si + 1];
        memset(&E.buftabs[E.num_buftabs - 1], 0, sizeof(BufTab));
        E.num_buftabs--;
        for (int pi = 0; pi < E.num_panes; pi++)
            if (E.panes[pi].buf_idx > closing_buf)
                E.panes[pi].buf_idx--;
        if (E.cur_buftab > closing_buf) E.cur_buftab--;
    } else {
        editor_quit();
    }
}

static void editor_open_terminal(const char *cmd) {
    if (E.num_buftabs >= MAX_BUFS) {
        status_err("Too many open buffers (max %d)", MAX_BUFS);
        return;
    }

    /* Split current pane: top keeps content, bottom gets terminal. */
    pane_save_cursor();
    if (E.num_panes >= MAX_PANES) { status_err("Too many panes"); return; }
    Pane *sp = &E.panes[E.cur_pane];
    int H = sp->height;
    int th = E.opts.term_height_rows;
    if (th > H - 2) th = H - 2;   /* leave at least 1 row + status bar for top */
    if (th < 1) { status_err("Pane too small for terminal"); return; }
    int h_top = H - 1 - th;       /* -1 for top pane's status bar */

    /* Insert new pane slot after current. */
    for (int i = E.num_panes; i > E.cur_pane + 1; i--)
        E.panes[i] = E.panes[i - 1];
    E.num_panes++;
    sp->height = h_top;
    Pane *np   = &E.panes[E.cur_pane + 1];
    *np        = *sp;
    np->top    = sp->top + h_top + 1;
    np->height = th;

    /* Allocate new buffer slot. */
    int idx = E.num_buftabs++;
    memset(&E.buftabs[idx], 0, sizeof(BufTab));

    /* Activate the new bottom pane and assign the terminal buffer. */
    int new_pane = E.cur_pane + 1;
    pane_activate(new_pane);
    editor_buf_save(E.cur_buftab);
    E.cur_buftab = idx;
    E.panes[E.cur_pane].buf_idx = idx;
    editor_buf_restore(idx);
    buf_init(&E.buf);
    if (cmd) {
        char title[128];
        snprintf(title, sizeof(title), "[Terminal: %s]", cmd);
        E.buf.filename = strdup(title);
    } else {
        E.buf.filename = strdup("[Terminal]");
    }

    Pane *p = &E.panes[E.cur_pane];
    TermState *ts = term_emu_open(p->height, p->width, cmd);
    if (!ts) {
        status_err("Failed to open terminal");
        return;
    }
    E.buftabs[idx].kind = BT_TERM;
    E.buftabs[idx].term = ts;
    E.mode = MODE_INSERT;  /* start in terminal-insert (keys go to PTY) */
}

/* Close the current buffer.  force=0 guards against unsaved changes.
   Returns 1 if the editor should quit (last buffer was closed). */
static int close_cur_buf(int force) {
    if (!force && E.buf.dirty && !buftab_is_special(&E.buftabs[E.cur_buftab])) {
        status_err("Unsaved changes (use :q! to override)");
        return 0;
    }

    /* Count content buffers that would remain after this close. */
    int remaining = 0;
    for (int i = 0; i < E.num_buftabs; i++) {
        if (i != E.cur_buftab && !buftab_is_special(&E.buftabs[i]))
            remaining++;
    }
    if (remaining == 0)
        return 1;   /* last content buffer — tell caller to quit */

    /* Free current live resources. */
    if (E.buftabs[E.cur_buftab].kind == BT_TERM) {
        term_emu_close(E.buftabs[E.cur_buftab].term);
        E.buftabs[E.cur_buftab].term = NULL;
    }
    buf_free(&E.buf);
    undo_tree_free(&E.undo_tree);
    if (E.has_pre_insert) {
        undo_state_free(&E.pre_insert_snapshot);
        E.has_pre_insert = 0;
    }
    la_free();
    insert_rec_reset();
    completion_free();
    E.pending_op = '\0';
    E.count      = 0;

    /* Remove cur_buftab slot from the parked array by shifting. */
    int cur = E.cur_buftab;
    for (int i = cur; i < E.num_buftabs - 1; i++)
        E.buftabs[i] = E.buftabs[i + 1];
    memset(&E.buftabs[E.num_buftabs - 1], 0, sizeof(BufTab));
    E.num_buftabs--;

    /* Update all pane buf_idx values after the shift. */
    for (int i = 0; i < E.num_panes; i++) {
        if (E.panes[i].buf_idx > cur)
            E.panes[i].buf_idx--;
        else if (E.panes[i].buf_idx == cur)
            E.panes[i].buf_idx = -1;   /* will be fixed to next below */
    }

    /* Pick next content buffer (prefer forward, then backward). */
    int next = -1;
    for (int i = cur; i < E.num_buftabs && next < 0; i++)
        if (!buftab_is_special(&E.buftabs[i])) next = i;
    for (int i = cur - 1; i >= 0 && next < 0; i--)
        if (!buftab_is_special(&E.buftabs[i])) next = i;
    if (next < 0) next = (cur < E.num_buftabs) ? cur : E.num_buftabs - 1;

    /* Redirect any panes that were showing the closed buffer. */
    for (int i = 0; i < E.num_panes; i++)
        if (E.panes[i].buf_idx == -1) E.panes[i].buf_idx = next;

    E.cur_buftab = next;
    E.panes[E.cur_pane].buf_idx = next;
    editor_buf_restore(next);
    E.mode = MODE_NORMAL;
    return 0;
}

/* ── Session save / restore ──────────────────────────────────────────── */

static void editor_save_session(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) { status_err("Cannot write %s", path); return; }

    /* Save CWD. */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        fprintf(fp, "cd %s\n", cwd);

    /* Save all non-special buffers. */
    for (int i = 0; i < E.num_buftabs; i++) {
        if (buftab_is_special(&E.buftabs[i])) continue;
        const Buffer *b = (i == E.cur_buftab) ? &E.buf : &E.buftabs[i].buf;
        if (!b->filename) continue;
        int cx = (i == E.cur_buftab) ? E.cx : E.buftabs[i].cx;
        int cy = (i == E.cur_buftab) ? E.cy : E.buftabs[i].cy;
        fprintf(fp, "buf %d %d %s\n", cy, cx, b->filename);
    }

    /* Mark the active buffer. */
    fprintf(fp, "active %d\n", E.cur_buftab);

    fclose(fp);
    status_msg("Session saved to %s", path);
}

static void editor_load_session(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { status_err("Cannot read %s", path); return; }

    char line[PATH_MAX + 64];
    int first = 1;
    int target_active = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline. */
        int len = (int)strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        if (strncmp(line, "cd ", 3) == 0) {
            chdir(line + 3);
        } else if (strncmp(line, "buf ", 4) == 0) {
            int cy, cx;
            char fname[PATH_MAX];
            if (sscanf(line + 4, "%d %d %[^\n]", &cy, &cx, fname) == 3) {
                /* Check if file exists. */
                struct stat st;
                if (stat(fname, &st) != 0) continue;

                if (first) {
                    /* Open in the current (live) buffer slot. */
                    buf_free(&E.buf);
                    buf_open(&E.buf, fname);
                    editor_detect_syntax();
                    filewatcher_add(E.cur_buftab);
                    E.cy = cy; E.cx = cx;
                    if (E.cy >= E.buf.numrows && E.buf.numrows > 0)
                        E.cy = E.buf.numrows - 1;
                    first = 0;
                } else {
                    open_new_buf(fname);
                    /* Restore cursor position in the newly opened buffer. */
                    E.cy = cy; E.cx = cx;
                    if (E.cy >= E.buf.numrows && E.buf.numrows > 0)
                        E.cy = E.buf.numrows - 1;
                }
            }
        } else if (strncmp(line, "active ", 7) == 0) {
            target_active = atoi(line + 7);
        }
    }
    fclose(fp);

    /* Switch to the saved active buffer. */
    if (target_active >= 0 && target_active < E.num_buftabs
        && !buftab_is_special(&E.buftabs[target_active])
        && target_active != E.cur_buftab) {
        switch_to_buf(target_active);
    }
    status_msg("Session loaded from %s", path);
}

/* ── Substitute helpers ──────────────────────────────────────────────── */

/* Parse a substitute command of the form [%|N[,M]]s/pat/rep/[flags].
   Returns 1 and fills params if it looks like a substitute command, else 0.
   *pat and *rep point into cmd (valid while cmd is valid). */
static int subst_parse(const char *cmd, int *r0, int *r1,
                       const char **pat, int *pat_len,
                       const char **rep, int *rep_len, int *global) {
    const char *p = cmd;
    *r0 = E.cy; *r1 = E.cy; *global = 0;

    /* Optional range prefix */
    if (*p == '%') {
        *r0 = 0;
        *r1 = E.buf.numrows > 0 ? E.buf.numrows - 1 : 0;
        p++;
    } else if (isdigit((unsigned char)*p)) {
        *r0 = atoi(p) - 1;
        while (isdigit((unsigned char)*p)) p++;
        if (*p == ',') {
            p++;
            *r1 = atoi(p) - 1;
            while (isdigit((unsigned char)*p)) p++;
        } else {
            *r1 = *r0;
        }
    }

    /* Must be 's' followed by a non-alphanumeric delimiter */
    if (*p != 's') return 0;
    p++;
    if (*p == '\0' || isalnum((unsigned char)*p)) return 0;
    char delim = *p++;

    /* Pattern (backslash escapes the delimiter) */
    const char *pat_start = p;
    while (*p && *p != delim) { if (*p == '\\' && p[1]) p++; p++; }
    *pat = pat_start;
    *pat_len = (int)(p - pat_start);
    if (*p != delim) return 0;   /* unterminated */
    p++;

    /* Replacement */
    const char *rep_start = p;
    while (*p && *p != delim) { if (*p == '\\' && p[1]) p++; p++; }
    *rep = rep_start;
    *rep_len = (int)(p - rep_start);
    if (*p == delim) p++;

    /* Flags */
    while (*p) { if (*p == 'g') *global = 1; p++; }
    return 1;
}

/* Apply substitution to one row. Returns number of replacements made. */
static int subst_row(Row *row, const SearchQuery *q,
                     const char *rep, int rep_len, int global) {
    int pos = 0, count = 0;
    int out_cap = row->len + rep_len + 64;
    char *out = malloc(out_cap + 1);
    if (!out) return 0;
    int out_len = 0;

    while (pos <= row->len) {
        int mc, ml;
        if (!q->match_fn(row->chars, row->len,
                         q->pattern, q->pat_len, pos, &mc, &ml)) {
            /* No more matches: copy remainder */
            int tail = row->len - pos;
            if (out_len + tail + 1 > out_cap) {
                out_cap = out_len + tail + 64;
                char *tmp = realloc(out, out_cap + 1);
                if (!tmp) { free(out); return count; }
                out = tmp;
            }
            memcpy(out + out_len, row->chars + pos, tail);
            out_len += tail;
            break;
        }

        /* Grow buffer if needed */
        int pre = mc - pos;
        if (out_len + pre + rep_len + 1 > out_cap) {
            out_cap = out_len + pre + rep_len + row->len + 64;
            char *tmp = realloc(out, out_cap + 1);
            if (!tmp) { free(out); return count; }
            out = tmp;
        }

        /* Copy pre-match text, then replacement */
        memcpy(out + out_len, row->chars + pos, pre); out_len += pre;
        memcpy(out + out_len, rep, rep_len);           out_len += rep_len;
        pos = mc + ml;
        count++;

        if (!global) {
            /* Copy rest and stop */
            int tail = row->len - pos;
            if (out_len + tail + 1 > out_cap) {
                out_cap = out_len + tail + 64;
                char *tmp = realloc(out, out_cap + 1);
                if (!tmp) { free(out); return count; }
                out = tmp;
            }
            memcpy(out + out_len, row->chars + pos, tail);
            out_len += tail;
            break;
        }

        /* Guard against zero-length match infinite loop */
        if (ml == 0) {
            if (pos < row->len) {
                if (out_len + 2 > out_cap) {
                    out_cap += 64;
                    char *tmp = realloc(out, out_cap + 1);
                    if (!tmp) { free(out); return count; }
                    out = tmp;
                }
                out[out_len++] = row->chars[pos];
            }
            pos++;
        }
    }

    out[out_len] = '\0';
    free(row->chars);
    free(row->hl);
    row->chars = out;
    row->len   = out_len;
    row->hl    = NULL;
    return count;
}

void editor_execute_command(void) {
    const char *cmd = E.cmdbuf;

    /* ── w/q/a/! compound family ──────────────────────────────────────
       Parses any combination of:
         [w][q][a][!][ filename]
       e.g. :w  :q  :wq  :qa  :wqa  :wqa!  :wa  etc.             */
    {
        const char *p = cmd;
        int dw = (*p == 'w'); if (dw) p++;
        int dq = (*p == 'q'); if (dq) p++;
        int da = (*p == 'a'); if (da) p++;
        int df = (*p == '!'); if (df) p++;
        const char *arg = (*p == ' ' && p[1]) ? p + 1 : NULL;

        if ((dw || dq) && (*p == '\0' || arg)) {

            /* Tree pane is read-only; :q from tree closes sidebar or quits. */
            if (E.buftabs[E.cur_buftab].kind == BT_TREE) {
                if (dw) { status_err("Tree pane is read-only"); goto done; }
                if (dq) {
                    if (E.num_panes > 1) {
                        /* Other panes exist — just close the tree sidebar. */
                        editor_open_tree_pane();
                    } else {
                        /* Tree is the last pane — quit the editor. */
                        if (!df) {
                            for (int i = 0; i < E.num_buftabs; i++) {
                                if (buftab_is_special(&E.buftabs[i])) continue;
                                Buffer *b = (i == E.cur_buftab)
                                            ? &E.buf : &E.buftabs[i].buf;
                                if (b->dirty) {
                                    const char *fn = b->filename
                                                     ? b->filename : "[No Name]";
                                    status_err("Unsaved changes in \"%s\" (use :q! to force)", fn);
                                    goto done;
                                }
                            }
                        }
                        editor_quit();
                    }
                    goto done;
                }
            }

            /* ── commit buffer: :wq commits, :q aborts ────────────── */
            if (E.buftabs[E.cur_buftab].kind == BT_COMMIT) {
                if (dw && dq) {
                    commit_execute();
                } else if (dq) {
                    /* Abort: close commit buffer without committing. */
                    for (int i = 0; i < E.buf.numrows; i++) {
                        free(E.buf.rows[i].chars);
                        free(E.buf.rows[i].hl);
                    }
                    free(E.buf.rows);
                    free(E.buf.git_signs);
                    memset(&E.buf, 0, sizeof(Buffer));
                    E.buf.hl_dirty_from = INT_MAX;
                    memset(&E.buftabs[E.cur_buftab], 0, sizeof(BufTab));
                    int prev = 0;
                    for (int i = E.num_buftabs - 1; i >= 0; i--) {
                        if (i != E.cur_buftab && (E.buftabs[i].buf.rows ||
                            i == 0)) { prev = i; break; }
                    }
                    E.num_buftabs--;
                    E.cur_buftab = prev;
                    E.panes[E.cur_pane].buf_idx = prev;
                    editor_buf_restore(prev);
                    editor_detect_syntax();
                    E.mode = MODE_NORMAL;
                    status_msg("Commit aborted");
                } else if (dw) {
                    status_err("Use :wq to commit or :q to abort");
                }
                goto done;
            }

            /* ── write ─────────────────────────────────────────────── */
            if (dw) {
                if (da) {
                    /* :wa / :wqa[!] — write every dirty buffer */
                    int errs = 0;
                    if (E.buf.dirty) {
                        if (!E.buf.filename) {
                            status_err("Buffer %d has no filename",
                                       E.cur_buftab + 1);
                            errs++;
                        } else if (buf_save(&E.buf) != 0) {
                            status_err("Cannot write \"%s\"", E.buf.filename);
                            errs++;
                        } else {
                            E.buftabs[E.cur_buftab].watch_skip++;
                            filewatcher_add(E.cur_buftab);
                        }
                    }
                    for (int i = 0; i < E.num_buftabs; i++) {
                        if (i == E.cur_buftab) continue;
                        Buffer *b = &E.buftabs[i].buf;
                        if (b->dirty) {
                            if (!b->filename) {
                                status_err("Buffer %d has no filename", i + 1);
                                errs++;
                            } else if (buf_save(b) != 0) {
                                status_err("Cannot write \"%s\"", b->filename);
                                errs++;
                            } else {
                                E.buftabs[i].watch_skip++;
                                filewatcher_add(i);
                            }
                        }
                    }
                    if (!errs && !dq) status_msg("All buffers written");
                    if (errs && !df) goto done; /* abort before quit */
                } else {
                    /* :w / :wq[!] — write current buffer */
                    if (E.readonly && !df) {
                        status_err("Read-only mode (use :w! to override)");
                        goto done;
                    }
                    if (arg) {
                        free(E.buf.filename);
                        E.buf.filename = strdup(arg);
                        editor_detect_syntax();
                    }
                    if (!E.buf.filename) {
                        status_err("No filename");
                        goto done;
                    }
                    if (buf_save(&E.buf) == 0) {
                        editor_update_git_signs();
                        E.buftabs[E.cur_buftab].watch_skip++;
                        filewatcher_add(E.cur_buftab);
                        if (!dq) status_msg("\"%s\" written", E.buf.filename);
                    } else {
                        status_err("Cannot write \"%s\"", E.buf.filename);
                        if (!df) goto done;
                    }
                }
            }

            /* ── quit ──────────────────────────────────────────────── */
            if (dq) {
                if (da) {
                    /* :qa / :wqa[!] — quit everything */
                    if (!df) {
                        char dirty_list[96] = "";
                        int  dirty_count = 0, pos = 0;
                        if (E.buf.dirty && !buftab_is_special(&E.buftabs[E.cur_buftab])) {
                            const char *fn = E.buf.filename
                                             ? E.buf.filename : "[No Name]";
                            const char *b = strrchr(fn, '/');
                            pos += snprintf(dirty_list + pos,
                                            sizeof(dirty_list) - pos,
                                            "%s ", b ? b + 1 : fn);
                            dirty_count++;
                        }
                        for (int i = 0; i < E.num_buftabs; i++) {
                            if (i == E.cur_buftab || buftab_is_special(&E.buftabs[i])) continue;
                            if (E.buftabs[i].buf.dirty) {
                                const char *fn = E.buftabs[i].buf.filename
                                                 ? E.buftabs[i].buf.filename
                                                 : "[No Name]";
                                const char *b = strrchr(fn, '/');
                                pos += snprintf(dirty_list + pos,
                                                sizeof(dirty_list) - pos,
                                                "%s ", b ? b + 1 : fn);
                                dirty_count++;
                            }
                        }
                        if (dirty_count > 0) {
                            if (pos > 0 && dirty_list[pos - 1] == ' ')
                                dirty_list[pos - 1] = '\0';
                            status_err("%d unsaved buffer%s (use :qa! to override): %s",
                                       dirty_count,
                                       dirty_count > 1 ? "s" : "",
                                       dirty_list);
                            goto done;
                        }
                    }
                    editor_quit();
                } else if (E.buftabs[E.cur_buftab].kind == BT_TERM) {
                    term_close_buf(E.cur_buftab);
                } else if (E.num_panes > 1) {
                    /* Multiple panes: close this pane; buffer stays in list. */
                    pane_close(E.cur_pane);
                } else {
                    /* Last pane: quit the editor. */
                    if (!df && E.buf.dirty) {
                        status_err("Unsaved changes (use :q! to override)");
                        goto done;
                    }
                    editor_quit();
                }
            }
            goto done;
        }
    }

    /* ── :split / :sp [filename] ──────────────────────────────────── */
    if ((strncmp(cmd, "split", 5) == 0 || strncmp(cmd, "sp", 2) == 0) &&
        (cmd[strncmp(cmd,"split",5)==0 ? 5 : 2] == '\0' ||
         cmd[strncmp(cmd,"split",5)==0 ? 5 : 2] == ' ')) {
        int kw   = (strncmp(cmd, "split", 5) == 0) ? 5 : 2;
        const char *arg = (cmd[kw] == ' ' && cmd[kw+1]) ? cmd + kw + 1 : NULL;
        pane_save_cursor();
        if (pane_split_h(E.cur_pane)) {
            int new_idx = E.cur_pane + 1;
            E.panes[new_idx].cx     = E.cx;
            E.panes[new_idx].cy     = E.cy;
            E.panes[new_idx].rowoff = E.rowoff;
            E.panes[new_idx].coloff = E.coloff;
            pane_activate(new_idx);
            if (arg) {
                open_new_buf(arg);
                E.panes[E.cur_pane].buf_idx = E.cur_buftab;
            }
        }
        goto done;

    /* ── :vsplit / :vs [filename] ──────────────────────────────────── */
    } else if ((strncmp(cmd, "vsplit", 6) == 0 || strncmp(cmd, "vs", 2) == 0) &&
               (cmd[strncmp(cmd,"vsplit",6)==0 ? 6 : 2] == '\0' ||
                cmd[strncmp(cmd,"vsplit",6)==0 ? 6 : 2] == ' ')) {
        int kw   = (strncmp(cmd, "vsplit", 6) == 0) ? 6 : 2;
        const char *arg = (cmd[kw] == ' ' && cmd[kw+1]) ? cmd + kw + 1 : NULL;
        pane_save_cursor();
        if (pane_split_v(E.cur_pane)) {
            int new_idx = E.cur_pane + 1;
            E.panes[new_idx].cx     = E.cx;
            E.panes[new_idx].cy     = E.cy;
            E.panes[new_idx].rowoff = E.rowoff;
            E.panes[new_idx].coloff = E.coloff;
            pane_activate(new_idx);
            if (arg) {
                open_new_buf(arg);
                E.panes[E.cur_pane].buf_idx = E.cur_buftab;
            }
        }
        goto done;

    /* ── :Tree ──────────────────────────────────────────────────────── */
    } else if ((strncmp(cmd, "terminal", 8) == 0 &&
                (cmd[8] == '\0' || cmd[8] == ' ')) ||
               (strncmp(cmd, "term", 4) == 0 &&
                (cmd[4] == '\0' || cmd[4] == ' '))) {
        int kw = (strncmp(cmd, "terminal", 8) == 0 && (cmd[8]=='\0'||cmd[8]==' ')) ? 8 : 4;
        const char *arg = (cmd[kw] == ' ' && cmd[kw+1]) ? cmd + kw + 1 : NULL;
        editor_open_terminal(arg);
        goto done;

    } else if (strcmp(cmd, "Tree") == 0 || strcmp(cmd, "tree") == 0) {
        editor_open_tree_pane();
        goto done;

    /* ── :revisions — undo tree browser ─────────────────────────────── */
    } else if (strcmp(cmd, "revisions") == 0 || strcmp(cmd, "Revisions") == 0
            || strcmp(cmd, "rev") == 0) {
        rev_open_pane();
        goto done;

    /* ── :Gdiff ─────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "Gdiff") == 0 || strcmp(cmd, "gdiff") == 0) {
        diff_open();
        goto done;

    /* ── :Gstage / :Grevert — hunk operations ──────────────────────── */
    } else if (strcmp(cmd, "Gstage") == 0 || strcmp(cmd, "gstage") == 0) {
        hunk_stage();
        goto done;
    } else if (strcmp(cmd, "Grevert") == 0 || strcmp(cmd, "grevert") == 0) {
        hunk_revert();
        goto done;

    /* ── :Gadd [file] ─────────────────────────────────────────────── */
    } else if (strncmp(cmd, "Gadd", 4) == 0 || strncmp(cmd, "gadd", 4) == 0) {
        const char *file = cmd + 4;
        while (*file == ' ') file++;
        if (!*file) file = E.buf.filename;
        if (!file) { status_err("No filename"); goto done; }
        if (git_add(file))
            status_msg("Staged: %s", file);
        else
            status_err("git add failed: %s", file);
        editor_update_git_signs();
        goto done;

    /* ── :Greset [file] ────────────────────────────────────────────── */
    } else if (strncmp(cmd, "Greset", 6) == 0 || strncmp(cmd, "greset", 6) == 0) {
        const char *file = cmd + 6;
        while (*file == ' ') file++;
        if (!*file) file = E.buf.filename;
        if (!file) { status_err("No filename"); goto done; }
        if (git_reset(file))
            status_msg("Unstaged: %s", file);
        else
            status_err("git reset failed: %s", file);
        editor_update_git_signs();
        goto done;

    /* ── :Gstash [message] ────────────────────────────────────────── */
    } else if (strncmp(cmd, "Gstash", 6) == 0 || strncmp(cmd, "gstash", 6) == 0) {
        const char *msg = cmd + 6;
        while (*msg == ' ') msg++;
        if (!*msg) msg = NULL;
        char out[256];
        if (git_stash(msg, out, sizeof(out)))
            status_msg("%s", out);
        else
            status_err("git stash failed");
        editor_update_git_signs();
        goto done;

    /* ── :Gpop ────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "Gpop") == 0 || strcmp(cmd, "gpop") == 0) {
        char out[256];
        if (git_stash_pop(out, sizeof(out)))
            status_msg("%s", out);
        else
            status_err("git stash pop failed");
        editor_update_git_signs();
        goto done;

    /* ── :Glog ─────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "Glog") == 0 || strcmp(cmd, "glog") == 0) {
        log_open();
        goto done;

    /* ── :Gcommit ──────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "Gcommit") == 0 || strcmp(cmd, "gcommit") == 0) {
        commit_open();
        goto done;

    /* ── :Gblame ────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "Gblame") == 0 || strcmp(cmd, "gblame") == 0) {
        blame_open();
        goto done;

    /* ── :Fuzzy ─────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "Fuzzy") == 0 || strcmp(cmd, "fuzzy") == 0) {
        fuzzy_open();
        goto done;

    /* ── :grep [pattern [path]] ─────────────────────────────────────── */
    } else if (strncmp(cmd, "grep ", 5) == 0 || strcmp(cmd, "grep") == 0) {
        const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
        if (!arg || !*arg) { status_err("Usage: :grep pattern [path]"); goto done; }
        /* Split pattern and optional path. */
        char pat[256] = {0}, path[512] = {0};
        const char *sp = strchr(arg, ' ');
        if (sp) {
            int plen = (int)(sp - arg);
            if (plen >= (int)sizeof(pat)) plen = (int)sizeof(pat) - 1;
            strncpy(pat, arg, plen);
            strncpy(path, sp + 1, sizeof(path) - 1);
        } else {
            strncpy(pat, arg, sizeof(pat) - 1);
        }
        QfList *ql = malloc(sizeof(QfList));
        if (!ql) { status_err("Out of memory"); goto done; }
        memset(ql, 0, sizeof(QfList));
        qf_run(ql, pat, path[0] ? path : NULL);
        if (ql->count == 0) {
            qf_free(ql); free(ql);
            status_err("No results for \"%s\"", pat);
            goto done;
        }
        qf_open_pane(ql);
        goto done;

    /* ── :copen ─────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "copen") == 0) {
        if (qf_pane_idx() >= 0) { status_err("Quickfix already open"); goto done; }
        /* Find existing qf buftab to reopen. */
        int qi = -1;
        for (int i = 0; i < E.num_buftabs; i++)
            if (E.buftabs[i].kind == BT_QF) { qi = i; break; }
        if (qi < 0) { status_err("No quickfix results"); goto done; }
        qf_open_pane(E.buftabs[qi].qf);
        goto done;

    /* ── :cclose ────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "cclose") == 0 || strcmp(cmd, "ccl") == 0) {
        if (qf_pane_idx() < 0) { status_err("No quickfix pane"); goto done; }
        qf_close_pane();
        goto done;

    /* ── :cn / :cp ──────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "cn") == 0 || strcmp(cmd, "cnext") == 0) {
        int qpi = qf_pane_idx();
        if (qpi < 0) { status_err("No quickfix list"); goto done; }
        QfList *ql = E.buftabs[E.panes[qpi].buf_idx].qf;
        if (ql && ql->selected < ql->count - 1) qf_jump(ql->selected + 1);
        else status_err("Already at last quickfix entry");
        goto done;

    } else if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "cprev") == 0) {
        int qpi = qf_pane_idx();
        if (qpi < 0) { status_err("No quickfix list"); goto done; }
        QfList *ql = E.buftabs[E.panes[qpi].buf_idx].qf;
        if (ql && ql->selected > 0) qf_jump(ql->selected - 1);
        else status_err("Already at first quickfix entry");
        goto done;

    /* ── :registers / :reg ─────────────────────────────────────────── */
    } else if (strcmp(cmd, "registers") == 0 || strcmp(cmd, "reg") == 0) {
        if (E.num_buftabs >= MAX_BUFS) { status_err("Too many buffers"); goto done; }
        int ridx = E.num_buftabs++;
        memset(&E.buftabs[ridx], 0, sizeof(BufTab));
        buf_init(&E.buftabs[ridx].buf);
        Buffer *rb = &E.buftabs[ridx].buf;
        const char *hdr = "--- Registers ---";
        buf_insert_row(rb, 0, hdr, (int)strlen(hdr));
        int row = 1;
        for (int ri = 0; ri < REG_COUNT; ri++) {
            if (!E.regs[ri].rows || E.regs[ri].numrows == 0) continue;
            char label[3];
            if (ri == 0) { label[0] = '"'; label[1] = '\0'; }
            else if (ri == REG_CLIPBOARD) { label[0] = '+'; label[1] = '\0'; }
            else { label[0] = 'a' + ri - 1; label[1] = '\0'; }
            /* Build a preview: first row, truncated. */
            char line[256];
            const char *preview = E.regs[ri].rows[0];
            int extra = E.regs[ri].numrows > 1 ? E.regs[ri].numrows - 1 : 0;
            int llen;
            if (extra)
                llen = snprintf(line, sizeof(line), " \"%s   %s (+%d lines)",
                                label, preview, extra);
            else
                llen = snprintf(line, sizeof(line), " \"%s   %s", label, preview);
            buf_insert_row(rb, row++, line, llen);
        }
        /* Show macro registers with content. */
        int has_macros = 0;
        for (int mi = 0; mi < MACRO_REGS; mi++) {
            if (!E.macros[mi].keys || E.macros[mi].len == 0) continue;
            if (!has_macros) {
                const char *mhdr = "--- Macros ---";
                buf_insert_row(rb, row++, mhdr, (int)strlen(mhdr));
                has_macros = 1;
            }
            char line[256];
            int llen = snprintf(line, sizeof(line), " @%c   %d keys",
                                'a' + mi, E.macros[mi].len);
            buf_insert_row(rb, row++, line, llen);
        }
        if (row == 1) {
            const char *empty = " (all registers empty)";
            buf_insert_row(rb, 1, empty, (int)strlen(empty));
        }
        rb->dirty = 0;
        rb->filename = strdup("[Registers]");
        pane_save_cursor();
        editor_buf_save(E.cur_buftab);
        E.panes[E.cur_pane].buf_idx = ridx;
        E.cur_buftab = ridx;
        editor_buf_restore(ridx);
        E.syntax = NULL;
        E.mode = MODE_NORMAL;
        E.cy = 0; E.cx = 0;
        E.rowoff = 0; E.coloff = 0;
        goto done;

    /* ── :close ────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "close") == 0) {
        pane_close(E.cur_pane);
        goto done;

    /* ── :only ─────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "only") == 0) {
        int keep = E.cur_pane;
        E.panes[0]      = E.panes[keep];
        E.num_panes     = 1;
        E.cur_pane      = 0;
        E.panes[0].top    = 1;
        E.panes[0].left   = 1;
        E.panes[0].height = E.term_rows - 2;
        E.panes[0].width  = E.term_cols;
        E.screenrows      = E.panes[0].height;
        E.screencols      = E.panes[0].width;
        goto done;
    }

    /* ── :e [!] [filename] ─────────────────────────────────────────── */
    if ((cmd[0] == 'e') &&
        (cmd[1] == '\0' || cmd[1] == '!' || cmd[1] == ' ')) {
        int force = (cmd[1] == '!');
        const char *arg = force ? (cmd[2] == ' ' && cmd[3] ? cmd + 3 : NULL)
                                : (cmd[1] == ' ' && cmd[2]  ? cmd + 2 : NULL);

        if (E.buf.dirty && !force) {
            status_err("Unsaved changes (use :e! to override)");
            goto done;
        }
        const char *target = arg ? arg : E.buf.filename;
        if (!target) {
            status_err("No filename");
            goto done;
        }
        char *fname = strdup(target);

        /* If another pane is showing this buffer, open the file in a new
           buftab so that pane is unaffected; otherwise open in-place. */
        int buf_shared = 0;
        for (int i = 0; i < E.num_panes; i++) {
            if (i != E.cur_pane && E.panes[i].buf_idx == E.cur_buftab)
                { buf_shared = 1; break; }
        }

        if (buf_shared) {
            if (open_new_buf(fname))
                E.panes[E.cur_pane].buf_idx = E.cur_buftab;
        } else {
            buf_free(&E.buf);
            undo_tree_free(&E.undo_tree);
            if (E.has_pre_insert) {
                undo_state_free(&E.pre_insert_snapshot);
                E.has_pre_insert = 0;
            }
            la_free();
            E.cx = 0; E.cy = 0;
            E.rowoff = 0; E.coloff = 0;
            buf_open(&E.buf, fname);
            editor_detect_syntax();
            editor_update_git_signs();
            filewatcher_add(E.cur_buftab);
            status_msg("\"%s\"", E.buf.filename);
        }
        free(fname);

    /* ── :bnew [filename] ──────────────────────────────────────────── */
    } else if (strcmp(cmd, "bnew") == 0 ||
               (cmd[0] == 'b' && cmd[1] == 'n' && cmd[2] == 'e' &&
                cmd[3] == 'w' && cmd[4] == ' ' && cmd[5])) {
        open_new_buf(cmd[4] == ' ' ? cmd + 5 : NULL);

    /* ── :bn / :bp / :b N ──────────────────────────────────────────── */
    } else if (strcmp(cmd, "bn") == 0) {
        int next = -1;
        for (int i = 1; i <= E.num_buftabs && next < 0; i++) {
            int idx = (E.cur_buftab + i) % E.num_buftabs;
            if (!buftab_is_special(&E.buftabs[idx])) next = idx;
        }
        if (next < 0) status_err("No other buffer");
        else { switch_to_buf(next); status_msg("Buffer %d/%d", E.cur_buftab + 1, E.num_buftabs); }

    } else if (strcmp(cmd, "bp") == 0) {
        int next = -1;
        for (int i = 1; i <= E.num_buftabs && next < 0; i++) {
            int idx = (E.cur_buftab - i + E.num_buftabs) % E.num_buftabs;
            if (!buftab_is_special(&E.buftabs[idx])) next = idx;
        }
        if (next < 0) status_err("No other buffer");
        else { switch_to_buf(next); status_msg("Buffer %d/%d", E.cur_buftab + 1, E.num_buftabs); }

    } else if (cmd[0] == 'b' && cmd[1] == ' ' && cmd[2] >= '1') {
        int n = atoi(cmd + 2) - 1;
        if (n < 0 || n >= E.num_buftabs)
            status_err("No buffer %d", n + 1);
        else if (buftab_is_special(&E.buftabs[n]))
            status_err("Not a content buffer");
        else {
            switch_to_buf(n);
            status_msg("Buffer %d/%d", E.cur_buftab + 1, E.num_buftabs);
        }

    /* ── :buffers — fuzzy buffer picker ───────────────────────────── */
    } else if (strcmp(cmd, "buffers") == 0) {
        fuzzy_open_buffers();
        goto done;

    /* ── :ls ───────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "ls") == 0) {
        char msg[128];
        int pos = 0;
        for (int i = 0; i < E.num_buftabs && pos < (int)sizeof(msg) - 1; i++) {
            if (buftab_is_special(&E.buftabs[i])) continue;
            const char *fn = (i == E.cur_buftab)
                ? (E.buf.filename          ? E.buf.filename          : "[No Name]")
                : (E.buftabs[i].buf.filename ? E.buftabs[i].buf.filename : "[No Name]");
            int  dirty = (i == E.cur_buftab) ? E.buf.dirty
                                             : E.buftabs[i].buf.dirty;
            const char *base = strrchr(fn, '/');
            base = base ? base + 1 : fn;
            if (i == E.cur_buftab)
                pos += snprintf(msg + pos, sizeof(msg) - pos,
                                "[%d:%s%s] ", i + 1, base, dirty ? "+" : "");
            else
                pos += snprintf(msg + pos, sizeof(msg) - pos,
                                "%d:%s%s ", i + 1, base, dirty ? "+" : "");
        }
        if (pos > 0 && msg[pos - 1] == ' ') msg[pos - 1] = '\0';
        status_msg("%s", msg);

    /* ── :set ──────────────────────────────────────────────────────── */
    } else if (strcmp(cmd, "set nu") == 0) {
        E.opts.line_numbers = 1;
        status_msg("Line numbers on");

    } else if (strcmp(cmd, "set nonu") == 0) {
        E.opts.line_numbers = 0;
        status_msg("Line numbers off");

    /* ── :mksession [file] ────────────────────────────────────────── */
    } else if (strncmp(cmd, "mksession", 9) == 0 &&
               (cmd[9] == '\0' || cmd[9] == ' ')) {
        const char *path = (cmd[9] == ' ' && cmd[10]) ? cmd + 10 : "Session.qe";
        editor_save_session(path);
        goto done;

    /* ── :source [file] — restore session ─────────────────────────── */
    } else if (strncmp(cmd, "source", 6) == 0 &&
               (cmd[6] == '\0' || cmd[6] == ' ')) {
        const char *path = (cmd[6] == ' ' && cmd[7]) ? cmd + 7 : "Session.qe";
        editor_load_session(path);
        goto done;

    } else {
        /* ── :s / :%s / :N,Ms — substitute ──────────────────────────── */
        const char *pat; int pat_len;
        const char *rep; int rep_len;
        int r0, r1, global;

        if (!subst_parse(cmd, &r0, &r1, &pat, &pat_len,
                         &rep, &rep_len, &global)) {
            if (cmd[0] != '\0')
                status_err("Not a command: %.100s", cmd);
            goto done;
        }

        if (E.buf.numrows == 0) goto done;
        if (r0 < 0) r0 = 0;
        if (r1 >= E.buf.numrows) r1 = E.buf.numrows - 1;
        if (r0 > r1) { status_err("Invalid range"); goto done; }

        /* Empty pattern reuses last search */
        if (pat_len == 0) {
            if (!E.last_search_valid) { status_err("No previous pattern"); goto done; }
            pat = E.last_query.pattern;
            pat_len = E.last_query.pat_len;
        }

        SearchQuery q;
        search_query_init_literal(&q, pat, pat_len);

        /* Update last search so n/N work after a substitution */
        E.last_query       = q;
        E.last_search_valid = 1;
        int slen = pat_len < (int)sizeof(E.searchbuf) - 1
                   ? pat_len : (int)sizeof(E.searchbuf) - 1;
        memcpy(E.searchbuf, pat, slen);
        E.searchbuf[slen] = '\0';
        E.searchlen = slen;

        /* Copy replacement out of cmdbuf before it's cleared */
        char rep_copy[512];
        int rlen = rep_len < (int)sizeof(rep_copy) - 1
                   ? rep_len : (int)sizeof(rep_copy) - 1;
        memcpy(rep_copy, rep, rlen);
        rep_copy[rlen] = '\0';

        push_undo("substitute");

        int total = 0;
        int first_changed = -1;
        for (int i = r0; i <= r1; i++) {
            int n = subst_row(&E.buf.rows[i], &q, rep_copy, rlen, global);
            if (n > 0) {
                total += n;
                E.buf.dirty++;
                buf_mark_hl_dirty(&E.buf, i);
                if (first_changed < 0) first_changed = i;
            }
        }

        if (total == 0) {
            status_err("Pattern not found: %s", q.pattern);
        } else {
            status_msg("%d substitution%s", total, total == 1 ? "" : "s");
            E.cy = first_changed;
            E.cx = 0;
        }
    }

done:
    if (E.mode != MODE_FUZZY)  /* fuzzy_open() sets MODE_FUZZY — don't clobber */
        E.mode = MODE_NORMAL;
    E.cmdbuf[0] = '\0';
    E.cmdlen    = 0;
}

/* ── Normal mode ─────────────────────────────────────────────────────── */

/* Begin an insert session: snapshot state for undo, then switch mode.
   entry is the key that triggered insert ('i', 'a', 'A', 'o', 'O'). */
static void enter_insert_mode(char entry) {
    if (E.readonly) { status_err("Read-only mode"); return; }
    E.pre_insert_snapshot = editor_capture_state();
    E.pre_insert_dirty    = E.buf.dirty;
    E.has_pre_insert      = 1;
    E.mode                = MODE_INSERT;
    E.insert_entry        = entry;
    insert_rec_reset();
}

static void editor_earlier(void) {
    if (!E.undo_tree.root) { status_err("No undo history"); return; }
    if (E.undo_tree.current) {
        undo_state_free(&E.undo_tree.current->state);
        E.undo_tree.current->state = editor_capture_state();
    }
    UndoNode *prev = undo_tree_earlier(&E.undo_tree);
    if (!prev) { status_err("Already at oldest change"); return; }
    editor_restore_state(&prev->state);
}

static void editor_later(void) {
    if (!E.undo_tree.root || !E.undo_tree.current) {
        status_err("No undo history"); return;
    }
    undo_state_free(&E.undo_tree.current->state);
    E.undo_tree.current->state = editor_capture_state();
    UndoNode *next = undo_tree_later(&E.undo_tree);
    if (!next) { status_err("Already at newest change"); return; }
    editor_restore_state(&next->state);
}

static void editor_undo(void) {
    if (!E.undo_tree.root) {
        status_err("Already at oldest change");
        return;
    }
    /* Save current buffer state into the current node before moving. */
    if (E.undo_tree.current) {
        undo_state_free(&E.undo_tree.current->state);
        E.undo_tree.current->state = editor_capture_state();
    }
    UndoNode *prev = undo_tree_undo(&E.undo_tree);
    if (!prev) {
        status_err("Already at oldest change");
        return;
    }
    editor_restore_state(&prev->state);
}

static void editor_redo(void) {
    if (!E.undo_tree.root || !E.undo_tree.current) {
        status_err("Already at newest change");
        return;
    }
    /* Save current buffer state into the current node before moving. */
    undo_state_free(&E.undo_tree.current->state);
    E.undo_tree.current->state = editor_capture_state();

    UndoNode *next = undo_tree_redo(&E.undo_tree);
    if (!next) {
        status_err("Already at newest change");
        return;
    }
    editor_restore_state(&next->state);
}

static void editor_process_normal(int c) {
    E.statusmsg[0] = '\0';  /* clear one-shot message on any keypress */
    E.statusmsg_is_error = 0;

    /* Ctrl-W second-key dispatch. */
    if (E.pending_ctrlw) {
        E.pending_ctrlw = 0;
        switch (c) {
            case 'h': case 'j': case 'k': case 'l':
                pane_navigate(c);
                break;
            case 0x17:  /* Ctrl-W Ctrl-W = cycle */
                if (E.num_panes > 1)
                    pane_activate((E.cur_pane + 1) % E.num_panes);
                break;
            case 'c': case 'q':
                pane_close(E.cur_pane);
                break;
            case '+': case '-': {
                /* Vertical resize: change height of horizontal splits. */
                Pane *cur = &E.panes[E.cur_pane];
                int delta = (c == '+') ? 1 : -1;
                int n = E.count > 0 ? E.count : 1;
                E.count = 0;
                delta *= n;
                /* Find a neighbor pane above or below to steal/give rows. */
                int neighbor = -1;
                for (int i = 0; i < E.num_panes; i++) {
                    if (i == E.cur_pane) continue;
                    Pane *np = &E.panes[i];
                    /* Same column range and vertically adjacent? */
                    if (np->left == cur->left && np->width == cur->width) {
                        if (np->top + np->height + 1 == cur->top ||
                            cur->top + cur->height + 1 == np->top) {
                            neighbor = i; break;
                        }
                    }
                }
                if (neighbor >= 0) {
                    Pane *np = &E.panes[neighbor];
                    if (cur->height + delta >= 2 && np->height - delta >= 2) {
                        cur->height += delta;
                        np->height -= delta;
                        if (np->top > cur->top)
                            np->top += delta;
                        else
                            cur->top -= delta;
                        E.screenrows = cur->height;
                    }
                }
                break;
            }
            case '<': case '>': {
                /* Horizontal resize: change width of vertical splits. */
                Pane *cur = &E.panes[E.cur_pane];
                int delta = (c == '>') ? 1 : -1;
                int n = E.count > 0 ? E.count : 1;
                E.count = 0;
                delta *= n;
                int neighbor = -1;
                for (int i = 0; i < E.num_panes; i++) {
                    if (i == E.cur_pane) continue;
                    Pane *np = &E.panes[i];
                    if (np->top == cur->top && np->height == cur->height) {
                        if (np->left + np->width + 1 == cur->left ||
                            cur->left + cur->width + 1 == np->left) {
                            neighbor = i; break;
                        }
                    }
                }
                if (neighbor >= 0) {
                    Pane *np = &E.panes[neighbor];
                    if (cur->width + delta >= 5 && np->width - delta >= 5) {
                        cur->width += delta;
                        np->width -= delta;
                        if (np->left > cur->left)
                            np->left += delta;
                        else
                            cur->left -= delta;
                        E.screencols = cur->width;
                    }
                }
                break;
            }
            case '=': {
                /* Equalize pane sizes. */
                E.count = 0;
                /* Equalize widths of panes at the same vertical level. */
                int total_w = 0, count_h = 0;
                Pane *cur = &E.panes[E.cur_pane];
                for (int i = 0; i < E.num_panes; i++) {
                    Pane *p = &E.panes[i];
                    if (p->top == cur->top && p->height == cur->height) {
                        total_w += p->width + 1;
                        count_h++;
                    }
                }
                if (count_h > 1) {
                    total_w--;  /* last pane has no separator */
                    int each = total_w / count_h;
                    int left = cur->left;
                    /* Find leftmost pane in this row. */
                    for (int i = 0; i < E.num_panes; i++) {
                        Pane *p = &E.panes[i];
                        if (p->top == cur->top && p->height == cur->height)
                            if (p->left < left) left = p->left;
                    }
                    int col = left;
                    int assigned = 0;
                    for (int i = 0; i < E.num_panes; i++) {
                        Pane *p = &E.panes[i];
                        if (p->top != cur->top || p->height != cur->height) continue;
                        p->left = col;
                        assigned++;
                        if (assigned == count_h)
                            p->width = total_w - (col - left);
                        else
                            p->width = each;
                        col += p->width + 1;
                    }
                    E.screencols = E.panes[E.cur_pane].width;
                }
                break;
            }
            default:
                status_err("Unknown Ctrl-W command");
                break;
        }
        return;
    }

    /* g second-key dispatch. */
    if (E.pending_g) {
        E.pending_g = 0;
        if (c == 'g') {
            /* gg: go to first line (or line N with count prefix) */
            jump_push();
            int n = E.count > 0 ? E.count : 1;
            E.count = 0;
            E.cy = n - 1;
            if (E.buf.numrows > 0 && E.cy >= E.buf.numrows)
                E.cy = E.buf.numrows - 1;
            s_vertical_move = 1;
            apply_preferred_col();
        } else if (c == 'd') {
            /* gd: go to local definition — search for word under cursor from top. */
            if (E.cy < E.buf.numrows && E.buf.numrows > 0) {
                Row *row = &E.buf.rows[E.cy];
                int start = E.cx, end = E.cx;
                if (start < row->len) {
                    /* Expand word boundaries. */
                    while (start > 0 && (isalnum((unsigned char)row->chars[start-1]) || row->chars[start-1] == '_'))
                        start--;
                    while (end < row->len && (isalnum((unsigned char)row->chars[end]) || row->chars[end] == '_'))
                        end++;
                    if (end > start) {
                        int wlen = end - start;
                        char word[256];
                        if (wlen > 255) wlen = 255;
                        memcpy(word, row->chars + start, wlen);
                        word[wlen] = '\0';
                        /* Search from line 0 for first occurrence. */
                        for (int r = 0; r < E.buf.numrows; r++) {
                            Row *sr = &E.buf.rows[r];
                            char *p = sr->chars;
                            while ((p = strstr(p, word)) != NULL) {
                                int col = (int)(p - sr->chars);
                                /* Ensure it's a whole word match. */
                                int before_ok = (col == 0 || (!isalnum((unsigned char)p[-1]) && p[-1] != '_'));
                                int after_ok = (col + wlen >= sr->len || (!isalnum((unsigned char)p[wlen]) && p[wlen] != '_'));
                                if (before_ok && after_ok && !(r == E.cy && col == start)) {
                                    jump_push();
                                    E.cy = r;
                                    E.cx = col;
                                    goto gd_done;
                                }
                                p++;
                            }
                        }
                        snprintf(E.statusmsg, sizeof(E.statusmsg), "Definition not found: %s", word);
                        E.statusmsg_is_error = 1;
                        gd_done: ;
                    }
                }
            }
            E.count = 0;
        } else if (c == '-') {
            /* g-: earlier state (chronological, crosses branches) */
            int n = E.count > 0 ? E.count : 1;
            E.count = 0;
            for (int i = 0; i < n; i++) editor_earlier();
        } else if (c == '+') {
            /* g+: later state (chronological, crosses branches) */
            int n = E.count > 0 ? E.count : 1;
            E.count = 0;
            for (int i = 0; i < n; i++) editor_later();
        }
        return;
    }

    /* z second-key dispatch (fold commands). */
    if (E.pending_z) {
        E.pending_z = 0;
        E.count = 0;
        switch (c) {
            case 'c':  /* zc — close fold at cursor */
                fold_close(E.cy);
                break;
            case 'o':  /* zo — open fold at cursor */
                fold_open(E.cy);
                break;
            case 'a':  /* za — toggle fold at cursor */
                if (E.buf.folds && E.cy < E.buf.folds_cap && E.buf.folds[E.cy])
                    fold_open(E.cy);
                else
                    fold_close(E.cy);
                break;
            case 'M':  /* zM — close all folds */
                buf_ensure_folds(&E.buf);
                for (int r = 0; r < E.buf.numrows; r++)
                    if (fold_can_fold(r)) E.buf.folds[r] = 1;
                fold_fix_cursor();
                break;
            case 'R':  /* zR — open all folds */
                if (E.buf.folds)
                    memset(E.buf.folds, 0, E.buf.folds_cap * sizeof(int8_t));
                break;
            default:
                status_err("Unknown z command");
                break;
        }
        return;
    }

    /* ] / [ second-key dispatch (hunk navigation). */
    if (E.pending_bracket) {
        int dir = E.pending_bracket;   /* ']' or '[' */
        E.pending_bracket = 0;
        if (c == 'c') {
            /* ]c / [c — jump to next/previous git hunk. */
            if (!E.buf.git_signs) { status_err("No git diff info"); return; }
            int n = E.count > 0 ? E.count : 1;
            E.count = 0;
            int row = E.cy;
            if (dir == ']') {
                for (int rep = 0; rep < n; rep++) {
                    /* Skip current hunk. */
                    while (row < E.buf.numrows - 1 &&
                           E.buf.git_signs[row] != GIT_SIGN_NONE) row++;
                    /* Find start of next hunk. */
                    while (row < E.buf.numrows - 1 &&
                           E.buf.git_signs[row] == GIT_SIGN_NONE) row++;
                }
            } else {
                for (int rep = 0; rep < n; rep++) {
                    /* Step back off current hunk. */
                    while (row > 0 &&
                           E.buf.git_signs[row] != GIT_SIGN_NONE) row--;
                    /* Scan back to find a hunk. */
                    while (row > 0 &&
                           E.buf.git_signs[row] == GIT_SIGN_NONE) row--;
                    /* Walk to start of that hunk. */
                    while (row > 0 &&
                           E.buf.git_signs[row - 1] != GIT_SIGN_NONE) row--;
                }
            }
            if (row != E.cy && E.buf.git_signs[row] != GIT_SIGN_NONE) {
                E.cy = row;
                E.cx = 0;
                s_vertical_move = 1;
                apply_preferred_col();
            }
        }
        return;
    }

    /* Pending <leader>h — git hunk operation: s=stage, r=revert. */
    if (E.pending_leader_h) {
        E.pending_leader_h = 0;
        if (c == 's')      hunk_stage();
        else if (c == 'r') hunk_revert();
        else               status_err("Unknown hunk command: h%c", (char)c);
        return;
    }

    /* Pending <leader>g — git view: b=blame, l=log, d=diff, c=commit. */
    if (E.pending_leader_g) {
        E.pending_leader_g = 0;
        if      (c == 'b') blame_open();
        else if (c == 'l') log_open();
        else if (c == 'd') diff_open();
        else if (c == 'c') commit_open();
        else               status_err("Unknown git command: g%c", (char)c);
        return;
    }

    /* Pending leader key sequence. */
    if (E.pending_leader) {
        E.pending_leader = 0;
        if (c == 'h') { E.pending_leader_h = 1; return; }
        if (c == 'g') { E.pending_leader_g = 1; return; }
        if (c == 'b') { fuzzy_open_buffers();     return; }
        if (c == 't') { editor_open_fuzzy();      return; }
        if (c == 'r') { rev_open_pane();           return; }
        if (c == 'x') { editor_open_terminal(NULL); return; }
        if (!lua_bridge_call_key(MODE_NORMAL, LEADER_BASE + c))
            status_err("No mapping for <leader>%c", (char)c);
        return;
    }

    /* Pending mark operation: next key is the mark letter. */
    if (E.pending_mark) {
        int pm = E.pending_mark;
        E.pending_mark = 0;
        if (c >= 'a' && c <= 'z') {
            int idx = c - 'a';
            if (pm == 'm') {
                E.marks[idx].valid   = 1;
                E.marks[idx].buf_idx = E.cur_buftab;
                E.marks[idx].row     = E.cy;
                E.marks[idx].col     = E.cx;
            } else {
                if (!E.marks[idx].valid) {
                    status_err("Mark not set");
                } else {
                    if (E.marks[idx].buf_idx != E.cur_buftab)
                        switch_to_buf(E.marks[idx].buf_idx);
                    else
                        jump_push();
                    int r = E.marks[idx].row;
                    if (r >= E.buf.numrows) r = E.buf.numrows > 0 ? E.buf.numrows - 1 : 0;
                    E.cy = r;
                    if (pm == '`') {
                        int mc = E.marks[idx].col;
                        if (E.buf.numrows > 0 && E.cy < E.buf.numrows) {
                            if (mc >= E.buf.rows[E.cy].len)
                                mc = E.buf.rows[E.cy].len > 0 ? E.buf.rows[E.cy].len - 1 : 0;
                        }
                        E.cx = mc;
                    } else { /* '\'' — first non-blank of line */
                        E.cx = 0;
                        if (E.buf.numrows > 0 && E.cy < E.buf.numrows) {
                            Row *row = &E.buf.rows[E.cy];
                            while (E.cx < row->len &&
                                   (row->chars[E.cx] == ' ' || row->chars[E.cx] == '\t'))
                                E.cx++;
                            if (E.cx >= row->len && row->len > 0) E.cx = row->len - 1;
                        }
                    }
                }
            }
        }
        return;
    }

    /* Terminal pane (normal mode): limited commands, i/a resume terminal. */
    if (E.buftabs[E.cur_buftab].kind == BT_TERM) {
        switch (c) {
            case 'i': case 'a': case 'A':
                E.mode = MODE_INSERT;
                break;
            case ':':
                E.mode = MODE_COMMAND;
                E.cmdbuf[0] = '\0'; E.cmdlen = 0;
                break;
            case 0x17: /* Ctrl-W */
                E.pending_ctrlw = 1;
                break;
            case 'p': {
                /* Paste from register into terminal. */
                int ri = (E.pending_reg >= 0) ? E.pending_reg : 0;
                E.pending_reg = -1;
                if (E.regs[ri].numrows > 0 && E.buftabs[E.cur_buftab].term) {
                    TermState *ts = E.buftabs[E.cur_buftab].term;
                    for (int r = 0; r < E.regs[ri].numrows; r++) {
                        if (r > 0) term_emu_write(ts, "\n", 1);
                        term_emu_write(ts, E.regs[ri].rows[r],
                                       (int)strlen(E.regs[ri].rows[r]));
                    }
                }
                break;
            }
            default:
                break;
        }
        return;
    }

    /* Tree pane: route all keys to tree handler. */
    if (E.buftabs[E.cur_buftab].kind == BT_TREE) {
        tree_handle_key(c);
        return;
    }

    /* Quickfix pane: route all keys to qf handler. */
    if (E.buftabs[E.cur_buftab].kind == BT_QF) {
        qf_handle_key(c);
        return;
    }

    /* Blame pane: route all keys to blame handler. */
    if (E.buftabs[E.cur_buftab].kind == BT_BLAME) {
        blame_handle_key(c);
        return;
    }

    /* Log pane: route all keys to log handler. */
    if (E.buftabs[E.cur_buftab].kind == BT_LOG) {
        log_handle_key(c);
        return;
    }

    /* Revisions pane: route all keys to revisions handler. */
    if (E.buftabs[E.cur_buftab].kind == BT_REVISIONS) {
        rev_handle_key(c);
        return;
    }

    /* Diff pane (HEAD): read-only, navigation + q to close. */
    if (E.buftabs[E.cur_buftab].kind == BT_DIFF) {
        if (c == 'q' || c == '\x1b') { diff_close(); return; }
        if ((c == ARROW_DOWN || c == 'j') && E.cy < E.buf.numrows - 1) {
            E.cy++;
            if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
        } else if ((c == ARROW_UP || c == 'k') && E.cy > 0) {
            E.cy--;
            if (E.cy < E.rowoff) E.rowoff = E.cy;
        } else if (c == 'G') {
            E.cy = E.buf.numrows - 1;
        } else if (c == 'g') {
            E.cy = 0; E.rowoff = 0;
        }
        /* Sync scroll to source pane. */
        int src_buf = E.buftabs[E.cur_buftab].diff_source_buf;
        for (int i = 0; i < E.num_panes; i++) {
            if (E.panes[i].buf_idx == src_buf) {
                E.panes[i].cy     = E.cy;
                E.panes[i].rowoff = E.rowoff;
                break;
            }
        }
        return;
    }

    if (lua_bridge_call_key(MODE_NORMAL, c)) return;

    /* Leader key? Set pending and wait for the next key. */
    if (c == (unsigned char)E.leader_char) {
        E.count = 0;
        E.pending_leader = 1;
        return;
    }

    /* Accumulate count prefix digits: 1-9 always; 0 only if count already started. */
    if ((c >= '1' && c <= '9') || (c == '0' && E.count > 0)) {
        E.count = E.count * 10 + (c - '0');
        return;
    }

    /* Register prefix: "a selects register a for the next yank/delete/paste.
       Reads the register name inline, sets pending_reg, then re-enters this
       function with the following key so it's processed in the same context. */
    if (c == '"') {
        int r = editor_read_key();
        if (r >= 'a' && r <= 'z') {
            E.pending_reg = r - 'a' + 1;
        } else if (r == '+') {
            E.pending_reg = REG_CLIPBOARD;
        } else {
            return;
        }
        int next = editor_read_key();
        editor_process_normal(next);
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
    if (c == 'd' || c == 'y' || c == 'c' || c == '>' || c == '<') {
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

        /* --- Ctrl-O / Ctrl-I: jump list navigation --- */
        case 0x0f:  /* Ctrl-O */
            jump_back();
            break;
        case '\t':  /* Ctrl-I */
            jump_forward();
            break;

        /* --- Ctrl-W: window command prefix --- */
        case 0x17:
            E.pending_ctrlw = 1;
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
            enter_insert_mode('i');
            break;

        case 'a':
            if (E.cy < E.buf.numrows && E.cx < E.buf.rows[E.cy].len)
                E.cx++;
            enter_insert_mode('a');
            break;

        case 'A':
            if (E.cy < E.buf.numrows)
                E.cx = E.buf.rows[E.cy].len;
            enter_insert_mode('A');
            break;

        case 'o': {
            char ind[256]; int ind_len = 0, extra = 0;
            if (E.opts.autoindent && E.cy < E.buf.numrows) {
                const Row *r = &E.buf.rows[E.cy];
                ind_len = row_indent(r, ind, sizeof(ind));
                if (is_open_bracket(last_nonws(r, r->len)))
                    extra = E.opts.tabwidth;
            }
            enter_insert_mode('o');
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
            enter_insert_mode('O');
            buf_insert_row(&E.buf, E.cy, ind, ind_len);
            E.cx = ind_len;
            break;
        }

        /* --- x: delete n chars under/after cursor --- */
        case 'x':
            if (E.cy < E.buf.numrows) {
                Row *row = &E.buf.rows[E.cy];
                if (E.cx < row->len) {
                    int del = n;
                    if (E.cx + del > row->len) del = row->len - E.cx;
                    yank_set_chars(E.cy, E.cx, E.cx + del);
                    push_undo("delete char");
                    for (int i = 0; i < del; i++)
                        buf_row_delete_char(row, E.cx);
                    E.buf.dirty++;
                    buf_mark_hl_dirty(&E.buf, E.cy);
                    if (E.cx > 0 && E.cx >= row->len) E.cx--;
                    if (!E.is_replaying) {
                        la_free();
                        E.last_action.type  = LA_X;
                        E.last_action.count = n;
                    }
                }
            }
            break;

        /* --- undo / redo (n times) --- */
        case 'u':
            for (int i = 0; i < n; i++) editor_undo();
            editor_update_git_signs();
            break;

        case 0x12:  /* Ctrl-R = redo */
            for (int i = 0; i < n; i++) editor_redo();
            editor_update_git_signs();
            break;

        /* --- r: replace character under cursor --- */
        case 'r': {
            int rk = editor_read_key();
            if (rk == '\x1b') break;
            if (rk < 32 || rk > 126) break;
            if (E.cy < E.buf.numrows) {
                Row *row = &E.buf.rows[E.cy];
                if (E.cx < row->len) {
                    push_undo("replace");
                    for (int i = 0; i < n && E.cx + i < row->len; i++)
                        row->chars[E.cx + i] = (char)rk;
                    E.buf.dirty++;
                    buf_mark_hl_dirty(&E.buf, E.cy);
                    if (!E.is_replaying) {
                        la_free();
                        E.last_action.type   = LA_REPLACE;
                        E.last_action.count  = n;
                        E.last_action.motion = (char)rk;
                    }
                }
            }
            break;
        }

        /* --- search repeat (n times) --- */
        case 'n': {
            if (!E.last_search_valid) break;
            jump_push();
            for (int i = 0; i < n; i++) {
                int row, col;
                if (search_find_next(&E.last_query, &E.buf, E.cy, E.cx, &row, &col)) {
                    E.cy = row; E.cx = col;
                } else {
                    status_err("Pattern not found");
                    break;
                }
            }
            break;
        }

        case 'N': {
            if (!E.last_search_valid) break;
            jump_push();
            for (int i = 0; i < n; i++) {
                int row, col;
                if (search_find_prev(&E.last_query, &E.buf, E.cy, E.cx, &row, &col)) {
                    E.cy = row; E.cx = col;
                } else {
                    status_err("Pattern not found");
                    break;
                }
            }
            break;
        }

        /* --- * / # : search word under cursor forward / backward --- */
        case '*':
        case '#': {
            if (E.buf.numrows == 0 || E.cy >= E.buf.numrows) break;
            Row *row = &E.buf.rows[E.cy];
            if (E.cx >= row->len) break;

            /* Find word boundaries around cx. */
            int ws = E.cx;
            while (ws > 0 && (isalnum((unsigned char)row->chars[ws-1])
                               || row->chars[ws-1] == '_'))
                ws--;
            int we = E.cx;
            while (we < row->len && (isalnum((unsigned char)row->chars[we])
                                     || row->chars[we] == '_'))
                we++;
            if (we == ws) break;  /* not on a word character */

            int wlen = we - ws;
            if (wlen >= (int)sizeof(E.searchbuf)) wlen = sizeof(E.searchbuf) - 1;
            memcpy(E.searchbuf, &row->chars[ws], wlen);
            E.searchbuf[wlen] = '\0';
            E.searchlen = wlen;
            search_query_init_literal(&E.last_query, E.searchbuf, E.searchlen);
            E.last_search_valid = 1;

            int found_row, found_col;
            int found = (c == '*')
                ? search_find_next(&E.last_query, &E.buf, E.cy, E.cx, &found_row, &found_col)
                : search_find_prev(&E.last_query, &E.buf, E.cy, E.cx, &found_row, &found_col);
            if (found) {
                jump_push();
                E.cy = found_row;
                E.cx = found_col;
            } else {
                status_err("Pattern not found: %s", E.searchbuf);
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

        case 0x16:  /* Ctrl-V: block visual mode */
            E.visual_anchor_row = E.cy;
            E.visual_anchor_col = E.cx;
            E.mode = MODE_VISUAL_BLOCK;
            break;

        /* --- paste n times --- */
        case 'p':
            for (int i = 0; i < n; i++) editor_paste(0);
            if (!E.is_replaying) {
                la_free();
                E.last_action.type   = LA_PASTE;
                E.last_action.count  = n;
                E.last_action.before = 0;
            }
            break;

        case 'P':
            for (int i = 0; i < n; i++) editor_paste(1);
            if (!E.is_replaying) {
                la_free();
                E.last_action.type   = LA_PASTE;
                E.last_action.count  = n;
                E.last_action.before = 1;
            }
            break;

        /* --- . repeat last change --- */
        case '.': {
            if (E.last_action.type == LA_NONE) break;
            int use_n = raw_count > 0 ? raw_count : E.last_action.count;
            E.is_replaying = 1;
            switch (E.last_action.type) {
                case LA_X: {
                    if (E.cy >= E.buf.numrows) break;
                    Row *row = &E.buf.rows[E.cy];
                    if (E.cx >= row->len) break;
                    push_undo("delete");
                    int del = use_n;
                    if (E.cx + del > row->len) del = row->len - E.cx;
                    for (int i = 0; i < del; i++)
                        buf_row_delete_char(row, E.cx);
                    E.buf.dirty++;
                    buf_mark_hl_dirty(&E.buf, E.cy);
                    if (E.cx > 0 && E.cx >= row->len) E.cx--;
                    break;
                }
                case LA_INDENT:
                    push_undo("indent");
                    indent_lines(E.cy, E.cy + use_n - 1, (char)E.last_action.motion);
                    E.cx = 0;
                    break;
                case LA_REPLACE: {
                    if (E.cy >= E.buf.numrows) break;
                    Row *row = &E.buf.rows[E.cy];
                    if (E.cx >= row->len) break;
                    push_undo("replace");
                    char rch = (char)E.last_action.motion;
                    for (int i = 0; i < use_n && E.cx + i < row->len; i++)
                        row->chars[E.cx + i] = rch;
                    E.buf.dirty++;
                    buf_mark_hl_dirty(&E.buf, E.cy);
                    break;
                }
                case LA_DELETE:
                    editor_apply_op('d', E.last_action.motion, use_n);
                    break;
                case LA_CHANGE: {
                    editor_apply_op('c', E.last_action.motion, use_n);
                    /* editor_apply_op('c') entered insert mode and set has_pre_insert. */
                    if (E.last_action.text && E.last_action.text_len > 0)
                        replay_insert_text(E.last_action.text, E.last_action.text_len);
                    if (E.cx > 0) E.cx--;
                    E.mode = MODE_NORMAL;
                    if (E.has_pre_insert) {
                        if (E.buf.dirty != E.pre_insert_dirty)
                            undo_tree_push(&E.undo_tree, E.pre_insert_snapshot, "change");
                        else
                            undo_state_free(&E.pre_insert_snapshot);
                        E.has_pre_insert = 0;
                    }
                    break;
                }
                case LA_PASTE:
                    for (int i = 0; i < use_n; i++)
                        editor_paste(E.last_action.before);
                    break;
                case LA_INSERT: {
                    push_undo("insert");
                    char ent = E.last_action.entry;
                    if (ent == 'a' && E.cy < E.buf.numrows &&
                        E.cx < E.buf.rows[E.cy].len) E.cx++;
                    if (ent == 'A' && E.cy < E.buf.numrows)
                        E.cx = E.buf.rows[E.cy].len;
                    if (E.last_action.text && E.last_action.text_len > 0)
                        replay_insert_text(E.last_action.text, E.last_action.text_len);
                    if (E.cx > 0) E.cx--;
                    break;
                }
                case LA_OPEN: {
                    push_undo("open line");
                    if (E.last_action.open_above) {
                        char ind[256]; int ind_len = 0;
                        if (E.opts.autoindent && E.cy < E.buf.numrows)
                            ind_len = row_indent(&E.buf.rows[E.cy], ind, sizeof(ind));
                        buf_insert_row(&E.buf, E.cy, ind, ind_len);
                        E.cx = ind_len;
                    } else {
                        char ind[256]; int ind_len = 0, extra = 0;
                        if (E.opts.autoindent && E.cy < E.buf.numrows) {
                            const Row *r = &E.buf.rows[E.cy];
                            ind_len = row_indent(r, ind, sizeof(ind));
                            if (is_open_bracket(last_nonws(r, r->len)))
                                extra = E.opts.tabwidth;
                        }
                        int at = E.cy + 1;
                        if (at > E.buf.numrows) at = E.buf.numrows;
                        char row_init[512];
                        memcpy(row_init, ind, ind_len);
                        memset(row_init + ind_len, ' ', extra);
                        buf_insert_row(&E.buf, at, row_init, ind_len + extra);
                        E.cy = at;
                        E.cx = ind_len + extra;
                    }
                    if (E.last_action.text && E.last_action.text_len > 0)
                        replay_insert_text(E.last_action.text, E.last_action.text_len);
                    if (E.cx > 0) E.cx--;
                    break;
                }
                default: break;
            }
            E.is_replaying = 0;
            break;
        }

        /* --- q: start macro recording --- */
        case 'q': {
            int r = editor_read_key();
            if (r >= 'a' && r <= 'z') {
                E.recording_reg = r - 'a';
                E.macro_len = 0;
                status_msg("Recording @%c...", r);
            }
            break;
        }

        /* --- @: replay macro --- */
        case '@': {
            int r = editor_read_key();
            int ri;
            if (r == '@') {
                ri = E.last_macro_reg;
                if (ri < 0) { status_err("No previous macro"); break; }
            } else if (r >= 'a' && r <= 'z') {
                ri = r - 'a';
            } else {
                break;
            }
            macro_play(ri, n);
            break;
        }

        /* --- f/F/t/T: find char on line; ;/, repeat --- */
        case 'f': case 'F': case 't': case 'T': {
            int tk = editor_read_key();
            if (tk == '\x1b') break;
            char fkey = (char)c, ftarget = (char)tk;
            E.last_find_key    = fkey;
            E.last_find_target = ftarget;
            int r = E.cy, col = E.cx;
            if (pos_find_char(&r, &col, fkey, ftarget, n))
                E.cx = col;
            break;
        }

        case ';': {
            if (!E.last_find_key) break;
            int r = E.cy, col = E.cx;
            if (pos_find_char(&r, &col, E.last_find_key, E.last_find_target, n))
                E.cx = col;
            break;
        }

        case ',': {
            if (!E.last_find_key) break;
            char rev = (E.last_find_key == 'f') ? 'F'
                     : (E.last_find_key == 'F') ? 'f'
                     : (E.last_find_key == 't') ? 'T' : 't';
            int r = E.cy, col = E.cx;
            if (pos_find_char(&r, &col, rev, E.last_find_target, n))
                E.cx = col;
            break;
        }

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

        case '_':
        case '^':
            /* First non-blank character on the line. */
            if (E.cy < E.buf.numrows) {
                Row *row = &E.buf.rows[E.cy];
                int col = 0;
                while (col < row->len && (row->chars[col] == ' ' || row->chars[col] == '\t'))
                    col++;
                E.cx = col < row->len ? col : (row->len > 0 ? row->len - 1 : 0);
            }
            break;

        case '{': {
            /* Move to previous blank line (paragraph boundary). */
            int n = E.count > 0 ? E.count : 1;
            E.count = 0;
            for (int i = 0; i < n; i++) {
                int r = E.cy - 1;
                /* Skip current blank lines. */
                while (r > 0 && E.buf.rows[r].len == 0) r--;
                /* Find next blank line. */
                while (r > 0 && E.buf.rows[r].len > 0) r--;
                E.cy = r;
            }
            E.cx = 0;
            s_vertical_move = 1;
            break;
        }

        case '}': {
            /* Move to next blank line (paragraph boundary). */
            int n = E.count > 0 ? E.count : 1;
            E.count = 0;
            int last = E.buf.numrows > 0 ? E.buf.numrows - 1 : 0;
            for (int i = 0; i < n; i++) {
                int r = E.cy + 1;
                /* Skip current blank lines. */
                while (r < last && E.buf.rows[r].len == 0) r++;
                /* Find next blank line. */
                while (r < last && E.buf.rows[r].len > 0) r++;
                E.cy = r;
            }
            E.cx = 0;
            s_vertical_move = 1;
            break;
        }

        /* --- G: nG = go to line n, G = last line --- */
        case 'G':
            jump_push();
            if (raw_count > 0) {
                E.cy = raw_count - 1;  /* 1-based input → 0-based index */
                if (E.cy >= E.buf.numrows && E.buf.numrows > 0)
                    E.cy = E.buf.numrows - 1;
            } else {
                if (E.buf.numrows > 0) E.cy = E.buf.numrows - 1;
            }
            s_vertical_move = 1;
            apply_preferred_col();
            break;

        /* --- g prefix (gg = first line) --- */
        case 'g':
            E.pending_g = 1;
            break;

        /* --- z prefix (fold commands) --- */
        case 'z':
            E.pending_z = 1;
            break;

        /* --- ] / [ prefix (hunk navigation) --- */
        case ']':
        case '[':
            E.pending_bracket = c;
            break;

        /* --- marks --- */
        case 'm':
            E.pending_mark = 'm';
            break;
        case '`':
            E.pending_mark = '`';
            break;
        case '\'':
            E.pending_mark = '\'';
            break;

        /* --- % : jump to matching bracket --- */
        case '%':
            if (E.match_bracket_valid) {
                jump_push();
                E.cy = E.match_bracket_row;
                E.cx = E.match_bracket_col;
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

    /* Record keystroke for . repeat (except Escape; not during replay). */
    if (!E.is_replaying && c != '\x1b') {
        if (c == '\r' || c == 127 || c == '\t' || (c >= 32 && c < 127))
            insert_rec_append((char)c);
    }

    switch (c) {
        case '\x1b':
            if (E.cx > 0) E.cx--;  /* nudge left on Escape, like Vim */
            E.mode = MODE_NORMAL;
            /* Push the pre-insert snapshot if anything changed. */
            if (E.has_pre_insert) {
                if (E.buf.dirty != E.pre_insert_dirty) {
                    undo_tree_push(&E.undo_tree, E.pre_insert_snapshot, "insert");
                    /* Finalise last_action for . repeat. */
                    if (!E.is_replaying) {
                        la_free();
                        char ent = E.insert_entry;
                        if (ent == 'c') {
                            E.last_action.type        = LA_CHANGE;
                            E.last_action.count       = E.insert_count;
                            E.last_action.motion      = E.insert_motion;
                            E.last_action.find_target = (char)E.insert_find_target;
                        } else if (ent == 'o' || ent == 'O') {
                            E.last_action.type      = LA_OPEN;
                            E.last_action.open_above = (ent == 'O');
                            E.last_action.count     = 1;
                        } else {
                            E.last_action.type  = LA_INSERT;
                            E.last_action.entry = ent;
                            E.last_action.count = 1;
                        }
                        if (E.insert_rec_len > 0) {
                            E.last_action.text = malloc(E.insert_rec_len);
                            if (E.last_action.text) {
                                memcpy(E.last_action.text,
                                       E.insert_rec, E.insert_rec_len);
                                E.last_action.text_len = E.insert_rec_len;
                            }
                        }
                    }
                } else {
                    undo_state_free(&E.pre_insert_snapshot);
                }
                E.has_pre_insert = 0;
            }
            /* Block-visual insert: apply recorded text to remaining rows. */
            if (E.block_insert_active && E.insert_rec_len > 0) {
                E.block_insert_active = 0;
                int col = E.block_insert_col;
                for (int r = E.block_insert_r0; r <= E.block_insert_r1
                         && r < E.buf.numrows; r++) {
                    if (r == E.block_insert_r0) continue; /* already edited */
                    Row *row = &E.buf.rows[r];
                    /* Pad with spaces if row is shorter than insert column. */
                    while (row->len < col)
                        buf_row_insert_char(row, row->len, ' ');
                    for (int i = 0; i < E.insert_rec_len; i++)
                        buf_row_insert_char(row, col + i, E.insert_rec[i]);
                    E.buf.dirty++;
                    buf_mark_hl_dirty(&E.buf, r);
                }
            } else {
                E.block_insert_active = 0;
            }
            editor_update_git_signs();
            break;

        case '\r':
            editor_insert_newline();
            break;

        case 127: { /* Backspace */
            /* Auto-pairs: delete both chars when cursor is between a pair. */
            if (E.opts.autopairs) {
                char before = char_before_cursor();
                char after  = char_at_cursor();
                if (before && autopair_close(before) == after && after != 0) {
                    /* Delete the closing char first (at cursor), then backspace. */
                    buf_row_delete_char(&E.buf.rows[E.cy], E.cx);
                    E.buf.dirty++;
                    buf_mark_hl_dirty(&E.buf, E.cy);
                }
            }
            editor_delete_char();
            break;
        }

        case '\t': {
            /* Insert spaces up to the next tab stop. */
            int spaces = E.opts.tabwidth - (E.cx % E.opts.tabwidth);
            for (int i = 0; i < spaces; i++)
                editor_insert_char(' ');
            break;
        }

        case ARROW_LEFT:
            if (E.cx > 0) E.cx--;
            break;
        case ARROW_RIGHT:
            if (E.cy < E.buf.numrows && E.cx < E.buf.rows[E.cy].len)
                E.cx++;
            break;
        case ARROW_UP:
            if (E.cy > 0) {
                E.cy--;
                int rowlen = (E.cy < E.buf.numrows) ? E.buf.rows[E.cy].len : 0;
                if (E.cx > rowlen) E.cx = rowlen;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.buf.numrows - 1) {
                E.cy++;
                int rowlen = (E.cy < E.buf.numrows) ? E.buf.rows[E.cy].len : 0;
                if (E.cx > rowlen) E.cx = rowlen;
            }
            break;

        default:
            if (c >= 32 && c < 127) {
                if (E.opts.autoindent && is_close_bracket((char)c))
                    editor_close_bracket((char)c);
                else if (E.opts.autopairs && is_autopair_open((char)c)) {
                    /* Skip-over: typing a quote that matches char at cursor. */
                    if ((c == '"' || c == '\'') && char_at_cursor() == c) {
                        E.cx++;
                    } else {
                        /* Insert opening + closing, cursor between. */
                        editor_insert_char((char)c);
                        editor_insert_char(autopair_close((char)c));
                        E.cx--;
                    }
                } else if (E.opts.autopairs && is_close_bracket((char)c)
                           && char_at_cursor() == c) {
                    /* Skip over matching closing bracket. */
                    E.cx++;
                } else {
                    editor_insert_char((char)c);
                }
            }
            break;
    }
}

/* ── Tab completion ──────────────────────────────────────────────────── */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void completion_free(void) {
    for (int i = 0; i < E.completion_count; i++)
        free(E.completion_matches[i]);
    free(E.completion_matches);
    E.completion_matches = NULL;
    E.completion_count   = 0;
    E.completion_idx     = -1;
}

static void completion_build(const char *prefix) {
    completion_free();

    /* Split prefix at last '/' so we can open the right directory.
       e.g. "src/inp" → open "src", match names starting with "inp",
            "src/"    → open "src", match all names,
            "my_f"    → open ".",   match names starting with "my_f" */
    const char *last_slash = strrchr(prefix, '/');
    int dir_len = last_slash ? (int)(last_slash - prefix + 1) : 0; /* incl. '/' */
    const char *name_part = prefix + dir_len;
    int namelen = (int)strlen(name_part);

    char dir_open[512];
    if (dir_len > 0)
        snprintf(dir_open, sizeof(dir_open), "%.*s", dir_len - 1, prefix);
    else
        strcpy(dir_open, ".");

    DIR *d = opendir(dir_open);
    if (!d) return;

    int cap = 16;
    E.completion_matches = malloc(sizeof(char *) * cap);
    struct dirent *de;
    while ((de = readdir(d))) {
        /* skip hidden files unless name_part itself starts with '.' */
        if (de->d_name[0] == '.' && (namelen == 0 || name_part[0] != '.'))
            continue;
        if (namelen && strncmp(de->d_name, name_part, namelen) != 0)
            continue;
        if (E.completion_count == cap) {
            cap *= 2;
            E.completion_matches = realloc(E.completion_matches,
                                           sizeof(char *) * cap);
        }
        /* Store full path: dir prefix + matched name */
        char full[768];
        if (dir_len > 0)
            snprintf(full, sizeof(full), "%.*s%s", dir_len, prefix, de->d_name);
        else
            snprintf(full, sizeof(full), "%s", de->d_name);
        E.completion_matches[E.completion_count++] = strdup(full);
    }
    closedir(d);
    if (E.completion_count == 0) {
        free(E.completion_matches);
        E.completion_matches = NULL;
        return;
    }
    qsort(E.completion_matches, E.completion_count, sizeof(char *), cmp_str);
    E.completion_idx = 0;
}

/* ── Command mode ────────────────────────────────────────────────────── */

static void editor_process_command(int c) {
    if (lua_bridge_call_key(MODE_COMMAND, c)) return;
    switch (c) {
        case '\t': {  /* Tab — file completion for :e */
            const char *cmd = E.cmdbuf;
            if (cmd[0] != 'e' || (cmd[1] != '\0' && cmd[1] != ' ')) break;
            const char *prefix = (cmd[1] == ' ') ? cmd + 2 : "";
            if (E.completion_idx == -1) {
                completion_build(prefix);
            } else {
                E.completion_idx = (E.completion_idx + 1) % E.completion_count;
            }
            if (E.completion_idx >= 0) {
                int len = snprintf(E.cmdbuf, sizeof(E.cmdbuf), "e %s",
                                   E.completion_matches[E.completion_idx]);
                E.cmdlen = len;
            }
            break;
        }

        case '\x1b':
            completion_free();
            E.mode = MODE_NORMAL;
            E.cmdbuf[0] = '\0';
            E.cmdlen    = 0;
            break;

        case '\r':
            completion_free();
            editor_execute_command();
            break;

        case 127:  /* Backspace */
            completion_free();
            if (E.cmdlen > 0)
                E.cmdbuf[--E.cmdlen] = '\0';
            else
                E.mode = MODE_NORMAL;  /* backspace on empty ":" exits */
            break;

        default:
            completion_free();
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
                    jump_push();
                    E.cy = row;
                    E.cx = col;
                } else {
                    status_err("Pattern not found");
                }
            }
            break;
        }

        case 127:  /* Backspace */
            if (E.searchlen > 0)
                E.searchbuf[--E.searchlen] = '\0';
            else
                E.mode = MODE_NORMAL;  /* backspace on empty "/" exits */
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

/* Compute gutter width for a buffer (mirrors render.c logic). */
static int gutter_width_for_buf(const Buffer *buf, int buf_idx) {
    if (!E.opts.line_numbers || buf->numrows == 0) return 0;
    int n = buf->numrows, w = 0;
    while (n > 0) { w++; n /= 10; }
    w++;
    for (int i = 0; i < MARK_MAX; i++)
        if (E.marks[i].valid && E.marks[i].buf_idx == buf_idx) { w += 2; break; }
    return w;
}

/* Handle a mouse click: focus the pane and position the cursor. */
static void handle_mouse_press(void) {
    int mx = E.mouse_x, my = E.mouse_y;

    /* Find the pane that contains the click. */
    int target = -1;
    for (int i = 0; i < E.num_panes; i++) {
        Pane *p = &E.panes[i];
        if (my >= p->top && my <= p->top + p->height &&
            mx >= p->left && mx < p->left + p->width) {
            target = i;
            break;
        }
    }
    if (target == -1) return;

    /* If in insert/visual mode, return to normal first. */
    if (E.mode == MODE_INSERT || E.mode == MODE_VISUAL ||
        E.mode == MODE_VISUAL_LINE || E.mode == MODE_VISUAL_BLOCK) {
        E.mode = MODE_NORMAL;
        if (E.cx > 0 && E.cy < E.buf.numrows &&
            E.cx >= E.buf.rows[E.cy].len) E.cx--;
    }

    /* Activate the target pane (saves current cursor, restores target's). */
    if (target != E.cur_pane) {
        pane_save_cursor();
        pane_activate(target);
    }

    Pane *p = &E.panes[E.cur_pane];

    /* Click on status bar row: just focus, don't move cursor. */
    if (my == p->top + p->height) return;

    /* Tree pane: move cursor to clicked row; click on current row activates. */
    if (E.buftabs[E.panes[E.cur_pane].buf_idx].kind == BT_TREE) {
        int filerow = (my - p->top) + E.rowoff;
        if (filerow < 0) filerow = 0;
        if (E.buf.numrows > 0 && filerow >= E.buf.numrows)
            filerow = E.buf.numrows - 1;
        int was_same = (filerow == E.cy);
        E.cy = filerow;
        E.cx = 0;
        if (was_same && filerow > 0)   /* second click on same row = open/toggle */
            tree_activate_entry();
        return;
    }

    /* Convert terminal coords to file coords. */
    int gw      = gutter_width_for_buf(&E.buf, E.cur_buftab);
    int filerow = (my - p->top) + E.rowoff;
    int click_vcol = (mx - p->left - gw) + E.coloff;  /* visual column */
    if (click_vcol < 0) click_vcol = 0;
    if (filerow < 0) filerow = 0;
    int filecol = 0;
    if (E.buf.numrows > 0) {
        if (filerow >= E.buf.numrows) filerow = E.buf.numrows - 1;
        Row *row = &E.buf.rows[filerow];
        filecol = vcol_to_col(row, click_vcol, E.opts.tabwidth);
        if (filecol > row->len) filecol = row->len;
    }
    E.cy = filerow;
    E.cx = filecol;
}

/* Handle mouse scroll: move the active pane's viewport by 3 lines. */
static void handle_mouse_scroll(int dir) {
    int delta = 3;
    if (dir < 0) {  /* scroll up */
        E.rowoff -= delta;
        if (E.rowoff < 0) E.rowoff = 0;
        if (E.cy > E.rowoff + E.screenrows - 1)
            E.cy = E.rowoff + E.screenrows - 1;
    } else {        /* scroll down */
        int max_off = E.buf.numrows > E.screenrows
                      ? E.buf.numrows - E.screenrows : 0;
        E.rowoff += delta;
        if (E.rowoff > max_off) E.rowoff = max_off;
        if (E.cy < E.rowoff) E.cy = E.rowoff;
    }
}

/* ── Fuzzy finder key handler ────────────────────────────────────────── */

static void editor_process_fuzzy(int c) {
    FuzzyState *f = &E.fuzzy;

    if (c == '\x1b') { fuzzy_close(); return; }

    if (c == '\r') {   /* Enter: open in current pane / switch buffer */
        if (f->match_count > 0) {
            if (f->buf_mode) {
                int oi = f->matches[f->selected].orig_idx;
                int bi = f->buf_indices[oi];
                fuzzy_close();
                if (bi != E.cur_buftab) {
                    pane_save_cursor();
                    editor_buf_save(E.cur_buftab);
                    E.panes[E.cur_pane].buf_idx = bi;
                    E.cur_buftab = bi;
                    editor_buf_restore(bi);
                    editor_detect_syntax();
                }
            } else {
                char path[512];
                strncpy(path, f->matches[f->selected].path, sizeof(path) - 1);
                path[sizeof(path)-1] = '\0';
                fuzzy_close();
                open_new_buf(path);
            }
        } else {
            fuzzy_close();
        }
        return;
    }

    if (c == 0x18) {   /* Ctrl-X: open in horizontal split */
        if (f->match_count > 0) {
            char path[512];
            strncpy(path, f->matches[f->selected].path, sizeof(path) - 1);
            path[sizeof(path)-1] = '\0';
            fuzzy_close();
            pane_save_cursor();
            if (pane_split_h(E.cur_pane)) {
                pane_activate(E.cur_pane + 1);
                open_new_buf(path);
                E.panes[E.cur_pane].buf_idx = E.cur_buftab;
            }
        } else {
            fuzzy_close();
        }
        return;
    }

    if (c == 0x16) {   /* Ctrl-V: open in vertical split */
        if (f->match_count > 0) {
            char path[512];
            strncpy(path, f->matches[f->selected].path, sizeof(path) - 1);
            path[sizeof(path)-1] = '\0';
            fuzzy_close();
            pane_save_cursor();
            if (pane_split_v(E.cur_pane)) {
                pane_activate(E.cur_pane + 1);
                open_new_buf(path);
                E.panes[E.cur_pane].buf_idx = E.cur_buftab;
            }
        } else {
            fuzzy_close();
        }
        return;
    }

    /* Navigation. */
    if (c == ARROW_UP || c == 0x0b) {   /* ↑ or Ctrl-K */
        if (f->selected > 0) {
            f->selected--;
            if (f->selected < f->scroll) f->scroll = f->selected;
        }
        return;
    }
    if (c == ARROW_DOWN || c == 0x0a) { /* ↓ or Ctrl-J */
        if (f->selected < f->match_count - 1) {
            f->selected++;
            if (f->selected >= f->scroll + FUZZY_MAX_VIS)
                f->scroll = f->selected - FUZZY_MAX_VIS + 1;
        }
        return;
    }

    /* Query editing. */
    if (c == 127 || c == 8) {           /* Backspace */
        if (f->query_len > 0) {
            f->query[--f->query_len] = '\0';
            f->selected = 0; f->scroll = 0;
            fuzzy_filter();
        }
        return;
    }
    if (c >= 32 && c < 127 && f->query_len < 255) {
        f->query[f->query_len++] = (char)c;
        f->query[f->query_len]   = '\0';
        f->selected = 0; f->scroll = 0;
        fuzzy_filter();
    }
}

static void macro_record_key(int c);
static void macro_stop(void);

static void macro_play(int ri, int count) {
    if (ri < 0 || ri >= MACRO_REGS) return;
    if (!E.macros[ri].keys || E.macros[ri].len == 0) {
        status_err("Register %c is empty", 'a' + ri);
        return;
    }
    E.last_macro_reg = ri;
    E.macro_playing++;
    for (int rep = 0; rep < count; rep++) {
        for (int i = 0; i < E.macros[ri].len; i++) {
            int c = E.macros[ri].keys[i];

            if (E.recording_reg >= 0)
                macro_record_key(c);

            s_vertical_move = 0;
            switch (E.mode) {
                case MODE_NORMAL:      editor_process_normal(c);  break;
                case MODE_INSERT:      editor_process_insert(c);  break;
                case MODE_COMMAND:     editor_process_command(c); break;
                case MODE_SEARCH:      editor_process_search(c);  break;
                case MODE_VISUAL:
                case MODE_VISUAL_LINE:
                case MODE_VISUAL_BLOCK: editor_process_visual(c);  break;
                default: break;
            }
        }
    }
    E.macro_playing--;
}

void editor_process_keypress(void) {
    int c = editor_read_key();

    /* File-changed prompt: intercept the next key. */
    if (E.watch_prompt_buf >= 0) {
        int bidx = E.watch_prompt_buf;
        E.watch_prompt_buf = -1;
        E.statusmsg[0] = '\0';
        E.statusmsg_is_error = 0;

        if (c == 'L' || c == 'l') {
            editor_reload_buf(bidx);
            const char *fname = (bidx == E.cur_buftab) ? E.buf.filename
                                                        : E.buftabs[bidx].buf.filename;
            const char *base = fname ? strrchr(fname, '/') : NULL;
            base = base ? base + 1 : fname;
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "\"%s\" reloaded", base ? base : "");
        }
        /* O, Escape, Enter, or anything else = keep local version */
        return;
    }

    /* Mouse events are handled in all modes. */
    if (c == MOUSE_PRESS) {
        handle_mouse_press();
        E.preferred_col = (E.buf.numrows > 0 && E.cy < E.buf.numrows)
            ? col_to_vcol(&E.buf.rows[E.cy], E.cx, E.opts.tabwidth)
            : E.cx;   /* click sets a new preferred column */
        return;
    }
    if (c == MOUSE_SCROLL_UP)   { handle_mouse_scroll(-1); return; }
    if (c == MOUSE_SCROLL_DOWN) { handle_mouse_scroll(+1); return; }

    /* Terminal pane in INSERT mode: forward keys to PTY.
       Ctrl-\ (0x1c) escapes back to normal mode for pane nav / commands. */
    if (E.buftabs[E.panes[E.cur_pane].buf_idx].kind == BT_TERM
        && E.mode == MODE_INSERT) {
        TermState *ts = E.buftabs[E.panes[E.cur_pane].buf_idx].term;
        if (c == 0x1c || c == '\x1b') {
            /* Ctrl-\ or Escape: return to normal mode (terminal-normal). */
            E.mode = MODE_NORMAL;
            return;
        }
        if (ts && !ts->exited) {
            /* Translate editor key codes to terminal escape sequences. */
            const char *seq = NULL;
            switch (c) {
                case ARROW_UP:    seq = "\x1b[A"; break;
                case ARROW_DOWN:  seq = "\x1b[B"; break;
                case ARROW_RIGHT: seq = "\x1b[C"; break;
                case ARROW_LEFT:  seq = "\x1b[D"; break;
                case HOME_KEY:    seq = "\x1b[H"; break;
                case END_KEY:     seq = "\x1b[F"; break;
                case PAGE_UP:     seq = "\x1b[5~"; break;
                case PAGE_DOWN:   seq = "\x1b[6~"; break;
                case DEL_KEY:     seq = "\x1b[3~"; break;
                default:
                    if (c >= 0 && c < 256) {
                        char ch = (char)c;
                        term_emu_write(ts, &ch, 1);
                    }
                    break;
            }
            if (seq) term_emu_write(ts, seq, (int)strlen(seq));
        }
        return;
    }

    /* Macro: stop recording on q in normal mode. */
    if (E.recording_reg >= 0 && c == 'q' && E.mode == MODE_NORMAL) {
        macro_stop();
        return;
    }

    /* Macro: record key (but not while replaying). */
    if (E.recording_reg >= 0 && !E.macro_playing)
        macro_record_key(c);

    s_vertical_move = 0;

    switch (E.mode) {
        case MODE_NORMAL:      editor_process_normal(c);  break;
        case MODE_INSERT:      editor_process_insert(c);  break;
        case MODE_COMMAND:     editor_process_command(c); break;
        case MODE_SEARCH:      editor_process_search(c);  break;
        case MODE_VISUAL:
        case MODE_VISUAL_LINE:
        case MODE_VISUAL_BLOCK: editor_process_visual(c);  break;
        case MODE_FUZZY:       editor_process_fuzzy(c);   break;
    }

    /* After any non-vertical-move action, record the current column as the
       preferred column so future j/k movements try to restore it. */
    if (!s_vertical_move) {
        /* Store visual column so j/k sticky-column works correctly with tabs. */
        E.preferred_col = (E.buf.numrows > 0 && E.cy < E.buf.numrows)
            ? col_to_vcol(&E.buf.rows[E.cy], E.cx, E.opts.tabwidth)
            : E.cx;
    }
}

void editor_reap_terminals(void) {
    for (int i = 0; i < E.num_buftabs; i++) {
        if (E.buftabs[i].kind == BT_TERM && E.buftabs[i].term
            && E.buftabs[i].term->exited) {
            term_close_buf(i);
            return;  /* indices shifted — restart on next call */
        }
    }
}
