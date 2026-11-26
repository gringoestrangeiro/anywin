#!/bin/bash
# P0 regression for todo.exe (PLAN-CORE §1) under Xvfb: launch, add, select, edit/save, complete,
# delete, search, column sort, persistence across a restart, no unimplemented calls (ANYWIN_STRICT=1).
# Driven with xdotool; checked from the app's stdout (debug traces), its data file and screenshots.
# Usage: tests/todo_p0.sh [build-dir]     exit 0 PASS, 77 SKIP (no xvfb-run/xdotool/import), else FAIL
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
for t in xvfb-run xdotool import python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "todo_p0: SKIP (no $t)"; exit 77; }
done
"$build/anywin" "$root/todo.exe" -o "$build/todo" >"$build/todo_p0-relink.log" 2>&1 || {
    echo "todo_p0: relink failed (see $build/todo_p0-relink.log)"; exit 1; }

work=$build/tests/todo_p0
rm -rf "$work"; mkdir -p "$work/home/.config"
log=$work/app.log
cat >"$work/inner.sh" <<'INNER'
#!/bin/bash
# runs inside xvfb-run; positions are client coordinates + window origin (50,50), no WM
work=$1; app=$2
pid=
click() { xdotool mousemove $(($1 + 50)) $(($2 + 50)) click 1; sleep 0.5; }
type_() { xdotool type --delay 20 "$1"; sleep 0.2; }
mark() { echo "== $1" >>"$work/app.log"; }
start() {
    "$app" >>"$work/app.log" 2>&1 & pid=$!
    for _ in $(seq 1 60); do xdotool search --name "Todo Application" >/dev/null 2>&1 && break; sleep 0.2; done
    sleep 1
}
stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }
shot() { import -window root -crop 800x560+50+50 +repage "$work/$1.png" 2>/dev/null; }
start
mark add
click 50 440; type_ "Buy milk"; click 140 440; type_ "Eggs"; click 220 440; sleep 0.5
click 50 440; type_ "Walk dog";  click 220 440; sleep 0.5
click 50 440; type_ "Alpha task"; click 220 440; sleep 0.5
shot after_add
mark edit
click 150 83; click 40 479; sleep 0.5
click 50 440; xdotool key ctrl+a; type_ "Buy oat milk"; click 110 479; sleep 0.5
mark complete
click 150 83; click 320 479; sleep 0.5
mark delete
click 150 100; click 250 479; sleep 0.5
mark search
click 250 30; type_ "alpha"; shot after_search
mark clear
xdotool key --repeat 10 BackSpace; sleep 0.5
mark sort
click 125 60; sleep 0.8
shot after_sort
mark restart
stop
start
shot after_restart
mark empty_title
click 220 440; sleep 1
stop
INNER
chmod +x "$work/inner.sh"
xvfb-run -a -s '-screen 0 1280x1024x24' env HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config" \
    ANYWIN_DEBUG=1 ANYWIN_STRICT=1 "$work/inner.sh" "$work" "$build/todo" >"$work/xvfb.log" 2>&1
python3 -I "$root/tests/todo_p0_check.py" "$work" "$log"
