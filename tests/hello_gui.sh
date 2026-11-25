#!/bin/bash
# Compile a minimal Win32 GUI program, relink it, run under Xvfb, screenshot it.
# Milestone for platform/fonts/GDI/user32 (window, paint, text, timer, close).
# Usage: tests/hello_gui.sh [build-dir]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
bin=$build/anywin
rt=$build/libanywin.o
src=$build/hello_gui.c
exe=$build/hello_gui.exe
out=$build/hello_gui
log=$build/hello_gui.log
shot=$build/hello_gui.png

if [ ! -x "$bin" ] || [ ! -f "$rt" ]; then echo "hello_gui: run make first" >&2; exit 2; fi
cat >"$src" <<'EOF'
#include <windows.h>
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 30, 30));
        TextOutA(dc, 20, 20, "anywin gui hello", 16);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = "AnywinHello";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)32512);
    if (!RegisterClassA(&wc)) return 1;
    HWND h = CreateWindowExA(0, "AnywinHello", "Anywin GUI Hello", WS_OVERLAPPEDWINDOW,
                             50, 50, 320, 200, NULL, NULL, inst, NULL);
    if (!h) return 2;
    ShowWindow(h, show);
    UpdateWindow(h);
    SetTimer(h, 1, 1200, NULL);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
EOF
x86_64-w64-mingw32-gcc -O1 -mwindows -o "$exe" "$src" || exit 1
"$bin" "$exe" -o "$out" -r "$rt" >/dev/null 2>&1 || { echo "hello_gui: relink failed"; exit 1; }
rm -f "$log" "$shot"
xvfb-run -a -s "-screen 0 800x600x24" bash -c '
    out=$1 log=$2 shot=$3
    "$out" >"$log" 2>&1 &
    pid=$!
    found=0
    for _ in $(seq 1 80); do
        if xdotool search --name "Anywin GUI Hello" >/dev/null 2>&1; then found=1; break; fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 0.4
    import -window root "$shot" 2>/dev/null || true
    for _ in $(seq 1 40); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    [ "$found" = 1 ] || exit 1
    exit 0
' _ "$out" "$log" "$shot"
rc=$?
if [ $rc -ne 0 ]; then
    echo "hello_gui: no window or program hung (log: $log)" >&2
    [ -f "$log" ] && tail -20 "$log" >&2
    exit 1
fi
echo "hello_gui: ok (screenshot: $shot)"
