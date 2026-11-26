#!/bin/bash
# Winsock asynchronous name resolution probe: relinks tests/core_wsa_async_probe.c (MinGW) and
# runs it under Xvfb.  Covers WSAAsyncGetHostByName / ByAddr / ServByName / ProtoByName (reply
# layout, error codes, WSAENOBUFS sizing) and WSACancelAsyncRequest.
# Usage: tests/core_wsa_async.sh [build-dir]    exit 0 PASS, 77 SKIP, 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_wsa_async_probe.c
exe=$build/core_wsa_async_probe.exe
out=$build/core_wsa_async
log=$build/core_wsa_async.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_wsa_async: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_wsa_async: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_wsa_async: SKIP (no xvfb-run)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" -lws2_32 || { echo "core_wsa_async: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 || { echo "core_wsa_async: relink failed (log: $out.relink.log)"; exit 1; }
timeout 90 xvfb-run -a "$out" >"$log" 2>&1
rc=$?
cat "$log"
if [ $rc -ne 0 ] || grep -q '^FAIL' "$log" || ! grep -q '^wsa_async_probe: ok$' "$log"; then
    echo "core_wsa_async: FAILED (exit $rc)" >&2
    exit 1
fi
echo "core_wsa_async: ok"
