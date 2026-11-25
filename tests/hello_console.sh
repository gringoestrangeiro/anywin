#!/bin/bash
# Compile a minimal MinGW console program, relink it and run it.
# This is the first end-to-end milestone (kernel32 + msvcrt + startup).
# Usage: tests/hello_console.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$build/hello_console.c
exe=$build/hello_console.exe
out=$build/hello_console
log=$build/hello_console.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "hello_console: run make first" >&2; exit 2; fi
cat >"$src" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    printf("hello argc=%d\n", argc);
    printf("argv0=%s\n", argv[0]);
    char *p = malloc(64);
    strcpy(p, "heap-ok");
    puts(p);
    free(p);
    return 7;
}
EOF
x86_64-w64-mingw32-gcc -O1 -o "$exe" "$src" || exit 1
"$bin" "$exe" -o "$out" -r "$rt" >/dev/null 2>&1 || { echo "hello_console: relink failed"; exit 1; }
timeout 20 "$out" >"$log" 2>&1
rc=$?
cat "$log"
if ! grep -q '^hello argc=1$' "$log" || ! grep -q '^heap-ok$' "$log"; then
    echo "hello_console: FAILED (exit $rc, log above)" >&2
    exit 1
fi
if [ $rc -ne 7 ]; then
    echo "hello_console: FAILED: expected exit code 7, got $rc" >&2
    exit 1
fi
echo "hello_console: ok"
