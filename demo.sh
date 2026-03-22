#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
#  swm-demo.sh — a choreographed tour of swm's scripting capabilities
#
#  Cycles through 5 workspaces.  Each one gets a distinct color theme,
#  a tiled layout of xterms, and a "breathing" color animation that
#  pulses the entire palette while splits fly in.
#
#  Requirements: swmctl (in PATH), xterm
#  Usage:        chmod +x swm-demo.sh && ./swm-demo.sh
# ─────────────────────────────────────────────────────────────────────

# ── preflight ────────────────────────────────────────────────────────

for dep in swmctl xterm; do
    if ! command -v "$dep" >/dev/null 2>&1; then
        echo "✗  '$dep' not found in PATH." >&2
        exit 1
    fi
done

if ! swmctl query ws >/dev/null 2>&1; then
    echo "✗  Can't reach swm — is it running?" >&2
    exit 1
fi

# ── color math ───────────────────────────────────────────────────────

hex2dec() { printf '%d' "0x$1"; }

# Lerp two #RRGGBB values.  $3 = 0..1000 (integer thousandths).
lerp() {
    local a="${1:1}" b="${2:1}" t="$3"
    local ar=$(hex2dec "${a:0:2}") ag=$(hex2dec "${a:2:2}") ab=$(hex2dec "${a:4:2}")
    local br=$(hex2dec "${b:0:2}") bg=$(hex2dec "${b:2:2}") bb=$(hex2dec "${b:4:2}")
    printf '#%02X%02X%02X' \
        $(( ar + (br - ar) * t / 1000 )) \
        $(( ag + (bg - ag) * t / 1000 )) \
        $(( ab + (bb - ab) * t / 1000 ))
}

# ── theme definitions ────────────────────────────────────────────────
#
# Each entry: "cfg_key  lo_color  hi_color"
# The breather interpolates every key between lo and hi.

THEME_1=(  # EMBER — deep reds / warm orange
    "col_desktop_bg             #0A0000 #1A0500"
    "col_statusbar_bg           #1A0500 #2D0A00"
    "col_statusbar_fg           #FF4500 #FF6A33"
    "col_statusbar_ws_active    #FF4500 #FF6A33"
    "col_statusbar_ws_inactive  #441500 #553300"
    "col_statusbar_ws_occupied  #882200 #AA4400"
    "col_statusbar_ws_fg_act    #000000 #000000"
    "col_statusbar_ws_fg_inact  #AA6644 #CC8866"
    "col_tab_active_bg          #FF4500 #FF6A33"
    "col_tab_inactive_bg        #1A0500 #2D0A00"
    "col_tab_active_fg          #000000 #000000"
    "col_tab_inactive_fg        #884422 #AA6644"
    "col_tab_bar_bg             #0F0200 #1A0500"
    "col_tab_active_bg_dim      #662200 #883300"
    "col_tab_active_fg_dim      #CCCCCC #DDDDDD"
    "col_border_active          #FF4500 #FF6A33"
    "col_border_inactive        #330E00 #441500"
    "col_timebar_bg             #1A0500 #2D0A00"
    "col_timebar_hour           #FF2200 #FF4400"
    "col_timebar_hex            #FF6600 #FF8833"
    "col_timebar_seg            #CC4400 #EE5500"
    "col_timebar_label          #FFCCAA #FFDDBB"
    "col_timebar_track          #331100 #442200"
)

THEME_2=(  # OCEAN — deep blues / cyan
    "col_desktop_bg             #000810 #001222"
    "col_statusbar_bg           #001122 #002244"
    "col_statusbar_fg           #00BBFF #33CCFF"
    "col_statusbar_ws_active    #00BBFF #33CCFF"
    "col_statusbar_ws_inactive  #002244 #003366"
    "col_statusbar_ws_occupied  #005588 #0077AA"
    "col_statusbar_ws_fg_act    #000000 #000000"
    "col_statusbar_ws_fg_inact  #4477AA #6699CC"
    "col_tab_active_bg          #00BBFF #33CCFF"
    "col_tab_inactive_bg        #001122 #002244"
    "col_tab_active_fg          #000000 #000000"
    "col_tab_inactive_fg        #225588 #4477AA"
    "col_tab_bar_bg             #000A14 #001122"
    "col_tab_active_bg_dim      #004477 #005599"
    "col_tab_active_fg_dim      #CCDDEE #DDEEFF"
    "col_border_active          #00BBFF #33CCFF"
    "col_border_inactive        #001933 #002E55"
    "col_timebar_bg             #001122 #002244"
    "col_timebar_hour           #0066CC #0088EE"
    "col_timebar_hex            #00AADD #00CCFF"
    "col_timebar_seg            #0088BB #00AADD"
    "col_timebar_label          #AADDFF #CCEEFF"
    "col_timebar_track          #001830 #002848"
)

THEME_3=(  # MOSS — dark greens / lime
    "col_desktop_bg             #000800 #001500"
    "col_statusbar_bg           #001A00 #003300"
    "col_statusbar_fg           #33FF33 #66FF66"
    "col_statusbar_ws_active    #33FF33 #66FF66"
    "col_statusbar_ws_inactive  #143314 #225522"
    "col_statusbar_ws_occupied  #227722 #339933"
    "col_statusbar_ws_fg_act    #000000 #000000"
    "col_statusbar_ws_fg_inact  #44AA44 #66CC66"
    "col_tab_active_bg          #33FF33 #66FF66"
    "col_tab_inactive_bg        #001A00 #003300"
    "col_tab_active_fg          #000000 #000000"
    "col_tab_inactive_fg        #228822 #44AA44"
    "col_tab_bar_bg             #000F00 #001A00"
    "col_tab_active_bg_dim      #116611 #228822"
    "col_tab_active_fg_dim      #CCEECC #DDFFDD"
    "col_border_active          #33FF33 #66FF66"
    "col_border_inactive        #0A2E0A #154015"
    "col_timebar_bg             #001A00 #003300"
    "col_timebar_hour           #00CC00 #00EE00"
    "col_timebar_hex            #33DD33 #55FF55"
    "col_timebar_seg            #22AA22 #44CC44"
    "col_timebar_label          #AAFFAA #CCFFCC"
    "col_timebar_track          #002200 #003800"
)

THEME_4=(  # ULTRAVIOLET — purple / magenta
    "col_desktop_bg             #080010 #120020"
    "col_statusbar_bg           #110022 #220044"
    "col_statusbar_fg           #CC33FF #DD66FF"
    "col_statusbar_ws_active    #CC33FF #DD66FF"
    "col_statusbar_ws_inactive  #2A0044 #3D0066"
    "col_statusbar_ws_occupied  #661199 #8833BB"
    "col_statusbar_ws_fg_act    #000000 #000000"
    "col_statusbar_ws_fg_inact  #8855AA #AA77CC"
    "col_tab_active_bg          #CC33FF #DD66FF"
    "col_tab_inactive_bg        #110022 #220044"
    "col_tab_active_fg          #000000 #000000"
    "col_tab_inactive_fg        #663388 #8855AA"
    "col_tab_bar_bg             #0A0015 #110022"
    "col_tab_active_bg_dim      #552288 #663399"
    "col_tab_active_fg_dim      #DDCCEE #EEDDFF"
    "col_border_active          #CC33FF #DD66FF"
    "col_border_inactive        #220033 #330055"
    "col_timebar_bg             #110022 #220044"
    "col_timebar_hour           #AA00DD #CC22FF"
    "col_timebar_hex            #BB33EE #DD55FF"
    "col_timebar_seg            #9922CC #BB44EE"
    "col_timebar_label          #DDAAFF #EECCFF"
    "col_timebar_track          #1A0030 #2A0050"
)

THEME_5=(  # SOLAR — gold / warm yellow
    "col_desktop_bg             #0A0700 #161000"
    "col_statusbar_bg           #1A1200 #332200"
    "col_statusbar_fg           #FFCC00 #FFDD44"
    "col_statusbar_ws_active    #FFCC00 #FFDD44"
    "col_statusbar_ws_inactive  #332200 #554400"
    "col_statusbar_ws_occupied  #886600 #AA8800"
    "col_statusbar_ws_fg_act    #000000 #000000"
    "col_statusbar_ws_fg_inact  #AA9966 #CCBB88"
    "col_tab_active_bg          #FFCC00 #FFDD44"
    "col_tab_inactive_bg        #1A1200 #332200"
    "col_tab_active_fg          #000000 #000000"
    "col_tab_inactive_fg        #887744 #AA9966"
    "col_tab_bar_bg             #100A00 #1A1200"
    "col_tab_active_bg_dim      #665500 #887700"
    "col_tab_active_fg_dim      #EEDDCC #FFEEDD"
    "col_border_active          #FFCC00 #FFDD44"
    "col_border_inactive        #2B1E00 #3D2D00"
    "col_timebar_bg             #1A1200 #332200"
    "col_timebar_hour           #DDAA00 #FFCC00"
    "col_timebar_hex            #EEBB22 #FFDD44"
    "col_timebar_seg            #CC9900 #EEBB00"
    "col_timebar_label          #FFEEAA #FFFFCC"
    "col_timebar_track          #221800 #332800"
)

# ── theme engine ─────────────────────────────────────────────────────

# Batch-set all theme colors at interpolation point $2 (0..1000).
# Pipes every "set" line through swmctl's stdin mode in one shot.
apply_theme() {
    local ws="$1" t="$2"
    local -n arr="THEME_${ws}"
    local batch=""

    for entry in "${arr[@]}"; do
        read -r key lo hi <<< "$entry"
        batch+="set $key $(lerp "$lo" "$hi" "$t")"$'\n'
    done

    printf '%s' "$batch" | swmctl -
}

snap_theme() { apply_theme "$1" 0; }

# Breathe: ease up 0→1000→0, repeated $2 times.
breathe() {
    local ws="$1" cycles="${2:-1}" delay="${3:-0.04}"
    local ramp=(0 80 250 500 750 920 1000 1000 920 750 500 250 80 0)

    for (( c = 0; c < cycles; c++ )); do
        for t in "${ramp[@]}"; do
            apply_theme "$ws" "$t"
            sleep "$delay"
        done
    done
}

# ── choreography helpers ─────────────────────────────────────────────

wait_win() {
    local before
    before=$(swmctl query win-count)
    for _ in $(seq 1 30); do
        sleep 0.1
        [[ "$(swmctl query win-count)" != "$before" ]] && return 0
    done
}

xterm_spawn() {
    swmctl exec "xterm -bg '${1:-black}' -fg '${2:-white}' -T '${3:-demo}'"
    wait_win
}

pause() { sleep "${1:-0.4}"; }

# ── the show ─────────────────────────────────────────────────────────

cat <<'BANNER'

  ╔═════════════════════════════════════════╗
  ║   swm demo — 5 workspaces, 5 themes    ║
  ║   breathing colors · scripted splits    ║
  ╚═════════════════════════════════════════╝

BANNER

# ── Act 1: EMBER — 3 panes, L-shape ─────────────────────────────
echo "▸ Act 1 · Ember"
swmctl workspace 1;  pause 0.2
snap_theme 1

xterm_spawn "#1A0500" "#FF6633" "ember:main"
breathe 1 1 0.03

swmctl split h;      pause 0.3
xterm_spawn "#0F0200" "#CC4400" "ember:side"
breathe 1 1 0.03

swmctl focus left;   pause 0.2
swmctl split v;      pause 0.3
xterm_spawn "#0A0000" "#FF8844" "ember:lower"
breathe 1 1 0.03

echo "  ✓  3 panes"

# ── Act 2: OCEAN — 4 panes, 2×2 grid ────────────────────────────
echo "▸ Act 2 · Ocean"
swmctl workspace 2;  pause 0.2
snap_theme 2

xterm_spawn "#001122" "#33CCFF" "ocean:TL"
breathe 2 1 0.03

swmctl split h;      pause 0.3
xterm_spawn "#001933" "#66DDFF" "ocean:TR"
breathe 2 1 0.03

swmctl focus left;   pause 0.2
swmctl split v;      pause 0.3
xterm_spawn "#000A14" "#0099DD" "ocean:BL"

swmctl focus right;  pause 0.2
swmctl split v;      pause 0.3
xterm_spawn "#001122" "#00AADD" "ocean:BR"
breathe 2 1 0.03

echo "  ✓  4 panes (2×2)"

# ── Act 3: MOSS — 3 panes, editor + right stack ─────────────────
echo "▸ Act 3 · Moss"
swmctl workspace 3;  pause 0.2
snap_theme 3

xterm_spawn "#001500" "#66FF66" "moss:editor"
breathe 3 1 0.03

swmctl split h;      pause 0.3
xterm_spawn "#000F00" "#33DD33" "moss:top"

swmctl split v;      pause 0.3
xterm_spawn "#001A00" "#22AA22" "moss:bottom"
breathe 3 1 0.03

echo "  ✓  3 panes (editor + stack)"

# ── Act 4: ULTRAVIOLET — 4 panes, 2×2 grid ──────────────────────
echo "▸ Act 4 · Ultraviolet"
swmctl workspace 4;  pause 0.2
snap_theme 4

xterm_spawn "#120020" "#DD66FF" "uv:TL"
breathe 4 1 0.03

swmctl split h;      pause 0.3
xterm_spawn "#110022" "#BB33EE" "uv:TR"

swmctl focus left;   pause 0.2
swmctl split v;      pause 0.3
xterm_spawn "#0A0015" "#CC33FF" "uv:BL"

swmctl focus right;  pause 0.2
swmctl split v;      pause 0.3
xterm_spawn "#220044" "#AA77CC" "uv:BR"
breathe 4 1 0.03

echo "  ✓  4 panes (2×2)"

# ── Act 5: SOLAR — 3 even columns ───────────────────────────────
echo "▸ Act 5 · Solar"
swmctl workspace 5;  pause 0.2
snap_theme 5

xterm_spawn "#161000" "#FFDD44" "solar:left"
breathe 5 1 0.03

swmctl split h;      pause 0.3
xterm_spawn "#1A1200" "#EEBB22" "solar:mid"

swmctl split h;      pause 0.3
xterm_spawn "#100A00" "#CC9900" "solar:right"
breathe 5 1 0.03

echo "  ✓  3 columns"

# ── Finale: grand tour ──────────────────────────────────────────
echo
echo "▸ Finale · breathing tour"

for ws in 1 2 3 4 5; do
    swmctl workspace "$ws"
    breathe "$ws" 2 0.03
done

# Fast lap
echo "▸ Speed round"
for ws in 1 2 3 4 5 1 2 3 4 5; do
    swmctl workspace "$ws"
    snap_theme "$ws"
    sleep 0.2
done

# Land on ws 1, mid-glow
swmctl workspace 1
apply_theme 1 500

echo
echo "  ═══════════════════════════════════"
echo "  Done.  Mod4+1‥5 to revisit themes."
echo "  ═══════════════════════════════════" 
