// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include "utils.h"

#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

static struct termios orig_termios;

void term_disable_raw_mode(void) {
    write(STDOUT_FILENO, "\x1b[?1000l\x1b[?1006l", 16); /* disable mouse */
    write(STDOUT_FILENO, "\x1b[?1049l", 8);              /* leave alt screen */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
}

void term_enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
        die("tcgetattr");

    atexit(term_disable_raw_mode);

    struct termios raw = orig_termios;

    /* input: no break signal, no CR→NL, no parity check, no strip, no flow ctrl */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output: no post-processing */
    raw.c_oflag &= ~(OPOST);
    /* 8-bit chars */
    raw.c_cflag |= (CS8);
    /* local: no echo, no canonical, no signals, no extension */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    /* read() returns after 1 byte or 100ms timeout */
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");

    write(STDOUT_FILENO, "\x1b[?1049h", 8);              /* enter alt screen */
    write(STDOUT_FILENO, "\x1b[?1000h\x1b[?1006h", 16); /* enable mouse tracking + SGR mode */
}

int term_get_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}
