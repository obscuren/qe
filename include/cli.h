// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CLI_H
#define CLI_H

/* Try to dispatch argv as a CLI subcommand.
   Returns:  exit code >= 0 if a command was handled (caller should exit),
            -1 if no subcommand matched (caller should start the editor).
   Also handles +N and -R flags, storing them in *out_line and *out_readonly. */
int cli_dispatch(int argc, char **argv, int *out_line, int *out_readonly);

/* Register a Lua subcommand handler (called from lua_bridge). */
void cli_register_lua_command(const char *name, int lua_ref);

/* Try dispatching through Lua-registered commands.
   Returns exit code >= 0 if handled, -1 if not. */
int cli_dispatch_lua(const char *name, int argc, char **argv);

#endif
