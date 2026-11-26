#!/bin/bash
# ListBox test: relinks tests/core_listbox_probe.c against libanywin.o and runs it under xvfb-run.
# Usage: tests/core_listbox.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_listbox_probe.c
exe=$build/core_listbox.exe
out=$build/core_listbox
log=$build/core_listbox.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_listbox: run make BUILD=$(basename "$build") first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_listbox: SKIP (no mingw)" >&2; exit 77; fi
for t in xvfb-run; do
    command -v "$t" >/dev/null 2>&1 || { echo "core_listbox: SKIP (no $t)" >&2; exit 77; }
done

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" -lcomctl32 || { echo "core_listbox: mingw compile failed"; exit 1; }
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { cat "$out.relink.log" >&2; echo "core_listbox: relink failed"; exit 1; }

timeout 60 xvfb-run -a -s "-screen 0 800x600x24" "$out" >"$log" 2>&1
status=$?
cat "$log"
if [ $status -ne 0 ]; then echo "core_listbox: FAIL (exit $status)"; exit 1; fi
if grep -q '^FAIL' "$log"; then echo "core_listbox: FAIL"; exit 1; fi
grep -q '^core_listbox: .* 0 failures' "$log" || { echo "core_listbox: FAIL (no summary)"; exit 1; }
echo "core_listbox: PASS"
exit 0
