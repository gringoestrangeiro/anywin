/* anywin runtime - Static control (PLAN-CORE 3.6, PLAN 6-H).
 * Text (SS_LEFT/CENTER/RIGHT, SS_SIMPLE, SS_LEFTNOWORDWRAP, ellipsis, SS_NOPREFIX, SS_CENTERIMAGE),
 * flat rectangles/frames/etched lines, SS_ICON (STM_SETICON/GETICON), SS_OWNERDRAW (WM_DRAWITEM),
 * SS_NOTIFY (STN_*). The background comes from WM_CTLCOLORSTATIC (ctl_begin fills with the brush
 * the parent returns). SS_BITMAP/SS_ENHMETAFILE are accepted and stored but not painted. */
#include "ctl.h"

#ifndef DI_NORMAL
#define DI_NORMAL 0x0003
#endif
#ifndef IMAGE_BITMAP
#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#endif
#define ST_SM_CXICON 11
#define ST_SM_CYICON 12
#ifndef SS_ENHMETAFILE
#define SS_ENHMETAFILE 0x0000000F
#endif

/* same layout as CREATESTRUCTW (x64) */
typedef struct {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    LPCWSTR lpszName;
    LPCWSTR lpszClass;
    DWORD dwExStyle;
} st_createw;

typedef struct {
    HICON icon;
    HGDIOBJ image;
} st_t;

static void st_draw_text(HWND hwnd, HDC hdc, const RECT *rc, UINT style)
{
    UINT type = style & SS_TYPEMASK;
    UINT ell = style & SS_ELLIPSISMASK;
    int n = __aw_user32_GetWindowTextLengthW(hwnd);
    WCHAR *text;
    UINT fmt;
    int single;
    RECT r = *rc;
    HFONT old;

    if (n < 0)
        n = 0;
    text = malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!text)
        return;
    __aw_user32_GetWindowTextW(hwnd, text, n + 1);

    fmt = DT_LEFT;
    if (type == SS_CENTER)
        fmt = DT_CENTER;
    else if (type == SS_RIGHT)
        fmt = DT_RIGHT;

    single = (type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP || (ell && ell != SS_WORDELLIPSIS));
    fmt |= single ? DT_SINGLELINE : DT_WORDBREAK;
    if (style & SS_CENTERIMAGE)
        fmt |= DT_SINGLELINE | DT_VCENTER;
    if (style & SS_NOPREFIX)
        fmt |= DT_NOPREFIX;
    if (ell == SS_ENDELLIPSIS)
        fmt |= DT_END_ELLIPSIS;
    else if (ell == SS_PATHELLIPSIS)
        fmt |= DT_PATH_ELLIPSIS;
    else if (ell == SS_WORDELLIPSIS)
        fmt |= DT_WORD_ELLIPSIS;

    old = (HFONT)__aw_gdi32_SelectObject(hdc, ctl_font(hwnd));
    __aw_gdi32_SetBkMode(hdc, 1 /* TRANSPARENT */);
    __aw_user32_DrawTextW(hdc, text, -1, &r, fmt);
    __aw_gdi32_SelectObject(hdc, old);
    free(text);
}

static void st_draw_icon(HDC hdc, const RECT *rc, HICON icon, UINT style)
{
    int cw = rc->right - rc->left;
    int ch = rc->bottom - rc->top;
    int x = rc->left, y = rc->top, w = cw, h = ch;

    if (!icon)
        return;
    if (style & SS_REALSIZEIMAGE) { /* native size; SS_CENTERIMAGE centres it */
        w = __aw_user32_GetSystemMetrics(ST_SM_CXICON);
        h = __aw_user32_GetSystemMetrics(ST_SM_CYICON);
        if (style & SS_CENTERIMAGE) {
            x += (cw - w) / 2;
            y += (ch - h) / 2;
        }
    }
    __aw_user32_DrawIconEx(hdc, x, y, icon, w, h, 0, NULL, DI_NORMAL);
}

static void st_draw_owner(HWND hwnd, HDC hdc, const RECT *rc)
{
    DRAWITEMSTRUCT dis;
    memset(&dis, 0, sizeof(dis));
    dis.CtlType = ODT_STATIC;
    dis.CtlID = (UINT)__aw_user32_GetDlgCtrlID(hwnd);
    dis.itemAction = ODA_DRAWENTIRE;
    dis.hwndItem = hwnd;
    dis.hDC = hdc;
    dis.rcItem = *rc;
    ctl_send_parent(hwnd, WM_DRAWITEM, dis.CtlID, (LPARAM)&dis);
}

static void st_draw(HWND hwnd, st_t *s, HDC hdc, const RECT *rc, UINT style)
{
    UINT type = style & SS_TYPEMASK;
    COLORREF shadow = aw_syscolor(COLOR_BTNSHADOW);
    COLORREF light = aw_syscolor(COLOR_BTNHIGHLIGHT);
    COLORREF col;
    HBRUSH br;

    switch (type) {
    case SS_BLACKRECT:
    case SS_GRAYRECT:
    case SS_WHITERECT:
        col = type == SS_BLACKRECT ? RGB(0, 0, 0) : type == SS_GRAYRECT ? shadow : RGB(255, 255, 255);
        br = __aw_gdi32_CreateSolidBrush(col);
        if (br) {
            __aw_user32_FillRect(hdc, rc, br);
            __aw_gdi32_DeleteObject(br);
        }
        break;
    case SS_BLACKFRAME:
    case SS_GRAYFRAME:
    case SS_WHITEFRAME:
        col = type == SS_BLACKFRAME ? RGB(0, 0, 0) : type == SS_GRAYFRAME ? shadow : RGB(255, 255, 255);
        ctl_frame(hdc, rc, col);
        break;
    case SS_ETCHEDHORZ: {
        int y = (rc->top + rc->bottom) / 2 - 1;
        ctl_hline(hdc, rc->left, rc->right, y, shadow);
        ctl_hline(hdc, rc->left, rc->right, y + 1, light);
        break;
    }
    case SS_ETCHEDVERT: {
        int x = (rc->left + rc->right) / 2 - 1;
        ctl_vline(hdc, x, rc->top, rc->bottom, shadow);
        ctl_vline(hdc, x + 1, rc->top, rc->bottom, light);
        break;
    }
    case SS_ETCHEDFRAME:
        ctl_frame(hdc, rc, shadow);
        break;
    case SS_ICON:
        st_draw_icon(hdc, rc, s ? s->icon : NULL, style);
        break;
    case SS_OWNERDRAW:
        st_draw_owner(hwnd, hdc, rc);
        break;
    case SS_BITMAP:
    case SS_ENHMETAFILE:
    case SS_USERITEM:
        break;
    default:
        st_draw_text(hwnd, hdc, rc, style);
        break;
    }
    if (style & SS_SUNKEN)
        ctl_frame(hdc, rc, shadow);
}

static void st_paint(HWND hwnd, st_t *s)
{
    HDC hdc;
    PAINTSTRUCT ps;
    RECT rc;
    HBRUSH bg;

    if (!ctl_begin(hwnd, WM_CTLCOLORSTATIC, &hdc, &ps, &rc, &bg))
        return;
    st_draw(hwnd, s, hdc, &rc, (UINT)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE));
    ctl_end(hwnd, &ps);
}

static LRESULT st_msg(HWND hwnd, st_t *s, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    UINT style = (UINT)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);

    *handled = 1;
    switch (msg) {
    case WM_PAINT:
        st_paint(hwnd, s);
        return 0;
    case WM_PRINTCLIENT: {
        RECT rc;
        HDC hdc = (HDC)(uintptr_t)wp;
        HBRUSH bg = (HBRUSH)ctl_ctlcolor(hwnd, WM_CTLCOLORSTATIC, hdc);
        __aw_user32_GetClientRect(hwnd, &rc);
        if (bg)
            ctl_fill(hdc, &rc, bg);
        st_draw(hwnd, s, hdc, &rc, style);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (style & SS_NOTIFY)
            ctl_command(hwnd, STN_CLICKED);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (style & SS_NOTIFY)
            ctl_command(hwnd, STN_DBLCLK);
        return 0;
    case WM_ENABLE:
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        if (style & SS_NOTIFY)
            ctl_command(hwnd, wp ? STN_ENABLE : STN_DISABLE);
        return 0;
    case WM_SETTEXT: {
        LRESULT r = __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return r;
    }
    case WM_SIZE:
    case WM_SETFONT:
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_STATIC;
    case STM_SETICON: {
        HICON old = s->icon;
        s->icon = (HICON)(uintptr_t)wp;
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return (LRESULT)(uintptr_t)old;
    }
    case STM_GETICON:
        return (LRESULT)(uintptr_t)s->icon;
    case STM_SETIMAGE: {
        HGDIOBJ old;
        if (wp == IMAGE_ICON) {
            old = s->icon;
            s->icon = (HICON)(uintptr_t)lp;
        } else {
            old = s->image;
            s->image = (HGDIOBJ)(uintptr_t)lp;
        }
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return (LRESULT)(uintptr_t)old;
    }
    case STM_GETIMAGE:
        if (wp == IMAGE_ICON)
            return (LRESULT)(uintptr_t)s->icon;
        return (LRESULT)(uintptr_t)s->image;
    default:
        break;
    }
    *handled = 0;
    return 0;
}

LRESULT WINAPI aw_static_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    st_t *s = ctl_get(hwnd);
    LRESULT res;
    int handled = 0;

    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;

    if (msg == WM_CREATE) {
        st_createw *cs = (st_createw *)(uintptr_t)lp;
        UINT style = (UINT)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
        s = ctl_alloc(hwnd, sizeof(st_t));
        if (!s)
            return -1;
        /* SS_ICON with a text: the text names an icon in the creating module */
        if ((style & SS_TYPEMASK) == SS_ICON && cs && cs->lpszName && cs->lpszName[0])
            s->icon = (HICON)__aw_user32_LoadImageW(cs->hInstance, cs->lpszName, IMAGE_ICON, 0, 0, 0);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        ctl_free(hwnd);
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!s)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);

    res = st_msg(hwnd, s, msg, wp, lp, &handled);
    if (handled)
        return res;
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}
