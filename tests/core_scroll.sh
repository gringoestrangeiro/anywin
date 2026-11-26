#!/bin/bash
# S6 part A: standard scroll bars. Relinks tests/core_scroll_probe.c and drives it under Xvfb with xdotool.
# Checks: NC paint of the vertical bar (thumb and track pixel colours of a top-level window and of a child
# window), arrow click -> WM_VSCROLL SB_LINEDOWN with the new position, track click -> SB_PAGEDOWN,
# thumb drag -> SB_THUMBTRACK ... SB_THUMBPOSITION, and ShowScrollBar changing the child's client width.
# Assertions use the probe's stdout and pixel colours. No vision.
# Usage: tests/core_scroll.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_scroll_probe.c
exe=$build/core_scroll_probe.exe
out=$build/core_scroll
log=$build/core_scroll.log
shot=$build/core_scroll.png

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_scroll: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_scroll: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_scroll: SKIP (no xvfb-run)" >&2; exit 77; fi
if ! command -v xdotool >/dev/null 2>&1; then echo "core_scroll: SKIP (no xdotool)" >&2; exit 77; fi
if ! command -v magick >/dev/null 2>&1; then echo "core_scroll: SKIP (no magick)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -mwindows -o "$exe" "$src" || { echo "core_scroll: mingw compile failed"; exit 1; }
# dev link: see tests/core_menu.sh (stand-ins for SEH entry points that are not linked yet)
rtl=$rt
devstub=$build/devstub
if nm -u "$rt" | grep -q ' aw_seh_init$'; then
    mkdir -p "$devstub"
    printf '%s\n' '/* dev-only stand-ins for runtime entry points that are not linked yet */' \
        'void aw_seh_init(void) {}' 'void aw_seh_signal(void) {}' >"$devstub/standin.c"
    cc -O1 -c -o "$devstub/standin.o" "$devstub/standin.c" && ld -r -o "$devstub/libanywin_dev.o" "$rt" "$devstub/standin.o" && rtl=$devstub/libanywin_dev.o
fi
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rtl" >"$out.relink.log" 2>&1 || { echo "core_scroll: relink failed (log: $out.relink.log)"; exit 1; }
rm -f "$log" "$shot"

xvfb-run -a -s "-screen 0 800x600x24" bash -c '
    out=$1 log=$2 shot=$3
    "$out" >"$log" 2>&1 &
    pid=$!
    # wait until the probe prints the line $1 (or dies / times out)
    wait_line() {
        for _ in $(seq 1 100); do
            grep -qx "$1" "$log" 2>/dev/null && return 0
            kill -0 "$pid" 2>/dev/null || return 1
            sleep 0.1
        done
        return 1
    }
    wait_line ready || echo "NOREADY" >>"$log"
    sleep 0.6
    import -window root "$shot" 2>/dev/null
    # 1. down arrow: the bottom 17 px of the vertical bar (x 323..339, y 40..239)
    xdotool mousemove 331 232 click 1
    wait_line "vscroll LINEDOWN 1"
    # 2. track below the thumb: page down
    xdotool mousemove 331 200 click 1
    wait_line "vscroll PAGEDOWN 11"
    # 3. drag the thumb (at y 75..92 after the page) down to y 150
    xdotool mousemove 331 85 mousedown 1 mousemove 331 110 mousemove 331 150 mouseup 1
    wait_line "vscroll THUMBPOSITION" || true
    sleep 0.3
    # 4. ShowScrollBar on the child; keys go to the focused top-level window
    xdotool key h
    wait_line "child w 200"
    xdotool key s
    wait_line "child w 183"
    xdotool key q
    for _ in $(seq 1 80); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then kill "$pid" 2>/dev/null; echo "HUNG" >>"$log"; fi
    wait "$pid"
    echo "EXIT=$?" >>"$log"
' _ "$out" "$log" "$shot"

fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "  ok   $1 ($3)"; else echo "  FAIL $1: expected $2, got $3"; fail=1; fi
}
# colour of root pixel (x,y) as #RRGGBB, taken from the screenshot made before any input
pixel() { magick "$shot" -crop "1x1+$1+$2" -depth 8 txt:- 2>/dev/null | awk 'NR==2 {print toupper($3)}'; }

if [ ! -f "$shot" ]; then echo "core_scroll: no screenshot" >&2; fail=1; fi
grep -qx 'ready' "$log" && echo "  ok   probe reached the message loop" || { echo "  FAIL no 'ready' line"; fail=1; }
check "top-level vertical thumb painted in the NC area" "#CDCDCD" "$(pixel 330 65)"
check "top-level vertical track painted" "#F1F1F1" "$(pixel 330 150)"
check "child vertical thumb painted in the child NC area" "#CDCDCD" "$(pixel 240 75)"
check "child vertical track painted" "#F1F1F1" "$(pixel 240 120)"
check "arrow click -> SB_LINEDOWN, position 1" 1 "$(grep -cx 'vscroll LINEDOWN 1' "$log")"
check "track click -> SB_PAGEDOWN, position 11" 1 "$(grep -cx 'vscroll PAGEDOWN 11' "$log")"
check "thumb drag -> SB_THUMBTRACK messages" "yes" "$(grep -q '^vscroll THUMBTRACK ' "$log" && echo yes || echo no)"
dragged=$(grep '^vscroll THUMBPOSITION ' "$log" | tail -1 | awk '{print $3}')
if [ -n "$dragged" ] && [ "$dragged" -gt 11 ] 2>/dev/null; then
    echo "  ok   thumb drag -> SB_THUMBPOSITION at $dragged (> 11)"
else
    echo "  FAIL thumb drag position (got '${dragged:-none}')"; fail=1
fi
grep -qx 'child w 200' "$log" && echo "  ok   ShowScrollBar(hide) grows the child client to 200" || { echo "  FAIL child client after hide"; fail=1; }
grep -qx 'child w 183' "$log" && echo "  ok   ShowScrollBar(show) shrinks the child client to 183" || { echo "  FAIL child client after show"; fail=1; }
grep -qx 'NOREADY' "$log" && { echo "  FAIL probe never printed ready"; fail=1; }
grep -qx 'HUNG' "$log" && { echo "  FAIL probe hung"; fail=1; }
grep -qx 'EXIT=0' "$log" && echo "  ok   probe exit status 0" || { echo "  FAIL exit status"; tail -20 "$log" >&2; fail=1; }

if [ $fail -ne 0 ]; then
    echo "core_scroll: FAIL (log: $log)" >&2
    tail -30 "$log" >&2
    exit 1
fi
echo "core_scroll: ok (screenshot: $shot)"
