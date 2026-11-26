/* popup menu and MessageBox probe (S6): TrackPopupMenu with RETURNCMD and WM_COMMAND delivery,
 * mnemonics, submenus, Esc, and MessageBoxA with OK/YESNO/ABORTRETRYIGNORE.
 * Output lines are checked by core_menu.sh.  Built with mingw; relinked by anywin.
 * "core_menu_probe nodisplay" only calls MessageBoxA (no display needed). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HWND g_main;
static HMENU g_menu;

static HMENU make_menu(void)
{
    HMENU m = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    AppendMenuA(sub, MF_STRING, 201, "&Alpha");
    AppendMenuA(sub, MF_STRING | MF_GRAYED, 202, "&Beta");
    AppendMenuA(sub, MF_STRING, 203, "&Gamma\tCtrl+G");
    AppendMenuA(m, MF_STRING, 101, "&Show");
    AppendMenuA(m, MF_STRING, 102, "&Hide");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | MF_CHECKED, 103, "Start with &Windows");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sub, "&More");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, 104, "E&xit");
    return m;
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        ClientToScreen(h, &p);
        UINT r = TrackPopupMenu(g_menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, h, NULL);
        printf("ret %u\n", r);
        fflush(stdout);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        ClientToScreen(h, &p);
        TrackPopupMenu(g_menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, p.x, p.y, 0, h, NULL);
        return 0;
    }
    case WM_COMMAND:
        printf("command %d\n", (int)LOWORD(w));
        fflush(stdout);
        return 0;
    case WM_KEYDOWN:
        if (w == 'M') {
            int r = MessageBoxA(h, "Field is empty.", "Probe", MB_OK | MB_ICONWARNING);
            printf("msgbox %d\n", r);
        } else if (w == 'N') {
            int r = MessageBoxA(h, "Delete this item?", "Confirm", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            printf("msgbox %d\n", r);
        } else if (w == 'E') {
            int r = MessageBoxA(h, "Could not save the file.", "Error", MB_ABORTRETRYIGNORE | MB_ICONERROR);
            printf("msgbox %d\n", r);
        } else if (w == 'Q') {
            DestroyWindow(h);
        }
        fflush(stdout);
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
    (void)show;
    if (cmd && strstr(cmd, "nodisplay")) {
        int r = MessageBoxA(NULL, "Field is empty.", "Probe", MB_OK | MB_ICONWARNING);
        printf("nodisplay %d\n", r);
        fflush(stdout);
        return 0;
    }
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = "CoreMenuProbe";
    RegisterClassA(&wc);
    g_menu = make_menu();
    g_main = CreateWindowExA(0, "CoreMenuProbe", "Core Menu Probe", WS_OVERLAPPEDWINDOW, 40, 40, 400, 300, NULL,
                             NULL, inst, NULL);
    if (!g_main) {
        printf("window failed\n");
        return 1;
    }
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
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
