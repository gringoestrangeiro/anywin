/* Dialog probe: the same template through the A and the W dialog calls.  Checks that a dialog and its
 * controls take the charset of the call that created them (IsWindowUnicode), that WM_NOTIFYFORMAT is
 * answered from that charset, that the tree sends the matching TVN_SELCHANGED code (A or W) to the
 * DLGPROC, that the A / W text helpers round-trip, and that the template controls are visible and sized.
 * Driven by tests/core_dialog.sh under xvfb-run.  Prints "ok" / "FAIL" lines. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define IDC_TREE 101
#define IDC_LBL 102
#define IDC_EDIT 103
#define IDC_CHK 104
#define IDC_RAD1 105
#define IDC_GRP 106
#define IDD_ANSI 200
#define IDD_WIDE 201
#define WM_PROBE_RUN (WM_APP + 1)

static int checks, failures;
static int n_sel_a, n_sel_w;
static int init_seen_uni_dlg, init_seen_uni_edit, init_seen_uni_tree;
static char text_a[64];
static WCHAR text_w[64];
static HWND g_tree;

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

/* the work done once the dialog is up: selection, text helpers, visibility, notify format */
static void run_checks(HWND dlg, int ansi, int *result)
{
    HWND edit = GetDlgItem(dlg, IDC_EDIT), chk = GetDlgItem(dlg, IDC_CHK), rad = GetDlgItem(dlg, IDC_RAD1);
    HWND grp = GetDlgItem(dlg, IDC_GRP), lbl = GetDlgItem(dlg, IDC_LBL);
    RECT r;

    CHECK(IsWindowUnicode(dlg) == !ansi, ansi ? "A dialog is ANSI" : "W dialog is Unicode");
    CHECK(IsWindowUnicode(edit) == !ansi, ansi ? "A dialog: edit control is ANSI" : "W dialog: edit is Unicode");
    CHECK(IsWindowUnicode(g_tree) == !ansi, ansi ? "A dialog: tree is ANSI" : "W dialog: tree is Unicode");
    CHECK(IsWindowVisible(edit) && IsWindowVisible(chk) && IsWindowVisible(rad) && IsWindowVisible(grp) &&
              IsWindowVisible(lbl), ansi ? "A dialog: template controls visible" : "W dialog: template controls visible");
    GetWindowRect(edit, &r);
    CHECK(r.right - r.left > 100, "edit control sized from dialog units");
    CHECK(SendMessageW(dlg, WM_NOTIFYFORMAT, (WPARAM)g_tree, NF_QUERY) == (ansi ? NFR_ANSI : NFR_UNICODE),
          ansi ? "WM_NOTIFYFORMAT answers NFR_ANSI for an A dialog" : "WM_NOTIFYFORMAT answers NFR_UNICODE for a W dialog");

    /* selection: the tree notifies the DLGPROC; the DLGPROC counts both A and W codes */
    n_sel_a = n_sel_w = 0;
    if (ansi) {
        HTREEITEM second;
        second = (HTREEITEM)SendMessageA(g_tree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
        second = (HTREEITEM)SendMessageA(g_tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)second);
        SendMessageA(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)second);
    } else {
        HTREEITEM second = (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
        second = (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)second);
        SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)second);
    }
    CHECK(ansi ? n_sel_a >= 1 && n_sel_w == 0 : n_sel_w >= 1 && n_sel_a == 0,
          ansi ? "A dialog: TVN_SELCHANGEDA delivered" : "W dialog: TVN_SELCHANGEDW delivered");

    if (ansi) {
        SetDlgItemTextA(dlg, IDC_EDIT, "host.example");
        GetDlgItemTextA(dlg, IDC_EDIT, text_a, sizeof text_a);
        CHECK(strcmp(text_a, "host.example") == 0, "A dialog: SetDlgItemTextA / GetDlgItemTextA round trip");
        CHECK(SendDlgItemMessageA(dlg, IDC_EDIT, WM_GETTEXTLENGTH, 0, 0) == 12, "A dialog: WM_GETTEXTLENGTH via A");
    } else {
        SetDlgItemTextW(dlg, IDC_EDIT, L"host.wide");
        GetDlgItemTextW(dlg, IDC_EDIT, text_w, 64);
        CHECK(wcscmp(text_w, L"host.wide") == 0, "W dialog: SetDlgItemTextW / GetDlgItemTextW round trip");
        CHECK(SendDlgItemMessageW(dlg, IDC_EDIT, WM_GETTEXTLENGTH, 0, 0) == 9, "W dialog: WM_GETTEXTLENGTH via W");
    }
    CheckRadioButton(dlg, IDC_RAD1, IDC_RAD1, IDC_RAD1);
    CHECK(IsDlgButtonChecked(dlg, IDC_RAD1) == BST_CHECKED, ansi ? "A dialog: radio check" : "W dialog: radio check");
    *result = 7;
}

static INT_PTR CALLBACK ProcCommon(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, int ansi)
{
    if (msg == WM_INITDIALOG) {
        HTREEITEM root, child;
        g_tree = GetDlgItem(dlg, IDC_TREE);
        init_seen_uni_dlg = IsWindowUnicode(dlg);
        init_seen_uni_edit = IsWindowUnicode(GetDlgItem(dlg, IDC_EDIT));
        init_seen_uni_tree = IsWindowUnicode(g_tree);
        if (ansi) {
            TVINSERTSTRUCTA ins;
            memset(&ins, 0, sizeof ins);
            ins.hParent = TVI_ROOT;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT;
            ins.item.pszText = "Session";
            root = (HTREEITEM)SendMessageA(g_tree, TVM_INSERTITEMA, 0, (LPARAM)&ins);
            ins.hParent = root;
            ins.item.pszText = "Logging";
            child = (HTREEITEM)SendMessageA(g_tree, TVM_INSERTITEMA, 0, (LPARAM)&ins);
            (void)child;
            ins.hParent = TVI_ROOT;
            ins.item.pszText = "Terminal";
            SendMessageA(g_tree, TVM_INSERTITEMA, 0, (LPARAM)&ins);
        } else {
            TVINSERTSTRUCTW ins;
            memset(&ins, 0, sizeof ins);
            ins.hParent = TVI_ROOT;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT;
            ins.item.pszText = L"Session";
            root = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
            ins.hParent = root;
            ins.item.pszText = L"Logging";
            child = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
            (void)child;
            ins.hParent = TVI_ROOT;
            ins.item.pszText = L"Terminal";
            SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
        }
        PostMessageW(dlg, WM_PROBE_RUN, 0, 0);
        return TRUE;
    }
    if (msg == WM_NOTIFY) {
        NMHDR *n = (NMHDR *)lp;
        if (n->code == TVN_SELCHANGEDA)
            n_sel_a++;
        else if (n->code == TVN_SELCHANGEDW)
            n_sel_w++;
        return FALSE;
    }
    if (msg == WM_PROBE_RUN) {
        int res = 0;
        run_checks(dlg, ansi, &res);
        EndDialog(dlg, res);
        return TRUE;
    }
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK ProcA(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) { return ProcCommon(dlg, msg, wp, lp, 1); }
static INT_PTR CALLBACK ProcW(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) { return ProcCommon(dlg, msg, wp, lp, 0); }

int main(void)
{
    INITCOMMONCONTROLSEX icc;
    HINSTANCE hi = GetModuleHandleW(NULL);
    INT_PTR r;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    r = DialogBoxParamA(hi, MAKEINTRESOURCEA(IDD_ANSI), NULL, ProcA, 0);
    printf("DialogBoxParamA -> %ld err=%lu\n", (long)r, GetLastError());
    CHECK(r == 7, "DialogBoxParamA returns the EndDialog result");
    r = DialogBoxParamW(hi, MAKEINTRESOURCEW(IDD_WIDE), NULL, ProcW, 0);
    CHECK(r == 7, "DialogBoxParamW returns the EndDialog result");

    printf("core_dialog: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
