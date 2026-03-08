// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include "claude.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "lua_bridge.h"
#include "term_emu.h"
#include "cli.h"

#include <poll.h>
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

        /* Resize + drain PTY output from all terminal panes. */
        for (int i = 0; i < E.num_buftabs; i++) {
            if (E.buftabs[i].is_term && E.buftabs[i].term) {
                TermState *ts = E.buftabs[i].term;
                for (int pi = 0; pi < E.num_panes; pi++) {
                    if (E.panes[pi].buf_idx == i) {
                        Pane *p = &E.panes[pi];
                        if (ts->rows != p->height || ts->cols != p->width)
                            term_emu_resize(ts, p->height, p->width);
                        break;
                    }
                }
                term_emu_read(ts);
            }
        }

        /* Auto-close terminal panes whose shell has exited. */
        editor_reap_terminals();

        editor_refresh_screen();

        /* Drain Claude streaming responses. */
        for (int i = 0; i < E.num_buftabs; i++) {
            if (E.buftabs[i].is_claude && E.buftabs[i].claude
                && !E.buftabs[i].claude->done
                && E.buftabs[i].claude->pipe_fd >= 0) {
                ClaudeState *cs = E.buftabs[i].claude;
                int got = claude_read_stream(cs);
                if (got != 0) {
                    /* Find the prompt text from the first row of the buffer. */
                    const char *prompt = "...";
                    Buffer *cbuf = (i == E.cur_buftab)
                                   ? &E.buf : &E.buftabs[i].buf;
                    if (cbuf->numrows > 0 && cbuf->rows[0].len > 5
                        && strncmp(cbuf->rows[0].chars, "You: ", 5) == 0)
                        prompt = cbuf->rows[0].chars + 5;
                    claude_update_buf(cbuf, cs, prompt);
                    /* Auto-scroll to bottom if Claude pane is active. */
                    if (i == E.cur_buftab) {
                        E.cy = E.buf.numrows > 0 ? E.buf.numrows - 1 : 0;
                        E.cx = 0;
                    }
                }
            }
        }

        /* Build poll set: stdin + all terminal PTY fds + Claude pipe fds. */
        struct pollfd pfds[1 + MAX_BUFS * 2];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
        for (int i = 0; i < E.num_buftabs; i++) {
            if (E.buftabs[i].is_term && E.buftabs[i].term
                && E.buftabs[i].term->pty_fd >= 0)
                pfds[nfds++] = (struct pollfd){
                    .fd = E.buftabs[i].term->pty_fd, .events = POLLIN };
        }
        /* Add Claude pipe fds so poll wakes on streaming data. */
        int has_claude_stream = 0;
        for (int i = 0; i < E.num_buftabs; i++) {
            if (E.buftabs[i].is_claude && E.buftabs[i].claude
                && !E.buftabs[i].claude->done
                && E.buftabs[i].claude->pipe_fd >= 0) {
                pfds[nfds++] = (struct pollfd){
                    .fd = E.buftabs[i].claude->pipe_fd, .events = POLLIN };
                has_claude_stream = 1;
            }
        }

        /* Wait for stdin OR any PTY/Claude output (or signal).
           Use short timeout when streaming to keep UI responsive. */
        int timeout = has_claude_stream ? 50 : -1;
        int pr = poll(pfds, nfds, timeout);
        if (pr <= 0) continue;  /* signal interrupted or error — just re-loop */

        /* If stdin has data, process one keypress. */
        if (pfds[0].revents & POLLIN)
            editor_process_keypress();
        /* PTY/Claude data (if any) will be drained at the top of the next iteration. */
    }

    return 0;
}
