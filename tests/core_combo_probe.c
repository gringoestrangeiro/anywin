/* Combo box geometry and show-after-move paint probe (built with mingw, run under the anywin runtime).
 * Covers the root cause of the PuTTY Session panel radio row: a dropdown combo's window is only as
 * tall as its closed face (CB_GETITEMHEIGHT(-1)), the height passed to CreateWindow is the dropped
 * list height (CB_GETDROPPEDCONTROLRECT), and a control moved after creation and shown paints at its
 * new place (no pixels left behind at the old position). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void check(int ok, const char *what, int line)
{
    if (!ok) {
        printf("FAIL %s (line %d)\n", what, line);
        fails++;
    } else {
        printf("ok   %s\n", what);
    }
}
#define CHECK(c, what) check((c) != 0, what, __LINE__)

static void pump(void)
{
    MSG m;
    for (int i = 0; i < 200 && PeekMessageW(&m, NULL, 0, 0, PM_REMOVE); i++) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

/* number of pixels in client rect r of w that differ from the top-left background pixel */
static int ink_in(HWND w, RECT r)
{
    HDC dc = GetDC(w);
    COLORREF bg = GetPixel(dc, 0, 0), c;
    int n = 0;
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++) {
            c = GetPixel(dc, x, y);
            if (c != bg)
                n++;
        }
    ReleaseDC(w, dc);
    return n;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    HWND top, combo, radio;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    RECT wr, dr, cr;
    int closed, h;
    (void)prev; (void)cmd; (void)show;

    /* a window class with a background brush, as dialogs have: the parent erases what a child leaves */
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"anywin_combo_probe";
    RegisterClassW(&wc);
    top = CreateWindowExW(0, wc.lpszClassName, L"combo-probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 300, 300, NULL, NULL, hi, NULL);
    CHECK(top != NULL, "top-level window created");

    /* a dropdown combo asks for 200 px: the list is 200 px tall, the window only the closed face */
    combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                            10, 10, 150, 200, top, (HMENU)1, hi, NULL);
    CHECK(combo != NULL, "dropdown combo created");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Raw");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Telnet");
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    pump();

    closed = (int)SendMessageW(combo, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
    GetWindowRect(combo, &wr);
    h = wr.bottom - wr.top;
    CHECK(closed > 0 && h == closed, "dropdown combo window is as tall as its closed face");
    CHECK(h < 100, "dropdown combo window does not keep the requested dropped height");
    CHECK(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "selection kept in the combo");

    SendMessageW(combo, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&dr);
    CHECK(dr.bottom - dr.top > closed, "drop list rectangle keeps the requested height");

    /* a radio shown at (10,150), then moved with SetWindowPos: the new place paints, the old one is cleared */
    radio = CreateWindowExW(0, L"BUTTON", L"Raw", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 10, 150, 80, 16,
                            top, (HMENU)2, hi, NULL);
    CHECK(radio != NULL, "radio created");
    pump();
    UpdateWindow(top);
    pump();
    cr.left = 10; cr.top = 150; cr.right = 90; cr.bottom = 166;
    CHECK(ink_in(top, cr) > 0, "radio paints at its creation position");
    SetWindowPos(radio, NULL, 10, 200, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump();
    UpdateWindow(top);
    pump();
    cr.left = 10; cr.top = 200; cr.right = 90; cr.bottom = 216;
    CHECK(ink_in(top, cr) > 0, "moved radio paints at its new position");
    cr.left = 10; cr.top = 150; cr.right = 90; cr.bottom = 166;
    CHECK(ink_in(top, cr) == 0, "nothing is left at the old position");

    printf("core_combo: %d failures\n", fails);
    return fails ? 1 : 0;
}
