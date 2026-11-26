#!/bin/bash
# S2 msvcrt-core test (PLAN-CORE 3.2, 5 S2).
#  1. every Tier 1 msvcrt export (and the aw_msvcrt_init / aw_msvcrt_flush_all hooks) is defined
#     in libanywin.o
#  2. tests/core_stdio_probe.c (default mingw-w64 toolchain, UCRT import names) relinks; every
#     import must be implemented
#  3. the probe runs with TZ=UTC and produces exactly the expected stdout (printf conversions,
#     width/precision, fopen through backslash paths, fread/fwrite, strftime/localtime, atexit
#     order C B A after main, stdio plumbing), exactly one stderr line, exit code 0, and the
#     file on disk has the bytes the probe wrote.
# Stand-ins: aw_seh_init / aw_seh_signal (package C) are called by start.c; when they are absent
# from libanywin.o, no-op stand-ins are linked and ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all
# is used (same as tests/core_env.sh, PLAN §4 rule 13).
# Usage: tests/core_stdio.sh [build-dir]    exit 0 PASS, 77 SKIP (no mingw), 1 FAIL, 2 setup error
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
case $build in /*) ;; *) build=$PWD/$build ;; esac
bin=$build/anywin
rt=$build/libanywin.o
probe_src=$root/tests/core_stdio_probe.c
exe=$build/core_stdio_probe.exe            # mingw build of the probe
work=$build/core_stdio                     # relinked probe
scratch=$build/core_stdio_scratch          # directory the probe writes into

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_stdio: run make first ($rt)" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_stdio: SKIP (no x86_64-w64-mingw32-gcc)"; exit 77; fi

fail() { echo "core_stdio: FAIL: $*" >&2; exit 1; }

# Tier 1 (PLAN-CORE 3.2) msvcrt names. Data exports are APIDATA (__aw_msvcrt_<name>), functions
# API (__aw_msvcrt_<name>); _errno is a function (msvcrt declares errno as *_errno()).
tier1="__getmainargs __iob_func __set_app_type __setusermatherr ___lc_codepage_func ___mb_cur_max_func
_amsg_exit _cexit _initterm _ismbblead _localtime64 _strlwr _time64 abort atexit calloc exit fclose
fopen fprintf fputc fread free fwrite getenv localeconv malloc memcpy signal strcat strcmp strcpy
strerror strftime strlen strncmp strstr vfprintf wcslen puts _errno
printf fputs fflush memset memcmp memmove strchr strrchr strncpy sprintf snprintf _snprintf _stricmp
toupper tolower _exit"
tier1_data="__initenv _acmdln _commode _fmode _iob"
hooks="aw_msvcrt_init aw_msvcrt_flush_all"

defined=$(nm -g --defined-only "$rt" | awk '{print $NF}')
defd() { printf '%s\n' "$defined" | grep -qx "$1"; }

# 1. exports and hooks
miss=""
for s in $tier1; do defd "__aw_msvcrt_$s" || miss="$miss $s"; done
for s in $tier1_data; do defd "__aw_msvcrt_$s" || miss="$miss $s"; done
for s in $hooks; do defd "$s" || miss="$miss $s"; done
[ -z "$miss" ] || fail "missing msvcrt exports or hooks:$miss"
echo "core_stdio: exports ok ($(echo $tier1 | wc -w) functions, $(echo $tier1_data | wc -w) data, $(echo $hooks | wc -w) hooks)"

# 2. probe build and relink
x86_64-w64-mingw32-gcc -O1 -Wall -o "$exe" "$probe_src" || fail "probe does not compile"
stubs=""
for s in aw_seh_init aw_seh_signal; do defd "$s" || stubs="$stubs $s"; done
rtl=$rt
if [ -n "$stubs" ]; then
    stub_src=$build/core_stdio_stubs.c
    {
        for s in $stubs; do
            case $s in
            aw_seh_init) echo 'void aw_seh_init(void) {}' ;;
            aw_seh_signal) echo 'int aw_seh_signal(int sig, void *si, void *uc) { (void)sig; (void)si; (void)uc; return 0; }' ;;
            esac
        done
    } >"$stub_src"
    cc -O1 -fno-pie -mcmodel=large -c -o "$build/core_stdio_stubs.o" "$stub_src" || fail "stub build"
    ld -r -o "$build/libanywin_core_stdio.o" "$rt" "$build/core_stdio_stubs.o" || fail "stub link"
    rtl=$build/libanywin_core_stdio.o
    echo "core_stdio: note: stand-ins used for:$stubs (not in tree yet)"
fi
ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all "$bin" "$exe" -o "$work" -r "$rtl" >"$work.relink.log" 2>&1 \
    || { cat "$work.relink.log" >&2; fail "relink failed (log $work.relink.log)"; }
unimpl=$(grep 'not implemented' "$work.relink.log" | sed 's/.*!\([^ ]*\) not implemented.*/\1/' | sort -u)
[ -z "$unimpl" ] || fail "unexpected unimplemented imports: $unimpl (log $work.relink.log)"
echo "core_stdio: relink ok, all imports implemented"

# 3. run
rm -rf "$scratch"
mkdir -p "$scratch" || fail "scratch dir"
winof() { printf '%s' "$1" | tr / '\\'; }   # unix path -> Windows path body (no drive)
out=$build/core_stdio.out
err=$build/core_stdio.err
env -i PATH="$PATH" TZ=UTC "$work" "Z:$(winof "$scratch")" >"$out" 2>"$err"
rc=$?
[ $rc -eq 0 ] || { cat "$out" "$err" >&2; fail "probe exit $rc"; }
grep -q 'unimplemented' "$out" "$err" && fail "probe hit an unimplemented import"

cat >"$build/core_stdio.expected" <<'EOF'
d=-42 u=42 x=ff X=FF o=10
ld=-7 lu=4000000000 lx=deadbeef
lld=-1234567890123 llu=18446744073709551615
I64d=1234567890123 I64x=ff
h=[4464] [44]
sizes=[5] [-3]
width [   42] [42   ] [00042] [+42] [ 42]
prec [007] [     007] []
star [     9] [1   |] [ab]
alt [0xff] [0XFF] [010]
pct [%] char [ok] [  z] [y  |]
str [hello] [        hi] [hi        |] [tru]
null=[(null)]
ptr [000000001234ABCD] [0000000000000000]
float [1.500000] [2.3] [   -3.14]
exp [1.000000e+00] [1.23E+04] [0.0001] [1E+20]
inf [inf] [-inf] [+inf]
sprintf=[7-x| 2.50]
snprintf n=4 buf=[7-xy]
truncated n=10 small=[abcdefg]
strchr=[/b/c] strrchr=[/c]
stricmp=0 lower=[ABC] upper=Qq
memcmp=-1 strerror=[No such file or directory]
decimal_point=[.]
wide=[wide] [w2] [!]
abc n=3
read=18 bytes first=[line 5] last=[Z]
missing=[null]
time=[2023-11-14 22:13:20] len=19
names=[Tuesday November 318] len=20
now_ok=1
fputs line
P
puts line
main done
atexit C
atexit B
atexit A
EOF
diff -u "$build/core_stdio.expected" "$out" >"$build/core_stdio.diff" || { cat "$build/core_stdio.diff" >&2; fail "stdout differs (diff $build/core_stdio.diff)"; }

[ "$(cat "$err")" = "to stderr 2" ] || fail "stderr differs: $(cat "$err")"

printf 'line 5\n0123456789Z' >"$build/core_stdio.want"
cmp -s "$build/core_stdio.want" "$scratch/data.bin" || fail "data.bin bytes differ (scratch $scratch)"

echo "core_stdio: ok (stdout $(wc -l <"$out") lines, stderr 1 line, exit 0, data.bin 18 bytes)"
