#!/bin/bash
# Posted-message delivery probe: relinks tests/core_postq_probe.c (MinGW) and runs it under Xvfb.
# Keys are sent with xdotool (one per WAIT_KEY line the probe prints); the clipboard owner is an
# xclip process, so the clipboard fetch goes over the X selection protocol.
# Usage: tests/core_postq.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac

if [ "${1:-}" = "--inner" ]; then
    # runs inside xvfb-run: drives the probe with keys until it exits
    out=$2; log=$3
    printf '%s' probe-clip-text | xclip -selection clipboard -i
    "$out" >"$log" 2>&1 &
    pid=$!
    sent=0
    moves=0
    for _ in $(seq 1 900); do
        kill -0 "$pid" 2>/dev/null || break
        n=$(grep -c '^WAIT_KEY' "$log")
        m=$(grep -c '^WAIT_MOVE' "$log")
        wid=$(xdotool search --name postq-probe 2>/dev/null | head -1)
        if [ "$n" -gt "$sent" ]; then
            [ -n "$wid" ] && xdotool windowfocus --sync "$wid" 2>/dev/null
            sleep 0.3
            xdotool key a
            sent=$n
        fi
        if [ "$m" -gt "$moves" ]; then
            # the pointer leaves and re-enters the window (WM_SETCURSOR), then a key while the loop runs
            [ -n "$wid" ] && xdotool windowfocus --sync "$wid" 2>/dev/null
            xdotool mousemove 1 1
            sleep 0.3
            xdotool mousemove --window "$wid" 60 60
            sleep 0.7
            xdotool key a
            moves=$m
        fi
        sleep 0.1
    done
    wait "$pid"
    exit $?
fi

bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_postq_probe.c
exe=$build/core_postq_probe.exe
out=$build/core_postq
log=$build/core_postq.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_postq: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_postq: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_postq: SKIP (no xvfb-run)" >&2; exit 77; fi
if ! command -v xdotool >/dev/null 2>&1; then echo "core_postq: SKIP (no xdotool)" >&2; exit 77; fi
if ! command -v xclip >/dev/null 2>&1; then echo "core_postq: SKIP (no xclip)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -Wall -mwindows -o "$exe" "$src" -lgdi32 -luser32 || { echo "core_postq: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 || { echo "core_postq: relink failed (log: $out.relink.log)"; exit 1; }
timeout 120 xvfb-run -a bash "$0" --inner "$out" "$log.run" >/dev/null 2>&1
rc=$?
cp "$log.run" "$log" 2>/dev/null
cat "$log"
if [ $rc -ne 0 ] || grep -q '^FAIL' "$log" || ! grep -q '^postq_probe: ok$' "$log"; then
    echo "core_postq: FAILED (exit $rc, log: $log)" >&2
    exit 1
fi
echo "core_postq: ok"
