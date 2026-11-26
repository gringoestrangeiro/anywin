#!/bin/bash
# Winsock async notification probe: relinks tests/core_wsa_probe.c (MinGW) and runs it under
# Xvfb.  Covers WSAAsyncSelect messages (accept/connect/write/read/close, error codes, cancel),
# WSAEventSelect and its wait/enumerate calls, cross-thread wake-up and idle CPU use.
# Usage: tests/core_wsa.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_wsa_probe.c
exe=$build/core_wsa_probe.exe
out=$build/core_wsa
log=$build/core_wsa.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_wsa: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_wsa: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_wsa: SKIP (no xvfb-run)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -o "$exe" "$src" -lws2_32 || { echo "core_wsa: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 || { echo "core_wsa: relink failed (log: $out.relink.log)"; exit 1; }
timeout 90 xvfb-run -a "$out" >"$log" 2>&1
rc=$?
cat "$log"
if [ $rc -ne 0 ] || grep -q '^FAIL' "$log" || ! grep -q '^wsa_async: ok$' "$log"; then
    echo "core_wsa: FAILED (exit $rc)" >&2
    exit 1
fi
echo "core_wsa: ok"
