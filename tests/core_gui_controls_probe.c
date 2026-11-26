/* Controls probe (S7b): Static (red WM_CTLCOLORSTATIC brush), Edit, ComboBox DROPDOWNLIST, ComboBox
 * DROPDOWN (editable) and Button. Every notification the parent receives is printed, with the control's
 * text or selection, so tests/core_gui_controls.sh can assert on the log. Built with mingw, relinked by anywin.
 * Main window client origin is root (20,20) (see core_gui.sh for the same convention). */
#include <windows.h>
#include <stdio.h>

#define ID_BTN 101
#define ID_STATIC 100
#define ID_EDIT 201
#define ID_COMBO1 301
#define ID_COMBO2 302

static HWND g_main, g_static, g_edit, g_combo1, g_combo2;
static HBRUSH g_red;

/* read through the W API (the control's native path); the test strings are ASCII */
static void print_text(const char *tag, HWND h)
{
    WCHAR wbuf[256];
    char buf[256];
    int i, n = GetWindowTextW(h, wbuf, 256);
    for (i = 0; i < n && i < 255; i++)
        buf[i] = (char)wbuf[i];
    buf[i] = 0;
    printf("%s [%s]\n", tag, buf);
    fflush(stdout);
}

static void print_drop(const char *tag, HWND cb)
{
    RECT r;
    int ih = (int)SendMessageA(cb, CB_GETITEMHEIGHT, 0, 0);
    SendMessageA(cb, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&r);
    printf("%s droprect %ld %ld %ld %ld itemh=%d\n", tag, r.left, r.top, r.right, r.bottom, ih);
    fflush(stdout);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_static) {
            SetBkColor((HDC)w, RGB(255, 0, 0));
            return (LRESULT)g_red;
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ID_BTN && code == BN_CLICKED) {
            printf("button clicked\n");
            fflush(stdout);
        } else if (id == ID_EDIT) {
            if (code == EN_CHANGE)
                print_text("edit change", g_edit);
            else if (code == EN_SETFOCUS)
                printf("edit focus\n"), fflush(stdout);
            else if (code == EN_KILLFOCUS)
                printf("edit killfocus\n"), fflush(stdout);
        } else if (id == ID_COMBO1) {
            switch (code) {
            case CBN_DROPDOWN:
                print_drop("combo1 dropdown", g_combo1);
                break;
            case CBN_SELCHANGE:
                printf("combo1 selchange %d\n", (int)SendMessageA(g_combo1, CB_GETCURSEL, 0, 0)), fflush(stdout);
                break;
            case CBN_SELENDOK:
                printf("combo1 selendok\n"), fflush(stdout);
                break;
            case CBN_SELENDCANCEL:
                printf("combo1 selendcancel\n"), fflush(stdout);
                break;
            case CBN_CLOSEUP:
                printf("combo1 closeup\n"), fflush(stdout);
                printf("combo1 final=%d ", (int)SendMessageA(g_combo1, CB_GETCURSEL, 0, 0));
                print_text("text", g_combo1);
                break;
            default:
                break;
            }
        } else if (id == ID_COMBO2) {
            switch (code) {
            case CBN_EDITCHANGE:
                print_text("combo2 editchange", g_combo2);
                break;
            case CBN_SELCHANGE:
                printf("combo2 selchange %d\n", (int)SendMessageA(g_combo2, CB_GETCURSEL, 0, 0)), fflush(stdout);
                print_text("combo2 text", g_combo2);
                break;
            default:
                break;
            }
        }
        return 0;
    }
    case WM_TIMER:
        PostQuitMessage(0);
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
    HWND st, btn;
    (void)prev;
    (void)cmd;
    g_red = CreateSolidBrush(RGB(255, 0, 0));
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = "CoreControlsProbe";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)32512);
    if (!RegisterClassA(&wc))
        return 1;

    g_main = CreateWindowExA(0, "CoreControlsProbe", "Core Controls Probe", WS_OVERLAPPEDWINDOW, 20, 20, 520, 420,
                             NULL, NULL, inst, NULL);
    if (!g_main)
        return 2;

    st = CreateWindowExA(0, "Static", "Static label", WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 10, 200, 24,
                         g_main, (HMENU)(UINT_PTR)ID_STATIC, inst, NULL);
    g_static = st;
    g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "Edit", "hello", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             10, 50, 200, 24, g_main, (HMENU)(UINT_PTR)ID_EDIT, inst, NULL);
    g_combo1 = CreateWindowExA(0, "ComboBox", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                               10, 100, 200, 200, g_main, (HMENU)(UINT_PTR)ID_COMBO1, inst, NULL);
    g_combo2 = CreateWindowExA(0, "ComboBox", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWN,
                               230, 100, 200, 200, g_main, (HMENU)(UINT_PTR)ID_COMBO2, inst, NULL);
    btn = CreateWindowExA(0, "Button", "Click", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 340, 100, 30,
                          g_main, (HMENU)(UINT_PTR)ID_BTN, inst, NULL);
    if (!st || !g_edit || !g_combo1 || !g_combo2 || !btn)
        return 3;

    SendMessageA(g_combo1, CB_ADDSTRING, 0, (LPARAM)"alpha");
    SendMessageA(g_combo1, CB_ADDSTRING, 0, (LPARAM)"beta");
    SendMessageA(g_combo1, CB_ADDSTRING, 0, (LPARAM)"gamma");
    SendMessageA(g_combo1, CB_ADDSTRING, 0, (LPARAM)"delta");
    SendMessageA(g_combo2, CB_ADDSTRING, 0, (LPARAM)"alpha");
    SendMessageA(g_combo2, CB_ADDSTRING, 0, (LPARAM)"beta");
    SendMessageA(g_combo2, CB_ADDSTRING, 0, (LPARAM)"gamma");

    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetTimer(g_main, 1, 40000, NULL); /* safety exit; the script normally kills the probe */
    printf("ready\n");
    fflush(stdout);
    print_text("static", st);
    print_text("edit", g_edit);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    printf("exit\n");
    fflush(stdout);
    DeleteObject(g_red);
    return 0;
}
