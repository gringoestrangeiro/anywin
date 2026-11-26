#!/bin/bash
# user32 core probe (S5): relinks tests/core_gui_probe.c and drives it under Xvfb with xdotool.
# Checks: custom child window paints and draws its WS_BORDER, Button click reaches the parent
# (WM_COMMAND), timer repaint, keyboard KEYDOWN + WM_CHAR, WM_PAINT after invalidate, clean exit.
# Assertions are pixel colours of a root screenshot plus the probe's stdout.  No vision.
# Usage: tests/core_gui.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_gui_probe.c
exe=$build/core_gui_probe.exe
out=$build/core_gui
log=$build/core_gui.log
shot=$build/core_gui.png
shot2=$build/core_gui_click.png

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_gui: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_gui: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_gui: SKIP (no xvfb-run)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -mwindows -o "$exe" "$src" || { echo "core_gui: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >/dev/null 2>&1 || { echo "core_gui: relink failed"; exit 1; }
rm -f "$log" "$shot" "$shot2"

# colour of root pixel (x,y) as #RRGGBB
pixel() {
    magick "$1" -crop "1x1+$2+$3" -depth 8 txt:- 2>/dev/null | awk 'NR==2 {print toupper($3)}'
}

xvfb-run -a -s "-screen 0 800x600x24" bash -c '
    out=$1 log=$2 shot=$3 shot2=$4
    "$out" >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 80); do
        xdotool search --name "Core GUI Probe" >/dev/null 2>&1 && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    wid=$(xdotool search --name "Core GUI Probe" 2>/dev/null | head -1)
    sleep 0.9
    import -window root "$shot" 2>/dev/null
    # keyboard goes to the window with the focus: the top-level, until the Button is clicked
    xdotool mousemove 300 250
    xdotool key a
    sleep 0.3
    # press the Button: window at (20,20), button at client (10,120) 120x30 -> root (30..150, 140..170)
    xdotool mousemove 90 155 click 1
    sleep 0.4
    import -window root "$shot2" 2>/dev/null
    for _ in $(seq 1 80); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then kill "$pid" 2>/dev/null; echo "HUNG" >>"$log"; fi
    wait "$pid"
    echo "EXIT=$?" >>"$log"
    [ -n "$wid" ] || echo "NOWINDOW" >>"$log"
' _ "$out" "$log" "$shot" "$shot2"

fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "  ok   $1 ($3)"; else echo "  FAIL $1: expected $2, got $3"; fail=1; fi
}
if [ ! -f "$shot" ] || [ ! -f "$shot2" ]; then echo "core_gui: no screenshots" >&2; fail=1; fi
if grep -q NOWINDOW "$log" 2>/dev/null; then echo "  FAIL window not found by title"; fail=1; else echo "  ok   window found by title"; fi
check "child window interior painted" "#0000FF" "$(pixel "$shot" 40 40)"
check "child WS_BORDER colour" "#646464" "$(pixel "$shot" 30 30)"
check "timer repaint (green square)" "#00FF00" "$(pixel "$shot" 220 40)"
check "parent background white" "#FFFFFF" "$(pixel "$shot" 300 250)"
check "click -> WM_COMMAND -> repaint (red square)" "#FF0000" "$(pixel "$shot2" 220 140)"
grep -qx 'ready' "$log" && echo "  ok   WinMain reached the message loop" || { echo "  FAIL no 'ready' line"; fail=1; }
grep -qx 'clicked 1' "$log" && echo "  ok   button click delivered" || { echo "  FAIL no 'clicked 1' line"; fail=1; }
grep -qx 'keydown 65' "$log" && echo "  ok   WM_KEYDOWN vk=65" || { echo "  FAIL no 'keydown 65' line"; fail=1; }
grep -qx 'char 97' "$log" && echo "  ok   WM_CHAR from TranslateMessage" || { echo "  FAIL no 'char 97' line"; fail=1; }
grep -q '^exit clicks=1 keys=1 ticks=' "$log" && echo "  ok   WM_QUIT path returned from GetMessage" || { echo "  FAIL no exit line"; fail=1; }
grep -qx 'EXIT=0' "$log" && echo "  ok   exit status 0" || { echo "  FAIL exit status"; fail=1; }
if [ $fail -ne 0 ]; then
    echo "core_gui: FAIL (log: $log)" >&2
    tail -20 "$log" >&2
    exit 1
fi
echo "core_gui: ok (screenshots: $shot $shot2)"
