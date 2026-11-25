#!/bin/bash
# Report imports of a PE that the runtime still does not implement.
# Usage: tests/check_imports.sh [build-dir] [exe...]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
shift 2>/dev/null || true
bin=$build/anywin
rt=$build/libanywin.o
if [ ! -x "$bin" ]; then
    echo "check_imports: $bin missing (run make)" >&2
    exit 2
fi
if [ $# -eq 0 ]; then
    set -- "$root/todo.exe"
fi
rc=0
for exe in "$@"; do
    echo "== $exe"
    out=$("$bin" --list -r "$rt" "$exe" 2>&1)
    echo "$out" | grep '^MISS' || true
    n=$(echo "$out" | grep -c '^MISS')
    echo "-- $n missing"
    [ "$n" -eq 0 ] || rc=1
done
exit $rc
