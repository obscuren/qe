// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef EDITOR_H
#define EDITOR_H

#include "buf.h"
#include "fuzzy.h"
#include "git.h"
#include "qf.h"
#include "undo.h"
#include "search.h"
#include "syntax.h"
#include "tree.h"

#define MAX_BUFS  32
#define MAX_PANES  8
#define JUMP_MAX  100
#define MARK_MAX  26

typedef struct { int buf_idx, row, col; } JumpEntry;
typedef struct { int valid, buf_idx, row, col; } Mark;

typedef struct {
    int top, left;      /* 1-based terminal row/col of first content row/col */
    int height, width;  /* content rows (excludes status bar row); columns   */
    int buf_idx;        /* into buftabs[]; == E.cur_buftab means live E.buf  */
    int cx, cy;         /* saved cursor   (synced from E.* when active)      */
    int rowoff, coloff; /* saved scroll   (synced from E.* when active)      */
} Pane;

/* Parked state of an inactive buffer (stored while another buffer is live). */
typedef struct {
    Buffer    buf;
    int       cx, cy, rowoff, coloff;
    UndoStack undo_stack;
    UndoStack redo_stack;
    UndoState pre_insert_snapshot;
    int       pre_insert_dirty;
    int       has_pre_insert;
    const SyntaxDef *syntax;
    int        is_tree;   /* 1 = this slot holds the file-tree buffer */
    TreeState *tree;      /* non-NULL when is_tree == 1               */
    int        is_qf;     /* 1 = this slot holds the quickfix buffer  */
    QfList    *qf;        /* non-NULL when is_qf == 1                 */
    int        is_blame;  /* 1 = this slot holds a git blame buffer   */
    int        blame_source_buf; /* buf_idx of the source file (scroll sync) */
    int        is_diff;   /* 1 = this slot holds a HEAD diff buffer   */
    int        diff_source_buf;  /* buf_idx of the working file (scroll sync) */
    int        is_commit; /* 1 = this slot holds a git commit message buffer */
    int        is_show;   /* 1 = this slot holds a git-show commit buffer    */
    int        is_log;    /* 1 = this slot holds the git log buffer          */
    GitLogEntry *log_entries; /* non-NULL when is_log == 1                   */
    int         log_count;
} BufTab;

typedef enum {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_COMMAND,
    MODE_SEARCH,
    MODE_VISUAL,       /* characterwise visual selection */
    MODE_VISUAL_LINE,  /* linewise visual selection      */
    MODE_FUZZY,        /* fuzzy file finder overlay      */
} EditorMode;

/* All user-configurable settings live here so they can later be
   exposed as a single table to Lua via qe.set_option(). */
typedef struct {
    int line_numbers;   /* 1 = show, 0 = hide */
    int autoindent;     /* 1 = copy indentation on Enter/o/O */
    int tabwidth;       /* spaces inserted by Tab key (default 4) */
    int fuzzy_width_pct;  /* fuzzy panel width as % of terminal (default 40) */
    int qf_height_rows;   /* quickfix pane height in rows (default 8)        */
} EditorOptions;

typedef struct {
    int           cx, cy;           /* cursor position in file coordinates */
    int           rowoff, coloff;   /* scroll offsets */
    int           screenrows;
    int           screencols;
    EditorMode    mode;
    int           readonly;       /* 1 = -R flag, blocks editing commands      */
    Buffer        buf;
    EditorOptions opts;
    char          cmdbuf[256];      /* command-line input */
    int           cmdlen;
    char          statusmsg[128];   /* one-shot message shown in command bar */
    int           statusmsg_is_error; /* 1 = display in red */

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
            LA_INDENT, LA_REPLACE,
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
    int   insert_find_target; /* for 'c' + text-obj: the object char           */

    /* Sticky column: remembered cx target for j/k vertical navigation */
    int   preferred_col;
    int   is_replaying;    /* 1 while inside repeat_last_action()              */

    /* Yank registers: 0 = unnamed (""), 1-26 = named (a-z), 27 = clipboard ("+) */
    #define REG_COUNT     28
    #define REG_CLIPBOARD 27
    struct {
        char **rows;
        int    numrows;
        int    linewise;   /* 1 = line-oriented (dd/yy), 0 = char-oriented */
    } regs[REG_COUNT];
    int    pending_reg;    /* -1 = none; 0 = unnamed; 1-26 = a-z; 27 = clipboard */

    /* Macro recording / playback */
    #define MACRO_REGS 26
    struct {
        int *keys;
        int  len;
    } macros[MACRO_REGS];         /* a-z: index 0-25                       */
    int    recording_reg;  /* -1 = not recording; 0-25 = recording into a-z */
    int   *macro_buf;      /* key buffer during recording                   */
    int    macro_len;
    int    macro_cap;
    int    last_macro_reg; /* register used by last @x (for @@); -1=none   */
    int    macro_playing;  /* >0 = currently replaying a macro             */

    /* Tab completion (command mode :e) */
    char **completion_matches;
    int    completion_count;
    int    completion_idx;   /* -1 = inactive, >= 0 = index of selected match */

    /* Multi-buffer */
    BufTab buftabs[MAX_BUFS]; /* parked state of all inactive buffers */
    int    num_buftabs;       /* total open buffers including the live one */
    int    cur_buftab;        /* 0-based index of the live buffer */

    /* Split panes */
    Pane   panes[MAX_PANES];
    int    num_panes;         /* 1 = single pane (startup default) */
    int    cur_pane;          /* index of active pane              */
    int    pending_ctrlw;     /* 1 = waiting for Ctrl-W second key */
    int    pending_g;         /* 1 = waiting for g second key      */
    int    pending_leader;    /* 1 = waiting for leader second key */
    char   leader_char;       /* the leader key (default ' ')      */
    int    last_content_pane; /* index of last non-tree active pane */
    int    mouse_x, mouse_y;  /* last mouse event terminal coords (1-based) */
    int    term_rows;         /* raw terminal height from ioctl    */
    int    term_cols;         /* raw terminal width  from ioctl    */

    /* Jump list (Ctrl-O / Ctrl-I) */
    JumpEntry jump_list[JUMP_MAX];
    int       jump_count;     /* number of stored entries           */
    int       jump_cur;       /* navigating index; == jump_count when live */

    /* Marks (ma = set, `a = jump exact, 'a = jump line) */
    Mark      marks[MARK_MAX];
    int       pending_mark;   /* 0=none; 'm'=set, '`'=jump-exact, '\''=jump-line */

    /* Fuzzy finder overlay */
    FuzzyState fuzzy;

    /* Git integration */
    char git_branch[64];    /* current branch name (empty if not a git repo) */
    int  pending_bracket;   /* ']' or '[' waiting for second key, 0 = none  */
    int  pending_leader_h;  /* 1 = <leader>h pressed, waiting for s/r       */
} EditorConfig;

extern EditorConfig E;

void editor_init(void);
void editor_detect_syntax(void);
void editor_update_git_signs(void);

UndoState editor_capture_state(void);
void      editor_restore_state(const UndoState *s);

void editor_buf_save(int i);
void editor_buf_restore(int i);

#endif
