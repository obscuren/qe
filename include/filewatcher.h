// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FILEWATCHER_H
#define FILEWATCHER_H

void filewatcher_init(void);
void filewatcher_add(int buftab_idx);
void filewatcher_remove(int buftab_idx);
void filewatcher_drain(void);

#endif
