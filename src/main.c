// SPDX-License-Identifier: GPL-3.0-or-later
#include "editor.h"
#include "input.h"
#include "render.h"
#include "lua_bridge.h"
#include "terminal.h"
#include "cli.h"

#include <signal.h>

static volatile int g_resized = 0;
static void handle_sigwinch(int s) { (void)s; g_resized = 1; }

int main(int argc, char *argv[]) {
    editor_init();
    lua_bridge_init();

    int start_line = 0, readonly = 0;
    int cli_ret = cli_dispatch(argc, argv, &start_line, &readonly);

    if (cli_ret == -2)
        return lua_bridge_cli(argv[1], argc, argv);
    if (cli_ret >= 0)
        return cli_ret;

    term_enable_raw_mode();
    signal(SIGWINCH, handle_sigwinch);
    editor_watch_init();
    E.readonly = readonly;

    const char *file_arg = editor_find_file_arg(argc, argv);
    if (file_arg) {
        editor_open_file_arg(file_arg);
        editor_watch_add(E.cur_buftab);
    }

    if (start_line > 0 && E.buf.numrows > 0) {
        E.cy = start_line - 1;
        if (E.cy >= E.buf.numrows) E.cy = E.buf.numrows - 1;
    }

    while (1) {
        if (g_resized) { g_resized = 0; editor_handle_resize(); }
        editor_drain_terminals();
        editor_reap_terminals();
        editor_refresh_screen();
        editor_poll_for_input();
    }

    return 0;
}
