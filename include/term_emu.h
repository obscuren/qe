// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TERM_EMU_H
#define TERM_EMU_H

#include <stdint.h>
#include <sys/types.h>

/* Per-cell color + attribute. */
typedef struct {
    char    ch;
    uint8_t fg;        /* 0 = default, 1-255 = 256-color index        */
    uint8_t bg;        /* 0 = default, 1-255 = 256-color index        */
    uint8_t bold : 1;
    uint8_t dim  : 1;
    uint8_t underline : 1;
    uint8_t reverse   : 1;
} TermCell;

/* Parser state for escape sequences. */
typedef enum {
    TP_GROUND,       /* normal text                           */
    TP_ESC,          /* just saw \x1b                         */
    TP_CSI,          /* in CSI sequence (\x1b[)               */
    TP_OSC,          /* in OSC sequence (\x1b])               */
    TP_OSC_ESC,      /* OSC got \x1b, expecting \\            */
} TermParseState;

#define TERM_MAX_PARAMS 16

typedef struct {
    /* PTY / child process. */
    int       pty_fd;
    pid_t     child_pid;
    int       exited;        /* 1 = child has exited                 */
    int       exit_status;

    /* Cell grid. */
    TermCell *cells;         /* flat array: cells[row * cols + col]  */
    int       rows, cols;

    /* Cursor. */
    int       c_row, c_col;

    /* Scroll region (0-based, inclusive). */
    int       scroll_top, scroll_bot;

    /* Current text attributes. */
    uint8_t   cur_fg, cur_bg;
    uint8_t   cur_bold, cur_dim, cur_underline, cur_reverse;

    /* Parser state. */
    TermParseState parse_state;
    int       params[TERM_MAX_PARAMS];
    int       num_params;
    int       param_partial;   /* current incomplete parameter digit */
    char      osc_buf[256];
    int       osc_len;

    /* Scrollback buffer. */
    TermCell *scrollback;      /* ring buffer of scrollback lines     */
    int       sb_lines;        /* number of lines in scrollback       */
    int       sb_cap;          /* max scrollback lines                */
    int       sb_start;        /* ring buffer start index             */
    int       sb_offset;       /* how many lines scrolled back (view) */
} TermState;

/* Create a terminal: fork shell, allocate grid. Returns NULL on failure. */
TermState *term_emu_open(int rows, int cols);

/* Destroy: kill child, free grid. */
void       term_emu_close(TermState *t);

/* Resize the terminal grid + PTY. */
void       term_emu_resize(TermState *t, int rows, int cols);

/* Non-blocking read from PTY and process output.
   Returns number of bytes processed, 0 if nothing available, -1 on error/EOF. */
int        term_emu_read(TermState *t);

/* Write raw bytes to the PTY (keystrokes). */
void       term_emu_write(TermState *t, const char *data, int len);

/* Access a cell in the grid (bounds-checked, returns blank cell if out of range). */
TermCell   term_emu_cell(const TermState *t, int row, int col);

#endif
