// SPDX-License-Identifier: GPL-3.0-or-later
#include "lua_bridge.h"
#include "editor.h"
#include "git.h"
#include "input.h"
#include "syntax.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── State ───────────────────────────────────────────────────────────── */

static lua_State *L;

/* ── Event hooks ─────────────────────────────────────────────────────── */

#define MAX_HOOKS_PER_EVENT 32

typedef enum {
    EVENT_BUF_OPEN,
    EVENT_BUF_SAVE,
    EVENT_BUF_CLOSE,
    EVENT_MODE_CHANGE,
    EVENT_COUNT
} EventType;

static const char *event_names[EVENT_COUNT] = {
    "BufOpen", "BufSave", "BufClose", "ModeChange"
};

static int hook_refs[EVENT_COUNT][MAX_HOOKS_PER_EVENT];
static int hook_counts[EVENT_COUNT];

/* ── Key bindings ────────────────────────────────────────────────────── */

#define MAX_BINDINGS 256

typedef struct {
    EditorMode mode;
    int        key;
    int        lua_ref;
} KeyBinding;

static KeyBinding bindings[MAX_BINDINGS];
static int        num_bindings;

int lua_bridge_call_key(EditorMode mode, int key) {
    for (int i = 0; i < num_bindings; i++) {
        if (bindings[i].mode == mode && bindings[i].key == key) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, bindings[i].lua_ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                snprintf(E.statusmsg, sizeof(E.statusmsg),
                         "lua: %s", lua_tostring(L, -1));
                E.statusmsg_is_error = 1;
                lua_pop(L, 1);
            }
            return 1;
        }
    }
    return 0;
}

/* ── qe.* API ────────────────────────────────────────────────────────── */

static int l_set_option(lua_State *LS) {
    const char *name = luaL_checkstring(LS, 1);
    if (strcmp(name, "line_numbers") == 0) {
        E.opts.line_numbers = lua_toboolean(LS, 2);
    } else if (strcmp(name, "relative_line_numbers") == 0) {
        E.opts.relative_line_numbers = lua_toboolean(LS, 2);
    } else if (strcmp(name, "autoindent") == 0) {
        E.opts.autoindent = lua_toboolean(LS, 2);
    } else if (strcmp(name, "tabwidth") == 0) {
        int w = (int)luaL_checkinteger(LS, 2);
        if (w > 0) E.opts.tabwidth = w;
    } else if (strcmp(name, "leader") == 0) {
        const char *v = luaL_checkstring(LS, 2);
        if (v[0] && !v[1]) E.leader_char = v[0];
        else luaL_error(LS, "leader must be a single character");
    } else if (strcmp(name, "fuzzy_width_pct") == 0) {
        int p = (int)luaL_checkinteger(LS, 2);
        if (p >= 10 && p <= 100) E.opts.fuzzy_width_pct = p;
        else luaL_error(LS, "fuzzy_width_pct must be 10-100");
    } else if (strcmp(name, "qf_height") == 0) {
        int h = (int)luaL_checkinteger(LS, 2);
        if (h >= 3 && h <= 20) E.opts.qf_height_rows = h;
        else luaL_error(LS, "qf_height must be 3-20");
    } else if (strcmp(name, "term_height") == 0) {
        int h = (int)luaL_checkinteger(LS, 2);
        if (h >= 3 && h <= 50) E.opts.term_height_rows = h;
        else luaL_error(LS, "term_height must be 3-50");
    } else if (strcmp(name, "autopairs") == 0) {
        E.opts.autopairs = lua_toboolean(LS, 2);
    } else if (strcmp(name, "cursorline") == 0) {
        E.opts.cursorline = lua_toboolean(LS, 2);
    } else if (strcmp(name, "scrolloff") == 0) {
        int so = (int)luaL_checkinteger(LS, 2);
        E.opts.scrolloff = so >= 0 ? so : 0;
    } else if (strcmp(name, "diffstyle") == 0) {
        const char *val = luaL_checkstring(LS, 2);
        if (strcmp(val, "unified") == 0)      E.opts.diffstyle = DIFFSTYLE_UNIFIED;
        else if (strcmp(val, "split") == 0)   E.opts.diffstyle = DIFFSTYLE_SPLIT;
        else luaL_error(LS, "diffstyle must be \"unified\" or \"split\"");
    } else {
        luaL_error(LS, "unknown option: %s", name);
    }
    return 0;
}

static int l_print(lua_State *LS) {
    const char *msg = luaL_checkstring(LS, 1);
    snprintf(E.statusmsg, sizeof(E.statusmsg), "%s", msg);
    E.statusmsg_is_error = 0;
    return 0;
}

static int l_bind_key(lua_State *LS) {
    const char *mode_str = luaL_checkstring(LS, 1);
    const char *key_str  = luaL_checkstring(LS, 2);
    luaL_checktype(LS, 3, LUA_TFUNCTION);

    EditorMode mode;
    if      (strcmp(mode_str, "n")      == 0 ||
             strcmp(mode_str, "normal") == 0) mode = MODE_NORMAL;
    else if (strcmp(mode_str, "i")      == 0 ||
             strcmp(mode_str, "insert") == 0) mode = MODE_INSERT;
    else if (strcmp(mode_str, "c")      == 0 ||
             strcmp(mode_str, "command")== 0) mode = MODE_COMMAND;
    else if (strcmp(mode_str, "s")      == 0 ||
             strcmp(mode_str, "search") == 0) mode = MODE_SEARCH;
    else return luaL_error(LS, "unknown mode: %s", mode_str);

    size_t klen = strlen(key_str);
    int key;
    if (klen == 1) {
        key = (unsigned char)key_str[0];
    } else if (klen == 9 && memcmp(key_str, "<leader>", 8) == 0) {
        key = LEADER_BASE + (unsigned char)key_str[8];
    } else {
        return luaL_error(LS, "key must be a single character or \"<leader>X\": %s",
                          key_str);
    }

    /* Replace existing binding for the same mode+key. */
    for (int i = 0; i < num_bindings; i++) {
        if (bindings[i].mode == mode && bindings[i].key == key) {
            luaL_unref(LS, LUA_REGISTRYINDEX, bindings[i].lua_ref);
            lua_pushvalue(LS, 3);
            bindings[i].lua_ref = luaL_ref(LS, LUA_REGISTRYINDEX);
            return 0;
        }
    }

    if (num_bindings >= MAX_BINDINGS)
        return luaL_error(LS, "too many key bindings (max %d)", MAX_BINDINGS);

    lua_pushvalue(LS, 3);
    bindings[num_bindings].mode    = mode;
    bindings[num_bindings].key     = key;
    bindings[num_bindings].lua_ref = luaL_ref(LS, LUA_REGISTRYINDEX);
    num_bindings++;
    return 0;
}

static int l_command(lua_State *LS) {
    const char *cmd = luaL_checkstring(LS, 1);
    int len = (int)strlen(cmd);
    if (len >= (int)sizeof(E.cmdbuf))
        len = (int)sizeof(E.cmdbuf) - 1;
    memcpy(E.cmdbuf, cmd, len);
    E.cmdbuf[len] = '\0';
    E.cmdlen = len;
    editor_execute_command();
    return 0;
}

/* ── qe.add_syntax ───────────────────────────────────────────────────── */

/* Read a Lua array of strings from the table at stack index `tbl_idx`
   under key `field`.  Returns a NULL-terminated array of strdup'd strings
   and sets *out_count.  Returns NULL (count=0) if field is absent/empty. */
static char **read_string_array(lua_State *LS, int tbl_idx,
                                const char *field, int *out_count) {
    *out_count = 0;
    lua_getfield(LS, tbl_idx, field);
    if (!lua_istable(LS, -1)) { lua_pop(LS, 1); return NULL; }

    int n = (int)lua_rawlen(LS, -1);
    if (n == 0) { lua_pop(LS, 1); return NULL; }

    char **arr = calloc(n + 1, sizeof(char *));
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(LS, -1, i);
        const char *s = lua_tostring(LS, -1);
        arr[i - 1] = s ? strdup(s) : strdup("");
        lua_pop(LS, 1);
    }
    arr[n] = NULL;
    *out_count = n;
    lua_pop(LS, 1);
    return arr;
}

static int l_add_syntax(lua_State *LS) {
    luaL_checktype(LS, 1, LUA_TTABLE);

    SyntaxDef *def = calloc(1, sizeof(SyntaxDef));

    def->filetypes = read_string_array(LS, 1, "filetypes", &def->num_filetypes);
    def->keywords  = read_string_array(LS, 1, "keywords",  &def->num_keywords);
    def->types     = read_string_array(LS, 1, "types",     &def->num_types);

    lua_getfield(LS, 1, "comment_single");
    if (lua_isstring(LS, -1))
        def->comment_single = strdup(lua_tostring(LS, -1));
    lua_pop(LS, 1);

    lua_getfield(LS, 1, "comment_multi");
    if (lua_istable(LS, -1)) {
        lua_rawgeti(LS, -1, 1);
        if (lua_isstring(LS, -1))
            def->comment_ml_start = strdup(lua_tostring(LS, -1));
        lua_pop(LS, 1);
        lua_rawgeti(LS, -1, 2);
        if (lua_isstring(LS, -1))
            def->comment_ml_end = strdup(lua_tostring(LS, -1));
        lua_pop(LS, 1);
    }
    lua_pop(LS, 1);

    syntax_register(def);
    return 0;
}

/* ── qe.add_command ──────────────────────────────────────────────────── */

#include "cli.h"

static int l_add_command(lua_State *LS) {
    const char *name = luaL_checkstring(LS, 1);
    luaL_checktype(LS, 2, LUA_TFUNCTION);
    lua_pushvalue(LS, 2);
    int ref = luaL_ref(LS, LUA_REGISTRYINDEX);
    cli_register_lua_command(name, ref);
    return 0;
}

/* ── Tier 1: Buffer & cursor API ─────────────────────────────────────── */

static void lua_push_undo(const char *desc) {
    if (!E.undo_tree.root)
        undo_tree_set_root(&E.undo_tree, editor_capture_state(), "initial");
    else
        undo_tree_push(&E.undo_tree, editor_capture_state(), desc);
}

static int l_get_cursor(lua_State *LS) {
    lua_pushinteger(LS, E.cy);
    lua_pushinteger(LS, E.cx);
    return 2;
}

static int l_set_cursor(lua_State *LS) {
    int row = (int)luaL_checkinteger(LS, 1);
    int col = (int)luaL_checkinteger(LS, 2);
    if (E.buf.numrows == 0) return 0;
    if (row < 0) row = 0;
    if (row >= E.buf.numrows) row = E.buf.numrows - 1;
    if (col < 0) col = 0;
    int maxcol = E.buf.rows[row].len > 0 ? E.buf.rows[row].len - 1 : 0;
    if (col > maxcol) col = maxcol;
    E.cy = row;
    E.cx = col;
    return 0;
}

static int l_get_line(lua_State *LS) {
    int row;
    if (lua_gettop(LS) >= 1)
        row = (int)luaL_checkinteger(LS, 1);
    else
        row = E.cy;
    if (row < 0 || row >= E.buf.numrows)
        return luaL_error(LS, "row %d out of range [0, %d)", row, E.buf.numrows);
    lua_pushlstring(LS, E.buf.rows[row].chars, E.buf.rows[row].len);
    return 1;
}

static int l_set_line(lua_State *LS) {
    int row = (int)luaL_checkinteger(LS, 1);
    size_t len;
    const char *text = luaL_checklstring(LS, 2, &len);
    if (row < 0 || row >= E.buf.numrows)
        return luaL_error(LS, "row %d out of range [0, %d)", row, E.buf.numrows);
    lua_push_undo("lua set_line");
    buf_delete_row(&E.buf, row);
    buf_insert_row(&E.buf, row, text, (int)len);
    E.buf.dirty++;
    return 0;
}

static int l_insert_line(lua_State *LS) {
    int row = (int)luaL_checkinteger(LS, 1);
    size_t len;
    const char *text = luaL_checklstring(LS, 2, &len);
    if (row < 0 || row > E.buf.numrows)
        return luaL_error(LS, "row %d out of range [0, %d]", row, E.buf.numrows);
    lua_push_undo("lua insert_line");
    buf_insert_row(&E.buf, row, text, (int)len);
    E.buf.dirty++;
    return 0;
}

static int l_delete_line(lua_State *LS) {
    int row = (int)luaL_checkinteger(LS, 1);
    if (row < 0 || row >= E.buf.numrows)
        return luaL_error(LS, "row %d out of range [0, %d)", row, E.buf.numrows);
    lua_push_undo("lua delete_line");
    buf_delete_row(&E.buf, row);
    E.buf.dirty++;
    if (E.cy >= E.buf.numrows && E.buf.numrows > 0)
        E.cy = E.buf.numrows - 1;
    return 0;
}

static int l_line_count(lua_State *LS) {
    lua_pushinteger(LS, E.buf.numrows);
    return 1;
}

static int l_get_filename(lua_State *LS) {
    if (E.buf.filename)
        lua_pushstring(LS, E.buf.filename);
    else
        lua_pushnil(LS);
    return 1;
}

static int l_is_dirty(lua_State *LS) {
    lua_pushboolean(LS, E.buf.dirty != 0);
    return 1;
}

static int l_get_mode(lua_State *LS) {
    const char *s;
    switch (E.mode) {
    case MODE_NORMAL:       s = "normal";       break;
    case MODE_INSERT:       s = "insert";       break;
    case MODE_COMMAND:      s = "command";       break;
    case MODE_SEARCH:       s = "search";        break;
    case MODE_VISUAL:       s = "visual";        break;
    case MODE_VISUAL_LINE:  s = "visual_line";   break;
    case MODE_VISUAL_BLOCK: s = "visual_block";  break;
    case MODE_FUZZY:        s = "fuzzy";         break;
    default:                s = "unknown";       break;
    }
    lua_pushstring(LS, s);
    return 1;
}

/* ── Tier 2: File & buffer management ────────────────────────────────── */

static int l_open(lua_State *LS) {
    const char *fname = luaL_checkstring(LS, 1);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "e %s", fname);
    int len = (int)strlen(cmd);
    if (len >= (int)sizeof(E.cmdbuf)) len = (int)sizeof(E.cmdbuf) - 1;
    memcpy(E.cmdbuf, cmd, len);
    E.cmdbuf[len] = '\0';
    E.cmdlen = len;
    editor_execute_command();
    return 0;
}

static int l_save(lua_State *LS) {
    char cmd[512];
    if (lua_gettop(LS) >= 1) {
        const char *fname = luaL_checkstring(LS, 1);
        snprintf(cmd, sizeof(cmd), "w %s", fname);
    } else {
        snprintf(cmd, sizeof(cmd), "w");
    }
    int len = (int)strlen(cmd);
    if (len >= (int)sizeof(E.cmdbuf)) len = (int)sizeof(E.cmdbuf) - 1;
    memcpy(E.cmdbuf, cmd, len);
    E.cmdbuf[len] = '\0';
    E.cmdlen = len;
    editor_execute_command();
    return 0;
}

static int l_buffers(lua_State *LS) {
    lua_newtable(LS);
    for (int i = 0; i < E.num_buftabs; i++) {
        lua_newtable(LS);

        lua_pushinteger(LS, i);
        lua_setfield(LS, -2, "index");

        const char *fn;
        int dirty;
        if (i == E.cur_buftab) {
            fn    = E.buf.filename;
            dirty = E.buf.dirty;
        } else {
            fn    = E.buftabs[i].buf.filename;
            dirty = E.buftabs[i].buf.dirty;
        }
        if (fn) lua_pushstring(LS, fn);
        else    lua_pushnil(LS);
        lua_setfield(LS, -2, "filename");

        lua_pushboolean(LS, dirty != 0);
        lua_setfield(LS, -2, "dirty");

        lua_pushboolean(LS, i == E.cur_buftab);
        lua_setfield(LS, -2, "active");

        lua_rawseti(LS, -2, i + 1);
    }
    return 1;
}

static int l_switch_buf(lua_State *LS) {
    int idx = (int)luaL_checkinteger(LS, 1);
    if (idx < 0 || idx >= E.num_buftabs)
        return luaL_error(LS, "buffer index %d out of range [0, %d)", idx, E.num_buftabs);
    if (idx == E.cur_buftab) return 0;
    switch_to_buf(idx);
    return 0;
}

static int l_get_selection(lua_State *LS) {
    if (E.mode != MODE_VISUAL && E.mode != MODE_VISUAL_LINE
        && E.mode != MODE_VISUAL_BLOCK) {
        lua_pushnil(LS);
        return 1;
    }

    int ar = E.visual_anchor_row, ac = E.visual_anchor_col;
    int cr = E.cy,                cc = E.cx;
    int r0, c0, r1, c1;
    if (ar < cr || (ar == cr && ac <= cc)) {
        r0 = ar; c0 = ac; r1 = cr; c1 = cc;
    } else {
        r0 = cr; c0 = cc; r1 = ar; c1 = ac;
    }

    luaL_Buffer lb;
    luaL_buffinit(LS, &lb);

    if (E.mode == MODE_VISUAL_LINE) {
        for (int r = r0; r <= r1; r++) {
            if (r > r0) luaL_addchar(&lb, '\n');
            luaL_addlstring(&lb, E.buf.rows[r].chars, E.buf.rows[r].len);
        }
    } else if (E.mode == MODE_VISUAL_BLOCK) {
        int lc = ac < cc ? ac : cc;
        int rc = ac > cc ? ac : cc;
        for (int r = r0; r <= r1; r++) {
            if (r > r0) luaL_addchar(&lb, '\n');
            int len = E.buf.rows[r].len;
            int s = lc < len ? lc : len;
            int e = (rc + 1) < len ? (rc + 1) : len;
            if (e > s) luaL_addlstring(&lb, E.buf.rows[r].chars + s, e - s);
        }
    } else {
        for (int r = r0; r <= r1; r++) {
            if (r > r0) luaL_addchar(&lb, '\n');
            int len = E.buf.rows[r].len;
            int s = (r == r0) ? c0 : 0;
            int e = (r == r1) ? (c1 + 1 < len ? c1 + 1 : len) : len;
            if (s > len) s = len;
            if (e > len) e = len;
            if (e > s) luaL_addlstring(&lb, E.buf.rows[r].chars + s, e - s);
        }
    }

    luaL_pushresult(&lb);
    return 1;
}

static int reg_name_to_index(lua_State *LS, const char *name) {
    if (name[0] == '+' && name[1] == '\0') return REG_CLIPBOARD;
    if (name[0] >= 'a' && name[0] <= 'z' && name[1] == '\0')
        return name[0] - 'a' + 1;
    if (name[0] == '"' && name[1] == '\0') return 0;
    return luaL_error(LS, "invalid register name: %s (use a-z, +, or \")", name);
}

static int l_get_register(lua_State *LS) {
    const char *name = luaL_checkstring(LS, 1);
    int ri = reg_name_to_index(LS, name);
    if (ri == REG_CLIPBOARD) clipboard_paste(ri);
    if (!E.regs[ri].rows || E.regs[ri].numrows == 0) {
        lua_pushnil(LS);
        return 1;
    }
    luaL_Buffer lb;
    luaL_buffinit(LS, &lb);
    for (int i = 0; i < E.regs[ri].numrows; i++) {
        if (i > 0) luaL_addchar(&lb, '\n');
        luaL_addstring(&lb, E.regs[ri].rows[i]);
    }
    luaL_pushresult(&lb);
    return 1;
}

static int l_set_register(lua_State *LS) {
    const char *name = luaL_checkstring(LS, 1);
    size_t tlen;
    const char *text = luaL_checklstring(LS, 2, &tlen);
    int ri = reg_name_to_index(LS, name);

    /* Free existing register contents */
    for (int i = 0; i < E.regs[ri].numrows; i++) free(E.regs[ri].rows[i]);
    free(E.regs[ri].rows);

    /* Split text on newlines */
    int nrows = 1;
    for (size_t i = 0; i < tlen; i++)
        if (text[i] == '\n') nrows++;
    char **rows = malloc(sizeof(char *) * nrows);
    const char *p = text;
    for (int i = 0; i < nrows; i++) {
        const char *nl = memchr(p, '\n', tlen - (p - text));
        int len = nl ? (int)(nl - p) : (int)(tlen - (p - text));
        rows[i] = malloc(len + 1);
        memcpy(rows[i], p, len);
        rows[i][len] = '\0';
        p = nl ? nl + 1 : p + len;
    }
    E.regs[ri].rows     = rows;
    E.regs[ri].numrows  = nrows;
    E.regs[ri].linewise = 0;

    /* Mirror to unnamed register */
    if (ri != 0) {
        for (int i = 0; i < E.regs[0].numrows; i++) free(E.regs[0].rows[i]);
        free(E.regs[0].rows);
        E.regs[0].numrows  = nrows;
        E.regs[0].linewise = 0;
        E.regs[0].rows     = malloc(sizeof(char *) * nrows);
        for (int i = 0; i < nrows; i++)
            E.regs[0].rows[i] = strdup(rows[i]);
    }

    /* Sync to system clipboard if writing to clipboard register */
    if (ri == REG_CLIPBOARD) clipboard_copy(ri);

    return 0;
}

/* ── Tier 3: Git queries ─────────────────────────────────────────────── */

static int l_git_branch(lua_State *LS) {
    if (E.git_branch[0] == '\0')
        lua_pushnil(LS);
    else
        lua_pushstring(LS, E.git_branch);
    return 1;
}

static int l_git_status(lua_State *LS) {
    GitStatus st = git_status_files();
    if (!st.staged && !st.unstaged) {
        lua_pushnil(LS);
        return 1;
    }

    lua_newtable(LS);

    lua_newtable(LS);
    for (int i = 0; i < st.staged_count; i++) {
        lua_pushstring(LS, st.staged[i]);
        lua_rawseti(LS, -2, i + 1);
    }
    lua_setfield(LS, -2, "staged");

    lua_newtable(LS);
    for (int i = 0; i < st.unstaged_count; i++) {
        lua_pushstring(LS, st.unstaged[i]);
        lua_rawseti(LS, -2, i + 1);
    }
    lua_setfield(LS, -2, "unstaged");

    git_status_free(&st);
    return 1;
}

static int l_git_diff_signs(lua_State *LS) {
    if (!E.buf.git_signs || E.buf.git_signs_count <= 0) {
        lua_pushnil(LS);
        return 1;
    }

    lua_createtable(LS, E.buf.git_signs_count, 0);
    for (int i = 0; i < E.buf.git_signs_count; i++) {
        char s[2] = { E.buf.git_signs[i], '\0' };
        lua_pushstring(LS, s);
        lua_rawseti(LS, -2, i + 1);
    }
    return 1;
}

static int l_git_log(lua_State *LS) {
    int limit = 50;
    if (lua_gettop(LS) >= 1)
        limit = (int)luaL_checkinteger(LS, 1);
    if (limit <= 0) limit = 50;

    int count;
    GitLogEntry *entries = git_log(limit, &count);
    if (!entries) {
        lua_pushnil(LS);
        return 1;
    }

    lua_createtable(LS, count, 0);
    for (int i = 0; i < count; i++) {
        lua_newtable(LS);

        lua_pushstring(LS, entries[i].hash);
        lua_setfield(LS, -2, "hash");

        lua_pushstring(LS, entries[i].date);
        lua_setfield(LS, -2, "date");

        lua_pushstring(LS, entries[i].author);
        lua_setfield(LS, -2, "author");

        lua_pushstring(LS, entries[i].subject);
        lua_setfield(LS, -2, "subject");

        lua_rawseti(LS, -2, i + 1);
    }

    free(entries);
    return 1;
}

static int l_git_blame(lua_State *LS) {
    if (!E.buf.filename) {
        lua_pushnil(LS);
        return 1;
    }

    int count;
    char **lines = git_blame(E.buf.filename, &count);
    if (!lines || count <= 0) {
        lua_pushnil(LS);
        return 1;
    }

    lua_createtable(LS, count, 0);
    for (int i = 0; i < count; i++) {
        lua_pushstring(LS, lines[i]);
        lua_rawseti(LS, -2, i + 1);
        free(lines[i]);
    }
    free(lines);
    return 1;
}

/* ── Tier 4: Event hooks ─────────────────────────────────────────────── */

static int l_on(lua_State *LS) {
    const char *name = luaL_checkstring(LS, 1);
    luaL_checktype(LS, 2, LUA_TFUNCTION);

    int ev = -1;
    for (int i = 0; i < EVENT_COUNT; i++) {
        if (strcmp(name, event_names[i]) == 0) { ev = i; break; }
    }
    if (ev < 0)
        return luaL_error(LS, "unknown event: %s", name);
    if (hook_counts[ev] >= MAX_HOOKS_PER_EVENT)
        return luaL_error(LS, "too many hooks for %s (max %d)",
                          name, MAX_HOOKS_PER_EVENT);

    lua_pushvalue(LS, 2);
    hook_refs[ev][hook_counts[ev]++] = luaL_ref(LS, LUA_REGISTRYINDEX);
    return 0;
}

/* ── Tier 5: Shell execution ──────────────────────────────────────────── */

static int l_exec(lua_State *LS) {
    const char *cmd = luaL_checkstring(LS, 1);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        lua_pushnil(LS);
        lua_pushinteger(LS, -1);
        return 2;
    }

    luaL_Buffer lb;
    luaL_buffinit(LS, &lb);
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp))
        luaL_addstring(&lb, buf);
    luaL_pushresult(&lb);

    int status = pclose(fp);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    lua_pushinteger(LS, code);
    return 2;
}

static int l_exec_async(lua_State *LS) {
    const char *cmd = luaL_checkstring(LS, 1);
    luaL_checktype(LS, 2, LUA_TFUNCTION);

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < MAX_ASYNC_CMDS; i++) {
        if (!E.async_cmds[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return luaL_error(LS, "too many async commands (max %d)", MAX_ASYNC_CMDS);

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return luaL_error(LS, "pipe() failed: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return luaL_error(LS, "fork() failed: %s", strerror(errno));
    }

    if (pid == 0) {
        /* Child: redirect stdout+stderr to pipe, exec command. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent: set up non-blocking read end. */
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    AsyncCmd *ac = &E.async_cmds[slot];
    ac->active      = 1;
    ac->pid         = pid;
    ac->pipe_fd     = pipefd[0];
    ac->output      = malloc(4096);
    ac->output_len  = 0;
    ac->output_cap  = 4096;
    ac->exited      = 0;
    ac->exit_status = -1;

    lua_pushvalue(LS, 2);
    ac->lua_cb_ref = luaL_ref(LS, LUA_REGISTRYINDEX);

    if (E.num_async_cmds <= slot)
        E.num_async_cmds = slot + 1;

    return 0;
}

static const luaL_Reg qe_lib[] = {
    {"set_option",   l_set_option},
    {"print",        l_print},
    {"bind_key",     l_bind_key},
    {"command",      l_command},
    {"add_syntax",   l_add_syntax},
    {"add_command",  l_add_command},
    /* Tier 1: Buffer & cursor */
    {"get_cursor",   l_get_cursor},
    {"set_cursor",   l_set_cursor},
    {"get_line",     l_get_line},
    {"set_line",     l_set_line},
    {"insert_line",  l_insert_line},
    {"delete_line",  l_delete_line},
    {"line_count",   l_line_count},
    {"get_filename", l_get_filename},
    {"is_dirty",     l_is_dirty},
    {"get_mode",     l_get_mode},
    /* Tier 2: File & buffer management */
    {"open",          l_open},
    {"save",          l_save},
    {"buffers",       l_buffers},
    {"switch_buf",    l_switch_buf},
    {"get_selection", l_get_selection},
    {"get_register",  l_get_register},
    {"set_register",  l_set_register},
    /* Tier 3: Git queries */
    {"git_branch",     l_git_branch},
    {"git_status",     l_git_status},
    {"git_diff_signs", l_git_diff_signs},
    {"git_log",        l_git_log},
    {"git_blame",      l_git_blame},
    /* Tier 4: Event hooks */
    {"on",             l_on},
    /* Tier 5: Shell execution */
    {"exec",           l_exec},
    {"exec_async",     l_exec_async},
    {NULL,           NULL}
};

/* ── Config loading ──────────────────────────────────────────────────── */

static void try_load(const char *path) {
    if (luaL_dofile(L, path) == LUA_OK)
        return;
    const char *err = lua_tostring(L, -1);
    if (err && strstr(err, "cannot open") == NULL) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "lua: %.110s", err);
        E.statusmsg_is_error = 1;
    }
    lua_pop(L, 1);
}

static void set_package_path(const char *config_dir) {
    char code[640];
    snprintf(code, sizeof(code),
             "package.path = package.path .. \";%s/?.lua\"", config_dir);
    luaL_dostring(L, code);
}

static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char config_dir[512];
    snprintf(config_dir, sizeof(config_dir), "%s/.config/qe", home);
    set_package_path(config_dir);

    char path[1024];
    snprintf(path, sizeof(path), "%s/init.lua", config_dir);
    if (luaL_dofile(L, path) == LUA_OK) return;
    const char *err = lua_tostring(L, -1);
    int missing = err && strstr(err, "cannot open") != NULL;
    if (!missing) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "lua: %.110s", err);
        E.statusmsg_is_error = 1;
    }
    lua_pop(L, 1);
    if (!missing) return;   /* real error in primary — don't try fallback */

    snprintf(path, sizeof(path), "%s/.qerc.lua", home);
    try_load(path);
}

/* ── Cleanup ─────────────────────────────────────────────────────────── */

static void close_lua(void) {
    if (L) {
        lua_close(L);
        L = NULL;
    }
}

/* ── Public init ─────────────────────────────────────────────────────── */

void lua_bridge_init(void) {
    L = luaL_newstate();
    luaL_openlibs(L);

    luaL_newlib(L, qe_lib);
    lua_setglobal(L, "qe");

    atexit(close_lua);

    load_config();
}

void lua_bridge_exec(const char *code) {
    if (!L) return;
    if (luaL_dostring(L, code) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        snprintf(E.statusmsg, sizeof(E.statusmsg), "lua: %.110s",
                 err ? err : "error");
        E.statusmsg_is_error = 1;
        lua_pop(L, 1);
    }
}

int lua_bridge_cli(const char *name, int argc, char **argv) {
    int ref = cli_dispatch_lua(name, argc, argv);
    if (ref < 0) return 1;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);

    /* Push argv as a Lua table. */
    lua_newtable(L);
    for (int i = 2; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        fprintf(stderr, "qe: lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }

    int ret = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    return ret;
}

const char *editor_mode_str(EditorMode m) {
    switch (m) {
    case MODE_NORMAL:       return "normal";
    case MODE_INSERT:       return "insert";
    case MODE_COMMAND:      return "command";
    case MODE_SEARCH:       return "search";
    case MODE_VISUAL:       return "visual";
    case MODE_VISUAL_LINE:  return "visual_line";
    case MODE_VISUAL_BLOCK: return "visual_block";
    case MODE_FUZZY:        return "fuzzy";
    default:                return "unknown";
    }
}

/* ── Async drain / reap ───────────────────────────────────────────────── */

void lua_bridge_drain_async(void) {
    for (int i = 0; i < E.num_async_cmds; i++) {
        AsyncCmd *ac = &E.async_cmds[i];
        if (!ac->active || ac->pipe_fd < 0) continue;

        char buf[4096];
        for (;;) {
            ssize_t nr = read(ac->pipe_fd, buf, sizeof(buf));
            if (nr <= 0) break;
            /* Grow buffer if needed, respecting cap. */
            int avail = ASYNC_OUTPUT_MAX - ac->output_len;
            int to_copy = (int)nr < avail ? (int)nr : avail;
            if (to_copy > 0) {
                while (ac->output_len + to_copy > ac->output_cap) {
                    ac->output_cap *= 2;
                    if (ac->output_cap > ASYNC_OUTPUT_MAX)
                        ac->output_cap = ASYNC_OUTPUT_MAX;
                    ac->output = realloc(ac->output, ac->output_cap);
                }
                memcpy(ac->output + ac->output_len, buf, to_copy);
                ac->output_len += to_copy;
            }
        }

        /* Check if child exited (non-blocking). */
        if (!ac->exited) {
            int status;
            pid_t ret = waitpid(ac->pid, &status, WNOHANG);
            if (ret == ac->pid) {
                ac->exited = 1;
                ac->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        }
    }
}

void lua_bridge_reap_async(void) {
    if (!L) return;
    for (int i = 0; i < E.num_async_cmds; i++) {
        AsyncCmd *ac = &E.async_cmds[i];
        if (!ac->active || !ac->exited) continue;

        /* Close pipe and drain any remaining data. */
        if (ac->pipe_fd >= 0) {
            close(ac->pipe_fd);
            ac->pipe_fd = -1;
        }

        /* Call the Lua callback with (output, exit_code). */
        lua_rawgeti(L, LUA_REGISTRYINDEX, ac->lua_cb_ref);
        lua_pushlstring(L, ac->output, ac->output_len);
        lua_pushinteger(L, ac->exit_status);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "lua [async]: %s", lua_tostring(L, -1));
            E.statusmsg_is_error = 1;
            lua_pop(L, 1);
        }

        /* Clean up. */
        luaL_unref(L, LUA_REGISTRYINDEX, ac->lua_cb_ref);
        free(ac->output);
        memset(ac, 0, sizeof(*ac));

        /* Shrink num_async_cmds if trailing slots are empty. */
        while (E.num_async_cmds > 0 && !E.async_cmds[E.num_async_cmds - 1].active)
            E.num_async_cmds--;

        return;  /* one per tick, like terminal reap */
    }
}

void lua_bridge_fire_event(const char *event, const char *arg1,
                           const char *arg2) {
    if (!L) return;

    int ev = -1;
    for (int i = 0; i < EVENT_COUNT; i++) {
        if (strcmp(event, event_names[i]) == 0) { ev = i; break; }
    }
    if (ev < 0 || hook_counts[ev] == 0) return;

    for (int i = 0; i < hook_counts[ev]; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, hook_refs[ev][i]);
        int nargs = 0;
        if (arg1) { lua_pushstring(L, arg1); nargs++; }
        if (arg2) { lua_pushstring(L, arg2); nargs++; }
        if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                     "lua [%s]: %s", event, lua_tostring(L, -1));
            E.statusmsg_is_error = 1;
            lua_pop(L, 1);
        }
    }
}
