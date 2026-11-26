#!/bin/bash
# Child z-order and hit-testing test: builds tests/core_zorder_probe.c with mingw, relinks against
# libanywin.o and runs it under xvfb-run (see core_zorder_probe.c for the covered semantics).
# Usage: tests/core_zorder.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_zorder_probe.c

exe=$build/core_zorder.exe
out=$build/core_zorder
log=$build/core_zorder.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_zorder: run make BUILD=$(basename "$build") first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_zorder: SKIP (no mingw)" >&2; exit 77; fi
command -v xvfb-run >/dev/null 2>&1 || { echo "core_zorder: SKIP (no xvfb-run)" >&2; exit 77; }

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" -mwindows -luser32 -lgdi32 || { echo "core_zorder: mingw compile failed"; exit 1; }
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_zorder: relink failed"; exit 1; }

timeout 60 xvfb-run -a -s "-screen 0 800x600x24" "$out" >"$log" 2>&1
status=$?
cat "$log"
if [ $status -ne 0 ]; then echo "core_zorder: FAIL (exit $status)"; exit 1; fi
if grep -q '^FAIL' "$log"; then echo "core_zorder: FAIL"; exit 1; fi
grep -q '^core_zorder: 0 failures' "$log" || { echo "core_zorder: FAIL (no summary)"; exit 1; }
echo "core_zorder: PASS"
exit 0
