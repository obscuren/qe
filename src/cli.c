// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.h"
#include "git.h"
#include "syntax.h"
#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Syntax-highlighted cat ──────────────────────────────────────────── */

static const char *hl_ansi(unsigned char hl) {
    switch ((HlType)hl) {
        case HL_COMMENT: return "\x1b[2;36m";
        case HL_KEYWORD: return "\x1b[1;33m";
        case HL_TYPE:    return "\x1b[36m";
        case HL_STRING:  return "\x1b[32m";
        case HL_NUMBER:  return "\x1b[35m";
        default:         return NULL;
    }
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: qe cat <file>\n"); return 1; }
    const char *file = argv[2];

    Buffer buf;
    buf_init(&buf);
    buf_open(&buf, file);
    if (buf.numrows == 0) { fprintf(stderr, "qe: cannot read '%s'\n", file); return 1; }

    const SyntaxDef *syn = syntax_detect(file);
    int open_comment = 0;

    /* Highlight and print each row. */
    for (int i = 0; i < buf.numrows; i++) {
        Row *row = &buf.rows[i];
        if (syn) {
            open_comment = syntax_highlight_row(syn, row, open_comment);
            unsigned char prev = HL_NORMAL;
            for (int j = 0; j < row->len; j++) {
                unsigned char cur = row->hl[j];
                if (cur != prev) {
                    if (prev != HL_NORMAL) fputs("\x1b[m", stdout);
                    const char *esc = hl_ansi(cur);
                    if (esc) fputs(esc, stdout);
                    prev = cur;
                }
                fputc(row->chars[j], stdout);
            }
            if (prev != HL_NORMAL) fputs("\x1b[m", stdout);
        } else {
            fwrite(row->chars, 1, row->len, stdout);
        }
        fputc('\n', stdout);
    }
    buf_free(&buf);
    return 0;
}

/* ── Blame ───────────────────────────────────────────────────────────── */

static int cmd_blame(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: qe blame <file>\n"); return 1; }
    const char *file = argv[2];

    int count = 0;
    char **lines = git_blame(file, &count);
    if (!lines) { fprintf(stderr, "qe: git blame failed for '%s'\n", file); return 1; }

    for (int i = 0; i < count; i++) {
        /* Format: "hash Author Date" — color hash yellow, rest cyan. */
        const char *s = lines[i];
        const char *sp = strchr(s, ' ');
        if (sp) {
            printf("\x1b[33m%.*s\x1b[36m%s\x1b[m\n", (int)(sp - s), s, sp);
        } else {
            printf("%s\n", s);
        }
        free(lines[i]);
    }
    free(lines);
    return 0;
}

/* ── Log ─────────────────────────────────────────────────────────────── */

static int cmd_log(int argc, char **argv) {
    int limit = 50;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = atoi(argv[++i]);
    }

    int count = 0;
    GitLogEntry *entries = git_log(limit, &count);
    if (!entries) { fprintf(stderr, "qe: git log failed\n"); return 1; }

    for (int i = 0; i < count; i++) {
        printf("\x1b[33m%s\x1b[m \x1b[2m%s\x1b[m \x1b[36m%s\x1b[m %s\n",
               entries[i].hash, entries[i].date, entries[i].author, entries[i].subject);
    }
    free(entries);
    return 0;
}

/* ── Grep ────────────────────────────────────────────────────────────── */

static int cmd_grep(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: qe grep <pattern> [path]\n"); return 1; }
    const char *pattern = argv[2];
    const char *path    = argc >= 4 ? argv[3] : ".";

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "grep -rn --color=always '%s' '%s' 2>/dev/null", pattern, path);
    int ret = system(cmd);
    return WEXITSTATUS(ret);
}

/* ── Lua subcommand registry ─────────────────────────────────────────── */

#define MAX_LUA_CMDS 64

typedef struct {
    char name[64];
    int  lua_ref;
} LuaCmd;

static LuaCmd lua_cmds[MAX_LUA_CMDS];
static int    num_lua_cmds;

void cli_register_lua_command(const char *name, int lua_ref) {
    if (num_lua_cmds >= MAX_LUA_CMDS) return;
    strncpy(lua_cmds[num_lua_cmds].name, name, sizeof(lua_cmds[0].name) - 1);
    lua_cmds[num_lua_cmds].lua_ref = lua_ref;
    num_lua_cmds++;
}

/* Called from main dispatch — returns the lua_ref or -1. */
static int find_lua_cmd(const char *name) {
    for (int i = 0; i < num_lua_cmds; i++)
        if (strcmp(lua_cmds[i].name, name) == 0)
            return lua_cmds[i].lua_ref;
    return -1;
}

/* ── Main dispatch ───────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    int (*handler)(int argc, char **argv);
} BuiltinCmd;

static const BuiltinCmd builtins[] = {
    {"cat",   cmd_cat},
    {"blame", cmd_blame},
    {"log",   cmd_log},
    {"grep",  cmd_grep},
    {NULL, NULL}
};

int cli_dispatch(int argc, char **argv, int *out_line, int *out_readonly,
                 const char **out_session) {
    *out_line = 0;
    *out_readonly = 0;
    *out_session = NULL;

    /* Parse flags that don't prevent editor startup. */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            *out_line = atoi(argv[i] + 1);
        } else if (strcmp(argv[i], "-R") == 0) {
            *out_readonly = 1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            *out_session = argv[++i];
        }
    }

    /* --version / -v */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("Quick Editor %s\n", QE_VERSION);
            return 0;
        }
    }

    /* --help / -h */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Quick Ed - Qe %s (%s %s)\n"
                   "\n"
                   "Usage: qe [arguments] [file ..]         edit specific file(s)\n"
                   "   or: qe [command]                     execute subcommand\n"
                   "\n"
                   "Arguments:\n"
                   "    -h, --help                          This help\n"
                   "    -v, --version                       Display Qe version number\n"
                   "    +N                                  Open file at line N\n"
                   "    -R                                  Read-only mode\n"
                   "    -s <file>                           Restore session from file\n"
                   "\n"
                   "Commands:\n"
                   "    cat <file>                          Syntax-highlighted file output\n"
                   "    diff <file>                         Open file with diff view\n"
                   "    blame <file>                        Show git blame\n"
                   "    log [-n N]                          Show git log\n"
                   "    grep <pat> [path]                   Search files\n",
                   QE_VERSION, __DATE__, __TIME__);
            return 0;
        }
    }

    /* Need at least a subcommand argument. */
    if (argc < 2) return -1;
    const char *cmd = argv[1];

    /* Skip flags — not subcommands. */
    if (cmd[0] == '+' || cmd[0] == '-') return -1;

    /* Check built-in commands. */
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(cmd, builtins[i].name) == 0)
            return builtins[i].handler(argc, argv);
    }

    /* Check Lua-registered commands (lua_ref stored, but we can't call Lua here
       without the Lua state — return the ref for the caller to invoke). */
    if (find_lua_cmd(cmd) >= 0) return -2;  /* sentinel: Lua command found */

    return -1;  /* not a subcommand — continue to editor */
}

int cli_dispatch_lua(const char *name, int argc, char **argv) {
    int ref = find_lua_cmd(name);
    if (ref < 0) return -1;
    (void)argc; (void)argv;  /* Lua bridge handles these */
    return ref;
}
