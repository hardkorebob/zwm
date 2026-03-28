/*
 * SWM — A scriptable tabbed tiling window manager inspired by Notion and TinyWM.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NUM_WORKSPACES 9
#define CMD_SOCK_PATH "/tmp/swm.sock"
#define CMD_MAX_CLIENTS 8
#define CMD_BUF_SIZE 512
#define MAX_TILES 256
#define MAX_MANAGED 512
#define MAX_SET 512
#define MAX_COLORS 256
#define MAX_BINDINGS 64
#define CLEAN_MASK(m) ((m) & ~(Mod2Mask | LockMask))

typedef enum
{
  ACT_SPAWN,
  ACT_SPLIT,
  ACT_REMOVE_SPLIT,
  ACT_CLOSE_WINDOW,
  ACT_QUIT,
  ACT_NEXT_TAB,
  ACT_PREV_TAB,
  ACT_MOVE_TAB_FWD,
  ACT_MOVE_TAB_BWD,
  ACT_NEXT_WORKSPACE,
  ACT_PREV_WORKSPACE,
  ACT_SWITCH_WORKSPACE,
  ACT_SEND_TO_WORKSPACE,
  ACT_FOCUS_DIR,
  ACT_MOVE_WIN_DIR,
} Action;

typedef struct
{
  KeySym key;
  unsigned mod;
  Action action;
  int iarg;
  char *sarg;
} Keybind;

typedef struct
{

  int tab_bar_height;
  int border_width;
  int border_gap;
  int statusbar_height;
  int statusbar_pos;
  int timebar_pos;
  int timebar_height;
  double bar_update_interval;

  char col_statusbar_bg[8];
  char col_statusbar_fg[8];
  char col_statusbar_ws_active[8];
  char col_statusbar_ws_inactive[8];
  char col_statusbar_ws_occupied[8];
  char col_statusbar_ws_fg_act[8];
  char col_statusbar_ws_fg_inact[8];

  char col_tab_active_bg[8];
  char col_tab_inactive_bg[8];
  char col_tab_active_fg[8];
  char col_tab_inactive_fg[8];
  char col_tab_bar_bg[8];
  char col_tab_active_bg_dim[8];
  char col_tab_active_fg_dim[8];

  char col_border_active[8];
  char col_border_inactive[8];
  char col_desktop_bg[8];

  char col_timebar_bg[8];
  char col_timebar_hour[8];
  char col_timebar_hex[8];
  char col_timebar_seg[8];
  char col_timebar_label[8];
  char col_timebar_track[8];

  Keybind binds[MAX_BINDINGS];
  int n_binds;
} Config;

typedef enum
{
  CFG_INT,
  CFG_DOUBLE
} CfgNumKind;

typedef enum
{
  NODE_TILE,
  NODE_SPLIT
} NodeType;

typedef struct Node
{
  NodeType type;
  int x, y, w, h;
  struct Node *parent;
  union
  {
    struct
    {
      Window *windows;
      int nwindows;
      int win_cap;
      int active_tab;
      int prev_active_tab;
      Window tab_bar_win;
      Window frame_win;
    } tile;
    struct
    {
      int horizontal;
      double ratio;
      struct Node *children[2];
    } split;
  };
} Node;

typedef struct
{
  int sw, sh, tile_y, tile_h;
  Node *root;
  Node *active_tile;
  Window fullscreen_win;

  Node *cached_tiles[MAX_TILES];
  int n_cached_tiles;
  int tiles_dirty;
} Workspace;

typedef struct
{
  Window wid;
  int ws;
} ManagedEntry;

typedef struct
{
  char hex[8];
  unsigned long px;
} ColorEntry;

static const struct
{
  const char *name;
  size_t off;
} col_map[] = {
  { "col_statusbar_bg", offsetof (Config, col_statusbar_bg) },
  { "col_statusbar_fg", offsetof (Config, col_statusbar_fg) },
  { "col_statusbar_ws_active", offsetof (Config, col_statusbar_ws_active) },
  { "col_statusbar_ws_inactive", offsetof (Config, col_statusbar_ws_inactive) },
  { "col_statusbar_ws_occupied", offsetof (Config, col_statusbar_ws_occupied) },
  { "col_statusbar_ws_fg_act", offsetof (Config, col_statusbar_ws_fg_act) },
  { "col_statusbar_ws_fg_inact", offsetof (Config, col_statusbar_ws_fg_inact) },
  { "col_tab_active_bg", offsetof (Config, col_tab_active_bg) },
  { "col_tab_inactive_bg", offsetof (Config, col_tab_inactive_bg) },
  { "col_tab_active_fg", offsetof (Config, col_tab_active_fg) },
  { "col_tab_inactive_fg", offsetof (Config, col_tab_inactive_fg) },
  { "col_tab_bar_bg", offsetof (Config, col_tab_bar_bg) },
  { "col_tab_active_bg_dim", offsetof (Config, col_tab_active_bg_dim) },
  { "col_tab_active_fg_dim", offsetof (Config, col_tab_active_fg_dim) },
  { "col_border_active", offsetof (Config, col_border_active) },
  { "col_border_inactive", offsetof (Config, col_border_inactive) },
  { "col_desktop_bg", offsetof (Config, col_desktop_bg) },
  { "col_timebar_bg", offsetof (Config, col_timebar_bg) },
  { "col_timebar_hour", offsetof (Config, col_timebar_hour) },
  { "col_timebar_hex", offsetof (Config, col_timebar_hex) },
  { "col_timebar_seg", offsetof (Config, col_timebar_seg) },
  { "col_timebar_label", offsetof (Config, col_timebar_label) },
  { "col_timebar_track", offsetof (Config, col_timebar_track) },
  { NULL, 0 }
};

static const struct
{
  const char *name;
  size_t off;
  CfgNumKind kind;
} num_map[]
    = { { "tab_bar_height", offsetof (Config, tab_bar_height), CFG_INT },
        { "border_width", offsetof (Config, border_width), CFG_INT },
        { "border_gap", offsetof (Config, border_gap), CFG_INT },
        { "statusbar_height", offsetof (Config, statusbar_height), CFG_INT },
        { "statusbar_pos", offsetof (Config, statusbar_pos), CFG_INT },
        { "timebar_pos", offsetof (Config, timebar_pos), CFG_INT },
        { "timebar_height", offsetof (Config, timebar_height), CFG_INT },
        { "bar_update_interval", offsetof (Config, bar_update_interval),
          CFG_DOUBLE },
        { NULL, 0, 0 } };

static const struct
{
  const char *name;
  Action act;
} action_map[] = { { "spawn", ACT_SPAWN },
                   { "split_h", ACT_SPLIT },
                   { "split_v", ACT_SPLIT },
                   { "split_h_move", ACT_SPLIT },
                   { "split_v_move", ACT_SPLIT },
                   { "remove_split", ACT_REMOVE_SPLIT },
                   { "close", ACT_CLOSE_WINDOW },
                   { "quit", ACT_QUIT },
                   { "next_tab", ACT_NEXT_TAB },
                   { "prev_tab", ACT_PREV_TAB },
                   { "move_tab_fwd", ACT_MOVE_TAB_FWD },
                   { "move_tab_bwd", ACT_MOVE_TAB_BWD },
                   { "next_ws", ACT_NEXT_WORKSPACE },
                   { "prev_ws", ACT_PREV_WORKSPACE },
                   { "switch_ws", ACT_SWITCH_WORKSPACE },
                   { "send_to_ws", ACT_SEND_TO_WORKSPACE },
                   { "focus", ACT_FOCUS_DIR },
                   { "move_win", ACT_MOVE_WIN_DIR },
                   { NULL, 0 } };

static struct
{
  int fd;
  char buf[CMD_BUF_SIZE];
  int len;
} cmd_clients[CMD_MAX_CLIENTS];

static Display *dpy;
static int scr_num;
static Screen *scr;
static Window root_win;
static int sw, sh;
static int tile_y_off, tile_h_val;
static Colormap cmap;
static XFontStruct *font;
static int depth;
static Workspace workspaces[NUM_WORKSPACES];
static int cur_ws;
static ManagedEntry managed[MAX_MANAGED];
static int n_managed;
static Window tab_bars[MAX_SET];
static int n_tab_bars;
static Window frame_wins[MAX_SET];
static int n_frame_wins;
static Window status_bar_win;
static Window timebar_win;
static Window float_wins[MAX_SET];
static int n_float_wins;
static double last_bar_update;
static int running;
static ColorEntry color_cache[MAX_COLORS];
static int n_colors;
static Atom a_net_wm_name, a_utf8, a_wm_protocols, a_wm_delete;
static Atom a_wm_type, a_wm_type_dock;
static Atom a_net_wm_state, a_net_wm_state_fullscreen;
static Atom a_wm_type_splash, a_wm_type_dialog;
static Atom a_wm_type_tooltip, a_wm_type_notification;
static Atom a_wm_type_popup_menu;
static Window ewmh_check_win;
static GC draw_gc;
static int cmd_listen_fd = -1;
static char cmd_sock_path[256];
static void ensure_frame (Node *tile);
static void destroy_frame (Node *tile);
static void ensure_tab_bar (Node *tile);
static void destroy_tab_bar (Node *tile);
static void draw_tab_bar (Node *tile);
static void arrange_tile (Node *tile);
static void arrange_workspace (Workspace *w);
static void raise_floats (void);
static void hide_workspace (Workspace *w);
static void focus_tile (Node *tile);
static void focus_set_input (Node *tile);
static void send_configure_notify (Window wid);
static void bar_draw (void);
static void bar_destroy (void);
static void timebar_init (void);
static void timebar_draw (void);
static void timebar_destroy (void);
static void unmanage_window (Window wid);
static void cmd_init (void);
static void cmd_cleanup (void);
static void cmd_poll (fd_set *fds);
static void action_switch_workspace (int n);
static int valid_hex_color (const char *hex);
static volatile sig_atomic_t reload_pending;
static volatile sig_atomic_t apply_pending;
static void cfg_apply (void);
static void grab_keys (void);
static Config cfg;
static int n_cmd_clients;
static int wm_detected;

static void
sarg_set (char **dst, const char *src)
{
  free (*dst);
  *dst = (src && *src) ? strdup (src) : NULL;
}

static void
cfg_defaults (void)
{
  cfg.tab_bar_height = 22;
  cfg.border_width = 1;
  cfg.border_gap = 2;
  cfg.statusbar_height = 24;
  cfg.statusbar_pos = 0;
  cfg.timebar_pos = 1;
  cfg.timebar_height = 14;
  cfg.bar_update_interval = 1.0;

#define SETCOL(dst, lit)                                                      \
  do                                                                          \
    {                                                                         \
      _Static_assert (sizeof (lit) == 8, "color literal must be 7 chars");    \
      memcpy ((dst), (lit), 8);                                               \
    }                                                                         \
  while (0)
  SETCOL (cfg.col_statusbar_bg, "#1E1E1E");
  SETCOL (cfg.col_statusbar_fg, "#FFBF00");
  SETCOL (cfg.col_statusbar_ws_active, "#fbe7ac");
  SETCOL (cfg.col_statusbar_ws_inactive, "#555555");
  SETCOL (cfg.col_statusbar_ws_occupied, "#888888");
  SETCOL (cfg.col_statusbar_ws_fg_act, "#000000");
  SETCOL (cfg.col_statusbar_ws_fg_inact, "#AAAAAA");

  SETCOL (cfg.col_tab_active_bg, "#fbe7ac");
  SETCOL (cfg.col_tab_inactive_bg, "#3C3C3C");
  SETCOL (cfg.col_tab_active_fg, "#000000");
  SETCOL (cfg.col_tab_inactive_fg, "#AAAAAA");
  SETCOL (cfg.col_tab_bar_bg, "#2B2B2B");
  SETCOL (cfg.col_tab_active_bg_dim, "#555555");
  SETCOL (cfg.col_tab_active_fg_dim, "#CCCCCC");

  SETCOL (cfg.col_border_active, "#696969");
  SETCOL (cfg.col_border_inactive, "#1E1E1E");
  SETCOL (cfg.col_desktop_bg, "#000000");

  SETCOL (cfg.col_timebar_bg, "#1E1E1E");
  SETCOL (cfg.col_timebar_hour, "#CC3300");
  SETCOL (cfg.col_timebar_hex, "#00CC66");
  SETCOL (cfg.col_timebar_seg, "#CCAA00");
  SETCOL (cfg.col_timebar_label, "#FFFFFF");
  SETCOL (cfg.col_timebar_track, "#333333");
#undef SETCOL

  for (int i = 0; i < cfg.n_binds; i++)
    free (cfg.binds[i].sarg);
  cfg.n_binds = 0;
}

static void
cfg_default_binds (void)
{
#define B(k, m, a, i, s)                                                      \
  do                                                                          \
    {                                                                         \
      Keybind *b = &cfg.binds[cfg.n_binds++];                                 \
      b->key = (k);                                                           \
      b->mod = (m);                                                           \
      b->action = (a);                                                        \
      b->iarg = (i);                                                          \
      b->sarg = NULL;                                                         \
      sarg_set (&b->sarg, (s));                                               \
    }                                                                         \
  while (0)

  B (XK_F1, 0, ACT_PREV_TAB, 0, NULL);
  B (XK_F2, 0, ACT_NEXT_TAB, 0, NULL);
  B (XK_F3, 0, ACT_PREV_WORKSPACE, 0, NULL);
  B (XK_F4, 0, ACT_NEXT_WORKSPACE, 0, NULL);
  B (XK_F6, 0, ACT_CLOSE_WINDOW, 0, NULL);
  B (XK_h, Mod4Mask, ACT_SPLIT, 1, NULL);
  B (XK_v, Mod4Mask, ACT_SPLIT, 0, NULL);
  B (XK_d, Mod4Mask, ACT_REMOVE_SPLIT, 0, NULL);
  B (XK_q, Mod4Mask, ACT_QUIT, 0, NULL);
  B (XK_comma, Mod4Mask, ACT_MOVE_TAB_BWD, 0, NULL);
  B (XK_period, Mod4Mask, ACT_MOVE_TAB_FWD, 0, NULL);
  B (XK_Left, Mod4Mask, ACT_FOCUS_DIR, 0, "left");
  B (XK_Right, Mod4Mask, ACT_FOCUS_DIR, 0, "right");
  B (XK_Up, Mod4Mask, ACT_FOCUS_DIR, 0, "up");
  B (XK_Down, Mod4Mask, ACT_FOCUS_DIR, 0, "down");
  B (XK_h, Mod4Mask | ShiftMask, ACT_SPLIT, 3, NULL);
  B (XK_v, Mod4Mask | ShiftMask, ACT_SPLIT, 2, NULL);
  B (XK_Left, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "left");
  B (XK_Right, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "right");
  B (XK_Up, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "up");
  B (XK_Down, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "down");

  for (int i = 0; i < 9; i++)
    B (XK_1 + i, Mod4Mask, ACT_SWITCH_WORKSPACE, i, NULL);

  for (int i = 0; i < 9; i++)
    B (XK_1 + i, Mod4Mask | ShiftMask, ACT_SEND_TO_WORKSPACE, i, NULL);

#undef B
}

static void
strip_quotes (char *s)
{
  size_t len = strlen (s);
  if (len >= 2
      && ((s[0] == '"' && s[len - 1] == '"')
      || (s[0] == '\'' && s[len - 1] == '\'')))
    {
      memmove (s, s + 1, len - 2);
      s[len - 2] = '\0';
    }
}

static unsigned
parse_mod_token (const char *s)
{
  if (strcmp (s, "Mod4") == 0 || strcmp (s, "Super") == 0)
    return Mod4Mask;
  if (strcmp (s, "Shift") == 0)
    return ShiftMask;
  if (strcmp (s, "Mod1") == 0 || strcmp (s, "Alt") == 0)
    return Mod1Mask;
  if (strcmp (s, "Control") == 0 || strcmp (s, "Ctrl") == 0)
    return ControlMask;
  if (strcmp (s, "Mod2") == 0)
    return Mod2Mask;
  if (strcmp (s, "Mod3") == 0)
    return Mod3Mask;
  if (strcmp (s, "Mod5") == 0)
    return Mod5Mask;
  return 0;
}

static int
split_iarg (const char *name)
{
  if (strcmp (name, "split_h") == 0)
    return 1;
  if (strcmp (name, "split_v") == 0)
    return 0;
  if (strcmp (name, "split_h_move") == 0)
    return 3;
  if (strcmp (name, "split_v_move") == 0)
    return 2;
  return 0;
}

static int
spawn_slot (const char *arg)
{
  if (strcmp (arg, "terminal") == 0)
    return 0;
  if (strcmp (arg, "file_manager") == 0)
    return 1;
  if (strcmp (arg, "browser") == 0)
    return 2;
  if (strcmp (arg, "launcher") == 0)
    return 3;
  if (strcmp (arg, "reload") == 0)
    return 4;
  return -1;
}

static int
cfg_parse_bind (const char *val)
{
  if (cfg.n_binds >= MAX_BINDINGS)
    {
      fprintf (stderr, "swm: max bindings (%d) reached\n", MAX_BINDINGS);
      return 0;
    }

  char buf[256];
  strncpy (buf, val, sizeof (buf) - 1);
  buf[sizeof (buf) - 1] = '\0';

  char *chord = buf;
  while (*chord == ' ' || *chord == '\t')
    chord++;

  char *sp = chord;
  while (*sp && *sp != ' ' && *sp != '\t')
    sp++;
  if (!*sp)
    {
      fprintf (stderr, "swm: bind: missing action in '%s'\n", val);
      return 0;
    }
  *sp++ = '\0';

  while (*sp == ' ' || *sp == '\t')
    sp++;
  char *act_name = sp;

  while (*sp && *sp != ' ' && *sp != '\t')
    sp++;
  char *arg = NULL;
  if (*sp)
    {
      *sp++ = '\0';
      while (*sp == ' ' || *sp == '\t')
        sp++;
      if (*sp)
        arg = sp;

      if (arg)
        {
          char *e = arg + strlen (arg) - 1;
          while (e > arg && (*e == ' ' || *e == '\t'))
            *e-- = '\0';
          strip_quotes (arg);
        }
    }

  unsigned mod = 0;
  KeySym ks = NoSymbol;
  {
    char chord_buf[128];
    strncpy (chord_buf, chord, sizeof (chord_buf) - 1);
    chord_buf[sizeof (chord_buf) - 1] = '\0';

    char *tokens[8];
    int ntok = 0;
    char *t = strtok (chord_buf, "+");
    while (t && ntok < 8)
      {
        tokens[ntok++] = t;
        t = strtok (NULL, "+");
      }

    if (ntok == 0)
      {
        fprintf (stderr, "swm: bind: empty chord in '%s'\n", val);
        return 0;
      }

    for (int i = 0; i < ntok - 1; i++)
      {
        unsigned m = parse_mod_token (tokens[i]);
        if (!m)
          {
            fprintf (stderr, "swm: bind: unknown modifier '%s'\n", tokens[i]);
            return 0;
          }
        mod |= m;
      }

    ks = XStringToKeysym (tokens[ntok - 1]);
    if (ks == NoSymbol)
      {
        fprintf (stderr, "swm: bind: unknown key '%s'\n", tokens[ntok - 1]);
        return 0;
      }
  }

  Action action = (Action)-1;
  for (int i = 0; action_map[i].name; i++)
    {
      if (strcmp (act_name, action_map[i].name) == 0)
        {
          action = action_map[i].act;
          break;
        }
    }
  if ((int)action == -1)
    {
      fprintf (stderr, "swm: bind: unknown action '%s'\n", act_name);
      return 0;
    }

  Keybind *b = &cfg.binds[cfg.n_binds];
  memset (b, 0, sizeof (*b));
  b->key = ks;
  b->mod = mod;
  b->action = action;

  switch (action)
    {
    case ACT_SPLIT:
      b->iarg = split_iarg (act_name);
      break;
    case ACT_SPAWN:
      if (arg)
        {
          int slot = spawn_slot (arg);
          if (slot >= 0)
            {
              b->iarg = slot;
            }
          else
            {

              b->iarg = -1;
              sarg_set (&b->sarg, arg);
            }
        }
      break;
    case ACT_SWITCH_WORKSPACE:
    case ACT_SEND_TO_WORKSPACE:
      if (arg)
        b->iarg = atoi (arg) - 1;
      if (b->iarg < 0)
        b->iarg = 0;
      if (b->iarg >= NUM_WORKSPACES)
        b->iarg = NUM_WORKSPACES - 1;
      break;
    case ACT_FOCUS_DIR:
    case ACT_MOVE_WIN_DIR:
      if (arg)
        sarg_set (&b->sarg, arg);
      break;
    default:
      break;
    }

  cfg.n_binds++;
  return 1;
}

static int
cfg_resolve_path (char *out, int len)
{
  const char *env;
  env = getenv ("XDG_CONFIG_HOME");
  if (env && *env)
    {
      snprintf (out, len, "%s/swm/config", env);
      if (access (out, R_OK) == 0)
        return 1;
    }
  const char *home = getenv ("HOME");
  if (!home)
    home = "/root";
  snprintf (out, len, "%s/.config/swm/config", home);
  if (access (out, R_OK) == 0)
    return 1;
  return 0;
}

static int
cfg_set_kv (const char *key, const char *val)
{

  for (int i = 0; num_map[i].name; i++)
    {
      if (strcmp (key, num_map[i].name) == 0)
        {
          if (num_map[i].kind == CFG_DOUBLE)
            *(double *)((char *)&cfg + num_map[i].off) = atof (val);
          else
            *(int *)((char *)&cfg + num_map[i].off) = atoi (val);
          return 1;
        }
    }

  if (strcmp (key, "bind") == 0)
    return cfg_parse_bind (val);

  for (int i = 0; col_map[i].name; i++)
    {
      if (strcmp (key, col_map[i].name) == 0)
        {
          if (!valid_hex_color (val))
            {
              fprintf (stderr, "swm: invalid color value '%s' for '%s'\n", val,
                       key);
              return 0;
            }
          memcpy ((char *)&cfg + col_map[i].off, val, 7);
          ((char *)&cfg + col_map[i].off)[7] = '\0';
          return 1;
        }
    }

  return 0;
}

static int
cfg_load (const char *path)
{
  FILE *f = fopen (path, "r");
  if (!f)
    return -1;

  char line[512];
  int lineno = 0, count = 0;
  while (fgets (line, sizeof (line), f))
    {
      lineno++;

      char *nl = strchr (line, '\n');
      if (nl)
        *nl = '\0';

      char *p = line;
      while (*p == ' ' || *p == '\t')
        p++;
      if (*p == '#' || *p == '\0')
        continue;

      char *eq = strchr (p, '=');
      if (!eq)
        {
          fprintf (stderr, "swm: config:%d: missing '='\n", lineno);
          continue;
        }

      char *kend = eq - 1;
      while (kend > p && (*kend == ' ' || *kend == '\t'))
        kend--;
      *(kend + 1) = '\0';

      char *val = eq + 1;
      while (*val == ' ' || *val == '\t')
        val++;

      char *vend = val + strlen (val) - 1;
      while (vend > val && (*vend == ' ' || *vend == '\t'))
        *vend-- = '\0';

      strip_quotes (val);

      if (cfg_set_kv (p, val))
        count++;
    }
  fclose (f);
  return count;
}

static void
on_sighup (int sig)
{
  (void)sig;
  reload_pending = 1;
}

static int
is_hex_digit (char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int
valid_hex_color (const char *hex)
{
  if (!hex || hex[0] != '#')
    return 0;
  for (int i = 1; i <= 6; i++)
    if (!is_hex_digit (hex[i]))
      return 0;
  return 1;
}

static int
hex_val (char c)
{
  if (c >= 'a')
    return c - 'a' + 10;
  if (c >= 'A')
    return c - 'A' + 10;
  return c - '0';
}

static unsigned long
px (const char *hex)
{
  int i;
  for (i = 0; i < n_colors; i++)
    if (strcmp (color_cache[i].hex, hex) == 0)
      return color_cache[i].px;

  if (!valid_hex_color (hex))
    {
      fprintf (stderr, "swm: px: invalid color '%s', using black\n",
               hex ? hex : "(null)");
      hex = "#000000";
      for (i = 0; i < n_colors; i++)
        if (strcmp (color_cache[i].hex, hex) == 0)
          return color_cache[i].px;
    }

  XColor c;
  c.red = (unsigned short)((hex_val (hex[1]) * 16 + hex_val (hex[2])) * 256);
  c.green
      = (unsigned short)((hex_val (hex[3]) * 16 + hex_val (hex[4])) * 256);
  c.blue
      = (unsigned short)((hex_val (hex[5]) * 16 + hex_val (hex[6])) * 256);
  c.flags = DoRed | DoGreen | DoBlue;
  XAllocColor (dpy, cmap, &c);

  if (n_colors < MAX_COLORS)
    {
      memcpy (color_cache[n_colors].hex, hex, 7);
      color_cache[n_colors].hex[7] = '\0';
      color_cache[n_colors].px = c.pixel;
      n_colors++;
    }
  return c.pixel;
}

static int
set_contains (const Window *s, int n, Window w)
{
  for (int i = 0; i < n; i++)
    if (s[i] == w)
      return 1;
  return 0;
}

static void
set_add (Window *s, int *n, Window w)
{
  if (*n < MAX_SET && !set_contains (s, *n, w))
    s[(*n)++] = w;
}

static void
set_remove (Window *s, int *n, Window w)
{
  for (int i = 0; i < *n; i++)
    if (s[i] == w)
      {
        s[i] = s[--(*n)];
        return;
      }
}

static int
managed_find (Window wid)
{
  for (int i = 0; i < n_managed; i++)
    if (managed[i].wid == wid)
      return i;
  return -1;
}

static void
managed_add (Window wid, int ws)
{
  if (n_managed < MAX_MANAGED)
    {
      managed[n_managed].wid = wid;
      managed[n_managed].ws = ws;
      n_managed++;
    }
}

static void
managed_del (Window wid)
{
  int i = managed_find (wid);
  if (i >= 0)
    managed[i] = managed[--n_managed];
}

static void
managed_set_ws (Window wid, int ws)
{
  int i = managed_find (wid);
  if (i >= 0)
    managed[i].ws = ws;
}

static int
tile_has (Node *t, Window w)
{
  for (int i = 0; i < t->tile.nwindows; i++)
    if (t->tile.windows[i] == w)
      return 1;
  return 0;
}

static void
tile_add (Node *t, Window w)
{
  if (t->tile.nwindows >= t->tile.win_cap)
    {
      int new_cap = t->tile.win_cap ? t->tile.win_cap * 2 : 4;
      Window *tmp
          = realloc (t->tile.windows, (size_t)new_cap * sizeof (Window));
      if (!tmp)
        {
          fprintf (stderr, "swm: tile_add: realloc failed\n");
          return;
        }
      t->tile.windows = tmp;
      t->tile.win_cap = new_cap;
    }
  t->tile.windows[t->tile.nwindows++] = w;
}

static void
tile_remove (Node *t, Window w)
{
  for (int i = 0; i < t->tile.nwindows; i++)
    {
      if (t->tile.windows[i] == w)
        {
          memmove (&t->tile.windows[i], &t->tile.windows[i + 1],
                   (size_t)(t->tile.nwindows - i - 1) * sizeof (Window));
          t->tile.nwindows--;
          return;
        }
    }
}

static int
tile_index (Node *t, Window w)
{
  for (int i = 0; i < t->tile.nwindows; i++)
    if (t->tile.windows[i] == w)
      return i;
  return -1;
}

static void
tile_clamp_tab (Node *t)
{
  if (t->tile.active_tab >= t->tile.nwindows)
    t->tile.active_tab = t->tile.nwindows > 0 ? t->tile.nwindows - 1 : 0;
}

static void
tile_client_area (Node *t, int *cx, int *cy, int *cw, int *ch)
{
  int b = cfg.border_width, g = cfg.border_gap;
  *cx = t->x + g + b;
  *cw = t->w - 2 * (g + b);
  if (*cw < 1)
    *cw = 1;
  *ch = t->h - cfg.tab_bar_height - 2 * (g + b);
  if (*ch < 1)
    *ch = 1;
  if (cfg.statusbar_pos)
    *cy = t->y + cfg.tab_bar_height + g + b;
  else
    *cy = t->y + g + b;
}

static Node *
node_new_tile (int x, int y, int w, int h)
{
  Node *n = calloc (1, sizeof (Node));
  if (!n)
    {
      fprintf (stderr, "swm: node_new_tile: alloc failed\n");
      return NULL;
    }
  n->type = NODE_TILE;
  n->x = x;
  n->y = y;
  n->w = w;
  n->h = h;
  n->tile.win_cap = 4;
  n->tile.windows = calloc ((size_t)n->tile.win_cap, sizeof (Window));
  if (!n->tile.windows)
    {
      fprintf (stderr, "swm: node_new_tile: alloc windows failed\n");
      free (n);
      return NULL;
    }
  n->tile.tab_bar_win = None;
  n->tile.frame_win = None;
  return n;
}

static Node *
node_new_split (int horizontal, double ratio)
{
  Node *n = calloc (1, sizeof (Node));
  if (!n)
    {
      fprintf (stderr, "swm: node_new_split: alloc failed\n");
      return NULL;
    }
  n->type = NODE_SPLIT;
  n->split.horizontal = horizontal;
  n->split.ratio = ratio;
  return n;
}

static void
node_free_tree (Node *n)
{
  if (!n)
    return;
  if (n->type == NODE_SPLIT)
    {
      node_free_tree (n->split.children[0]);
      node_free_tree (n->split.children[1]);
    }
  else if (n->type == NODE_TILE)
    {
      free (n->tile.windows);
    }
  free (n);
}

static int
collect_tiles (Node *node, Node **buf, int max)
{
  if (!node || max <= 0)
    return 0;
  if (node->type == NODE_TILE)
    {
      buf[0] = node;
      return 1;
    }
  int n = collect_tiles (node->split.children[0], buf, max);
  n += collect_tiles (node->split.children[1], buf + n, max - n);
  return n;
}

static void
ws_invalidate_tiles (Workspace *w)
{
  w->tiles_dirty = 1;
}

static int
ws_get_tiles (Workspace *w, Node ***out)
{
  if (w->tiles_dirty)
    {
      w->n_cached_tiles = collect_tiles (w->root, w->cached_tiles, MAX_TILES);
      w->tiles_dirty = 0;
    }
  *out = w->cached_tiles;
  return w->n_cached_tiles;
}

static Node *
ws_find_tile (Workspace *w, Window wid)
{
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    for (int j = 0; j < tiles[i]->tile.nwindows; j++)
      if (tiles[i]->tile.windows[j] == wid)
        return tiles[i];
  return NULL;
}

static int
ws_has_windows (Workspace *w)
{
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    if (tiles[i]->tile.nwindows > 0)
      return 1;
  return 0;
}

static void
ws_recalc (Workspace *ws, Node *node)
{
  if (!node)
    {
      node = ws->root;
      node->x = 0;
      node->y = ws->tile_y;
      node->w = ws->sw;
      node->h = ws->tile_h;
    }
  if (node->type == NODE_TILE)
    return;
  Node *c0 = node->split.children[0], *c1 = node->split.children[1];
  if (node->split.horizontal)
    {
      int w0 = (int)(node->w * node->split.ratio);
      c0->x = node->x;
      c0->y = node->y;
      c0->w = w0;
      c0->h = node->h;
      c1->x = node->x + w0;
      c1->y = node->y;
      c1->w = node->w - w0;
      c1->h = node->h;
    }
  else
    {
      int h0 = (int)(node->h * node->split.ratio);
      c0->x = node->x;
      c0->y = node->y;
      c0->w = node->w;
      c0->h = h0;
      c1->x = node->x;
      c1->y = node->y + h0;
      c1->w = node->w;
      c1->h = node->h - h0;
    }
  ws_recalc (ws, c0);
  ws_recalc (ws, c1);
}

static void
ws_replace (Workspace *ws, Node *old, Node *rep)
{
  rep->parent = old->parent;
  if (!old->parent)
    {
      ws->root = rep;
    }
  else
    {
      Node *p = old->parent;
      p->split.children[(p->split.children[0] == old) ? 0 : 1] = rep;
    }
}

static double
mono_time (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int
xerr_handler (Display *d, XErrorEvent *e)
{
  (void)d;
  (void)e;
  return 0;
}

static int
xerr_detect (Display *d, XErrorEvent *e)
{
  (void)d;
  (void)e;
  wm_detected = 1;
  return 0;
}

static void
get_wm_name (Window wid, char *buf, int len)
{
  Atom type;
  int fmt;
  unsigned long ni, after;
  unsigned char *data = NULL;

  if (XGetWindowProperty (dpy, wid, a_net_wm_name, 0, 256, False, a_utf8,
                          &type, &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      snprintf (buf, len, "%.*s", (int)ni, (char *)data);
      XFree (data);
      return;
    }
  if (data)
    {
      XFree (data);
      data = NULL;
    }

  if (XGetWindowProperty (dpy, wid, XA_WM_NAME, 0, 256, False, XA_STRING,
                          &type, &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      snprintf (buf, len, "%.*s", (int)ni, (char *)data);
      XFree (data);
      return;
    }
  if (data)
    XFree (data);
  snprintf (buf, len, "?");
}

static Workspace *
ws (void)
{
  return &workspaces[cur_ws];
}

static void ensure_frame (Node *tile);
static void destroy_frame (Node *tile);
static void ensure_tab_bar (Node *tile);
static void destroy_tab_bar (Node *tile);
static void draw_tab_bar (Node *tile);
static void arrange_tile (Node *tile);
static void arrange_workspace (Workspace *w);
static void raise_floats (void);
static void hide_workspace (Workspace *w);
static void focus_tile (Node *tile);
static void focus_set_input (Node *tile);
static void send_configure_notify (Window wid);
static void bar_draw (void);
static void bar_destroy (void);
static void timebar_init (void);
static void timebar_draw (void);
static void timebar_destroy (void);
static void unmanage_window (Window wid);
static void cmd_init (void);
static void cmd_cleanup (void);
static void cmd_poll (fd_set *fds);
static void action_switch_workspace (int n);

static void
init_ewmh (void)
{
  Atom supporting = XInternAtom (dpy, "_NET_SUPPORTING_WM_CHECK", False);

  ewmh_check_win = XCreateSimpleWindow (dpy, root_win, 0, 0, 1, 1, 0, 0, 0);

  XChangeProperty (dpy, root_win, supporting, XA_WINDOW, 32, PropModeReplace,
                   (unsigned char *)&ewmh_check_win, 1);
  XChangeProperty (dpy, ewmh_check_win, supporting, XA_WINDOW, 32,
                   PropModeReplace, (unsigned char *)&ewmh_check_win, 1);

  XChangeProperty (dpy, ewmh_check_win, a_net_wm_name, a_utf8, 8,
                   PropModeReplace, (unsigned char *)"SWM", 3);
  XChangeProperty (dpy, root_win, a_net_wm_name, a_utf8, 8, PropModeReplace,
                   (unsigned char *)"SWM", 3);

  Atom net_supported = XInternAtom (dpy, "_NET_SUPPORTED", False);
  Atom supported[] = {
    a_net_wm_state,
    a_net_wm_state_fullscreen,
    a_net_wm_name,
    supporting,
  };
  XChangeProperty (dpy, root_win, net_supported, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)supported,
                   (int)(sizeof (supported) / sizeof (supported[0])));

  XFlush (dpy);
}

static void
cleanup_ewmh (void)
{
  if (ewmh_check_win != None)
    {
      XDestroyWindow (dpy, ewmh_check_win);
      ewmh_check_win = None;
    }
}

static void
fullscreen_enter (Window wid)
{
  if (ws ()->fullscreen_win != None)
    return;
  ws ()->fullscreen_win = wid;

  XChangeProperty (dpy, wid, a_net_wm_state, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)&a_net_wm_state_fullscreen, 1);

  if (status_bar_win != None)
    XUnmapWindow (dpy, status_bar_win);
  if (timebar_win != None)
    XUnmapWindow (dpy, timebar_win);
  Node **tiles;
  int n = ws_get_tiles (ws (), &tiles);
  for (int i = 0; i < n; i++)
    {
      if (tiles[i]->tile.tab_bar_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.tab_bar_win);
      if (tiles[i]->tile.frame_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.frame_win);

      for (int j = 0; j < tiles[i]->tile.nwindows; j++)
        if (tiles[i]->tile.windows[j] != wid)
          XUnmapWindow (dpy, tiles[i]->tile.windows[j]);
    }

  XMoveResizeWindow (dpy, wid, 0, 0, (unsigned)sw, (unsigned)sh);
  XRaiseWindow (dpy, wid);
  XSetInputFocus (dpy, wid, RevertToParent, CurrentTime);
  XFlush (dpy);
}

static void
fullscreen_exit (Window wid)
{
  if (ws ()->fullscreen_win == None || ws ()->fullscreen_win != wid)
    return;
  ws ()->fullscreen_win = None;

  XChangeProperty (dpy, wid, a_net_wm_state, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)NULL, 0);

  if (status_bar_win != None)
    {
      XMapWindow (dpy, status_bar_win);
      XRaiseWindow (dpy, status_bar_win);
    }
  if (timebar_win != None)
    {
      XMapWindow (dpy, timebar_win);
      XRaiseWindow (dpy, timebar_win);
    }

  arrange_workspace (ws ());
  focus_set_input (ws ()->active_tile);
  bar_draw ();
  timebar_draw ();
}

static void
fullscreen_toggle (Window wid)
{
  if (ws ()->fullscreen_win == wid)
    fullscreen_exit (wid);
  else
    fullscreen_enter (wid);
}

static void
ensure_frame (Node *tile)
{
  int g = cfg.border_gap;
  int fx = tile->x + g, fy = tile->y + g;
  int fw = tile->w - 2 * g;
  if (fw < 1)
    fw = 1;
  int fh = tile->h - 2 * g;
  if (fh < 1)
    fh = 1;
  int is_active = (tile == ws ()->active_tile);
  const char *bg = is_active ? cfg.col_border_active : cfg.col_border_inactive;

  if (tile->tile.frame_win == None)
    {
      XSetWindowAttributes swa;
      swa.background_pixel = px (bg);
      swa.override_redirect = True;
      swa.event_mask = 0;
      Window w = XCreateWindow (
          dpy, root_win, fx, fy, (unsigned)fw, (unsigned)fh, 0, depth,
          InputOutput, CopyFromParent,
          CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
      tile->tile.frame_win = w;
      set_add (frame_wins, &n_frame_wins, w);
      XLowerWindow (dpy, w);
      XMapWindow (dpy, w);
    }
  else
    {
      Window w = tile->tile.frame_win;
      XMoveResizeWindow (dpy, w, fx, fy, (unsigned)fw, (unsigned)fh);
      XSetWindowBackground (dpy, w, px (bg));
      XClearWindow (dpy, w);
      XLowerWindow (dpy, w);
      XMapWindow (dpy, w);
    }
}

static void
destroy_frame (Node *tile)
{
  if (tile->tile.frame_win != None)
    {
      Window w = tile->tile.frame_win;
      XUnmapWindow (dpy, w);
      XDestroyWindow (dpy, w);
      set_remove (frame_wins, &n_frame_wins, w);
      tile->tile.frame_win = None;
    }
}

static void
bar_init (void)
{
  int by = cfg.statusbar_pos ? 0 : (sh - cfg.statusbar_height);
  XSetWindowAttributes swa;
  swa.background_pixel = px (cfg.col_statusbar_bg);
  swa.override_redirect = True;
  swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask;
  status_bar_win = XCreateWindow (
      dpy, root_win, 0, by, (unsigned)sw, cfg.statusbar_height, 0, depth,
      InputOutput, CopyFromParent,
      CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XRaiseWindow (dpy, status_bar_win);
  XMapWindow (dpy, status_bar_win);
  bar_draw ();
}

static void
bar_draw (void)
{
  if (status_bar_win == None)
    return;
  int w = sw, h = cfg.statusbar_height;

  Pixmap pm = XCreatePixmap (dpy, status_bar_win, (unsigned)w, (unsigned)h,
                             (unsigned)depth);

  XSetForeground (dpy, draw_gc, px (cfg.col_statusbar_bg));
  XFillRectangle (dpy, pm, draw_gc, 0, 0, (unsigned)w, (unsigned)h);

  int pad = 6, ws_w = 22, ws_gap = 3, text_y = h / 2 + 4;

  int x = pad;
  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      int is_cur = (i == cur_ws);
      int has_w = ws_has_windows (&workspaces[i]);
      const char *bg_col, *fg_col;
      if (is_cur)
        {
          bg_col = cfg.col_statusbar_ws_active;
          fg_col = cfg.col_statusbar_ws_fg_act;
        }
      else if (has_w)
        {
          bg_col = cfg.col_statusbar_ws_occupied;
          fg_col = cfg.col_statusbar_ws_fg_act;
        }
      else
        {
          bg_col = cfg.col_statusbar_ws_inactive;
          fg_col = cfg.col_statusbar_ws_fg_inact;
        }

      XSetForeground (dpy, draw_gc, px (bg_col));
      XFillRectangle (dpy, pm, draw_gc, x, 2, (unsigned)ws_w,
                      (unsigned)(h - 4));

      XSetForeground (dpy, draw_gc, px (fg_col));
      XSetFont (dpy, draw_gc, font->fid);
      char label[2] = { (char)('1' + i), '\0' };
      XDrawString (dpy, pm, draw_gc, x + ws_w / 2 - 3, text_y, label, 1);

      x += ws_w + ws_gap;
    }

  time_t now = time (NULL);
  struct tm *tm = localtime (&now);
  char status[64];
  strftime (status, sizeof (status), "%a %b %d  %H:%M", tm);

  int text_w = (int)strlen (status) * 7;
  int tx = w - text_w - pad;
  XSetForeground (dpy, draw_gc, px (cfg.col_statusbar_fg));
  XSetFont (dpy, draw_gc, font->fid);
  XDrawString (dpy, pm, draw_gc, tx, text_y, status, (int)strlen (status));

  XCopyArea (dpy, pm, status_bar_win, draw_gc, 0, 0, (unsigned)w, (unsigned)h,
             0, 0);
  XFreePixmap (dpy, pm);
  XFlush (dpy);
}

static void
bar_handle_click (XEvent *ev)
{
  int pad = 6, ws_w = 22, ws_gap = 3;
  int cx = ev->xbutton.x;
  int x = pad;
  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      if (cx >= x && cx < x + ws_w)
        {

          action_switch_workspace (i);
          return;
        }
      x += ws_w + ws_gap;
    }
}

static void
bar_destroy (void)
{
  if (status_bar_win != None)
    {
      XUnmapWindow (dpy, status_bar_win);
      XDestroyWindow (dpy, status_bar_win);
      status_bar_win = None;
    }
}

static void
timebar_init (void)
{
  int by;
  if (cfg.timebar_pos)
    by = cfg.statusbar_pos ? cfg.statusbar_height : 0;
  else
    by = cfg.statusbar_pos ? sh - cfg.timebar_height
                           : sh - cfg.statusbar_height - cfg.timebar_height;
  XSetWindowAttributes swa;
  swa.background_pixel = px (cfg.col_timebar_bg);
  swa.override_redirect = True;
  swa.event_mask = ExposureMask;
  timebar_win
      = XCreateWindow (dpy, root_win, 0, by, (unsigned)sw, cfg.timebar_height,
                       0, depth, InputOutput, CopyFromParent,
                       CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XRaiseWindow (dpy, timebar_win);
  XMapWindow (dpy, timebar_win);
  timebar_draw ();
}

static void
timebar_draw (void)
{
  if (timebar_win == None)
    return;
  int w = sw, h = cfg.timebar_height;

  Pixmap pm = XCreatePixmap (dpy, timebar_win, (unsigned)w, (unsigned)h,
                             (unsigned)depth);

  XSetForeground (dpy, draw_gc, px (cfg.col_timebar_bg));
  XFillRectangle (dpy, pm, draw_gc, 0, 0, (unsigned)w, (unsigned)h);

  time_t now_t = time (NULL);
  struct tm *tm = localtime (&now_t);
  int hours24 = tm->tm_hour;
  int minutes = tm->tm_min;
  int seconds = tm->tm_sec;
  //int hours12 = (hours24 == 0) ? 12 : (hours24 > 12 ? hours24 - 12 : hours24);

  int total_secs_in_hour = minutes * 60 + seconds;
  int hex_segment = total_secs_in_hour / 225;
  if (hex_segment > 15)
    hex_segment = 15;
  double progress_in_seg = (total_secs_in_hour % 225) / 225.0;

  double hour_pct = ((hours24 * 60) + minutes) / (24.0 * 60.0);
  double hex_pct = total_secs_in_hour / 3600.0;
  double seg_pct = progress_in_seg;

  //const char *hex_chars = "0123456789ABCDEF";

  //int label_w = 16;
  int gap = 2;
  int bar_y = 1;
  int bar_h = h - 2;
  if (bar_h < 1)
    bar_h = 1;

  int zone_w = w / 3;

  {
    int x0 = 0;
//    int fill_x = x0 + label_w + gap;
//    int fill_w = zone_w - label_w - gap - gap;
    int fill_x = x0 + gap;
    int fill_w = zone_w - gap;
    if (fill_w < 1)
      fill_w = 1;

    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_track));
    XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fill_w,
                    (unsigned)bar_h);

    int fw = (int)(fill_w * hour_pct);
    if (fw > 0)
      {
        XSetForeground (dpy, draw_gc, px (cfg.col_timebar_hour));
        XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fw,
                        (unsigned)bar_h);
      }

//    char lbl[12];
//    snprintf (lbl, sizeof (lbl), "%d", hours12);
//    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_label));
//    XSetFont (dpy, draw_gc, font->fid);
//    XDrawString (dpy, pm, draw_gc, x0 + 2, bar_y + bar_h - 1, lbl,
                 //(int)strlen (lbl));
  }

  {
    int x0 = zone_w;
//    int fill_x = x0 + label_w + gap;
//    int fill_w = zone_w - label_w - gap - gap;
    int fill_x = x0 + gap;
    int fill_w = zone_w - gap;
    if (fill_w < 1)
      fill_w = 1;

    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_track));
    XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fill_w,
                    (unsigned)bar_h);

    int fw = (int)(fill_w * hex_pct);
    if (fw > 0)
      {
        XSetForeground (dpy, draw_gc, px (cfg.col_timebar_hex));
        XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fw,
                        (unsigned)bar_h);
      }

//    char lbl[2] = { hex_chars[hex_segment], '\0' };
//    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_label));
//    XSetFont (dpy, draw_gc, font->fid);
//    XDrawString (dpy, pm, draw_gc, x0 + 2, bar_y + bar_h - 1, lbl, 1);
  }

  {
    int x0 = zone_w * 2;
    int fill_w = w - x0 - gap;
    if (fill_w < 1)
      fill_w = 1;

    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_track));
    XFillRectangle (dpy, pm, draw_gc, x0 + gap, bar_y, (unsigned)fill_w,
                    (unsigned)bar_h);

    int fw = (int)(fill_w * seg_pct);
    if (fw > 0)
      {
        XSetForeground (dpy, draw_gc, px (cfg.col_timebar_seg));
        XFillRectangle (dpy, pm, draw_gc, x0 + gap, bar_y, (unsigned)fw,
                        (unsigned)bar_h);
      }
  }

  XCopyArea (dpy, pm, timebar_win, draw_gc, 0, 0, (unsigned)w, (unsigned)h, 0,
             0);
  XFreePixmap (dpy, pm);
  XFlush (dpy);
}

static void
timebar_destroy (void)
{
  if (timebar_win != None)
    {
      XUnmapWindow (dpy, timebar_win);
      XDestroyWindow (dpy, timebar_win);
      timebar_win = None;
    }
}

static void
ensure_tab_bar (Node *tile)
{
  int g = cfg.border_gap, b = cfg.border_width;
  int bx = tile->x + g + b;
  int bw = tile->w - 2 * (g + b);
  if (bw < 1)
    bw = 1;
  int bh = cfg.tab_bar_height - b;
  if (bh < 1)
    bh = 1;
  int by;
  if (cfg.statusbar_pos)
    by = tile->y + g + b;
  else
    by = tile->y + tile->h - g - cfg.tab_bar_height;
  int is_active = (tile == ws ()->active_tile);
  const char *bar_bg = is_active ? cfg.col_border_active : cfg.col_tab_bar_bg;

  if (tile->tile.tab_bar_win == None)
    {
      XSetWindowAttributes swa;
      swa.background_pixel = px (bar_bg);
      swa.override_redirect = True;
      swa.event_mask = ExposureMask | ButtonPressMask;
      Window w = XCreateWindow (
          dpy, root_win, bx, by, (unsigned)bw, (unsigned)bh, 0, depth,
          InputOutput, CopyFromParent,
          CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
      tile->tile.tab_bar_win = w;
      set_add (tab_bars, &n_tab_bars, w);
      XMapWindow (dpy, w);
    }
  else
    {
      Window w = tile->tile.tab_bar_win;
      XMoveResizeWindow (dpy, w, bx, by, (unsigned)bw, (unsigned)bh);
      XSetWindowBackground (dpy, w, px (bar_bg));
      XMapWindow (dpy, w);
    }
}

static void
destroy_tab_bar (Node *tile)
{
  if (tile->tile.tab_bar_win != None)
    {
      Window w = tile->tile.tab_bar_win;
      XUnmapWindow (dpy, w);
      XDestroyWindow (dpy, w);
      set_remove (tab_bars, &n_tab_bars, w);
      tile->tile.tab_bar_win = None;
    }
}

static void
draw_tab_bar (Node *tile)
{
  if (tile->tile.tab_bar_win == None)
    return;
  Window win = tile->tile.tab_bar_win;
  XWindowAttributes wa;
  XGetWindowAttributes (dpy, win, &wa);
  int w = wa.width, h = wa.height;
  if (w < 1 || h < 1)
    return;

  int is_active = (tile == ws ()->active_tile);
  const char *bar_bg = is_active ? cfg.col_border_active : cfg.col_tab_bar_bg;

  Pixmap pm
      = XCreatePixmap (dpy, win, (unsigned)w, (unsigned)h, (unsigned)depth);

  XSetForeground (dpy, draw_gc, px (bar_bg));
  XFillRectangle (dpy, pm, draw_gc, 0, 0, (unsigned)w, (unsigned)h);

  int n = tile->tile.nwindows;
  if (n == 0)
    {
      if (is_active)
        {
          XSetForeground (dpy, draw_gc, px ("#FFFFFF"));
          //XSetFont (dpy, draw_gc, font->fid);
          //XDrawString (dpy, pm, draw_gc, 6, h / 2 + 4, "...", 3);
        }
    }
  else
    {
      int tab_w = w / n;
      if (tab_w < 1)
        tab_w = 1;
      for (int i = 0; i < n; i++)
        {
          int is_tab_act = (i == tile->tile.active_tab);
          const char *bg, *fg;
          if (is_tab_act)
            {
              bg = is_active ? cfg.col_tab_active_bg
                             : cfg.col_tab_active_bg_dim;
              fg = is_active ? cfg.col_tab_active_fg
                             : cfg.col_tab_active_fg_dim;
            }
          else
            {
              bg = cfg.col_tab_inactive_bg;
              fg = cfg.col_tab_inactive_fg;
            }
          int x0 = i * tab_w;
          int tw = (i < n - 1) ? tab_w : (w - x0);

          XSetForeground (dpy, draw_gc, px (bg));
          XFillRectangle (dpy, pm, draw_gc, x0, 0, (unsigned)tw, (unsigned)h);

          if (i < n - 1)
            {
              XSetForeground (dpy, draw_gc, px (bar_bg));
              XFillRectangle (dpy, pm, draw_gc, x0 + tw - 1, 0, 1,
                              (unsigned)h);
            }

//          char title[128];
//          get_wm_name (tile->tile.windows[i], title, sizeof (title));
//          int tlen = (int)strlen (title);
//          if (tlen > 20)
//            {
//              title[18] = '.';
//              title[19] = '.';
//              title[20] = '\0';
//              tlen = 20;
//            }
//
          XSetForeground (dpy, draw_gc, px (fg));
//          XSetFont (dpy, draw_gc, font->fid);
//          XDrawString (dpy, pm, draw_gc, x0 + 6, h / 2 + 4, title, tlen);
        }
    }

  XCopyArea (dpy, pm, win, draw_gc, 0, 0, (unsigned)w, (unsigned)h, 0, 0);
  XFreePixmap (dpy, pm);
  XFlush (dpy);
}
 static void
raise_floats (void)
{
  for (int i = 0; i < n_float_wins; i++)
    XRaiseWindow (dpy, float_wins[i]);
}

static void
arrange_tile (Node *tile)
{
  ensure_frame (tile);
  ensure_tab_bar (tile);

  int cx, cy, cw, ch;
  tile_client_area (tile, &cx, &cy, &cw, &ch);

  for (int i = 0; i < tile->tile.nwindows; i++)
    {
      Window w = tile->tile.windows[i];
      if (i == tile->tile.active_tab)
        {
          XMoveResizeWindow (dpy, w, cx, cy, (unsigned)cw, (unsigned)ch);
          XRaiseWindow (dpy, w);
          XMapWindow (dpy, w);
        }
      else
        {
          XUnmapWindow (dpy, w);
        }
    }
  draw_tab_bar (tile);
  raise_floats ();
}

static void
arrange_workspace (Workspace *w)
{
  ws_recalc (w, NULL);
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    arrange_tile (tiles[i]);
}

static void
hide_workspace (Workspace *w)
{
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    {
      for (int j = 0; j < tiles[i]->tile.nwindows; j++)
        XUnmapWindow (dpy, tiles[i]->tile.windows[j]);
      if (tiles[i]->tile.tab_bar_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.tab_bar_win);
      if (tiles[i]->tile.frame_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.frame_win);
    }
}

static void
focus_tile (Node *tile)
{
  focus_set_input(tile);
  Node **tiles;
  int n = ws_get_tiles (ws (), &tiles);
  for (int i = 0; i < n; i++)
    {
      ensure_frame (tiles[i]);
      ensure_tab_bar (tiles[i]);
      draw_tab_bar (tiles[i]);
      raise_floats ();
    }
}

static void
focus_set_input (Node *tile)
{
  ws ()->active_tile = tile;
  if (tile->tile.nwindows > 0)
    {
      Window w = tile->tile.windows[tile->tile.active_tab];
      XSetInputFocus (dpy, w, RevertToParent, CurrentTime);
      XRaiseWindow (dpy, w);
    }
  raise_floats ();
}

static Node *
find_adjacent (const char *dir)
{
  Node *cur = ws ()->active_tile;
  int cxc = cur->x + cur->w / 2, cyc = cur->y + cur->h / 2;
  Node *best = NULL;
  long best_dist = 0x7FFFFFFF;

  Node **tiles;
  int n = ws_get_tiles (ws (), &tiles);
  for (int i = 0; i < n; i++)
    {
      if (tiles[i] == cur)
        continue;
      int tx = tiles[i]->x + tiles[i]->w / 2,
          ty = tiles[i]->y + tiles[i]->h / 2;
      int dx = tx - cxc, dy = ty - cyc;
      int ok = 0;
      if (strcmp (dir, "left") == 0 && dx < -5)
        ok = 1;
      else if (strcmp (dir, "right") == 0 && dx > 5)
        ok = 1;
      else if (strcmp (dir, "up") == 0 && dy < -5)
        ok = 1;
      else if (strcmp (dir, "down") == 0 && dy > 5)
        ok = 1;
      if (ok)
        {
          long dist = (long)dx * dx + (long)dy * dy;
          if (dist < best_dist)
            {
              best_dist = dist;
              best = tiles[i];
            }
        }
    }
  return best;
}

static void
spawn (const char *cmd)
{
  pid_t pid = fork ();
  if (pid < 0)
    {
      fprintf (stderr, "swm: fork failed: %s\n", strerror (errno));
      return;
    }
  if (pid == 0)
    {
      setsid ();

      if (dpy)
        close (ConnectionNumber (dpy));
      execl ("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit (1);
    }
}

static int
should_float (Window wid)
{
  Atom type;
  int fmt;
  unsigned long ni, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty (dpy, wid, a_wm_type, 0, 32, False, XA_ATOM, &type,
                          &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      Atom *atoms = (Atom *)data;
      for (unsigned long i = 0; i < ni; i++)
        {
          if (atoms[i] == a_wm_type_splash || atoms[i] == a_wm_type_tooltip
              || atoms[i] == a_wm_type_notification
              || atoms[i] == a_wm_type_popup_menu || atoms[i] == a_wm_type_dock
              || atoms[i] == a_wm_type_dialog)
            {
              XFree (data);
              return 1;
            }
        }
      XFree (data);
    }
  else if (data)
    {
      XFree (data);
    }

  Window transient_for = None;
  if (XGetTransientForHint (dpy, wid, &transient_for) && transient_for != None)
    return 1;

  return 0;
}

static void
manage_window (Window wid)
{
  if (managed_find (wid) >= 0)
    return;
  if (set_contains (tab_bars, n_tab_bars, wid))
    return;
  if (set_contains (frame_wins, n_frame_wins, wid))
    return;
  if (wid == timebar_win || wid == status_bar_win)
    return;

  XWindowAttributes wa;
  if (!XGetWindowAttributes (dpy, wid, &wa))
    return;
  if (wa.override_redirect)
    return;

  if (should_float (wid))
    {
      if (wa.x <= 0 && wa.y <= 0 && wa.width < sw && wa.height < sh)
        {
          int nx = (sw - wa.width) / 2;
          int ny = (sh - wa.height) / 2;
          if (nx < 0)
            nx = 0;
          if (ny < 0)
            ny = 0;
          XMoveWindow (dpy, wid, nx, ny);
        }
      XSelectInput (dpy, wid, StructureNotifyMask);
      XMapWindow (dpy, wid);
      XRaiseWindow (dpy, wid);
      set_add (float_wins, &n_float_wins, wid);
      XSetInputFocus (dpy, wid, RevertToParent, CurrentTime);
      XFlush (dpy);
      return;
    }

  XSetWindowAttributes swa;
  swa.event_mask = StructureNotifyMask | PropertyChangeMask;
  XSetWindowBorderWidth(dpy, wid, 0);
  XChangeWindowAttributes (dpy, wid, CWEventMask, &swa);

  Node *tile = ws ()->active_tile;
  tile_add (tile, wid);
  tile->tile.prev_active_tab = tile->tile.active_tab;
  tile->tile.active_tab = tile->tile.nwindows - 1;
  managed_add (wid, cur_ws);

  arrange_tile (tile);
  send_configure_notify (wid);
  focus_set_input (tile);
  bar_draw ();

  {
    Atom type;
    int fmt;
    unsigned long ni, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty (dpy, wid, a_net_wm_state, 0, 32, False, XA_ATOM,
                            &type, &fmt, &ni, &after, &data)
            == Success
        && data && ni > 0)
      {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < ni; i++)
          {
            if (atoms[i] == a_net_wm_state_fullscreen)
              {
                XFree (data);
                fullscreen_enter (wid);
                return;
              }
          }
        XFree (data);
      }
    else if (data)
      {
        XFree (data);
      }
  }
}

static void
unmanage_window (Window wid)
{
  int idx = managed_find (wid);
  if (idx < 0)
    return;
  int ws_idx = managed[idx].ws;
  managed_del (wid);

  if (workspaces[ws_idx].fullscreen_win == wid)
    {
      workspaces[ws_idx].fullscreen_win = None;
      if (status_bar_win != None)
        {
          XMapWindow (dpy, status_bar_win);
          XRaiseWindow (dpy, status_bar_win);
        }
      if (timebar_win != None)
        {
          XMapWindow (dpy, timebar_win);
          XRaiseWindow (dpy, timebar_win);
        }
    }

  Workspace *w = &workspaces[ws_idx];
  Node *tile = ws_find_tile (w, wid);
  if (tile)
    {
      int closed_idx = tile_index (tile, wid);
      int was_active = (closed_idx == tile->tile.active_tab);
      tile_remove (tile, wid);

      if (was_active && tile->tile.nwindows > 0)
        {
          int prev = tile->tile.prev_active_tab;
          if (prev > closed_idx)
            prev--;
          if (prev < 0)
            prev = 0;
          if (prev >= tile->tile.nwindows)
            prev = tile->tile.nwindows - 1;
          tile->tile.active_tab = prev;
        }
      else
        {
          tile_clamp_tab (tile);
        }
      if (ws_idx == cur_ws)
        {
        arrange_tile (tile);
        if (tile->tile.nwindows > 0)
          focus_set_input (tile);
        }
    }
  bar_draw ();
}

static void
action_switch_workspace (int n)
{
  if (n == cur_ws)
    return;
  if (ws ()->fullscreen_win != None)
    fullscreen_exit (ws ()->fullscreen_win);
  hide_workspace (ws ());
  cur_ws = n;
  arrange_workspace (ws ());
  focus_set_input (ws ()->active_tile);
  bar_draw ();
}

static void
action_next_workspace (void)
{
  action_switch_workspace ((cur_ws + 1) % NUM_WORKSPACES);
}

static void
action_prev_workspace (void)
{
  action_switch_workspace ((cur_ws + NUM_WORKSPACES - 1) % NUM_WORKSPACES);
}

static void
action_send_to_workspace (int n)
{
  if (n == cur_ws)
    return;
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows == 0)
      focus_set_input(tile);
  Window wid = tile->tile.windows[tile->tile.active_tab];

  tile_remove (tile, wid);
  tile_clamp_tab (tile);

  Workspace *tw = &workspaces[n];
  tile_add (tw->active_tile, wid);
  tw->active_tile->tile.active_tab = tw->active_tile->tile.nwindows - 1;
  managed_set_ws (wid, n);

  XUnmapWindow (dpy, wid);
  arrange_tile (tile);
  bar_draw ();
}

static void
action_close_window (void)
{
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows == 0)
    return;
  Window wid = tile->tile.windows[tile->tile.active_tab];

  Atom type;
  int fmt;
  unsigned long ni, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty (dpy, wid, a_wm_protocols, 0, 32, False, XA_ATOM,
                          &type, &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      Atom *atoms = (Atom *)data;
      int found = 0;
      for (unsigned long i = 0; i < ni; i++)
        if (atoms[i] == a_wm_delete)
          {
            found = 1;
            break;
          }
      XFree (data);
      if (found)
        {
          XEvent ev;
          memset (&ev, 0, sizeof (ev));
          ev.xclient.type = ClientMessage;
          ev.xclient.window = wid;
          ev.xclient.message_type = a_wm_protocols;
          ev.xclient.format = 32;
          ev.xclient.data.l[0] = (long)a_wm_delete;
          ev.xclient.data.l[1] = CurrentTime;
          XSendEvent (dpy, wid, False, 0, &ev);
          XFlush (dpy);
          return;
        }
    }
  else if (data)
    {
      XFree (data);
    }

  XDestroyWindow (dpy, wid);
}

static void
action_split (int horizontal, int move_window)
{
  Node *tile = ws ()->active_tile;
  Node *sp = node_new_split (horizontal, 0.5);
  if (!sp)
    {
      fprintf (stderr, "swm: action_split: alloc split failed\n");
      return;
    }
  sp->x = tile->x;
  sp->y = tile->y;
  sp->w = tile->w;
  sp->h = tile->h;
  Node *sibling = node_new_tile (0, 0, 1, 1);
  if (!sibling)
    {
      fprintf (stderr, "swm: action_split: alloc tile failed\n");
      free (sp);
      return;
    }

  ws_replace (ws (), tile, sp);
  sp->split.children[0] = tile;
  tile->parent = sp;
  sp->split.children[1] = sibling;
  sibling->parent = sp;

  if (move_window && tile->tile.nwindows > 0)
    {
      Window wid = tile->tile.windows[tile->tile.active_tab];
      tile_remove (tile, wid);
      tile_clamp_tab (tile);
      tile_add (sibling, wid);
      sibling->tile.active_tab = 0;
      ws ()->active_tile = sibling;
    }
  else
    {
      ws ()->active_tile = tile;
    }
  ws_invalidate_tiles (ws ());
  arrange_workspace (ws ());
}

static void
action_remove_split (void)
{
  Node *tile = ws ()->active_tile;
  Node *parent = tile->parent;
  if (!parent || parent->type != NODE_SPLIT)
    return;

  int idx = (parent->split.children[0] == tile) ? 0 : 1;
  Node *sibling = parent->split.children[1 - idx];

  Node *stiles[MAX_TILES];
  int sn = collect_tiles (sibling, stiles, MAX_TILES);
  for (int i = 0; i < sn; i++)
    {
      for (int j = 0; j < stiles[i]->tile.nwindows; j++)
        {
          Window wid = stiles[i]->tile.windows[j];
          if (!tile_has (tile, wid))
            tile_add (tile, wid);
          managed_set_ws (wid, cur_ws);
        }
      destroy_tab_bar (stiles[i]);
      destroy_frame (stiles[i]);
    }

  destroy_tab_bar (tile);
  destroy_frame (tile);

  tile->parent = parent->parent;
  if (!parent->parent)
    {
      ws ()->root = tile;
    }
  else
    {
      Node *gp = parent->parent;
      int gi = (gp->split.children[0] == parent) ? 0 : 1;
      gp->split.children[gi] = tile;
    }

  tile_clamp_tab (tile);

  ws ()->active_tile = tile;

  node_free_tree (sibling);
  parent->split.children[0] = NULL;
  parent->split.children[1] = NULL;
  free (parent);

  ws_invalidate_tiles (ws ());
  arrange_workspace (ws ());
}

static void
action_next_tab (void)
{
  if (ws ()->fullscreen_win != None)
    fullscreen_exit (ws ()->fullscreen_win);
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows > 1)
    {
      tile->tile.active_tab
          = (tile->tile.active_tab + 1) % tile->tile.nwindows;
      arrange_tile (tile);
      focus_set_input (tile);
    }
}

static void
action_prev_tab (void)
{
  if (ws ()->fullscreen_win != None)
    fullscreen_exit (ws ()->fullscreen_win);
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows > 1)
    {
      tile->tile.active_tab = (tile->tile.active_tab + tile->tile.nwindows - 1)
                              % tile->tile.nwindows;
      arrange_tile (tile);
      focus_set_input (tile);
    }
}

static void
action_move_tab_forward (void)
{
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows < 2)
    return;
  int i = tile->tile.active_tab;
  int j = (i + 1) % tile->tile.nwindows;
  Window tmp = tile->tile.windows[i];
  tile->tile.windows[i] = tile->tile.windows[j];
  tile->tile.windows[j] = tmp;
  tile->tile.active_tab = j;
  arrange_tile (tile);
}

static void
action_move_tab_backward (void)
{
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows < 2)
    return;
  int i = tile->tile.active_tab;
  int j = (i + tile->tile.nwindows - 1) % tile->tile.nwindows;
  Window tmp = tile->tile.windows[i];
  tile->tile.windows[i] = tile->tile.windows[j];
  tile->tile.windows[j] = tmp;
  tile->tile.active_tab = j;
  arrange_tile (tile);
}

static void
action_focus_direction (const char *dir)
{
  Node *adj = find_adjacent (dir);
  if (adj)
    {
      focus_tile (adj);

    }
}

static void
action_move_window_direction (const char *dir)
{
  Node *src = ws ()->active_tile;
  if (src->tile.nwindows == 0)
    return;
  Node *dst = find_adjacent (dir);
  if (!dst)
    return;

  Window wid = src->tile.windows[src->tile.active_tab];
  tile_remove (src, wid);
  tile_clamp_tab (src);

  tile_add (dst, wid);
  dst->tile.active_tab = dst->tile.nwindows - 1;

  arrange_tile (src);
  arrange_tile (dst);
  ws ()->active_tile = dst;
  focus_tile (dst);

}

static void
action_quit (void)
{
  running = 0;
}

static void
cmd_init (void)
{

  const char *xdg = getenv ("XDG_RUNTIME_DIR");
  if (xdg && strlen (xdg) < sizeof (cmd_sock_path) - 16)
    snprintf (cmd_sock_path, sizeof (cmd_sock_path), "%s/swm.sock", xdg);
  else
    snprintf (cmd_sock_path, sizeof (cmd_sock_path), "%s", CMD_SOCK_PATH);

  unlink (cmd_sock_path);

  cmd_listen_fd
      = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (cmd_listen_fd < 0)
    {
      fprintf (stderr, "swm: cmd socket: %s\n", strerror (errno));
      cmd_sock_path[0] = '\0';
      return;
    }

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  {
    size_t plen = strlen (cmd_sock_path);
    if (plen >= sizeof (addr.sun_path))
      plen = sizeof (addr.sun_path) - 1;
    memcpy (addr.sun_path, cmd_sock_path, plen);
    addr.sun_path[plen] = '\0';
  }

  if (bind (cmd_listen_fd, (struct sockaddr *)&addr, sizeof (addr)) < 0)
    {
      fprintf (stderr, "swm: cmd bind(%s): %s\n", cmd_sock_path,
               strerror (errno));
      close (cmd_listen_fd);
      cmd_listen_fd = -1;
      cmd_sock_path[0] = '\0';
      return;
    }

  if (listen (cmd_listen_fd, 4) < 0)
    {
      fprintf (stderr, "swm: cmd listen: %s\n", strerror (errno));
      close (cmd_listen_fd);
      cmd_listen_fd = -1;
      unlink (cmd_sock_path);
      cmd_sock_path[0] = '\0';
      return;
    }

  n_cmd_clients = 0;
  fprintf (stderr, "swm: listening on %s\n", cmd_sock_path);
}

static void
cmd_cleanup (void)
{
  for (int i = 0; i < n_cmd_clients; i++)
    close (cmd_clients[i].fd);
  n_cmd_clients = 0;

  if (cmd_listen_fd >= 0)
    {
      close (cmd_listen_fd);
      cmd_listen_fd = -1;
    }

  if (cmd_sock_path[0])
    {
      unlink (cmd_sock_path);
      cmd_sock_path[0] = '\0';
    }
}

static void
cmd_drop_client (int idx)
{
  close (cmd_clients[idx].fd);
  cmd_clients[idx] = cmd_clients[--n_cmd_clients];
}

static void
cmd_reply (int fd, const char *msg)
{
  size_t len = strlen (msg);
  char buf[CMD_BUF_SIZE];
  if (len >= sizeof (buf) - 1)
    len = sizeof (buf) - 2;
  memcpy (buf, msg, len);
  buf[len] = '\n';

  (void)write (fd, buf, len + 1);
}

static void
cmd_dispatch (int client_fd, char *line)
{

  while (*line == ' ' || *line == '\t')
    line++;
  char *end = line + strlen (line) - 1;
  while (end > line && (*end == ' ' || *end == '\t' || *end == '\r'))
    *end-- = '\0';
  if (*line == '\0')
    return;

  if (strcmp (line, "help") == 0)
    {
      cmd_reply (client_fd,
                 "exec <cmd> | split h|v | split-move h|v | unsplit");
      cmd_reply (client_fd, "close | quit | reload | fullscreen");
      cmd_reply (client_fd, "set <key> <value> | get <key>");
      cmd_reply (client_fd,
                 "next-tab | prev-tab | move-tab-fwd | move-tab-bwd");
      cmd_reply (client_fd,
                 "next-ws | prev-ws | workspace <1-9> | send <1-9>");
      cmd_reply (client_fd,
                 "focus <dir> | move <dir>  (dir: left|right|up|down)");
      cmd_reply (
          client_fd,
          "query ws | query win-count | query win-title | query layout");
      return;
    }

  if (strncmp (line, "exec ", 5) == 0)
    {
      spawn (line + 5);
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strcmp (line, "split h") == 0)
    {
      action_split (1, 0);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "split v") == 0)
    {
      action_split (0, 0);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "split-move h") == 0)
    {
      action_split (1, 1);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "split-move v") == 0)
    {
      action_split (0, 1);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "unsplit") == 0)
    {
      action_remove_split ();
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strcmp (line, "close") == 0)
    {
      action_close_window ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "quit") == 0)
    {
      cmd_reply (client_fd, "ok");
      action_quit ();
      return;
    }
  if (strcmp (line, "reload") == 0)
    {
      reload_pending = 1;
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strncmp (line, "set ", 4) == 0)
    {
      char *kv = line + 4;
      while (*kv == ' ')
        kv++;
      char *sp = strchr (kv, ' ');
      if (sp)
        {
          *sp = '\0';
          char *val = sp + 1;
          while (*val == ' ')
            val++;
          strip_quotes (val);
          if (cfg_set_kv (kv, val))
            {
              apply_pending = 1;
              cmd_reply (client_fd, "ok");
            }
          else
            {
              cmd_reply (client_fd, "err: unknown key");
            }
        }
      else
        {
          cmd_reply (client_fd, "err: set <key> <value>");
        }
      return;
    }

  if (strncmp (line, "get ", 4) == 0)
    {
      const char *key = line + 4;
      while (*key == ' ')
        key++;
      char r[256];

      for (int i = 0; num_map[i].name; i++)
        {
          if (strcmp (key, num_map[i].name) == 0)
            {
              if (num_map[i].kind == CFG_DOUBLE)
                snprintf (r, sizeof (r), "%.2f",
                          *(double *)((const char *)&cfg + num_map[i].off));
              else
                snprintf (r, sizeof (r), "%d",
                          *(int *)((const char *)&cfg + num_map[i].off));
              cmd_reply (client_fd, r);
              return;
            }
        }

      for (int i = 0; col_map[i].name; i++)
        {
          if (strcmp (key, col_map[i].name) == 0)
            {
              cmd_reply (client_fd, (const char *)&cfg + col_map[i].off);
              return;
            }
        }

      cmd_reply (client_fd, "err: unknown key");
      return;
    }

  if (strcmp (line, "next-tab") == 0)
    {
      action_next_tab ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "prev-tab") == 0)
    {
      action_prev_tab ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "move-tab-fwd") == 0)
    {
      action_move_tab_forward ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "move-tab-bwd") == 0)
    {
      action_move_tab_backward ();
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strcmp (line, "next-ws") == 0)
    {
      action_next_workspace ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "prev-ws") == 0)
    {
      action_prev_workspace ();
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strncmp (line, "workspace ", 10) == 0)
    {
      int n = atoi (line + 10) - 1;
      if (n >= 0 && n < NUM_WORKSPACES)
        {
          action_switch_workspace (n);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: workspace 1-9");
        }
      return;
    }
  if (strncmp (line, "send ", 5) == 0)
    {
      int n = atoi (line + 5) - 1;
      if (n >= 0 && n < NUM_WORKSPACES)
        {
          action_send_to_workspace (n);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: workspace 1-9");
        }
      return;
    }

  if (strncmp (line, "focus ", 6) == 0)
    {
      const char *d = line + 6;
      if (strcmp (d, "left") == 0 || strcmp (d, "right") == 0
          || strcmp (d, "up") == 0 || strcmp (d, "down") == 0)
        {
          action_focus_direction (d);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: direction left|right|up|down");
        }
      return;
    }
  if (strncmp (line, "move ", 5) == 0)
    {
      const char *d = line + 5;
      if (strcmp (d, "left") == 0 || strcmp (d, "right") == 0
          || strcmp (d, "up") == 0 || strcmp (d, "down") == 0)
        {
          action_move_window_direction (d);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: direction left|right|up|down");
        }
      return;
    }

  if (strcmp (line, "fullscreen") == 0)
    {
      Node *tile = ws ()->active_tile;
      if (tile->tile.nwindows > 0)
        {
          fullscreen_toggle (tile->tile.windows[tile->tile.active_tab]);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: no window");
        }
      return;
    }

  if (strcmp (line, "query ws") == 0)
    {
      char r[16];
      snprintf (r, sizeof (r), "%d", cur_ws + 1);
      cmd_reply (client_fd, r);
      return;
    }
  if (strcmp (line, "query win-count") == 0)
    {
      char r[16];
      snprintf (r, sizeof (r), "%d", ws ()->active_tile->tile.nwindows);
      cmd_reply (client_fd, r);
      return;
    }
  if (strcmp (line, "query win-title") == 0)
    {
      Node *tile = ws ()->active_tile;
      if (tile->tile.nwindows > 0)
        {
          char title[256];
          get_wm_name (tile->tile.windows[tile->tile.active_tab], title,
                       sizeof (title));
          cmd_reply (client_fd, title);
        }
      else
        {
          cmd_reply (client_fd, "");
        }
      return;
    }
  if (strcmp (line, "query layout") == 0)
    {
      Node **tiles;
      int n = ws_get_tiles (ws (), &tiles);
      char r[16];
      snprintf (r, sizeof (r), "%d", n);
      cmd_reply (client_fd, r);
      return;
    }

  cmd_reply (client_fd, "err: unknown command");
}

static void
cmd_poll (fd_set *fds)
{
  if (cmd_listen_fd < 0)
    return;

  if (FD_ISSET (cmd_listen_fd, fds))
    {
      int cfd = accept (cmd_listen_fd, NULL, NULL);
      if (cfd >= 0)
        {
          fcntl (cfd, F_SETFL, O_NONBLOCK);
          fcntl (cfd, F_SETFD, FD_CLOEXEC);
          if (n_cmd_clients < CMD_MAX_CLIENTS)
            {
              cmd_clients[n_cmd_clients].fd = cfd;
              cmd_clients[n_cmd_clients].len = 0;
              n_cmd_clients++;
            }
          else
            {
              const char *msg = "err: too many clients\n";
              (void)write (cfd, msg, strlen (msg));
              close (cfd);
            }
        }
    }

  for (int i = 0; i < n_cmd_clients;)
    {
      if (!FD_ISSET (cmd_clients[i].fd, fds))
        {
          i++;
          continue;
        }

      char tmp[256];
      ssize_t nr = read (cmd_clients[i].fd, tmp, sizeof (tmp));
      if (nr <= 0)
        {
          cmd_drop_client (i);
          continue;
        }

      for (ssize_t j = 0; j < nr; j++)
        {
          if (tmp[j] == '\n' || tmp[j] == '\0')
            {
              cmd_clients[i].buf[cmd_clients[i].len] = '\0';
              cmd_dispatch (cmd_clients[i].fd, cmd_clients[i].buf);
              cmd_clients[i].len = 0;
            }
          else if (cmd_clients[i].len < CMD_BUF_SIZE - 1)
            {
              cmd_clients[i].buf[cmd_clients[i].len++] = tmp[j];
            }
        }
      i++;
    }
}

static int
cmd_fdset (fd_set *fds)
{
  int max_fd = -1;
  if (cmd_listen_fd < 0)
    return max_fd;
  FD_SET (cmd_listen_fd, fds);
  if (cmd_listen_fd > max_fd)
    max_fd = cmd_listen_fd;
  for (int i = 0; i < n_cmd_clients; i++)
    {
      FD_SET (cmd_clients[i].fd, fds);
      if (cmd_clients[i].fd > max_fd)
        max_fd = cmd_clients[i].fd;
    }
  return max_fd;
}

static void
cfg_apply (void)
{

  if (n_colors > 0)
    {
      unsigned long pixels[MAX_COLORS];
      for (int i = 0; i < n_colors; i++)
        pixels[i] = color_cache[i].px;
      XFreeColors (dpy, cmap, pixels, n_colors, 0);
    }
  n_colors = 0;

  int top_r = (cfg.statusbar_pos ? cfg.statusbar_height : 0)
              + (cfg.timebar_pos ? cfg.timebar_height : 0);
  int bot_r = (!cfg.statusbar_pos ? cfg.statusbar_height : 0)
              + (!cfg.timebar_pos ? cfg.timebar_height : 0);
  tile_y_off = top_r;
  tile_h_val = sh - top_r - bot_r;

  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      workspaces[i].tile_y = tile_y_off;
      workspaces[i].tile_h = tile_h_val;
    }

  bar_destroy ();
  if (cfg.statusbar_height > 0)
    bar_init ();

  timebar_destroy ();
  timebar_init ();

  XSetWindowBackground (dpy, root_win, px (cfg.col_desktop_bg));
  XClearWindow (dpy, root_win);

  arrange_workspace (ws ());
  focus_set_input (ws ()->active_tile);
  if (cfg.statusbar_height > 0)
    bar_draw ();
  timebar_draw ();

  grab_keys ();

  fprintf (stderr, "swm: config applied\n");
}

static void
on_map_request (XEvent *ev)
{
  Window wid = ev->xmaprequest.window;
  manage_window (wid);
}

static void
send_configure_notify (Window wid)
{
  XWindowAttributes wa;
  if (!XGetWindowAttributes (dpy, wid, &wa))
    return;

  XEvent ev;
  memset (&ev, 0, sizeof (ev));
  ev.xconfigure.type = ConfigureNotify;
  ev.xconfigure.event = wid;
  ev.xconfigure.window = wid;
  ev.xconfigure.x = wa.x;
  ev.xconfigure.y = wa.y;
  ev.xconfigure.width = wa.width;
  ev.xconfigure.height = wa.height;
  ev.xconfigure.border_width = wa.border_width;
  ev.xconfigure.above = None;
  ev.xconfigure.override_redirect = False;
  XSendEvent (dpy, wid, False, StructureNotifyMask, &ev);
}

static void
on_configure_request (XEvent *ev)
{
  XConfigureRequestEvent *cr = &ev->xconfigurerequest;
  Window wid = cr->window;
  if (managed_find (wid) >= 0)
    {

      send_configure_notify (wid);
      return;
    }

  XWindowChanges wc;
  unsigned int mask = 0;
  if (cr->value_mask & CWX)
    {
      wc.x = cr->x;
      mask |= CWX;
    }
  if (cr->value_mask & CWY)
    {
      wc.y = cr->y;
      mask |= CWY;
    }
  if (cr->value_mask & CWWidth)
    {
      wc.width = cr->width > 0 ? cr->width : 1;
      mask |= CWWidth;
    }
  if (cr->value_mask & CWHeight)
    {
      wc.height = cr->height > 0 ? cr->height : 1;
      mask |= CWHeight;
    }
  if (cr->value_mask & CWBorderWidth)
    {
      wc.border_width = cr->border_width;
      mask |= CWBorderWidth;
    }
  if (cr->value_mask & CWStackMode)
    {
      wc.stack_mode = cr->detail;
      mask |= CWStackMode;
    }
  if (mask)
    XConfigureWindow (dpy, wid, mask, &wc);
}

static void
on_destroy_notify (XEvent *ev)
{
  Window wid = ev->xdestroywindow.window;
  int was_float = set_contains (float_wins, n_float_wins, wid);
  set_remove (float_wins, &n_float_wins, wid);
  if (managed_find (wid) < 0)
    {
      if (was_float)
        {
          Node *tile = ws ()->active_tile;
          if (tile && tile->tile.nwindows > 0)
            focus_set_input (tile);
        }
      return;
    }
  unmanage_window (ev->xdestroywindow.window);
}

static void
on_unmap_notify (XEvent *ev)
{
  if (ev->xunmap.send_event)
    return;
  Window wid = ev->xunmap.window;
  int was_float = set_contains (float_wins, n_float_wins, wid);
  set_remove (float_wins, &n_float_wins, wid);
  int mi = managed_find (wid);
  if (mi < 0)
    {
      if (was_float)
        {
          Node *tile = ws ()->active_tile;
          if (tile && tile->tile.nwindows > 0)
            focus_set_input (tile);
        }
      return;
    }

  XWindowAttributes wa;
  if (!XGetWindowAttributes (dpy, wid, &wa))
    {
      unmanage_window (wid);
      return;
    }

  int ws_idx = managed[mi].ws;
  if (ws_idx == cur_ws)
    {
      Workspace *w = &workspaces[ws_idx];

      if (w->fullscreen_win != None && w->fullscreen_win != wid)
        return;

      Node *tile = ws_find_tile (w, wid);
      if (tile)
        {

          int idx = tile_index (tile, wid);
          if (idx == tile->tile.active_tab)
           {
            unmanage_window (wid);
           }
        }
    }
}

static void
on_expose (XEvent *ev)
{
  Window wid = ev->xexpose.window;
  if (status_bar_win != None && wid == status_bar_win)
    {
      bar_draw ();
      return;
    }
  if (timebar_win != None && wid == timebar_win)
    {
      timebar_draw ();
      return;
    }
  if (set_contains (tab_bars, n_tab_bars, wid))
    {
      Node **tiles;
      int n = ws_get_tiles (ws (), &tiles);
      for (int i = 0; i < n; i++)
        if (tiles[i]->tile.tab_bar_win == wid)
          {
            draw_tab_bar (tiles[i]);
            break;
          }
    }
}

static void
on_button_press (XEvent *ev)
{
  Window wid = ev->xbutton.window;

  if (status_bar_win != None && wid == status_bar_win)
    {
      bar_handle_click (ev);
      return;
    }

  if (timebar_win != None && wid == timebar_win)
    return;

  if (set_contains (tab_bars, n_tab_bars, wid))
    {
      Node **tiles;
      int n = ws_get_tiles (ws (), &tiles);
      for (int i = 0; i < n; i++)
        {
          if (tiles[i]->tile.tab_bar_win == wid)
            {
              Node *tile = tiles[i];
              if (tile->tile.nwindows == 0)
                {
                  ws ()->active_tile = tile;
                  focus_tile (tile);
                  break;
                }
              XWindowAttributes wa;
              XGetWindowAttributes (dpy, wid, &wa);
              int tab_w = wa.width / tile->tile.nwindows;
              if (tab_w < 1)
                tab_w = 1;
              int idx = ev->xbutton.x / tab_w;
              if (idx >= tile->tile.nwindows)
                idx = tile->tile.nwindows - 1;
              tile->tile.active_tab = idx;
              ws ()->active_tile = tile;
              arrange_tile (tile);
              focus_tile (tile);
              break;
            }
        }
      return;
    }

  int mi = managed_find (wid);
  if (mi >= 0 && managed[mi].ws == cur_ws)
    {
      Node *tile = ws_find_tile (ws (), wid);
      if (tile)
        {
          ws ()->active_tile = tile;
          tile->tile.active_tab = tile_index (tile, wid);
          focus_tile (tile);
        }
    }

  {
    XWindowAttributes wa;
    if (XGetWindowAttributes (dpy, wid, &wa) && !wa.override_redirect
        && wid != root_win && wid != status_bar_win && wid != timebar_win)
      {
        XRaiseWindow (dpy, wid);
        XSetInputFocus (dpy, wid, RevertToParent, CurrentTime);
      }
  }
}

static void
on_property_notify (XEvent *ev)
{
  Window wid = ev->xproperty.window;
  int mi = managed_find (wid);
  if (mi >= 0 && managed[mi].ws == cur_ws)
    {
      Node *tile = ws_find_tile (ws (), wid);
      if (tile)
        draw_tab_bar (tile);
    }
}

static void
on_client_message (XEvent *ev)
{
  XClientMessageEvent *cm = &ev->xclient;

  if (cm->message_type == a_net_wm_state && cm->format == 32)
    {
      long action = cm->data.l[0];
      Atom prop1 = (Atom)cm->data.l[1];
      Atom prop2 = (Atom)cm->data.l[2];

      if (prop1 == a_net_wm_state_fullscreen
          || prop2 == a_net_wm_state_fullscreen)
        {
          Window wid = cm->window;
          if (managed_find (wid) < 0)
            return;

          if (action == 2)
            fullscreen_toggle (wid);
          else if (action == 1)
            fullscreen_enter (wid);
          else
            fullscreen_exit (wid);
        }
    }
}

static void
on_key_press (XEvent *ev)
{
  KeySym ks = XLookupKeysym (&ev->xkey, 0);
  unsigned state = CLEAN_MASK (ev->xkey.state);

  for (int i = 0; i < cfg.n_binds; i++)
    {
      if (cfg.binds[i].key != ks)
        continue;
      if (CLEAN_MASK (cfg.binds[i].mod) != state)
        continue;

      switch (cfg.binds[i].action)
        {
        case ACT_SPAWN:
          {
            const char *cmd = (cfg.binds[i].sarg && cfg.binds[i].sarg[0])
                                  ? cfg.binds[i].sarg
                                  : NULL;
            if (cmd)
              spawn (cmd);
            break;
          }
        case ACT_SPLIT:
          action_split (cfg.binds[i].iarg & 1, cfg.binds[i].iarg & 2);
          break;
        case ACT_REMOVE_SPLIT:
          action_remove_split ();
          break;
        case ACT_CLOSE_WINDOW:
          action_close_window ();
          break;
        case ACT_QUIT:
          action_quit ();
          break;
        case ACT_NEXT_TAB:
          action_next_tab ();
          break;
        case ACT_PREV_TAB:
          action_prev_tab ();
          break;
        case ACT_MOVE_TAB_FWD:
          action_move_tab_forward ();
          break;
        case ACT_MOVE_TAB_BWD:
          action_move_tab_backward ();
          break;
        case ACT_NEXT_WORKSPACE:
          action_next_workspace ();
          break;
        case ACT_PREV_WORKSPACE:
          action_prev_workspace ();
          break;
        case ACT_SWITCH_WORKSPACE:
          action_switch_workspace (cfg.binds[i].iarg);
          break;
        case ACT_SEND_TO_WORKSPACE:
          action_send_to_workspace (cfg.binds[i].iarg);
          break;
        case ACT_FOCUS_DIR:
          if (cfg.binds[i].sarg)
            action_focus_direction (cfg.binds[i].sarg);
          break;
        case ACT_MOVE_WIN_DIR:
          if (cfg.binds[i].sarg)
            action_move_window_direction (cfg.binds[i].sarg);
          break;
        }
      return;
    }
}

static void
grab_keys (void)
{

  XUngrabKey (dpy, AnyKey, AnyModifier, root_win);

  unsigned lock_mods[] = { 0, Mod2Mask, LockMask, Mod2Mask | LockMask };
  int n_lock = (int)(sizeof (lock_mods) / sizeof (lock_mods[0]));

  for (int i = 0; i < cfg.n_binds; i++)
    {
      KeyCode kc = XKeysymToKeycode (dpy, cfg.binds[i].key);
      if (!kc)
        continue;
      for (int j = 0; j < n_lock; j++)
        XGrabKey (dpy, kc, cfg.binds[i].mod | lock_mods[j], root_win, True,
                  GrabModeAsync, GrabModeAsync);
    }

  XGrabButton (dpy, Button1, AnyModifier, root_win, True, ButtonPressMask,
               GrabModeSync, GrabModeAsync, None, None);
}

int
main (int argc, char **argv)
{
  (void)argc;
  (void)argv;
  signal (SIGCHLD, SIG_IGN);
  signal (SIGHUP, on_sighup);

  cfg_defaults ();
  {
    char cfgpath[512];
    if (cfg_resolve_path (cfgpath, sizeof (cfgpath)))
      {
        int n = cfg_load (cfgpath);
        if (n >= 0)
          fprintf (stderr, "swm: loaded %d settings from %s\n", n, cfgpath);
        else
          fprintf (stderr, "swm: failed to open %s\n", cfgpath);
      }
  }
  if (cfg.n_binds == 0)
    cfg_default_binds ();

  dpy = XOpenDisplay (NULL);
  if (!dpy)
    {
      fprintf (stderr, "Cannot open display\n");
      return 1;
    }

  scr_num = DefaultScreen (dpy);
  scr = ScreenOfDisplay (dpy, scr_num);
  root_win = RootWindow (dpy, scr_num);
  sw = scr->width;
  sh = scr->height;
  cmap = DefaultColormap (dpy, scr_num);
  depth = DefaultDepth (dpy, scr_num);

  {
    int top_r = (cfg.statusbar_pos ? cfg.statusbar_height : 0)
                + (cfg.timebar_pos ? cfg.timebar_height : 0);
    int bot_r = (!cfg.statusbar_pos ? cfg.statusbar_height : 0)
                + (!cfg.timebar_pos ? cfg.timebar_height : 0);
    tile_y_off = top_r;
    tile_h_val = sh - top_r - bot_r;
  }

  font = XLoadQueryFont (dpy,
                         "-misc-fixed-medium-r-*-*-13-*-*-*-*-*-iso8859-1");
  if (!font)
    font = XLoadQueryFont (dpy, "fixed");
  if (!font)
    {
      fprintf (stderr, "Cannot load font\n");
      XCloseDisplay (dpy);
      return 1;
    }

  a_net_wm_name = XInternAtom (dpy, "_NET_WM_NAME", False);
  a_utf8 = XInternAtom (dpy, "UTF8_STRING", False);
  a_wm_protocols = XInternAtom (dpy, "WM_PROTOCOLS", False);
  a_wm_delete = XInternAtom (dpy, "WM_DELETE_WINDOW", False);
  a_wm_type = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE", False);
  a_wm_type_dock = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
  a_net_wm_state = XInternAtom (dpy, "_NET_WM_STATE", False);
  a_net_wm_state_fullscreen
      = XInternAtom (dpy, "_NET_WM_STATE_FULLSCREEN", False);
  a_wm_type_splash = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
  a_wm_type_dialog = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  a_wm_type_tooltip = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
  a_wm_type_notification
      = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
  a_wm_type_popup_menu
      = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);

  draw_gc = XCreateGC (dpy, root_win, 0, NULL);

  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      workspaces[i].sw = sw;
      workspaces[i].sh = sh;
      workspaces[i].tile_y = tile_y_off;
      workspaces[i].tile_h = tile_h_val;
      workspaces[i].root = node_new_tile (0, tile_y_off, sw, tile_h_val);
      if (!workspaces[i].root)
        {
          fprintf (stderr, "swm: workspace %d: alloc failed\n", i);
          XCloseDisplay (dpy);
          return 1;
        }
      workspaces[i].active_tile = workspaces[i].root;
      workspaces[i].fullscreen_win = None;
      workspaces[i].tiles_dirty = 1;
    }
  cur_ws = 0;
  status_bar_win = None;
  timebar_win = None;
  running = 1;

  XSetWindowBackground (dpy, root_win, px (cfg.col_desktop_bg));
  XClearWindow (dpy, root_win);

  wm_detected = 0;
  XSetErrorHandler (xerr_detect);
  XSelectInput (dpy, root_win,
                SubstructureRedirectMask | SubstructureNotifyMask
                    | KeyPressMask | ButtonPressMask | FocusChangeMask);
  XSync (dpy, False);
  if (wm_detected)
    {
      fprintf (stderr, "Another WM is running\n");
      XCloseDisplay (dpy);
      return 1;
    }
  XSetErrorHandler (xerr_handler);

  grab_keys ();
  XSync (dpy, False);

  cmd_init ();

  init_ewmh ();

  if (cfg.statusbar_height > 0)
    bar_init ();

  timebar_init ();

  {
    Window d1, d2, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree (dpy, root_win, &d1, &d2, &children, &nchildren))
      {
        for (unsigned int i = 0; i < nchildren; i++)
          {
            XWindowAttributes wa;
            if (XGetWindowAttributes (dpy, children[i], &wa)
                && wa.map_state == IsViewable && !wa.override_redirect)
              {
                manage_window (children[i]);
              }
          }
        if (children)
          XFree (children);
      }
  }

  arrange_workspace (ws ());
  last_bar_update = mono_time ();

  int x_fd = ConnectionNumber (dpy);

  while (running)
    {

      while (XPending (dpy))
        {
          XEvent ev;
          XNextEvent (dpy, &ev);
          switch (ev.type)
            {
            case MapRequest:
              on_map_request (&ev);
              break;
            case ConfigureRequest:
              on_configure_request (&ev);
              break;
            case DestroyNotify:
              on_destroy_notify (&ev);
              break;
            case UnmapNotify:
              on_unmap_notify (&ev);
              break;
            case Expose:
              on_expose (&ev);
              break;
            case KeyPress:
              on_key_press (&ev);
              break;
            case ButtonPress:
              on_button_press (&ev);
              XAllowEvents (dpy, ReplayPointer, CurrentTime);
              break;
            case PropertyNotify:
              on_property_notify (&ev);
              break;
            case ClientMessage:
              on_client_message (&ev);
              break;
            }
        }

      double now = mono_time ();
      if ((now - last_bar_update) >= cfg.bar_update_interval)
        {
          if (cfg.statusbar_height > 0)
            bar_draw ();
          timebar_draw ();
          last_bar_update = now;
        }

      double remaining
          = cfg.bar_update_interval - (mono_time () - last_bar_update);
      if (remaining < 0.05)
        remaining = 0.05;
      struct timeval tv;
      tv.tv_sec = (long)remaining;
      tv.tv_usec = (long)((remaining - tv.tv_sec) * 1e6);
      fd_set fds;
      FD_ZERO (&fds);
      FD_SET (x_fd, &fds);
      int max_fd = x_fd;
      int cmd_max = cmd_fdset (&fds);
      if (cmd_max > max_fd)
        max_fd = cmd_max;
      select (max_fd + 1, &fds, NULL, NULL, &tv);

      cmd_poll (&fds);

      if (apply_pending)
        {
          apply_pending = 0;
          cfg_apply ();
        }

      if (reload_pending)
        {
          reload_pending = 0;
          char cfgpath[512];
          if (cfg_resolve_path (cfgpath, sizeof (cfgpath)))
            {
              cfg_defaults ();
              int n = cfg_load (cfgpath);
              if (cfg.n_binds == 0)
                cfg_default_binds ();
              fprintf (stderr, "swm: reload %d settings from %s\n", n,
                       cfgpath);
              cfg_apply ();
            }
        }
    }

  cmd_cleanup ();
  bar_destroy ();
  timebar_destroy ();
  cleanup_ewmh ();
  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      Node **tiles;
      int n = ws_get_tiles (&workspaces[i], &tiles);
      for (int j = 0; j < n; j++)
        {
          destroy_tab_bar (tiles[j]);
          destroy_frame (tiles[j]);
        }
      node_free_tree (workspaces[i].root);
      workspaces[i].root = NULL;
    }

  for (int i = 0; i < cfg.n_binds; i++)
    {
      free (cfg.binds[i].sarg);
      cfg.binds[i].sarg = NULL;
    }
  cfg.n_binds = 0;

  if (n_colors > 0)
    {
      unsigned long pixels[MAX_COLORS];
      for (int i = 0; i < n_colors; i++)
        pixels[i] = color_cache[i].px;
      XFreeColors (dpy, cmap, pixels, n_colors, 0);
      n_colors = 0;
    }

  if (font)
    {
      XFreeFont (dpy, font);
      font = NULL;
    }

  XFreeGC (dpy, draw_gc);
  XCloseDisplay (dpy);
  return 0;
}/*
 * SWM — A scriptable tabbed tiling window manager inspired by Notion and TinyWM.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NUM_WORKSPACES 9
#define CMD_SOCK_PATH "/tmp/swm.sock"
#define CMD_MAX_CLIENTS 8
#define CMD_BUF_SIZE 512
#define MAX_TILES 256
#define MAX_MANAGED 512
#define MAX_SET 512
#define MAX_COLORS 256
#define MAX_BINDINGS 64
#define CLEAN_MASK(m) ((m) & ~(Mod2Mask | LockMask))

typedef enum
{
  ACT_SPAWN,
  ACT_SPLIT,
  ACT_REMOVE_SPLIT,
  ACT_CLOSE_WINDOW,
  ACT_QUIT,
  ACT_NEXT_TAB,
  ACT_PREV_TAB,
  ACT_MOVE_TAB_FWD,
  ACT_MOVE_TAB_BWD,
  ACT_NEXT_WORKSPACE,
  ACT_PREV_WORKSPACE,
  ACT_SWITCH_WORKSPACE,
  ACT_SEND_TO_WORKSPACE,
  ACT_FOCUS_DIR,
  ACT_MOVE_WIN_DIR,
} Action;

typedef struct
{
  KeySym key;
  unsigned mod;
  Action action;
  int iarg;
  char *sarg;
} Keybind;

typedef struct
{

  int tab_bar_height;
  int border_width;
  int border_gap;
  int statusbar_height;
  int statusbar_pos;
  int timebar_pos;
  int timebar_height;
  double bar_update_interval;

  char col_statusbar_bg[8];
  char col_statusbar_fg[8];
  char col_statusbar_ws_active[8];
  char col_statusbar_ws_inactive[8];
  char col_statusbar_ws_occupied[8];
  char col_statusbar_ws_fg_act[8];
  char col_statusbar_ws_fg_inact[8];

  char col_tab_active_bg[8];
  char col_tab_inactive_bg[8];
  char col_tab_active_fg[8];
  char col_tab_inactive_fg[8];
  char col_tab_bar_bg[8];
  char col_tab_active_bg_dim[8];
  char col_tab_active_fg_dim[8];

  char col_border_active[8];
  char col_border_inactive[8];
  char col_desktop_bg[8];

  char col_timebar_bg[8];
  char col_timebar_hour[8];
  char col_timebar_hex[8];
  char col_timebar_seg[8];
  char col_timebar_label[8];
  char col_timebar_track[8];

  Keybind binds[MAX_BINDINGS];
  int n_binds;
} Config;

typedef enum
{
  CFG_INT,
  CFG_DOUBLE
} CfgNumKind;

typedef enum
{
  NODE_TILE,
  NODE_SPLIT
} NodeType;

typedef struct Node
{
  NodeType type;
  int x, y, w, h;
  struct Node *parent;
  union
  {
    struct
    {
      Window *windows;
      int nwindows;
      int win_cap;
      int active_tab;
      int prev_active_tab;
      Window tab_bar_win;
      Window frame_win;
    } tile;
    struct
    {
      int horizontal;
      double ratio;
      struct Node *children[2];
    } split;
  };
} Node;

typedef struct
{
  int sw, sh, tile_y, tile_h;
  Node *root;
  Node *active_tile;
  Window fullscreen_win;

  Node *cached_tiles[MAX_TILES];
  int n_cached_tiles;
  int tiles_dirty;
} Workspace;

typedef struct
{
  Window wid;
  int ws;
} ManagedEntry;

typedef struct
{
  char hex[8];
  unsigned long px;
} ColorEntry;

static const struct
{
  const char *name;
  size_t off;
} col_map[] = {
  { "col_statusbar_bg", offsetof (Config, col_statusbar_bg) },
  { "col_statusbar_fg", offsetof (Config, col_statusbar_fg) },
  { "col_statusbar_ws_active", offsetof (Config, col_statusbar_ws_active) },
  { "col_statusbar_ws_inactive",
    offsetof (Config, col_statusbar_ws_inactive) },
  { "col_statusbar_ws_occupied",
    offsetof (Config, col_statusbar_ws_occupied) },
  { "col_statusbar_ws_fg_act", offsetof (Config, col_statusbar_ws_fg_act) },
  { "col_statusbar_ws_fg_inact",
    offsetof (Config, col_statusbar_ws_fg_inact) },
  { "col_tab_active_bg", offsetof (Config, col_tab_active_bg) },
  { "col_tab_inactive_bg", offsetof (Config, col_tab_inactive_bg) },
  { "col_tab_active_fg", offsetof (Config, col_tab_active_fg) },
  { "col_tab_inactive_fg", offsetof (Config, col_tab_inactive_fg) },
  { "col_tab_bar_bg", offsetof (Config, col_tab_bar_bg) },
  { "col_tab_active_bg_dim", offsetof (Config, col_tab_active_bg_dim) },
  { "col_tab_active_fg_dim", offsetof (Config, col_tab_active_fg_dim) },
  { "col_border_active", offsetof (Config, col_border_active) },
  { "col_border_inactive", offsetof (Config, col_border_inactive) },
  { "col_desktop_bg", offsetof (Config, col_desktop_bg) },
  { "col_timebar_bg", offsetof (Config, col_timebar_bg) },
  { "col_timebar_hour", offsetof (Config, col_timebar_hour) },
  { "col_timebar_hex", offsetof (Config, col_timebar_hex) },
  { "col_timebar_seg", offsetof (Config, col_timebar_seg) },
  { "col_timebar_label", offsetof (Config, col_timebar_label) },
  { "col_timebar_track", offsetof (Config, col_timebar_track) },
  { NULL, 0 }
};

static const struct
{
  const char *name;
  size_t off;
  CfgNumKind kind;
} num_map[]
    = { { "tab_bar_height", offsetof (Config, tab_bar_height), CFG_INT },
        { "border_width", offsetof (Config, border_width), CFG_INT },
        { "border_gap", offsetof (Config, border_gap), CFG_INT },
        { "statusbar_height", offsetof (Config, statusbar_height), CFG_INT },
        { "statusbar_pos", offsetof (Config, statusbar_pos), CFG_INT },
        { "timebar_pos", offsetof (Config, timebar_pos), CFG_INT },
        { "timebar_height", offsetof (Config, timebar_height), CFG_INT },
        { "bar_update_interval", offsetof (Config, bar_update_interval),
          CFG_DOUBLE },
        { NULL, 0, 0 } };

static const struct
{
  const char *name;
  Action act;
} action_map[] = { { "spawn", ACT_SPAWN },
                   { "split_h", ACT_SPLIT },
                   { "split_v", ACT_SPLIT },
                   { "split_h_move", ACT_SPLIT },
                   { "split_v_move", ACT_SPLIT },
                   { "remove_split", ACT_REMOVE_SPLIT },
                   { "close", ACT_CLOSE_WINDOW },
                   { "quit", ACT_QUIT },
                   { "next_tab", ACT_NEXT_TAB },
                   { "prev_tab", ACT_PREV_TAB },
                   { "move_tab_fwd", ACT_MOVE_TAB_FWD },
                   { "move_tab_bwd", ACT_MOVE_TAB_BWD },
                   { "next_ws", ACT_NEXT_WORKSPACE },
                   { "prev_ws", ACT_PREV_WORKSPACE },
                   { "switch_ws", ACT_SWITCH_WORKSPACE },
                   { "send_to_ws", ACT_SEND_TO_WORKSPACE },
                   { "focus", ACT_FOCUS_DIR },
                   { "move_win", ACT_MOVE_WIN_DIR },
                   { NULL, 0 } };

static struct
{
  int fd;
  char buf[CMD_BUF_SIZE];
  int len;
} cmd_clients[CMD_MAX_CLIENTS];

static Display *dpy;
static int scr_num;
static Screen *scr;
static Window root_win;
static int sw, sh;
static int tile_y_off, tile_h_val;
static Colormap cmap;
static XFontStruct *font;
static int depth;
static Workspace workspaces[NUM_WORKSPACES];
static int cur_ws;
static ManagedEntry managed[MAX_MANAGED];
static int n_managed;
static Window tab_bars[MAX_SET];
static int n_tab_bars;
static Window frame_wins[MAX_SET];
static int n_frame_wins;
static Window status_bar_win;
static Window timebar_win;
static Window float_wins[MAX_SET];
static int n_float_wins;
static double last_bar_update;
static int running;
static ColorEntry color_cache[MAX_COLORS];
static int n_colors;
static Atom a_net_wm_name, a_utf8, a_wm_protocols, a_wm_delete;
static Atom a_wm_type, a_wm_type_dock;
static Atom a_net_wm_state, a_net_wm_state_fullscreen;
static Atom a_wm_type_splash, a_wm_type_dialog;
static Atom a_wm_type_tooltip, a_wm_type_notification;
static Atom a_wm_type_popup_menu;
static Window ewmh_check_win;
static GC draw_gc;
static int cmd_listen_fd = -1;
static char cmd_sock_path[256];
static void ensure_frame (Node *tile);
static void destroy_frame (Node *tile);
static void ensure_tab_bar (Node *tile);
static void destroy_tab_bar (Node *tile);
static void draw_tab_bar (Node *tile);
static void arrange_tile (Node *tile);
static void arrange_workspace (Workspace *w);
static void raise_floats (void);
static void hide_workspace (Workspace *w);
static void focus_tile (Node *tile);
static void focus_set_input (Node *tile);
static void send_configure_notify (Window wid);
static void bar_draw (void);
static void bar_destroy (void);
static void timebar_init (void);
static void timebar_draw (void);
static void timebar_destroy (void);
static void unmanage_window (Window wid);
static void cmd_init (void);
static void cmd_cleanup (void);
static void cmd_poll (fd_set *fds);
static void action_switch_workspace (int n);
static int valid_hex_color (const char *hex);
static volatile sig_atomic_t reload_pending;
static volatile sig_atomic_t apply_pending;
static void cfg_apply (void);
static void grab_keys (void);
static Config cfg;
static int n_cmd_clients;
static int wm_detected;

static void
sarg_set (char **dst, const char *src)
{
  free (*dst);
  *dst = (src && *src) ? strdup (src) : NULL;
}

static void
cfg_defaults (void)
{
  cfg.tab_bar_height = 22;
  cfg.border_width = 1;
  cfg.border_gap = 2;
  cfg.statusbar_height = 24;
  cfg.statusbar_pos = 0;
  cfg.timebar_pos = 1;
  cfg.timebar_height = 14;
  cfg.bar_update_interval = 1.0;

#define SETCOL(dst, lit)                                                      \
  do                                                                          \
    {                                                                         \
      _Static_assert (sizeof (lit) == 8, "color literal must be 7 chars");    \
      memcpy ((dst), (lit), 8);                                               \
    }                                                                         \
  while (0)
  SETCOL (cfg.col_statusbar_bg, "#1E1E1E");
  SETCOL (cfg.col_statusbar_fg, "#FFBF00");
  SETCOL (cfg.col_statusbar_ws_active, "#fbe7ac");
  SETCOL (cfg.col_statusbar_ws_inactive, "#555555");
  SETCOL (cfg.col_statusbar_ws_occupied, "#888888");
  SETCOL (cfg.col_statusbar_ws_fg_act, "#000000");
  SETCOL (cfg.col_statusbar_ws_fg_inact, "#AAAAAA");

  SETCOL (cfg.col_tab_active_bg, "#fbe7ac");
  SETCOL (cfg.col_tab_inactive_bg, "#3C3C3C");
  SETCOL (cfg.col_tab_active_fg, "#000000");
  SETCOL (cfg.col_tab_inactive_fg, "#AAAAAA");
  SETCOL (cfg.col_tab_bar_bg, "#2B2B2B");
  SETCOL (cfg.col_tab_active_bg_dim, "#555555");
  SETCOL (cfg.col_tab_active_fg_dim, "#CCCCCC");

  SETCOL (cfg.col_border_active, "#696969");
  SETCOL (cfg.col_border_inactive, "#1E1E1E");
  SETCOL (cfg.col_desktop_bg, "#000000");

  SETCOL (cfg.col_timebar_bg, "#1E1E1E");
  SETCOL (cfg.col_timebar_hour, "#CC3300");
  SETCOL (cfg.col_timebar_hex, "#00CC66");
  SETCOL (cfg.col_timebar_seg, "#CCAA00");
  SETCOL (cfg.col_timebar_label, "#FFFFFF");
  SETCOL (cfg.col_timebar_track, "#333333");
#undef SETCOL

  for (int i = 0; i < cfg.n_binds; i++)
    free (cfg.binds[i].sarg);
  cfg.n_binds = 0;
}

static void
cfg_default_binds (void)
{
#define B(k, m, a, i, s)                                                      \
  do                                                                          \
    {                                                                         \
      Keybind *b = &cfg.binds[cfg.n_binds++];                                 \
      b->key = (k);                                                           \
      b->mod = (m);                                                           \
      b->action = (a);                                                        \
      b->iarg = (i);                                                          \
      b->sarg = NULL;                                                         \
      sarg_set (&b->sarg, (s));                                               \
    }                                                                         \
  while (0)

  B (XK_F1, 0, ACT_PREV_TAB, 0, NULL);
  B (XK_F2, 0, ACT_NEXT_TAB, 0, NULL);
  B (XK_F3, 0, ACT_PREV_WORKSPACE, 0, NULL);
  B (XK_F4, 0, ACT_NEXT_WORKSPACE, 0, NULL);
  B (XK_F6, 0, ACT_CLOSE_WINDOW, 0, NULL);
  B (XK_h, Mod4Mask, ACT_SPLIT, 1, NULL);
  B (XK_v, Mod4Mask, ACT_SPLIT, 0, NULL);
  B (XK_d, Mod4Mask, ACT_REMOVE_SPLIT, 0, NULL);
  B (XK_q, Mod4Mask, ACT_QUIT, 0, NULL);
  B (XK_comma, Mod4Mask, ACT_MOVE_TAB_BWD, 0, NULL);
  B (XK_period, Mod4Mask, ACT_MOVE_TAB_FWD, 0, NULL);
  B (XK_Left, Mod4Mask, ACT_FOCUS_DIR, 0, "left");
  B (XK_Right, Mod4Mask, ACT_FOCUS_DIR, 0, "right");
  B (XK_Up, Mod4Mask, ACT_FOCUS_DIR, 0, "up");
  B (XK_Down, Mod4Mask, ACT_FOCUS_DIR, 0, "down");
  B (XK_h, Mod4Mask | ShiftMask, ACT_SPLIT, 3, NULL);
  B (XK_v, Mod4Mask | ShiftMask, ACT_SPLIT, 2, NULL);
  B (XK_Left, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "left");
  B (XK_Right, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "right");
  B (XK_Up, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "up");
  B (XK_Down, Mod4Mask | ShiftMask, ACT_MOVE_WIN_DIR, 0, "down");

  for (int i = 0; i < 9; i++)
    B (XK_1 + i, Mod4Mask, ACT_SWITCH_WORKSPACE, i, NULL);

  for (int i = 0; i < 9; i++)
    B (XK_1 + i, Mod4Mask | ShiftMask, ACT_SEND_TO_WORKSPACE, i, NULL);

#undef B
}

static void
strip_quotes (char *s)
{
  size_t len = strlen (s);
  if (len >= 2
      && ((s[0] == '"' && s[len - 1] == '"')
      || (s[0] == '\'' && s[len - 1] == '\'')))
    {
      memmove (s, s + 1, len - 2);
      s[len - 2] = '\0';
    }
}

static unsigned
parse_mod_token (const char *s)
{
  if (strcmp (s, "Mod4") == 0 || strcmp (s, "Super") == 0)
    return Mod4Mask;
  if (strcmp (s, "Shift") == 0)
    return ShiftMask;
  if (strcmp (s, "Mod1") == 0 || strcmp (s, "Alt") == 0)
    return Mod1Mask;
  if (strcmp (s, "Control") == 0 || strcmp (s, "Ctrl") == 0)
    return ControlMask;
  if (strcmp (s, "Mod2") == 0)
    return Mod2Mask;
  if (strcmp (s, "Mod3") == 0)
    return Mod3Mask;
  if (strcmp (s, "Mod5") == 0)
    return Mod5Mask;
  return 0;
}

static int
split_iarg (const char *name)
{
  if (strcmp (name, "split_h") == 0)
    return 1;
  if (strcmp (name, "split_v") == 0)
    return 0;
  if (strcmp (name, "split_h_move") == 0)
    return 3;
  if (strcmp (name, "split_v_move") == 0)
    return 2;
  return 0;
}

static int
spawn_slot (const char *arg)
{
  if (strcmp (arg, "terminal") == 0)
    return 0;
  if (strcmp (arg, "file_manager") == 0)
    return 1;
  if (strcmp (arg, "browser") == 0)
    return 2;
  if (strcmp (arg, "launcher") == 0)
    return 3;
  if (strcmp (arg, "reload") == 0)
    return 4;
  return -1;
}

static int
cfg_parse_bind (const char *val)
{
  if (cfg.n_binds >= MAX_BINDINGS)
    {
      fprintf (stderr, "swm: max bindings (%d) reached\n", MAX_BINDINGS);
      return 0;
    }

  char buf[256];
  strncpy (buf, val, sizeof (buf) - 1);
  buf[sizeof (buf) - 1] = '\0';

  char *chord = buf;
  while (*chord == ' ' || *chord == '\t')
    chord++;

  char *sp = chord;
  while (*sp && *sp != ' ' && *sp != '\t')
    sp++;
  if (!*sp)
    {
      fprintf (stderr, "swm: bind: missing action in '%s'\n", val);
      return 0;
    }
  *sp++ = '\0';

  while (*sp == ' ' || *sp == '\t')
    sp++;
  char *act_name = sp;

  while (*sp && *sp != ' ' && *sp != '\t')
    sp++;
  char *arg = NULL;
  if (*sp)
    {
      *sp++ = '\0';
      while (*sp == ' ' || *sp == '\t')
        sp++;
      if (*sp)
        arg = sp;

      if (arg)
        {
          char *e = arg + strlen (arg) - 1;
          while (e > arg && (*e == ' ' || *e == '\t'))
            *e-- = '\0';
          strip_quotes (arg);
        }
    }

  unsigned mod = 0;
  KeySym ks = NoSymbol;
  {
    char chord_buf[128];
    strncpy (chord_buf, chord, sizeof (chord_buf) - 1);
    chord_buf[sizeof (chord_buf) - 1] = '\0';

    char *tokens[8];
    int ntok = 0;
    char *t = strtok (chord_buf, "+");
    while (t && ntok < 8)
      {
        tokens[ntok++] = t;
        t = strtok (NULL, "+");
      }

    if (ntok == 0)
      {
        fprintf (stderr, "swm: bind: empty chord in '%s'\n", val);
        return 0;
      }

    for (int i = 0; i < ntok - 1; i++)
      {
        unsigned m = parse_mod_token (tokens[i]);
        if (!m)
          {
            fprintf (stderr, "swm: bind: unknown modifier '%s'\n", tokens[i]);
            return 0;
          }
        mod |= m;
      }

    ks = XStringToKeysym (tokens[ntok - 1]);
    if (ks == NoSymbol)
      {
        fprintf (stderr, "swm: bind: unknown key '%s'\n", tokens[ntok - 1]);
        return 0;
      }
  }

  Action action = (Action)-1;
  for (int i = 0; action_map[i].name; i++)
    {
      if (strcmp (act_name, action_map[i].name) == 0)
        {
          action = action_map[i].act;
          break;
        }
    }
  if ((int)action == -1)
    {
      fprintf (stderr, "swm: bind: unknown action '%s'\n", act_name);
      return 0;
    }

  Keybind *b = &cfg.binds[cfg.n_binds];
  memset (b, 0, sizeof (*b));
  b->key = ks;
  b->mod = mod;
  b->action = action;

  switch (action)
    {
    case ACT_SPLIT:
      b->iarg = split_iarg (act_name);
      break;
    case ACT_SPAWN:
      if (arg)
        {
          int slot = spawn_slot (arg);
          if (slot >= 0)
            {
              b->iarg = slot;
            }
          else
            {

              b->iarg = -1;
              sarg_set (&b->sarg, arg);
            }
        }
      break;
    case ACT_SWITCH_WORKSPACE:
    case ACT_SEND_TO_WORKSPACE:
      if (arg)
        b->iarg = atoi (arg) - 1;
      if (b->iarg < 0)
        b->iarg = 0;
      if (b->iarg >= NUM_WORKSPACES)
        b->iarg = NUM_WORKSPACES - 1;
      break;
    case ACT_FOCUS_DIR:
    case ACT_MOVE_WIN_DIR:
      if (arg)
        sarg_set (&b->sarg, arg);
      break;
    default:
      break;
    }

  cfg.n_binds++;
  return 1;
}

static int
cfg_resolve_path (char *out, int len)
{
  const char *env;
  env = getenv ("XDG_CONFIG_HOME");
  if (env && *env)
    {
      snprintf (out, len, "%s/swm/config", env);
      if (access (out, R_OK) == 0)
        return 1;
    }
  const char *home = getenv ("HOME");
  if (!home)
    home = "/root";
  snprintf (out, len, "%s/.config/swm/config", home);
  if (access (out, R_OK) == 0)
    return 1;
  return 0;
}

static int
cfg_set_kv (const char *key, const char *val)
{

  for (int i = 0; num_map[i].name; i++)
    {
      if (strcmp (key, num_map[i].name) == 0)
        {
          if (num_map[i].kind == CFG_DOUBLE)
            *(double *)((char *)&cfg + num_map[i].off) = atof (val);
          else
            *(int *)((char *)&cfg + num_map[i].off) = atoi (val);
          return 1;
        }
    }

  if (strcmp (key, "bind") == 0)
    return cfg_parse_bind (val);

  for (int i = 0; col_map[i].name; i++)
    {
      if (strcmp (key, col_map[i].name) == 0)
        {
          if (!valid_hex_color (val))
            {
              fprintf (stderr, "swm: invalid color value '%s' for '%s'\n", val,
                       key);
              return 0;
            }
          memcpy ((char *)&cfg + col_map[i].off, val, 7);
          ((char *)&cfg + col_map[i].off)[7] = '\0';
          return 1;
        }
    }

  return 0;
}

static int
cfg_load (const char *path)
{
  FILE *f = fopen (path, "r");
  if (!f)
    return -1;

  char line[512];
  int lineno = 0, count = 0;
  while (fgets (line, sizeof (line), f))
    {
      lineno++;

      char *nl = strchr (line, '\n');
      if (nl)
        *nl = '\0';

      char *p = line;
      while (*p == ' ' || *p == '\t')
        p++;
      if (*p == '#' || *p == '\0')
        continue;

      char *eq = strchr (p, '=');
      if (!eq)
        {
          fprintf (stderr, "swm: config:%d: missing '='\n", lineno);
          continue;
        }

      char *kend = eq - 1;
      while (kend > p && (*kend == ' ' || *kend == '\t'))
        kend--;
      *(kend + 1) = '\0';

      char *val = eq + 1;
      while (*val == ' ' || *val == '\t')
        val++;

      char *vend = val + strlen (val) - 1;
      while (vend > val && (*vend == ' ' || *vend == '\t'))
        *vend-- = '\0';

      strip_quotes (val);

      if (cfg_set_kv (p, val))
        count++;
    }
  fclose (f);
  return count;
}

static void
on_sighup (int sig)
{
  (void)sig;
  reload_pending = 1;
}

static int
is_hex_digit (char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int
valid_hex_color (const char *hex)
{
  if (!hex || hex[0] != '#')
    return 0;
  for (int i = 1; i <= 6; i++)
    if (!is_hex_digit (hex[i]))
      return 0;
  return 1;
}

static int
hex_val (char c)
{
  if (c >= 'a')
    return c - 'a' + 10;
  if (c >= 'A')
    return c - 'A' + 10;
  return c - '0';
}

static unsigned long
px (const char *hex)
{
  int i;
  for (i = 0; i < n_colors; i++)
    if (strcmp (color_cache[i].hex, hex) == 0)
      return color_cache[i].px;

  if (!valid_hex_color (hex))
    {
      fprintf (stderr, "swm: px: invalid color '%s', using black\n",
               hex ? hex : "(null)");
      hex = "#000000";
      for (i = 0; i < n_colors; i++)
        if (strcmp (color_cache[i].hex, hex) == 0)
          return color_cache[i].px;
    }

  XColor c;
  c.red = (unsigned short)((hex_val (hex[1]) * 16 + hex_val (hex[2])) * 256);
  c.green
      = (unsigned short)((hex_val (hex[3]) * 16 + hex_val (hex[4])) * 256);
  c.blue
      = (unsigned short)((hex_val (hex[5]) * 16 + hex_val (hex[6])) * 256);
  c.flags = DoRed | DoGreen | DoBlue;
  XAllocColor (dpy, cmap, &c);

  if (n_colors < MAX_COLORS)
    {
      memcpy (color_cache[n_colors].hex, hex, 7);
      color_cache[n_colors].hex[7] = '\0';
      color_cache[n_colors].px = c.pixel;
      n_colors++;
    }
  return c.pixel;
}

static int
set_contains (const Window *s, int n, Window w)
{
  for (int i = 0; i < n; i++)
    if (s[i] == w)
      return 1;
  return 0;
}

static void
set_add (Window *s, int *n, Window w)
{
  if (*n < MAX_SET && !set_contains (s, *n, w))
    s[(*n)++] = w;
}

static void
set_remove (Window *s, int *n, Window w)
{
  for (int i = 0; i < *n; i++)
    if (s[i] == w)
      {
        s[i] = s[--(*n)];
        return;
      }
}

static int
managed_find (Window wid)
{
  for (int i = 0; i < n_managed; i++)
    if (managed[i].wid == wid)
      return i;
  return -1;
}

static void
managed_add (Window wid, int ws)
{
  if (n_managed < MAX_MANAGED)
    {
      managed[n_managed].wid = wid;
      managed[n_managed].ws = ws;
      n_managed++;
    }
}

static void
managed_del (Window wid)
{
  int i = managed_find (wid);
  if (i >= 0)
    managed[i] = managed[--n_managed];
}

static void
managed_set_ws (Window wid, int ws)
{
  int i = managed_find (wid);
  if (i >= 0)
    managed[i].ws = ws;
}

static int
tile_has (Node *t, Window w)
{
  for (int i = 0; i < t->tile.nwindows; i++)
    if (t->tile.windows[i] == w)
      return 1;
  return 0;
}

static void
tile_add (Node *t, Window w)
{
  if (t->tile.nwindows >= t->tile.win_cap)
    {
      int new_cap = t->tile.win_cap ? t->tile.win_cap * 2 : 4;
      Window *tmp
          = realloc (t->tile.windows, (size_t)new_cap * sizeof (Window));
      if (!tmp)
        {
          fprintf (stderr, "swm: tile_add: realloc failed\n");
          return;
        }
      t->tile.windows = tmp;
      t->tile.win_cap = new_cap;
    }
  t->tile.windows[t->tile.nwindows++] = w;
}

static void
tile_remove (Node *t, Window w)
{
  for (int i = 0; i < t->tile.nwindows; i++)
    {
      if (t->tile.windows[i] == w)
        {
          memmove (&t->tile.windows[i], &t->tile.windows[i + 1],
                   (size_t)(t->tile.nwindows - i - 1) * sizeof (Window));
          t->tile.nwindows--;
          return;
        }
    }
}

static int
tile_index (Node *t, Window w)
{
  for (int i = 0; i < t->tile.nwindows; i++)
    if (t->tile.windows[i] == w)
      return i;
  return -1;
}

static void
tile_clamp_tab (Node *t)
{
  if (t->tile.active_tab >= t->tile.nwindows)
    t->tile.active_tab = t->tile.nwindows > 0 ? t->tile.nwindows - 1 : 0;
}

static void
tile_client_area (Node *t, int *cx, int *cy, int *cw, int *ch)
{
  int b = cfg.border_width, g = cfg.border_gap;
  *cx = t->x + g + b;
  *cw = t->w - 2 * (g + b);
  if (*cw < 1)
    *cw = 1;
  *ch = t->h - cfg.tab_bar_height - 2 * (g + b);
  if (*ch < 1)
    *ch = 1;
  if (cfg.statusbar_pos)
    *cy = t->y + cfg.tab_bar_height + g + b;
  else
    *cy = t->y + g + b;
}

static Node *
node_new_tile (int x, int y, int w, int h)
{
  Node *n = calloc (1, sizeof (Node));
  if (!n)
    {
      fprintf (stderr, "swm: node_new_tile: alloc failed\n");
      return NULL;
    }
  n->type = NODE_TILE;
  n->x = x;
  n->y = y;
  n->w = w;
  n->h = h;
  n->tile.win_cap = 4;
  n->tile.windows = calloc ((size_t)n->tile.win_cap, sizeof (Window));
  if (!n->tile.windows)
    {
      fprintf (stderr, "swm: node_new_tile: alloc windows failed\n");
      free (n);
      return NULL;
    }
  n->tile.tab_bar_win = None;
  n->tile.frame_win = None;
  return n;
}

static Node *
node_new_split (int horizontal, double ratio)
{
  Node *n = calloc (1, sizeof (Node));
  if (!n)
    {
      fprintf (stderr, "swm: node_new_split: alloc failed\n");
      return NULL;
    }
  n->type = NODE_SPLIT;
  n->split.horizontal = horizontal;
  n->split.ratio = ratio;
  return n;
}

static void
node_free_tree (Node *n)
{
  if (!n)
    return;
  if (n->type == NODE_SPLIT)
    {
      node_free_tree (n->split.children[0]);
      node_free_tree (n->split.children[1]);
    }
  else if (n->type == NODE_TILE)
    {
      free (n->tile.windows);
    }
  free (n);
}

static int
collect_tiles (Node *node, Node **buf, int max)
{
  if (!node || max <= 0)
    return 0;
  if (node->type == NODE_TILE)
    {
      buf[0] = node;
      return 1;
    }
  int n = collect_tiles (node->split.children[0], buf, max);
  n += collect_tiles (node->split.children[1], buf + n, max - n);
  return n;
}

static void
ws_invalidate_tiles (Workspace *w)
{
  w->tiles_dirty = 1;
}

static int
ws_get_tiles (Workspace *w, Node ***out)
{
  if (w->tiles_dirty)
    {
      w->n_cached_tiles = collect_tiles (w->root, w->cached_tiles, MAX_TILES);
      w->tiles_dirty = 0;
    }
  *out = w->cached_tiles;
  return w->n_cached_tiles;
}

static Node *
ws_find_tile (Workspace *w, Window wid)
{
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    for (int j = 0; j < tiles[i]->tile.nwindows; j++)
      if (tiles[i]->tile.windows[j] == wid)
        return tiles[i];
  return NULL;
}

static int
ws_has_windows (Workspace *w)
{
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    if (tiles[i]->tile.nwindows > 0)
      return 1;
  return 0;
}

static void
ws_recalc (Workspace *ws, Node *node)
{
  if (!node)
    {
      node = ws->root;
      node->x = 0;
      node->y = ws->tile_y;
      node->w = ws->sw;
      node->h = ws->tile_h;
    }
  if (node->type == NODE_TILE)
    return;
  Node *c0 = node->split.children[0], *c1 = node->split.children[1];
  if (node->split.horizontal)
    {
      int w0 = (int)(node->w * node->split.ratio);
      c0->x = node->x;
      c0->y = node->y;
      c0->w = w0;
      c0->h = node->h;
      c1->x = node->x + w0;
      c1->y = node->y;
      c1->w = node->w - w0;
      c1->h = node->h;
    }
  else
    {
      int h0 = (int)(node->h * node->split.ratio);
      c0->x = node->x;
      c0->y = node->y;
      c0->w = node->w;
      c0->h = h0;
      c1->x = node->x;
      c1->y = node->y + h0;
      c1->w = node->w;
      c1->h = node->h - h0;
    }
  ws_recalc (ws, c0);
  ws_recalc (ws, c1);
}

static void
ws_replace (Workspace *ws, Node *old, Node *rep)
{
  rep->parent = old->parent;
  if (!old->parent)
    {
      ws->root = rep;
    }
  else
    {
      Node *p = old->parent;
      p->split.children[(p->split.children[0] == old) ? 0 : 1] = rep;
    }
}

static double
mono_time (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int
xerr_handler (Display *d, XErrorEvent *e)
{
  (void)d;
  (void)e;
  return 0;
}

static int
xerr_detect (Display *d, XErrorEvent *e)
{
  (void)d;
  (void)e;
  wm_detected = 1;
  return 0;
}

static void
get_wm_name (Window wid, char *buf, int len)
{
  Atom type;
  int fmt;
  unsigned long ni, after;
  unsigned char *data = NULL;

  if (XGetWindowProperty (dpy, wid, a_net_wm_name, 0, 256, False, a_utf8,
                          &type, &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      snprintf (buf, len, "%.*s", (int)ni, (char *)data);
      XFree (data);
      return;
    }
  if (data)
    {
      XFree (data);
      data = NULL;
    }

  if (XGetWindowProperty (dpy, wid, XA_WM_NAME, 0, 256, False, XA_STRING,
                          &type, &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      snprintf (buf, len, "%.*s", (int)ni, (char *)data);
      XFree (data);
      return;
    }
  if (data)
    XFree (data);
  snprintf (buf, len, "?");
}

static Workspace *
ws (void)
{
  return &workspaces[cur_ws];
}

static void ensure_frame (Node *tile);
static void destroy_frame (Node *tile);
static void ensure_tab_bar (Node *tile);
static void destroy_tab_bar (Node *tile);
static void draw_tab_bar (Node *tile);
static void arrange_tile (Node *tile);
static void arrange_workspace (Workspace *w);
static void raise_floats (void);
static void hide_workspace (Workspace *w);
static void focus_tile (Node *tile);
static void focus_set_input (Node *tile);
static void send_configure_notify (Window wid);
static void bar_draw (void);
static void bar_destroy (void);
static void timebar_init (void);
static void timebar_draw (void);
static void timebar_destroy (void);
static void unmanage_window (Window wid);
static void cmd_init (void);
static void cmd_cleanup (void);
static void cmd_poll (fd_set *fds);
static void action_switch_workspace (int n);

static void
init_ewmh (void)
{
  Atom supporting = XInternAtom (dpy, "_NET_SUPPORTING_WM_CHECK", False);

  ewmh_check_win = XCreateSimpleWindow (dpy, root_win, 0, 0, 1, 1, 0, 0, 0);

  XChangeProperty (dpy, root_win, supporting, XA_WINDOW, 32, PropModeReplace,
                   (unsigned char *)&ewmh_check_win, 1);
  XChangeProperty (dpy, ewmh_check_win, supporting, XA_WINDOW, 32,
                   PropModeReplace, (unsigned char *)&ewmh_check_win, 1);

  XChangeProperty (dpy, ewmh_check_win, a_net_wm_name, a_utf8, 8,
                   PropModeReplace, (unsigned char *)"SWM", 3);
  XChangeProperty (dpy, root_win, a_net_wm_name, a_utf8, 8, PropModeReplace,
                   (unsigned char *)"SWM", 3);

  Atom net_supported = XInternAtom (dpy, "_NET_SUPPORTED", False);
  Atom supported[] = {
    a_net_wm_state,
    a_net_wm_state_fullscreen,
    a_net_wm_name,
    supporting,
  };
  XChangeProperty (dpy, root_win, net_supported, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)supported,
                   (int)(sizeof (supported) / sizeof (supported[0])));

  XFlush (dpy);
}

static void
cleanup_ewmh (void)
{
  if (ewmh_check_win != None)
    {
      XDestroyWindow (dpy, ewmh_check_win);
      ewmh_check_win = None;
    }
}

static void
fullscreen_enter (Window wid)
{
  if (ws ()->fullscreen_win != None)
    return;
  ws ()->fullscreen_win = wid;

  XChangeProperty (dpy, wid, a_net_wm_state, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)&a_net_wm_state_fullscreen, 1);

  if (status_bar_win != None)
    XUnmapWindow (dpy, status_bar_win);
  if (timebar_win != None)
    XUnmapWindow (dpy, timebar_win);
  Node **tiles;
  int n = ws_get_tiles (ws (), &tiles);
  for (int i = 0; i < n; i++)
    {
      if (tiles[i]->tile.tab_bar_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.tab_bar_win);
      if (tiles[i]->tile.frame_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.frame_win);

      for (int j = 0; j < tiles[i]->tile.nwindows; j++)
        if (tiles[i]->tile.windows[j] != wid)
          XUnmapWindow (dpy, tiles[i]->tile.windows[j]);
    }

  XMoveResizeWindow (dpy, wid, 0, 0, (unsigned)sw, (unsigned)sh);
  XRaiseWindow (dpy, wid);
  XSetInputFocus (dpy, wid, RevertToParent, CurrentTime);
  XFlush (dpy);
}

static void
fullscreen_exit (Window wid)
{
  if (ws ()->fullscreen_win == None || ws ()->fullscreen_win != wid)
    return;
  ws ()->fullscreen_win = None;

  XChangeProperty (dpy, wid, a_net_wm_state, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)NULL, 0);

  if (status_bar_win != None)
    {
      XMapWindow (dpy, status_bar_win);
      XRaiseWindow (dpy, status_bar_win);
    }
  if (timebar_win != None)
    {
      XMapWindow (dpy, timebar_win);
      XRaiseWindow (dpy, timebar_win);
    }

  arrange_workspace (ws ());
  focus_set_input (ws ()->active_tile);
  bar_draw ();
  timebar_draw ();
}

static void
fullscreen_toggle (Window wid)
{
  if (ws ()->fullscreen_win == wid)
    fullscreen_exit (wid);
  else
    fullscreen_enter (wid);
}

static void
ensure_frame (Node *tile)
{
  int g = cfg.border_gap;
  int fx = tile->x + g, fy = tile->y + g;
  int fw = tile->w - 2 * g;
  if (fw < 1)
    fw = 1;
  int fh = tile->h - 2 * g;
  if (fh < 1)
    fh = 1;
  int is_active = (tile == ws ()->active_tile);
  const char *bg = is_active ? cfg.col_border_active : cfg.col_border_inactive;

  if (tile->tile.frame_win == None)
    {
      XSetWindowAttributes swa;
      swa.background_pixel = px (bg);
      swa.override_redirect = True;
      swa.event_mask = 0;
      Window w = XCreateWindow (
          dpy, root_win, fx, fy, (unsigned)fw, (unsigned)fh, 0, depth,
          InputOutput, CopyFromParent,
          CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
      tile->tile.frame_win = w;
      set_add (frame_wins, &n_frame_wins, w);
      XLowerWindow (dpy, w);
      XMapWindow (dpy, w);
    }
  else
    {
      Window w = tile->tile.frame_win;
      XMoveResizeWindow (dpy, w, fx, fy, (unsigned)fw, (unsigned)fh);
      XSetWindowBackground (dpy, w, px (bg));
      XClearWindow (dpy, w);
      XLowerWindow (dpy, w);
      XMapWindow (dpy, w);
    }
}

static void
destroy_frame (Node *tile)
{
  if (tile->tile.frame_win != None)
    {
      Window w = tile->tile.frame_win;
      XUnmapWindow (dpy, w);
      XDestroyWindow (dpy, w);
      set_remove (frame_wins, &n_frame_wins, w);
      tile->tile.frame_win = None;
    }
}

static void
bar_init (void)
{
  int by = cfg.statusbar_pos ? 0 : (sh - cfg.statusbar_height);
  XSetWindowAttributes swa;
  swa.background_pixel = px (cfg.col_statusbar_bg);
  swa.override_redirect = True;
  swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask;
  status_bar_win = XCreateWindow (
      dpy, root_win, 0, by, (unsigned)sw, cfg.statusbar_height, 0, depth,
      InputOutput, CopyFromParent,
      CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XRaiseWindow (dpy, status_bar_win);
  XMapWindow (dpy, status_bar_win);
  bar_draw ();
}

static void
bar_draw (void)
{
  if (status_bar_win == None)
    return;
  int w = sw, h = cfg.statusbar_height;

  Pixmap pm = XCreatePixmap (dpy, status_bar_win, (unsigned)w, (unsigned)h,
                             (unsigned)depth);

  XSetForeground (dpy, draw_gc, px (cfg.col_statusbar_bg));
  XFillRectangle (dpy, pm, draw_gc, 0, 0, (unsigned)w, (unsigned)h);

  int pad = 6, ws_w = 22, ws_gap = 3, text_y = h / 2 + 4;

  int x = pad;
  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      int is_cur = (i == cur_ws);
      int has_w = ws_has_windows (&workspaces[i]);
      const char *bg_col, *fg_col;
      if (is_cur)
        {
          bg_col = cfg.col_statusbar_ws_active;
          fg_col = cfg.col_statusbar_ws_fg_act;
        }
      else if (has_w)
        {
          bg_col = cfg.col_statusbar_ws_occupied;
          fg_col = cfg.col_statusbar_ws_fg_act;
        }
      else
        {
          bg_col = cfg.col_statusbar_ws_inactive;
          fg_col = cfg.col_statusbar_ws_fg_inact;
        }

      XSetForeground (dpy, draw_gc, px (bg_col));
      XFillRectangle (dpy, pm, draw_gc, x, 2, (unsigned)ws_w,
                      (unsigned)(h - 4));

      XSetForeground (dpy, draw_gc, px (fg_col));
      XSetFont (dpy, draw_gc, font->fid);
      char label[2] = { (char)('1' + i), '\0' };
      XDrawString (dpy, pm, draw_gc, x + ws_w / 2 - 3, text_y, label, 1);

      x += ws_w + ws_gap;
    }

  time_t now = time (NULL);
  struct tm *tm = localtime (&now);
  char status[64];
  strftime (status, sizeof (status), "%a %b %d  %H:%M", tm);

  int text_w = (int)strlen (status) * 7;
  int tx = w - text_w - pad;
  XSetForeground (dpy, draw_gc, px (cfg.col_statusbar_fg));
  XSetFont (dpy, draw_gc, font->fid);
  XDrawString (dpy, pm, draw_gc, tx, text_y, status, (int)strlen (status));

  XCopyArea (dpy, pm, status_bar_win, draw_gc, 0, 0, (unsigned)w, (unsigned)h,
             0, 0);
  XFreePixmap (dpy, pm);
  XFlush (dpy);
}

static void
bar_handle_click (XEvent *ev)
{
  int pad = 6, ws_w = 22, ws_gap = 3;
  int cx = ev->xbutton.x;
  int x = pad;
  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      if (cx >= x && cx < x + ws_w)
        {

          action_switch_workspace (i);
          return;
        }
      x += ws_w + ws_gap;
    }
}

static void
bar_destroy (void)
{
  if (status_bar_win != None)
    {
      XUnmapWindow (dpy, status_bar_win);
      XDestroyWindow (dpy, status_bar_win);
      status_bar_win = None;
    }
}

static void
timebar_init (void)
{
  int by;
  if (cfg.timebar_pos)
    by = cfg.statusbar_pos ? cfg.statusbar_height : 0;
  else
    by = cfg.statusbar_pos ? sh - cfg.timebar_height
                           : sh - cfg.statusbar_height - cfg.timebar_height;
  XSetWindowAttributes swa;
  swa.background_pixel = px (cfg.col_timebar_bg);
  swa.override_redirect = True;
  swa.event_mask = ExposureMask;
  timebar_win
      = XCreateWindow (dpy, root_win, 0, by, (unsigned)sw, cfg.timebar_height,
                       0, depth, InputOutput, CopyFromParent,
                       CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XRaiseWindow (dpy, timebar_win);
  XMapWindow (dpy, timebar_win);
  timebar_draw ();
}

static void
timebar_draw (void)
{
  if (timebar_win == None)
    return;
  int w = sw, h = cfg.timebar_height;

  Pixmap pm = XCreatePixmap (dpy, timebar_win, (unsigned)w, (unsigned)h,
                             (unsigned)depth);

  XSetForeground (dpy, draw_gc, px (cfg.col_timebar_bg));
  XFillRectangle (dpy, pm, draw_gc, 0, 0, (unsigned)w, (unsigned)h);

  time_t now_t = time (NULL);
  struct tm *tm = localtime (&now_t);
  int hours24 = tm->tm_hour;
  int minutes = tm->tm_min;
  int seconds = tm->tm_sec;
  //int hours12 = (hours24 == 0) ? 12 : (hours24 > 12 ? hours24 - 12 : hours24);

  int total_secs_in_hour = minutes * 60 + seconds;
  int hex_segment = total_secs_in_hour / 225;
  if (hex_segment > 15)
    hex_segment = 15;
  double progress_in_seg = (total_secs_in_hour % 225) / 225.0;

  double hour_pct = ((hours24 * 60) + minutes) / (24.0 * 60.0);
  double hex_pct = total_secs_in_hour / 3600.0;
  double seg_pct = progress_in_seg;

  //const char *hex_chars = "0123456789ABCDEF";

  //int label_w = 16;
  int gap = 2;
  int bar_y = 1;
  int bar_h = h - 2;
  if (bar_h < 1)
    bar_h = 1;

  int zone_w = w / 3;

  {
    int x0 = 0;
//    int fill_x = x0 + label_w + gap;
//    int fill_w = zone_w - label_w - gap - gap;
    int fill_x = x0 + gap;
    int fill_w = zone_w - gap;
    if (fill_w < 1)
      fill_w = 1;

    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_track));
    XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fill_w,
                    (unsigned)bar_h);

    int fw = (int)(fill_w * hour_pct);
    if (fw > 0)
      {
        XSetForeground (dpy, draw_gc, px (cfg.col_timebar_hour));
        XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fw,
                        (unsigned)bar_h);
      }

//    char lbl[12];
//    snprintf (lbl, sizeof (lbl), "%d", hours12);
//    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_label));
//    XSetFont (dpy, draw_gc, font->fid);
//    XDrawString (dpy, pm, draw_gc, x0 + 2, bar_y + bar_h - 1, lbl,
                 //(int)strlen (lbl));
  }

  {
    int x0 = zone_w;
//    int fill_x = x0 + label_w + gap;
//    int fill_w = zone_w - label_w - gap - gap;
    int fill_x = x0 + gap;
    int fill_w = zone_w - gap;
    if (fill_w < 1)
      fill_w = 1;

    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_track));
    XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fill_w,
                    (unsigned)bar_h);

    int fw = (int)(fill_w * hex_pct);
    if (fw > 0)
      {
        XSetForeground (dpy, draw_gc, px (cfg.col_timebar_hex));
        XFillRectangle (dpy, pm, draw_gc, fill_x, bar_y, (unsigned)fw,
                        (unsigned)bar_h);
      }

//    char lbl[2] = { hex_chars[hex_segment], '\0' };
//    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_label));
//    XSetFont (dpy, draw_gc, font->fid);
//    XDrawString (dpy, pm, draw_gc, x0 + 2, bar_y + bar_h - 1, lbl, 1);
  }

  {
    int x0 = zone_w * 2;
    int fill_w = w - x0 - gap;
    if (fill_w < 1)
      fill_w = 1;

    XSetForeground (dpy, draw_gc, px (cfg.col_timebar_track));
    XFillRectangle (dpy, pm, draw_gc, x0 + gap, bar_y, (unsigned)fill_w,
                    (unsigned)bar_h);

    int fw = (int)(fill_w * seg_pct);
    if (fw > 0)
      {
        XSetForeground (dpy, draw_gc, px (cfg.col_timebar_seg));
        XFillRectangle (dpy, pm, draw_gc, x0 + gap, bar_y, (unsigned)fw,
                        (unsigned)bar_h);
      }
  }

  XCopyArea (dpy, pm, timebar_win, draw_gc, 0, 0, (unsigned)w, (unsigned)h, 0,
             0);
  XFreePixmap (dpy, pm);
  XFlush (dpy);
}

static void
timebar_destroy (void)
{
  if (timebar_win != None)
    {
      XUnmapWindow (dpy, timebar_win);
      XDestroyWindow (dpy, timebar_win);
      timebar_win = None;
    }
}

static void
ensure_tab_bar (Node *tile)
{
  int g = cfg.border_gap, b = cfg.border_width;
  int bx = tile->x + g + b;
  int bw = tile->w - 2 * (g + b);
  if (bw < 1)
    bw = 1;
  int bh = cfg.tab_bar_height - b;
  if (bh < 1)
    bh = 1;
  int by;
  if (cfg.statusbar_pos)
    by = tile->y + g + b;
  else
    by = tile->y + tile->h - g - cfg.tab_bar_height;
  int is_active = (tile == ws ()->active_tile);
  const char *bar_bg = is_active ? cfg.col_border_active : cfg.col_tab_bar_bg;

  if (tile->tile.tab_bar_win == None)
    {
      XSetWindowAttributes swa;
      swa.background_pixel = px (bar_bg);
      swa.override_redirect = True;
      swa.event_mask = ExposureMask | ButtonPressMask;
      Window w = XCreateWindow (
          dpy, root_win, bx, by, (unsigned)bw, (unsigned)bh, 0, depth,
          InputOutput, CopyFromParent,
          CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
      tile->tile.tab_bar_win = w;
      set_add (tab_bars, &n_tab_bars, w);
      XMapWindow (dpy, w);
    }
  else
    {
      Window w = tile->tile.tab_bar_win;
      XMoveResizeWindow (dpy, w, bx, by, (unsigned)bw, (unsigned)bh);
      XSetWindowBackground (dpy, w, px (bar_bg));
      XMapWindow (dpy, w);
    }
}

static void
destroy_tab_bar (Node *tile)
{
  if (tile->tile.tab_bar_win != None)
    {
      Window w = tile->tile.tab_bar_win;
      XUnmapWindow (dpy, w);
      XDestroyWindow (dpy, w);
      set_remove (tab_bars, &n_tab_bars, w);
      tile->tile.tab_bar_win = None;
    }
}

static void
draw_tab_bar (Node *tile)
{
  if (tile->tile.tab_bar_win == None)
    return;
  Window win = tile->tile.tab_bar_win;
  XWindowAttributes wa;
  XGetWindowAttributes (dpy, win, &wa);
  int w = wa.width, h = wa.height;
  if (w < 1 || h < 1)
    return;

  int is_active = (tile == ws ()->active_tile);
  const char *bar_bg = is_active ? cfg.col_border_active : cfg.col_tab_bar_bg;

  Pixmap pm
      = XCreatePixmap (dpy, win, (unsigned)w, (unsigned)h, (unsigned)depth);

  XSetForeground (dpy, draw_gc, px (bar_bg));
  XFillRectangle (dpy, pm, draw_gc, 0, 0, (unsigned)w, (unsigned)h);

  int n = tile->tile.nwindows;
  if (n == 0)
    {
      if (is_active)
        {
          XSetForeground (dpy, draw_gc, px ("#FFFFFF"));
          //XSetFont (dpy, draw_gc, font->fid);
          //XDrawString (dpy, pm, draw_gc, 6, h / 2 + 4, "...", 3);
        }
    }
  else
    {
      int tab_w = w / n;
      if (tab_w < 1)
        tab_w = 1;
      for (int i = 0; i < n; i++)
        {
          int is_tab_act = (i == tile->tile.active_tab);
          const char *bg, *fg;
          if (is_tab_act)
            {
              bg = is_active ? cfg.col_tab_active_bg
                             : cfg.col_tab_active_bg_dim;
              fg = is_active ? cfg.col_tab_active_fg
                             : cfg.col_tab_active_fg_dim;
            }
          else
            {
              bg = cfg.col_tab_inactive_bg;
              fg = cfg.col_tab_inactive_fg;
            }
          int x0 = i * tab_w;
          int tw = (i < n - 1) ? tab_w : (w - x0);

          XSetForeground (dpy, draw_gc, px (bg));
          XFillRectangle (dpy, pm, draw_gc, x0, 0, (unsigned)tw, (unsigned)h);

          if (i < n - 1)
            {
              XSetForeground (dpy, draw_gc, px (bar_bg));
              XFillRectangle (dpy, pm, draw_gc, x0 + tw - 1, 0, 1,
                              (unsigned)h);
            }

//          char title[128];
//          get_wm_name (tile->tile.windows[i], title, sizeof (title));
//          int tlen = (int)strlen (title);
//          if (tlen > 20)
//            {
//              title[18] = '.';
//              title[19] = '.';
//              title[20] = '\0';
//              tlen = 20;
//            }
//
          XSetForeground (dpy, draw_gc, px (fg));
//          XSetFont (dpy, draw_gc, font->fid);
//          XDrawString (dpy, pm, draw_gc, x0 + 6, h / 2 + 4, title, tlen);
        }
    }

  XCopyArea (dpy, pm, win, draw_gc, 0, 0, (unsigned)w, (unsigned)h, 0, 0);
  XFreePixmap (dpy, pm);
  XFlush (dpy);
}
 static void
raise_floats (void)
{
  for (int i = 0; i < n_float_wins; i++)
    XRaiseWindow (dpy, float_wins[i]);
}

static void
arrange_tile (Node *tile)
{
  ensure_frame (tile);
  ensure_tab_bar (tile);

  int cx, cy, cw, ch;
  tile_client_area (tile, &cx, &cy, &cw, &ch);

  for (int i = 0; i < tile->tile.nwindows; i++)
    {
      Window w = tile->tile.windows[i];
      if (i == tile->tile.active_tab)
        {
          XMoveResizeWindow (dpy, w, cx, cy, (unsigned)cw, (unsigned)ch);
          XRaiseWindow (dpy, w);
          XMapWindow (dpy, w);
        }
      else
        {
          XUnmapWindow (dpy, w);
        }
    }
  draw_tab_bar (tile);
  raise_floats ();
}

static void
arrange_workspace (Workspace *w)
{
  ws_recalc (w, NULL);
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    arrange_tile (tiles[i]);
}

static void
hide_workspace (Workspace *w)
{
  Node **tiles;
  int n = ws_get_tiles (w, &tiles);
  for (int i = 0; i < n; i++)
    {
      for (int j = 0; j < tiles[i]->tile.nwindows; j++)
        XUnmapWindow (dpy, tiles[i]->tile.windows[j]);
      if (tiles[i]->tile.tab_bar_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.tab_bar_win);
      if (tiles[i]->tile.frame_win != None)
        XUnmapWindow (dpy, tiles[i]->tile.frame_win);
    }
}

static void
focus_tile (Node *tile)
{
  focus_set_input(tile);
  Node **tiles;
  int n = ws_get_tiles (ws (), &tiles);
  for (int i = 0; i < n; i++)
    {
      ensure_frame (tiles[i]);
      ensure_tab_bar (tiles[i]);
      draw_tab_bar (tiles[i]);
      raise_floats ();
    }
}

static void
focus_set_input (Node *tile)
{
  ws ()->active_tile = tile;
  if (tile->tile.nwindows > 0)
    {
      Window w = tile->tile.windows[tile->tile.active_tab];
      XSetInputFocus (dpy, w, RevertToParent, CurrentTime);
      XRaiseWindow (dpy, w);
    }
  raise_floats ();
}

static Node *
find_adjacent (const char *dir)
{
  Node *cur = ws ()->active_tile;
  int cxc = cur->x + cur->w / 2, cyc = cur->y + cur->h / 2;
  Node *best = NULL;
  long best_dist = 0x7FFFFFFF;

  Node **tiles;
  int n = ws_get_tiles (ws (), &tiles);
  for (int i = 0; i < n; i++)
    {
      if (tiles[i] == cur)
        continue;
      int tx = tiles[i]->x + tiles[i]->w / 2,
          ty = tiles[i]->y + tiles[i]->h / 2;
      int dx = tx - cxc, dy = ty - cyc;
      int ok = 0;
      if (strcmp (dir, "left") == 0 && dx < -5)
        ok = 1;
      else if (strcmp (dir, "right") == 0 && dx > 5)
        ok = 1;
      else if (strcmp (dir, "up") == 0 && dy < -5)
        ok = 1;
      else if (strcmp (dir, "down") == 0 && dy > 5)
        ok = 1;
      if (ok)
        {
          long dist = (long)dx * dx + (long)dy * dy;
          if (dist < best_dist)
            {
              best_dist = dist;
              best = tiles[i];
            }
        }
    }
  return best;
}

static void
spawn (const char *cmd)
{
  pid_t pid = fork ();
  if (pid < 0)
    {
      fprintf (stderr, "swm: fork failed: %s\n", strerror (errno));
      return;
    }
  if (pid == 0)
    {
      setsid ();

      if (dpy)
        close (ConnectionNumber (dpy));
      execl ("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit (1);
    }
}

static int
should_float (Window wid)
{
  Atom type;
  int fmt;
  unsigned long ni, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty (dpy, wid, a_wm_type, 0, 32, False, XA_ATOM, &type,
                          &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      Atom *atoms = (Atom *)data;
      for (unsigned long i = 0; i < ni; i++)
        {
          if (atoms[i] == a_wm_type_splash || atoms[i] == a_wm_type_tooltip
              || atoms[i] == a_wm_type_notification
              || atoms[i] == a_wm_type_popup_menu || atoms[i] == a_wm_type_dock
              || atoms[i] == a_wm_type_dialog)
            {
              XFree (data);
              return 1;
            }
        }
      XFree (data);
    }
  else if (data)
    {
      XFree (data);
    }

  Window transient_for = None;
  if (XGetTransientForHint (dpy, wid, &transient_for) && transient_for != None)
    return 1;

  return 0;
}

static void
manage_window (Window wid)
{
  if (managed_find (wid) >= 0)
    return;
  if (set_contains (tab_bars, n_tab_bars, wid))
    return;
  if (set_contains (frame_wins, n_frame_wins, wid))
    return;
  if (wid == timebar_win || wid == status_bar_win)
    return;

  XWindowAttributes wa;
  if (!XGetWindowAttributes (dpy, wid, &wa))
    return;
  if (wa.override_redirect)
    return;

  if (should_float (wid))
    {
      if (wa.x <= 0 && wa.y <= 0 && wa.width < sw && wa.height < sh)
        {
          int nx = (sw - wa.width) / 2;
          int ny = (sh - wa.height) / 2;
          if (nx < 0)
            nx = 0;
          if (ny < 0)
            ny = 0;
          XMoveWindow (dpy, wid, nx, ny);
        }
      XSelectInput (dpy, wid, StructureNotifyMask);
      XMapWindow (dpy, wid);
      XRaiseWindow (dpy, wid);
      set_add (float_wins, &n_float_wins, wid);
      XSetInputFocus (dpy, wid, RevertToParent, CurrentTime);
      XFlush (dpy);
      return;
    }

  XSetWindowAttributes swa;
  swa.event_mask = StructureNotifyMask | PropertyChangeMask;
  XSetWindowBorderWidth(dpy, wid, 0);
  XChangeWindowAttributes (dpy, wid, CWEventMask, &swa);

  Node *tile = ws ()->active_tile;
  tile_add (tile, wid);
  tile->tile.prev_active_tab = tile->tile.active_tab;
  tile->tile.active_tab = tile->tile.nwindows - 1;
  managed_add (wid, cur_ws);

  arrange_tile (tile);
  send_configure_notify (wid);
  focus_set_input (tile);
  bar_draw ();

  {
    Atom type;
    int fmt;
    unsigned long ni, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty (dpy, wid, a_net_wm_state, 0, 32, False, XA_ATOM,
                            &type, &fmt, &ni, &after, &data)
            == Success
        && data && ni > 0)
      {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < ni; i++)
          {
            if (atoms[i] == a_net_wm_state_fullscreen)
              {
                XFree (data);
                fullscreen_enter (wid);
                return;
              }
          }
        XFree (data);
      }
    else if (data)
      {
        XFree (data);
      }
  }
}

static void
unmanage_window (Window wid)
{
  int idx = managed_find (wid);
  if (idx < 0)
    return;
  int ws_idx = managed[idx].ws;
  managed_del (wid);

  if (workspaces[ws_idx].fullscreen_win == wid)
    {
      workspaces[ws_idx].fullscreen_win = None;
      if (status_bar_win != None)
        {
          XMapWindow (dpy, status_bar_win);
          XRaiseWindow (dpy, status_bar_win);
        }
      if (timebar_win != None)
        {
          XMapWindow (dpy, timebar_win);
          XRaiseWindow (dpy, timebar_win);
        }
    }

  Workspace *w = &workspaces[ws_idx];
  Node *tile = ws_find_tile (w, wid);
  if (tile)
    {
      int closed_idx = tile_index (tile, wid);
      int was_active = (closed_idx == tile->tile.active_tab);
      tile_remove (tile, wid);

      if (was_active && tile->tile.nwindows > 0)
        {
          int prev = tile->tile.prev_active_tab;
          if (prev > closed_idx)
            prev--;
          if (prev < 0)
            prev = 0;
          if (prev >= tile->tile.nwindows)
            prev = tile->tile.nwindows - 1;
          tile->tile.active_tab = prev;
        }
      else
        {
          tile_clamp_tab (tile);
        }
      if (ws_idx == cur_ws)
        {
        arrange_tile (tile);
        if (tile->tile.nwindows > 0)
          focus_set_input (tile);
        }
    }
  bar_draw ();
}

static void
action_switch_workspace (int n)
{
  if (n == cur_ws)
    return;
  if (ws ()->fullscreen_win != None)
    fullscreen_exit (ws ()->fullscreen_win);
  hide_workspace (ws ());
  cur_ws = n;
  arrange_workspace (ws ());
  focus_set_input (ws ()->active_tile);
  bar_draw ();
}

static void
action_next_workspace (void)
{
  action_switch_workspace ((cur_ws + 1) % NUM_WORKSPACES);
}

static void
action_prev_workspace (void)
{
  action_switch_workspace ((cur_ws + NUM_WORKSPACES - 1) % NUM_WORKSPACES);
}

static void
action_send_to_workspace (int n)
{
  if (n == cur_ws)
    return;
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows == 0)
      focus_set_input(tile);
  Window wid = tile->tile.windows[tile->tile.active_tab];

  tile_remove (tile, wid);
  tile_clamp_tab (tile);

  Workspace *tw = &workspaces[n];
  tile_add (tw->active_tile, wid);
  tw->active_tile->tile.active_tab = tw->active_tile->tile.nwindows - 1;
  managed_set_ws (wid, n);

  XUnmapWindow (dpy, wid);
  arrange_tile (tile);
  bar_draw ();
}

static void
action_close_window (void)
{
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows == 0)
    return;
  Window wid = tile->tile.windows[tile->tile.active_tab];

  Atom type;
  int fmt;
  unsigned long ni, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty (dpy, wid, a_wm_protocols, 0, 32, False, XA_ATOM,
                          &type, &fmt, &ni, &after, &data)
          == Success
      && data && ni > 0)
    {
      Atom *atoms = (Atom *)data;
      int found = 0;
      for (unsigned long i = 0; i < ni; i++)
        if (atoms[i] == a_wm_delete)
          {
            found = 1;
            break;
          }
      XFree (data);
      if (found)
        {
          XEvent ev;
          memset (&ev, 0, sizeof (ev));
          ev.xclient.type = ClientMessage;
          ev.xclient.window = wid;
          ev.xclient.message_type = a_wm_protocols;
          ev.xclient.format = 32;
          ev.xclient.data.l[0] = (long)a_wm_delete;
          ev.xclient.data.l[1] = CurrentTime;
          XSendEvent (dpy, wid, False, 0, &ev);
          XFlush (dpy);
          return;
        }
    }
  else if (data)
    {
      XFree (data);
    }

  XDestroyWindow (dpy, wid);
}

static void
action_split (int horizontal, int move_window)
{
  Node *tile = ws ()->active_tile;
  Node *sp = node_new_split (horizontal, 0.5);
  if (!sp)
    {
      fprintf (stderr, "swm: action_split: alloc split failed\n");
      return;
    }
  sp->x = tile->x;
  sp->y = tile->y;
  sp->w = tile->w;
  sp->h = tile->h;
  Node *sibling = node_new_tile (0, 0, 1, 1);
  if (!sibling)
    {
      fprintf (stderr, "swm: action_split: alloc tile failed\n");
      free (sp);
      return;
    }

  ws_replace (ws (), tile, sp);
  sp->split.children[0] = tile;
  tile->parent = sp;
  sp->split.children[1] = sibling;
  sibling->parent = sp;

  if (move_window && tile->tile.nwindows > 0)
    {
      Window wid = tile->tile.windows[tile->tile.active_tab];
      tile_remove (tile, wid);
      tile_clamp_tab (tile);
      tile_add (sibling, wid);
      sibling->tile.active_tab = 0;
      ws ()->active_tile = sibling;
    }
  else
    {
      ws ()->active_tile = tile;
    }
  ws_invalidate_tiles (ws ());
  arrange_workspace (ws ());
}

static void
action_remove_split (void)
{
  Node *tile = ws ()->active_tile;
  Node *parent = tile->parent;
  if (!parent || parent->type != NODE_SPLIT)
    return;

  int idx = (parent->split.children[0] == tile) ? 0 : 1;
  Node *sibling = parent->split.children[1 - idx];

  Node *stiles[MAX_TILES];
  int sn = collect_tiles (sibling, stiles, MAX_TILES);
  for (int i = 0; i < sn; i++)
    {
      for (int j = 0; j < stiles[i]->tile.nwindows; j++)
        {
          Window wid = stiles[i]->tile.windows[j];
          if (!tile_has (tile, wid))
            tile_add (tile, wid);
          managed_set_ws (wid, cur_ws);
        }
      destroy_tab_bar (stiles[i]);
      destroy_frame (stiles[i]);
    }

  destroy_tab_bar (tile);
  destroy_frame (tile);

  tile->parent = parent->parent;
  if (!parent->parent)
    {
      ws ()->root = tile;
    }
  else
    {
      Node *gp = parent->parent;
      int gi = (gp->split.children[0] == parent) ? 0 : 1;
      gp->split.children[gi] = tile;
    }

  tile_clamp_tab (tile);

  ws ()->active_tile = tile;

  node_free_tree (sibling);
  parent->split.children[0] = NULL;
  parent->split.children[1] = NULL;
  free (parent);

  ws_invalidate_tiles (ws ());
  arrange_workspace (ws ());
}

static void
action_next_tab (void)
{
  if (ws ()->fullscreen_win != None)
    fullscreen_exit (ws ()->fullscreen_win);
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows > 1)
    {
      tile->tile.active_tab
          = (tile->tile.active_tab + 1) % tile->tile.nwindows;
      arrange_tile (tile);
      focus_set_input (tile);
    }
}

static void
action_prev_tab (void)
{
  if (ws ()->fullscreen_win != None)
    fullscreen_exit (ws ()->fullscreen_win);
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows > 1)
    {
      tile->tile.active_tab = (tile->tile.active_tab + tile->tile.nwindows - 1)
                              % tile->tile.nwindows;
      arrange_tile (tile);
      focus_set_input (tile);
    }
}

static void
action_move_tab_forward (void)
{
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows < 2)
    return;
  int i = tile->tile.active_tab;
  int j = (i + 1) % tile->tile.nwindows;
  Window tmp = tile->tile.windows[i];
  tile->tile.windows[i] = tile->tile.windows[j];
  tile->tile.windows[j] = tmp;
  tile->tile.active_tab = j;
  arrange_tile (tile);
}

static void
action_move_tab_backward (void)
{
  Node *tile = ws ()->active_tile;
  if (tile->tile.nwindows < 2)
    return;
  int i = tile->tile.active_tab;
  int j = (i + tile->tile.nwindows - 1) % tile->tile.nwindows;
  Window tmp = tile->tile.windows[i];
  tile->tile.windows[i] = tile->tile.windows[j];
  tile->tile.windows[j] = tmp;
  tile->tile.active_tab = j;
  arrange_tile (tile);
}

static void
action_focus_direction (const char *dir)
{
  Node *adj = find_adjacent (dir);
  if (adj)
    {
      focus_tile (adj);

    }
}

static void
action_move_window_direction (const char *dir)
{
  Node *src = ws ()->active_tile;
  if (src->tile.nwindows == 0)
    return;
  Node *dst = find_adjacent (dir);
  if (!dst)
    return;

  Window wid = src->tile.windows[src->tile.active_tab];
  tile_remove (src, wid);
  tile_clamp_tab (src);

  tile_add (dst, wid);
  dst->tile.active_tab = dst->tile.nwindows - 1;

  arrange_tile (src);
  arrange_tile (dst);
  ws ()->active_tile = dst;
  focus_tile (dst);

}

static void
action_quit (void)
{
  running = 0;
}

static void
cmd_init (void)
{

  const char *xdg = getenv ("XDG_RUNTIME_DIR");
  if (xdg && strlen (xdg) < sizeof (cmd_sock_path) - 16)
    snprintf (cmd_sock_path, sizeof (cmd_sock_path), "%s/swm.sock", xdg);
  else
    snprintf (cmd_sock_path, sizeof (cmd_sock_path), "%s", CMD_SOCK_PATH);

  unlink (cmd_sock_path);

  cmd_listen_fd
      = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (cmd_listen_fd < 0)
    {
      fprintf (stderr, "swm: cmd socket: %s\n", strerror (errno));
      cmd_sock_path[0] = '\0';
      return;
    }

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  {
    size_t plen = strlen (cmd_sock_path);
    if (plen >= sizeof (addr.sun_path))
      plen = sizeof (addr.sun_path) - 1;
    memcpy (addr.sun_path, cmd_sock_path, plen);
    addr.sun_path[plen] = '\0';
  }

  if (bind (cmd_listen_fd, (struct sockaddr *)&addr, sizeof (addr)) < 0)
    {
      fprintf (stderr, "swm: cmd bind(%s): %s\n", cmd_sock_path,
               strerror (errno));
      close (cmd_listen_fd);
      cmd_listen_fd = -1;
      cmd_sock_path[0] = '\0';
      return;
    }

  if (listen (cmd_listen_fd, 4) < 0)
    {
      fprintf (stderr, "swm: cmd listen: %s\n", strerror (errno));
      close (cmd_listen_fd);
      cmd_listen_fd = -1;
      unlink (cmd_sock_path);
      cmd_sock_path[0] = '\0';
      return;
    }

  n_cmd_clients = 0;
  fprintf (stderr, "swm: listening on %s\n", cmd_sock_path);
}

static void
cmd_cleanup (void)
{
  for (int i = 0; i < n_cmd_clients; i++)
    close (cmd_clients[i].fd);
  n_cmd_clients = 0;

  if (cmd_listen_fd >= 0)
    {
      close (cmd_listen_fd);
      cmd_listen_fd = -1;
    }

  if (cmd_sock_path[0])
    {
      unlink (cmd_sock_path);
      cmd_sock_path[0] = '\0';
    }
}

static void
cmd_drop_client (int idx)
{
  close (cmd_clients[idx].fd);
  cmd_clients[idx] = cmd_clients[--n_cmd_clients];
}

static void
cmd_reply (int fd, const char *msg)
{
  size_t len = strlen (msg);
  char buf[CMD_BUF_SIZE];
  if (len >= sizeof (buf) - 1)
    len = sizeof (buf) - 2;
  memcpy (buf, msg, len);
  buf[len] = '\n';

  (void)write (fd, buf, len + 1);
}

static void
cmd_dispatch (int client_fd, char *line)
{

  while (*line == ' ' || *line == '\t')
    line++;
  char *end = line + strlen (line) - 1;
  while (end > line && (*end == ' ' || *end == '\t' || *end == '\r'))
    *end-- = '\0';
  if (*line == '\0')
    return;

  if (strcmp (line, "help") == 0)
    {
      cmd_reply (client_fd,
                 "exec <cmd> | split h|v | split-move h|v | unsplit");
      cmd_reply (client_fd, "close | quit | reload | fullscreen");
      cmd_reply (client_fd, "set <key> <value> | get <key>");
      cmd_reply (client_fd,
                 "next-tab | prev-tab | move-tab-fwd | move-tab-bwd");
      cmd_reply (client_fd,
                 "next-ws | prev-ws | workspace <1-9> | send <1-9>");
      cmd_reply (client_fd,
                 "focus <dir> | move <dir>  (dir: left|right|up|down)");
      cmd_reply (
          client_fd,
          "query ws | query win-count | query win-title | query layout");
      return;
    }

  if (strncmp (line, "exec ", 5) == 0)
    {
      spawn (line + 5);
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strcmp (line, "split h") == 0)
    {
      action_split (1, 0);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "split v") == 0)
    {
      action_split (0, 0);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "split-move h") == 0)
    {
      action_split (1, 1);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "split-move v") == 0)
    {
      action_split (0, 1);
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "unsplit") == 0)
    {
      action_remove_split ();
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strcmp (line, "close") == 0)
    {
      action_close_window ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "quit") == 0)
    {
      cmd_reply (client_fd, "ok");
      action_quit ();
      return;
    }
  if (strcmp (line, "reload") == 0)
    {
      reload_pending = 1;
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strncmp (line, "set ", 4) == 0)
    {
      char *kv = line + 4;
      while (*kv == ' ')
        kv++;
      char *sp = strchr (kv, ' ');
      if (sp)
        {
          *sp = '\0';
          char *val = sp + 1;
          while (*val == ' ')
            val++;
          strip_quotes (val);
          if (cfg_set_kv (kv, val))
            {
              apply_pending = 1;
              cmd_reply (client_fd, "ok");
            }
          else
            {
              cmd_reply (client_fd, "err: unknown key");
            }
        }
      else
        {
          cmd_reply (client_fd, "err: set <key> <value>");
        }
      return;
    }

  if (strncmp (line, "get ", 4) == 0)
    {
      const char *key = line + 4;
      while (*key == ' ')
        key++;
      char r[256];

      for (int i = 0; num_map[i].name; i++)
        {
          if (strcmp (key, num_map[i].name) == 0)
            {
              if (num_map[i].kind == CFG_DOUBLE)
                snprintf (r, sizeof (r), "%.2f",
                          *(double *)((const char *)&cfg + num_map[i].off));
              else
                snprintf (r, sizeof (r), "%d",
                          *(int *)((const char *)&cfg + num_map[i].off));
              cmd_reply (client_fd, r);
              return;
            }
        }

      for (int i = 0; col_map[i].name; i++)
        {
          if (strcmp (key, col_map[i].name) == 0)
            {
              cmd_reply (client_fd, (const char *)&cfg + col_map[i].off);
              return;
            }
        }

      cmd_reply (client_fd, "err: unknown key");
      return;
    }

  if (strcmp (line, "next-tab") == 0)
    {
      action_next_tab ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "prev-tab") == 0)
    {
      action_prev_tab ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "move-tab-fwd") == 0)
    {
      action_move_tab_forward ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "move-tab-bwd") == 0)
    {
      action_move_tab_backward ();
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strcmp (line, "next-ws") == 0)
    {
      action_next_workspace ();
      cmd_reply (client_fd, "ok");
      return;
    }
  if (strcmp (line, "prev-ws") == 0)
    {
      action_prev_workspace ();
      cmd_reply (client_fd, "ok");
      return;
    }

  if (strncmp (line, "workspace ", 10) == 0)
    {
      int n = atoi (line + 10) - 1;
      if (n >= 0 && n < NUM_WORKSPACES)
        {
          action_switch_workspace (n);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: workspace 1-9");
        }
      return;
    }
  if (strncmp (line, "send ", 5) == 0)
    {
      int n = atoi (line + 5) - 1;
      if (n >= 0 && n < NUM_WORKSPACES)
        {
          action_send_to_workspace (n);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: workspace 1-9");
        }
      return;
    }

  if (strncmp (line, "focus ", 6) == 0)
    {
      const char *d = line + 6;
      if (strcmp (d, "left") == 0 || strcmp (d, "right") == 0
          || strcmp (d, "up") == 0 || strcmp (d, "down") == 0)
        {
          action_focus_direction (d);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: direction left|right|up|down");
        }
      return;
    }
  if (strncmp (line, "move ", 5) == 0)
    {
      const char *d = line + 5;
      if (strcmp (d, "left") == 0 || strcmp (d, "right") == 0
          || strcmp (d, "up") == 0 || strcmp (d, "down") == 0)
        {
          action_move_window_direction (d);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: direction left|right|up|down");
        }
      return;
    }

  if (strcmp (line, "fullscreen") == 0)
    {
      Node *tile = ws ()->active_tile;
      if (tile->tile.nwindows > 0)
        {
          fullscreen_toggle (tile->tile.windows[tile->tile.active_tab]);
          cmd_reply (client_fd, "ok");
        }
      else
        {
          cmd_reply (client_fd, "err: no window");
        }
      return;
    }

  if (strcmp (line, "query ws") == 0)
    {
      char r[16];
      snprintf (r, sizeof (r), "%d", cur_ws + 1);
      cmd_reply (client_fd, r);
      return;
    }
  if (strcmp (line, "query win-count") == 0)
    {
      char r[16];
      snprintf (r, sizeof (r), "%d", ws ()->active_tile->tile.nwindows);
      cmd_reply (client_fd, r);
      return;
    }
  if (strcmp (line, "query win-title") == 0)
    {
      Node *tile = ws ()->active_tile;
      if (tile->tile.nwindows > 0)
        {
          char title[256];
          get_wm_name (tile->tile.windows[tile->tile.active_tab], title,
                       sizeof (title));
          cmd_reply (client_fd, title);
        }
      else
        {
          cmd_reply (client_fd, "");
        }
      return;
    }
  if (strcmp (line, "query layout") == 0)
    {
      Node **tiles;
      int n = ws_get_tiles (ws (), &tiles);
      char r[16];
      snprintf (r, sizeof (r), "%d", n);
      cmd_reply (client_fd, r);
      return;
    }

  cmd_reply (client_fd, "err: unknown command");
}

static void
cmd_poll (fd_set *fds)
{
  if (cmd_listen_fd < 0)
    return;

  if (FD_ISSET (cmd_listen_fd, fds))
    {
      int cfd = accept (cmd_listen_fd, NULL, NULL);
      if (cfd >= 0)
        {
          fcntl (cfd, F_SETFL, O_NONBLOCK);
          fcntl (cfd, F_SETFD, FD_CLOEXEC);
          if (n_cmd_clients < CMD_MAX_CLIENTS)
            {
              cmd_clients[n_cmd_clients].fd = cfd;
              cmd_clients[n_cmd_clients].len = 0;
              n_cmd_clients++;
            }
          else
            {
              const char *msg = "err: too many clients\n";
              (void)write (cfd, msg, strlen (msg));
              close (cfd);
            }
        }
    }

  for (int i = 0; i < n_cmd_clients;)
    {
      if (!FD_ISSET (cmd_clients[i].fd, fds))
        {
          i++;
          continue;
        }

      char tmp[256];
      ssize_t nr = read (cmd_clients[i].fd, tmp, sizeof (tmp));
      if (nr <= 0)
        {
          cmd_drop_client (i);
          continue;
        }

      for (ssize_t j = 0; j < nr; j++)
        {
          if (tmp[j] == '\n' || tmp[j] == '\0')
            {
              cmd_clients[i].buf[cmd_clients[i].len] = '\0';
              cmd_dispatch (cmd_clients[i].fd, cmd_clients[i].buf);
              cmd_clients[i].len = 0;
            }
          else if (cmd_clients[i].len < CMD_BUF_SIZE - 1)
            {
              cmd_clients[i].buf[cmd_clients[i].len++] = tmp[j];
            }
        }
      i++;
    }
}

static int
cmd_fdset (fd_set *fds)
{
  int max_fd = -1;
  if (cmd_listen_fd < 0)
    return max_fd;
  FD_SET (cmd_listen_fd, fds);
  if (cmd_listen_fd > max_fd)
    max_fd = cmd_listen_fd;
  for (int i = 0; i < n_cmd_clients; i++)
    {
      FD_SET (cmd_clients[i].fd, fds);
      if (cmd_clients[i].fd > max_fd)
        max_fd = cmd_clients[i].fd;
    }
  return max_fd;
}

static void
cfg_apply (void)
{

  if (n_colors > 0)
    {
      unsigned long pixels[MAX_COLORS];
      for (int i = 0; i < n_colors; i++)
        pixels[i] = color_cache[i].px;
      XFreeColors (dpy, cmap, pixels, n_colors, 0);
    }
  n_colors = 0;

  int top_r = (cfg.statusbar_pos ? cfg.statusbar_height : 0)
              + (cfg.timebar_pos ? cfg.timebar_height : 0);
  int bot_r = (!cfg.statusbar_pos ? cfg.statusbar_height : 0)
              + (!cfg.timebar_pos ? cfg.timebar_height : 0);
  tile_y_off = top_r;
  tile_h_val = sh - top_r - bot_r;

  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      workspaces[i].tile_y = tile_y_off;
      workspaces[i].tile_h = tile_h_val;
    }

  bar_destroy ();
  if (cfg.statusbar_height > 0)
    bar_init ();

  timebar_destroy ();
  timebar_init ();

  XSetWindowBackground (dpy, root_win, px (cfg.col_desktop_bg));
  XClearWindow (dpy, root_win);

  arrange_workspace (ws ());
  focus_set_input (ws ()->active_tile);
  if (cfg.statusbar_height > 0)
    bar_draw ();
  timebar_draw ();

  grab_keys ();

  fprintf (stderr, "swm: config applied\n");
}

static void
on_map_request (XEvent *ev)
{
  Window wid = ev->xmaprequest.window;
  manage_window (wid);
}

static void
send_configure_notify (Window wid)
{
  XWindowAttributes wa;
  if (!XGetWindowAttributes (dpy, wid, &wa))
    return;

  XEvent ev;
  memset (&ev, 0, sizeof (ev));
  ev.xconfigure.type = ConfigureNotify;
  ev.xconfigure.event = wid;
  ev.xconfigure.window = wid;
  ev.xconfigure.x = wa.x;
  ev.xconfigure.y = wa.y;
  ev.xconfigure.width = wa.width;
  ev.xconfigure.height = wa.height;
  ev.xconfigure.border_width = wa.border_width;
  ev.xconfigure.above = None;
  ev.xconfigure.override_redirect = False;
  XSendEvent (dpy, wid, False, StructureNotifyMask, &ev);
}

static void
on_configure_request (XEvent *ev)
{
  XConfigureRequestEvent *cr = &ev->xconfigurerequest;
  Window wid = cr->window;
  if (managed_find (wid) >= 0)
    {

      send_configure_notify (wid);
      return;
    }

  XWindowChanges wc;
  unsigned int mask = 0;
  if (cr->value_mask & CWX)
    {
      wc.x = cr->x;
      mask |= CWX;
    }
  if (cr->value_mask & CWY)
    {
      wc.y = cr->y;
      mask |= CWY;
    }
  if (cr->value_mask & CWWidth)
    {
      wc.width = cr->width > 0 ? cr->width : 1;
      mask |= CWWidth;
    }
  if (cr->value_mask & CWHeight)
    {
      wc.height = cr->height > 0 ? cr->height : 1;
      mask |= CWHeight;
    }
  if (cr->value_mask & CWBorderWidth)
    {
      wc.border_width = cr->border_width;
      mask |= CWBorderWidth;
    }
  if (cr->value_mask & CWStackMode)
    {
      wc.stack_mode = cr->detail;
      mask |= CWStackMode;
    }
  if (mask)
    XConfigureWindow (dpy, wid, mask, &wc);
}

static void
on_destroy_notify (XEvent *ev)
{
  Window wid = ev->xdestroywindow.window;
  int was_float = set_contains (float_wins, n_float_wins, wid);
  set_remove (float_wins, &n_float_wins, wid);
  if (managed_find (wid) < 0)
    {
      if (was_float)
        {
          Node *tile = ws ()->active_tile;
          if (tile && tile->tile.nwindows > 0)
            focus_set_input (tile);
        }
      return;
    }
  unmanage_window (ev->xdestroywindow.window);
}

static void
on_unmap_notify (XEvent *ev)
{
  if (ev->xunmap.send_event)
    return;
  Window wid = ev->xunmap.window;
  int was_float = set_contains (float_wins, n_float_wins, wid);
  set_remove (float_wins, &n_float_wins, wid);
  int mi = managed_find (wid);
  if (mi < 0)
    {
      if (was_float)
        {
          Node *tile = ws ()->active_tile;
          if (tile && tile->tile.nwindows > 0)
            focus_set_input (tile);
        }
      return;
    }

  XWindowAttributes wa;
  if (!XGetWindowAttributes (dpy, wid, &wa))
    {
      unmanage_window (wid);
      return;
    }

  int ws_idx = managed[mi].ws;
  if (ws_idx == cur_ws)
    {
      Workspace *w = &workspaces[ws_idx];

      if (w->fullscreen_win != None && w->fullscreen_win != wid)
        return;

      Node *tile = ws_find_tile (w, wid);
      if (tile)
        {

          int idx = tile_index (tile, wid);
          if (idx == tile->tile.active_tab)
           {
            unmanage_window (wid);
           }
        }
    }
}

static void
on_expose (XEvent *ev)
{
  Window wid = ev->xexpose.window;
  if (status_bar_win != None && wid == status_bar_win)
    {
      bar_draw ();
      return;
    }
  if (timebar_win != None && wid == timebar_win)
    {
      timebar_draw ();
      return;
    }
  if (set_contains (tab_bars, n_tab_bars, wid))
    {
      Node **tiles;
      int n = ws_get_tiles (ws (), &tiles);
      for (int i = 0; i < n; i++)
        if (tiles[i]->tile.tab_bar_win == wid)
          {
            draw_tab_bar (tiles[i]);
            break;
          }
    }
}

static void
on_button_press (XEvent *ev)
{
  Window wid = ev->xbutton.window;

  if (status_bar_win != None && wid == status_bar_win)
    {
      bar_handle_click (ev);
      return;
    }

  if (timebar_win != None && wid == timebar_win)
    return;

  if (set_contains (tab_bars, n_tab_bars, wid))
    {
      Node **tiles;
      int n = ws_get_tiles (ws (), &tiles);
      for (int i = 0; i < n; i++)
        {
          if (tiles[i]->tile.tab_bar_win == wid)
            {
              Node *tile = tiles[i];
              if (tile->tile.nwindows == 0)
                {
                  ws ()->active_tile = tile;
                  focus_tile (tile);
                  break;
                }
              XWindowAttributes wa;
              XGetWindowAttributes (dpy, wid, &wa);
              int tab_w = wa.width / tile->tile.nwindows;
              if (tab_w < 1)
                tab_w = 1;
              int idx = ev->xbutton.x / tab_w;
              if (idx >= tile->tile.nwindows)
                idx = tile->tile.nwindows - 1;
              tile->tile.active_tab = idx;
              ws ()->active_tile = tile;
              arrange_tile (tile);
              focus_tile (tile);
              break;
            }
        }
      return;
    }

  int mi = managed_find (wid);
  if (mi >= 0 && managed[mi].ws == cur_ws)
    {
      Node *tile = ws_find_tile (ws (), wid);
      if (tile)
        {
          ws ()->active_tile = tile;
          tile->tile.active_tab = tile_index (tile, wid);
          focus_tile (tile);
        }
    }

  {
    XWindowAttributes wa;
    if (XGetWindowAttributes (dpy, wid, &wa) && !wa.override_redirect
        && wid != root_win && wid != status_bar_win && wid != timebar_win)
      {
        XRaiseWindow (dpy, wid);
        XSetInputFocus (dpy, wid, RevertToParent, CurrentTime);
      }
  }
}

static void
on_property_notify (XEvent *ev)
{
  Window wid = ev->xproperty.window;
  int mi = managed_find (wid);
  if (mi >= 0 && managed[mi].ws == cur_ws)
    {
      Node *tile = ws_find_tile (ws (), wid);
      if (tile)
        draw_tab_bar (tile);
    }
}

static void
on_client_message (XEvent *ev)
{
  XClientMessageEvent *cm = &ev->xclient;

  if (cm->message_type == a_net_wm_state && cm->format == 32)
    {
      long action = cm->data.l[0];
      Atom prop1 = (Atom)cm->data.l[1];
      Atom prop2 = (Atom)cm->data.l[2];

      if (prop1 == a_net_wm_state_fullscreen
          || prop2 == a_net_wm_state_fullscreen)
        {
          Window wid = cm->window;
          if (managed_find (wid) < 0)
            return;

          if (action == 2)
            fullscreen_toggle (wid);
          else if (action == 1)
            fullscreen_enter (wid);
          else
            fullscreen_exit (wid);
        }
    }
}

static void
on_key_press (XEvent *ev)
{
  KeySym ks = XLookupKeysym (&ev->xkey, 0);
  unsigned state = CLEAN_MASK (ev->xkey.state);

  for (int i = 0; i < cfg.n_binds; i++)
    {
      if (cfg.binds[i].key != ks)
        continue;
      if (CLEAN_MASK (cfg.binds[i].mod) != state)
        continue;

      switch (cfg.binds[i].action)
        {
        case ACT_SPAWN:
          {
            const char *cmd = (cfg.binds[i].sarg && cfg.binds[i].sarg[0])
                                  ? cfg.binds[i].sarg
                                  : NULL;
            if (cmd)
              spawn (cmd);
            break;
          }
        case ACT_SPLIT:
          action_split (cfg.binds[i].iarg & 1, cfg.binds[i].iarg & 2);
          break;
        case ACT_REMOVE_SPLIT:
          action_remove_split ();
          break;
        case ACT_CLOSE_WINDOW:
          action_close_window ();
          break;
        case ACT_QUIT:
          action_quit ();
          break;
        case ACT_NEXT_TAB:
          action_next_tab ();
          break;
        case ACT_PREV_TAB:
          action_prev_tab ();
          break;
        case ACT_MOVE_TAB_FWD:
          action_move_tab_forward ();
          break;
        case ACT_MOVE_TAB_BWD:
          action_move_tab_backward ();
          break;
        case ACT_NEXT_WORKSPACE:
          action_next_workspace ();
          break;
        case ACT_PREV_WORKSPACE:
          action_prev_workspace ();
          break;
        case ACT_SWITCH_WORKSPACE:
          action_switch_workspace (cfg.binds[i].iarg);
          break;
        case ACT_SEND_TO_WORKSPACE:
          action_send_to_workspace (cfg.binds[i].iarg);
          break;
        case ACT_FOCUS_DIR:
          if (cfg.binds[i].sarg)
            action_focus_direction (cfg.binds[i].sarg);
          break;
        case ACT_MOVE_WIN_DIR:
          if (cfg.binds[i].sarg)
            action_move_window_direction (cfg.binds[i].sarg);
          break;
        }
      return;
    }
}

static void
grab_keys (void)
{

  XUngrabKey (dpy, AnyKey, AnyModifier, root_win);

  unsigned lock_mods[] = { 0, Mod2Mask, LockMask, Mod2Mask | LockMask };
  int n_lock = (int)(sizeof (lock_mods) / sizeof (lock_mods[0]));

  for (int i = 0; i < cfg.n_binds; i++)
    {
      KeyCode kc = XKeysymToKeycode (dpy, cfg.binds[i].key);
      if (!kc)
        continue;
      for (int j = 0; j < n_lock; j++)
        XGrabKey (dpy, kc, cfg.binds[i].mod | lock_mods[j], root_win, True,
                  GrabModeAsync, GrabModeAsync);
    }

  XGrabButton (dpy, Button1, AnyModifier, root_win, True, ButtonPressMask,
               GrabModeSync, GrabModeAsync, None, None);
}

int
main (int argc, char **argv)
{
  (void)argc;
  (void)argv;
  signal (SIGCHLD, SIG_IGN);
  signal (SIGHUP, on_sighup);

  cfg_defaults ();
  {
    char cfgpath[512];
    if (cfg_resolve_path (cfgpath, sizeof (cfgpath)))
      {
        int n = cfg_load (cfgpath);
        if (n >= 0)
          fprintf (stderr, "swm: loaded %d settings from %s\n", n, cfgpath);
        else
          fprintf (stderr, "swm: failed to open %s\n", cfgpath);
      }
  }
  if (cfg.n_binds == 0)
    cfg_default_binds ();

  dpy = XOpenDisplay (NULL);
  if (!dpy)
    {
      fprintf (stderr, "Cannot open display\n");
      return 1;
    }

  scr_num = DefaultScreen (dpy);
  scr = ScreenOfDisplay (dpy, scr_num);
  root_win = RootWindow (dpy, scr_num);
  sw = scr->width;
  sh = scr->height;
  cmap = DefaultColormap (dpy, scr_num);
  depth = DefaultDepth (dpy, scr_num);

  {
    int top_r = (cfg.statusbar_pos ? cfg.statusbar_height : 0)
                + (cfg.timebar_pos ? cfg.timebar_height : 0);
    int bot_r = (!cfg.statusbar_pos ? cfg.statusbar_height : 0)
                + (!cfg.timebar_pos ? cfg.timebar_height : 0);
    tile_y_off = top_r;
    tile_h_val = sh - top_r - bot_r;
  }

  font = XLoadQueryFont (dpy,
                         "-misc-fixed-medium-r-*-*-13-*-*-*-*-*-iso8859-1");
  if (!font)
    font = XLoadQueryFont (dpy, "fixed");
  if (!font)
    {
      fprintf (stderr, "Cannot load font\n");
      XCloseDisplay (dpy);
      return 1;
    }

  a_net_wm_name = XInternAtom (dpy, "_NET_WM_NAME", False);
  a_utf8 = XInternAtom (dpy, "UTF8_STRING", False);
  a_wm_protocols = XInternAtom (dpy, "WM_PROTOCOLS", False);
  a_wm_delete = XInternAtom (dpy, "WM_DELETE_WINDOW", False);
  a_wm_type = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE", False);
  a_wm_type_dock = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
  a_net_wm_state = XInternAtom (dpy, "_NET_WM_STATE", False);
  a_net_wm_state_fullscreen
      = XInternAtom (dpy, "_NET_WM_STATE_FULLSCREEN", False);
  a_wm_type_splash = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
  a_wm_type_dialog = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  a_wm_type_tooltip = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
  a_wm_type_notification
      = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
  a_wm_type_popup_menu
      = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);

  draw_gc = XCreateGC (dpy, root_win, 0, NULL);

  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      workspaces[i].sw = sw;
      workspaces[i].sh = sh;
      workspaces[i].tile_y = tile_y_off;
      workspaces[i].tile_h = tile_h_val;
      workspaces[i].root = node_new_tile (0, tile_y_off, sw, tile_h_val);
      if (!workspaces[i].root)
        {
          fprintf (stderr, "swm: workspace %d: alloc failed\n", i);
          XCloseDisplay (dpy);
          return 1;
        }
      workspaces[i].active_tile = workspaces[i].root;
      workspaces[i].fullscreen_win = None;
      workspaces[i].tiles_dirty = 1;
    }
  cur_ws = 0;
  status_bar_win = None;
  timebar_win = None;
  running = 1;

  XSetWindowBackground (dpy, root_win, px (cfg.col_desktop_bg));
  XClearWindow (dpy, root_win);

  wm_detected = 0;
  XSetErrorHandler (xerr_detect);
  XSelectInput (dpy, root_win,
                SubstructureRedirectMask | SubstructureNotifyMask
                    | KeyPressMask | ButtonPressMask | FocusChangeMask);
  XSync (dpy, False);
  if (wm_detected)
    {
      fprintf (stderr, "Another WM is running\n");
      XCloseDisplay (dpy);
      return 1;
    }
  XSetErrorHandler (xerr_handler);

  grab_keys ();
  XSync (dpy, False);

  cmd_init ();

  init_ewmh ();

  if (cfg.statusbar_height > 0)
    bar_init ();

  timebar_init ();

  {
    Window d1, d2, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree (dpy, root_win, &d1, &d2, &children, &nchildren))
      {
        for (unsigned int i = 0; i < nchildren; i++)
          {
            XWindowAttributes wa;
            if (XGetWindowAttributes (dpy, children[i], &wa)
                && wa.map_state == IsViewable && !wa.override_redirect)
              {
                manage_window (children[i]);
              }
          }
        if (children)
          XFree (children);
      }
  }

  arrange_workspace (ws ());
  last_bar_update = mono_time ();

  int x_fd = ConnectionNumber (dpy);

  while (running)
    {

      while (XPending (dpy))
        {
          XEvent ev;
          XNextEvent (dpy, &ev);
          switch (ev.type)
            {
            case MapRequest:
              on_map_request (&ev);
              break;
            case ConfigureRequest:
              on_configure_request (&ev);
              break;
            case DestroyNotify:
              on_destroy_notify (&ev);
              break;
            case UnmapNotify:
              on_unmap_notify (&ev);
              break;
            case Expose:
              on_expose (&ev);
              break;
            case KeyPress:
              on_key_press (&ev);
              break;
            case ButtonPress:
              on_button_press (&ev);
              XAllowEvents (dpy, ReplayPointer, CurrentTime);
              break;
            case PropertyNotify:
              on_property_notify (&ev);
              break;
            case ClientMessage:
              on_client_message (&ev);
              break;
            }
        }

      double now = mono_time ();
      if ((now - last_bar_update) >= cfg.bar_update_interval)
        {
          if (cfg.statusbar_height > 0)
            bar_draw ();
          timebar_draw ();
          last_bar_update = now;
        }

      double remaining
          = cfg.bar_update_interval - (mono_time () - last_bar_update);
      if (remaining < 0.05)
        remaining = 0.05;
      struct timeval tv;
      tv.tv_sec = (long)remaining;
      tv.tv_usec = (long)((remaining - tv.tv_sec) * 1e6);
      fd_set fds;
      FD_ZERO (&fds);
      FD_SET (x_fd, &fds);
      int max_fd = x_fd;
      int cmd_max = cmd_fdset (&fds);
      if (cmd_max > max_fd)
        max_fd = cmd_max;
      select (max_fd + 1, &fds, NULL, NULL, &tv);

      cmd_poll (&fds);

      if (apply_pending)
        {
          apply_pending = 0;
          cfg_apply ();
        }

      if (reload_pending)
        {
          reload_pending = 0;
          char cfgpath[512];
          if (cfg_resolve_path (cfgpath, sizeof (cfgpath)))
            {
              cfg_defaults ();
              int n = cfg_load (cfgpath);
              if (cfg.n_binds == 0)
                cfg_default_binds ();
              fprintf (stderr, "swm: reload %d settings from %s\n", n,
                       cfgpath);
              cfg_apply ();
            }
        }
    }

  cmd_cleanup ();
  bar_destroy ();
  timebar_destroy ();
  cleanup_ewmh ();
  for (int i = 0; i < NUM_WORKSPACES; i++)
    {
      Node **tiles;
      int n = ws_get_tiles (&workspaces[i], &tiles);
      for (int j = 0; j < n; j++)
        {
          destroy_tab_bar (tiles[j]);
          destroy_frame (tiles[j]);
        }
      node_free_tree (workspaces[i].root);
      workspaces[i].root = NULL;
    }

  for (int i = 0; i < cfg.n_binds; i++)
    {
      free (cfg.binds[i].sarg);
      cfg.binds[i].sarg = NULL;
    }
  cfg.n_binds = 0;

  if (n_colors > 0)
    {
      unsigned long pixels[MAX_COLORS];
      for (int i = 0; i < n_colors; i++)
        pixels[i] = color_cache[i].px;
      XFreeColors (dpy, cmap, pixels, n_colors, 0);
      n_colors = 0;
    }

  if (font)
    {
      XFreeFont (dpy, font);
      font = NULL;
    }

  XFreeGC (dpy, draw_gc);
  XCloseDisplay (dpy);
  return 0;
}
