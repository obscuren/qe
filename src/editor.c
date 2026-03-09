// SPDX-License-Identifier: GPL-3.0-or-later
#include "editor.h"
#include "git.h"
#include "input.h"
#include "terminal.h"
#include "term_emu.h"
#include "utils.h"

#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

EditorConfig E;

void editor_init(void) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.mode = MODE_NORMAL;
    E.readonly     = 0;
    E.cmdbuf[0]    = '\0';
    E.cmdlen       = 0;
    E.statusmsg[0] = '\0';
    E.statusmsg_is_error = 0;
    E.opts.line_numbers   = 1;
    E.opts.autoindent     = 1;
    E.opts.tabwidth       = 4;
    E.opts.fuzzy_width_pct = 40;
    E.opts.qf_height_rows  = 8;
    E.opts.term_height_rows = 8;
    E.opts.autopairs        = 1;

    buf_init(&E.buf);

    if (term_get_size(&E.term_rows, &E.term_cols) == -1) {
        E.term_rows = 24;  /* sensible defaults for non-interactive use */
        E.term_cols = 80;
    }

    E.screenrows = E.term_rows - 2;  /* status bar + command bar */
    E.screencols = E.term_cols;

    E.num_panes        = 1;
    E.cur_pane         = 0;
    E.pending_ctrlw    = 0;
    E.pending_g        = 0;
    E.pending_leader   = 0;
    E.leader_char      = ' ';
    E.last_content_pane = 0;
    E.panes[0] = (Pane){ .top=1, .left=1,
                         .height=E.screenrows, .width=E.screencols,
                         .buf_idx=0 };

    E.undo_stack.count = 0;
    E.redo_stack.count = 0;
    E.has_pre_insert   = 0;

    E.searchbuf[0]      = '\0';
    E.searchlen         = 0;
    E.last_search_valid = 0;

    E.syntax = NULL;
    E.match_bracket_valid = 0;

    E.visual_anchor_row = 0;
    E.visual_anchor_col = 0;

    E.pending_op    = '\0';
    E.count         = 0;

    E.last_action.type        = LA_NONE;
    E.last_action.text        = NULL;
    E.last_action.text_len    = 0;
    E.last_action.find_target = '\0';
    E.last_find_key           = '\0';
    E.last_find_target        = '\0';
    E.insert_rec           = NULL;
    E.insert_rec_len       = 0;
    E.insert_rec_cap       = 0;
    E.insert_entry         = 'i';
    E.insert_motion        = 0;
    E.insert_count         = 1;
    E.is_replaying         = 0;

    memset(E.regs, 0, sizeof(E.regs));
    E.pending_reg    = -1;
    memset(E.macros, 0, sizeof(E.macros));
    E.recording_reg  = -1;
    E.macro_buf      = NULL;
    E.macro_len      = 0;
    E.macro_cap      = 0;
    E.last_macro_reg = -1;
    E.macro_playing  = 0;

    E.completion_matches = NULL;
    E.completion_count   = 0;
    E.completion_idx     = -1;

    E.jump_count = 0;
    E.jump_cur   = 0;

    for (int i = 0; i < MARK_MAX; i++) E.marks[i].valid = 0;
    E.pending_mark = 0;

    E.num_buftabs = 1;
    E.cur_buftab  = 0;

    E.git_branch[0]    = '\0';
    E.pending_bracket  = 0;
    E.pending_leader_h = 0;
    E.inotify_fd       = -1;
    E.watch_prompt_buf = -1;
    git_current_branch(E.git_branch, sizeof(E.git_branch));
}

void editor_detect_syntax(void) {
    E.syntax             = syntax_detect(E.buf.filename);
    E.buf.hl_dirty_from  = 0;   /* force full open-comment recompute */
}

void editor_update_git_signs(void) {
    free(E.buf.git_signs);
    E.buf.git_signs       = NULL;
    E.buf.git_signs_count = 0;
    if (!E.buf.filename || E.buf.numrows <= 0) return;

    /* Build temporary arrays for the git module. */
    const char **chars = malloc(sizeof(char *) * E.buf.numrows);
    int         *lens  = malloc(sizeof(int)    * E.buf.numrows);
    if (!chars || !lens) { free(chars); free(lens); return; }
    for (int i = 0; i < E.buf.numrows; i++) {
        chars[i] = E.buf.rows[i].chars;
        lens[i]  = E.buf.rows[i].len;
    }
    E.buf.git_signs = git_diff_signs(E.buf.filename, chars, lens, E.buf.numrows);
    E.buf.git_signs_count = E.buf.git_signs ? E.buf.numrows : 0;
    free(chars);
    free(lens);
}

UndoState editor_capture_state(void) {
    UndoState s;
    s.numrows   = E.buf.numrows;
    s.cx        = E.cx;
    s.cy        = E.cy;
    /* Allocate at least 1 slot so malloc(0) is never called. */
    s.row_chars = malloc(sizeof(char *) * (s.numrows > 0 ? s.numrows : 1));
    s.row_lens  = malloc(sizeof(int)    * (s.numrows > 0 ? s.numrows : 1));
    for (int i = 0; i < s.numrows; i++) {
        s.row_chars[i] = strdup(E.buf.rows[i].chars);
        s.row_lens[i]  = E.buf.rows[i].len;
    }
    return s;
}

/* Save the live EditorConfig buffer state into buftabs[i].
   Uses move semantics: ownership of heap data transfers to the BufTab. */
void editor_buf_save(int i) {
    BufTab *t = &E.buftabs[i];

    /* Commit or discard any pending pre-insert snapshot. */
    if (E.has_pre_insert) {
        if (E.buf.dirty != E.pre_insert_dirty)
            undo_push(&E.undo_stack, E.pre_insert_snapshot);
        else
            undo_state_free(&E.pre_insert_snapshot);
        memset(&E.pre_insert_snapshot, 0, sizeof(UndoState));
        E.has_pre_insert = 0;
    }

    /* Move buffer — struct copy transfers pointer ownership; zero source. */
    t->buf = E.buf;
    memset(&E.buf, 0, sizeof(Buffer));
    E.buf.hl_dirty_from = INT_MAX;

    t->cx = E.cx; t->cy = E.cy;
    t->rowoff = E.rowoff; t->coloff = E.coloff;

    /* Move undo stacks — copy fixed array, zero source count. */
    t->undo_stack = E.undo_stack;  E.undo_stack.count = 0;
    t->redo_stack = E.redo_stack;  E.redo_stack.count = 0;

    t->pre_insert_snapshot = E.pre_insert_snapshot;
    t->pre_insert_dirty    = E.pre_insert_dirty;
    t->has_pre_insert      = E.has_pre_insert;   /* always 0 at this point */

    t->syntax = E.syntax;
}

/* Restore buftabs[i] into the live EditorConfig buffer state. */
void editor_buf_restore(int i) {
    BufTab *t = &E.buftabs[i];

    E.buf = t->buf;
    memset(&t->buf, 0, sizeof(Buffer));

    E.cx = t->cx; E.cy = t->cy;
    E.rowoff = t->rowoff; E.coloff = t->coloff;

    E.undo_stack = t->undo_stack;  t->undo_stack.count = 0;
    E.redo_stack = t->redo_stack;  t->redo_stack.count = 0;

    E.pre_insert_snapshot = t->pre_insert_snapshot;
    E.pre_insert_dirty    = t->pre_insert_dirty;
    E.has_pre_insert      = t->has_pre_insert;
    memset(&t->pre_insert_snapshot, 0, sizeof(UndoState));
    t->has_pre_insert = 0;

    E.syntax = t->syntax;
}

/* ── Main loop helpers ────────────────────────────────────────────── */
#ifndef QE_TEST

const char *editor_find_file_arg(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+' || strcmp(argv[i], "-R") == 0) continue;
        return argv[i];
    }
    return NULL;
}

void editor_open_file_arg(const char *file_arg) {
    struct stat st;
    if (stat(file_arg, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (chdir(file_arg) == 0)
            editor_open_tree();
    } else {
        buf_open(&E.buf, file_arg);
        editor_detect_syntax();
    }
}

void editor_handle_resize(void) {
    int nr, nc;
    if (term_get_size(&nr, &nc) != 0) return;
    E.term_rows = nr; E.term_cols = nc;
    E.num_panes = 1; E.cur_pane = 0;
    E.panes[0].top    = 1;
    E.panes[0].left   = 1;
    E.panes[0].height = nr - 2;
    E.panes[0].width  = nc;
    E.screenrows      = nr - 2;
    E.screencols      = nc;
}

void editor_drain_terminals(void) {
    for (int i = 0; i < E.num_buftabs; i++) {
        if (!E.buftabs[i].is_term || !E.buftabs[i].term) continue;
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

int editor_poll_for_input(void) {
    struct pollfd pfds[2 + MAX_BUFS];
    int nfds = 0;
    int inotify_slot = -1;
    pfds[nfds++] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
    if (E.inotify_fd >= 0) {
        inotify_slot = nfds;
        pfds[nfds++] = (struct pollfd){ .fd = E.inotify_fd, .events = POLLIN };
    }
    for (int i = 0; i < E.num_buftabs; i++) {
        if (E.buftabs[i].is_term && E.buftabs[i].term
            && E.buftabs[i].term->pty_fd >= 0)
            pfds[nfds++] = (struct pollfd){
                .fd = E.buftabs[i].term->pty_fd, .events = POLLIN };
    }

    int pr = poll(pfds, nfds, -1);
    if (pr <= 0) return 0;

    if (inotify_slot >= 0 && (pfds[inotify_slot].revents & POLLIN))
        editor_watch_drain();
    if (pfds[0].revents & POLLIN)
        editor_process_keypress();
    return 1;
}

/* ── File watching (inotify) ──────────────────────────────────────── */

void editor_watch_init(void) {
    E.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    /* -1 on failure is fine — watching is best-effort */
}

void editor_watch_add(int buftab_idx) {
    if (E.inotify_fd < 0) return;
    BufTab *bt = &E.buftabs[buftab_idx];

    /* Only watch regular file buffers. */
    const char *path = (buftab_idx == E.cur_buftab)
                       ? E.buf.filename : bt->buf.filename;
    if (!path || buftab_is_special(bt)) { bt->watch_wd = -1; return; }

    /* Remove existing watch if any. */
    if (bt->watch_wd >= 0) {
        inotify_rm_watch(E.inotify_fd, bt->watch_wd);
        bt->watch_wd = -1;
    }

    bt->watch_wd = inotify_add_watch(E.inotify_fd, path,
                                      IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF);
    bt->file_changed = 0;
}

void editor_watch_remove(int buftab_idx) {
    if (E.inotify_fd < 0) return;
    BufTab *bt = &E.buftabs[buftab_idx];
    if (bt->watch_wd >= 0) {
        inotify_rm_watch(E.inotify_fd, bt->watch_wd);
        bt->watch_wd = -1;
    }
    bt->file_changed = 0;
}

void editor_watch_drain(void) {
    if (E.inotify_fd < 0) return;

    /* Read all pending inotify events. */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(E.inotify_fd, buf, sizeof(buf));
        if (n <= 0) break;

        for (char *ptr = buf; ptr < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            ptr += sizeof(*ev) + ev->len;

            /* Find the buftab that owns this watch descriptor. */
            for (int i = 0; i < E.num_buftabs; i++) {
                if (E.buftabs[i].watch_wd == ev->wd) {
                    /* Skip events caused by our own save. */
                    if ((ev->mask & IN_MODIFY) && E.buftabs[i].watch_skip > 0) {
                        E.buftabs[i].watch_skip--;
                        break;
                    }
                    E.buftabs[i].file_changed = 1;

                    /* Deleted/moved: the watch is auto-removed by the kernel. */
                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
                        E.buftabs[i].watch_wd = -1;
                    break;
                }
            }
        }
    }

    /* Show reload prompt for the first changed buffer found.
       Only one prompt at a time — remaining changes queued via file_changed. */
    if (E.watch_prompt_buf >= 0) return;  /* already prompting */

    for (int i = 0; i < E.num_buftabs; i++) {
        if (!E.buftabs[i].file_changed) continue;
        E.buftabs[i].file_changed = 0;

        const char *fname = (i == E.cur_buftab) ? E.buf.filename
                                                 : E.buftabs[i].buf.filename;
        if (!fname) continue;
        const char *base = strrchr(fname, '/');
        base = base ? base + 1 : fname;

        E.watch_prompt_buf = i;
        E.statusmsg_is_error = 1;
        snprintf(E.statusmsg, sizeof(E.statusmsg),
                 "W: \"%s\" changed on disk. [O]K, (L)oad File: ", base);

        /* Re-add watch so future changes are also detected. */
        editor_watch_add(i);
        break;  /* handle one at a time */
    }
}

void editor_reload_buf(int buftab_idx) {
    if (buftab_idx == E.cur_buftab) {
        /* Live buffer: save filename, free, reopen. */
        char *fname = strdup(E.buf.filename);
        int saved_cy = E.cy, saved_cx = E.cx;

        buf_free(&E.buf);
        buf_open(&E.buf, fname);
        E.buf.hl_dirty_from = 0;
        free(fname);

        if (saved_cy >= E.buf.numrows)
            saved_cy = E.buf.numrows > 0 ? E.buf.numrows - 1 : 0;
        E.cy = saved_cy;
        E.cx = saved_cx;
        if (E.cy < E.buf.numrows && E.cx > E.buf.rows[E.cy].len)
            E.cx = E.buf.rows[E.cy].len > 0 ? E.buf.rows[E.cy].len - 1 : 0;
    } else {
        /* Parked buffer. */
        Buffer *b = &E.buftabs[buftab_idx].buf;
        char *fname = strdup(b->filename);

        buf_free(b);
        buf_open(b, fname);
        b->hl_dirty_from = 0;
        free(fname);

        if (E.buftabs[buftab_idx].cy >= b->numrows)
            E.buftabs[buftab_idx].cy = b->numrows > 0 ? b->numrows - 1 : 0;
    }

    editor_watch_add(buftab_idx);
}

#endif /* QE_TEST */

/* ── Undo state ──────────────────────────────────────────────────── */

void editor_restore_state(const UndoState *s) {
    for (int i = 0; i < E.buf.numrows; i++)
        free(E.buf.rows[i].chars);
    free(E.buf.rows);

    E.buf.numrows = s->numrows;
    if (s->numrows > 0) {
        E.buf.rows = malloc(sizeof(Row) * s->numrows);
        for (int i = 0; i < s->numrows; i++) {
            E.buf.rows[i].chars          = strdup(s->row_chars[i]);
            E.buf.rows[i].len            = s->row_lens[i];
            E.buf.rows[i].hl             = NULL;
            E.buf.rows[i].hl_open_comment = 0;
        }
    } else {
        E.buf.rows = NULL;
    }

    E.cx                 = s->cx;
    E.cy                 = s->cy;
    E.buf.dirty          = 1;
    E.buf.hl_dirty_from  = 0;   /* restored content needs re-highlighting */
}
