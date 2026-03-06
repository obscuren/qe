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
    } else if (strcmp(name, "autoindent") == 0) {
        E.opts.autoindent = lua_toboolean(LS, 2);
    } else if (strcmp(name, "tabwidth") == 0) {
        int w = (int)luaL_checkinteger(LS, 2);
        if (w > 0) E.opts.tabwidth = w;
    } else if (strcmp(name, "leader") == 0) {
        const char *v = luaL_checkstring(LS, 2);
        if (v[0] && !v[1]) E.leader_char = v[0];
        else luaL_error(LS, "leader must be a single character");
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

static const luaL_Reg qe_lib[] = {
    {"set_option",  l_set_option},
    {"print",       l_print},
    {"bind_key",    l_bind_key},
    {"command",     l_command},
    {"add_syntax",  l_add_syntax},
    {NULL,          NULL}
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
