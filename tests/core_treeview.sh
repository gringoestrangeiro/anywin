#!/bin/bash
# Tree view test (SysTreeView32): relinks tests/core_treeview_probe.c against libanywin.o and runs it
# under xvfb-run.  The probe builds a PuTTY-like category tree, drives expansion, selection, hit
# testing, keyboard, mouse, veto, text callbacks, sorting and deletion, and checks the notifications.
# Usage: tests/core_treeview.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_treeview_probe.c
exe=$build/core_treeview.exe
out=$build/core_treeview
log=$build/core_treeview.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_treeview: run make BUILD=$(basename "$build") first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_treeview: SKIP (no mingw)" >&2; exit 77; fi
for t in xvfb-run; do
    command -v "$t" >/dev/null 2>&1 || { echo "core_treeview: SKIP (no $t)" >&2; exit 77; }
done

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" -lcomctl32 || { echo "core_treeview: mingw compile failed"; exit 1; }
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_treeview: relink failed"; exit 1; }

timeout 60 xvfb-run -a -s "-screen 0 800x600x24" "$out" >"$log" 2>&1
status=$?
cat "$log"
if [ $status -ne 0 ]; then echo "core_treeview: FAIL (exit $status)"; exit 1; fi
if grep -q '^FAIL' "$log"; then echo "core_treeview: FAIL"; exit 1; fi
grep -q '^core_treeview: .* 0 failures' "$log" || { echo "core_treeview: FAIL (no summary)"; exit 1; }
echo "core_treeview: PASS"
exit 0
