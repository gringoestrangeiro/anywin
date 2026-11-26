/* Radio-group semantics probe (built with mingw, run under the anywin runtime).
 * Covers BS_AUTORADIOBUTTON group clearing (click, BM_CLICK, arrow keys with wrap-around),
 * BS_RADIOBUTTON staying manual, CheckRadioButton, Tab stops on the checked radio only,
 * BN_CLICKED notifications, WM_GETDLGCODE and MapDialogRect with the Tahoma-8 dialog font. */
#include <windows.h>
#include <stdio.h>

#define IDD_RADIO 100
#define IDC_A1 201
#define IDC_A2 202
#define IDC_A3 203
#define IDC_B1 211
#define IDC_B2 212
#define IDC_C1 221
#define IDC_C2 222

static int fails, clicks[300];

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

static INT_PTR CALLBACK dlgproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)h;
    (void)lp;
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED && LOWORD(wp) < 300)
        clicks[LOWORD(wp)]++;
    return FALSE;
}

static int chk(HWND d, int id) { return (int)IsDlgButtonChecked(d, id); }

static void click(HWND d, int id)
{
    HWND b = GetDlgItem(d, id);
    SendMessageW(b, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
    SendMessageW(b, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
}

static int key(HWND d, int id, int vk)
{
    MSG m;
    memset(&m, 0, sizeof m);
    m.hwnd = GetDlgItem(d, id);
    m.message = WM_KEYDOWN;
    m.wParam = vk;
    {
        return IsDialogMessageW(d, &m);
    }
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    HWND d;
    POINT dummy;
    RECT rc;
    (void)prev; (void)cmd; (void)show; (void)dummy;

    d = CreateDialogW(hi, MAKEINTRESOURCEW(IDD_RADIO), NULL, dlgproc);
    CHECK(d != NULL, "dialog created");
    if (!d) {
        printf("core_radio: %d failures\n", fails + 1);
        return 1;
    }
    ShowWindow(d, SW_SHOW);
    UpdateWindow(d);

    /* dialog units: 100 x 100 DLU are 150 x 163 px with the 6x13 base units of MS Shell Dlg 8 (MulDiv) */
    SetRect(&rc, 0, 0, 100, 100);
    MapDialogRect(d, &rc);
    CHECK(rc.right == 150 && rc.bottom == 163, "MapDialogRect 100x100 DLU -> 150x163 px (MulDiv rounding)");

    CheckRadioButton(d, IDC_A1, IDC_A3, IDC_A2);
    CHECK(chk(d, IDC_A2) && !chk(d, IDC_A1) && !chk(d, IDC_A3), "CheckRadioButton selects A2 only");

    click(d, IDC_A3);
    CHECK(chk(d, IDC_A3) && !chk(d, IDC_A2) && !chk(d, IDC_A1), "mouse click on A3 clears A2 (earlier radio of group)");
    CHECK(clicks[IDC_A3] == 1, "BN_CLICKED sent once for A3");

    SendMessageW(GetDlgItem(d, IDC_A1), BM_CLICK, 0, 0);
    CHECK(chk(d, IDC_A1) && !chk(d, IDC_A2) && !chk(d, IDC_A3), "BM_CLICK on A1 leaves exactly one checked");

    SendMessageW(GetDlgItem(d, IDC_B1), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(d, IDC_B2), BM_CLICK, 0, 0);
    CHECK(chk(d, IDC_B1) && !chk(d, IDC_B2), "BS_RADIOBUTTON (manual) click does not change the group");
    CHECK(chk(d, IDC_A1), "groups are independent (A unchanged by B clicks)");

    SendMessageW(GetDlgItem(d, IDC_C1), BM_SETCHECK, BST_CHECKED, 0);
    SetFocus(GetDlgItem(d, IDC_C1));
    key(d, IDC_C1, VK_DOWN);
    CHECK(GetFocus() == GetDlgItem(d, IDC_C2) && chk(d, IDC_C2) && !chk(d, IDC_C1), "Down arrow moves and checks C2");
    key(d, IDC_C2, VK_DOWN);
    CHECK(GetFocus() == GetDlgItem(d, IDC_C1) && chk(d, IDC_C1) && !chk(d, IDC_C2), "Down arrow wraps inside group C");
    key(d, IDC_C1, VK_UP);
    CHECK(chk(d, IDC_C2) && !chk(d, IDC_C1), "Up arrow wraps inside group C");

    SetFocus(GetDlgItem(d, IDC_A1));
    key(d, IDC_A1, VK_RIGHT);
    CHECK(GetFocus() == GetDlgItem(d, IDC_A2) && chk(d, IDC_A2) && !chk(d, IDC_A1), "Right arrow stays inside group A");

    CheckRadioButton(d, IDC_B1, IDC_B2, IDC_B2);
    CHECK(chk(d, IDC_B2) && !chk(d, IDC_B1), "CheckRadioButton on manual radios");

    {
        LRESULT dc = SendMessageW(GetDlgItem(d, IDC_A1), WM_GETDLGCODE, 0, 0);
        CHECK((dc & DLGC_RADIOBUTTON) != 0, "WM_GETDLGCODE reports DLGC_RADIOBUTTON for auto radio");
    }

    /* tab order: one tab stop per group, the checked radio (C2 is checked now) */
    CHECK(GetNextDlgTabItem(d, GetDlgItem(d, IDC_A1), FALSE) == GetDlgItem(d, IDC_A2),
          "Tab from A1 goes to the checked radio of group A (A2)");
    CHECK(GetNextDlgTabItem(d, GetDlgItem(d, IDC_A2), FALSE) == GetDlgItem(d, IDC_B2),
          "Tab from group A goes to the checked radio of group B (B2)");
    CHECK(GetNextDlgTabItem(d, GetDlgItem(d, IDC_B2), FALSE) == GetDlgItem(d, IDC_C2),
          "Tab stop of group C is the checked radio C2");
    CHECK(GetNextDlgTabItem(d, GetDlgItem(d, IDC_C2), FALSE) == GetDlgItem(d, IDOK),
          "Tab after C2 goes to OK");

    DestroyWindow(d);
    printf("core_radio: %d failures\n", fails);
    return fails ? 1 : 0;
}
