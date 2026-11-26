#!/bin/bash
# S8 registry (advapi32): relinks tests/core_registry_probe.c and runs its three phases against a
# temporary HOME: "set" (values, sizes, ERROR_MORE_DATA, the Run value), "check" in a new process
# (persistence) and "delete". Also checks the files: ~/.anywin/registry/hkcu.reg and the autostart
# entry ~/.config/autostart/AnyWinProbe.desktop (Exec = this ELF plus the Run value's arguments).
# Usage: tests/core_registry.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_registry_probe.c
exe=$build/core_registry_probe.exe
out=$build/core_registry
home=$build/core_registry_home
status=0

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_registry: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_registry: SKIP (no mingw)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$src" || { echo "core_registry: mingw compile failed"; exit 1; }
# dev link while other packages are incomplete: unresolved references are only stored as pointers
if ! "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1; then
    ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 \
        || { echo "core_registry: relink failed (log: $out.relink.log)"; exit 1; }
fi

rm -rf "$home"
mkdir -p "$home"
check() { # name, exit status of the test
    if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; status=1; fi
}

HOME="$home" "$out" set >"$out.set.log" 2>&1; rc=$?
cat "$out.set.log"
check "set phase" "$rc"

HOME="$home" "$out" check >"$out.check.log" 2>&1; rc=$?
cat "$out.check.log"
check "check phase in a new process" "$rc"

hive=$home/.anywin/registry/hkcu.reg
[ -s "$hive" ] && grep -q 'Greeting' "$hive" && grep -q 'Blob' "$hive"
check "hkcu.reg holds the values" "$?"

desk=$home/.config/autostart/AnyWinProbe.desktop
[ -f "$desk" ] && grep -qxF "Exec=$(realpath "$out") --autostart" "$desk"
check "autostart entry runs this ELF with the Run arguments" "$?"

HOME="$home" "$out" delete >"$out.delete.log" 2>&1; rc=$?
cat "$out.delete.log"
check "delete phase" "$rc"

[ ! -e "$desk" ]
check "deleting the Run value removes the autostart entry" "$?"
! grep -q -e 'Blob' -e 'Greeting' "$hive" 2>/dev/null
check "deleting the values and the key rewrites the hive without them" "$?"

exit $status
