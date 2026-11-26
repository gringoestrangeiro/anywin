/* user32 core probe (S5): top-level window, a custom child window with WS_BORDER, a push button,
 * a timer that repaints, mouse click and keyboard input.  Output lines are checked by core_gui.sh.
 * Built with mingw; relinked by anywin. */
#include <windows.h>
#include <stdio.h>

static HWND g_main, g_btn;
static int g_clicks, g_keys, g_ticks;

static LRESULT CALLBACK BoxProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void paint_square(HDC dc, int x, int y, COLORREF c)
{
    RECT r = {x, y, x + 60, y + 60};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        SetTimer(h, 1, 300, NULL);
        return 0;
    case WM_TIMER: {
        if (w != 1)
            return 0;
        g_ticks++;
        if (g_ticks == 1) {
            RECT r = {200, 20, 260, 80};
            InvalidateRect(h, &r, FALSE);
        }
        if (g_ticks >= 14)
            PostQuitMessage(0);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 101 && HIWORD(w) == BN_CLICKED) {
            g_clicks++;
            printf("clicked %d\n", g_clicks);
            fflush(stdout);
            RECT r = {200, 120, 260, 180};
            InvalidateRect(h, &r, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        g_keys++;
        printf("keydown %d\n", (int)w);
        fflush(stdout);
        return 0;
    case WM_CHAR:
        printf("char %d\n", (int)w);
        fflush(stdout);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (g_ticks >= 1)
            paint_square(dc, 200, 20, RGB(0, 255, 0));
        if (g_clicks >= 1)
            paint_square(dc, 200, 120, RGB(255, 0, 0));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = "CoreProbeMain";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)32512);
    if (!RegisterClassA(&wc)) return 1;
    wc.lpfnWndProc = BoxProc;
    wc.lpszClassName = "CoreProbeBox";
    if (!RegisterClassA(&wc)) return 2;

    g_main = CreateWindowExA(0, "CoreProbeMain", "Core GUI Probe", WS_OVERLAPPEDWINDOW, 20, 20, 400, 300,
                             NULL, NULL, inst, NULL);
    if (!g_main) return 3;
    CreateWindowExA(0, "CoreProbeBox", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 10, 150, 80, g_main,
                    NULL, inst, NULL);
    g_btn = CreateWindowExA(0, "Button", "Click", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 120, 120, 30,
                            g_main, (HMENU)(UINT_PTR)101, inst, NULL);
    if (!g_btn) {
        printf("button failed err=%lu\n", (unsigned long)GetLastError());
        fflush(stdout);
        return 4;
    }
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    printf("ready\n");
    fflush(stdout);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    printf("exit clicks=%d keys=%d ticks=%d\n", g_clicks, g_keys, g_ticks);
    fflush(stdout);
    return 0;
}
