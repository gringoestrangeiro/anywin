#!/bin/bash
# Build todo.exe with anywin and check that it opens its main window under Xvfb.
# Usage: tests/todo_smoke.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
out=$build/todo
log=$build/todo-smoke.log
shot=$build/todo-smoke.png

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then
    echo "todo_smoke: build $build missing (run make)" >&2
    exit 2
fi
if ! "$bin" "$root/todo.exe" -o "$out" -r "$rt"; then
    echo "todo_smoke: relink failed" >&2
    exit 1
fi
rm -f "$log" "$shot"
xvfb-run -a -s "-screen 0 1280x1024x24" bash -c '
    out=$1 log=$2 shot=$3
    ANYWIN_DEBUG=${ANYWIN_DEBUG:-1} "$out" >"$log" 2>&1 &
    pid=$!
    found=0
    for _ in $(seq 1 100); do
        if xdotool search --name "Todo Application" >/dev/null 2>&1; then
            found=1
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 0.5
    import -window root "$shot" 2>/dev/null || true
    if [ "$found" = 1 ]; then
        xdotool search --name "Todo Application" getwindowgeometry --shell 2>/dev/null | head -5
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        exit 0
    fi
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    exit 1
' _ "$out" "$log" "$shot"
rc=$?
if [ $rc -ne 0 ]; then
    echo "todo_smoke: no Todo Application window (log: $log)" >&2
    [ -f "$log" ] && tail -20 "$log" >&2
fi
exit $rc
