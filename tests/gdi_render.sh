#!/bin/sh
# S4 GDI render test: native build of the GDI objects (surface/region/gdi/gdi_draw/drawtext + font.c),
# a scene rendered to PPM, API checks in C, and independent pixel assertions in python3 (-I).
# No X server, no user32.  Usage: gdi_render.sh [outdir].  Exit 0 = PASS, 77 = SKIP, otherwise FAIL.
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${1:-$root/build-s4/tests/gdi_render}   # outputs (binary, PPM, expectations) are kept here
mkdir -p "$work" || exit 1

skip() { echo "gdi_render: SKIP ($1)"; exit 77; }
command -v cc >/dev/null 2>&1 || skip "no cc"
command -v python3 >/dev/null 2>&1 || skip "no python3"

rt="$root/runtime"
if ! cc -std=gnu11 -O1 -g -Wall -Wno-unused-function -o "$work/gdi_render" \
        "$root/tests/gdi_render.c" \
        "$rt/surface.c" "$rt/region.c" "$rt/gdi.c" "$rt/gdi_draw.c" "$rt/drawtext.c" "$rt/font.c" "$rt/util.c" \
        -I"$rt" -lm 2>"$work/build.log"; then
    echo "gdi_render: FAIL (build)"; cat "$work/build.log"; exit 1
fi

if ! "$work/gdi_render" "$work/scene.ppm" "$work/expect.txt"; then
    echo "gdi_render: FAIL (C checks)"; exit 1
fi

python3 -I - "$work/scene.ppm" "$work/expect.txt" <<'PY'
import sys

ppm_path, exp_path = sys.argv[1], sys.argv[2]
with open(ppm_path, 'rb') as f:
    data = f.read()
# P6 header: magic, width height, maxval, then raw RGB
parts = data.split(b'\n', 3)
assert parts[0] == b'P6', 'not a P6 file'
w, h = (int(v) for v in parts[1].split())
assert parts[2] == b'255'
px = parts[3]
assert len(px) == w * h * 3, 'short PPM body'

def at(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]

fails = 0
pix = count = 0
with open(exp_path) as f:
    for line in f:
        t = line.split()
        if not t:
            continue
        if t[0] == 'PIX':
            x, y, hexv, tol = int(t[1]), int(t[2]), int(t[3], 16), int(t[4])
            want = ((hexv >> 16) & 255, (hexv >> 8) & 255, hexv & 255)
            got = at(x, y)
            pix += 1
            if max(abs(a - b) for a, b in zip(got, want)) > tol:
                fails += 1
                print('PIXEL FAIL (%d,%d) got #%02x%02x%02x want #%02x%02x%02x tol %d' %
                      (x, y, *got, *want, tol))
        elif t[0] == 'COUNT':
            x0, y0, x1, y1, mn = (int(v) for v in t[1:6])
            ink = sum(1 for y in range(y0, y1) for x in range(x0, x1) if at(x, y) != (255, 255, 255))
            count += 1
            if ink < mn:
                fails += 1
                print('COUNT FAIL box (%d,%d)-(%d,%d) ink %d < %d' % (x0, y0, x1, y1, ink, mn))
print('gdi_render pixels: %d probes, %d ink-count checks, %d failed' % (pix, count, fails))
sys.exit(1 if fails else 0)
PY
status=$?
if [ "$status" -eq 0 ]; then echo "gdi_render: PASS"; else echo "gdi_render: FAIL (pixels)"; fi
exit "$status"
