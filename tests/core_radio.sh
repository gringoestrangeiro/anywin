#!/bin/bash
# Radio-group test: builds tests/core_radio.rc + tests/core_radio_probe.c with mingw, relinks against
# libanywin.o and runs it under xvfb-run (see core_radio_probe.c for the covered semantics).
# Usage: tests/core_radio.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_radio_probe.c
rc=$root/tests/core_radio.rc
res=$build/core_radio.res.o
exe=$build/core_radio.exe
out=$build/core_radio
log=$build/core_radio.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_radio: run make BUILD=$(basename "$build") first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_radio: SKIP (no mingw)" >&2; exit 77; fi
command -v xvfb-run >/dev/null 2>&1 || { echo "core_radio: SKIP (no xvfb-run)" >&2; exit 77; }

x86_64-w64-mingw32-windres -o "$res" "$rc" || { echo "core_radio: windres failed"; exit 1; }
x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" "$res" -mwindows -lcomctl32 -luser32 -lgdi32 || { echo "core_radio: mingw compile failed"; exit 1; }
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_radio: relink failed"; exit 1; }

timeout 60 xvfb-run -a -s "-screen 0 800x600x24" "$out" >"$log" 2>&1
status=$?
cat "$log"
if [ $status -ne 0 ]; then echo "core_radio: FAIL (exit $status)"; exit 1; fi
if grep -q '^FAIL' "$log"; then echo "core_radio: FAIL"; exit 1; fi
grep -q '^core_radio: 0 failures' "$log" || { echo "core_radio: FAIL (no summary)"; exit 1; }
echo "core_radio: PASS"
exit 0
