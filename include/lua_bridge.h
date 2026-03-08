// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUA_BRIDGE_H
#define LUA_BRIDGE_H

#include "editor.h"  /* EditorMode */

void lua_bridge_init(void);
/* Returns 1 if a Lua binding consumed the key, 0 otherwise. */
int  lua_bridge_call_key(EditorMode mode, int key);
/* Run a Lua-registered CLI subcommand.  Returns exit code. */
int  lua_bridge_cli(const char *name, int argc, char **argv);

#endif
