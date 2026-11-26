#!/bin/bash
# Winsock 2 end-to-end test: compile a MinGW probe, relink it, and make real
# TCP/UDP loopback connections through the runtime's ws2_32 implementation.
# Usage: tests/winsock.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
exe=$build/winsock_probe.exe
out=$build/winsock_probe
log=$build/winsock_probe.log
if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "winsock: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "winsock: SKIP (no mingw)" >&2; exit 77; fi
x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$root/tests/winsock_probe.c" -lws2_32 \
    || { echo "winsock: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { echo "winsock: relink failed (log: $out.relink.log)"; exit 1; }
timeout 30 "$out" >"$log" 2>&1
rc=$?
cat "$log"
if [ $rc -ne 0 ] || grep -q '^FAIL' "$log" || ! grep -q '^winsock: ok$' "$log"; then
    echo "winsock: FAILED (exit $rc)" >&2
    exit 1
fi
echo "winsock: ok"
