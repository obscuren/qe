// SPDX-License-Identifier: GPL-3.0-or-later
#include "term_emu.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define SCROLLBACK_MAX 1000

/* ── Helpers ──────────────────────────────────────────────────────────── */

static TermCell blank_cell(void) {
    return (TermCell){ .ch = ' ' };
}

static TermCell *cell_at(TermState *t, int row, int col) {
    if (row < 0 || row >= t->rows || col < 0 || col >= t->cols)
        return NULL;
    return &t->cells[row * t->cols + col];
}

static void clamp_cursor(TermState *t) {
    if (t->c_row < 0) t->c_row = 0;
    if (t->c_row >= t->rows) t->c_row = t->rows - 1;
    if (t->c_col < 0) t->c_col = 0;
    if (t->c_col >= t->cols) t->c_col = t->cols - 1;
}

/* ── Scrollback ───────────────────────────────────────────────────────── */

static void sb_push_line(TermState *t, const TermCell *line) {
    if (!t->scrollback || t->sb_cap <= 0) return;
    int idx = (t->sb_start + t->sb_lines) % t->sb_cap;
    if (t->sb_lines >= t->sb_cap) {
        /* Ring full — overwrite oldest. */
        t->sb_start = (t->sb_start + 1) % t->sb_cap;
    } else {
        t->sb_lines++;
    }
    memcpy(&t->scrollback[idx * t->cols], line, t->cols * sizeof(TermCell));
}

/* ── Scroll region ────────────────────────────────────────────────────── */

/* Scroll lines up within the scroll region: top line pushed to scrollback
   (if region starts at row 0), bottom gets a blank line. */
static void scroll_up(TermState *t) {
    int top = t->scroll_top, bot = t->scroll_bot;
    if (top == 0)
        sb_push_line(t, &t->cells[top * t->cols]);
    for (int r = top; r < bot; r++)
        memcpy(&t->cells[r * t->cols], &t->cells[(r + 1) * t->cols],
               t->cols * sizeof(TermCell));
    for (int c = 0; c < t->cols; c++)
        t->cells[bot * t->cols + c] = blank_cell();
}

/* Scroll lines down within the scroll region. */
static void scroll_down(TermState *t) {
    int top = t->scroll_top, bot = t->scroll_bot;
    for (int r = bot; r > top; r--)
        memcpy(&t->cells[r * t->cols], &t->cells[(r - 1) * t->cols],
               t->cols * sizeof(TermCell));
    for (int c = 0; c < t->cols; c++)
        t->cells[top * t->cols + c] = blank_cell();
}

/* ── Erase helpers ────────────────────────────────────────────────────── */

static void erase_in_line(TermState *t, int mode) {
    int r = t->c_row;
    int start = 0, end = t->cols;
    if (mode == 0)      start = t->c_col;        /* cursor to end   */
    else if (mode == 1)  end = t->c_col + 1;      /* start to cursor */
    /* mode == 2: entire line */
    for (int c = start; c < end; c++)
        t->cells[r * t->cols + c] = blank_cell();
}

static void erase_in_display(TermState *t, int mode) {
    if (mode == 0) {
        /* Cursor to end of screen. */
        erase_in_line(t, 0);
        for (int r = t->c_row + 1; r < t->rows; r++)
            for (int c = 0; c < t->cols; c++)
                t->cells[r * t->cols + c] = blank_cell();
    } else if (mode == 1) {
        /* Start to cursor. */
        for (int r = 0; r < t->c_row; r++)
            for (int c = 0; c < t->cols; c++)
                t->cells[r * t->cols + c] = blank_cell();
        erase_in_line(t, 1);
    } else if (mode == 2 || mode == 3) {
        /* Entire screen. */
        for (int i = 0; i < t->rows * t->cols; i++)
            t->cells[i] = blank_cell();
    }
}

/* ── CSI dispatch ─────────────────────────────────────────────────────── */

static void csi_dispatch(TermState *t, char final) {
    int *p = t->params;
    int n  = t->num_params;
    int p0 = (n >= 1 && p[0] > 0) ? p[0] : 1;
    int p1 = (n >= 2 && p[1] > 0) ? p[1] : 1;

    switch (final) {
    case 'A': /* CUU — cursor up */
        t->c_row -= p0;
        if (t->c_row < t->scroll_top) t->c_row = t->scroll_top;
        break;
    case 'B': /* CUD — cursor down */
        t->c_row += p0;
        if (t->c_row > t->scroll_bot) t->c_row = t->scroll_bot;
        break;
    case 'C': /* CUF — cursor forward */
        t->c_col += p0;
        if (t->c_col >= t->cols) t->c_col = t->cols - 1;
        break;
    case 'D': /* CUB — cursor backward */
        t->c_col -= p0;
        if (t->c_col < 0) t->c_col = 0;
        break;
    case 'E': /* CNL — cursor next line */
        t->c_row += p0; t->c_col = 0;
        if (t->c_row > t->scroll_bot) t->c_row = t->scroll_bot;
        break;
    case 'F': /* CPL — cursor prev line */
        t->c_row -= p0; t->c_col = 0;
        if (t->c_row < t->scroll_top) t->c_row = t->scroll_top;
        break;
    case 'G': /* CHA — cursor horizontal absolute */
        t->c_col = p0 - 1;
        if (t->c_col >= t->cols) t->c_col = t->cols - 1;
        break;
    case 'H': /* CUP — cursor position */
    case 'f': /* HVP */
        t->c_row = p0 - 1;
        t->c_col = p1 - 1;
        clamp_cursor(t);
        break;
    case 'J': /* ED — erase in display */
        erase_in_display(t, (n >= 1) ? p[0] : 0);
        break;
    case 'K': /* EL — erase in line */
        erase_in_line(t, (n >= 1) ? p[0] : 0);
        break;
    case 'L': { /* IL — insert lines */
        int count = p0;
        for (int i = 0; i < count; i++) {
            /* Shift lines down from cursor row to scroll_bot. */
            for (int r = t->scroll_bot; r > t->c_row; r--)
                memcpy(&t->cells[r * t->cols], &t->cells[(r - 1) * t->cols],
                       t->cols * sizeof(TermCell));
            for (int c = 0; c < t->cols; c++)
                t->cells[t->c_row * t->cols + c] = blank_cell();
        }
        break;
    }
    case 'M': { /* DL — delete lines */
        int count = p0;
        for (int i = 0; i < count; i++) {
            for (int r = t->c_row; r < t->scroll_bot; r++)
                memcpy(&t->cells[r * t->cols], &t->cells[(r + 1) * t->cols],
                       t->cols * sizeof(TermCell));
            for (int c = 0; c < t->cols; c++)
                t->cells[t->scroll_bot * t->cols + c] = blank_cell();
        }
        break;
    }
    case 'P': { /* DCH — delete characters */
        int count = p0;
        int r = t->c_row;
        for (int c = t->c_col; c + count < t->cols; c++)
            t->cells[r * t->cols + c] = t->cells[r * t->cols + c + count];
        for (int c = t->cols - count; c < t->cols; c++)
            if (c >= 0) t->cells[r * t->cols + c] = blank_cell();
        break;
    }
    case '@': { /* ICH — insert characters */
        int count = p0;
        int r = t->c_row;
        for (int c = t->cols - 1; c >= t->c_col + count; c--)
            t->cells[r * t->cols + c] = t->cells[r * t->cols + c - count];
        for (int c = t->c_col; c < t->c_col + count && c < t->cols; c++)
            t->cells[r * t->cols + c] = blank_cell();
        break;
    }
    case 'S': /* SU — scroll up */
        for (int i = 0; i < p0; i++) scroll_up(t);
        break;
    case 'T': /* SD — scroll down */
        for (int i = 0; i < p0; i++) scroll_down(t);
        break;
    case 'd': /* VPA — line position absolute */
        t->c_row = p0 - 1;
        clamp_cursor(t);
        break;
    case 'm': /* SGR — select graphic rendition */
        if (n == 0) {
            /* Reset all. */
            t->cur_fg = 0; t->cur_bg = 0;
            t->cur_bold = 0; t->cur_dim = 0;
            t->cur_underline = 0; t->cur_reverse = 0;
        }
        for (int i = 0; i < n; i++) {
            int v = p[i];
            if (v == 0) {
                t->cur_fg = 0; t->cur_bg = 0;
                t->cur_bold = 0; t->cur_dim = 0;
                t->cur_underline = 0; t->cur_reverse = 0;
            } else if (v == 1) t->cur_bold = 1;
            else if (v == 2) t->cur_dim = 1;
            else if (v == 4) t->cur_underline = 1;
            else if (v == 7) t->cur_reverse = 1;
            else if (v == 22) { t->cur_bold = 0; t->cur_dim = 0; }
            else if (v == 24) t->cur_underline = 0;
            else if (v == 27) t->cur_reverse = 0;
            else if (v >= 30 && v <= 37) t->cur_fg = v - 30 + 1;
            else if (v == 38 && i + 2 < n && p[i+1] == 5) {
                t->cur_fg = p[i+2] + 1; i += 2;  /* 256-color fg */
            }
            else if (v == 39) t->cur_fg = 0;
            else if (v >= 40 && v <= 47) t->cur_bg = v - 40 + 1;
            else if (v == 48 && i + 2 < n && p[i+1] == 5) {
                t->cur_bg = p[i+2] + 1; i += 2;  /* 256-color bg */
            }
            else if (v == 49) t->cur_bg = 0;
            else if (v >= 90 && v <= 97) t->cur_fg = v - 90 + 9;  /* bright fg */
            else if (v >= 100 && v <= 107) t->cur_bg = v - 100 + 9; /* bright bg */
        }
        break;
    case 'r': /* DECSTBM — set scroll region */
        t->scroll_top = (n >= 1 && p[0] > 0) ? p[0] - 1 : 0;
        t->scroll_bot = (n >= 2 && p[1] > 0) ? p[1] - 1 : t->rows - 1;
        if (t->scroll_top >= t->rows) t->scroll_top = t->rows - 1;
        if (t->scroll_bot >= t->rows) t->scroll_bot = t->rows - 1;
        if (t->scroll_top > t->scroll_bot) t->scroll_top = t->scroll_bot;
        t->c_row = 0; t->c_col = 0;
        break;
    case 'h': /* SM — set mode (ignore most) */
    case 'l': /* RM — reset mode (ignore most) */
        /* We silently ignore mode changes (alternate screen, cursor visibility, etc.)
           since we ARE the terminal. */
        break;
    case 'n': /* DSR — device status report */
        if (n >= 1 && p[0] == 6) {
            /* CPR — report cursor position. */
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "\x1b[%d;%dR",
                               t->c_row + 1, t->c_col + 1);
            write(t->pty_fd, resp, len);
        }
        break;
    case 'X': { /* ECH — erase characters */
        int count = p0;
        for (int c = t->c_col; c < t->c_col + count && c < t->cols; c++)
            t->cells[t->c_row * t->cols + c] = blank_cell();
        break;
    }
    /* Ignore unknown sequences. */
    }
}

/* ── VT100 parser ─────────────────────────────────────────────────────── */

static void put_char(TermState *t, char ch) {
    TermCell *c = cell_at(t, t->c_row, t->c_col);
    if (c) {
        c->ch        = ch;
        c->fg        = t->cur_fg;
        c->bg        = t->cur_bg;
        c->bold      = t->cur_bold;
        c->dim       = t->cur_dim;
        c->underline = t->cur_underline;
        c->reverse   = t->cur_reverse;
    }
    t->c_col++;
    if (t->c_col >= t->cols) {
        /* Auto-wrap. */
        t->c_col = 0;
        if (t->c_row == t->scroll_bot)
            scroll_up(t);
        else if (t->c_row < t->rows - 1)
            t->c_row++;
    }
}

static void process_byte(TermState *t, unsigned char ch) {
    switch (t->parse_state) {
    case TP_GROUND:
        if (ch == '\x1b') {
            t->parse_state = TP_ESC;
        } else if (ch == '\n') {
            if (t->c_row == t->scroll_bot)
                scroll_up(t);
            else if (t->c_row < t->rows - 1)
                t->c_row++;
        } else if (ch == '\r') {
            t->c_col = 0;
        } else if (ch == '\b') {
            if (t->c_col > 0) t->c_col--;
        } else if (ch == '\t') {
            int next = (t->c_col + 8) & ~7;
            if (next >= t->cols) next = t->cols - 1;
            t->c_col = next;
        } else if (ch == '\a') {
            /* Bell — ignore. */
        } else if (ch >= 0x20) {
            put_char(t, ch);
        }
        break;

    case TP_ESC:
        if (ch == '[') {
            t->parse_state = TP_CSI;
            t->num_params = 0;
            t->param_partial = 0;
            memset(t->params, 0, sizeof(t->params));
        } else if (ch == ']') {
            t->parse_state = TP_OSC;
            t->osc_len = 0;
        } else if (ch == 'M') {
            /* RI — reverse index (scroll down). */
            if (t->c_row == t->scroll_top)
                scroll_down(t);
            else if (t->c_row > 0)
                t->c_row--;
            t->parse_state = TP_GROUND;
        } else if (ch == 'D') {
            /* IND — index (scroll up). */
            if (t->c_row == t->scroll_bot)
                scroll_up(t);
            else if (t->c_row < t->rows - 1)
                t->c_row++;
            t->parse_state = TP_GROUND;
        } else if (ch == 'E') {
            /* NEL — next line. */
            t->c_col = 0;
            if (t->c_row == t->scroll_bot)
                scroll_up(t);
            else if (t->c_row < t->rows - 1)
                t->c_row++;
            t->parse_state = TP_GROUND;
        } else if (ch == '7') {
            /* DECSC — save cursor (simplified: just position). */
            t->parse_state = TP_GROUND;
        } else if (ch == '8') {
            /* DECRC — restore cursor (simplified: ignore). */
            t->parse_state = TP_GROUND;
        } else if (ch == '(' || ch == ')') {
            /* Charset designation — skip next byte. */
            /* We'll consume it in TP_GROUND since it's just one byte.
               For simplicity, just go back to ground. The next byte
               will be consumed harmlessly. */
            t->parse_state = TP_GROUND;
        } else {
            /* Unknown ESC sequence — ignore and return to ground. */
            t->parse_state = TP_GROUND;
        }
        break;

    case TP_CSI:
        if (ch >= '0' && ch <= '9') {
            t->param_partial = t->param_partial * 10 + (ch - '0');
        } else if (ch == ';') {
            if (t->num_params < TERM_MAX_PARAMS)
                t->params[t->num_params++] = t->param_partial;
            t->param_partial = 0;
        } else if (ch == '?' || ch == '>' || ch == '!') {
            /* Private mode prefix — store but we mostly ignore. */
        } else if (ch >= 0x40 && ch <= 0x7e) {
            /* Final byte. */
            if (t->num_params < TERM_MAX_PARAMS)
                t->params[t->num_params++] = t->param_partial;
            t->param_partial = 0;
            csi_dispatch(t, ch);
            t->parse_state = TP_GROUND;
        } else {
            /* Intermediate byte or unknown — ignore. */
        }
        break;

    case TP_OSC:
        if (ch == '\a') {
            /* ST (BEL) — end OSC, ignore content. */
            t->parse_state = TP_GROUND;
        } else if (ch == '\x1b') {
            t->parse_state = TP_OSC_ESC;
        } else {
            if (t->osc_len < (int)sizeof(t->osc_buf) - 1)
                t->osc_buf[t->osc_len++] = ch;
        }
        break;

    case TP_OSC_ESC:
        if (ch == '\\') {
            /* ST (ESC \) — end OSC. */
            t->parse_state = TP_GROUND;
        } else {
            /* Not a valid ST, return to ground. */
            t->parse_state = TP_GROUND;
        }
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

TermState *term_emu_open(int rows, int cols) {
    TermState *t = calloc(1, sizeof(TermState));
    if (!t) return NULL;

    t->rows = rows;
    t->cols = cols;
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
    t->parse_state = TP_GROUND;

    /* Allocate cell grid. */
    t->cells = calloc(rows * cols, sizeof(TermCell));
    if (!t->cells) { free(t); return NULL; }
    for (int i = 0; i < rows * cols; i++)
        t->cells[i] = blank_cell();

    /* Allocate scrollback. */
    t->sb_cap = SCROLLBACK_MAX;
    t->scrollback = calloc(t->sb_cap * cols, sizeof(TermCell));

    /* Fork with PTY. */
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    pid_t pid = forkpty(&t->pty_fd, NULL, NULL, &ws);
    if (pid < 0) {
        free(t->cells);
        free(t->scrollback);
        free(t);
        return NULL;
    }

    if (pid == 0) {
        /* Child: exec shell. */
        const char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/sh";
        setenv("TERM", "xterm-256color", 1);
        execlp(shell, shell, (char *)NULL);
        _exit(127);
    }

    /* Parent. */
    t->child_pid = pid;

    /* Make PTY non-blocking. */
    int flags = fcntl(t->pty_fd, F_GETFL);
    if (flags >= 0) fcntl(t->pty_fd, F_SETFL, flags | O_NONBLOCK);

    return t;
}

void term_emu_close(TermState *t) {
    if (!t) return;
    if (t->pty_fd >= 0) close(t->pty_fd);
    if (t->child_pid > 0 && !t->exited) {
        kill(t->child_pid, SIGHUP);
        waitpid(t->child_pid, NULL, WNOHANG);
    }
    free(t->cells);
    free(t->scrollback);
    free(t);
}

void term_emu_resize(TermState *t, int rows, int cols) {
    if (!t || (rows == t->rows && cols == t->cols)) return;

    TermCell *newcells = calloc(rows * cols, sizeof(TermCell));
    if (!newcells) return;
    for (int i = 0; i < rows * cols; i++)
        newcells[i] = blank_cell();

    /* Copy old content. */
    int copy_rows = (rows < t->rows) ? rows : t->rows;
    int copy_cols = (cols < t->cols) ? cols : t->cols;
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++)
            newcells[r * cols + c] = t->cells[r * t->cols + c];

    free(t->cells);
    t->cells = newcells;
    t->rows = rows;
    t->cols = cols;
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
    clamp_cursor(t);

    /* Resize scrollback lines to new width. */
    free(t->scrollback);
    t->scrollback = calloc(t->sb_cap * cols, sizeof(TermCell));
    t->sb_lines = 0; t->sb_start = 0; t->sb_offset = 0;

    /* Notify child of new size. */
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    ioctl(t->pty_fd, TIOCSWINSZ, &ws);
}

int term_emu_read(TermState *t) {
    if (!t || t->pty_fd < 0) return -1;

    /* Check if child has exited. */
    if (!t->exited) {
        int status;
        pid_t ret = waitpid(t->child_pid, &status, WNOHANG);
        if (ret == t->child_pid) {
            t->exited = 1;
            t->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }

    char buf[4096];
    int total = 0;
    for (;;) {
        ssize_t nr = read(t->pty_fd, buf, sizeof(buf));
        if (nr <= 0) {
            if (nr == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                return total > 0 ? total : -1;
            break;
        }
        for (ssize_t i = 0; i < nr; i++)
            process_byte(t, (unsigned char)buf[i]);
        total += nr;
    }
    return total;
}

void term_emu_write(TermState *t, const char *data, int len) {
    if (!t || t->pty_fd < 0 || len <= 0) return;
    /* Write all bytes, handling partial writes. */
    int off = 0;
    while (off < len) {
        ssize_t nw = write(t->pty_fd, data + off, len - off);
        if (nw <= 0) break;
        off += nw;
    }
}

TermCell term_emu_cell(const TermState *t, int row, int col) {
    if (!t || row < 0 || row >= t->rows || col < 0 || col >= t->cols)
        return blank_cell();
    return t->cells[row * t->cols + col];
}
