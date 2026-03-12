// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RECOVERY_H
#define RECOVERY_H

#include "buf.h"

/* Write a recovery snapshot of the buffer to .qe/recovery/<hash>.rec.
   Returns 0 on success, -1 on failure. */
int  recovery_save(const char *filepath, const Buffer *b, int cx, int cy);

/* Check if a recovery file exists and is newer than the file on disk.
   Returns 1 if recovery is available, 0 otherwise. */
int  recovery_exists(const char *filepath);

/* Load recovered content into the buffer. Sets dirty flag but does NOT
   write to disk. Returns 0 on success, -1 on failure. */
int  recovery_load(const char *filepath, Buffer *b, int *cx, int *cy);

/* Delete the recovery file for the given filepath. */
void recovery_remove(const char *filepath);

/* Write recovery files for all dirty buffers. Called periodically from
   the main loop poll timeout. */
void recovery_tick(void);

#endif
