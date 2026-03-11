// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef UTILS_H
#define UTILS_H

void die(const char *s);

/* Shell-quote s into out[outlen] with single quotes. */
void shell_quote(const char *s, char *out, int outlen);

#endif
