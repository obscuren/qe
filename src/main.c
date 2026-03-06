// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "lua_bridge.h"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile int g_resized = 0;
static void handle_sigwinch(int s) { (void)s; g_resized = 1; }

int main(int argc, char *argv[]) {
    term_enable_raw_mode();
    editor_init();
    lua_bridge_init();

    signal(SIGWINCH, handle_sigwinch);

    if (argc >= 2) {
        struct stat st;
        if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) {
            if (chdir(argv[1]) == 0)
                editor_open_tree();
        } else {
            buf_open(&E.buf, argv[1]);
            editor_detect_syntax();
        }
    }

    while (1) {
        if (g_resized) {
            g_resized = 0;
            int nr, nc;
            if (term_get_size(&nr, &nc) == 0) {
                E.term_rows = nr; E.term_cols = nc;
                /* Collapse to one pane on resize (simple, safe). */
                E.num_panes = 1; E.cur_pane = 0;
                E.panes[0].top    = 1;
                E.panes[0].left   = 1;
                E.panes[0].height = nr - 2;
                E.panes[0].width  = nc;
                E.screenrows      = nr - 2;
                E.screencols      = nc;
            }
        }
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
