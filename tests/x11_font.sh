#!/bin/bash
# S3 test: X11 client (runtime/x11.c) and TrueType rasterizer (runtime/font.c).
# Usage: tests/x11_font.sh [work-dir]          (default: build/tests/x11_font)
# Exit 0 = PASS, 77 = SKIP (no xvfb-run / xdotool / import / python3), otherwise FAIL.
# Every X client runs on a private Xvfb started by `xvfb-run -a`; DISPLAY :0 is never used.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-$root/build/tests/x11_font}
mkdir -p "$work" || exit 1
CF="-O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
-fno-pie -mcmodel=large -fno-stack-protector -fcf-protection=none -fno-strict-aliasing \
-D_FILE_OFFSET_BITS=64 -I$root/runtime"

fails=0
pass() { printf '%-34s PASS\n' "$1"; }
bad() { printf '%-34s FAIL (%s)\n' "$1" "$2"; fails=$((fails + 1)); }
skip() { printf '%-34s SKIP (no %s)\n' "$1" "$2"; exit 77; }

command -v python3 >/dev/null 2>&1 || skip "x11_font" python3
command -v xvfb-run >/dev/null 2>&1 || skip "x11_font" xvfb-run
command -v xdotool >/dev/null 2>&1 || skip "x11_font" xdotool
command -v import >/dev/null 2>&1 || skip "x11_font" import

# ---- 1. font: metrics, coverage, face mapping (no X server) ----
if ! cc $CF -o "$work/font_check" "$root/tests/font_check.c" -no-pie 2>"$work/font_build.log"; then
    bad "font build" "see $work/font_build.log"
else
    "$work/font_check" "$work/line.pgm" >"$work/font.log" 2>&1
    if [ $? -eq 0 ] && grep -q '^RESULT PASS' "$work/font.log"; then
        pass "font harness (metrics, AA)"
    else
        bad "font harness" "see $work/font.log"
    fi
    # independent check: winAscent/winDescent from the font files (OS/2, head) vs the harness
    if python3 -I - "$work/font.log" <<'PY'
import math, struct, re, sys
log = open(sys.argv[1]).read()
def tables(path):
    d = open(path, 'rb').read()
    n = struct.unpack('>H', d[4:6])[0]
    t = {}
    for i in range(n):
        tag, _, off, _ = struct.unpack('>4sIII', d[12 + 16 * i:28 + 16 * i])
        t[tag.decode('latin1')] = off
    upem = struct.unpack('>H', d[t['head'] + 18:t['head'] + 20])[0]
    wa, wd = struct.unpack('>HH', d[t['OS/2'] + 74:t['OS/2'] + 78])
    return upem, wa, wd
ok = True
for tag in ('DejaVuSans-12', 'NotoSans-12'):
    path = re.search(r'^PATH %s (\S+)' % tag, log, re.M).group(1)
    m = re.search(r'^METRICS %s asc=(\d+) desc=(\d+)' % tag, log, re.M)
    upem, wa, wd = tables(path)
    exp = (math.ceil(wa * 12 / upem), math.ceil(wd * 12 / upem))
    got = (int(m.group(1)), int(m.group(2)))
    print('metrics %-14s file=%s expected=%s got=%s' % (tag, path.split('/')[-1], exp, got))
    ok = ok and exp == got
sys.exit(0 if ok else 1)
PY
    then pass "font metrics vs OS/2 tables"; else bad "font metrics vs OS/2 tables" "see $work/font.log"; fi
fi

# font directory override: a file in ANYWIN_FONT_DIR wins over the system copy of the same name
mkdir -p "$work/fontdir" && ln -sf /usr/share/fonts/TTF/DejaVuSans.ttf "$work/fontdir/NotoSans-Regular.ttf"
ANYWIN_FONT_DIR="$work/fontdir" "$work/font_check" "$work/line2.pgm" >"$work/font2.log" 2>&1
grep -q "^PATH NotoSans-12 $work/fontdir/NotoSans-Regular.ttf" "$work/font2.log" \
    && pass "font ANYWIN_FONT_DIR override" || bad "font ANYWIN_FONT_DIR" "see $work/font2.log"

# ---- 2. X11 client: build the harnesses ----
if ! cc $CF -o "$work/x11_check" "$root/tests/x11_check.c" "$root/runtime/x11.c" -no-pie 2>"$work/x11_build.log"; then
    bad "x11 build" "see $work/x11_build.log"
    echo "RESULT FAIL ($fails)"; exit 1
fi
if ! cc $CF -o "$work/x11_tray" "$root/tests/x11_tray.c" -no-pie 2>"$work/tray_build.log"; then
    bad "tray build" "see $work/tray_build.log"
    echo "RESULT FAIL ($fails)"; exit 1
fi

# no DISPLAY (and no auth): plat_init must fail cleanly, not crash
env -u DISPLAY -u XAUTHORITY "$work/x11_check" >"$work/nodisplay.log" 2>&1
grep -q '^RESULT FAIL no display' "$work/nodisplay.log" && pass "x11 no display -> plat_init 0" \
    || bad "x11 no display" "see $work/nodisplay.log"

# ---- 3. window, PutImage, input, screenshot ----
rm -f "$work/x11.log" "$work/x11.png"
xvfb-run -a -s '-screen 0 1024x768x24' sh -c '
    "$1" >"$2" 2>&1 &
    pid=$!
    for i in $(seq 100); do grep -q "^READY" "$2" 2>/dev/null && break; sleep 0.1; done
    wid=$(sed -n "s/^READY //p" "$2" | head -1)
    [ -n "$wid" ] || { kill $pid 2>/dev/null; exit 2; }
    xdotool windowfocus --sync "$wid" 2>/dev/null
    xdotool getwindowname "$wid" >"$2.title" 2>/dev/null
    sleep 0.3
    xdotool mousemove --sync 150 150 click 1
    xdotool mousemove --sync 152 151
    xdotool click 4
    xdotool key a
    xdotool key shift+b
    sleep 0.5
    import -window root "$3" 2>/dev/null
    xdotool key Escape
    wait $pid
    echo "harness exit $?" >>"$2"
' sh "$work/x11_check" "$work/x11.log" "$work/x11.png" || true

x11_ok=1
grep -q '^READY ' "$work/x11.log" || { bad "x11 READY" "no window"; x11_ok=0; }
grep -q '^EV KEYDOWN vk=0x41 ' "$work/x11.log" && pass "x11 key a -> KEYDOWN VK_A" || { bad "x11 key a" "$work/x11.log"; x11_ok=0; }
grep -q '^EV KEYDOWN vk=0x42 scan=0x30 text=\[B\] rep=0 mods=0x1$' "$work/x11.log" && pass "x11 shift+b -> B with SHIFT" || { bad "x11 shift" "$work/x11.log"; x11_ok=0; }
grep -q '^EV BUTTONDOWN button=1 ' "$work/x11.log" && pass "x11 click -> BUTTONDOWN 1" || { bad "x11 click" "$work/x11.log"; x11_ok=0; }
grep -q '^EV WHEEL delta=120 horizontal=0' "$work/x11.log" && pass "x11 wheel 4 -> WHEEL +120" || { bad "x11 wheel" "$work/x11.log"; x11_ok=0; }
grep -q '^COUNTS .*motions=[1-9]' "$work/x11.log" && pass "x11 pointer motion" || { bad "x11 motion" "$work/x11.log"; x11_ok=0; }
grep -q '^EV KEYDOWN vk=0x1b' "$work/x11.log" && pass "x11 Escape -> KEYDOWN VK_ESCAPE" || { bad "x11 escape" "$work/x11.log"; x11_ok=0; }
if grep -q '^EV EXPOSE' "$work/x11.log"; then pass "x11 EXPOSE event"; else bad "x11 EXPOSE event" "$work/x11.log"; fi
grep -q '^POS 100 100' "$work/x11.log" && pass "x11 window position (100,100)" || { bad "x11 position" "$work/x11.log"; x11_ok=0; }
grep -q '^CLIP \[anywin clip é\]' "$work/x11.log" && pass "x11 clipboard UTF-8 round trip" || bad "x11 clipboard" "$work/x11.log"
grep -q '^MOVED 120 130' "$work/x11.log" && pass "x11 window move (ConfigureWindow)" || bad "x11 move" "$work/x11.log"
grep -q '^GRAB 1 1' "$work/x11.log" && pass "x11 pointer+keyboard grab" || bad "x11 grab" "$work/x11.log"
grep -q '^TRAY_NO_MANAGER 0 size=0' "$work/x11.log" && pass "x11 tray without manager -> 0" || bad "x11 tray none" "$work/x11.log"
grep -q '^RESULT PASS' "$work/x11.log" && pass "x11 harness result" || { bad "x11 harness result" "$work/x11.log"; x11_ok=0; }
if [ "$(cat "$work/x11.log.title" 2>/dev/null)" = "anywin x11 test" ]; then
    pass "x11 _NET_WM_NAME / WM_NAME title"
else
    bad "x11 title" "$(cat "$work/x11.log.title" 2>/dev/null)"
fi
px() { convert "$work/x11.png" -format '%[hex:p{'"$1"','"$2"'}]' info: 2>/dev/null; }
if [ -s "$work/x11.png" ]; then
    g=$(px 115 115); r=$(px 250 200); w=$(px 5 5)
    [ "$g" = "00FF00" ] && pass "x11 PutImage green square pixel" || bad "x11 green pixel" "got $g"
    [ "$r" = "966480" ] && pass "x11 PutImage gradient (chunked, clipped)" || bad "x11 gradient pixel" "got $r"
    c=$(px 100 100)
    [ "$c" = "000080" ] && pass "x11 window origin pixel" || bad "x11 origin pixel" "got $c"
    [ "$w" = "000000" ] && pass "x11 outside window untouched" || bad "x11 outside" "got $w"
else
    bad "x11 screenshot" "no $work/x11.png"
fi

# ---- 4. tray: XEmbed dock + composite over the tray background ----
rm -f "$work/tray.log" "$work/tray.png"
xvfb-run -a -s '-screen 0 1024x768x24' sh -c '
    "$1" >"$2" 2>&1 &
    pid=$!
    for i in $(seq 100); do grep -q "^READY" "$2" 2>/dev/null && break; sleep 0.1; done
    sleep 0.5
    import -window root "$3" 2>/dev/null
    wait $pid
    echo "harness exit $?" >>"$2"
' sh "$work/x11_tray" "$work/tray.log" "$work/tray.png" || true
grep -q '^CLIP_REMOTE \[remote é\]' "$work/tray.log" && pass "x11 clipboard via SelectionRequest" || bad "x11 remote clipboard" "$work/tray.log"
grep -q '^DOCKED OK' "$work/tray.log" && pass "tray XEmbed dock request" || bad "tray dock" "$work/tray.log"
if [ -s "$work/tray.png" ]; then
    bgp=$(convert "$work/tray.png" -format '%[hex:p{300,200}]' info: 2>/dev/null)
    redp=$(convert "$work/tray.png" -format '%[hex:p{311,211}]' info: 2>/dev/null)
    [ "$bgp" = "102030" ] && pass "tray transparent pixel = bg" || bad "tray bg" "got $bgp"
    [ "$redp" = "FF0000" ] && pass "tray opaque pixel composited" || bad "tray fg" "got $redp"
else
    bad "tray screenshot" "no $work/tray.png"
fi

if [ $fails -eq 0 ]; then
    echo "RESULT PASS"
    exit 0
fi
echo "RESULT FAIL ($fails)"
exit 1
