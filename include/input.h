// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INPUT_H
#define INPUT_H

enum EditorKey {
    ARROW_LEFT  = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY,
    MOUSE_PRESS,
    MOUSE_SCROLL_UP,
    MOUSE_SCROLL_DOWN,
    LEADER_BASE = 2000, /* leader sequences: LEADER_BASE + char */
};

int  editor_read_key(void);
void editor_process_keypress(void);
void editor_execute_command(void);
void editor_open_tree(void);    /* open (or toggle) the file-tree sidebar */

#endif
