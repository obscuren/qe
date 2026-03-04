# Quick Ed

A fast, lightweight terminal text editor with modal editing, syntax highlighting, and Lua-based configuration.

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
```

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
| `G`              | Jump to last line               |
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

---

## Normal mode commands

| Key        | Action                                         |
|------------|------------------------------------------------|
| `x`        | Delete character under cursor                  |
| `u`        | Undo                                           |
| `r`        | Redo                                           |
| `n`        | Repeat search forward                          |
| `N`        | Repeat search backward                         |
| `p`        | Paste after cursor / below current line        |
| `P`        | Paste before cursor / above current line       |

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

Deleted or yanked text goes into the internal register and can be pasted with `p` / `P`.
`c` operations are a single undo step — pressing `u` restores the text before the change.

## Visual mode (`v` / `V`)

Press `v` for characterwise selection or `V` for linewise selection. The selection extends as you move the cursor using any normal-mode motion key. Press the same key again or `Esc` to cancel.

| Key      | Action                                      |
|----------|---------------------------------------------|
| `d` / `x`| Delete selection (goes into register)       |
| `y`      | Yank (copy) selection into register         |
| `c`      | Delete selection and enter Insert mode      |
| `v`      | Toggle characterwise / cancel               |
| `V`      | Toggle linewise / cancel                    |
| `Esc`    | Cancel selection, return to Normal          |

All normal-mode motion keys (`h j k l`, `w e b`, `0 $`, `G`, arrows, page keys) extend the selection.

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
| `:q`             | Quit (fails if unsaved changes)              |
| `:q!`            | Quit without saving                          |
| `:wq`            | Save and quit                                |
| `:set nu`        | Show line numbers                            |
| `:set nonu`      | Hide line numbers                            |

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

Quick Ed highlights keywords, types, strings, numbers, and comments. Language definitions are configured in Lua (see below). Supported out of the box (via `~/.config/qe/init.lua`): **C/C++**, **Lua**.

When the cursor is on a bracket (`(`, `)`, `[`, `]`, `{`, `}`), the matching counterpart is highlighted in bold reverse video. The match search crosses line boundaries.

---

## Lua Configuration

Quick Ed is configured with Lua. The configuration file is loaded at startup from:

1. `~/.config/qe/init.lua`
2. `~/.qerc.lua` (fallback)

### `qe.set_option(name, value)`

Set editor options.

| Option          | Type    | Default | Description                         |
|-----------------|---------|---------|-------------------------------------|
| `line_numbers`  | boolean | `true`  | Show line numbers in the gutter     |
| `autoindent`    | boolean | `true`  | Copy indentation on new lines       |
| `tabwidth`      | integer | `4`     | Number of spaces inserted by Tab    |

```lua
qe.set_option("tabwidth", 2)
qe.set_option("line_numbers", false)
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

### Full example `~/.config/qe/init.lua`

```lua
qe.set_option("tabwidth", 4)
qe.set_option("line_numbers", true)

-- Save with W in Normal mode
qe.bind_key("n", "W", function() qe.command("w") end)

qe.add_syntax({
    filetypes = {"c", "h", "cpp", "cc"},
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
