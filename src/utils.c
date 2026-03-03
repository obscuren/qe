// SPDX-License-Identifier: GPL-3.0-or-later
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J",  4);
    write(STDOUT_FILENO, "\x1b[H",   3);
    write(STDOUT_FILENO, "\x1b[0 q", 5);  /* reset cursor shape */
    perror(s);
    exit(EXIT_FAILURE);
}
