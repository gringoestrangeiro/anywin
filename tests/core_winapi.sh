#!/bin/bash
# W-variant and system surface added with the file/service/token/shell packages: relinks
# tests/core_winapi_probe.c and runs it against a temporary HOME and work directory. Covers heap,
# temp and current directory paths, CreateFileW/ReadFile/FindFirstFileExW/MoveFileExW, the
# registry W functions, known folders, EnumServicesStatusExW, tokens, FormatMessageW,
# accelerators, SystemParametersInfoW, QueryPerformanceCounter and Toolhelp.
# Usage: tests/core_winapi.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_winapi_probe.c
exe=$build/core_winapi_probe.exe
out=$build/core_winapi
home=$build/core_winapi_home
work=$build/core_winapi_work
status=0

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_winapi: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_winapi: SKIP (no mingw)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" -lshell32 -lole32 -luuid -ladvapi32 \
    || { echo "core_winapi: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
    || { echo "core_winapi: relink failed (log: $out.relink.log)"; exit 1; }

rm -rf "$home" "$work"
mkdir -p "$home/.config" "$work"
winwork="Z:$(echo "$work" | sed 's#/#\\#g')"

HOME="$home" "$out" "$winwork" >"$out.run.log" 2>&1; rc=$?
cat "$out.run.log"
if [ "$rc" = 0 ]; then echo "PASS core_winapi probe"; else echo "FAIL core_winapi probe (rc=$rc)"; status=1; fi
if grep -q "unimplemented" "$out.run.log"; then echo "FAIL core_winapi: unimplemented calls"; status=1; fi
exit $status
