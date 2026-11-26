#!/bin/bash
# WM_SIZE test: relinks tests/core_wsize_probe.c against libanywin.o and runs it under xvfb-run.
# Checks that WM_SIZE carries the client size (never the outer size) and that GetClientRect already
# reports it when WM_SIZE arrives (first show, resize, scroll bar removed).
# Usage: tests/core_wsize.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_wsize_probe.c
exe=$build/core_wsize.exe
out=$build/core_wsize
log=$build/core_wsize.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_wsize: run make BUILD=$(basename "$build") first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_wsize: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_wsize: SKIP (no xvfb-run)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" || { echo "core_wsize: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_wsize: relink failed"; exit 1; }

timeout 60 xvfb-run -a -s "-screen 0 800x600x24" "$out" >"$log" 2>&1
status=$?
cat "$log"
if [ $status -ne 0 ]; then echo "core_wsize: FAIL (exit $status)"; exit 1; fi
if grep -q '^FAIL' "$log"; then echo "core_wsize: FAIL"; exit 1; fi
grep -q '^core_wsize: .* 0 failures' "$log" || { echo "core_wsize: FAIL (no summary)"; exit 1; }
echo "core_wsize: PASS"
exit 0
