// SPDX-License-Identifier: GPL-3.0-or-later
#include "lua_bridge.h"
#include "editor.h"
#include "input.h"
#include "syntax.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── State ───────────────────────────────────────────────────────────── */

static lua_State *L;

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
