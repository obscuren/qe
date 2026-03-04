// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef EDITOR_H
#define EDITOR_H

#include "buf.h"
#include "undo.h"
#include "search.h"
#include "syntax.h"

typedef enum {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_COMMAND,
    MODE_SEARCH,
    MODE_VISUAL,       /* characterwise visual selection */
    MODE_VISUAL_LINE,  /* linewise visual selection      */
} EditorMode;

/* All user-configurable settings live here so they can later be
   exposed as a single table to Lua via qe.set_option(). */
typedef struct {
    int line_numbers;   /* 1 = show, 0 = hide */
    int autoindent;     /* 1 = copy indentation on Enter/o/O */
    int tabwidth;       /* spaces inserted by Tab key (default 4) */
} EditorOptions;

typedef struct {
    int           cx, cy;           /* cursor position in file coordinates */
    int           rowoff, coloff;   /* scroll offsets */
    int           screenrows;
    int           screencols;
    EditorMode    mode;
    Buffer        buf;
    EditorOptions opts;
    char          cmdbuf[256];      /* command-line input */
    int           cmdlen;
    char          statusmsg[128];   /* one-shot message shown in command bar */

    /* Search */
    char          searchbuf[256];   /* live input while in MODE_SEARCH */
    int           searchlen;
    SearchQuery   last_query;       /* last executed query — used by n / N */
    int           last_search_valid; /* 1 when last_query holds a valid query */

    /* Undo / redo */
    UndoStack     undo_stack;
    UndoStack     redo_stack;
    UndoState     pre_insert_snapshot; /* captured on entering insert mode */
    int           pre_insert_dirty;    /* buf.dirty value at that moment */
    int           has_pre_insert;      /* 1 when snapshot is valid */

    /* Syntax highlighting */
    const SyntaxDef *syntax;           /* NULL = no highlighting */

    /* Bracket match (recomputed each frame) */
    int match_bracket_valid;
    int match_bracket_row;
    int match_bracket_col;

    /* Visual mode anchor */
    int visual_anchor_row;
    int visual_anchor_col;

    /* Operator-pending state (normal mode) */
    char  pending_op;      /* 'd' or 'y' when waiting for motion; '\0' = none */
    int   count;           /* accumulated count prefix (0 = none typed yet)   */

    /* . repeat — last repeatable action */
    struct {
        enum {
            LA_NONE, LA_X, LA_DELETE, LA_CHANGE, LA_PASTE, LA_INSERT, LA_OPEN,
        } type;
        int   count;
        int   motion;       /* LA_DELETE / LA_CHANGE: motion key                */
        char  find_target;  /* LA_DELETE / LA_CHANGE when motion is f/F/t/T    */
        int   before;       /* LA_PASTE: 1 = P (before), 0 = p (after)         */
        char  entry;        /* LA_INSERT: 'i','a','A'; LA_OPEN: 'o','O'         */
        int   open_above;   /* LA_OPEN: 1 = O, 0 = o                           */
        char *text;         /* LA_CHANGE / LA_INSERT / LA_OPEN: malloc'd text   */
        int   text_len;
    } last_action;

    /* Last f/F/t/T search (for ; and , repeat) */
    char last_find_key;    /* 'f', 'F', 't', 'T', or '\0' = none */
    char last_find_target; /* the character that was searched for */

    /* Insert-session recording (for . repeat) */
    char *insert_rec;
    int   insert_rec_len;
    int   insert_rec_cap;
    char  insert_entry;    /* which key entered insert: i/a/A/o/O/c            */
    int   insert_motion;   /* for 'c' entry: motion key                        */
    int   insert_count;    /* for 'c' entry: count used                        */
    int   is_replaying;    /* 1 while inside repeat_last_action()              */

    /* Yank register (internal clipboard) */
    char **yank_rows;
    int    yank_numrows;
    int    yank_linewise;  /* 1 = line-oriented (dd/yy), 0 = char-oriented */
} EditorConfig;

extern EditorConfig E;

void editor_init(void);
void editor_detect_syntax(void);

UndoState editor_capture_state(void);
void      editor_restore_state(const UndoState *s);

#endif
