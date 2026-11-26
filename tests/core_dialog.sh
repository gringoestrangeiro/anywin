#!/bin/bash
# Dialog test: builds tests/core_dialog.rc + tests/core_dialog_probe.c with mingw, relinks against
# libanywin.o and runs it under xvfb-run.  Covers the A and W dialog calls (charset of dialogs and
# their controls, WM_NOTIFYFORMAT, tree notifications, text helpers, visibility).
# Usage: tests/core_dialog.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_dialog_probe.c
rc=$root/tests/core_dialog.rc
res=$build/core_dialog.res.o
exe=$build/core_dialog.exe
out=$build/core_dialog
log=$build/core_dialog.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_dialog: run make BUILD=$(basename "$build") first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_dialog: SKIP (no mingw)" >&2; exit 77; fi
for t in xvfb-run; do
    command -v "$t" >/dev/null 2>&1 || { echo "core_dialog: SKIP (no $t)" >&2; exit 77; }
done

x86_64-w64-mingw32-windres -o "$res" "$rc" || { echo "core_dialog: windres failed"; exit 1; }
x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" "$res" -lcomctl32 || { echo "core_dialog: mingw compile failed"; exit 1; }
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_dialog: relink failed"; exit 1; }

timeout 60 xvfb-run -a -s "-screen 0 800x600x24" "$out" >"$log" 2>&1
status=$?
cat "$log"
if [ $status -ne 0 ]; then echo "core_dialog: FAIL (exit $status)"; exit 1; fi
if grep -q '^FAIL' "$log"; then echo "core_dialog: FAIL"; exit 1; fi
grep -q '^core_dialog: .* 0 failures' "$log" || { echo "core_dialog: FAIL (no summary)"; exit 1; }
echo "core_dialog: PASS"
exit 0
