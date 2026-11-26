#!/bin/bash
# Combo box sizing and show-after-move paint test: builds tests/core_combo_probe.c with mingw, relinks
# against libanywin.o and runs it under xvfb-run (see core_combo_probe.c for the covered semantics).
# Usage: tests/core_combo.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_combo_probe.c

exe=$build/core_combo.exe
out=$build/core_combo
log=$build/core_combo.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_combo: run make BUILD=$(basename "$build") first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_combo: SKIP (no mingw)" >&2; exit 77; fi
command -v xvfb-run >/dev/null 2>&1 || { echo "core_combo: SKIP (no xvfb-run)" >&2; exit 77; }

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" -mwindows -lcomctl32 -luser32 -lgdi32 || { echo "core_combo: mingw compile failed"; exit 1; }
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_combo: relink failed"; exit 1; }

timeout 60 xvfb-run -a -s "-screen 0 800x600x24" "$out" >"$log" 2>&1
status=$?
cat "$log"
if [ $status -ne 0 ]; then echo "core_combo: FAIL (exit $status)"; exit 1; fi
if grep -q '^FAIL' "$log"; then echo "core_combo: FAIL"; exit 1; fi
grep -q '^core_combo: 0 failures' "$log" || { echo "core_combo: FAIL (no summary)"; exit 1; }
echo "core_combo: PASS"
exit 0
