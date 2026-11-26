#!/bin/bash
# Clipboard and input message probe: relinks tests/core_clip_probe.c (MinGW) and runs it under Xvfb.
#   local   SetClipboardData / GetClipboardData (GlobalSize, NUL termination), format enumeration,
#           RegisterClipboardFormatA, EmptyClipboard and the owner
#   remote  CF_UNICODETEXT / CF_TEXT of the X CLIPBOARD owned by xclip, read twice: the owner's text is
#           changed in between, so the second read must follow it (freshness)
#   mouse   WM_*BUTTON* wParam MK_* flags, WM_MOUSEWHEEL delta and screen coordinates, WM_KEYDOWN with Shift
#           (xdotool drives the pointer and keyboard)
# Usage: tests/core_clip.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$root/tests/core_clip_probe.c
exe=$build/core_clip_probe.exe
out=$build/core_clip
log=$build/core_clip.log

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "core_clip: run make first" >&2; exit 2; fi
if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then echo "core_clip: SKIP (no mingw)" >&2; exit 77; fi
for t in xvfb-run xdotool xclip; do
    if ! command -v "$t" >/dev/null 2>&1; then echo "core_clip: SKIP (no $t)" >&2; exit 77; fi
done

x86_64-w64-mingw32-gcc -O1 -o "$exe" "$src" || { echo "core_clip: mingw compile failed"; exit 1; }
"$bin" "$exe" -o "$out" -r "$rt" >"$out.relink.log" 2>&1 || { echo "core_clip: relink failed (log: $out.relink.log)"; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/core_clip.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
: >"$log"
fail=0
check() { # check NAME PATTERN FILE
    if grep -q -- "$2" "$3"; then
        echo "ok   $1" | tee -a "$log"
    else
        echo "FAIL $1 (no '$2' in $3)" | tee -a "$log"
        fail=1
    fi
}

# ---- local: the owner's own data -------------------------------------------------------------
timeout 60 xvfb-run -a "$out" local >"$work/local.txt" 2>&1
cat "$work/local.txt" >>"$log"
if ! grep -q '^clip: ok$' "$work/local.txt"; then echo "core_clip: local checks FAILED" | tee -a "$log"; fail=1; fi

# ---- remote: data owned by another client, freshness across an owner change -----------------
cat >"$work/remote.sh" <<EOF
#!/bin/bash
printf 'first text\r\n' | xclip -selection clipboard -i
sleep 0.5
timeout 30 "$out" remote \$'first text\r\n' 'second one' > "$work/remote.txt" 2>&1 &
sleep 1
printf 'second one' | xclip -selection clipboard -i
wait
pkill -x xclip 2>/dev/null
exit 0
EOF
timeout 90 xvfb-run -a bash "$work/remote.sh" >/dev/null 2>&1
cat "$work/remote.txt" >>"$log"
check "remote: CF_UNICODETEXT of the other owner, NUL-terminated (CRLF kept)" 'first: unicode \[first text' "$work/remote.txt"
check "remote: CF_TEXT of the other owner" 'first: ansi \[first text' "$work/remote.txt"
check "remote: second read follows the owner's new text" 'second: unicode \[second one\] len=10 size=22' "$work/remote.txt"
grep -q '^clip: ok$' "$work/remote.txt" || { echo "FAIL remote probe (see $log)" | tee -a "$log"; fail=1; }

# ---- mouse and keyboard messages ---------------------------------------------------------------
cat >"$work/mouse.sh" <<EOF
#!/bin/bash
"$out" mouse 14 > "$work/mouse.txt" 2>&1 &
PP=\$!
for i in \$(seq 1 100); do grep -q "window ready" "$work/mouse.txt" 2>/dev/null && break; sleep 0.1; done
W=\$(xdotool search --name "anywin clip probe mouse" | head -1)
xdotool windowfocus --sync "\$W" 2>/dev/null; sleep 0.5
OX=\$(sed -n 's/.*origin=\([-0-9]*\),\([-0-9]*\).*/\1/p' "$work/mouse.txt")
OY=\$(sed -n 's/.*origin=\([-0-9]*\),\([-0-9]*\).*/\2/p' "$work/mouse.txt")
xdotool mousemove \$((OX + 50)) \$((OY + 60)); sleep 0.3
xdotool click 2; sleep 0.3                                  # middle down / up
xdotool click 3; sleep 0.3                                  # right down / up
xdotool keydown shift; sleep 0.2; xdotool click 1; sleep 0.2; xdotool keyup shift; sleep 0.3
xdotool click 4; sleep 0.3                                  # wheel up
xdotool key shift+Insert; sleep 0.5                         # Shift+Insert reaches the window with Shift down
wait \$PP
EOF
timeout 90 xvfb-run -a bash "$work/mouse.sh" >/dev/null 2>&1
cat "$work/mouse.txt" >>"$log"
check "mouse: middle button down carries MK_MBUTTON" 'MSG WM_MBUTTONDOWN wParam=0x10 keys=0x10 client=50,60' "$work/mouse.txt"
check "mouse: middle button up clears MK_MBUTTON" 'MSG WM_MBUTTONUP wParam=0x0 keys=0x0 client=50,60' "$work/mouse.txt"
check "mouse: right button down carries MK_RBUTTON" 'MSG WM_RBUTTONDOWN wParam=0x2 keys=0x2 client=50,60' "$work/mouse.txt"
check "mouse: left down with Shift = MK_LBUTTON|MK_SHIFT" 'MSG WM_LBUTTONDOWN wParam=0x5 keys=0x5' "$work/mouse.txt"
check "mouse: left up with Shift keeps MK_SHIFT only" 'MSG WM_LBUTTONUP wParam=0x4 keys=0x4' "$work/mouse.txt"
check "mouse: wheel delta 120 in the high word, screen coordinates" 'MSG WM_MOUSEWHEEL wParam=0x780000 delta=120 keys=0x0 lParam=150,160' "$work/mouse.txt"
check "keys: Shift+Insert delivers VK_INSERT with Shift down" 'MSG WM_KEYDOWN vk=0x2d .* shift=1' "$work/mouse.txt"
grep -q '^mouse: done$' "$work/mouse.txt" || { echo "FAIL mouse probe did not finish" | tee -a "$log"; fail=1; }

if [ $fail -ne 0 ]; then
    echo "core_clip: FAILED (log: $log)" >&2
    exit 1
fi
echo "core_clip: ok"
