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
void editor_open_fuzzy(void);   /* open the fuzzy file finder overlay     */
void editor_reap_terminals(void); /* close terminal panes whose shell exited */
void editor_load_session(const char *path); /* restore a saved session file */
void diff_open(void);   /* open the diff view for the current buffer */
void switch_to_buf(int idx);       /* switch live buffer slot        */
void clipboard_copy(int ri);       /* push register to system clipboard */
void clipboard_paste(int ri);      /* pull system clipboard into register */

#endif
