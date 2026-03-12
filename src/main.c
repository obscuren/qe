// SPDX-License-Identifier: GPL-3.0-or-later
#include "editor.h"
#include "input.h"
#include "render.h"
#include "lua_bridge.h"
#include "terminal.h"
#include "cli.h"

#include <signal.h>
#include <string.h>

static volatile int g_resized = 0;
static void handle_sigwinch(int s) { (void)s; g_resized = 1; }

int main(int argc, char *argv[]) {
    editor_init();
    lua_bridge_init();

    int start_line = 0, readonly = 0;
    const char *session_file = NULL;
    int cli_ret = cli_dispatch(argc, argv, &start_line, &readonly, &session_file);

    if (cli_ret == -2)
        return lua_bridge_cli(argv[1], argc, argv);
    if (cli_ret >= 0)
        return cli_ret;

    term_enable_raw_mode();
    signal(SIGWINCH, handle_sigwinch);
    filewatcher_init();
    E.readonly = readonly;

    int is_diff = (argc >= 3 && strcmp(argv[1], "diff") == 0);

    if (session_file) {
        editor_load_session(session_file);
    } else {
        const char *file_arg = is_diff ? argv[2] : editor_find_file_arg(argc, argv);
        if (file_arg) {
            editor_open_file_arg(file_arg);
            filewatcher_add(E.cur_buftab);
        }

        if (start_line > 0 && E.buf.numrows > 0) {
            E.cy = start_line - 1;
            if (E.cy >= E.buf.numrows) E.cy = E.buf.numrows - 1;
        }
    }

    if (is_diff) diff_open();

    while (1) {
        if (g_resized) { g_resized = 0; editor_handle_resize(); }
        editor_drain_terminals();
        lua_bridge_drain_async();
        editor_reap_terminals();
        lua_bridge_reap_async();
        editor_refresh_screen();
        editor_poll_for_input();
    }

    return 0;
}
