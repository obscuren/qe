// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * lang.c — built-in language syntax definitions.
 *
 * These are registered before user Lua runs, so any qe.add_syntax() call in
 * init.lua will be appended after these entries.  syntax_detect() scans
 * newest-first, so user definitions always win over built-ins.
 */

#include "lang.h"
#include "syntax.h"

#include <stddef.h>

/* ── C / C++ ─────────────────────────────────────────────────────────── */

static char *c_filetypes[] = { "c", "h", "cpp", "hpp", "cc", "cxx" };
static char *c_keywords[] = {
    "if", "else", "for", "while", "do", "return", "switch", "case",
    "break", "continue", "goto", "default", "typedef", "struct", "enum",
    "union", "sizeof", "static", "extern", "inline", "volatile", "const",
    "register", "auto",
};
static char *c_types[] = {
    "int", "char", "void", "float", "double", "long", "short",
    "unsigned", "signed", "size_t", "ssize_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "bool", "NULL",
};
static SyntaxDef lang_c = {
    .filetypes      = c_filetypes,
    .num_filetypes  = 6,
    .keywords       = c_keywords,
    .num_keywords   = 24,
    .types          = c_types,
    .num_types      = 20,
    .comment_single   = "//",
    .comment_ml_start = "/*",
    .comment_ml_end   = "*/",
};

/* ── Lua ─────────────────────────────────────────────────────────────── */

static char *lua_filetypes[] = { "lua" };
static char *lua_keywords[] = {
    "if", "then", "else", "elseif", "end", "for", "while", "do",
    "repeat", "until", "return", "function", "local", "and", "or",
    "not", "in", "break",
};
static char *lua_types[] = {
    "nil", "true", "false",
};
static SyntaxDef lang_lua = {
    .filetypes      = lua_filetypes,
    .num_filetypes  = 1,
    .keywords       = lua_keywords,
    .num_keywords   = 18,
    .types          = lua_types,
    .num_types      = 3,
    .comment_single   = "--",
    .comment_ml_start = "--[[",
    .comment_ml_end   = "]]",
};

/* ── Python ──────────────────────────────────────────────────────────── */

static char *py_filetypes[] = { "py", "pyi" };
static char *py_keywords[] = {
    "if", "elif", "else", "for", "while", "with", "try", "except",
    "finally", "raise", "return", "import", "from", "as", "class", "def",
    "pass", "break", "continue", "and", "or", "not", "in", "is",
    "lambda", "yield", "async", "await", "del", "global", "nonlocal",
    "assert",
};
static char *py_types[] = {
    "True", "False", "None",
};
static SyntaxDef lang_python = {
    .filetypes      = py_filetypes,
    .num_filetypes  = 2,
    .keywords       = py_keywords,
    .num_keywords   = 32,
    .types          = py_types,
    .num_types      = 3,
    .comment_single   = "#",
    .comment_ml_start = NULL,
    .comment_ml_end   = NULL,
};

/* ── JavaScript / TypeScript ─────────────────────────────────────────── */

static char *js_filetypes[] = { "js", "ts", "jsx", "tsx", "mjs" };
static char *js_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "break",
    "continue", "return", "function", "const", "let", "var", "class",
    "import", "export", "default", "async", "await", "typeof",
    "instanceof", "new", "delete", "in", "of", "try", "catch", "finally",
    "throw", "yield",
};
static char *js_types[] = {
    "true", "false", "null", "undefined", "NaN", "Infinity",
};
static SyntaxDef lang_js = {
    .filetypes      = js_filetypes,
    .num_filetypes  = 5,
    .keywords       = js_keywords,
    .num_keywords   = 31,
    .types          = js_types,
    .num_types      = 6,
    .comment_single   = "//",
    .comment_ml_start = "/*",
    .comment_ml_end   = "*/",
};

/* ── Shell ───────────────────────────────────────────────────────────── */

static char *sh_filetypes[] = { "sh", "bash", "zsh", "fish" };
static char *sh_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "do", "done",
    "case", "esac", "function", "return", "local", "export", "source",
    "in", "break", "continue",
};
static char *sh_types[] = {
    "true", "false",
};
static SyntaxDef lang_sh = {
    .filetypes      = sh_filetypes,
    .num_filetypes  = 4,
    .keywords       = sh_keywords,
    .num_keywords   = 19,
    .types          = sh_types,
    .num_types      = 2,
    .comment_single   = "#",
    .comment_ml_start = NULL,
    .comment_ml_end   = NULL,
};

/* ── Go ──────────────────────────────────────────────────────────────── */

static char *go_filetypes[] = { "go" };
static char *go_keywords[] = {
    "if", "else", "for", "switch", "case", "break", "continue", "return",
    "func", "type", "struct", "interface", "import", "package", "var",
    "const", "defer", "go", "select", "chan", "map", "range", "fallthrough",
    "goto", "default",
};
static char *go_types[] = {
    "true", "false", "nil", "int", "int8", "int16", "int32", "int64",
    "uint", "uint8", "uint16", "uint32", "uint64", "uintptr", "float32",
    "float64", "complex64", "complex128", "bool", "byte", "rune", "string",
    "error",
};
static SyntaxDef lang_go = {
    .filetypes      = go_filetypes,
    .num_filetypes  = 1,
    .keywords       = go_keywords,
    .num_keywords   = 25,
    .types          = go_types,
    .num_types      = 23,
    .comment_single   = "//",
    .comment_ml_start = "/*",
    .comment_ml_end   = "*/",
};

/* ── Rust ────────────────────────────────────────────────────────────── */

static char *rs_filetypes[] = { "rs" };
static char *rs_keywords[] = {
    "if", "else", "for", "while", "loop", "match", "break", "continue",
    "return", "fn", "let", "mut", "struct", "enum", "impl", "use", "mod",
    "pub", "crate", "self", "super", "where", "trait", "type", "const",
    "static", "unsafe", "async", "await", "move", "ref", "in",
};
static char *rs_types[] = {
    "true", "false", "Some", "None", "Ok", "Err",
    "i8", "i16", "i32", "i64", "i128", "isize",
    "u8", "u16", "u32", "u64", "u128", "usize",
    "f32", "f64", "bool", "char", "str", "String",
};
static SyntaxDef lang_rust = {
    .filetypes      = rs_filetypes,
    .num_filetypes  = 1,
    .keywords       = rs_keywords,
    .num_keywords   = 32,
    .types          = rs_types,
    .num_types      = 24,
    .comment_single   = "//",
    .comment_ml_start = "/*",
    .comment_ml_end   = "*/",
};

/* ── Markdown ────────────────────────────────────────────────────────── */

static char *md_filetypes[] = { "md", "markdown" };
static SyntaxDef lang_md = {
    .filetypes      = md_filetypes,
    .num_filetypes  = 2,
    .keywords       = NULL,
    .num_keywords   = 0,
    .types          = NULL,
    .num_types      = 0,
    .comment_single   = NULL,
    .comment_ml_start = NULL,
    .comment_ml_end   = NULL,
};

/* ── Public entry point ──────────────────────────────────────────────── */

void lang_register_defaults(void) {
    syntax_register(&lang_c);
    syntax_register(&lang_lua);
    syntax_register(&lang_python);
    syntax_register(&lang_js);
    syntax_register(&lang_sh);
    syntax_register(&lang_go);
    syntax_register(&lang_rust);
    syntax_register(&lang_md);
}
