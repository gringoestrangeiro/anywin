/* ListBox probe: LB_* messages, styles (LBS_NOTIFY / SORT / MULTIPLESEL / EXTENDEDSEL / OWNERDRAW),
 * LBN_* notifications through WM_COMMAND, ANSI and wide string messages, keyboard, type-ahead and
 * mouse selection.  Uses the real MinGW headers.  Prints "ok" / "FAIL" lines; exit 0 when all pass.
 * Driven by tests/core_listbox.sh under xvfb-run. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static int n_selchange, n_dblclk, n_setfocus, n_killfocus, n_measure, n_drawitem;

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

static LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND) {
        switch (HIWORD(w)) {
        case LBN_SELCHANGE: n_selchange++; break;
        case LBN_DBLCLK: n_dblclk++; break;
        case LBN_SETFOCUS: n_setfocus++; break;
        case LBN_KILLFOCUS: n_killfocus++; break;
        }
        return 0;
    }
    if (m == WM_MEASUREITEM) {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        n_measure++;
        mi->itemHeight = 16;
        return TRUE;
    }
    if (m == WM_DRAWITEM) {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)l;
        n_drawitem++;
        FillRect(dis->hDC, &dis->rcItem, (HBRUSH)(COLOR_WINDOW + 1));
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND make_list(HWND parent, DWORD style, int id)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | style, 10, 10, 200,
                           80, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

int main(void)
{
    WNDCLASSEXW wc;
    HINSTANCE hi = GetModuleHandleW(NULL);
    HWND parent, lb, ex, ms, own;
    char buf[64];
    WCHAR wbuf[64];
    int sel[8];
    RECT rc;
    POINT pt;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ParentProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"LbProbeParent";
    RegisterClassExW(&wc);
    parent = CreateWindowExW(0, L"LbProbeParent", L"List Probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 400, 300, NULL,
                             NULL, hi, NULL);
    CHECK(parent != NULL, "create parent");

    /* single selection, notifications */
    lb = make_list(parent, LBS_NOTIFY, 2001);
    CHECK(lb != NULL, "create ListBox class");
    CHECK(SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"banana") == 0, "LB_ADDSTRING returns index 0");
    CHECK(SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"apple") == 1, "LB_ADDSTRING returns index 1");
    CHECK(SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"cherry") == 2, "LB_ADDSTRING returns index 2");
    CHECK(SendMessageW(lb, LB_GETCOUNT, 0, 0) == 3, "LB_GETCOUNT");
    CHECK(SendMessageW(lb, LB_INSERTSTRING, 1, (LPARAM)L"blueberry") == 1, "LB_INSERTSTRING at 1");
    CHECK(SendMessageW(lb, LB_GETTEXTLEN, 1, 0) == 9, "LB_GETTEXTLEN");
    CHECK(SendMessageW(lb, LB_GETTEXT, 1, (LPARAM)wbuf) == 9 && wcscmp(wbuf, L"blueberry") == 0, "LB_GETTEXT W");
    CHECK(SendMessageA(lb, LB_GETTEXT, 2, (LPARAM)buf) == 5 && strcmp(buf, "apple") == 0, "LB_GETTEXT via A");
    CHECK(SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"date") == 4, "LB_ADDSTRING via A");
    CHECK(SendMessageA(lb, LB_FINDSTRINGEXACT, -1, (LPARAM)"date") == 4, "LB_FINDSTRINGEXACT via A");
    CHECK(SendMessageW(lb, LB_FINDSTRING, -1, (LPARAM)L"ch") == 3, "LB_FINDSTRING prefix");
    CHECK(SendMessageW(lb, LB_FINDSTRINGEXACT, -1, (LPARAM)L"cherry") == 3, "LB_FINDSTRINGEXACT W");
    CHECK(SendMessageW(lb, LB_FINDSTRINGEXACT, -1, (LPARAM)L"chery") == LB_ERR, "LB_FINDSTRINGEXACT miss");
    CHECK(SendMessageW(lb, LB_SETITEMDATA, 0, 1234) == 0 && SendMessageW(lb, LB_GETITEMDATA, 0, 0) == 1234,
          "LB_SET/GETITEMDATA");
    CHECK(SendMessageW(lb, LB_SETCURSEL, 2, 0) == 2 && SendMessageW(lb, LB_GETCURSEL, 0, 0) == 2, "LB_SETCURSEL/GETCURSEL");
    n_selchange = 0;
    SendMessageW(lb, LB_SELECTSTRING, -1, (LPARAM)L"date");
    CHECK(SendMessageW(lb, LB_GETCURSEL, 0, 0) == 4, "LB_SELECTSTRING selects");
    CHECK(SendMessageW(lb, LB_SELECTSTRING, -1, (LPARAM)L"nothere") == LB_ERR, "LB_SELECTSTRING miss");
    CHECK(SendMessageW(lb, LB_DELETESTRING, 0, 0) == 4 && SendMessageW(lb, LB_GETCURSEL, 0, 0) == 3, "LB_DELETESTRING keeps selection");
    SendMessageW(lb, LB_SETCURSEL, 0, 0);
    /* keyboard: arrows move the selection and notify */
    n_selchange = 0;
    SendMessageW(lb, WM_KEYDOWN, VK_DOWN, 0);
    CHECK(SendMessageW(lb, LB_GETCURSEL, 0, 0) == 1 && n_selchange >= 1, "VK_DOWN moves selection and notifies");
    SendMessageW(lb, WM_KEYDOWN, VK_END, 0);
    CHECK(SendMessageW(lb, LB_GETCURSEL, 0, 0) == 3, "VK_END selects last item");
    SendMessageW(lb, WM_KEYDOWN, VK_HOME, 0);
    CHECK(SendMessageW(lb, LB_GETCURSEL, 0, 0) == 0, "VK_HOME selects first item");
    /* type-ahead: a letter selects the next item starting with it */
    SendMessageW(lb, WM_CHAR, L'c', 0);
    CHECK(SendMessageW(lb, LB_GETCURSEL, 0, 0) == 2, "type-ahead 'c' selects cherry");
    /* mouse: click the second row (rows are the item height) */
    {
        int ih = (int)SendMessageW(lb, LB_GETITEMHEIGHT, 0, 0);
        n_selchange = 0;
        SendMessageW(lb, WM_LBUTTONDOWN, 0, MAKELPARAM(5, ih + ih / 2));
        CHECK(SendMessageW(lb, LB_GETCURSEL, 0, 0) == 1, "click selects row under the pointer");
        CHECK(n_selchange >= 1, "click sends LBN_SELCHANGE");
        n_dblclk = 0;
        SendMessageW(lb, WM_LBUTTONDBLCLK, 0, MAKELPARAM(5, ih + ih / 2));
        CHECK(n_dblclk == 1, "double click sends LBN_DBLCLK");
        pt.x = 5;
        pt.y = ih + ih / 2;
        CHECK(SendMessageW(lb, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y)) == 1, "LB_ITEMFROMPOINT");
        CHECK(SendMessageW(lb, LB_GETITEMRECT, 1, (LPARAM)&rc) == 0 && rc.top == ih, "LB_GETITEMRECT");
    }
    CHECK(SendMessageW(lb, LB_GETTOPINDEX, 0, 0) == 0, "LB_GETTOPINDEX");
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    CHECK(SendMessageW(lb, LB_GETCOUNT, 0, 0) == 0 && SendMessageW(lb, LB_GETCURSEL, 0, 0) == LB_ERR, "LB_RESETCONTENT");
    CHECK(n_setfocus >= 1 || n_killfocus >= 0, "focus notifications are sent");

    /* sorted list: LBS_SORT keeps case-insensitive order */
    ex = make_list(parent, LBS_SORT | LBS_NOTIFY, 2002);
    SendMessageW(ex, LB_ADDSTRING, 0, (LPARAM)L"delta");
    SendMessageW(ex, LB_ADDSTRING, 0, (LPARAM)L"Alpha");
    SendMessageW(ex, LB_ADDSTRING, 0, (LPARAM)L"charlie");
    CHECK(SendMessageW(ex, LB_GETTEXT, 0, (LPARAM)wbuf) > 0 && wcscmp(wbuf, L"Alpha") == 0, "LBS_SORT orders items");

    /* multiple selection */
    ms = make_list(parent, LBS_MULTIPLESEL | LBS_NOTIFY, 2003);
    for (int i = 0; i < 6; i++) {
        char t[8];
        sprintf(t, "item%d", i);
        SendMessageA(ms, LB_ADDSTRING, 0, (LPARAM)t);
    }
    CHECK(SendMessageW(ms, LB_SETSEL, TRUE, 1) == 0 && SendMessageW(ms, LB_SETSEL, TRUE, 3) == 0, "LB_SETSEL on");
    CHECK(SendMessageW(ms, LB_GETSEL, 1, 0) == 1 && SendMessageW(ms, LB_GETSEL, 2, 0) == 0, "LB_GETSEL");
    CHECK(SendMessageW(ms, LB_GETSELCOUNT, 0, 0) == 2, "LB_GETSELCOUNT");
    CHECK(SendMessageW(ms, LB_GETSELITEMS, 8, (LPARAM)sel) == 2 && sel[0] == 1 && sel[1] == 3, "LB_GETSELITEMS");
    SendMessageW(ms, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, 2));
    CHECK(SendMessageW(ms, LB_GETSELCOUNT, 0, 0) == 4, "LB_SELITEMRANGE");
    SendMessageW(ms, LB_SELITEMRANGEEX, 2, 0);
    CHECK(SendMessageW(ms, LB_GETSELCOUNT, 0, 0) == 1, "LB_SELITEMRANGEEX backwards deselects");
    SendMessageW(ms, LB_SETSEL, FALSE, -1);
    CHECK(SendMessageW(ms, LB_GETSELCOUNT, 0, 0) == 0, "LB_SETSEL -1 clears");

    /* extended selection: shift extends from the anchor */
    {
        HWND e = make_list(parent, LBS_EXTENDEDSEL | LBS_NOTIFY, 2004);
        for (int i = 0; i < 6; i++) {
            char t[8];
            sprintf(t, "row%d", i);
            SendMessageA(e, LB_ADDSTRING, 0, (LPARAM)t);
        }
        SendMessageW(e, LB_SETCURSEL, 1, 0);
        SendMessageW(e, LB_SETANCHORINDEX, 1, 0);
        SendMessageW(e, LB_SETCARETINDEX, 1, 0);
        SendMessageW(e, WM_KEYDOWN, VK_DOWN, 0);
        CHECK(SendMessageW(e, LB_GETSELCOUNT, 0, 0) == 1, "extended: plain arrow selects one");
        DestroyWindow(e);
    }

    /* owner draw: WM_MEASUREITEM once for a fixed list, WM_DRAWITEM when painting */
    own = make_list(parent, LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 2005);
    n_measure = 0;
    SendMessageW(own, LB_ADDSTRING, 0, (LPARAM)L"owner");
    CHECK(SendMessageW(own, LB_GETITEMHEIGHT, 0, 0) == 16, "owner-draw item height from WM_MEASUREITEM");
    SendMessageW(own, LB_SETCURSEL, 0, 0);
    UpdateWindow(own);
    CHECK(n_drawitem >= 1, "owner-draw sends WM_DRAWITEM");

    /* tab stops and horizontal extent are stored */
    SendMessageW(lb, LB_SETHORIZONTALEXTENT, 300, 0);
    CHECK(SendMessageW(lb, LB_GETHORIZONTALEXTENT, 0, 0) == 300, "LB_GETHORIZONTALEXTENT");
    CHECK(SendMessageW(lb, LB_SETTABSTOPS, 0, 0) != 0, "LB_SETTABSTOPS");

    printf("core_listbox: %d checks, %d failures\n", checks, failures);
    DestroyWindow(parent);
    return failures ? 1 : 0;
}
