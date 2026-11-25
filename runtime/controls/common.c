/* anywin runtime - common control helpers + built-in class registration */
#include "ctl.h"

/* window extra bytes, same for every control class */
#define CTL_OFF_STATE 0
#define CTL_OFF_FONT 8
#define CTL_OFF_NF 16
#define CTL_WNDEXTRA 24

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */
const WCHAR ctl_empty_w[1] = {0};

WCHAR *ctl_wstr(const char *utf8)
{
    if (!utf8)
        return NULL;
    return aw_u2w(utf8, -1);
}

void ctl_wbuf(const char *utf8, WCHAR *out, int n)
{
    if (n <= 0)
        return;
    out[0] = 0;
    if (!utf8)
        return;
    aw_u2w_buf(utf8, -1, out, n - 1);
    out[n - 1] = 0;
}

int ctl_wlen(const WCHAR *s)
{
    int n = 0;
    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

static WCHAR ctl_lower(WCHAR c)
{
    if (c >= 'A' && c <= 'Z')
        return (WCHAR)(c + 32);
    return c;
}

BOOL ctl_weq(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b) {
        if (ctl_lower(*a) != ctl_lower(*b))
            return FALSE;
        a++;
        b++;
    }
    return *a == *b;
}

BOOL ctl_wprefix(const WCHAR *s, const WCHAR *prefix)
{
    if (!s || !prefix)
        return FALSE;
    while (*prefix) {
        if (ctl_lower(*s) != ctl_lower(*prefix))
            return FALSE;
        s++;
        prefix++;
    }
    return TRUE;
}

WCHAR *ctl_wdup(const WCHAR *s)
{
    int n;
    WCHAR *p;
    if (!s)
        return NULL;
    n = ctl_wlen(s);
    p = malloc((size_t)(n + 1) * sizeof(WCHAR));
    if (!p)
        return NULL;
    memcpy(p, s, (size_t)(n + 1) * sizeof(WCHAR));
    return p;
}

void ctl_wfree(WCHAR *s)
{
    free(s);
}

/* ------------------------------------------------------------------ */
/* per-window state                                                    */
/* ------------------------------------------------------------------ */
void *ctl_get(HWND hwnd)
{
    return (void *)(intptr_t)__aw_user32_GetWindowLongPtrW(hwnd, CTL_OFF_STATE);
}

void *ctl_alloc(HWND hwnd, size_t size)
{
    void *p = calloc(1, size);
    if (!p)
        return NULL;
    __aw_user32_SetWindowLongPtrW(hwnd, CTL_OFF_STATE, (LONG_PTR)(intptr_t)p);
    return p;
}

void ctl_free(HWND hwnd)
{
    void *p = ctl_get(hwnd);
    free(p);
    __aw_user32_SetWindowLongPtrW(hwnd, CTL_OFF_STATE, 0);
}

static HFONT ctl_default_font(void)
{
    HGDIOBJ o = __aw_gdi32_GetStockObject(DEFAULT_GUI_FONT);
    return (HFONT)o;
}

HFONT ctl_font(HWND hwnd)
{
    HFONT f = (HFONT)(intptr_t)__aw_user32_GetWindowLongPtrW(hwnd, CTL_OFF_FONT);
    if (!f)
        f = ctl_default_font();
    return f;
}

int ctl_font_height(HWND hwnd)
{
    HDC hdc;
    TEXTMETRICW tm;
    HFONT old;
    int h = 0;
    if (!hwnd)
        return 12;
    hdc = __aw_user32_GetDC(hwnd);
    if (!hdc)
        return 12;
    old = (HFONT)__aw_gdi32_SelectObject(hdc, ctl_font(hwnd));
    if (__aw_gdi32_GetTextMetricsW(hdc, &tm))
        h = tm.tmHeight;
    __aw_gdi32_SelectObject(hdc, old);
    __aw_user32_ReleaseDC(hwnd, hdc);
    return h > 0 ? h : 12;
}

void ctl_get_text_size(HDC hdc, HFONT font, const WCHAR *text, int len, SIZE *sz)
{
    HFONT old = NULL;
    if (font)
        old = (HFONT)__aw_gdi32_SelectObject(hdc, font);
    if (len < 0)
        len = ctl_wlen(text);
    if (!__aw_gdi32_GetTextExtentPoint32W(hdc, text ? text : ctl_empty_w, len, sz)) {
        sz->cx = 0;
        sz->cy = 0;
    }
    if (font && old)
        __aw_gdi32_SelectObject(hdc, old);
}

int ctl_text_width(HDC hdc, const WCHAR *text, int len)
{
    SIZE sz;
    ctl_get_text_size(hdc, NULL, text, len, &sz);
    return sz.cx;
}

/* ------------------------------------------------------------------ */
/* notifications                                                       */
/* ------------------------------------------------------------------ */
LRESULT ctl_send_parent(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND parent = __aw_user32_GetParent(hwnd);
    if (!parent)
        return 0;
    return __aw_user32_SendMessageW(parent, msg, wp, lp);
}

HBRUSH ctl_ctlcolor(HWND hwnd, UINT msg, HDC hdc)
{
    HWND parent = __aw_user32_GetParent(hwnd);
    if (!parent)
        return NULL;
    return (HBRUSH)(intptr_t)__aw_user32_SendMessageW(parent, msg, (WPARAM)hdc, (LPARAM)hwnd);
}

void ctl_command(HWND hwnd, UINT code)
{
    HWND parent = __aw_user32_GetParent(hwnd);
    if (!parent)
        return;
    __aw_user32_SendMessageW(parent, WM_COMMAND,
                             MAKEWPARAM(__aw_user32_GetDlgCtrlID(hwnd), code & 0xFFFF), (LPARAM)hwnd);
}

void ctl_notify_simple(HWND hwnd, UINT code)
{
    NMHDR n;
    HWND parent = __aw_user32_GetParent(hwnd);
    if (!parent)
        return;
    n.hwndFrom = hwnd;
    n.idFrom = (UINT_PTR)__aw_user32_GetDlgCtrlID(hwnd);
    n.code = code;
    __aw_user32_SendMessageW(parent, WM_NOTIFY, (WPARAM)n.idFrom, (LPARAM)&n);
}

int ctl_get_notify_unicode(HWND hwnd)
{
    return (int)(intptr_t)__aw_user32_GetWindowLongPtrW(hwnd, CTL_OFF_NF) - 1;
}

void ctl_set_notify_unicode(HWND hwnd, int on)
{
    __aw_user32_SetWindowLongPtrW(hwnd, CTL_OFF_NF, on ? 2 : 1);
}

BOOL ctl_is_unicode_notify(HWND hwnd)
{
    LONG_PTR v = __aw_user32_GetWindowLongPtrW(hwnd, CTL_OFF_NF);
    HWND parent;
    if (v == 1)
        return FALSE;
    if (v == 2)
        return TRUE;
    parent = __aw_user32_GetParent(hwnd);
    if (parent) {
        LRESULT r = __aw_user32_SendMessageW(parent, WM_NOTIFYFORMAT, (WPARAM)hwnd, NF_QUERY);
        v = (r == NFR_UNICODE) ? 2 : 1;
        __aw_user32_SetWindowLongPtrW(hwnd, CTL_OFF_NF, v);
        return v == 2;
    }
    return TRUE;
}

LRESULT ctl_notifyformat_msg(HWND hwnd, LPARAM lp)
{
    if (lp == NF_REQUERY) {
        __aw_user32_SetWindowLongPtrW(hwnd, CTL_OFF_NF, 0);
        return ctl_is_unicode_notify(hwnd) ? NFR_UNICODE : NFR_ANSI;
    }
    return ctl_is_unicode_notify(hwnd) ? NFR_UNICODE : NFR_ANSI;
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */
static int ctl_is_color_ref(HBRUSH br)
{
    /* user32 passes (HBRUSH)(COLOR_xxx+1) as a class background; controls
     * sometimes get those through WM_CTLCOLOR as well. */
    return (uintptr_t)br <= (uintptr_t)COLOR_INFOBK + 1;
}

void ctl_fill(HDC hdc, const RECT *rc, HBRUSH br)
{
    if (!br)
        return;
    if (ctl_is_color_ref(br)) {
        int idx = (int)(uintptr_t)br - 1;
        HBRUSH b = __aw_user32_GetSysColorBrush(idx);
        if (b)
            __aw_user32_FillRect(hdc, rc, b);
        return;
    }
    __aw_user32_FillRect(hdc, rc, br);
}

int ctl_begin(HWND hwnd, UINT ctlcolor, HDC *hdc, PAINTSTRUCT *ps, RECT *rc, HBRUSH *bg)
{
    *hdc = __aw_user32_BeginPaint(hwnd, ps);
    __aw_user32_GetClientRect(hwnd, rc);
    *bg = ctl_ctlcolor(hwnd, ctlcolor, *hdc);
    if (*bg)
        ctl_fill(*hdc, rc, *bg);
    return *hdc != NULL;
}

void ctl_end(HWND hwnd, PAINTSTRUCT *ps)
{
    __aw_user32_EndPaint(hwnd, ps);
}

void ctl_frame(HDC hdc, const RECT *rc, COLORREF color)
{
    HBRUSH br = __aw_gdi32_CreateSolidBrush(color);
    RECT r = *rc;
    if (!br)
        return;
    r.bottom = r.top + 1;
    __aw_user32_FillRect(hdc, &r, br);
    r = *rc;
    r.top = r.bottom - 1;
    __aw_user32_FillRect(hdc, &r, br);
    r = *rc;
    r.right = r.left + 1;
    __aw_user32_FillRect(hdc, &r, br);
    r = *rc;
    r.left = r.right - 1;
    __aw_user32_FillRect(hdc, &r, br);
    __aw_gdi32_DeleteObject(br);
}

void ctl_hline(HDC hdc, int x1, int x2, int y, COLORREF color)
{
    RECT r;
    HBRUSH br;
    if (x2 <= x1)
        return;
    br = __aw_gdi32_CreateSolidBrush(color);
    if (!br)
        return;
    r.left = x1;
    r.right = x2;
    r.top = y;
    r.bottom = y + 1;
    __aw_user32_FillRect(hdc, &r, br);
    __aw_gdi32_DeleteObject(br);
}

void ctl_vline(HDC hdc, int x, int y1, int y2, COLORREF color)
{
    RECT r;
    HBRUSH br;
    if (y2 <= y1)
        return;
    br = __aw_gdi32_CreateSolidBrush(color);
    if (!br)
        return;
    r.left = x;
    r.right = x + 1;
    r.top = y1;
    r.bottom = y2;
    __aw_user32_FillRect(hdc, &r, br);
    __aw_gdi32_DeleteObject(br);
}

void ctl_draw_focus(HDC hdc, const RECT *rc)
{
    __aw_user32_DrawFocusRect(hdc, rc);
}

void ctl_text(HDC hdc, HFONT f, const WCHAR *s, RECT rc, UINT fmt, COLORREF color)
{
    HFONT old = NULL;
    if (f)
        old = (HFONT)__aw_gdi32_SelectObject(hdc, f);
    __aw_gdi32_SetTextColor(hdc, color);
    __aw_gdi32_SetBkMode(hdc, 1 /* TRANSPARENT */);
    __aw_user32_DrawTextW(hdc, s ? s : ctl_empty_w, -1, &rc, fmt);
    if (f && old)
        __aw_gdi32_SelectObject(hdc, old);
}

SIZE ctl_measure(HWND hwnd, HFONT f, const WCHAR *s, int len)
{
    SIZE sz = {0, 0};
    HDC hdc = __aw_user32_GetDC(hwnd);
    if (hdc) {
        ctl_get_text_size(hdc, f ? f : ctl_font(hwnd), s, len, &sz);
        __aw_user32_ReleaseDC(hwnd, hdc);
    }
    return sz;
}

/* ------------------------------------------------------------------ */
/* common messages                                                     */
/* ------------------------------------------------------------------ */
int ctl_common_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res)
{
    switch (msg) {
    case WM_SETFONT:
        __aw_user32_SetWindowLongPtrW(hwnd, CTL_OFF_FONT, (LONG_PTR)wp);
        if (lp)
            __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        *res = 0;
        return 1;
    case WM_GETFONT:
        *res = (LRESULT)(LONG_PTR)__aw_user32_GetWindowLongPtrW(hwnd, CTL_OFF_FONT);
        return 1;
    case WM_NOTIFYFORMAT:
        *res = ctl_notifyformat_msg(hwnd, lp);
        return 1;
    case WM_SYSCOLORCHANGE:
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        *res = 0;
        return 1;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* class registration                                                  */
/* ------------------------------------------------------------------ */
static void reg_class(const char *name, WNDPROC proc, UINT style, int dblclk)
{
    WNDCLASSEXW wc;
    WCHAR *wn = ctl_wstr(name);
    if (!wn)
        return;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = style | CS_GLOBALCLASS | (dblclk ? CS_DBLCLKS : 0);
    wc.lpfnWndProc = proc;
    wc.cbWndExtra = CTL_WNDEXTRA;
    wc.hInstance = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszClassName = wn;
    __aw_user32_RegisterClassExW(&wc);
}

void aw_controls_register(void)
{
    static int done;
    if (done)
        return;
    done = 1;

    reg_class("Button", aw_btn_proc, CS_PARENTDC | CS_VREDRAW | CS_HREDRAW, 1);
    reg_class("Static", aw_static_proc, CS_PARENTDC, 1);
    reg_class("Edit", aw_edit_proc, CS_PARENTDC, 1);
    reg_class("ListBox", aw_listbox_proc, CS_PARENTDC, 1);
    reg_class("ComboBox", aw_combo_proc, CS_PARENTDC, 1);
    reg_class("ComboLBox", aw_combolbox_proc, 0, 1);
    reg_class("ScrollBar", aw_scrollbar_proc, CS_PARENTDC | CS_VREDRAW | CS_HREDRAW, 1);
    reg_class("SysListView32", aw_listview_proc, 0, 1);
    reg_class("SysHeader32", aw_header_proc, 0, 1);
    reg_class("msctls_progress32", aw_progress_proc, 0, 0);
    reg_class("msctls_trackbar32", aw_trackbar_proc, 0, 1);
    reg_class("msctls_statusbar32", aw_statusbar_proc, 0, 1);
    reg_class("msctls_updown32", aw_updown_proc, 0, 1);
    reg_class("SysTabControl32", aw_tab_proc, 0, 1);
    reg_class("tooltips_class32", aw_tooltip_proc, 0, 0);
    reg_class("SysTreeView32", aw_treeview_proc, 0, 1);
    reg_class("ToolbarWindow32", aw_toolbar_proc, 0, 1);
    reg_class("ReBarWindow32", aw_rebar_proc, 0, 1);
    reg_class("SysLink", aw_syslink_proc, 0, 1);
    reg_class("SysDateTimePick32", aw_dtp_proc, 0, 1);
    reg_class("SysMonthCal32", aw_monthcal_proc, 0, 1);
    reg_class("RichEdit20A", aw_richedit_proc, 0, 1);
    reg_class("RichEdit20W", aw_richedit_proc, 0, 1);
    reg_class("RICHEDIT50W", aw_richedit_proc, 0, 1);
    reg_class("RichEdit", aw_richedit_proc, 0, 1);
    reg_class("msctls_hotkey32", aw_hotkey_proc, 0, 1);
}
