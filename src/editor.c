// SPDX-License-Identifier: GPL-3.0-or-later
#include "editor.h"
#include "terminal.h"
#include "utils.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

EditorConfig E;

void editor_init(void) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.mode = MODE_NORMAL;
    E.cmdbuf[0]    = '\0';
    E.cmdlen       = 0;
    E.statusmsg[0] = '\0';
    E.opts.line_numbers = 1;
    E.opts.autoindent   = 1;
    E.opts.tabwidth     = 4;

    buf_init(&E.buf);

    if (term_get_size(&E.screenrows, &E.screencols) == -1)
        die("term_get_size");

    E.screenrows -= 2;  /* status bar + command bar */

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

    E.yank_rows     = NULL;
    E.yank_numrows  = 0;
    E.yank_linewise = 0;
}

void editor_detect_syntax(void) {
    E.syntax             = syntax_detect(E.buf.filename);
    E.buf.hl_dirty_from  = 0;   /* force full open-comment recompute */
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
