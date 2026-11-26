#!/bin/sh
# S4 region unit test: rgn_* rect-list operations vs a bitmap oracle, native build, no X server.
# Exit 0 = PASS, 77 = SKIP (no C compiler), otherwise FAIL.
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-$root/build-s4/tests/region_unit}   # outputs (binary, PPM, expectations) are kept here
mkdir -p "$work" || exit 1

skip() { echo "region_unit: SKIP ($1)"; exit 77; }
command -v cc >/dev/null 2>&1 || skip "no cc"

if ! cc -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -o "$work/region_unit" \
        "$root/tests/region_unit.c" 2>"$work/build.log"; then
    echo "region_unit: FAIL (build)"; cat "$work/build.log"; exit 1
fi

status=0
for seed in 12345 7 987654321; do
    if ! "$work/region_unit" "$seed" 400; then status=1; fi
done
# a long run on one seed to exercise larger rect lists
"$work/region_unit" 2024 3000 || status=1

if [ "$status" -eq 0 ]; then echo "region_unit: PASS"; else echo "region_unit: FAIL"; fi
exit "$status"
