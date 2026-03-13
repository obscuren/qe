// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef THEME_H
#define THEME_H

#define HL_TYPE_COUNT 16  /* enough to cover all HlType values */
#define MAX_THEMES    32

typedef struct {
    char  name[64];
    char *hl_colors[HL_TYPE_COUNT]; /* ANSI escape indexed by HlType; NULL = default */
    char *bg;                       /* editor background (NULL = terminal default)   */
    char *fg;                       /* text foreground (NULL = terminal default)      */
    char *statusbar_active;         /* active status bar                             */
    char *statusbar_inactive;       /* inactive status bar                           */
    char *cursorline_bg;            /* cursorline background                         */
} Theme;

void         theme_init(void);
void         theme_register(const Theme *t);
int          theme_set(const char *name);
const char  *theme_hl_escape(unsigned char hl);
const char  *theme_statusbar_escape(int is_active);
const char  *theme_cursorline_bg(void);
const char  *theme_bg(void);
const char  *theme_fg(void);

#endif
