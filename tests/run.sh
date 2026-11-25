#!/bin/bash
# Run the anywin test suite and print a PASS / FAIL / SKIP summary.
# Usage: tests/run.sh [build-dir]        (make test passes build/)
# Per test: exit 0 = PASS, exit 77 = SKIP (test reports a missing prerequisite), else FAIL.
# A test is reported SKIP up front when its toolchain (mingw, Xvfb) is not installed.
# Logs go to <build-dir>/tests/<name>.log.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
logs=$build/tests
mkdir -p "$logs"

pass=0
fail=0
skip=0

# run NAME [TOOL] -- CMD ARGS...   (TOOL, if given, must be on PATH or the test is SKIPped)
run() {
    local name=$1 tool=
    shift
    if [ "${1:-}" = "--tool" ]; then tool=$2; shift 2; fi
    [ "${1:-}" = "--" ] && shift
    local log=$logs/$name.log st rc
    if [ -n "$tool" ] && ! command -v "$tool" >/dev/null 2>&1; then
        printf '%-20s SKIP (no %s)\n' "$name" "$tool"
        skip=$((skip + 1))
        return
    fi
    "$@" >"$log" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        st=PASS; pass=$((pass + 1))
    elif [ $rc -eq 77 ]; then
        st=SKIP; skip=$((skip + 1))
    else
        st="FAIL(rc=$rc)"; fail=$((fail + 1))
    fi
    printf '%-20s %-12s log: %s\n' "$name" "$st" "$log"
}

run no_overfit       -- "$root/tests/no_overfit.sh"
run check_hooks_core -- "$root/tests/check_hooks_core.sh" "$build"
run check_imports    -- "$root/tests/check_imports.sh" "$build"
run hello_console    --tool x86_64-w64-mingw32-gcc -- "$root/tests/hello_console.sh" "$build"
run hello_gui        --tool Xvfb -- "$root/tests/hello_gui.sh" "$build"
run todo_smoke       --tool Xvfb -- "$root/tests/todo_smoke.sh" "$build"

echo "---- pass=$pass fail=$fail skip=$skip"
[ $fail -eq 0 ]
