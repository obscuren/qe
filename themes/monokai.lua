-- Monokai color theme for qe
-- Load in ~/.config/qe/init.lua:
--   dofile(os.getenv("HOME") .. "/.config/qe/themes/monokai.lua")
--   qe.set_theme("monokai")

qe.add_theme({
    name = "monokai",
    colors = {
        normal   = "\x1b[38;2;248;248;242m",
        comment  = "\x1b[3;38;2;117;113;94m",      -- italic dim
        keyword  = "\x1b[38;2;249;38;114m",         -- pink
        type     = "\x1b[38;2;102;217;239m",         -- blue
        string   = "\x1b[38;2;230;219;116m",         -- yellow
        number   = "\x1b[38;2;174;129;255m",         -- purple
        escape   = "\x1b[1;38;2;174;129;255m",       -- bold purple
        preproc  = "\x1b[38;2;249;38;114m",          -- pink (like keyword)
        bracket1 = "\x1b[38;2;249;38;114m",          -- pink
        bracket2 = "\x1b[38;2;102;217;239m",          -- blue
        bracket3 = "\x1b[38;2;230;219;116m",          -- yellow
        bracket4 = "\x1b[38;2;174;129;255m",          -- purple
    },
    bg                 = "\x1b[48;2;39;40;34m",
    fg                 = "\x1b[38;2;248;248;242m",
    statusbar_active   = "\x1b[48;2;73;72;62;38;2;248;248;242m",
    statusbar_inactive = "\x1b[48;2;56;56;48;38;2;117;113;94m",
    cursorline_bg      = "\x1b[48;2;52;52;44m",
})
