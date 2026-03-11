// SPDX-License-Identifier: GPL-3.0-or-later
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void shell_quote(const char *s, char *out, int outlen) {
    int j = 0;
    if (j < outlen - 1) out[j++] = '\'';
    for (int i = 0; s[i] && j < outlen - 5; i++) {
        if (s[i] == '\'') {
            out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\'';
        } else {
            out[j++] = s[i];
        }
    }
    if (j < outlen - 1) out[j++] = '\'';
    out[j] = '\0';
}

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[?1000l\x1b[?1006l", 16); /* disable mouse */
    write(STDOUT_FILENO, "\x1b[?1049l", 8);              /* leave alt screen */
    write(STDOUT_FILENO, "\x1b[0 q", 5);                 /* reset cursor shape */
    perror(s);
    exit(EXIT_FAILURE);
}
