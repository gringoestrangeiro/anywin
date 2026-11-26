#!/bin/bash
# Controls test (S7b, PLAN-CORE 3.6 / 5 S7): relinks tests/core_gui_controls_probe.c and drives it under
# Xvfb with xdotool. Covers Static (WM_CTLCOLORSTATIC brush), Edit (typing, Backspace, Ctrl+A, Home,
# Shift+Right, Delete, EN_CHANGE), ComboBox DROPDOWNLIST (keyboard and mouse selection, outside-click
# cancel, CBN_* order) and ComboBox DROPDOWN (editable text, forwarded Down key), Button click.
# Assertions: pixel colour of a root screenshot plus the probe's stdout. No vision.
# Stand-ins: aw_seh_init / aw_seh_signal (S8) are linked as no-ops in the build dir when absent from
# libanywin.o; unresolved references are ignored for the relink (PLAN §4 rule 13).
# Usage: tests/core_gui_controls.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_gui_controls_probe.c
exe=$build/core_gui_controls.exe
out=$build/core_gui_controls
log=$build/core_gui_controls.log
shot_a=$build/core_gui_controls_a.png
shot_b=$build/core_gui_controls_b.png

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_gui_controls: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_gui_controls: SKIP (no mingw)" >&2; exit 77; fi
for t in xvfb-run xdotool import magick; do
    command -v "$t" >/dev/null 2>&1 || { echo "core_gui_controls: SKIP (no $t)" >&2; exit 77; }
done

defd() { nm "$rt" 2>/dev/null | grep -q " T $1\$"; }
rtl=$rt
stubs=""
for s in aw_seh_init aw_seh_signal; do defd "$s" || stubs="$stubs $s"; done
if [ -n "$stubs" ]; then
    stub_src=$build/core_gui_controls_stubs.c
    {
        for s in $stubs; do
            case $s in
            aw_seh_init) echo 'void aw_seh_init(void) {}' ;;
            aw_seh_signal) echo 'int aw_seh_signal(int sig, void *si, void *uc) { (void)sig; (void)si; (void)uc; return 0; }' ;;
            esac
        done
    } >"$stub_src"
    cc -O1 -fno-pie -mcmodel=large -c -o "$build/core_gui_controls_stubs.o" "$stub_src" || { echo "core_gui_controls: stub build failed" >&2; exit 2; }
    ld -r -o "$build/libanywin_core_gui_controls.o" "$rt" "$build/core_gui_controls_stubs.o" || exit 2
    rtl=$build/libanywin_core_gui_controls.o
    echo "core_gui_controls: note: stand-ins used for:$stubs (not in tree yet)"
fi

x86_64-w64-mingw32-gcc -O1 -Wall -mwindows -o "$exe" "$src" || { echo "core_gui_controls: mingw compile failed"; exit 1; }
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rtl" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_gui_controls: relink failed"; exit 1; }
echo "core_gui_controls: relink: $(tail -1 "$out.relink.log" | sed 's/.*(//')"
rm -f "$log" "$shot_a" "$shot_b"

# pixel colour of root (x,y) as #RRGGBB
pixel() {
    magick "$1" -crop "1x1+$2+$3" -depth 8 txt:- 2>/dev/null | awk 'NR==2 {print toupper($3)}'
}

# Main window client origin is root (20,20). Coordinates below are root coordinates.
xvfb-run -a -s "-screen 0 800x600x24" bash -c '
    out=$1 log=$2 shot_a=$3 shot_b=$4
    "$out" >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 80); do
        xdotool search --name "Core Controls Probe" >/dev/null 2>&1 && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 0.8
    import -window root "$shot_a" 2>/dev/null
    # Edit at client (10,50): click, End, type XY, Backspace, Ctrl+A, type new, Home, Shift+Right, Delete
    xdotool mousemove 150 82 click 1; sleep 0.4
    xdotool key End; sleep 0.2
    xdotool type --delay 80 XY; sleep 0.3
    xdotool key BackSpace; sleep 0.3
    xdotool key ctrl+a; sleep 0.2
    xdotool type --delay 80 new; sleep 0.3
    xdotool key Home; xdotool key shift+Right; xdotool key Delete; sleep 0.3
    # ComboBox DROPDOWNLIST at client (10,100): open, Down Down Enter
    xdotool mousemove 100 130 click 1; sleep 0.5
    xdotool key Down; sleep 0.2; xdotool key Down; sleep 0.2; xdotool key Return; sleep 0.4
    # open again and pick row 0 with the mouse; the row centre comes from the last droprect line
    xdotool mousemove 100 130 click 1; sleep 0.5
    dline=$(grep "^combo1 dropdown droprect" "$log" | tail -1)
    dtop=$(echo "$dline" | awk "{print \$5}")
    dih=$(echo "$dline" | sed "s/.*itemh=//")
    xdotool mousemove 100 $((dtop + 1 + dih / 2)) click 1; sleep 0.5
    # editable ComboBox DROPDOWN at client (230,100): type into its edit, then Down (forwarded to the combo)
    xdotool mousemove 300 130 click 1; sleep 0.4
    xdotool type --delay 80 bet; sleep 0.3
    xdotool key Down; sleep 0.3
    # open combo1 and cancel: a press inside the main window but outside the list is delivered to the
    # captured list (a press outside every window is not visible to the app; see the report)
    xdotool mousemove 100 130 click 1; sleep 0.5
    xdotool mousemove 150 82 click 1; sleep 0.5
    # Button at client (10,340)
    xdotool mousemove 80 375 click 1; sleep 0.4
    import -window root "$shot_b" 2>/dev/null
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
' _ "$out" "$log" "$shot_a" "$shot_b"

fail=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "  ok   $1 ($3)"; else echo "  FAIL $1: expected $2, got $3"; fail=1; fi
}
have() { # name pattern
    if grep -qxF -- "$2" "$log"; then echo "  ok   $1"; else echo "  FAIL $1: no line '$2'"; fail=1; fi
}
count() { grep -cxF -- "$1" "$log"; }

if [ ! -f "$shot_a" ] || [ ! -f "$shot_b" ]; then echo "core_gui_controls: no screenshots" >&2; fail=1; fi
grep -qx 'ready' "$log" && echo "  ok   probe reached the message loop" || { echo "  FAIL no 'ready' line"; fail=1; }
check "static background from WM_CTLCOLORSTATIC brush" "#FF0000" "$(pixel "$shot_a" 222 42)"
have "static text read back" "static [Static label]"
have "edit initial text" "edit [hello]"
have "edit focus notification" "edit focus"
have "EN_CHANGE after typing X" "edit change [helloX]"
have "EN_CHANGE after typing Y" "edit change [helloXY]"
have "EN_CHANGE after Backspace" "edit change [helloX]"
have "Ctrl+A then typing replaces the selection" "edit change [new]"
check "Shift+Right + Delete leaves the last change" "edit change [ew]" "$(grep '^edit change' "$log" | tail -1)"
grep -q '^edit killfocus$' "$log" && echo "  ok   edit killfocus when focus moved" || { echo "  FAIL no edit killfocus"; fail=1; }
have "combo1 dropdown opened" "$(grep -m1 '^combo1 dropdown droprect' "$log")"
have "combo1 keyboard: Down -> selchange 1" "combo1 selchange 1"
have "combo1 keyboard: Down -> selchange 2" "combo1 selchange 2"
have "combo1 Enter commits (SELENDOK)" "combo1 selendok"
have "combo1 CLOSEUP after commit" "combo1 closeup"
have "combo1 final selection gamma" "combo1 final=2 text [gamma]"
have "combo1 mouse pick of row 0 -> selchange 0" "combo1 selchange 0"
check "combo1 mouse pick commits alpha (2nd commit line)" "2" "$(count 'combo1 final=0 text [alpha]')"
check "combo1 outside click cancels once" "1" "$(count 'combo1 selendcancel')"
check "combo1 cancel keeps the committed selection" "combo1 final=0 text [alpha]" "$(grep '^combo1 final=' "$log" | tail -1)"
have "combo2 editable: typed text b" "combo2 editchange [b]"
have "combo2 editable: typed text be" "combo2 editchange [be]"
have "combo2 editable: typed text bet" "combo2 editchange [bet]"
have "combo2 Down (forwarded from edit) -> selchange 0" "combo2 selchange 0"
have "combo2 text becomes alpha" "combo2 text [alpha]"
have "button click reaches the parent" "button clicked"
if [ $fail -ne 0 ]; then
    echo "core_gui_controls: FAIL (log: $log)" >&2
    tail -40 "$log" >&2
    exit 1
fi
echo "core_gui_controls: PASS"
exit 0
