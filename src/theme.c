// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"
#include "syntax.h"

#include <stdlib.h>
#include <string.h>

/* ── Registry ────────────────────────────────────────────────────────── */

static Theme  registry[MAX_THEMES];
static int    num_themes;
static Theme *current_theme;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void theme_free_strings(Theme *t) {
    for (int i = 0; i < HL_TYPE_COUNT; i++) { free(t->hl_colors[i]); t->hl_colors[i] = NULL; }
    free(t->bg);                t->bg = NULL;
    free(t->fg);                t->fg = NULL;
    free(t->statusbar_active);  t->statusbar_active = NULL;
    free(t->statusbar_inactive);t->statusbar_inactive = NULL;
    free(t->cursorline_bg);     t->cursorline_bg = NULL;
}

static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }

/* ── Public API ──────────────────────────────────────────────────────── */

void theme_init(void) {
    Theme def = {0};
    strncpy(def.name, "default", sizeof(def.name) - 1);

    def.hl_colors[HL_COMMENT]       = strdup("\x1b[2;36m");   /* dim cyan     */
    def.hl_colors[HL_KEYWORD]       = strdup("\x1b[1;33m");   /* bold yellow  */
    def.hl_colors[HL_TYPE]          = strdup("\x1b[36m");      /* cyan         */
    def.hl_colors[HL_STRING]        = strdup("\x1b[32m");      /* green        */
    def.hl_colors[HL_NUMBER]        = strdup("\x1b[35m");      /* magenta      */
    def.hl_colors[HL_ESCAPE]        = strdup("\x1b[1;32m");    /* bold green   */
    def.hl_colors[HL_PREPROC]       = strdup("\x1b[1;35m");    /* bold magenta */
    def.hl_colors[HL_BRACKET1]      = strdup("\x1b[33m");      /* yellow       */
    def.hl_colors[HL_BRACKET2]      = strdup("\x1b[35m");      /* magenta      */
    def.hl_colors[HL_BRACKET3]      = strdup("\x1b[36m");      /* cyan         */
    def.hl_colors[HL_BRACKET4]      = strdup("\x1b[34m");      /* blue         */
    def.hl_colors[HL_SEARCH]        = strdup("\x1b[7m");       /* reverse      */
    def.hl_colors[HL_BRACKET_MATCH] = strdup("\x1b[104;97m");  /* bright blue bg */
    def.hl_colors[HL_VISUAL]        = strdup("\x1b[44m");      /* blue bg      */

    def.statusbar_active   = strdup("\x1b[7m");
    def.statusbar_inactive = strdup("\x1b[2;7m");
    def.cursorline_bg      = strdup("\x1b[48;5;236m");
    def.bg = NULL;
    def.fg = NULL;

    theme_register(&def);
    theme_set("default");
}

void theme_register(const Theme *t) {
    /* Overwrite if a theme with same name already exists. */
    for (int i = 0; i < num_themes; i++) {
        if (strcmp(registry[i].name, t->name) == 0) {
            theme_free_strings(&registry[i]);
            registry[i] = (Theme){0};
            strncpy(registry[i].name, t->name, sizeof(registry[i].name) - 1);
            for (int j = 0; j < HL_TYPE_COUNT; j++)
                registry[i].hl_colors[j] = dup_or_null(t->hl_colors[j]);
            registry[i].bg                = dup_or_null(t->bg);
            registry[i].fg                = dup_or_null(t->fg);
            registry[i].statusbar_active  = dup_or_null(t->statusbar_active);
            registry[i].statusbar_inactive= dup_or_null(t->statusbar_inactive);
            registry[i].cursorline_bg     = dup_or_null(t->cursorline_bg);
            if (current_theme == &registry[i])
                current_theme = &registry[i]; /* pointer still valid */
            return;
        }
    }
    if (num_themes >= MAX_THEMES) return;
    Theme *slot = &registry[num_themes++];
    *slot = (Theme){0};
    strncpy(slot->name, t->name, sizeof(slot->name) - 1);
    for (int j = 0; j < HL_TYPE_COUNT; j++)
        slot->hl_colors[j] = dup_or_null(t->hl_colors[j]);
    slot->bg                = dup_or_null(t->bg);
    slot->fg                = dup_or_null(t->fg);
    slot->statusbar_active  = dup_or_null(t->statusbar_active);
    slot->statusbar_inactive= dup_or_null(t->statusbar_inactive);
    slot->cursorline_bg     = dup_or_null(t->cursorline_bg);
}

int theme_set(const char *name) {
    for (int i = 0; i < num_themes; i++) {
        if (strcmp(registry[i].name, name) == 0) {
            current_theme = &registry[i];
            return 0;
        }
    }
    return -1;
}

const char *theme_hl_escape(unsigned char hl) {
    if (!current_theme || hl >= HL_TYPE_COUNT) return NULL;
    return current_theme->hl_colors[hl];
}

const char *theme_statusbar_escape(int is_active) {
    if (!current_theme) return is_active ? "\x1b[7m" : "\x1b[2;7m";
    return is_active ? current_theme->statusbar_active
                     : current_theme->statusbar_inactive;
}

const char *theme_cursorline_bg(void) {
    if (!current_theme) return "\x1b[48;5;236m";
    return current_theme->cursorline_bg;
}

const char *theme_bg(void) {
    return current_theme ? current_theme->bg : NULL;
}

const char *theme_fg(void) {
    return current_theme ? current_theme->fg : NULL;
}

void theme_reset(void) {
    for (int i = 0; i < num_themes; i++)
        theme_free_strings(&registry[i]);
    num_themes    = 0;
    current_theme = NULL;
    memset(registry, 0, sizeof(registry));
}
