/* WM_SIZE probe: WM_SIZE always carries the CLIENT area size, and GetClientRect already reports
 * the new client size when WM_SIZE arrives.  Covers the first show of a window (ShowWindow after
 * SetWindowPos, as PuTTY does), a resize, and a scroll bar that is removed with a frame change.
 * PuTTY derives its terminal column count from the WM_SIZE lParam, so an outer-size lParam makes
 * it draw the whole text row one cell off to the left.
 * Prints "ok" / "FAIL" lines; exit 0 when all checks pass.  Driven by tests/core_wsize.sh. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static int n_size, bad_size;

#define CHECK(cond, name)                                                                             \
    do {                                                                                              \
        checks++;                                                                                     \
        if (cond)                                                                                     \
            printf("ok   %s\n", name);                                                                \
        else {                                                                                        \
            failures++;                                                                               \
            printf("FAIL %s (line %d)\n", name, __LINE__);                                            \
        }                                                                                             \
    } while (0)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SIZE) {
        RECT rc;
        int cw = LOWORD(l), ch = HIWORD(l);
        GetClientRect(h, &rc);
        n_size++;
        printf("     WM_SIZE lParam=%dx%d GetClientRect=%ldx%ld\n", cw, ch, rc.right - rc.left,
               rc.bottom - rc.top);
        if (cw != rc.right - rc.left || ch != rc.bottom - rc.top)
            bad_size++;
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(int ms)
{
    MSG msg;
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}

static int client_w(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);
    return rc.right - rc.left;
}

int main(void)
{
    WNDCLASSA wc;
    HWND h;
    int before, after, bar = GetSystemMetrics(SM_CXVSCROLL);
    int size_before;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "CoreWsizeProbe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    /* created the way PuTTY does it: a popup with caption and a vertical scroll bar, default size */
    h = CreateWindowExA(0, "CoreWsizeProbe", "Core WSize Probe", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VSCROLL,
                        CW_USEDEFAULT, CW_USEDEFAULT, 808, 508, NULL, NULL, wc.hInstance, NULL);
    CHECK(h != NULL, "CreateWindowEx");
    pump(50);

    /* the window is moved and sized before it is shown */
    SetWindowPos(h, NULL, 76, 76, 659, 410, SWP_NOZORDER | SWP_NOACTIVATE);
    pump(50);
    before = client_w(h);
    CHECK(before > 0 && before < 659, "client width is smaller than the outer width (scroll bar, frame)");

    /* first show: the size that arrives with it must be the client size */
    size_before = n_size;
    ShowWindow(h, SW_SHOWDEFAULT);
    pump(200);
    CHECK(n_size > size_before, "ShowWindow sends WM_SIZE on the first show");

    /* resize with the window visible */
    SetWindowPos(h, NULL, 0, 0, 700, 410, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump(100);

    /* scroll bar removed by a style change and a frame change: the client grows by one bar */
    before = client_w(h);
    SetWindowLongA(h, GWL_STYLE, GetWindowLongA(h, GWL_STYLE) & ~WS_VSCROLL);
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    pump(100);
    after = client_w(h);
    CHECK(after - before == bar, "removing the scroll bar grows the client area by the bar width");

    CHECK(n_size >= 4, "WM_SIZE sent for each size change");
    CHECK(bad_size == 0, "every WM_SIZE lParam equals the client size at that moment");

    DestroyWindow(h);
    pump(20);
    printf("core_wsize: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
