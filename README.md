# zwm

ZWM - tabbed manual tiling window manager inspired by Notion WM by https://notionwm.net/ https://github.com/raboof/notion and TinyWM by Nick Welch http://incise.org/tinywm.html

Depends:
  python-xlib

Features:

  • Tiling:    Screen is divided into non-overlapping tiles; every managed
               window is maximized to its tile.
               
  • Tabbing:   Each tile may hold multiple windows shown as tabs with a
               clickable tab bar.
               
  • Static:    Tiles only change when the user explicitly splits or removes
               a frame — no automatic rearrangement.
               
  • Workspaces: 9 independent workspaces, each with its own tile tree.

Keybindings (Mod4 = Super):

  Mod4+Return        Launch terminal (xterm)
  
  Mod4+h             Split active tile horizontally (left / right)
  
  Mod4+v             Split active tile vertically  (top / bottom)
  
  Mod4+Shift+h       Split horiz and MOVE active window to new tile
  
  Mod4+Shift+v       Split vert  and MOVE active window to new tile
  
  Mod4+d             Remove split — merge active tile with sibling
  
  F1                 Next tab in active tile
  
  Mod4+Shift+Tab     Previous tab in active tile
  
  Mod4+Left/Right/Up/Down   Focus adjacent tile
  
  Mod4+Shift+Left/Right/Up/Down  Move window to adjacent tile
  
  Mod4+1…9           Switch workspace
  
  Mod4+Shift+1…9     Send active window to workspace
  
  Mod4+r             Restart
  
  F7                 Launch external program (default: dmenu_run)
  
  Mod4+q             Quit WM

Mouse:

  Click tab bar      Activate that tab
  

Bar reservation (polybar):

  Set BAR_POSITION to "top" or "bottom" and BAR_HEIGHT to the bar's
  pixel height. The WM reserves that strip and never tiles into it.
  Polybar (or any bar using _NET_WM_STRUT*) is auto-detected and
  excluded from tiling.

