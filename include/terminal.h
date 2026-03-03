// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TERMINAL_H
#define TERMINAL_H

void term_enable_raw_mode(void);
void term_disable_raw_mode(void);
int  term_get_size(int *rows, int *cols);

#endif
