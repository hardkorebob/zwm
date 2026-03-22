# ZWM

A tabbed tiling window manager for X11. Single C file, no dependencies beyond Xlib.

Inspired by Notion and TinyWM.

Hello, everybody. I just wanted to show off the project that I made with Claude and wanted to write a personal note using my voice and the tool called Handy to dictate my text. The following is a technical summary that Claude wrote. I've been playing with LLMs for a minute, and I just wanted to say thank you to everyone for checking the project out and having all the work that everyone's done in the world to make this possible. I do a (Sanskrit)[https://github.com/hardkorebob/spte] inspired prompt engineering style of building apps. I don't use agents/MCP. Patience and caution one step a time. I usually start with a prototype in HTML or Python and then I'll convert it to whatever language seems necessary for the type of tool that I'm making. And of course after a lot of trial and error, I get it to work and I learn along the way.  I really appreciate everyone. Thank you so much. And also it's a fun little window manager. It's a different take on my way of using my computer. So please enjoy. Thank you again. 

## Features

- Binary tree tiling with horizontal/vertical splits
- Tabbed windows within each tile (Notion-style)
- 9 workspaces
- Status bar with workspace indicators, CPU, RAM, IP, volume
- Bottom bar with hex-time display
- EWMH fullscreen support
- IPC via Unix domain socket (`zwmctl`)
- Runtime config reload (SIGHUP or `zwmctl reload`)
- Click-to-focus, click on tab bar to switch tabs

## Build

```
cc -O2 -o zwm zwm.c -lX11
```

Requires `libX11-dev` (or equivalent) and a C compiler.

## Install

Copy `zwm` and `zwmctl` somewhere in your `$PATH`. Add `exec zwm` to your `.xinitrc` or X session file.

`zwmctl` requires `socat`.

## Configuration

Config file location: `$XDG_CONFIG_HOME/zwm/config` or `~/.config/zwm/config`.

Format is `key = value`, one per line. Lines starting with `#` are comments.

### Dimensions

| Key | Default | Description |
|-----|---------|-------------|
| `tab_bar_height` | 22 | Height of tab bars in pixels |
| `border_width` | 1 | Border width around tiles |
| `border_gap` | 2 | Gap between tiles |
| `statusbar_height` | 24 | Status bar height (0 to disable) |
| `statusbar_pos` | 0 | 0 = bottom, 1 = top |
| `timebar_height` | 14 | Hex-time bar height |
| `timebar_pos` | 1 | 0 = bottom, 1 = top |
| `bar_update_interval` | 1.0 | Bar refresh interval in seconds |

### Commands

| Key | Default |
|-----|---------|
| `terminal` | xterm |
| `file_manager` | thunar |
| `browser` | firefox |
| `launcher` | dmenu_run |

### Colors

All color values are `#RRGGBB`. Configurable groups: status bar (`col_statusbar_*`), tabs (`col_tab_*`), borders (`col_border_*`), desktop (`col_desktop_bg`), time bar (`col_timebar_*`).

Example config:

```
terminal = alacritty
browser = chromium
border_gap = 4
col_border_active = #5599CC
col_desktop_bg = #111111
```

## Keybindings

### No modifier

| Key | Action |
|-----|--------|
| F1 | Previous tab |
| F2 | Next tab |
| F3 | Previous workspace |
| F4 | Next workspace |
| F6 | Close window |
| F7 | Launcher |
| F8 | File manager |
| F9 | Browser |

### Mod4 (Super)

| Key | Action |
|-----|--------|
| Return | Terminal |
| h | Split horizontal |
| v | Split vertical |
| d | Remove split |
| q | Quit |
| , | Move tab backward |
| . | Move tab forward |
| Arrows | Focus adjacent tile |
| 1-9 | Switch workspace |

### Mod4+Shift

| Key | Action |
|-----|--------|
| h | Split horizontal + move window |
| v | Split vertical + move window |
| Arrows | Move window to adjacent tile |
| 1-9 | Send window to workspace |

## IPC

ZWM listens on a Unix socket at `$XDG_RUNTIME_DIR/zwm.sock` (fallback: `/tmp/zwm.sock`).

Use `zwmctl` to send commands:

```
zwmctl split h
zwmctl workspace 3
zwmctl exec rofi -show run
zwmctl query win-title
zwmctl set border_gap 6
zwmctl reload
zwmctl fullscreen
```

Pipe mode:

```
echo "next-tab" | zwmctl -
```

Run `zwmctl` with no arguments to see all commands.

### Query commands

| Command | Returns |
|---------|---------|
| `query ws` | Current workspace number |
| `query win-count` | Window count in active tile |
| `query win-title` | Active window title |
| `query layout` | Tile count in current workspace |

## How it works

The tiling layout is a binary tree per workspace. Each leaf node is a tile that holds zero or more windows as tabs. Splitting a tile replaces it with a split node and two new tiles. Removing a split merges the sibling back into the parent.

Windows are reparented into their tile's geometry. Only the active tab in each tile is mapped; the rest are unmapped. Tab bars and border frames are separate override-redirect windows managed by the WM.

## License

See source.
