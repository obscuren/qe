// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "lua_bridge.h"
#include "cli.h"

#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile int g_resized = 0;
static void handle_sigwinch(int s) { (void)s; g_resized = 1; }

int main(int argc, char *argv[]) {
    /* Lightweight init for CLI subcommands (no raw mode / no editor). */
    editor_init();
    lua_bridge_init();

    int start_line = 0, readonly = 0;
    int cli_ret = cli_dispatch(argc, argv, &start_line, &readonly);

    if (cli_ret == -2) {
        /* Lua subcommand: dispatch through Lua bridge. */
        int exit_code = lua_bridge_cli(argv[1], argc, argv);
        return exit_code;
    }
    if (cli_ret >= 0) return cli_ret;

    /* Normal editor startup. */
    term_enable_raw_mode();
    signal(SIGWINCH, handle_sigwinch);

    E.readonly = readonly;

    /* Find the file argument (skip flags like +N and -R). */
    const char *file_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+' || strcmp(argv[i], "-R") == 0) continue;
        file_arg = argv[i];
        break;
    }

    if (file_arg) {
        struct stat st;
        if (stat(file_arg, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (chdir(file_arg) == 0)
                editor_open_tree();
        } else {
            buf_open(&E.buf, file_arg);
            editor_detect_syntax();
        }
    }

    /* Jump to +N line. */
    if (start_line > 0 && E.buf.numrows > 0) {
        E.cy = start_line - 1;
        if (E.cy >= E.buf.numrows) E.cy = E.buf.numrows - 1;
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
