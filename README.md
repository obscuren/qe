# Quick Ed

A fast, lightweight terminal text editor with modal editing, syntax highlighting, and Lua-based configuration.

**Website:** [jeff.lookingforteam.com/qe](https://jeff.lookingforteam.com/qe)

Please note that this is a research project. Quick Ed has been entirely built based on prompting using Claude.

---

## Installation

**Requirements:** CMake 3.16+, a C11 compiler, Lua 5.4 development headers.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build          # installs to /usr/local/bin/qe
```

**Fedora/RHEL:**
```bash
sudo dnf install lua-devel
```

**Debian/Ubuntu:**
```bash
sudo apt install liblua5.4-dev
```

---

## Usage

```bash
qe                  # open empty buffer
qe file.c           # open a file
qe +42 file.c       # open file at line 42
qe -R file.c        # open file read-only
qe src/             # open directory tree
```

### CLI subcommands

```bash
qe cat file.c       # syntax-highlighted cat
qe diff file.c      # colored git diff vs HEAD
qe blame file.c     # colored git blame
qe log              # colored git log (default 50 entries)
qe log --limit 20   # limit number of entries
qe grep pattern     # recursive grep with colors
qe grep pattern src/ # grep in specific path
```

CLI subcommands output directly to the terminal and exit — they don't start the editor. Custom subcommands can be added via Lua:

```lua
qe.add_command("hello", function(args)
    print("Hello from qe! Args: " .. table.concat(args, ", "))
    return 0  -- exit code
end)
```

Then run: `qe hello world` → `Hello from qe! Args: world`

---

## Modes

Quick Ed is modal, like Vim. The current mode is shown in the command bar at the bottom.

| Mode         | How to enter              | What it does                        |
|--------------|---------------------------|-------------------------------------|
| Normal       | `Esc` from any mode       | Navigate and issue commands         |
| Insert       | `i`, `a`, `o`, `O`        | Type and edit text                  |
| Command      | `:`                       | Run editor commands                 |
| Search       | `/`                       | Search through the buffer           |
| Visual       | `v`                       | Characterwise selection             |
| Visual Line  | `V`                       | Linewise selection                  |

---

## Count prefixes (Normal mode)

Most Normal-mode commands accept a count prefix that repeats or qualifies the action:

```
3j      move down 3 lines          5w      move forward 5 words
3dd     delete 3 lines             d3w     delete 3 words
5G      jump to line 5             3x      delete 3 characters
3p      paste 3 times              10j     move down 10 lines
```

The accumulating count and any pending operator are shown live in the status bar.

---

## Navigation (Normal mode)

| Key              | Action                          |
|------------------|---------------------------------|
| `h` / `←`        | Move left                       |
| `l` / `→`        | Move right                      |
| `k` / `↑`        | Move up                         |
| `j` / `↓`        | Move down                       |
| `w`              | Move to start of next word      |
| `e`              | Move to end of next word        |
| `E`              | Move to end of line             |
| `b`              | Move to start of previous word  |
| `B`              | Move to start of line           |
| `0`              | Move to start of line           |
| `$`              | Move to end of line             |
| `_` / `^`        | First non-blank character       |
| `{`              | Previous blank line (paragraph) |
| `}`              | Next blank line (paragraph)     |
| `gg`             | Jump to first line              |
| `G`              | Jump to last line               |
| `{n}G`           | Jump to line n                  |
| `gd`             | Go to local definition          |
| `%`              | Jump to matching bracket        |
| `f{char}`        | Jump to next `char` on line     |
| `F{char}`        | Jump to prev `char` on line     |
| `t{char}`        | Jump to just before next `char` |
| `T{char}`        | Jump to just after prev `char`  |
| `;`              | Repeat last f/F/t/T forward     |
| `,`              | Repeat last f/F/t/T backward    |
| `Page Up/Down`   | Scroll one screen               |
| `Home` / `End`   | Start / end of line             |

---

## Editing (Insert mode)

Enter insert mode with `i` (before cursor), `a` (after cursor), `o` (new line below), or `O` (new line above). Press `Esc` to return to Normal mode.

| Key        | Action                                  |
|------------|-----------------------------------------|
| `Tab`      | Insert spaces to the next tab stop      |
| `Backspace`| Delete character to the left            |
| `Enter`    | Split line / new line                   |
| `Esc`      | Return to Normal mode                   |

**Auto-pairs** (enabled by default): typing `(`, `{`, `[`, `"`, or `'` automatically inserts the closing counterpart and places the cursor between them. Typing the closing character when it already matches the character under the cursor skips over it instead of inserting a duplicate. Backspace between a matched pair deletes both characters. Disable with `qe.set_option("autopairs", false)`.

---

## Normal mode commands

| Key        | Action                                         |
|------------|------------------------------------------------|
| `x`        | Delete character under cursor                  |
| `.`        | Repeat last change (count overrides stored count) |
| `u`        | Undo (tree-based, preserves all branches)      |
| `Ctrl-R`   | Redo (follows most recent branch)              |
| `g-`       | Earlier state (chronological across branches)  |
| `g+`       | Later state (chronological across branches)    |
| `r{char}`  | Replace character(s) under cursor with `{char}` |
| `n`        | Repeat search forward                          |
| `N`        | Repeat search backward                         |
| `p`        | Paste after cursor / below current line        |
| `P`        | Paste before cursor / above current line       |

### `.` repeat

`.` re-executes the last buffer-modifying action, including any text typed in the subsequent insert session:

| Original action     | `.` replays                                 |
|---------------------|---------------------------------------------|
| `3x`                | Delete 3 chars at cursor again              |
| `dw` / `3dd`        | Same delete + count                         |
| `cw` + typed text   | Delete word, re-insert the same text        |
| `p` / `P`           | Paste again (same direction + count)        |
| `i` / `a` / `A` + text | Re-insert same text at current position |
| `o` / `O` + text    | Open new line, re-insert same text          |

A count before `.` (e.g. `5.`) overrides the stored count.

### Undo tree

Undo history forms a tree rather than a linear stack. After undoing and making a new edit, the old redo branch is preserved — nothing is ever lost. `u` and `Ctrl-R` navigate the tree branch by branch, while `g-`/`g+` traverse all states chronologically (crossing branches). The tree is capped at 200 nodes; oldest unused leaves are pruned automatically.

#### Local revisions browser

Use `:revisions` (or `:rev`) to open a visual undo tree browser in a sidebar on the left. The tree is rendered with branch visualization:

```
● initial  ◀  (3 lines, 1:0)
├─ insert  (4 lines, 2:5)
│  └─ delete  (3 lines, 1:0)
└─ insert  (4 lines, 3:12)
```

| Key     | Action                                      |
|---------|---------------------------------------------|
| `j`/`k` | Navigate revisions                         |
| `G`/`gg`| Jump to last / first revision              |
| `Enter` | Accept the selected revision               |
| `q`/`Esc`| Close and revert to original state        |

As you browse, the content pane updates live to preview the selected revision. Changed lines are highlighted in the gutter: `+` (green) for added lines, `~` (yellow) for modified lines. Closing without pressing Enter reverts the buffer to its original state — no changes are committed until you explicitly accept.

## Operators (Normal mode)

Operators combine with a motion to act on a range of text. Type the operator key then a motion key.

| Operator   | Motion  | Action                                  |
|------------|---------|-----------------------------------------|
| `d`        | `w`     | Delete to start of next word            |
| `d`        | `e`     | Delete to end of word (inclusive)       |
| `d`        | `b`     | Delete to start of previous word        |
| `d`        | `0`     | Delete to start of line                 |
| `d`        | `$`     | Delete to end of line                   |
| `dd`       |         | Delete current line                     |
| `y`        | `w`     | Yank (copy) to start of next word       |
| `y`        | `e`     | Yank to end of word (inclusive)         |
| `y`        | `b`     | Yank to start of previous word          |
| `y`        | `0`     | Yank to start of line                   |
| `y`        | `$`     | Yank to end of line                     |
| `yy`       |         | Yank current line                       |
| `c`        | `w`     | Delete to start of next word, enter Insert |
| `c`        | `e`     | Delete to end of word, enter Insert     |
| `c`        | `b`     | Delete to start of previous word, enter Insert |
| `c`        | `0`     | Delete to start of line, enter Insert   |
| `c`        | `$`     | Delete to end of line, enter Insert     |
| `cc`       |         | Clear current line, enter Insert        |

All motions also work after operators: `df,` deletes to the next comma, `ct"` changes to the next quote, `y3f.` yanks up to the 3rd period, etc.

### Indent / outdent

| Key   | Action                                             |
|-------|----------------------------------------------------|
| `>>`  | Indent current line by one tabwidth                |
| `<<`  | Outdent current line by one tabwidth               |
| `3>>` | Indent 3 lines                                     |
| `>`   | Indent selection (Visual / Visual Line mode)       |
| `<`   | Outdent selection (Visual / Visual Line mode)      |

`.` repeats the last indent/outdent with the same count (overridable with a new count prefix).

Word motions (`w`, `e`, `b`) cross line boundaries, so `d2w` near the end of a line deletes through the newline into the next line. Yanked multi-line text can be pasted back with `p`.

Deleted or yanked text goes into the unnamed register and can be pasted with `p` / `P`.

### Named registers

Prefix any yank, delete, or paste with `"a` through `"z` to use a named register:

| Sequence   | Action                                      |
|------------|---------------------------------------------|
| `"ayy`     | Yank current line into register `a`         |
| `"ap`      | Paste from register `a`                     |
| `"bdd`     | Delete line into register `b`               |
| `"cy$`     | Yank to end of line into register `c`       |
| `"+yy`     | Yank current line to system clipboard       |
| `"+p`      | Paste from system clipboard                 |

All yank/delete operations also write to the unnamed register, so `p` always pastes the most recently yanked/deleted text regardless of which named register was used. The `"+` register interfaces with the system clipboard (via `wl-copy`/`wl-paste`, `xclip`, or `xsel`). The active register is shown in the status bar.

Use `:registers` (or `:reg`) to view all register and macro contents in a scratch buffer.

### Macros

Record and replay keystroke sequences:

| Key      | Action                                    |
|----------|-------------------------------------------|
| `qa`     | Start recording into register `a`         |
| `q`      | Stop recording                            |
| `@a`     | Replay macro in register `a`              |
| `@@`     | Replay the last used macro                |
| `5@a`    | Replay macro `a` five times               |

While recording, a red `recording @a` indicator appears in the command bar. Macros capture all keystrokes across all modes (normal, insert, command, search, visual).

`c` operations are a single undo step — pressing `u` restores the text before the change.

## Text objects

Text objects work after an operator (`d`, `y`, `c`) or inside Visual mode.

| Object          | `i` (inner)                     | `a` (around)                            |
|-----------------|---------------------------------|-----------------------------------------|
| `w`             | word under cursor               | word + surrounding whitespace           |
| `"` / `'` / `` ` `` | text between matching quotes | including the quote characters     |
| `(` / `)` / `b` | text inside parentheses        | including the parentheses               |
| `[` / `]`       | text inside square brackets     | including the brackets                  |
| `{` / `}` / `B` | text inside curly braces       | including the braces                    |
| `<` / `>`       | text inside angle brackets      | including the angle brackets            |

Examples: `diw` (delete inner word), `ci"` (change inside quotes), `va(` (visually select including parens), `da{` (delete a block including braces).

Bracket objects are multi-line aware and handle nesting. Dot-repeat works for all text-object delete/change operations.

## Visual mode (`v` / `V` / `Ctrl-V`)

Press `v` for characterwise selection, `V` for linewise selection, or `Ctrl-V` for block (column) selection. The selection extends as you move the cursor using any normal-mode motion key. Press the same key again or `Esc` to cancel. You can switch between modes by pressing the other key.

| Key      | Action                                      |
|----------|---------------------------------------------|
| `d` / `x`| Delete selection (goes into register)       |
| `y`      | Yank (copy) selection into register         |
| `c`      | Delete selection and enter Insert mode      |
| `v`      | Toggle characterwise / cancel               |
| `V`      | Toggle linewise / cancel                    |
| `Ctrl-V` | Toggle block visual / cancel                |
| `I`      | Block insert at left edge (block mode only) |
| `A`      | Block append at right edge (block mode only)|
| `Esc`    | Cancel selection, return to Normal          |

All normal-mode motion keys (`h j k l`, `w e b`, `0 $`, `G`, arrows, page keys) extend the selection.

**Block visual mode** (`Ctrl-V`) selects a rectangular region. `I` inserts text at the left column of the block on all rows when you press `Esc`. `A` appends text at the right column.

## Entering Insert mode (Normal mode)

| Key  | Action                                        |
|------|-----------------------------------------------|
| `i`  | Insert before cursor                          |
| `a`  | Append after cursor                           |
| `A`  | Append at end of line                         |
| `o`  | Open new line below and enter Insert mode     |
| `O`  | Open new line above and enter Insert mode     |

---

## Command mode (`:`)

| Command          | Action                                       |
|------------------|----------------------------------------------|
| `:w`             | Save                                         |
| `:w filename`    | Save as filename                             |
| `:q`             | Close current buffer (quit if last)          |
| `:q!`            | Force close current buffer                   |
| `:qa`            | Close all buffers (fails if any unsaved)     |
| `:qa!`           | Force close all buffers and quit             |
| `:wq`            | Save current buffer and close it             |
| `:wa`            | Save all dirty buffers                       |
| `:wqa`           | Save all buffers and quit                    |
| `:wqa!`          | Save all buffers, force quit regardless      |
| `:e filename`    | Open file (fails if unsaved changes)         |
| `:e! filename`   | Open file, discarding unsaved changes        |
| `:e`             | Reload current file from disk                |
| `:e!`            | Reload current file, discarding changes      |
| `:bnew`          | Open a new empty buffer                      |
| `:bnew filename` | Open file in a new buffer                    |
| `:bn`            | Switch to next buffer                        |
| `:bp`            | Switch to previous buffer                    |
| `:b N`           | Switch to buffer N (1-indexed)               |
| `:ls`            | List open buffers in status bar              |
| `:buffers`       | Fuzzy buffer picker                          |
| `<leader>b`      | Fuzzy buffer picker (default leader: Space)  |
| `:set nu`        | Show line numbers                            |
| `:set nonu`      | Hide line numbers                            |

Unsaved changes are indicated by `[+]` in the status bar. Commands that would discard them require `!` to confirm.

`:q` closes the current buffer (or pane, if multiple panes show the same buffer). When only one buffer remains, `:q` quits the editor. The status bar shows `[n/total]` when multiple buffers are open.

### Split windows

| Command              | Action                                             |
|----------------------|----------------------------------------------------|
| `:split [file]`      | Split horizontally; optionally open file in new pane |
| `:sp [file]`         | Same as `:split`                                   |
| `:vsplit [file]`     | Split vertically; optionally open file in new pane |
| `:vs [file]`         | Same as `:vsplit`                                  |
| `:close`             | Close current pane (adjacent pane expands)         |
| `:only`              | Close all other panes, keep only current           |

#### Ctrl-W window navigation (Normal mode)

| Key sequence   | Action                                  |
|----------------|-----------------------------------------|
| `Ctrl-W h`     | Move focus left                         |
| `Ctrl-W j`     | Move focus down                         |
| `Ctrl-W k`     | Move focus up                           |
| `Ctrl-W l`     | Move focus right                        |
| `Ctrl-W Ctrl-W`| Cycle to next pane                      |
| `Ctrl-W c`     | Close current pane                      |
| `Ctrl-W q`     | Close current pane                      |
| `Ctrl-W +`     | Increase pane height                    |
| `Ctrl-W -`     | Decrease pane height                    |
| `Ctrl-W >`     | Increase pane width                     |
| `Ctrl-W <`     | Decrease pane width                     |
| `Ctrl-W =`     | Equalize pane widths                    |

Each pane has its own cursor position and scroll offset. Two panes may display the same buffer simultaneously — edits in one are immediately visible in the other. The new pane becomes the active one immediately after a split.

Vertical splits (`:vsplit`) are separated by a white divider column. Side-by-side panes share a single combined status bar spanning the full width; the active pane's info is shown in full reverse video, inactive panes in dim reverse video.

Using `:e filename` inside a split pane opens the file in a new buffer, leaving the other pane's buffer untouched.

The terminal is automatically redrawn on resize (`SIGWINCH`); the layout collapses to a single pane on resize.

### Embedded terminal

| Command              | Action                                             |
|----------------------|----------------------------------------------------|
| `:terminal` / `:term`| Open an embedded terminal in a split below          |
| `:terminal cmd`      | Run a specific command (e.g. `:terminal make`)      |

Opens your `$SHELL` (or `/bin/sh`) in a horizontal split below the current pane. When a command is given (e.g. `:term make`), it runs that command via `sh -c` instead. The terminal defaults to 8 rows; configure via Lua:

```lua
qe.set_option("term_height", 12)   -- 3-50 rows
```

The terminal starts in **TERMINAL** mode where all keystrokes are forwarded to the shell.

| Key sequence      | Action                                          |
|-------------------|-------------------------------------------------|
| `Esc` / `Ctrl-\`  | Escape to Normal mode (pane nav / commands)     |
| `i` / `a`         | Return from Normal mode to terminal input       |
| `p`               | Paste register contents into terminal           |
| `:q`              | Close terminal pane (from Normal mode)          |
| `Ctrl-W h/j/k/l` | Navigate to adjacent pane (from Normal mode)    |

The terminal supports 256-color SGR, cursor positioning, scroll regions, and line editing. The PTY is resized automatically when the pane dimensions change. When the shell exits (e.g. `exit` or `Ctrl-D`), the terminal pane closes automatically. Terminal buffers are excluded from `:ls`, `:bn`/`:bp`, and the fuzzy buffer picker.

### Tab completion for `:e`

Press `Tab` after `:e` (or `:e <prefix>`) to complete filenames from the current working directory. Matching files are shown in the status bar; the currently selected entry is highlighted. Press `Tab` again to cycle to the next match. Any other key dismisses the list.

```
:e <Tab>          list all files; complete to first match
:e src/<Tab>      list only files starting with "src/"
<Tab>             cycle to next match
<any other key>   dismiss list, continue editing
```

---

## Fuzzy File Finder

Open the fuzzy finder with `<leader>t` (default leader = `Space`) or `:Fuzzy`. It scans all files in the current working directory and subdirectories and lets you filter them by typing.

```
┌──────────────────────────────────────────────┐
│ search: > ren                                │
├──────────────────────────────────────────────┤
│▶ src/ render.c                               │
│  src/ render.h                               │
│  tests/ test_search.c                        │
├──────────────────────────────────────────────┤
│ 3 / 127    <Enter> open  <C-x> sp  <C-v> vsp │
└──────────────────────────────────────────────┘
```

- The **directory prefix** is shown dim; matched characters are **bold yellow**.
- The selected result is highlighted with a `▶` marker.

| Key        | Action                                      |
|------------|---------------------------------------------|
| Type       | Filter results (subsequence, case-insensitive) |
| `↑` / `Ctrl-K` | Move selection up                       |
| `↓` / `Ctrl-J` | Move selection down                     |
| `Enter`    | Open selected file in current pane          |
| `Ctrl-X`   | Open in a new horizontal split              |
| `Ctrl-V`   | Open in a new vertical split                |
| `Backspace`| Delete last query character                 |
| `Esc`      | Close the finder                            |

Hidden directories (`.git`, `node_modules`, `build`, `target`, etc.) are excluded automatically.

The panel width is configurable in Lua:

```lua
qe.set_option("fuzzy_width_pct", 60)   -- default 40
```

---

## File Tree Sidebar

Open the file tree with `<leader>e` (default leader = `Space`) or `:Tree`. It displays the directory structure of the current working directory in a sidebar pane on the left.

| Key        | Action                                      |
|------------|---------------------------------------------|
| `j` / `k`  | Move up / down                              |
| `Enter`    | Open file / toggle directory                |
| `I`        | Toggle hidden files                         |
| `r`        | Refresh tree                                |
| `q`        | Close tree                                  |

Filenames are colored by git status: **green** for untracked/added, **yellow** for modified, **red** for deleted. Status propagates to parent directories.

You can open a directory directly:

```bash
qe ~/projects/myapp    # opens file tree for that directory
```

---

## Git Integration

Quick Ed provides deep git integration when working inside a git repository.

### Gutter signs

The gutter column shows per-line diff status against HEAD:

| Sign | Color  | Meaning          |
|------|--------|------------------|
| `+`  | Green  | Added line       |
| `~`  | Yellow | Modified line    |
| `-`  | Red    | Deleted line     |

Signs update automatically after saving, undoing, or leaving insert mode.

### Hunk navigation

| Key    | Action                        |
|--------|-------------------------------|
| `]c`   | Jump to next changed hunk     |
| `[c`   | Jump to previous changed hunk |

### Hunk operations

| Key            | Action                                      |
|----------------|---------------------------------------------|
| `<leader>hs`   | Stage the hunk under the cursor             |
| `<leader>hr`   | Revert hunk to HEAD version (undoable)      |

Also available as `:Gstage` and `:Grevert`.

### Staging files

| Command          | Action                                    |
|------------------|-------------------------------------------|
| `:Gadd`          | Stage the current file                    |
| `:Gadd path`     | Stage a specific file or directory        |
| `:Greset`        | Unstage the current file                  |
| `:Greset path`   | Unstage a specific file or directory      |

In the file tree, press `a` on any entry to stage it, or `u` to unstage. The tree colors update immediately.

### Stash

| Command              | Action                                |
|----------------------|---------------------------------------|
| `:Gstash`            | Stash working changes                 |
| `:Gstash message`    | Stash with a custom message           |
| `:Gpop`              | Pop the top stash entry               |

### Branch name

The current git branch (or short SHA for detached HEAD) is shown in the status bar.

### `:Glog`

Opens a bottom pane showing the commit history. Each entry is color-coded:
- **Hash** in yellow, **date** in dim grey, **author** in cyan, **subject** in default white

| Key     | Action                              |
|---------|-------------------------------------|
| `j`/`k` | Navigate entries                   |
| `g`/`G` | Jump to first / last entry         |
| `Enter` | Open full commit diff in a buffer  |
| `q`     | Close log pane                     |

### `:Gblame`

Opens a scroll-synced blame pane to the left showing commit hash, author, and date for each line. Hash is shown in yellow, metadata in cyan. Press `q` to close.

### `:Gdiff`

Opens a side-by-side split: left pane shows the HEAD version (read-only), right pane shows the working copy. Both panes have syntax highlighting and line numbers.

Changed lines are highlighted with background tinting:
- **Green** background for added lines
- **Yellow** background for modified lines
- **Red** background for deleted lines

Line numbers are tinted to match. Navigation with `j`/`k`/`g`/`G`, close with `q`.

### `:Gcommit`

Opens a commit message buffer for staged changes. The staged diff summary is shown as dimmed comment lines (prefixed with `#`). Write your message, then:

| Command | Action                     |
|---------|----------------------------|
| `:wq`   | Commit with the message    |
| `:q`    | Abort the commit           |

---

## Quickfix / Grep

Search across files with `:grep`:

```
:grep pattern           search all files
:grep pattern src/      search only in src/
```

Results open in a quickfix pane at the bottom. Navigate with `j`/`k`, press `Enter` to jump to the match. Press `q` to close the quickfix pane.

---

## Jump List

| Key       | Action                    |
|-----------|---------------------------|
| `Ctrl-O`  | Jump to previous position |
| `Ctrl-I`  | Jump to next position     |

Jumps are recorded when navigating between files, searching, or using `G`/`gg`.

---

## Marks

| Key       | Action                              |
|-----------|-------------------------------------|
| `m{a-z}`  | Set mark at current position        |
| `` `{a-z} `` | Jump to exact mark position      |
| `'{a-z}`  | Jump to mark line (first non-blank) |

Marks persist across buffer switches. The mark letter is shown in the gutter (yellow).

---

## Mouse Support

- **Click** in a pane to focus it and move the cursor
- **Scroll wheel** scrolls the pane under the pointer
- Click in the file tree to navigate entries

---

## Search (`/`)

Type `/` in Normal mode, then enter a pattern and press `Enter`. The cursor jumps to the first match and all occurrences are highlighted.

| Key    | Action                   |
|--------|--------------------------|
| `/`    | Enter search mode        |
| `Enter`| Execute search           |
| `Esc`  | Cancel search            |
| `n`    | Next match               |
| `N`    | Previous match           |

---

## Syntax Highlighting

Quick Ed highlights keywords, types, strings, numbers, and comments. Language definitions live in `~/.config/qe/languages/` and are loaded by `init.lua`. Supported out of the box: **C/C++**, **Lua**, **Markdown**.

When the cursor is on a bracket (`(`, `)`, `[`, `]`, `{`, `}`), the matching counterpart is highlighted with a bright blue background and white text. The match search crosses line boundaries.

## Code Folding

Fold code by indent level using `z`-prefixed commands:

| Key  | Action                                       |
|------|----------------------------------------------|
| `zc` | Close fold at cursor (hide indented block)   |
| `zo` | Open fold at cursor                          |
| `za` | Toggle fold at cursor                        |
| `zM` | Close all folds in the buffer                |
| `zR` | Open all folds in the buffer                 |

Folds are indent-based: `zc` on a line hides all subsequent lines with strictly greater indentation. Lines at the same or lesser indent level remain visible — for example, folding a function signature hides the body but leaves the closing brace visible. The fold header shows a dim `[N lines]` indicator after the line content. Cursor movement (`j`/`k`) skips over folded regions, and scrolling accounts for hidden lines. Blank lines within an indented block are included in the fold.

## Session Save / Restore

| Command                | Action                                       |
|------------------------|----------------------------------------------|
| `:mksession [file]`   | Save session to file (default `Session.qe`)  |
| `:source [file]`      | Restore session from file                    |

A session file records the working directory, all open buffers (with cursor positions), and the active buffer. Special buffers (tree, terminal, quickfix, etc.) are excluded.

---

## Lua Configuration

Quick Ed is configured with Lua. The configuration file is loaded at startup from:

1. `~/.config/qe/init.lua`
2. `~/.qerc.lua` (fallback)

### `qe.set_option(name, value)`

Set editor options.

| Option            | Type    | Default | Description                                   |
|-------------------|---------|---------|-----------------------------------------------|
| `line_numbers`    | boolean | `true`  | Show line numbers in the gutter               |
| `autoindent`      | boolean | `true`  | Copy indentation on new lines                 |
| `tabwidth`        | integer | `4`     | Number of spaces inserted by Tab              |
| `fuzzy_width_pct` | integer | `40`    | Fuzzy finder panel width as % of terminal     |
| `autopairs`       | boolean | `true`  | Auto-close `()`, `{}`, `[]`, `""`, `''`       |

```lua
qe.set_option("tabwidth", 2)
qe.set_option("line_numbers", false)
qe.set_option("autopairs", false)   -- disable auto-pairs
```

### `qe.bind_key(mode, key, fn)`

Bind a key in a given mode to a Lua function. Bound keys take priority over built-in bindings.

- `mode`: `"n"` (Normal), `"i"` (Insert), `"c"` (Command), `"s"` (Search)
- `key`: a single printable character

```lua
qe.bind_key("n", "W", function()
    qe.command("w")
end)
```

### `qe.command(cmd)`

Execute a built-in command as if typed in command mode.

```lua
qe.command("w")     -- save
qe.command("q")     -- quit
```

### `qe.print(msg)`

Display a message in the command bar.

```lua
qe.print("Hello from Lua!")
```

### `qe.add_syntax(def)`

Register a syntax definition for one or more file types. The highlighting engine is built into the editor; this call supplies the language-specific rules.

| Field            | Type            | Description                                  |
|------------------|-----------------|----------------------------------------------|
| `filetypes`      | list of strings | File extensions to match (without `.`)       |
| `keywords`       | list of strings | Control-flow keywords (highlighted yellow)   |
| `types`          | list of strings | Type names (highlighted cyan)                |
| `comment_single` | string          | Single-line comment prefix (e.g. `"//"`)     |
| `comment_multi`  | `{start, end}`  | Multi-line comment delimiters                |

```lua
qe.add_syntax({
    filetypes = {"c", "h", "cpp"},
    keywords  = {"if", "else", "for", "while", "return", "struct", "typedef"},
    types     = {"int", "char", "void", "float", "size_t", "bool"},
    comment_single = "//",
    comment_multi  = {"/*", "*/"},
})
```

### Language files

Language syntax definitions live in `~/.config/qe/languages/`. Each file calls `qe.add_syntax()` and is loaded from `init.lua` via `require`:

```
~/.config/qe/
├── init.lua
└── languages/
    ├── c.lua
    ├── lua_lang.lua
    └── markdown.lua
```

### Full example `~/.config/qe/init.lua`

```lua
qe.set_option("tabwidth", 4)
qe.set_option("line_numbers", true)

-- Load language definitions
require("languages.c")
require("languages.lua_lang")
require("languages.markdown")

-- Save with W in Normal mode
qe.bind_key("n", "W", function() qe.command("w") end)
```

### Example `~/.config/qe/languages/c.lua`

```lua
qe.add_syntax({
    filetypes = {"c", "h", "cpp", "cc", "cxx", "hpp"},
    keywords  = {
        "if", "else", "for", "while", "do", "return", "break", "continue",
        "switch", "case", "default", "goto", "sizeof", "typedef", "struct",
        "union", "enum", "extern", "static", "const", "volatile", "inline",
    },
    types = {
        "int", "char", "void", "float", "double", "long", "short",
        "unsigned", "signed", "size_t", "ssize_t", "bool", "FILE",
    },
    comment_single = "//",
    comment_multi  = {"/*", "*/"},
})
```

---

## License

GPL
