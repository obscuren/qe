// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "lua_bridge.h"

int main(int argc, char *argv[]) {
    term_enable_raw_mode();
    editor_init();
    lua_bridge_init();

    if (argc >= 2) {
        buf_open(&E.buf, argv[1]);
        editor_detect_syntax();
    }

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
