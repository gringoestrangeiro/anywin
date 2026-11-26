/* anywin dialog manager (S-DLG): the "#32770" dialog class, DialogBox/CreateDialog from the
 * DLGTEMPLATE / DLGTEMPLATEEX resources (or an in-memory template), modal loops, the default
 * dialog procedure, Tab / Enter / Esc / mnemonic handling in IsDialogMessage, and the
 * control helpers (GetDlgItem, SetDlgItemText, CheckDlgButton, ...).
 *
 * Dialog state lives in the window's extra bytes (see AW_DLG_* below): the user's DLGPROC, its
 * result slot and a pointer to a dlg_state record that is freed when the window is destroyed.
 * Positions in templates are dialog units: x * baseX / 4 and y * baseY / 8, where the base units
 * come from the average character width and the height of the dialog font. */
#include "user32_int.h"
#include "gdi_int.h"
#include "aw_api.h"
#include <strings.h>
#include <limits.h>

#ifndef DS_ABSALIGN
#define DS_ABSALIGN 0x00000001L
#define DS_SETFONT 0x00000040L
#define DS_MODALFRAME 0x00000080L
#define DS_SETFOREGROUND 0x00000200L
#define DS_CENTER 0x00000800L
#define DS_CENTERMOUSE 0x00001000L
#endif
#define AW_DS_FIXEDSYS 0x00000008L
#define AW_BS_TYPEMASK 0x0000000FL
#define AW_BS_DEFPUSHBUTTON 0x01L
#define AW_BS_RADIOBUTTON 0x04L
#define AW_BS_AUTORADIOBUTTON 0x09L
#define AW_BS_PUSHBUTTON 0x00L
#ifndef BN_CLICKED
#define BN_CLICKED 0
#endif
#ifndef BST_CHECKED
#define BST_CHECKED 1
#endif
#define AW_BM_SETCHECK 0x00F1
#define AW_BM_GETCHECK 0x00F0
#define AW_BM_SETSTYLE 0x00F4
#define AW_BM_CLICK 0x00F5
#define AW_WM_SETFONT 0x0030
#define AW_WM_INITDIALOG 0x0110
#define AW_WM_NEXTDLGCTL 0x0028
#define AW_WM_CLOSE 0x0010
#define AW_WM_DESTROY 0x0002
#define AW_WM_NCDESTROY 0x0082
#define AW_WM_NULL 0x0000
#define AW_WM_NCCREATE 0x0081
#define AW_WM_KEYDOWN 0x0100
#define AW_WM_SYSKEYDOWN 0x0104
#define AW_WM_SYSCHAR 0x0106
#define AW_WM_COMMAND 0x0111
#define AW_WM_GETDLGCODE 0x0087
#define AW_WM_CTLCOLORMSGBOX 0x0132
#define AW_WM_CTLCOLORDLG 0x0136
#define AW_WM_CTLCOLORSTATIC 0x0138
#define AW_DM_GETDEFID (0x0400 + 0)
#define AW_DM_SETDEFID (0x0400 + 1)
#define AW_DC_HASDEFID 0x534C
#define AW_RT_DIALOG ((const void *)(uintptr_t)5)
#define AW_DLG_CLASS_NAME u"#32770"
/* extra bytes: DWLP_MSGRESULT / DWLP_DLGPROC / DWLP_USER as in Win64, then 48 bytes that programs
 * may use (DLGWINDOWEXTRA on x64 is 48; PuTTY writes at offset 30), then our private record */
#define AW_DLG_EXTRA 56
#define AW_DLG_MSGRESULT 0
#define AW_DLG_PROC 8
#define AW_DLG_USER 16
#define AW_DLG_STATE 48
#define AW_ERROR_INVALID_PARAMETER 87
#define AW_ERROR_CONTROL_ID_NOT_FOUND 1426
#define AW_LOWORD(v) ((WORD)((uintptr_t)(v) & 0xFFFF))
#define AW_HIWORD(v) ((WORD)(((uintptr_t)(v) >> 16) & 0xFFFF))
#define AW_MAKELONG(a, b) ((LONG)(((WORD)((a) & 0xFFFF)) | ((DWORD)((WORD)((b) & 0xFFFF)) << 16)))
#define AW_MAKELPARAM(a, b) ((LPARAM)AW_MAKELONG(a, b))
#define AW_TEMPLATE_BOUND (64 * 1024)

typedef struct dlg_state {
    int modal;
    volatile int done;
    INT_PTR result;
    HFONT font;       /* the dialog font when the template asked for one (DS_SETFONT) */
    int own_font;
    int base_x, base_y;
    UINT defid;       /* DM_GETDEFID / DM_SETDEFID */
} dlg_state;

typedef struct {
    DLGPROC proc;
    dlg_state *st;
} dlg_init;

static dlg_state *st_of(HWND h)
{
    return (dlg_state *)(intptr_t)__aw_user32_GetWindowLongPtrW(h, AW_DLG_STATE);
}

static int mul_div(int a, int b, int c)
{
    if (c == 0)
        return 0;
    long long v = (long long)a * b;
    return (int)(v >= 0 ? (v + c / 2) / c : (v - c / 2) / c);
}

/* ---------------------------------------------------------------------- */
/* navigation                                                              */
/* ---------------------------------------------------------------------- */

/* aw_wcmp_ci returns 0 for equal strings */
static int is_button_name(const WCHAR *cls)
{
    return aw_wcmp_ci(cls, u"Button") == 0;
}

static int is_radio(HWND c)
{
    WCHAR cls[32];
    if (!__aw_user32_GetClassNameW(c, cls, 32) || !is_button_name(cls))
        return 0;
    LONG st = __aw_user32_GetWindowLongW(c, -16);
    int t = (int)(st & AW_BS_TYPEMASK);
    return t == AW_BS_RADIOBUTTON || t == AW_BS_AUTORADIOBUTTON;
}

static int is_button_class(HWND c)
{
    WCHAR cls[32];
    return __aw_user32_GetClassNameW(c, cls, 32) && is_button_name(cls);
}

/* the direct child of dlg that contains h (h itself when it is a direct child) */
static HWND direct_child(HWND dlg, HWND h)
{
    while (h && __aw_user32_GetParent(h) != dlg)
        h = __aw_user32_GetParent(h);
    return h;
}

/* children of dlg in tab order: z-order from the bottom, as GetWindow(GW_CHILD/GW_HWNDNEXT) */
static int children_of(HWND dlg, HWND *out, int cap)
{
    int n = 0;
    for (HWND c = __aw_user32_GetWindow(dlg, GW_CHILD); c && n < cap; c = __aw_user32_GetWindow(c, GW_HWNDNEXT))
        out[n++] = c;
    return n;
}

/* a tab stop: WS_TABSTOP, visible and enabled; of a radio group only the checked button (or the
 * first one when none is checked) is a stop */
static int is_tab_stop(HWND dlg, HWND *kids, int n, int i)
{
    HWND c = kids[i];
    LONG st = __aw_user32_GetWindowLongW(c, -16);
    if (!(st & WS_TABSTOP) || !__aw_user32_IsWindowVisible(c) || !__aw_user32_IsWindowEnabled(c))
        return 0;
    if (!is_radio(c))
        return 1;
    /* find the group: the run of children starting at the last WS_GROUP before i */
    int g = i;
    while (g > 0 && !(__aw_user32_GetWindowLongW(kids[g], -16) & WS_GROUP))
        g--;
    int chosen = -1;
    for (int j = g; j < n; j++) {
        if (j > g && (__aw_user32_GetWindowLongW(kids[j], -16) & WS_GROUP))
            break;
        if (!is_radio(kids[j]))
            continue;
        if (__aw_user32_SendMessageW(kids[j], AW_BM_GETCHECK, 0, 0) == BST_CHECKED) {
            chosen = j;
            break;
        }
        if (chosen < 0)
            chosen = j;
    }
    (void)dlg;
    return chosen == i;
}

static HWND next_tab(HWND dlg, HWND from, int prev)
{
    HWND kids[512];
    int n = children_of(dlg, kids, 512);
    if (n == 0)
        return NULL;
    int start = -1;
    for (int i = 0; i < n; i++)
        if (kids[i] == from)
            start = i;
    /* in reverse order the children are visited last to first */
    for (int step = 1; step <= n; step++) {
        int idx;
        if (start < 0)
            idx = prev ? n - step : step - 1;
        else
            idx = prev ? ((start - step) % n + n) % n : (start + step) % n;
        if (is_tab_stop(dlg, kids, n, idx))
            return kids[idx];
    }
    return NULL;
}

/* the item with the given id among the direct children */
static HWND item_of(HWND dlg, int id)
{
    for (HWND c = __aw_user32_GetWindow(dlg, GW_CHILD); c; c = __aw_user32_GetWindow(c, GW_HWNDNEXT))
        if (__aw_user32_GetDlgCtrlID(c) == id)
            return c;
    return NULL;
}

static void dlg_next_ctl(HWND dlg, WPARAM wp, LPARAM lp)
{
    HWND target;
    if (AW_LOWORD(lp)) {
        target = (HWND)wp; /* the control itself */
    } else {
        HWND cur = __aw_user32_GetFocus();
        if (cur && !__aw_user32_IsChild(dlg, cur))
            cur = NULL;
        else if (cur)
            cur = direct_child(dlg, cur);
        target = next_tab(dlg, cur, wp != 0);
    }
    if (target)
        __aw_user32_SetFocus(target);
}

/* ---------------------------------------------------------------------- */
/* end / default procedure                                                 */
/* ---------------------------------------------------------------------- */

/* EndDialog: a modal dialog returns result from its DialogBox call; a modeless one is destroyed */
static BOOL dlg_end(HWND h, INT_PTR result)
{
    dlg_state *st = st_of(h);
    if (!st) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (st->modal) {
        st->result = result;
        st->done = 1;
        __aw_user32_PostMessageW(h, AW_WM_NULL, 0, 0); /* wake the modal loop */
    } else {
        __aw_user32_DestroyWindow(h);
    }
    return TRUE;
}

/* the dialog's own default procedure; its arguments are always UTF-16 (the class procedure's charset) */
static LRESULT dlg_default(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    dlg_state *st = st_of(h);
    switch (msg) {
    case AW_WM_CLOSE:
        dlg_end(h, IDCANCEL);
        return 0;
    case AW_WM_COMMAND: {
        UINT id = AW_LOWORD(wp);
        if ((id == IDOK || id == IDCANCEL) && (lp == 0 || AW_HIWORD(wp) == BN_CLICKED)) {
            dlg_end(h, id);
            return 0;
        }
        break;
    }
    case AW_WM_NEXTDLGCTL:
        dlg_next_ctl(h, wp, lp);
        return 0;
    case 0x0132: /* WM_CTLCOLORMSGBOX (WM_CTLCOLOREDIT is 0x0133 and keeps the window colour) */
    case 0x0135: /* WM_CTLCOLORBTN */
    case AW_WM_CTLCOLORSTATIC:
    case 0x0137: /* WM_CTLCOLORSCROLLBAR */
        /* DefDlgProc paints static text, buttons and scroll bars on the dialog face (edits and list
         * boxes keep the window colour from DefWindowProc) */
        __aw_gdi32_SetBkColor((HDC)wp, aw_syscolor(COLOR_BTNFACE));
        __aw_gdi32_SetTextColor((HDC)wp, aw_syscolor(msg == AW_WM_CTLCOLORSTATIC ? COLOR_WINDOWTEXT : COLOR_BTNTEXT));
        return (LRESULT)(uintptr_t)__aw_user32_GetSysColorBrush(COLOR_BTNFACE);
    case AW_WM_GETDLGCODE:
        return 0;
    case AW_DM_GETDEFID:
        return st && st->defid ? (LRESULT)AW_MAKELONG(st->defid, AW_DC_HASDEFID) : 0;
    case AW_DM_SETDEFID: {
        if (!st)
            return 0;
        UINT old = st->defid;
        st->defid = (UINT)AW_LOWORD(wp);
        if (old) {
            HWND b = item_of(h, (int)old);
            if (b && is_button_class(b))
                __aw_user32_SendMessageW(b, AW_BM_SETSTYLE, AW_BS_PUSHBUTTON, TRUE);
        }
        HWND nb = item_of(h, (int)st->defid);
        if (nb && is_button_class(nb))
            __aw_user32_SendMessageW(nb, AW_BM_SETSTYLE, AW_BS_DEFPUSHBUTTON, TRUE);
        return TRUE;
    }
    case AW_WM_NCDESTROY:
        if (st) {
            __aw_user32_SetWindowLongPtrW(h, AW_DLG_STATE, 0);
            if (st->own_font)
                __aw_gdi32_DeleteObject(st->font);
            /* a modal dialog's record is released by run_modal, which reads its result */
            if (!st->modal)
                free(st);
            else
                st->done = 1;
        }
        break;
    default:
        break;
    }
    return __aw_user32_DefWindowProcW(h, msg, wp, lp);
}

/* the class procedure gets UTF-16 messages; a dialog created by an A call hands its DLGPROC ANSI ones */
static LRESULT AWCALL dlg_class_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == AW_WM_NCCREATE) {
        /* CreateWindowEx passes the dlg_init record as lpCreateParams (first member of CREATESTRUCT) */
        dlg_init *di = *(dlg_init **)(void *)lp;
        if (di) {
            __aw_user32_SetWindowLongPtrW(h, AW_DLG_PROC, (LONG_PTR)(intptr_t)di->proc);
            __aw_user32_SetWindowLongPtrW(h, AW_DLG_STATE, (LONG_PTR)(intptr_t)di->st);
        }
        return __aw_user32_DefWindowProcW(h, msg, wp, lp);
    }
    DLGPROC up = (DLGPROC)(intptr_t)__aw_user32_GetWindowLongPtrW(h, AW_DLG_PROC);
    if (up && msg != AW_WM_NCDESTROY) {
        aw_win *w = aw_win_from_hwnd(h);
        INT_PTR r = (INT_PTR)aw_call_conv((WNDPROC)(void *)up, h, msg, wp, lp, 1, w ? w->wcs : 1);
        if (msg == AW_WM_INITDIALOG) {
            /* TRUE: the system focuses the control passed in wParam */
            if (r && wp)
                __aw_user32_SetFocus((HWND)wp);
            return 0;
        }
        if (r)
            return (LRESULT)__aw_user32_GetWindowLongPtrW(h, AW_DLG_MSGRESULT);
    }
    return dlg_default(h, msg, wp, lp);
}

void aw_dialogs_register(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = dlg_class_proc;
    wc.cbWndExtra = AW_DLG_EXTRA;
    wc.hbrBackground = (HBRUSH)(intptr_t)(COLOR_BTNFACE + 1);
    wc.lpszClassName = AW_DLG_CLASS_NAME;
    __aw_user32_RegisterClassExW(&wc);
    aw_class_mark_builtin(AW_DLG_CLASS_NAME);
}

static LRESULT AWCALL dlg_default_thunk(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return dlg_default(h, msg, wp, lp); }

API(user32, LRESULT, DefDlgProcA, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    return aw_call_conv((WNDPROC)(void *)dlg_default_thunk, h, msg, wp, lp, 0, 1);
}

API(user32, LRESULT, DefDlgProcW, (HWND h, UINT msg, WPARAM wp, LPARAM lp))
{
    return aw_call_conv((WNDPROC)(void *)dlg_default_thunk, h, msg, wp, lp, 1, 1);
}

API(user32, BOOL, EndDialog, (HWND h, INT_PTR result)) { return dlg_end(h, result); }

/* ---------------------------------------------------------------------- */
/* templates                                                               */
/* ---------------------------------------------------------------------- */

typedef struct {
    const uint8_t *base, *p, *end;
    int ok;
} tmpl_rd;

static int rd_need(tmpl_rd *r, size_t n)
{
    if (!r->ok || (size_t)(r->end - r->p) < n) {
        r->ok = 0;
        return 0;
    }
    return 1;
}

static uint16_t rd_u16(tmpl_rd *r)
{
    uint16_t v = 0;
    if (rd_need(r, 2)) {
        memcpy(&v, r->p, 2);
        r->p += 2;
    }
    return v;
}

static uint32_t rd_u32(tmpl_rd *r)
{
    uint32_t v = 0;
    if (rd_need(r, 4)) {
        memcpy(&v, r->p, 4);
        r->p += 4;
    }
    return v;
}

static void rd_align(tmpl_rd *r)
{
    size_t off = (size_t)(r->p - r->base);
    size_t pad = (4 - off % 4) % 4;
    if (pad && rd_need(r, pad))
        r->p += pad;
}

/* a menu / class / title / item name: either an ordinal (0xFFFF, id) or a NUL-terminated UTF-16 string */
typedef struct {
    int is_ord;
    WORD ord;
    const WCHAR *s; /* string (possibly empty) when !is_ord */
} tmpl_str;

static tmpl_str rd_str(tmpl_rd *r)
{
    tmpl_str t;
    memset(&t, 0, sizeof t);
    if (!r->ok)
        return t;
    if (rd_need(r, 2) && *(const WORD *)(const void *)r->p == 0xFFFF) {
        r->p += 2;
        t.is_ord = 1;
        t.ord = rd_u16(r);
        return t;
    }
    const uint8_t *start = r->p;
    size_t units = 0;
    for (;;) {
        if (!rd_need(r, 2))
            break;
        WORD c;
        memcpy(&c, r->p, 2);
        r->p += 2;
        units++;
        if (c == 0)
            break;
    }
    t.s = (const WCHAR *)(const void *)start;
    if (units == 0)
        t.s = u"";
    return t;
}

static const WCHAR *class_of_ord(WORD ord)
{
    switch (ord) {
    case 0x80: return u"Button";
    case 0x81: return u"Edit";
    case 0x82: return u"Static";
    case 0x83: return u"ListBox";
    case 0x84: return u"ScrollBar";
    case 0x85: return u"ComboBox";
    default: return NULL;
    }
}

/* the base units of a font: average character width (of the 52 letters) and cell height */
static void base_units(aw_font *f, int *bx, int *by)
{
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    int sum = 0;
    for (int i = 0; i < 52; i++)
        sum += font_advance(f, (uint32_t)(unsigned char)alpha[i]);
    *bx = (sum + 26) / 52;
    if (*bx <= 0)
        *bx = 6;
    *by = font_metrics(f)->height;
    if (*by <= 0)
        *by = 13;
}

typedef struct {
    tmpl_str cls, title;
    DWORD style, exstyle, id;
    int x, y, cx, cy;
    const void *data;
    WORD cbdata;
} tmpl_item;

/* builds the dialog and its controls from a template; returns the dialog (not shown) or NULL */
static HWND dlg_from_template(const uint8_t *tp, size_t sz, HWND owner, DLGPROC proc, LPARAM init, int modal,
                              int *visible, int ansi)
{
    tmpl_rd r = {tp, tp, tp + sz, 1};
    int ex = 0;
    DWORD style = 0, exstyle = 0;
    WORD ncdit = 0;
    int x = 0, y = 0, cx = 0, cy = 0;
    if (rd_need(&r, 4) && *(const WORD *)(const void *)tp == 1 && *(const WORD *)(const void *)(tp + 2) == 0xFFFF) {
        ex = 1;
        rd_u16(&r); /* dlgVer */
        rd_u16(&r); /* signature */
        rd_u32(&r); /* helpID */
        exstyle = rd_u32(&r);
        style = rd_u32(&r);
        ncdit = rd_u16(&r);
        x = (short)rd_u16(&r);
        y = (short)rd_u16(&r);
        cx = (short)rd_u16(&r);
        cy = (short)rd_u16(&r);
    } else {
        style = rd_u32(&r);
        exstyle = rd_u32(&r);
        ncdit = rd_u16(&r);
        x = (short)rd_u16(&r);
        y = (short)rd_u16(&r);
        cx = (short)rd_u16(&r);
        cy = (short)rd_u16(&r);
    }
    rd_str(&r); /* menu */
    tmpl_str cls = rd_str(&r);
    tmpl_str title = rd_str(&r);
    (void)cls;
    WORD pointsize = 0, weight = 0;
    BYTE italic = 0;
    const WCHAR *face = NULL;
    if (style & DS_SETFONT) {
        pointsize = rd_u16(&r);
        if (ex) {
            weight = rd_u16(&r);
            if (rd_need(&r, 2)) {
                italic = r.p[0];
                r.p += 2; /* italic, charset */
            }
        }
        tmpl_str fs = rd_str(&r);
        face = fs.s ? fs.s : u"MS Shell Dlg";
    }
    if (!r.ok)
        return NULL;

    dlg_state *st = calloc(1, sizeof *st);
    if (!st)
        aw_fatal("user32: out of memory");
    st->modal = modal;
    st->font = NULL;
    /* the font the dialog and its controls use: the template's own, else the default GUI font */
    aw_font *af;
    if (style & DS_SETFONT) {
        int lfh = -mul_div(pointsize, 96, 72);
        if (lfh == 0)
            lfh = -11;
        char *face8 = aw_w2u(face, -1);
        af = font_open(face8 ? face8 : "MS Shell Dlg", lfh, 0, weight ? weight : 400, italic, 0, 0);
        free(face8);
        st->font = (HFONT)__aw_gdi32_CreateFontW(lfh, 0, 0, 0, weight ? weight : 400, italic, 0, 0, 1, 0, 0, 0, 0,
                                                 face);
        st->own_font = st->font != NULL;
    } else {
        af = gdi_default_font();
    }
    base_units(af, &st->base_x, &st->base_y);

    /* window rectangle: client size in pixels, grown to the frame of its style */
    DWORD wstyle = style & 0xFFFF0000u;
    wstyle &= ~WS_VISIBLE;
    if (!(wstyle & (WS_CHILD | WS_POPUP)))
        wstyle |= WS_POPUP;
    DWORD wex = exstyle;
    if (style & DS_MODALFRAME)
        wex |= WS_EX_DLGMODALFRAME;
    RECT frame = {0, 0, mul_div(cx, st->base_x, 4), mul_div(cy, st->base_y, 8)};
    __aw_user32_AdjustWindowRectEx(&frame, wstyle, FALSE, wex);
    int w = frame.right - frame.left, h = frame.bottom - frame.top;

    int px, py;
    if (style & DS_ABSALIGN) {
        px = x;
        py = y;
    } else {
        px = mul_div(x, st->base_x, 4);
        py = mul_div(y, st->base_y, 8);
    }
    if (style & (DS_CENTER | DS_CENTERMOUSE)) {
        RECT area;
        if (!owner || !__aw_user32_GetWindowRect(owner, &area)) {
            area.left = 0;
            area.top = 0;
            area.right = __aw_user32_GetSystemMetrics(0);
            area.bottom = __aw_user32_GetSystemMetrics(1);
        }
        px = area.left + ((area.right - area.left) - w) / 2;
        py = area.top + ((area.bottom - area.top) - h) / 2;
    } else if (!(style & DS_ABSALIGN) && owner) {
        POINT o = {0, 0};
        __aw_user32_ClientToScreen(owner, &o);
        px += o.x;
        py += o.y;
    }

    dlg_init di = {proc, st};
    const WCHAR *tt = title.is_ord || !title.s ? u"" : title.s;
    /* the dialog and its controls have the charset of the call that created the dialog */
    int wcs = ansi ? 0 : 1;
    HWND dlg = aw_create_window_cs(wex, AW_DLG_CLASS_NAME, tt, wstyle, px, py, w, h, owner, NULL, NULL, &di, wcs);
    if (!dlg) {
        if (st->own_font)
            __aw_gdi32_DeleteObject(st->font);
        free(st);
        return NULL;
    }

    /* the controls, in template order */
    for (WORD i = 0; i < ncdit && r.ok; i++) {
        rd_align(&r);
        tmpl_item it;
        memset(&it, 0, sizeof it);
        if (ex) {
            rd_u32(&r); /* helpID */
            it.exstyle = rd_u32(&r);
            it.style = rd_u32(&r);
            it.x = (short)rd_u16(&r);
            it.y = (short)rd_u16(&r);
            it.cx = (short)rd_u16(&r);
            it.cy = (short)rd_u16(&r);
            it.id = rd_u32(&r);
        } else {
            it.style = rd_u32(&r);
            it.exstyle = rd_u32(&r);
            it.x = (short)rd_u16(&r);
            it.y = (short)rd_u16(&r);
            it.cx = (short)rd_u16(&r);
            it.cy = (short)rd_u16(&r);
            it.id = rd_u16(&r);
        }
        it.cls = rd_str(&r);
        it.title = rd_str(&r);
        it.cbdata = rd_u16(&r);
        if (it.cbdata) {
            if (rd_need(&r, it.cbdata)) {
                it.data = r.p;
                r.p += it.cbdata;
            }
        }
        if (!r.ok)
            break;

        const WCHAR *cname = it.cls.is_ord ? class_of_ord(it.cls.ord) : it.cls.s;
        if (!cname)
            cname = u"Static";
        const WCHAR *ctitle = (!it.title.is_ord && it.title.s) ? it.title.s : NULL;
        DWORD cst = it.style | WS_CHILD;
        cst &= ~WS_VISIBLE;
        if (it.style & WS_VISIBLE)
            cst |= WS_VISIBLE;
        int ix = mul_div(it.x, st->base_x, 4), iy = mul_div(it.y, st->base_y, 8);
        int iw = mul_div(it.cx, st->base_x, 4), ih = mul_div(it.cy, st->base_y, 8);
        HWND c = aw_create_window_cs(it.exstyle, cname, ctitle ? ctitle : u"", cst, ix, iy, iw, ih, dlg,
                                     (HMENU)(uintptr_t)it.id, NULL, (void *)it.data, wcs);
        if (c) {
            /* new controls go on top of the z-order; restack to the bottom so that the sibling
             * list (GW_CHILD / GW_HWNDNEXT, the tab order) follows the template order */
            __aw_user32_SetWindowPos(c, HWND_BOTTOM, 0, 0, 0, 0, 0x0002 | 0x0001 | 0x0010);
        }
        if (c && st->font)
            __aw_user32_SendMessageW(c, AW_WM_SETFONT, (WPARAM)st->font, 0);
        if (c && !st->defid && is_button_name(cname) && (it.style & AW_BS_TYPEMASK) == AW_BS_DEFPUSHBUTTON)
            st->defid = (UINT)it.id; /* the first default push button is the dialog's default */
    }
    if (st->font)
        __aw_user32_SendMessageW(dlg, AW_WM_SETFONT, (WPARAM)st->font, 0);

    /* WM_INITDIALOG, with the first tab stop as the focus candidate */
    HWND focus = next_tab(dlg, NULL, 0);
    __aw_user32_SendMessageW(dlg, AW_WM_INITDIALOG, (WPARAM)focus, init);
    if (style & DS_SETFOREGROUND)
        __aw_user32_SetForegroundWindow(dlg);
    if (visible)
        *visible = (style & WS_VISIBLE) != 0;
    return dlg;
}

/* ---------------------------------------------------------------------- */
/* modal loop                                                              */
/* ---------------------------------------------------------------------- */

/* Shows a modal dialog, disables its owner and pumps messages until EndDialog.  Messages go
 * through IsDialogMessage first, so keyboard navigation works. */
static INT_PTR run_modal(HWND dlg, HWND owner, int visible)
{
    dlg_state *st = st_of(dlg);
    if (!st)
        return -1;
    int owner_enabled = owner && __aw_user32_IsWindowEnabled(owner);
    if (owner)
        __aw_user32_EnableWindow(owner, FALSE);
    (void)visible;
    __aw_user32_ShowWindow(dlg, SW_SHOW);
    __aw_user32_SetForegroundWindow(dlg);
    MSG m;
    while (!st->done) {
        BOOL got = __aw_user32_GetMessageW(&m, NULL, 0, 0);
        if (got <= 0) {
            if (got == 0)
                __aw_user32_PostQuitMessage((int)m.wParam); /* WM_QUIT: leave it for the caller */
            break;
        }
        if (!__aw_user32_IsDialogMessageW(dlg, &m)) {
            __aw_user32_TranslateMessage(&m);
            __aw_user32_DispatchMessageW(&m);
        }
    }
    INT_PTR result = st->result;
    __aw_user32_DestroyWindow(dlg);
    free(st); /* the dialog is destroyed; its record is no longer used */
    if (owner) {
        if (owner_enabled)
            __aw_user32_EnableWindow(owner, TRUE);
        __aw_user32_SetForegroundWindow(owner);
    }
    return result;
}

/* ---------------------------------------------------------------------- */
/* resources and creation                                                  */
/* ---------------------------------------------------------------------- */

/* the dialog template of a resource name (ordinal or UTF-16 string) */
static const uint8_t *find_template(HINSTANCE hinst, const void *name, uint32_t *size)
{
    return aw_find_resource(hinst, AW_RT_DIALOG, name, size);
}

static HWND create_from_resource(HINSTANCE hinst, const void *name, HWND owner, DLGPROC proc, LPARAM init, int modal,
                                 int *visible, int ansi)
{
    uint32_t sz = 0;
    const uint8_t *tp = find_template(hinst, name, &sz);
    if (!tp) {
        aw_set_last_error(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */);
        return NULL;
    }
    return dlg_from_template(tp, sz, owner, proc, init, modal, visible, ansi);
}

/* the ANSI variants convert a string name; ordinals pass through */
static const void *name_arg(const void *name, int ansi, WCHAR **owned)
{
    *owned = NULL;
    if (!ansi || IS_INTRESOURCE(name) || !name)
        return name;
    *owned = aw_u2w((const char *)name, -1);
    return *owned;
}

static HWND create_dialog_named(HINSTANCE hinst, const void *name, HWND owner, DLGPROC proc, LPARAM init, int ansi,
                                int modal, int *visible)
{
    WCHAR *own;
    const void *n = name_arg(name, ansi, &own);
    HWND h = create_from_resource(hinst, n, owner, proc, init, modal, visible, ansi);
    free(own);
    return h;
}

static INT_PTR box_named(HINSTANCE hinst, const void *name, HWND owner, DLGPROC proc, LPARAM init, int ansi)
{
    int visible = 0;
    WCHAR *own;
    const void *n = name_arg(name, ansi, &own);
    HWND h = create_from_resource(hinst, n, owner, proc, init, 1, &visible, ansi);
    free(own);
    if (!h)
        return -1;
    return run_modal(h, owner, visible);
}

API(user32, HWND, CreateDialogParamA, (HINSTANCE hinst, LPCSTR name, HWND owner, DLGPROC proc, LPARAM init))
{
    int visible = 0;
    HWND h = create_dialog_named(hinst, name, owner, proc, init, 1, 0, &visible);
    if (h && visible)
        __aw_user32_ShowWindow(h, SW_SHOW);
    return h;
}

API(user32, HWND, CreateDialogParamW, (HINSTANCE hinst, LPCWSTR name, HWND owner, DLGPROC proc, LPARAM init))
{
    int visible = 0;
    HWND h = create_dialog_named(hinst, name, owner, proc, init, 0, 0, &visible);
    if (h && visible)
        __aw_user32_ShowWindow(h, SW_SHOW);
    return h;
}

static HWND create_indirect(const void *tp, HWND owner, DLGPROC proc, LPARAM init, int ansi)
{
    int visible = 0;
    if (!tp)
        return NULL;
    HWND h = dlg_from_template(tp, AW_TEMPLATE_BOUND, owner, proc, init, 0, &visible, ansi);
    if (h && visible)
        __aw_user32_ShowWindow(h, SW_SHOW);
    return h;
}

API(user32, HWND, CreateDialogIndirectParamA, (HINSTANCE hinst, const void *tp, HWND owner, DLGPROC proc, LPARAM init))
{
    (void)hinst;
    return create_indirect(tp, owner, proc, init, 1);
}

API(user32, HWND, CreateDialogIndirectParamW, (HINSTANCE hinst, const void *tp, HWND owner, DLGPROC proc, LPARAM init))
{
    (void)hinst;
    return create_indirect(tp, owner, proc, init, 0);
}

API(user32, INT_PTR, DialogBoxParamA, (HINSTANCE hinst, LPCSTR name, HWND owner, DLGPROC proc, LPARAM init))
{
    return box_named(hinst, name, owner, proc, init, 1);
}

API(user32, INT_PTR, DialogBoxParamW, (HINSTANCE hinst, LPCWSTR name, HWND owner, DLGPROC proc, LPARAM init))
{
    return box_named(hinst, name, owner, proc, init, 0);
}

static INT_PTR box_indirect(const void *tp, HWND owner, DLGPROC proc, LPARAM init, int ansi)
{
    int visible = 0;
    if (!tp)
        return -1;
    HWND h = dlg_from_template(tp, AW_TEMPLATE_BOUND, owner, proc, init, 1, &visible, ansi);
    if (!h)
        return -1;
    return run_modal(h, owner, visible);
}

API(user32, INT_PTR, DialogBoxIndirectParamA, (HINSTANCE hinst, const void *tp, HWND owner, DLGPROC proc, LPARAM init))
{
    (void)hinst;
    return box_indirect(tp, owner, proc, init, 1);
}

API(user32, INT_PTR, DialogBoxIndirectParamW, (HINSTANCE hinst, const void *tp, HWND owner, DLGPROC proc, LPARAM init))
{
    (void)hinst;
    return box_indirect(tp, owner, proc, init, 0);
}

/* ---------------------------------------------------------------------- */
/* message filter                                                          */
/* ---------------------------------------------------------------------- */

static int shift_down(void) { return (__aw_user32_GetKeyState(VK_SHIFT) & 0x8000) != 0; }

/* the control whose mnemonic is ch (case-insensitive, "&x" in its text); static labels pass the
 * focus to the next tab stop, buttons are clicked */
static int mnemonic(HWND dlg, UINT ch)
{
    HWND kids[512];
    int n = children_of(dlg, kids, 512);
    UINT want = ch < 128 ? (UINT)tolower((int)ch) : ch;
    for (int i = 0; i < n; i++) {
        WCHAR text[256];
        int len = __aw_user32_GetWindowTextW(kids[i], text, 256);
        for (int j = 0; j + 1 < len; j++) {
            if (text[j] != '&' || text[j + 1] == '&')
                continue;
            UINT got = text[j + 1] < 128 ? (UINT)tolower((int)text[j + 1]) : text[j + 1];
            if (got != want)
                continue;
            if (is_button_class(kids[i])) {
                if (__aw_user32_IsWindowEnabled(kids[i]))
                    __aw_user32_SendMessageW(kids[i], AW_BM_CLICK, 0, 0);
                return 1;
            }
            HWND t = next_tab(dlg, kids[i], 0);
            if (t)
                __aw_user32_SetFocus(t);
            return 1;
        }
    }
    return 0;
}

static int is_dlg_message(HWND dlg, MSG *m)
{
    if (!dlg || !m)
        return 0;
    if (m->hwnd != dlg && !__aw_user32_IsChild(dlg, m->hwnd))
        return 0;
    switch (m->message) {
    case AW_WM_KEYDOWN:
    case AW_WM_SYSKEYDOWN: {
        UINT vk = (UINT)m->wParam;
        LRESULT dlgc = __aw_user32_SendMessageW(m->hwnd, AW_WM_GETDLGCODE, vk, (LPARAM)m);
        if (vk == VK_TAB) {
            if (dlgc & DLGC_WANTTAB)
                return 0;
            HWND from = direct_child(dlg, m->hwnd);
            HWND t = next_tab(dlg, from, shift_down());
            if (t)
                __aw_user32_SetFocus(t);
            return 1;
        }
        if (vk == VK_RETURN) {
            if (dlgc & DLGC_WANTMESSAGE)
                return 0;
            if (is_button_class(m->hwnd) && (__aw_user32_GetWindowLongW(m->hwnd, -16) & AW_BS_TYPEMASK) <= 1) {
                __aw_user32_SendMessageW(m->hwnd, AW_BM_CLICK, 0, 0);
                return 1;
            }
            dlg_state *st = st_of(dlg);
            HWND def = st && st->defid ? item_of(dlg, (int)st->defid) : NULL;
            if (def)
                __aw_user32_SendMessageW(def, AW_BM_CLICK, 0, 0);
            else
                __aw_user32_SendMessageW(dlg, AW_WM_COMMAND, IDOK, 0);
            return 1;
        }
        if (vk == VK_ESCAPE) {
            HWND cancel = item_of(dlg, IDCANCEL);
            if (cancel && is_button_class(cancel))
                __aw_user32_SendMessageW(cancel, AW_BM_CLICK, 0, 0);
            else
                __aw_user32_SendMessageW(dlg, AW_WM_COMMAND, IDCANCEL, 0);
            return 1;
        }
        if (vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN) {
            if (dlgc & DLGC_WANTARROWS)
                return 0;
            HWND focus = direct_child(dlg, m->hwnd);
            if (!is_radio(m->hwnd))
                return 0;
            int prev = vk == VK_LEFT || vk == VK_UP;
            HWND kids[512];
            int n = children_of(dlg, kids, 512), cur = -1;
            for (int i = 0; i < n; i++)
                if (kids[i] == focus)
                    cur = i;
            if (cur < 0)
                return 0;
            /* the arrows move among the radio buttons of the focus's group, which wraps around */
            int g = cur, ge = cur + 1;
            while (g > 0 && !(__aw_user32_GetWindowLongW(kids[g], -16) & WS_GROUP))
                g--;
            while (ge < n && !(__aw_user32_GetWindowLongW(kids[ge], -16) & WS_GROUP))
                ge++;
            int len = ge - g;
            for (int step = 1; step < len; step++) {
                int idx = g + (prev ? ((cur - g - step) % len + len) % len : (cur - g + step) % len);
                HWND t = kids[idx];
                if (!is_radio(t) || !__aw_user32_IsWindowVisible(t) || !__aw_user32_IsWindowEnabled(t))
                    continue;
                __aw_user32_SetFocus(t);
                if ((__aw_user32_GetWindowLongW(t, -16) & AW_BS_TYPEMASK) == AW_BS_AUTORADIOBUTTON) {
                    __aw_user32_SendMessageW(t, AW_BM_CLICK, 0, 0);
                } else {
                    for (int k = g; k < ge; k++)
                        if (kids[k] != t && is_radio(kids[k]))
                            __aw_user32_SendMessageW(kids[k], AW_BM_SETCHECK, 0, 0);
                    __aw_user32_SendMessageW(t, AW_BM_SETCHECK, BST_CHECKED, 0);
                }
                return 1;
            }
            return 1;
        }
        return 0;
    }
    case AW_WM_SYSCHAR:
        return mnemonic(dlg, (UINT)m->wParam);
    default:
        return 0;
    }
}

/* IsDialogMessage consumes what the dialog handles; a key it passes on to its controls is translated
 * here (as Windows does), so the controls get WM_CHAR without the caller's TranslateMessage */
static BOOL is_dialog_message(HWND dlg, MSG *m)
{
    int r = is_dlg_message(dlg, m);
    if (!r && m && dlg && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) &&
        (m->hwnd == dlg || __aw_user32_IsChild(dlg, m->hwnd)))
        __aw_user32_TranslateMessage(m);
    return r;
}

API(user32, BOOL, IsDialogMessageA, (HWND dlg, MSG *m)) { return is_dialog_message(dlg, m); }
API(user32, BOOL, IsDialogMessageW, (HWND dlg, MSG *m)) { return is_dialog_message(dlg, m); }

/* ---------------------------------------------------------------------- */
/* control helpers                                                         */
/* ---------------------------------------------------------------------- */

API(user32, HWND, GetDlgItem, (HWND dlg, int id))
{
    if (!dlg)
        return NULL;
    return item_of(dlg, id);
}

API(user32, LRESULT, SendDlgItemMessageA, (HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp))
{
    HWND c = item_of(dlg, id);
    return c ? __aw_user32_SendMessageA(c, msg, wp, lp) : 0;
}

API(user32, LRESULT, SendDlgItemMessageW, (HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp))
{
    HWND c = item_of(dlg, id);
    return c ? __aw_user32_SendMessageW(c, msg, wp, lp) : 0;
}

API(user32, BOOL, SetDlgItemTextA, (HWND dlg, int id, LPCSTR text))
{
    HWND c = item_of(dlg, id);
    return c ? __aw_user32_SetWindowTextA(c, text) : FALSE;
}

API(user32, BOOL, SetDlgItemTextW, (HWND dlg, int id, LPCWSTR text))
{
    HWND c = item_of(dlg, id);
    return c ? __aw_user32_SetWindowTextW(c, text) : FALSE;
}

API(user32, UINT, GetDlgItemTextA, (HWND dlg, int id, LPSTR buf, int max))
{
    HWND c = item_of(dlg, id);
    if (!c || !buf || max <= 0) {
        if (buf && max > 0)
            buf[0] = 0;
        return 0;
    }
    return (UINT)__aw_user32_GetWindowTextA(c, buf, max);
}

API(user32, UINT, GetDlgItemTextW, (HWND dlg, int id, LPWSTR buf, int max))
{
    HWND c = item_of(dlg, id);
    if (!c || !buf || max <= 0) {
        if (buf && max > 0)
            buf[0] = 0;
        return 0;
    }
    return (UINT)__aw_user32_GetWindowTextW(c, buf, max);
}

API(user32, BOOL, CheckDlgButton, (HWND dlg, int id, UINT check))
{
    HWND c = item_of(dlg, id);
    if (!c)
        return FALSE;
    __aw_user32_SendMessageW(c, AW_BM_SETCHECK, check, 0);
    return TRUE;
}

API(user32, UINT, IsDlgButtonChecked, (HWND dlg, int id))
{
    HWND c = item_of(dlg, id);
    if (!c)
        return 0;
    return (UINT)__aw_user32_SendMessageW(c, AW_BM_GETCHECK, 0, 0);
}

API(user32, BOOL, CheckRadioButton, (HWND dlg, int first, int last, int check))
{
    HWND target = NULL;
    for (int id = first; id <= last; id++) {
        HWND c = item_of(dlg, id);
        if (!c)
            continue;
        __aw_user32_SendMessageW(c, AW_BM_SETCHECK, id == check ? BST_CHECKED : 0, 0);
        if (id == check)
            target = c;
    }
    if (target)
        __aw_user32_SetFocus(target);
    return target != NULL;
}

/* the next (or previous) tab stop after ctl, wrapping around; ctl NULL starts from the first (last) */
API(user32, HWND, GetNextDlgTabItem, (HWND dlg, HWND ctl, BOOL prev))
{
    if (!dlg || !__aw_user32_IsWindow(dlg))
        return NULL;
    return next_tab(dlg, ctl ? direct_child(dlg, ctl) : NULL, prev);
}

/* the next (or previous) control of the group of ctl, wrapping around inside the group */
API(user32, HWND, GetNextDlgGroupItem, (HWND dlg, HWND ctl, BOOL prev))
{
    HWND kids[512];
    int n, cur = -1, g, ge, len;
    if (!dlg || !ctl)
        return NULL;
    n = children_of(dlg, kids, 512);
    ctl = direct_child(dlg, ctl);
    for (int i = 0; i < n; i++)
        if (kids[i] == ctl)
            cur = i;
    if (cur < 0)
        return NULL;
    g = cur;
    ge = cur + 1;
    while (g > 0 && !(__aw_user32_GetWindowLongW(kids[g], -16) & WS_GROUP))
        g--;
    while (ge < n && !(__aw_user32_GetWindowLongW(kids[ge], -16) & WS_GROUP))
        ge++;
    len = ge - g;
    return kids[g + (prev ? ((cur - g - 1) % len + len) % len : (cur - g + 1) % len)];
}

API(user32, UINT, GetDlgItemInt, (HWND dlg, int id, BOOL *ok, BOOL sign))
{
    WCHAR buf[64];
    const WCHAR *p = buf;
    long long v = 0;
    int neg = 0, any = 0;
    HWND c = item_of(dlg, id);
    if (ok)
        *ok = FALSE;
    if (!c || !__aw_user32_GetWindowTextW(c, buf, 64))
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    for (; *p >= '0' && *p <= '9'; p++, any = 1)
        v = v * 10 + (*p - '0');
    while (*p == ' ' || *p == '\t')
        p++;
    if (!any || *p)
        return 0;
    if (neg)
        v = -v;
    if (sign ? (v < INT_MIN || v > INT_MAX) : (v < 0 || v > (long long)UINT_MAX))
        return 0;
    if (ok)
        *ok = TRUE;
    return (UINT)v;
}

API(user32, BOOL, SetDlgItemInt, (HWND dlg, int id, UINT value, BOOL sign))
{
    WCHAR buf[32];
    HWND c = item_of(dlg, id);
    int i = 31;
    unsigned long long u;
    int neg = sign && (int)value < 0;
    if (!c)
        return FALSE;
    buf[i] = 0;
    u = neg ? (unsigned long long)(-(long long)(int)value) : (unsigned long long)value;
    do {
        buf[--i] = (WCHAR)('0' + u % 10);
        u /= 10;
    } while (u && i > 1);
    if (neg)
        buf[--i] = '-';
    return __aw_user32_SetWindowTextW(c, buf + i);
}

API(user32, BOOL, MapDialogRect, (HWND dlg, RECT *rc))
{
    if (!rc)
        return FALSE;
    dlg_state *st = dlg ? st_of(dlg) : NULL;
    int bx = 6, by = 13;
    if (st) {
        bx = st->base_x;
        by = st->base_y;
    }
    rc->left = mul_div(rc->left, bx, 4);
    rc->right = mul_div(rc->right, bx, 4);
    rc->top = mul_div(rc->top, by, 8);
    rc->bottom = mul_div(rc->bottom, by, 8);
    return TRUE;
}

API(user32, LONG, GetDialogBaseUnits, (void))
{
    int bx = 6, by = 13;
    base_units(gdi_default_font(), &bx, &by);
    return AW_MAKELONG(bx, by);
}
