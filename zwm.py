#!/usr/bin/env python3
"""
ZWM — A tabbed tiling window manager inspired by Notion WM.

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
  Mod4+,             Move tab back
  Mod4+.             Move tab forward
  Mod4+Left/Right/Up/Down   Focus adjacent tile
  Mod4+Shift+Left/Right/Up/Down  Move window to adjacent tile
  Mod4+1…9           Switch workspace
  Mod4+Shift+1…9     Send active window to workspace
  Mod4+r             Restart
  F1                 Next tab in active tile
  F2                 Prev tab
  F6                 Close window
  F7                 dmenu_run
  F8                 thunar
  F10                firefox
  Mod4+q             Quit WM

Mouse:
  Click tab bar      Activate that tab
"""
import signal
import subprocess
import sys
import os
import struct
from Xlib.display import Display
from Xlib import X, XK, Xatom
from Xlib.protocol import event as xevent

# Constants

TAB_BAR_HEIGHT = 22
BORDER_WIDTH = 1          # coloured border around each tile
BORDER_GAP = 2            # gap between tiles
NUM_WORKSPACES = 9
TERMINAL_CMD = "xterm"
FM_CMD = "thunar"
WWW_CMD = "firefox"
F7_LAUNCHER_CMD = "dmenu_run"

# Bar reservation — set for polybar or any external bar.
# "top" or "bottom";  set BAR_HEIGHT = 0 to disable.
BAR_POSITION = "bottom"      # "top" | "bottom"
BAR_HEIGHT   = 24          # pixels reserved for the bar

# Hex colours
COL_TAB_ACTIVE_BG   = "#fbe7ac"
COL_TAB_INACTIVE_BG = "#3C3C3C"
COL_TAB_ACTIVE_FG   = "#000000"
COL_TAB_INACTIVE_FG = "#AAAAAA"
COL_TAB_BAR_BG      = "#2B2B2B"
COL_BORDER_ACTIVE   = "#696969"   # black highlight on active tile
COL_BORDER_INACTIVE = "#1E1E1E"   # nearly invisible when not focused

# Tree nodes

class Tile:
    """Leaf node — holds zero or more tabbed client windows."""

    def __init__(self, x=0, y=0, w=1, h=1):
        self.x, self.y, self.w, self.h = x, y, w, h
        self.windows = []       # list of X window ids (ints)
        self.active_tab = 0
        self.parent = None      # parent Split or None (root)
        self.tab_bar_win = None # X window for the tab bar (override-redirect)
        self.frame_win = None   # X window for the coloured border frame

    # Region available for client windows (below the tab bar, inside the border)
    @property
    def client_area(self):
        b = BORDER_WIDTH
        g = BORDER_GAP
        return (self.x + g + b,
                self.y + TAB_BAR_HEIGHT + g + b,
                max(1, self.w - 2 * (g + b)),
                max(1, self.h - TAB_BAR_HEIGHT - 2 * (g + b)))

    def is_leaf(self):
        return True

    def center(self):
        return (self.x + self.w // 2, self.y + self.h // 2)


class Split:
    """Internal node — divides space between exactly two children."""

    def __init__(self, orientation="horizontal", ratio=0.5):
        self.orientation = orientation   # "horizontal" (side-by-side) | "vertical" (top/bottom)
        self.ratio = ratio
        self.children = [None, None]
        self.x, self.y, self.w, self.h = 0, 0, 1, 1
        self.parent = None

    def is_leaf(self):
        return False


class Workspace:
    """One workspace — owns a tile tree and tracks which tile is focused."""

    def __init__(self, sw, sh, tile_y=0, tile_h=None):
        self.sw = sw
        self.sh = sh
        self.tile_y = tile_y
        self.tile_h = tile_h if tile_h else sh
        root_tile = Tile(0, self.tile_y, sw, self.tile_h)
        self.root = root_tile
        self.active_tile = root_tile

    # ----- helpers -----
    def all_tiles(self, node=None):
        """Yield every Tile in the tree."""
        if node is None:
            node = self.root
        if node.is_leaf():
            yield node
        else:
            for ch in node.children:
                if ch:
                    yield from self.all_tiles(ch)

    def all_windows(self):
        for t in self.all_tiles():
            yield from t.windows

    def find_tile_for_window(self, wid):
        for t in self.all_tiles():
            if wid in t.windows:
                return t
        return None

    def recalc(self, node=None):
        """Recompute geometry of every node from *node* downward."""
        if node is None:
            node = self.root
            node.x, node.y, node.w, node.h = 0, self.tile_y, self.sw, self.tile_h
        if node.is_leaf():
            return
        s = node
        c0, c1 = s.children
        if s.orientation == "horizontal":
            w0 = int(s.w * s.ratio)
            w1 = s.w - w0
            c0.x, c0.y, c0.w, c0.h = s.x, s.y, w0, s.h
            c1.x, c1.y, c1.w, c1.h = s.x + w0, s.y, w1, s.h
        else:
            h0 = int(s.h * s.ratio)
            h1 = s.h - h0
            c0.x, c0.y, c0.w, c0.h = s.x, s.y, s.w, h0
            c1.x, c1.y, c1.w, c1.h = s.x, s.y + h0, s.w, h1
        for ch in (c0, c1):
            self.recalc(ch)

    def replace_node(self, old, new):
        """Replace *old* with *new* in the tree, updating parent links."""
        new.parent = old.parent
        if old.parent is None:
            self.root = new
        else:
            p = old.parent
            idx = p.children.index(old)
            p.children[idx] = new

class TilingWM:
    """Core window-manager logic running on its own thread."""

    def __init__(self):
        self.dpy = Display()
        self.screen = self.dpy.screen()
        self.root_win = self.screen.root
        self.sw = self.screen.width_in_pixels
        self.sh = self.screen.height_in_pixels

        # Bar reservation — compute tileable region
        if BAR_HEIGHT > 0 and BAR_POSITION == "top":
            self.tile_y = BAR_HEIGHT
            self.tile_h = self.sh - BAR_HEIGHT
        elif BAR_HEIGHT > 0 and BAR_POSITION == "bottom":
            self.tile_y = 0
            self.tile_h = self.sh - BAR_HEIGHT
        else:
            self.tile_y = 0
            self.tile_h = self.sh

        # Colour map & pixel cache
        self.colormap = self.screen.default_colormap
        self._px_cache = {}

        # Font for tab labels
        try:
            self.font = self.dpy.open_font("-misc-fixed-medium-r-*-*-13-*-*-*-*-*-iso8859-1")
        except Exception:
            self.font = self.dpy.open_font("fixed")

        # Workspaces (with bar-aware geometry)
        self.workspaces = [
            Workspace(self.sw, self.sh, self.tile_y, self.tile_h)
            for _ in range(NUM_WORKSPACES)
        ]
        self.current_ws = 0

        # Track all managed client wids -> workspace index
        self.managed = {}   # wid -> ws_index

        # Windows that set _NET_WM_STRUT* (bars) — never tile these
        self._bar_windows = set()

        # The Tk desktop window id — we skip managing it
        #self.desktop_wid = None

        # Internal tab-bar / frame windows we must ignore
        self._tab_bars = set()
        self._frame_wins = set()

        # Floating drag state (fallback)
        self._drag_start = None
        self._drag_attr = None

        self._running = True

    # ----- colours -----
    def _px(self, hexcol):
        if hexcol in self._px_cache:
            return self._px_cache[hexcol]
        r = int(hexcol[1:3], 16) * 256
        g = int(hexcol[3:5], 16) * 256
        b = int(hexcol[5:7], 16) * 256
        px = self.colormap.alloc_color(r, g, b).pixel
        self._px_cache[hexcol] = px
        return px

    # ----- property helpers -----
    def _get_wm_name(self, wid):
        try:
            win = self.dpy.create_resource_object("window", wid)
            net_name_atom = self.dpy.intern_atom("_NET_WM_NAME")
            utf8_atom = self.dpy.intern_atom("UTF8_STRING")
            prop = win.get_full_property(net_name_atom, utf8_atom)
            if prop and prop.value:
                val = prop.value
                return val.decode("utf-8", errors="replace") if isinstance(val, bytes) else str(val)
            prop = win.get_full_property(Xatom.WM_NAME, Xatom.STRING)
            if prop and prop.value:
                val = prop.value
                return val.decode("latin-1", errors="replace") if isinstance(val, bytes) else str(val)
        except Exception:
            pass
        return "?"

    # ----- workspace helpers -----
    @property
    def ws(self):
        return self.workspaces[self.current_ws]

    # ----- frame (border highlight) management -----
    def _ensure_frame(self, tile):
        """Create or reposition the coloured border frame behind a tile."""
        g = BORDER_GAP
        fx, fy = tile.x + g, tile.y + g
        fw = max(1, tile.w - 2 * g)
        fh = max(1, tile.h - 2 * g)

        is_active = (tile is self.ws.active_tile)
        bg_col = COL_BORDER_ACTIVE if is_active else COL_BORDER_INACTIVE

        if tile.frame_win is None:
            win = self.root_win.create_window(
                fx, fy, fw, fh, 0,
                self.screen.root_depth,
                X.InputOutput,
                X.CopyFromParent,
                background_pixel=self._px(bg_col),
                override_redirect=True,
                event_mask=0,
            )
            tile.frame_win = win
            self._frame_wins.add(win.id)
            win.configure(stack_mode=X.Below)
            win.map()
        else:
            tile.frame_win.configure(x=fx, y=fy, width=fw, height=fh)
            tile.frame_win.change_attributes(background_pixel=self._px(bg_col))
            tile.frame_win.clear_area(0, 0, fw, fh)
            tile.frame_win.configure(stack_mode=X.Below)
            tile.frame_win.map()

    def _destroy_frame(self, tile):
        if tile.frame_win is not None:
            wid = tile.frame_win.id
            try:
                tile.frame_win.unmap()
                tile.frame_win.destroy()
            except Exception:
                pass
            self._frame_wins.discard(wid)
            tile.frame_win = None

    # ----- tab bar management -----
    def _ensure_tab_bar(self, tile):
        """Create (or reposition) the tab-bar window for *tile*."""
        g = BORDER_GAP
        b = BORDER_WIDTH
        bx = tile.x + g + b
        by = tile.y + g + b
        bw = max(1, tile.w - 2 * (g + b))
        bh = TAB_BAR_HEIGHT - b

        is_active = (tile is self.ws.active_tile)
        bar_bg = COL_BORDER_ACTIVE if is_active else COL_TAB_BAR_BG

        if tile.tab_bar_win is None:
            win = self.root_win.create_window(
                bx, by, bw, bh, 0,
                self.screen.root_depth,
                X.InputOutput,
                X.CopyFromParent,
                background_pixel=self._px(bar_bg),
                override_redirect=True,
                event_mask=X.ExposureMask | X.ButtonPressMask,
            )
            tile.tab_bar_win = win
            self._tab_bars.add(win.id)
            win.map()
        else:
            tile.tab_bar_win.configure(x=bx, y=by, width=bw, height=bh)
            tile.tab_bar_win.change_attributes(background_pixel=self._px(bar_bg))
            tile.tab_bar_win.map()

    def _destroy_tab_bar(self, tile):
        if tile.tab_bar_win is not None:
            wid = tile.tab_bar_win.id
            try:
                tile.tab_bar_win.unmap()
                tile.tab_bar_win.destroy()
            except Exception:
                pass
            self._tab_bars.discard(wid)
            tile.tab_bar_win = None


    def _draw_tab_bar(self, tile):
        """Render tab labels — double-buffered to prevent flicker."""
        if tile.tab_bar_win is None:
            return
        win = tile.tab_bar_win
        geom = win.get_geometry()
        w, h = geom.width, geom.height

        is_active = (tile is self.ws.active_tile)
        bar_bg = COL_BORDER_ACTIVE if is_active else COL_TAB_BAR_BG

        # Draw everything to an offscreen pixmap first
        pm = win.create_pixmap(w, h, self.screen.root_depth)

        gc_bg = pm.create_gc(foreground=self._px(bar_bg))
        pm.fill_rectangle(gc_bg, 0, 0, w, h)
        gc_bg.free()

        n = len(tile.windows)
        if n == 0:
            if is_active:
                gc_txt = pm.create_gc(foreground=self._px("#FFFFFF"), font=self.font)
                pm.draw_text(gc_txt, 6, h // 2 + 4, b"(empty)")
                gc_txt.free()
        else:
            tab_w = max(1, w // n)
            for i, wid in enumerate(tile.windows):
                is_tab_active = (i == tile.active_tab)
                bg = COL_TAB_ACTIVE_BG if is_tab_active else COL_TAB_INACTIVE_BG
                fg = COL_TAB_ACTIVE_FG if is_tab_active else COL_TAB_INACTIVE_FG

                x0 = i * tab_w
                tw = tab_w if i < n - 1 else (w - x0)

                gc = pm.create_gc(foreground=self._px(bg))
                pm.fill_rectangle(gc, x0, 0, tw, h)
                gc.free()

                if i < n - 1:
                    gc_sep = pm.create_gc(foreground=self._px(bar_bg))
                    pm.fill_rectangle(gc_sep, x0 + tw - 1, 0, 1, h)
                    gc_sep.free()

                title = self._get_wm_name(wid)
                if len(title) > 20:
                    title = title[:18] + ".."
                gc_txt = pm.create_gc(foreground=self._px(fg), font=self.font)
                pm.draw_text(gc_txt, x0 + 6, h // 2 + 4,
                             title.encode("latin-1", errors="replace"))
                gc_txt.free()

        # Single
        gc_copy = win.create_gc()
        win.copy_area(gc_copy, pm, 0, 0, w, h, 0, 0)
        gc_copy.free()
        pm.free()

        self.dpy.flush()
    # ----- tile arrangement -----
    def _arrange_tile(self, tile):
        """Configure all client windows in *tile* and refresh decorations."""
        self._ensure_frame(tile)
        self._ensure_tab_bar(tile)

        cx, cy, cw, ch = tile.client_area
        for i, wid in enumerate(tile.windows):
            try:
                win = self.dpy.create_resource_object("window", wid)
                if i == tile.active_tab:
                    win.configure(x=cx, y=cy, width=cw, height=ch, stack_mode=X.Above)
                    win.map()
                else:
                    win.unmap()
            except Exception:
                pass

        self._draw_tab_bar(tile)

    def _arrange_workspace(self, ws=None):
        """Recalc geometry and arrange every tile in the workspace."""
        if ws is None:
            ws = self.ws
        ws.recalc()
        for tile in ws.all_tiles():
            self._arrange_tile(tile)

    def _hide_workspace(self, ws):
        """Unmap every client, tab bar, and frame in *ws*."""
        for tile in ws.all_tiles():
            for wid in tile.windows:
                try:
                    self.dpy.create_resource_object("window", wid).unmap()
                except Exception:
                    pass
            if tile.tab_bar_win:
                try:
                    tile.tab_bar_win.unmap()
                except Exception:
                    pass
            if tile.frame_win:
                try:
                    tile.frame_win.unmap()
                except Exception:
                    pass

    def _show_workspace(self, ws):
        self._arrange_workspace(ws)

    # ----- focus -----
    def _focus_tile(self, tile):
        """Make *tile* the active tile and focus its active window."""
        self.ws.active_tile = tile
        if tile.windows:
            wid = tile.windows[tile.active_tab]
            try:
                win = self.dpy.create_resource_object("window", wid)
                win.set_input_focus(X.RevertToParent, X.CurrentTime)
                win.configure(stack_mode=X.Above)
            except Exception:
                pass
        # Redraw ALL tiles to update active/inactive borders and tab bars
        for t in self.ws.all_tiles():
            self._ensure_frame(t)
            self._ensure_tab_bar(t)
            self._draw_tab_bar(t)

    # ----- spatial navigation -----
    def _find_adjacent_tile(self, direction):
        """Find the tile adjacent to the active tile in *direction*."""
        current = self.ws.active_tile
        cx, cy = current.center()
        best = None
        best_dist = float("inf")

        for t in self.ws.all_tiles():
            if t is current:
                continue
            tx, ty = t.center()
            dx, dy = tx - cx, ty - cy

            ok = False
            if direction == "left" and dx < -10:
                ok = True
            elif direction == "right" and dx > 10:
                ok = True
            elif direction == "up" and dy < -10:
                ok = True
            elif direction == "down" and dy > 10:
                ok = True

            if ok:
                dist = dx * dx + dy * dy
                if dist < best_dist:
                    best_dist = dist
                    best = t
        return best

    # ----- public actions -----

    def action_move_tab_forward(self):
        """Swap active tab with the next one."""
        tile = self.ws.active_tile
        if len(tile.windows) < 2:
            return
        i = tile.active_tab
        j = (i + 1) % len(tile.windows)
        tile.windows[i], tile.windows[j] = tile.windows[j], tile.windows[i]
        tile.active_tab = j
        self._arrange_tile(tile)

    def action_move_tab_backward(self):
        """Swap active tab with the previous one."""
        tile = self.ws.active_tile
        if len(tile.windows) < 2:
            return
        i = tile.active_tab
        j = (i - 1) % len(tile.windows)
        tile.windows[i], tile.windows[j] = tile.windows[j], tile.windows[i]
        tile.active_tab = j
        self._arrange_tile(tile)

    def action_split(self, orientation, move_window=False):
        """Split the active tile into two.
        If move_window is True, move the currently active window
        into the newly created sibling tile."""
        tile = self.ws.active_tile
        new_split = Split(orientation, 0.5)
        new_split.x, new_split.y = tile.x, tile.y
        new_split.w, new_split.h = tile.w, tile.h

        sibling = Tile()

        # Replace tile in the tree FIRST while tile.parent is still correct
        self.ws.replace_node(tile, new_split)

        # Now wire up children
        new_split.children = [tile, sibling]
        tile.parent = new_split
        sibling.parent = new_split

        # Optionally move the active window to the new tile
        if move_window and tile.windows:
            wid = tile.windows[tile.active_tab]
            tile.windows.remove(wid)
            if tile.active_tab >= len(tile.windows):
                tile.active_tab = max(0, len(tile.windows) - 1)
            sibling.windows.append(wid)
            sibling.active_tab = 0
            self.ws.active_tile = sibling
        else:
            self.ws.active_tile = tile

        self._arrange_workspace()

    def action_remove_split(self):
        """Remove the split containing the active tile — merge with sibling."""
        tile = self.ws.active_tile
        parent = tile.parent
        if parent is None:
            return

        idx = parent.children.index(tile)
        sibling = parent.children[1 - idx]

        if sibling.is_leaf():
            for wid in sibling.windows:
                if wid not in tile.windows:
                    tile.windows.append(wid)
                self.managed[wid] = self.current_ws
            self._destroy_tab_bar(sibling)
            self._destroy_frame(sibling)
        else:
            sub_ws = self.workspaces[self.current_ws]
            for st in sub_ws.all_tiles(sibling):
                for wid in st.windows:
                    if wid not in tile.windows:
                        tile.windows.append(wid)
                    self.managed[wid] = self.current_ws
                self._destroy_tab_bar(st)
                self._destroy_frame(st)

        self._destroy_tab_bar(tile)
        self._destroy_frame(tile)

        tile.parent = parent.parent
        if parent.parent is None:
            self.ws.root = tile
        else:
            gp = parent.parent
            gp_idx = gp.children.index(parent)
            gp.children[gp_idx] = tile

        if tile.windows:
            tile.active_tab = min(tile.active_tab, len(tile.windows) - 1)

        self.ws.active_tile = tile
        self._arrange_workspace()

    def action_next_tab(self):
        tile = self.ws.active_tile
        if len(tile.windows) > 1:
            tile.active_tab = (tile.active_tab + 1) % len(tile.windows)
            self._arrange_tile(tile)
            self._focus_tile(tile)

    def action_prev_tab(self):
        tile = self.ws.active_tile
        if len(tile.windows) > 1:
            tile.active_tab = (tile.active_tab - 1) % len(tile.windows)
            self._arrange_tile(tile)
            self._focus_tile(tile)

    def action_focus_direction(self, direction):
        adj = self._find_adjacent_tile(direction)
        if adj:
            self._focus_tile(adj)
            self._arrange_tile(self.ws.active_tile)
            self._arrange_tile(adj)

    def action_move_window_direction(self, direction):
        """Move the active window from the current tile to the adjacent tile."""
        src = self.ws.active_tile
        if not src.windows:
            return
        dst = self._find_adjacent_tile(direction)
        if dst is None:
            return

        wid = src.windows[src.active_tab]
        src.windows.remove(wid)
        if src.active_tab >= len(src.windows):
            src.active_tab = max(0, len(src.windows) - 1)

        dst.windows.append(wid)
        dst.active_tab = len(dst.windows) - 1

        self._arrange_tile(src)
        self._arrange_tile(dst)
        self.ws.active_tile = dst
        self._focus_tile(dst)

    def action_switch_workspace(self, n):
        if n == self.current_ws:
            return
        self._hide_workspace(self.ws)
        self.current_ws = n
        self._show_workspace(self.ws)
        self._focus_tile(self.ws.active_tile)

    def action_send_to_workspace(self, n):
        if n == self.current_ws:
            return
        tile = self.ws.active_tile
        if not tile.windows:
            return
        wid = tile.windows[tile.active_tab]

        tile.windows.remove(wid)
        if tile.active_tab >= len(tile.windows):
            tile.active_tab = max(0, len(tile.windows) - 1)

        target_ws = self.workspaces[n]
        target_ws.active_tile.windows.append(wid)
        target_ws.active_tile.active_tab = len(target_ws.active_tile.windows) - 1
        self.managed[wid] = n

        try:
            self.dpy.create_resource_object("window", wid).unmap()
        except Exception:
            pass
        self._arrange_tile(tile)

    def action_spawn(self, cmd):
        subprocess.Popen(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, 
                         preexec_fn=os.setsid)

    def action_close_window(self):
        """Close the active window in the current tile."""
        tile = self.ws.active_tile
        if not tile.windows:
            return
        wid = tile.windows[tile.active_tab]
        win = self.dpy.create_resource_object("window", wid)

        # Try WM_DELETE_WINDOW (polite close) first
        try:
            wm_protocols = self.dpy.intern_atom("WM_PROTOCOLS")
            wm_delete = self.dpy.intern_atom("WM_DELETE_WINDOW")
            prop = win.get_full_property(wm_protocols, Xatom.ATOM)
            if prop and wm_delete in prop.value:
                # Send a ClientMessage asking the app to close
                from Xlib.protocol.event import ClientMessage
                ev = ClientMessage(
                    window=win,
                    client_type=wm_protocols,
                    data=(32, [wm_delete, X.CurrentTime, 0, 0, 0]),
                )
                win.send_event(ev)
                self.dpy.flush()
                return
        except Exception:
            pass

        # Fallback: force kill
        win.destroy()

    def action_restart(self):
        """Re-exec the WM process. All client windows survive."""
        # Destroy our internal windows so the new instance starts clean
        for ws in self.workspaces:
            for tile in ws.all_tiles():
                self._destroy_tab_bar(tile)
                self._destroy_frame(tile)
        self.dpy.close()
        # Replace this process with a fresh copy
        os.execvp(sys.executable, [sys.executable] + sys.argv)

    def action_quit(self):
        """Stop the WM and terminate the X session."""
        self._running = False
        # Destroy our decorations
        for ws in self.workspaces:
            for tile in ws.all_tiles():
                self._destroy_tab_bar(tile)
                self._destroy_frame(tile)
        self.dpy.close()
        # Kill the X session entirely
        os.kill(os.getpid(), signal.SIGTERM)

    # ----- manage / unmanage -----

    def _has_strut(self, wid):
        try:
            win = self.dpy.create_resource_object("window", wid)
            for prop_name in ("_NET_WM_STRUT_PARTIAL", "_NET_WM_STRUT"):
                atom = self.dpy.intern_atom(prop_name)
                prop = win.get_full_property(atom, Xatom.CARDINAL)
                if prop and prop.value and any(v > 0 for v in prop.value):
                    return True
        except Exception:
            pass
        return False

    def _is_dock_type(self, wid):
        try:
            win = self.dpy.create_resource_object("window", wid)
            type_atom = self.dpy.intern_atom("_NET_WM_WINDOW_TYPE")
            dock_atom = self.dpy.intern_atom("_NET_WM_WINDOW_TYPE_DOCK")
            prop = win.get_full_property(type_atom, Xatom.ATOM)
            if prop and prop.value:
                return dock_atom in prop.value
        except Exception:
            pass
        return False

    def _manage_window(self, wid):
        """Add a new client window to the active tile of the current workspace."""
        if wid in self.managed:
            return
        if wid in self._tab_bars:
            return
        if wid in self._frame_wins:
            return
        if wid in self._bar_windows:
            return

        try:
            win = self.dpy.create_resource_object("window", wid)
            attrs = win.get_attributes()
            if attrs.override_redirect:
                return
        except Exception:
            return

        if self._has_strut(wid) or self._is_dock_type(wid):
            self._bar_windows.add(wid)
            return

        try:
            win.change_attributes(event_mask=X.StructureNotifyMask | X.PropertyChangeMask)
        except Exception:
            pass

        tile = self.ws.active_tile
        tile.windows.append(wid)
        tile.active_tab = len(tile.windows) - 1
        self.managed[wid] = self.current_ws

        self._arrange_tile(tile)
        self._focus_tile(tile)

    def _unmanage_window(self, wid):
        self._bar_windows.discard(wid)
        if wid not in self.managed:
            return
        ws_idx = self.managed.pop(wid)
        ws = self.workspaces[ws_idx]
        tile = ws.find_tile_for_window(wid)
        if tile:
            tile.windows.remove(wid)
            if tile.active_tab >= len(tile.windows):
                tile.active_tab = max(0, len(tile.windows) - 1)
            if ws_idx == self.current_ws:
                self._arrange_tile(tile)

    # ----- X event handlers -----

    def _on_map_request(self, ev):
        wid = ev.window.id
        ev.window.map()
        self._manage_window(wid)

    def _on_configure_request(self, ev):
        wid = ev.window.id
        if wid in self.managed:
            return
        kwargs = {}
        if ev.value_mask & X.CWX: kwargs["x"] = ev.x
        if ev.value_mask & X.CWY: kwargs["y"] = ev.y
        if ev.value_mask & X.CWWidth: kwargs["width"] = max(1, ev.width)
        if ev.value_mask & X.CWHeight: kwargs["height"] = max(1, ev.height)
        if ev.value_mask & X.CWBorderWidth: kwargs["border_width"] = ev.border_width
        if ev.value_mask & X.CWStackMode: kwargs["stack_mode"] = ev.stack_mode
        try:
            ev.window.configure(**kwargs)
        except Exception:
            pass

    def _on_destroy_notify(self, ev):
        self._unmanage_window(ev.window.id)

    def _on_unmap_notify(self, ev):
        wid = ev.window.id
        if wid in self.managed:
            try:
                self.dpy.create_resource_object("window", wid).get_attributes()
            except Exception:
                self._unmanage_window(wid)

    def _on_expose(self, ev):
        wid = ev.window.id
        if wid in self._tab_bars:
            for tile in self.ws.all_tiles():
                if tile.tab_bar_win and tile.tab_bar_win.id == wid:
                    self._draw_tab_bar(tile)
                    break

    def _on_button_press(self, ev):
        wid = ev.window.id
        if wid in self._tab_bars:
            for tile in self.ws.all_tiles():
                if tile.tab_bar_win and tile.tab_bar_win.id == wid:
                    if not tile.windows:
                        self.ws.active_tile = tile
                        self._focus_tile(tile)
                        break
                    geom = tile.tab_bar_win.get_geometry()
                    tab_w = max(1, geom.width // len(tile.windows))
                    idx = min(ev.event_x // tab_w, len(tile.windows) - 1)
                    tile.active_tab = idx
                    self.ws.active_tile = tile
                    self._arrange_tile(tile)
                    self._focus_tile(tile)
                    break
            return

        if wid in self.managed and self.managed[wid] == self.current_ws:
            tile = self.ws.find_tile_for_window(wid)
            if tile:
                self.ws.active_tile = tile
                tile.active_tab = tile.windows.index(wid)
                self._focus_tile(tile)

    def _on_property_notify(self, ev):
        wid = ev.window.id
        if wid in self.managed and self.managed[wid] == self.current_ws:
            tile = self.ws.find_tile_for_window(wid)
            if tile:
                self._draw_tab_bar(tile)

    def _on_key_press(self, ev):
        keysym = self.dpy.keycode_to_keysym(ev.detail, 0)
        shifted = bool(ev.state & X.ShiftMask)

        # Mod4+r — restart WM (reload code changes)
        if keysym == XK.XK_r:
            self.action_restart()
            return

        # F6 — close active tab/window
        if keysym == XK.XK_F6:
            self.action_close_window()
            return

        # F7 — launch external program (no modifier required)
        if keysym == XK.XK_F7:
            self.action_spawn(F7_LAUNCHER_CMD)
            return

        # Mod4+Return — spawn terminal
        if keysym == XK.XK_Return:
            self.action_spawn(TERMINAL_CMD)
            return

        if keysym == XK.XK_F8:
            self.action_spawn(FM_CMD)
            return

        if keysym == XK.XK_F10:
            self.action_spawn(WWW_CMD)
            return

        # Mod4+h / Mod4+Shift+h — horizontal split (shift = move window too)
        if keysym == XK.XK_h:
            self.action_split("horizontal", move_window=shifted)
            return

        # Mod4+v / Mod4+Shift+v — vertical split (shift = move window too)
        if keysym == XK.XK_v:
            self.action_split("vertical", move_window=shifted)
            return

        # Mod4+d — remove split
        if keysym == XK.XK_d and not shifted:
            self.action_remove_split()
            return

        # F1 / F2 — cycle tabs
        if keysym == XK.XK_F1:
            self.action_next_tab()
            return
        if keysym == XK.XK_F2:
            self.action_prev_tab()
            return
        # Arrows — focus or move
        direction_map = {
            XK.XK_Left: "left", XK.XK_Right: "right",
            XK.XK_Up: "up", XK.XK_Down: "down",
        }
        if keysym in direction_map:
            d = direction_map[keysym]
            if shifted:
                self.action_move_window_direction(d)
            else:
                self.action_focus_direction(d)
            return

        # Mod4+1-9 — workspaces;  Mod4+Shift+1-9 — send to workspace
        num_syms = [XK.XK_1, XK.XK_2, XK.XK_3, XK.XK_4, XK.XK_5,
                    XK.XK_6, XK.XK_7, XK.XK_8, XK.XK_9]
       
        if keysym in num_syms:
            idx = num_syms.index(keysym)
            if shifted:
                self.action_send_to_workspace(idx)
            else:
                self.action_switch_workspace(idx)
            return

        # Mod4+q — quit
        if keysym == XK.XK_q:
            self.action_quit()
            return

        # Mod4+period — move tab forward
        if keysym == XK.XK_period:
            self.action_move_tab_forward()
            return

        # Mod4+comma — move tab backward
        if keysym == XK.XK_comma:
            self.action_move_tab_backward()
            return

    # ----- grab keys & buttons -----
    def _grab_keys(self):
        keys = [
            "Return", "h", "v", "d", "r", "q",
            "Left", "Right", "Up", "Down",
            "1", "2", "3", "4", "5", "6", "7", "8", "9",
            "comma", "period",
        ]
        for name in keys:
            ks = XK.string_to_keysym(name)
            kc = self.dpy.keysym_to_keycode(ks)
            if kc:
                self.root_win.grab_key(kc, X.Mod4Mask, True,
                                       X.GrabModeAsync, X.GrabModeAsync)
                self.root_win.grab_key(kc, X.Mod4Mask | X.Mod2Mask, True,
                                       X.GrabModeAsync, X.GrabModeAsync)
                self.root_win.grab_key(kc, X.Mod4Mask | X.LockMask, True,
                                       X.GrabModeAsync, X.GrabModeAsync)
                self.root_win.grab_key(kc, X.Mod4Mask | X.ShiftMask, True,
                                       X.GrabModeAsync, X.GrabModeAsync)
                self.root_win.grab_key(kc, X.Mod4Mask | X.ShiftMask | X.Mod2Mask, True,
                                       X.GrabModeAsync, X.GrabModeAsync)

        # Grab button1 on root for tab-bar clicks
        self.root_win.grab_button(1, X.AnyModifier, True,
                                  X.ButtonPressMask, X.GrabModeSync, X.GrabModeAsync,
                                  X.NONE, X.NONE)

        f1_kc = self.dpy.keysym_to_keycode(XK.string_to_keysym("F1"))
        if f1_kc:
            for mod in (0, X.Mod2Mask, X.LockMask, X.Mod2Mask | X.LockMask):
                self.root_win.grab_key(f1_kc, mod, True,
                                       X.GrabModeAsync, X.GrabModeAsync)

        f2_kc = self.dpy.keysym_to_keycode(XK.string_to_keysym("F2"))
        if f2_kc:
            for mod in (0, X.Mod2Mask, X.LockMask, X.Mod2Mask | X.LockMask):
                self.root_win.grab_key(f2_kc, mod, True,
                                       X.GrabModeAsync, X.GrabModeAsync)


        f6_kc = self.dpy.keysym_to_keycode(XK.string_to_keysym("F6"))
        if f6_kc:
            for mod in (0, X.Mod2Mask, X.LockMask, X.Mod2Mask | X.LockMask):
                self.root_win.grab_key(f6_kc, mod, True,
                                       X.GrabModeAsync, X.GrabModeAsync)

        f7_kc = self.dpy.keysym_to_keycode(XK.string_to_keysym("F7"))
        if f7_kc:
            for mod in (0, X.Mod2Mask, X.LockMask, X.Mod2Mask | X.LockMask):
                self.root_win.grab_key(f7_kc, mod, True,
                                       X.GrabModeAsync, X.GrabModeAsync)

        f8_kc = self.dpy.keysym_to_keycode(XK.string_to_keysym("F8"))
        if f8_kc:
            for mod in (0, X.Mod2Mask, X.LockMask, X.Mod2Mask | X.LockMask):
                self.root_win.grab_key(f8_kc, mod, True,
                                       X.GrabModeAsync, X.GrabModeAsync)

        f10_kc = self.dpy.keysym_to_keycode(XK.string_to_keysym("F10"))
        if f10_kc:
            for mod in (0, X.Mod2Mask, X.LockMask, X.Mod2Mask | X.LockMask):
                self.root_win.grab_key(f10_kc, mod, True,
                                       X.GrabModeAsync, X.GrabModeAsync)



    # ----- main loop -----
    def run(self):
        try:
            self.root_win.change_attributes(
                event_mask=(X.SubstructureRedirectMask |
                            X.SubstructureNotifyMask |
                            X.KeyPressMask |
                            X.ButtonPressMask |
                            X.FocusChangeMask)
            )
        except Exception as e:
            print(f"Another WM is running: {e}", file=sys.stderr)
            return

        self._grab_keys()
        self.dpy.sync()

        # Manage existing windows
        try:
            children = self.root_win.query_tree().children
            for ch in children:
                try:
                    attrs = ch.get_attributes()
                    if attrs.map_state == X.IsViewable and not attrs.override_redirect:
                        if ch.id != self.desktop_wid:
                            self._manage_window(ch.id)
                except Exception:
                    pass
        except Exception:
            pass

        self._arrange_workspace()

        while self._running:
            try:
                ev = self.dpy.next_event()
            except Exception:
                break

            if ev.type == X.MapRequest:
                self._on_map_request(ev)
            elif ev.type == X.ConfigureRequest:
                self._on_configure_request(ev)
            elif ev.type == X.DestroyNotify:
                self._on_destroy_notify(ev)
            elif ev.type == X.UnmapNotify:
                self._on_unmap_notify(ev)
            elif ev.type == X.Expose:
                self._on_expose(ev)
            elif ev.type == X.KeyPress:
                self._on_key_press(ev)
            elif ev.type == X.ButtonPress:
                self._on_button_press(ev)
                self.dpy.allow_events(X.ReplayPointer, X.CurrentTime)
            elif ev.type == X.PropertyNotify:
                self._on_property_notify(ev)

        # Cleanup
        for ws in self.workspaces:
            for tile in ws.all_tiles():
                self._destroy_tab_bar(tile)
                self._destroy_frame(tile)
        self.dpy.close()

def main():
    signal.signal(signal.SIGCHLD, signal.SIG_IGN)
    wm = TilingWM()
    bg_px = wm._px(COL_DESKTOP_BG)
    wm.root_win.change_attributes(background_pixel=bg_px)
    wm.root_win.clear_area(0, 0, wm.sw, wm.sh)
    wm.desktop_wid = None
    wm.run()


if __name__ == "__main__":
    main()
