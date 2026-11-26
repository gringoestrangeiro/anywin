#!/bin/bash
# Keyboard probe: relinks tests/core_keys_probe.c (MinGW) and runs it under Xvfb with xdotool.
# Checks ToUnicodeEx/ToAsciiEx (key-down rules, Shift/Ctrl/CapsLock/NumLock) and that WM_CHAR
# arrives once per key only when the app calls TranslateMessage (not with a bare DispatchMessage).
# Usage: tests/core_keys.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_keys_probe.c
exe=$build/core_keys_probe.exe
log=$build/core_keys.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_keys: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_keys: SKIP (no mingw)" >&2; exit 77; fi
if ! command -v xvfb-run >/dev/null 2>&1; then echo "core_keys: SKIP (no xvfb-run)" >&2; exit 77; fi
if ! command -v xdotool >/dev/null 2>&1; then echo "core_keys: SKIP (no xdotool)" >&2; exit 77; fi

x86_64-w64-mingw32-gcc -O1 -mwindows -o "$exe" "$src" -lgdi32 -luser32 || { echo "core_keys: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$build/core_keys" -r "$rt" >/dev/null 2>&1 || { echo "core_keys: relink failed"; exit 1; }

# drive one run: $1 = mode (tm|notm), output in $build/core_keys_$1.out
drive() {
    local mode=$1 out=$build/core_keys_$1.out
    "$build/core_keys" "$mode" > "$out" 2>&1 &
    local pid=$!
    local i wid=
    for i in $(seq 1 40); do
        wid=$(xdotool search --name keys-probe 2>/dev/null | head -1)
        [ -n "$wid" ] && break
        sleep 0.1
    done
    [ -n "$wid" ] && xdotool windowfocus --sync "$wid" 2>/dev/null
    sleep 0.5
    xdotool type --delay 150 "ab"
    sleep 0.3
    xdotool key --delay 150 Return
    wait $pid
    return $?
}

fail=0
xvfb-run -a -s "-screen 0 1024x768x24" bash -c "$(declare -f drive); build=$build; drive tm; echo rc_tm=\$? > $build/core_keys_rc_tm; drive notm; echo rc_notm=\$? > $build/core_keys_rc_notm" >"$log" 2>&1

grep -q "^ok   \|^translate: passed" "$build/core_keys_tm.out" || true
if grep -q "FAIL" "$build/core_keys_tm.out"; then echo "core_keys: translation checks failed"; fail=1; fi
if ! grep -q "translate: passed" "$build/core_keys_tm.out"; then echo "core_keys: probe did not finish (tm)"; fail=1; fi
# with TranslateMessage: exactly one CH per typed letter
for code in 0061 0062; do
    n=$(grep -c "^CH $code\$" "$build/core_keys_tm.out")
    [ "$n" = 1 ] || { echo "core_keys: CH $code count $n (tm), want 1"; fail=1; }
done
# without TranslateMessage: no WM_CHAR at all, but the key-downs still arrive
if grep -q "^CH " "$build/core_keys_notm.out"; then echo "core_keys: WM_CHAR without TranslateMessage"; fail=1; fi
grep -q "^KD 41" "$build/core_keys_notm.out" || { echo "core_keys: no WM_KEYDOWN for a (notm)"; fail=1; }

[ $fail -eq 0 ] && { echo "core_keys: PASS"; exit 0; }
echo "core_keys: FAIL (see $build/core_keys_tm.out and $build/core_keys_notm.out)"
exit 1
