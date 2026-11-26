#!/bin/bash
# S1 kernel32-core test (PLAN-CORE §3.1, §5 S1).
#  1. every Tier 1 kernel32 export and the S1 internal hooks are defined in libanywin.o
#  2. tests/core_env_probe.c (kernel32-only, mingw, no CRT) relinks with 0 unimplemented imports
#  3. the probe runs under ANYWIN_STRICT=1 in three environments and exits 0:
#       A: argv with spaces and quotes, XDG_CONFIG_HOME set   -> command line, APPDATA, mkdir
#       B: no XDG_CONFIG_HOME, HOME set                        -> APPDATA = $HOME/.config
#       C: APPDATA preset to C:\preset                          -> kept, drive C maps to $HOME/.anywin/drive_c
# Stand-ins: aw_msvcrt_init / aw_seh_init / aw_seh_signal (packages B and C) are called by start.c
# before the entry point. When they are absent from libanywin.o, no-op stand-ins are linked and
# unresolved references are ignored (PLAN §4 rule 13); the probe reaches none of the unresolved code.
# Usage: tests/core_env.sh [build-dir]    exit 0 PASS, 77 SKIP (no mingw), 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
probe_src=$root/tests/core_env_probe.c
work=$build/core_env                       # relinked probe
exe=$build/core_env.exe                    # mingw build of the probe
home=$build/core_env_home                  # HOME for the runs (scratch)
cfg=$build/core_env_cfg                    # XDG_CONFIG_HOME for run A

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_env: run make first ($rt)" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_env: SKIP (no x86_64-w64-mingw32-gcc)"; exit 77; fi

fail() { echo "core_env: FAIL: $*" >&2; exit 1; }

# Tier 1 (§3.1) exports, as defined by kernel32.c, sync.c and the S1 hooks
tier1="CreateDirectoryA CreateMutexA DeleteCriticalSection EnterCriticalSection GetLastError
GetModuleFileNameA GetModuleHandleA GetProcAddress GetStartupInfoA InitializeCriticalSection
IsDBCSLeadByteEx LeaveCriticalSection MulDiv MultiByteToWideChar SetUnhandledExceptionFilter Sleep
TlsGetValue VirtualProtect VirtualQuery WideCharToMultiByte ExitProcess GetCommandLineA SetLastError
GetTickCount GetTickCount64"
hooks="aw_kernel32_init aw_find_resource aw_now_filetime aw_tick_ms aw_file_close aw_process_poll"

defined=$(nm -g --defined-only "$rt" | awk '{print $NF}')
defd() { printf '%s\n' "$defined" | grep -qx "$1"; }

# 1. exports and hooks
miss=""
for s in $tier1; do defd "__aw_kernel32_$s" || miss="$miss $s"; done
for s in $hooks; do defd "$s" || miss="$miss $s"; done
[ -z "$miss" ] || fail "missing kernel32 exports or S1 hooks:$miss"
echo "core_env: exports ok ($(echo $tier1 | wc -w) Tier 1 + $(echo $hooks | wc -w) hooks)"

# 2. probe build and relink
x86_64-w64-mingw32-gcc -O1 -Wall -nostdlib -e entry -o "$exe" "$probe_src" -lkernel32 \
    || fail "probe does not compile"
stubs=""
for s in aw_msvcrt_init aw_seh_init aw_seh_signal; do defd "$s" || stubs="$stubs $s"; done
rtl=$rt
if [ -n "$stubs" ]; then
    stub_src=$build/core_env_stubs.c
    {
        for s in $stubs; do
            case $s in
            aw_msvcrt_init) echo 'void aw_msvcrt_init(void) {}' ;;
            aw_seh_init) echo 'void aw_seh_init(void) {}' ;;
            aw_seh_signal) echo 'int aw_seh_signal(int sig, void *si, void *uc) { (void)sig; (void)si; (void)uc; return 0; }' ;;
            esac
        done
    } >"$stub_src"
    cc -O1 -fno-pie -mcmodel=large -c -o "$build/core_env_stubs.o" "$stub_src" || fail "stub build"
    ld -r -o "$build/libanywin_core_env.o" "$rt" "$build/core_env_stubs.o" || fail "stub link"
    rtl=$build/libanywin_core_env.o
    echo "core_env: note: stand-ins used for:$stubs (not in tree yet)"
fi
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$work" -r "$rtl" >"$work.relink.log" 2>&1 \
    || { cat "$work.relink.log" >&2; fail "relink failed (log $work.relink.log)"; }
grep -q ', 0 unimplemented)' "$work.relink.log" || fail "relink left unimplemented imports: $(cat "$work.relink.log")"
echo "core_env: relink ok, 0 unimplemented imports"

# 3. runs
rm -rf "$home" "$cfg"
mkdir -p "$home/.config" "$home/.anywin/drive_c/preset" "$cfg" || fail "scratch dirs"
winof() { printf '%s' "$1" | tr / '\\'; }   # unix path -> Windows path body (no drive)

run() { # name log env... -- args...
    local name=$1 log=$2; shift 2
    env -i PATH="$PATH" "$@" >"$log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then fail "run $name exit $rc (log $log)"; fi
    if grep -q 'unimplemented' "$log"; then fail "run $name hit an unimplemented import (log $log)"; fi
    grep -q '^failures=0$' "$log" || fail "run $name reported failures (log $log)"
}
expect_line() { grep -qxF "$2" "$1" || fail "expected line not found: $2 (log $1)"; }

logA=$build/core_env.A.log
run A "$logA" HOME="$home" XDG_CONFIG_HOME="$cfg" ANYWIN_STRICT=1 "$work" "a b" 'c"d'
expect_line "$logA" "cmdline=Z:$(winof "$work") \"a b\" \"c\\\"d\""
expect_line "$logA" "APPDATA=Z:$(winof "$cfg")"
expect_line "$logA" 'TEMP=Z:\tmp'
expect_line "$logA" 'OS=Windows_NT'
expect_line "$logA" 'SystemRoot=C:\windows'
[ -d "$cfg/anywin-core-env" ] || fail "run A did not create the directory under APPDATA"

logB=$build/core_env.B.log
run B "$logB" HOME="$home" ANYWIN_STRICT=1 "$work"
expect_line "$logB" "APPDATA=Z:$(winof "$home/.config")"
[ -d "$home/.config/anywin-core-env" ] || fail "run B did not create the directory under ~/.config"

logC=$build/core_env.C.log
run C "$logC" HOME="$home" APPDATA='C:\preset' ANYWIN_STRICT=1 "$work"
expect_line "$logC" 'APPDATA=C:\preset'
[ -d "$home/.anywin/drive_c/preset/anywin-core-env" ] || fail "run C did not map C:\\preset under HOME"

echo "core_env: ok (3 runs: argv+XDG, HOME default, preset APPDATA with drive mapping)"
