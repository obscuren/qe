// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUA_BRIDGE_H
#define LUA_BRIDGE_H

#include "editor.h"  /* EditorMode */

void lua_bridge_init(void);
/* Returns 1 if a Lua binding consumed the key, 0 otherwise. */
int  lua_bridge_call_key(EditorMode mode, int key);
/* Execute a string of Lua code (for :lua ex-command). */
void lua_bridge_exec(const char *code);
/* Run a Lua-registered CLI subcommand.  Returns exit code. */
int  lua_bridge_cli(const char *name, int argc, char **argv);
/* Fire all Lua hooks registered for the named event. */
void lua_bridge_fire_event(const char *event, const char *arg1,
                           const char *arg2);
/* Convert an EditorMode enum to its Lua string name. */
const char *editor_mode_str(EditorMode m);
/* Drain output from async Lua commands (non-blocking reads). */
void lua_bridge_drain_async(void);
/* Reap finished async Lua commands and fire their callbacks. */
void lua_bridge_reap_async(void);

#endif
