/* scroll bar probe (S6 part A): a top-level window with a vertical WS_VSCROLL bar and a child window
 * with its own WS_VSCROLL bar.  Prints WM_VSCROLL codes and positions, and the client width of the
 * child after ShowScrollBar (h hides, s shows).  Output lines are checked by core_scroll.sh.
 * Built with mingw; relinked by anywin. */
#include <windows.h>
#include <stdio.h>

static HWND g_main, g_child;

static void print_child_width(void)
{
    RECT r;
    GetClientRect(g_child, &r);
    printf("child w %ld\n", r.right - r.left);
    fflush(stdout);
}

static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_VSCROLL: {
        const char *name = "?";
        SCROLLINFO si;
        int pos;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_POS | SIF_TRACKPOS;
        GetScrollInfo(h, SB_VERT, &si);
        pos = si.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP: name = "LINEUP"; pos -= 1; break;
        case SB_LINEDOWN: name = "LINEDOWN"; pos += 1; break;
        case SB_PAGEUP: name = "PAGEUP"; pos -= 10; break;
        case SB_PAGEDOWN: name = "PAGEDOWN"; pos += 10; break;
        case SB_THUMBTRACK: name = "THUMBTRACK"; pos = si.nTrackPos; break;
        case SB_THUMBPOSITION: name = "THUMBPOSITION"; pos = si.nTrackPos; break;
        case SB_ENDSCROLL: name = "ENDSCROLL"; break;
        }
        if (pos < 0) pos = 0;
        if (pos > 89) pos = 89; /* range 0..99 with page 10 */
        if (LOWORD(w) != SB_ENDSCROLL) {
            SetScrollPos(h, SB_VERT, pos, TRUE);
            printf("vscroll %s %d\n", name, pos);
        } else {
            printf("vscroll %s\n", name);
        }
        fflush(stdout);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == 'H') {
            ShowScrollBar(g_child, SB_VERT, FALSE);
            print_child_width();
        } else if (w == 'S') {
            ShowScrollBar(g_child, SB_VERT, TRUE);
            print_child_width();
        } else if (w == 'Q') {
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    (void)show;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = "CoreScrollMain";
    RegisterClassA(&wc);
    wc.lpfnWndProc = ChildProc;
    wc.lpszClassName = "CoreScrollChild";
    RegisterClassA(&wc);

    g_main = CreateWindowExA(0, "CoreScrollMain", "Core Scroll Probe", WS_OVERLAPPEDWINDOW | WS_VSCROLL, 40, 40,
                             300, 200, NULL, NULL, inst, NULL);
    if (!g_main) {
        printf("window failed\n");
        return 1;
    }
    SCROLLINFO si;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = 99;
    si.nPage = 10;
    si.nPos = 0;
    SetScrollInfo(g_main, SB_VERT, &si, FALSE);

    g_child = CreateWindowExA(0, "CoreScrollChild", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 10, 10, 200, 100,
                              g_main, NULL, inst, NULL);
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    print_child_width();
    printf("ready\n");
    fflush(stdout);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    printf("exit\n");
    fflush(stdout);
    return 0;
}
