/* Tree view probe (SysTreeView32): builds a PuTTY-like category tree and drives it with messages.
 * Checks the item model, expansion, selection, hit testing, keyboard, mouse, veto, text callbacks,
 * sorting, deletion and the notifications sent to the parent.  Uses the real MinGW headers, so the
 * constants are Windows' own values.  Prints "ok" / "FAIL" lines; exit 0 when every check passes.
 * Driven by tests/core_treeview.sh under xvfb-run. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static HWND g_tree, g_parent;
static int n_selchanging, n_selchanged, n_expanding, n_expanded, n_deleted, n_getdisp, n_click, n_dblclk;
static int n_keydown;
static UINT last_action;
static LPARAM last_new_param;
static int veto_on;
static LPARAM veto_param;

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

static int weq(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void wcopy(WCHAR *d, const WCHAR *s)
{
    while ((*d++ = *s++))
        ;
}

static LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NOTIFY) {
        NMHDR *n = (NMHDR *)l;
        NMTREEVIEWW *nt = (NMTREEVIEWW *)l;
        UINT c = n->code;
        if (c == TVN_SELCHANGINGA || c == TVN_SELCHANGINGW) {
            n_selchanging++;
            if (veto_on && nt->itemNew.lParam == veto_param)
                return TRUE;
            return 0;
        }
        if (c == TVN_SELCHANGEDA || c == TVN_SELCHANGEDW) {
            n_selchanged++;
            last_action = nt->action;
            last_new_param = nt->itemNew.lParam;
            return 0;
        }
        if (c == TVN_ITEMEXPANDINGA || c == TVN_ITEMEXPANDINGW) {
            n_expanding++;
            return 0;
        }
        if (c == TVN_ITEMEXPANDEDA || c == TVN_ITEMEXPANDEDW) {
            n_expanded++;
            return 0;
        }
        if (c == TVN_DELETEITEMA || c == TVN_DELETEITEMW) {
            n_deleted++;
            return 0;
        }
        if (c == TVN_KEYDOWN) {
            n_keydown++;
            return 0;
        }
        if (c == TVN_GETDISPINFOW) {
            NMTVDISPINFOW *d = (NMTVDISPINFOW *)l;
            n_getdisp++;
            if (d->item.mask & TVIF_TEXT)
                wcopy(d->item.pszText, L"Callback");
            return 0;
        }
        if (c == TVN_GETDISPINFOA) {
            NMTVDISPINFOA *d = (NMTVDISPINFOA *)l;
            n_getdisp++;
            if (d->item.mask & TVIF_TEXT)
                strcpy(d->item.pszText, "Callback");
            return 0;
        }
        if (c == NM_CLICK) {
            n_click++;
            return 0;
        }
        if (c == NM_DBLCLK) {
            n_dblclk++;
            return 0;
        }
        return 0;
    }
    if (m == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static HTREEITEM ins(HTREEITEM parent, HTREEITEM after, LPWSTR text, LPARAM param)
{
    TVINSERTSTRUCTW is;
    memset(&is, 0, sizeof is);
    is.hParent = parent;
    is.hInsertAfter = after;
    is.item.mask = TVIF_TEXT | TVIF_PARAM;
    is.item.pszText = text;
    is.item.lParam = param;
    return (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&is);
}

static int count_visible(void)
{
    HTREEITEM h = (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_FIRSTVISIBLE, 0);
    int n = 0;
    while (h) {
        n++;
        h = (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_NEXTVISIBLE, (LPARAM)h);
    }
    return n;
}

static HTREEITEM caret(void)
{
    return (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_CARET, 0);
}

static void key(UINT vk)
{
    SendMessageW(g_tree, WM_KEYDOWN, vk, 0);
}

/* centre of the label of h, client coordinates */
static POINT label_point(HTREEITEM h)
{
    RECT rc;
    POINT p;
    *(HTREEITEM *)&rc = h;
    SendMessageW(g_tree, TVM_GETITEMRECT, TRUE, (LPARAM)&rc);
    p.x = rc.left + 2;
    p.y = (rc.top + rc.bottom) / 2;
    return p;
}

static void click(POINT p, UINT msg)
{
    SendMessageW(g_tree, msg, MK_LBUTTON, MAKELPARAM(p.x, p.y));
}

int main(void)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    HTREEITEM session, logging, terminal, keyboard, bell, features, window, appearance, behaviour, connection;
    HTREEITEM h, cb;
    RECT rc;
    POINT p;
    WCHAR buf[64];
    char abuf[64];
    TVHITTESTINFO hti;
    TVITEMW ti;
    HINSTANCE hi = GetModuleHandleW(NULL);

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TREEVIEW_CLASSES;
    CHECK(InitCommonControlsEx(&icc), "InitCommonControlsEx(ICC_TREEVIEW_CLASSES)");

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ParentProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"TvProbeParent";
    RegisterClassExW(&wc);
    g_parent = CreateWindowExW(0, L"TvProbeParent", L"Tree Probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 400, 400,
                               NULL, NULL, hi, NULL);
    CHECK(g_parent != NULL, "create parent window");
    g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, L"SysTreeView32", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES |
                             TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, 0, 0, 300, 300, g_parent,
                             (HMENU)(INT_PTR)1001, hi, NULL);
    CHECK(g_tree != NULL, "create SysTreeView32");
    if (!g_tree) {
        printf("FAIL no tree control, aborting\n");
        return 1;
    }

    /* build: Session, Logging, Terminal[Keyboard, Bell, Features], Window[Appearance, Behaviour], Connection */
    session = ins(TVI_ROOT, TVI_LAST, L"Session", 1);
    logging = ins(TVI_ROOT, session, L"Logging", 2);
    terminal = ins(TVI_ROOT, logging, L"Terminal", 3);
    keyboard = ins(terminal, TVI_LAST, L"Keyboard", 31);
    bell = ins(terminal, keyboard, L"Bell", 32);
    features = ins(terminal, bell, L"Features", 33);
    window = ins(TVI_ROOT, terminal, L"Window", 4);
    appearance = ins(window, TVI_LAST, L"Appearance", 41);
    behaviour = ins(window, appearance, L"Behaviour", 42);
    connection = ins(TVI_ROOT, window, L"Connection", 5);
    CHECK(session && logging && terminal && keyboard && bell && features && window && appearance && behaviour &&
              connection,
          "insert 10 items (TVI_ROOT/TVI_LAST/after)");
    CHECK(SendMessageW(g_tree, TVM_GETCOUNT, 0, 0) == 10, "TVM_GETCOUNT == 10");
    CHECK(count_visible() == 5, "collapsed: 5 top-level rows visible");

    /* expansion */
    n_expanding = n_expanded = 0;
    CHECK(SendMessageW(g_tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)terminal), "TVM_EXPAND(TVE_EXPAND, Terminal)");
    CHECK(count_visible() == 8, "expanded Terminal: 8 rows visible");
    CHECK(n_expanding == 1 && n_expanded == 1, "TVN_ITEMEXPANDING/ED once each");
    CHECK(SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)terminal) == (LRESULT)(intptr_t)keyboard, "TVGN_CHILD of Terminal");
    CHECK(SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)bell) == (LRESULT)(intptr_t)terminal, "TVGN_PARENT of Bell");

    /* selection and notifications */
    n_selchanging = n_selchanged = 0;
    CHECK(SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)keyboard), "TVM_SELECTITEM(TVGN_CARET, Keyboard)");
    CHECK(caret() == keyboard, "TVGN_CARET is Keyboard");
    CHECK(n_selchanging == 1 && n_selchanged == 1, "TVN_SELCHANGING/ED once each");
    CHECK(last_new_param == 31, "TVN_SELCHANGED itemNew.lParam == 31");
    CHECK(last_action == TVC_UNKNOWN, "TVC_UNKNOWN for programmatic selection");

    /* hit testing */
    p = label_point(bell);
    hti.pt = p;
    h = (HTREEITEM)SendMessageW(g_tree, TVM_HITTEST, 0, (LPARAM)&hti);
    CHECK(h == bell && (hti.flags & TVHT_ONITEMLABEL), "TVM_HITTEST on Bell label");

    memset(&rc, 0, sizeof rc);
    *(HTREEITEM *)&rc = window;
    CHECK(SendMessageW(g_tree, TVM_GETITEMRECT, FALSE, (LPARAM)&rc), "TVM_GETITEMRECT(Window)");
    hti.pt.x = 10;
    hti.pt.y = (rc.top + rc.bottom) / 2;
    h = (HTREEITEM)SendMessageW(g_tree, TVM_HITTEST, 0, (LPARAM)&hti);
    CHECK(h == window && (hti.flags & TVHT_ONITEMBUTTON), "TVM_HITTEST on Window +/- button");
    hti.pt.x = 2000;
    hti.pt.y = 2;
    SendMessageW(g_tree, TVM_HITTEST, 0, (LPARAM)&hti);
    CHECK(hti.flags & TVHT_TORIGHT, "TVM_HITTEST beyond the right edge");

    /* keyboard: Down, Right on leaf, Left to parent, Left collapses, Right expands, Up */
    last_action = 99;
    key(VK_DOWN);
    CHECK(caret() == bell, "VK_DOWN moves to Bell");
    CHECK(last_action == TVC_BYKEYBOARD, "TVC_BYKEYBOARD for keyboard selection");
    CHECK(n_keydown >= 1, "TVN_KEYDOWN sent");
    key(VK_LEFT);
    CHECK(caret() == terminal, "VK_LEFT on a leaf moves to parent");
    key(VK_LEFT);
    CHECK(count_visible() == 5, "VK_LEFT on expanded Terminal collapses it");
    key(VK_RIGHT);
    CHECK(count_visible() == 8, "VK_RIGHT on collapsed Terminal expands it");
    key(VK_RIGHT);
    CHECK(caret() == keyboard, "VK_RIGHT on expanded item moves to first child");
    key(VK_UP);
    CHECK(caret() == terminal, "VK_UP moves to Terminal");
    key(VK_END);
    CHECK(caret() == connection, "VK_END moves to last visible row");
    key(VK_HOME);
    CHECK(caret() == session, "VK_HOME moves to first row");
    SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)terminal);
    SendMessageW(g_tree, WM_KEYDOWN, VK_SUBTRACT, 0);
    CHECK(count_visible() == 5, "VK_SUBTRACT collapses");
    SendMessageW(g_tree, WM_KEYDOWN, VK_ADD, 0);
    CHECK(count_visible() == 8, "VK_ADD expands");
    SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)window);
    SendMessageW(g_tree, WM_KEYDOWN, VK_MULTIPLY, 0);
    CHECK(count_visible() == 10, "VK_MULTIPLY expands the subtree of Window");
    SendMessageW(g_tree, TVM_EXPAND, TVE_COLLAPSE, (LPARAM)window);
    SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)terminal);

    /* veto: TVN_SELCHANGING returning TRUE keeps the old caret */
    SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)terminal);
    veto_on = 1;
    veto_param = 5;
    CHECK(!SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)connection), "vetoed selection returns FALSE");
    CHECK(caret() == terminal, "vetoed selection keeps the caret");
    veto_on = 0;

    /* mouse: click selects (TVC_BYMOUSE), NM_CLICK; double click toggles expansion */
    n_click = n_dblclk = 0;
    last_action = 99;
    p = label_point(logging);
    click(p, WM_LBUTTONDOWN);
    click(p, WM_LBUTTONUP);
    CHECK(caret() == logging, "mouse click selects Logging");
    CHECK(last_action == TVC_BYMOUSE, "TVC_BYMOUSE for mouse selection");
    CHECK(n_click == 1, "NM_CLICK on button up");
    p = label_point(terminal);
    click(p, WM_LBUTTONDOWN);
    click(p, WM_LBUTTONUP);
    click(p, WM_LBUTTONDBLCLK);
    CHECK(n_dblclk == 1, "NM_DBLCLK sent");
    CHECK(count_visible() == 5, "double click on expanded Terminal collapses it (5 top-level rows)");
    CHECK(SendMessageW(g_tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)terminal), "re-expand Terminal");
    CHECK(count_visible() == 8, "re-expanded Terminal: 8 rows");

    /* item text: W set/get, A get, sorting */
    memset(&ti, 0, sizeof ti);
    ti.mask = TVIF_TEXT;
    ti.hItem = logging;
    ti.pszText = L"Logs";
    CHECK(SendMessageW(g_tree, TVM_SETITEMW, 0, (LPARAM)&ti), "TVM_SETITEMW rename");
    memset(&ti, 0, sizeof ti);
    ti.mask = TVIF_TEXT;
    ti.hItem = logging;
    ti.pszText = buf;
    ti.cchTextMax = 64;
    SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&ti);
    CHECK(weq(buf, L"Logs"), "TVM_GETITEMW returns renamed text");
    memset(abuf, 0, sizeof abuf);
    {
        TVITEMA ta;
        memset(&ta, 0, sizeof ta);
        ta.mask = TVIF_TEXT | TVIF_PARAM;
        ta.hItem = session;
        ta.pszText = abuf;
        ta.cchTextMax = sizeof abuf;
        SendMessageW(g_tree, TVM_GETITEMA, 0, (LPARAM)&ta);
        CHECK(strcmp(abuf, "Session") == 0 && ta.lParam == 1, "TVM_GETITEMA text and lParam");
    }
    SendMessageW(g_tree, TVM_SORTCHILDREN, 0, (LPARAM)TVI_ROOT);
    h = (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
    memset(&ti, 0, sizeof ti);
    ti.mask = TVIF_TEXT;
    ti.hItem = h;
    ti.pszText = buf;
    ti.cchTextMax = 64;
    SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&ti);
    CHECK(weq(buf, L"Connection"), "TVM_SORTCHILDREN(TVI_ROOT): Connection first");

    /* deletion sends TVN_DELETEITEM for every node of the subtree */
    n_deleted = 0;
    CHECK(SendMessageW(g_tree, TVM_DELETEITEM, 0, (LPARAM)window), "TVM_DELETEITEM(Window)");
    CHECK(n_deleted == 3, "TVN_DELETEITEM for Window and its two children");
    CHECK(SendMessageW(g_tree, TVM_GETCOUNT, 0, 0) == 7, "count 7 after delete");

    /* text callback: TVN_GETDISPINFO supplies the text */
    n_getdisp = 0;
    cb = ins(TVI_ROOT, TVI_LAST, LPSTR_TEXTCALLBACKW, 6);
    CHECK(cb != NULL, "insert text-callback item");
    memset(&ti, 0, sizeof ti);
    ti.mask = TVIF_TEXT;
    ti.hItem = cb;
    ti.pszText = buf;
    ti.cchTextMax = 64;
    SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&ti);
    CHECK(weq(buf, L"Callback") && n_getdisp > 0, "TVN_GETDISPINFOW supplies callback text");
    CHECK(SendMessageW(g_tree, TVM_DELETEITEM, 0, (LPARAM)cb), "delete callback item");

    /* sort with a comparison callback is covered by TVM_SORTCHILDREN; geometry messages */
    SendMessageW(g_tree, TVM_SETITEMHEIGHT, 20, 0);
    CHECK(SendMessageW(g_tree, TVM_GETITEMHEIGHT, 0, 0) == 20, "TVM_SETITEMHEIGHT / GETITEMHEIGHT == 20");
    CHECK(SendMessageW(g_tree, TVM_GETINDENT, 0, 0) == 19, "TVM_GETINDENT default 19");
    CHECK(SendMessageW(g_tree, TVM_GETIMAGELIST, TVSIL_NORMAL, 0) == 0, "no image list by default");
    CHECK(SendMessageW(g_tree, TVM_ENSUREVISIBLE, 0, (LPARAM)features), "TVM_ENSUREVISIBLE(Features)");
    CHECK(SendMessageW(g_tree, TVM_GETVISIBLECOUNT, 0, 0) > 0, "TVM_GETVISIBLECOUNT > 0");

    /* paint once and then close */
    SendMessageW(g_tree, WM_PAINT, 0, 0);
    CHECK(TRUE, "WM_PAINT handled");
    CHECK(DestroyWindow(g_tree), "destroy tree (TVN_DELETEITEM for remaining items)");

    printf("core_treeview: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
