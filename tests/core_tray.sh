#!/bin/bash
# S8 tray (shell32): relinks tests/core_tray_probe.c and checks Shell_NotifyIconA's bookkeeping.
# Runs without a display, so there is no notification area: every valid call must still return TRUE.
# Usage: tests/core_tray.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
exe=$build/core_tray_probe.exe
out=$build/core_tray
if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_tray: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_tray: SKIP (no mingw)" >&2; exit 77; fi
x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$root/tests/core_tray_probe.c" || { echo "core_tray: mingw compile failed"; exit 1; }
if ! "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1; then
    ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
        || { echo "core_tray: relink failed (log: $out.relink.log)"; exit 1; }
fi
env -u DISPLAY "$out"
