#!/bin/bash
# S6 popup menus + MessageBox: relinks tests/core_menu_probe.c and drives it under Xvfb with xdotool.
# Checks: TrackPopupMenu RETURNCMD with keyboard (Down, Return), mnemonic 'h', submenu (Right, Esc),
# WM_COMMAND delivery of a non-RETURNCMD menu, cancel with Esc, MessageBoxA OK / YESNO (default No) /
# ABORTRETRYIGNORE (Esc), highlight and dialog colours in screenshots, and the no-display path
# (stderr text and default button).  Assertions use the probe's stdout, stderr and pixel colours.
# Usage: tests/core_menu.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_menu_probe.c
exe=$build/core_menu_probe.exe
out=$build/core_menu
log=$build/core_menu.log
nolog=$build/core_menu_nodisplay.log
shot_popup=$build/core_menu_popup.png
shot_box=$build/core_menu_box.png
shot_hover=$build/core_menu_hover.png

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_menu: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_menu: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_menu: SKIP (no xvfb-run)" >&2; exit 77; fi
if ! command -v xdotool >/dev/null 2>&1; then echo "core_menu: SKIP (no xdotool)" >&2; exit 77; fi
if ! command -v magick >/dev/null 2>&1; then echo "core_menu: SKIP (no magick)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -mwindows -o "$exe" "$src" || { echo "core_menu: mingw compile failed"; exit 1; }
# dev link: packages that are not in the tree yet (SEH, controls procs) may be missing from libanywin.o.
# Their entry points are replaced by no-op stand-ins in build/devstub (never in the sources), and the
# remaining unresolved references are allowed at link time (they are only stored as pointers).
rtl=$rt
devstub=$build/devstub
if nm -u "$rt" | grep -q ' aw_seh_init$'; then
    mkdir -p "$devstub"
    printf '%s\n' '/* dev-only stand-ins for runtime entry points that are not linked yet */' \
        'void aw_seh_init(void) {}' 'void aw_seh_signal(void) {}' >"$devstub/standin.c"
    cc -O1 -c -o "$devstub/standin.o" "$devstub/standin.c" && ld -r -o "$devstub/libanywin_dev.o" "$rt" "$devstub/standin.o" && rtl=$devstub/libanywin_dev.o
fi
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rtl" >"$out.relink.log" 2>&1 || { echo "core_menu: relink failed (log: $out.relink.log)"; exit 1; }
rm -f "$log" "$nolog" "$shot_popup" "$shot_box" "$shot_hover"

# does the screenshot contain the colour #RRGGBB anywhere?
has_colour() { magick "$1" -depth 8 txt:- 2>/dev/null | grep -qi "#$2"; }

xvfb-run -a -s "-screen 0 800x600x24" bash -c '
    out=$1 log=$2 shot_popup=$3 shot_box=$4 shot_hover=$5
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
    # wait until the line $1 has been printed at least $2 times
    wait_count() {
        for _ in $(seq 1 100); do
            [ "$(grep -cx "$1" "$log" 2>/dev/null)" -ge "$2" ] && return 0
            kill -0 "$pid" 2>/dev/null || return 1
            sleep 0.1
        done
        return 1
    }
    wait_line ready || echo "NOREADY" >>"$log"
    sleep 0.5
    # 1. left click opens the menu (RETURNCMD); Down Down selects Hide; Return picks it
    xdotool mousemove 100 100 click 1
    sleep 0.6
    xdotool key Down
    sleep 0.3
    import -window root "$shot_popup" 2>/dev/null
    xdotool key Down Return
    wait_line "ret 102"
    # 2. again; the mnemonic h selects the single match "&Hide"
    xdotool mousemove 100 100 click 1
    sleep 0.6
    xdotool key h
    wait_count "ret 102" 2
    # 3. right click: the menu posts WM_COMMAND. Down x4 reaches More, Right opens it,
    #    Down Down reaches Gamma (203), Return
    xdotool mousemove 100 100 click 3
    sleep 0.6
    xdotool key Down Down Down Down Right
    sleep 0.3
    xdotool key Down Down Return
    wait_line "command 203"
    # 4. left click, hover over the first row (Show): the row is highlighted; Esc cancels (returns 0)
    xdotool mousemove 100 100 click 1
    sleep 0.6
    xdotool mousemove 150 112
    sleep 0.4
    import -window root "$shot_hover" 2>/dev/null
    xdotool key Escape
    wait_line "ret 0"
    # 4b. left click on the popup, then press and release on the first row: returns Show (101)
    xdotool mousemove 100 100 click 1
    sleep 0.6
    xdotool mousemove 150 112 click 1
    wait_line "ret 101"
    # 5. MessageBox OK: key m in the main window, Return on the default button
    xdotool mousemove 200 150
    xdotool key m
    sleep 0.8
    import -window root "$shot_box" 2>/dev/null
    xdotool mousemove 200 150 key Return
    wait_line "msgbox 1"
    # 6. YESNO with DEFBUTTON2: Return answers No (7)
    xdotool key n
    sleep 0.8
    xdotool mousemove 200 150 key Return
    wait_line "msgbox 7"
    # 7. ABORTRETRYIGNORE: Escape has no Cancel button, so it answers the default Abort (3)
    xdotool key e
    sleep 0.8
    xdotool mousemove 200 150 key Escape
    wait_line "msgbox 3"
    xdotool key q
    for _ in $(seq 1 80); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then kill "$pid" 2>/dev/null; echo "HUNG" >>"$log"; fi
    wait "$pid"
    echo "EXIT=$?" >>"$log"
' _ "$out" "$log" "$shot_popup" "$shot_box" "$shot_hover"

fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: expected $2, got $3"; fail=1; fi
}
line_count() { grep -cx "$1" "$log" 2>/dev/null; }

grep -qx 'ready' "$log" && echo "  ok   probe reached the message loop" || { echo "  FAIL no 'ready' line"; fail=1; }
check "popup RETURNCMD: Down Down Return and mnemonic h both return Hide (102)" 2 "$(line_count 'ret 102')"
grep -qx 'command 203' "$log" && echo "  ok   submenu item delivered as WM_COMMAND 203" || { echo "  FAIL no 'command 203'"; fail=1; }
grep -qx 'ret 0' "$log" && echo "  ok   Escape cancels the menu (returns 0)" || { echo "  FAIL no 'ret 0'"; fail=1; }
grep -qx 'ret 101' "$log" && echo "  ok   mouse press+release on a row activates it (101)" || { echo "  FAIL no 'ret 101'"; fail=1; }
if [ -f "$shot_hover" ] && [ "$(magick "$shot_hover" -crop '1x1+110+112' -depth 8 txt:- 2>/dev/null | awk 'NR==2 {print toupper($3)}')" = "#0078D7" ]; then
    echo "  ok   mouse hover selects the row under the pointer"
else
    echo "  FAIL mouse hover did not highlight the row under the pointer"; fail=1
fi
grep -qx 'msgbox 1' "$log" && echo "  ok   MessageBox OK -> IDOK" || { echo "  FAIL no 'msgbox 1'"; fail=1; }
grep -qx 'msgbox 7' "$log" && echo "  ok   MessageBox YESNO default 2 -> IDNO" || { echo "  FAIL no 'msgbox 7'"; fail=1; }
grep -qx 'msgbox 3' "$log" && echo "  ok   MessageBox ABORTRETRYIGNORE Esc -> IDABORT" || { echo "  FAIL no 'msgbox 3'"; fail=1; }
grep -qx 'NOREADY' "$log" && { echo "  FAIL probe never printed ready"; fail=1; }
grep -qx 'HUNG' "$log" && { echo "  FAIL probe hung"; fail=1; }
grep -qx 'EXIT=0' "$log" && echo "  ok   probe exit status 0" || { echo "  FAIL exit status"; tail -20 "$log" >&2; fail=1; }
if [ -f "$shot_popup" ] && has_colour "$shot_popup" "0078D7"; then
    echo "  ok   popup highlight (#0078D7) drawn"
else
    echo "  FAIL popup highlight colour not on screen"; fail=1
fi
if [ -f "$shot_box" ] && has_colour "$shot_box" "0078D7"; then
    echo "  ok   MessageBox default button outline drawn"
else
    echo "  FAIL MessageBox not drawn"; fail=1
fi

# no display: MessageBox prints to stderr and returns the default button (IDOK = 1)
env -u DISPLAY "$out" nodisplay >"$nolog" 2>&1 </dev/null
grep -q 'Probe: Field is empty.' "$nolog" && echo "  ok   no display: text on stderr" || { echo "  FAIL no-display stderr"; fail=1; }
grep -qx 'nodisplay 1' "$nolog" && echo "  ok   no display: default button returned" || { echo "  FAIL no-display return"; fail=1; }

if [ $fail -ne 0 ]; then
    echo "core_menu: FAIL (log: $log)" >&2
    tail -20 "$log" >&2
    exit 1
fi
echo "core_menu: ok (screenshots: $shot_popup $shot_box)"
