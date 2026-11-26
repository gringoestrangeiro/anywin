#!/bin/sh
# SysListView32 model unit test (S7): rows, sub-items, selection/focus policy, hooks, stable sort
# through an ms_abi comparator, search, randomized invariants. Native build, no X server.
# Exit 0 = PASS, 77 = SKIP (no C compiler), otherwise FAIL.
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-$root/build-s7a/tests/listview_unit}   # binary and logs are kept here
mkdir -p "$work" || exit 1

skip() { echo "listview_unit: SKIP ($1)"; exit 77; }
command -v cc >/dev/null 2>&1 || skip "no cc"

if ! cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
        -o "$work/listview_unit" "$root/tests/listview_unit.c" 2>"$work/build.log"; then
    echo "listview_unit: FAIL (build)"; cat "$work/build.log"; exit 1
fi

status=0
for seed in 12345 7 987654321; do
    if ! "$work/listview_unit" "$seed" 1500 >"$work/run.$seed.log" 2>&1; then
        status=1; cat "$work/run.$seed.log"
    else
        cat "$work/run.$seed.log"
    fi
done

if [ "$status" -eq 0 ]; then echo "listview_unit: PASS"; else echo "listview_unit: FAIL"; fi
exit "$status"
