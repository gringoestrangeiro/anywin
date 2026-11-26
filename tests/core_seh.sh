#!/bin/bash
# S8 SEH. (1) An unhandled host fault (MinGW probe) ends in the runtime's crash report: the
# exception code, RIP and RVA, stdout kept, the process killed by SIGSEGV (status 139). An
# UnhandledExceptionFilter set by the program runs first. (2) C __try/__except/__finally and
# RaiseException (tests/core_seh_try_probe.c, compiled by clang for the mingw target because gcc
# has no __try; skipped when clang is missing).
# Usage: tests/core_seh.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
status=0
work=$build/core_seh
mkdir -p "$work"

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_seh: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_seh: SKIP (no mingw)" >&2; exit 77; fi

check() { # name, status
    if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; status=1; fi
}
relink() { # src.exe out.elf
    if "$bin" "$1" -o "$2" -r "$rt" >"$2.relink.log" 2>&1; then return 0; fi
    # dev link while other packages are incomplete (their references are only stored as pointers)
    ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$1" -o "$2" -r "$rt" >"$2.relink.log" 2>&1
}

# ---- (1) unhandled fault: crash report, exit by signal ---------------------------------------
x86_64-w64-mingw32-gcc -O1 -o "$work/crash.exe" "$root/tests/core_seh_crash_probe.c" \
    || { echo "core_seh: mingw compile failed"; exit 1; }
relink "$work/crash.exe" "$work/crash" || { echo "core_seh: relink failed (log: $work/crash.relink.log)"; exit 1; }

(ulimit -c 0; "$work/crash" >"$work/crash.out" 2>"$work/crash.err"); rc=$?
check "plain fault: stdout written before the fault" "$(grep -qx before "$work/crash.out"; echo $?)"
check "plain fault: report names exception 0xc0000005" "$(grep -q 'unhandled exception 0xc0000005' "$work/crash.err"; echo $?)"
check "plain fault: report gives RIP and RVA in the PE image" \
    "$(grep -Eq 'rip=0x[0-9a-f]+ in PE image, rva=0x[0-9a-f]+' "$work/crash.err"; echo $?)"
check "plain fault: process ends by SIGSEGV (status 139)" "$([ "$rc" = 139 ]; echo $?)"

(ulimit -c 0; "$work/crash" filter >"$work/filter.out" 2>"$work/filter.err"); rc=$?
check "filter: UnhandledExceptionFilter runs first" "$(grep -qx 'filter code=c0000005' "$work/filter.out"; echo $?)"
check "filter: report still printed after the filter" "$(grep -Eq 'rva=0x' "$work/filter.err"; echo $?)"
check "filter: process ends by SIGSEGV (status 139)" "$([ "$rc" = 139 ]; echo $?)"

# ---- (2) C structured exceptions: clang-built probe ------------------------------------------
if ! command -v clang >/dev/null 2>&1; then
    echo "core_seh: SKIP try/except probe (no clang)" >&2
else
    if clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -c "$root/tests/core_seh_try_probe.c" \
        -o "$work/try.o" 2>"$work/try.cc.log" \
        && x86_64-w64-mingw32-gcc -O1 -o "$work/try.exe" "$work/try.o"; then
        relink "$work/try.exe" "$work/try" || { echo "core_seh: relink failed (log: $work/try.relink.log)"; exit 1; }
        "$work/try" >"$work/try.out" 2>"$work/try.err"; rc=$?
        cat "$work/try.out"
        check "try/except probe: all cases pass" "$([ "$rc" = 0 ] && [ "$(grep -c '^PASS' "$work/try.out")" = 3 ]; echo $?)"
    else
        echo "core_seh: SKIP try/except probe (clang could not build it, see $work/try.cc.log)" >&2
    fi
fi

exit $status
