# SWM

A tabbed manual-tiling window manager for X11. No dependencies beyond Xlib. Inspired by Notion and TinyWM.

Hello, everybody. I just wanted to show off the project that I made with Claude and wanted to write a personal note using my voice and the tool called Handy to dictate my text. The following is a technical summary that Claude wrote. I've been playing with LLMs for a minute, and I just wanted to say thank you to everyone for checking the project out and having all the work that everyone's done in the world to make this possible. I do a [Sanskrit](https://github.com/hardkorebob/spte) inspired prompt engineering style of building apps. I don't use agents/MCP. Patience and caution one step a time. I usually start with a prototype in HTML or Python and then I'll convert it to whatever language seems necessary for the type of tool that I'm making. And of course after a lot of trial and error, I get it to work and I learn along the way.  I really appreciate everyone. Thank you so much. And also it's a fun little window manager. It's a different take on my way of using my computer. So please enjoy. Thank you again. PS: I'm sure there is a bug or ten kicking around but hey, its all just for fun! Plus I fixed all the ASCII tables cuz AI sucks at art. Womp womp...didnt know markdown ascii tables look like real tables in readme... all that time fixing the | to line up ;-)

## Features

- Binary tree tiling with manual horizontal/vertical splits
- Tabbed windows within each tile
- 9 workspaces
- Status bar with workspace indicators, CPU, RAM, IP, Volume (mouse-wheel to adjust)
- Time bar with hex-time (block of 3m 45s * 16 = One hour; Blocks 0-F)
- EWMH fullscreen support
- IPC via Unix domain socket (`swmctl`)
- Runtime config reload (SIGHUP or `swmctl reload`)
- Click-to-focus, click on tab bar to switch tabs
- Raise only using Mod4+Arrow (Amiga-style)

## Build

Requires `libX11-dev` library & `-misc-fixed-medium-r-*-*-13-*-*-*-*-*-iso8859-1` font

```
cc -O2 -o swm swm.c -lX11
```

## Install

Copy to your `$PATH`: `swm` and `swmctl`(requires `socat`). X session file goes in `/usr/share/xsessions`

## Configuration

Config file location: `$XDG_CONFIG_HOME/swm/config` or `~/.config/swm/config`.

Format is `key = value`, one per line. Lines starting with `#` are comments.

## Keybindings

### No modifier

| Key | Action            |
|----|--------------------|
| F1 | Previous tab       |
| F2 | Next tab           |
| F3 | Previous workspace |
| F4 | Next workspace     |
| F6 | Close window       |
| F7 | Launcher           |
| F8 | File manager       |
| F9 | Browser            |

### Mod4 (Super)

| Mod4+Key | Action            |
|----------|-------------------|
| Return   | Terminal          |
| h | Split horizontal         |
| v | Split vertical           |
| d | Remove split             |
| q | Quit                     |
| , | Move tab backward        |
| . | Move tab forward         |
| Arrows | Raise adjacent tile |
| 1-9    | Switch workspace    |

### Mod4+Shift

| Key | Action                          |
|-----|---------------------------------|
| h | Split horizontal + move window    |
| v | Split vertical + move window      |
| Arrows | Move window to adjacent tile |
| 1-9 | Send window to workspace        |

## IPC

ZWM listens on a Unix socket at `$XDG_RUNTIME_DIR/swm.sock` (fallback: `/tmp/swm.sock`). This part is quirky, but I like it because it reminds me of Plan9. And you can use it for a startup script so that you can create your own layouts automatically. 

Use `swmctl` to send commands:(no arguments to see help)

```
swmctl split h
swmctl workspace 3
swmctl exec rofi -show run
swmctl query win-title
swmctl set border_gap 6
swmctl reload
swmctl fullscreen
```

Pipe mode:

```
echo "next-tab" | swmctl -
```

### Dimensions

| Key                 | Default | Description                      |
|-----------------------|-----|------------------------------------|
| `tab_bar_height`      | 22  |  Height of tab bars in pixels      |
| `border_width`        | 1   |  Border width around tiles         |
| `border_gap`          | 2   |  Gap between tiles                 |
| `statusbar_height`    | 24  |  Status bar height (0 to disable)  |
| `statusbar_pos`       | 0   |  0 = bottom, 1 = top               |
| `timebar_height`      | 14  |  Hex-time bar height               |
| `timebar_pos`         | 1   |  0 = bottom, 1 = top               |
| `bar_update_interval` | 1.0 |  Bar refresh interval in seconds   |

### Commands

| Key            | Default   |
|----------------|-----------|
| `terminal`     | xterm     |
| `file_manager` | thunar    |
| `browser`      | firefox   |
| `launcher`     | dmenu_run |

## License

MIT
