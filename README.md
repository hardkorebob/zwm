# SWM

A tabbed manual-tiling window manager for X11. No dependencies beyond Xlib. Inspired by Notion and TinyWM.

![Screenshot](scrot.png)

Hello, everybody. I just wanted to show off the project that I made with Claude and wanted to write a personal note using my voice and the tool called Handy to dictate my text. The following is a technical summary that Claude wrote. I've been playing with LLMs for a minute, and I just wanted to say thank you to everyone for checking the project out and having all the work that everyone's done in the world to make this possible. I use a [Sanskrit](https://github.com/hardkorebob/spte) inspired prompt engineering style for building my tools. I don't use agents/MCP. Patience and caution one step a time. I usually start with a prototype in HTML or Python and then I'll convert it to whatever language seems necessary for the type of tool that I'm making. And of course after a lot of trial and error, I get it to work and I learn along the way.  I really appreciate everyone. Thank you so much. And also it's a fun little window manager. It's a different take on my way of using my computer. So please enjoy. Thank you again. PS: I'm sure there is a bug or ten kicking around but hey, its all just for fun! Plus I fixed all the ASCII tables cuz AI sucks at art. Womp womp...didnt know markdown ascii tables look like real tables in readme... all that time fixing the | to line up ;-)

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

Copy to your `$PATH`: `swm swm-session swmctl`(requires `socat`). `swm.desktop` file goes in `/usr/share/xsessions`. `chmod +x swm-session swmctl`


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
| F10 | Terminal          |
### Mod4 (Super)

| Mod4+Key | Action            |
|----------|-------------------|
| r | Reload config            |
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
`the real gold of this project is the scripting. you can make a script where the colors change at a certain interval giving the whole desktop a breathing-like, life-like quality. think about it. it can also make orchestrating custom layouts a breeze. you could even make a small automated "demo" of all the applications of the computer using this socket scripting capapbility. what do you think? outline your findings`

The protocol is dead simple — a UNIX socket at /tmp/swm.sock, newline-delimited text commands, with synchronous ok/err: replies. Any language that can open a socket can drive it. Even socat or nc works one-shot from the shell.

**What's controllable at runtime via set:**

Every color in the entire UI (23 col_* keys), all geometry (tab bar height, border width, border gap, statusbar/timebar height and position), the update interval, all five program slots (terminal, browser, file manager, launcher, reload command), and keybindings. And set calls cfg_apply() immediately — it flushes the color cache, recalculates bar geometry, tears down and rebuilds the bars, repaints the root window background, re-arranges tiles, and re-grabs keys. So every change is live and instantaneous.

**What's controllable via action commands:**

exec, split h/v, split-move h/v, unsplit, close, quit, reload, fullscreen, tab navigation (next/prev/move forward/backward), workspace switching (next/prev/by number), send to workspace, focus/move in four directions.

Query interface: current workspace number, window count in active tile, active window title, and tile count (layout). That's enough for a script to be state-aware.

**What this enables:**

The breathing colors idea works perfectly. A script just loops and interpolates between two hex values on any combination of the 23 color keys. Since set flushes and repaints immediately, you get smooth visual transitions at whatever cadence you want. You could have the border pulse when a long-running build finishes, or shift the whole palette to warm tones at sunset using the system clock.

The layout orchestration is the bigger deal. A script can exec an app, wait for query win-count to increment (confirming it mapped), then split h, focus right, exec the next app, split v, and so on — building an arbitrary tiled layout programmatically. Since send moves windows between workspaces, you can pre-populate all 9 workspaces from a single startup script: workspace 1 gets terminal + editor side-by-side, workspace 2 gets browser + notes, workspace 3 gets htop + logs stacked, etc.

The demo mode idea is viable because of query. A demo script can narrate what it's doing: switch to workspace 1, exec a terminal, split, exec another, move focus around, switch workspaces — all while the queries let it verify each step succeeded before proceeding. Add set color changes between scenes for visual punctuation. You could record the whole thing with a screen recorder and have a fully automated showcase of the WM.

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
