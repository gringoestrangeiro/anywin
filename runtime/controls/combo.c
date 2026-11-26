/* anywin runtime - ComboBox and its internal drop list ComboLBox (PLAN-CORE 3.6, PLAN 6-H).
 * CBS_DROPDOWN (a child Edit holds the text), CBS_DROPDOWNLIST (text is drawn by the combo).
 * CBS_SIMPLE is accepted with the child Edit but without a list (documented gap).
 * The drop list is a WS_POPUP ComboLBox window owned by the combo: it takes the mouse capture while
 * open, so a click outside it cancels, and a click on a row (or Enter) commits. The keyboard stays with
 * the combo (or its Edit, which forwards Up/Down/F4/Enter/Esc). Notifications are CBN_* in WM_COMMAND. */
#include "ctl.h"

#define CB_EDIT_ID 1001
#define CB_MSG_SETCOMBO (WM_USER + 0x1F00) /* private: marks the edit child (value shared with edit.c) */
#define CB_ARROW_W 17
#define CB_DEF_VISIBLE 30
#define CB_SM_CXSCREEN 0
#define CB_SM_CYSCREEN 1
#define CB_MA_NOACTIVATE 3

#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215
#endif
#ifndef ETO_CLIPPED
#define ETO_CLIPPED 0x0004
#endif
#ifndef CB_SETMINVISIBLE
#define CB_SETMINVISIBLE 0x1701
#define CB_GETMINVISIBLE 0x1702
#endif
#ifndef CB_SETCUEBANNER
#define CB_SETCUEBANNER 0x1703
#define CB_GETCUEBANNER 0x1704
#endif
#define CB_EM_GETCUEBANNER 0x1502

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
} cb_createw;

typedef struct {
    WCHAR *text;
    LPARAM data;
} cb_item;

typedef struct {
    cb_item *items;
    int count, cap;
    int cursel;
    int kind;          /* CBS_SIMPLE / CBS_DROPDOWN / CBS_DROPDOWNLIST */
    int sorted;
    int style;
    HWND edit;         /* DROPDOWN / SIMPLE child */
    HWND list;         /* ComboLBox while dropped */
    HWND popup;        /* the ComboLBox window: created once, shown and hidden */
    int dropped;
    int hot, top, rows;
    int orig_sel;      /* selection when the list opened (restored on cancel) */
    int item_h;        /* 0 = font height + 2 */
    int drop_w;        /* 0 = combo width */
    int min_visible;
    int ext_ui, hext;
    int focused;
    int suppress;      /* >0 while the combo itself changes the edit text */
} cb_t;

typedef struct {
    HWND owner;
    int armed;         /* a button press reached the list: a release outside it cancels */
} lb_t;

static const WCHAR cb_cls_lbox[] = {'C', 'o', 'm', 'b', 'o', 'L', 'B', 'o', 'x', 0};
static const WCHAR cb_cls_edit[] = {'E', 'd', 'i', 't', 0};

/* ------------------------------------------------------------------ geometry */

static int cb_closed_h(HWND hwnd)
{
    return ctl_font_height(hwnd) + 8;
}

static int cb_itemh(HWND hwnd, const cb_t *c)
{
    return c->item_h > 0 ? c->item_h : ctl_font_height(hwnd) + 2;
}

static void cb_layout(HWND hwnd, cb_t *c)
{
    RECT rc;
    int w, closed;
    if (!c->edit)
        return;
    __aw_user32_GetClientRect(hwnd, &rc);
    w = rc.right - rc.left - CB_ARROW_W - 2;
    if (w < 0)
        w = 0;
    closed = cb_closed_h(hwnd);
    __aw_user32_MoveWindow(c->edit, 1, 1, w, closed - 2 > 0 ? closed - 2 : 0, TRUE);
}

/* the drop list rectangle in screen coordinates and its visible row count */
static void cb_geom(HWND hwnd, const cb_t *c, RECT *scr, int *rows_out)
{
    RECT cr;
    POINT org;
    int closed = cb_closed_h(hwnd), ih = cb_itemh(hwnd, c), ch, cw, rows, lh, lw, sh;

    __aw_user32_GetClientRect(hwnd, &cr);
    cw = cr.right - cr.left;
    ch = cr.bottom - cr.top;
    rows = c->count < c->min_visible ? c->count : c->min_visible;
    if (ch > closed) { /* the combo's own height is the dropped height */
        int fit = (ch - closed) / ih;
        if (fit >= 1)
            rows = c->count < fit ? c->count : fit;
    }
    if (rows < 1)
        rows = 1;
    lh = rows * ih + 2;
    lw = cw;
    if (c->drop_w > lw)
        lw = c->drop_w;
    org.x = 0;
    org.y = 0;
    __aw_user32_ClientToScreen(hwnd, &org);
    scr->left = org.x;
    scr->top = org.y + closed;
    scr->right = scr->left + lw;
    scr->bottom = scr->top + lh;
    sh = __aw_user32_GetSystemMetrics(CB_SM_CYSCREEN);
    if (sh > 0 && scr->bottom > sh && org.y - lh >= 0) {
        scr->top = org.y - lh;
        scr->bottom = org.y;
    }
    if (rows_out)
        *rows_out = rows;
}

static void cb_show_hot(cb_t *c)
{
    int max_top;
    if (c->hot < 0)
        return;
    if (c->hot < c->top)
        c->top = c->hot;
    if (c->rows > 0 && c->hot >= c->top + c->rows)
        c->top = c->hot - c->rows + 1;
    max_top = c->count - c->rows;
    if (max_top < 0)
        max_top = 0;
    if (c->top > max_top)
        c->top = max_top;
    if (c->top < 0)
        c->top = 0;
}

static void cb_inval_list(cb_t *c)
{
    if (c->list)
        __aw_user32_InvalidateRect(c->list, NULL, FALSE);
}

/* ------------------------------------------------------------------ items */

static int cb_reserve(cb_t *c, int n)
{
    int nc;
    cb_item *p;
    if (n <= c->cap)
        return 1;
    nc = c->cap > 0 ? c->cap : 8;
    while (nc < n)
        nc *= 2;
    p = realloc(c->items, (size_t)nc * sizeof(cb_item));
    if (!p)
        return 0;
    c->items = p;
    c->cap = nc;
    return 1;
}

static int cb_cmpi(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (WCHAR)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (WCHAR)(y + 32);
        if (x != y)
            return x < y ? -1 : 1;
        a++;
        b++;
    }
    if (*a == *b)
        return 0;
    return *a ? 1 : -1;
}

static void cb_set_edit_text(cb_t *c, const WCHAR *s)
{
    if (!c->edit)
        return;
    c->suppress++;
    __aw_user32_SetWindowTextW(c->edit, s ? s : ctl_empty_w);
    c->suppress--;
}

static int cb_insert_at(cb_t *c, int at, const WCHAR *s)
{
    cb_item it;
    if (!cb_reserve(c, c->count + 1))
        return CB_ERRSPACE;
    it.text = ctl_wdup(s ? s : ctl_empty_w);
    if (!it.text)
        return CB_ERRSPACE;
    it.data = 0;
    memmove(&c->items[at + 1], &c->items[at], (size_t)(c->count - at) * sizeof(cb_item));
    c->items[at] = it;
    c->count++;
    if (c->cursel >= at)
        c->cursel++;
    return at;
}

static void cb_remove_at(cb_t *c, int at)
{
    free(c->items[at].text);
    memmove(&c->items[at], &c->items[at + 1], (size_t)(c->count - at - 1) * sizeof(cb_item));
    c->count--;
}

static void cb_clear(cb_t *c)
{
    int i;
    for (i = 0; i < c->count; i++)
        free(c->items[i].text);
    c->count = 0;
    c->cursel = -1;
    c->top = 0;
    c->hot = -1;
}

static int cb_find(const cb_t *c, int start, const WCHAR *s, int exact)
{
    int k, i, n = c->count, from = (start < -1 || start >= n) ? -1 : start;
    if (!s || n == 0)
        return -1;
    for (k = 1; k <= n; k++) {
        i = (from + k) % n;
        if (exact ? ctl_weq(c->items[i].text, s) : ctl_wprefix(c->items[i].text, s))
            return i;
    }
    return -1;
}

/* sets the selection without notification; DROPDOWN shows the item text in the edit */
static void cb_set_cursel(HWND hwnd, cb_t *c, int idx)
{
    if (idx < 0 || idx >= c->count) {
        c->cursel = -1;
        cb_set_edit_text(c, NULL);
    } else {
        c->cursel = idx;
        cb_set_edit_text(c, c->items[idx].text);
    }
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
    cb_inval_list(c);
}

/* a user selection: CBN_SELCHANGE only when the selection really changes */
static void cb_user_select(HWND hwnd, cb_t *c, int idx)
{
    if (idx < 0 || idx >= c->count || idx == c->cursel)
        return;
    cb_set_cursel(hwnd, c, idx);
    ctl_command(hwnd, CBN_SELCHANGE);
}

/* keyboard/type-ahead movement: the highlight follows when the list is open */
static void cb_step(HWND hwnd, cb_t *c, int idx)
{
    if (c->count == 0)
        return;
    if (idx < 0)
        idx = 0;
    if (idx >= c->count)
        idx = c->count - 1;
    if (c->dropped) {
        c->hot = idx;
        cb_show_hot(c);
        cb_inval_list(c);
    }
    cb_user_select(hwnd, c, idx);
}

static void cb_key_move(HWND hwnd, cb_t *c, int delta)
{
    int base = (c->dropped && c->hot >= 0) ? c->hot : c->cursel;
    int idx;
    if (c->count == 0)
        return;
    idx = base < 0 ? (delta > 0 ? 0 : c->count - 1) : base + delta;
    cb_step(hwnd, c, idx);
}

/* ------------------------------------------------------------------ drop list */

static void cb_open(HWND hwnd, cb_t *c)
{
    RECT r;
    int rows = 1;
    HWND lb;

    if (c->dropped || c->kind == CBS_SIMPLE)
        return;
    ctl_command(hwnd, CBN_DROPDOWN);
    if (c->dropped)
        return;
    cb_geom(hwnd, c, &r, &rows);
    lb = c->popup;
    if (!lb) {
        lb = __aw_user32_CreateWindowExW(0, cb_cls_lbox, NULL, WS_POPUP | WS_BORDER, r.left, r.top,
                                         r.right - r.left, r.bottom - r.top, NULL, NULL, NULL, hwnd);
        if (!lb)
            return;
        c->popup = lb;
    } else {
        __aw_user32_MoveWindow(lb, r.left, r.top, r.right - r.left, r.bottom - r.top, FALSE);
    }
    c->list = lb;
    if (ctl_get(lb))
        ((lb_t *)ctl_get(lb))->armed = 0;
    c->dropped = 1;
    c->orig_sel = c->cursel;
    c->rows = rows;
    c->hot = c->cursel >= 0 ? c->cursel : (c->count > 0 ? 0 : -1);
    cb_show_hot(c);
    __aw_user32_ShowWindow(lb, SW_SHOWNA);
    __aw_user32_SetCapture(lb);
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
}

/* commit: select the highlighted row (SELCHANGE if changed, SELENDOK); otherwise cancel (restores
 * the selection from when the list opened, SELENDCANCEL). Always CLOSEUP last. */
static void cb_close(HWND hwnd, cb_t *c, int commit)
{
    HWND lb;
    if (!c->dropped)
        return;
    c->dropped = 0;
    lb = c->list;
    c->list = NULL;
    if (lb) {
        int had_focus = __aw_user32_GetFocus() == lb;
        __aw_user32_ShowWindow(lb, SW_HIDE);
        if (__aw_user32_GetCapture() == lb)
            __aw_user32_ReleaseCapture();
        if (had_focus)
            __aw_user32_SetFocus(c->edit ? c->edit : hwnd);
    }
    if (commit && c->hot >= 0 && c->hot < c->count) {
        cb_user_select(hwnd, c, c->hot);
        ctl_command(hwnd, CBN_SELENDOK);
    } else {
        if (c->cursel != c->orig_sel)
            cb_set_cursel(hwnd, c, c->orig_sel);
        ctl_command(hwnd, CBN_SELENDCANCEL);
    }
    ctl_command(hwnd, CBN_CLOSEUP);
}

/* ------------------------------------------------------------------ painting */

static void cb_draw_arrow(HDC hdc, const RECT *a, int enabled)
{
    int cx = (a->left + a->right) / 2, cy = (a->top + a->bottom) / 2, i;
    HBRUSH br = __aw_gdi32_CreateSolidBrush(enabled ? RGB(0x00, 0x00, 0x00) : aw_syscolor(COLOR_GRAYTEXT));
    if (!br)
        return;
    for (i = 0; i < 4; i++) {
        RECT r;
        r.left = cx - (3 - i);
        r.right = cx + (3 - i) + 1;
        r.top = cy - 2 + i;
        r.bottom = r.top + 1;
        __aw_user32_FillRect(hdc, &r, br);
    }
    __aw_gdi32_DeleteObject(br);
}

static void cb_draw_text(HWND hwnd, cb_t *c, HDC hdc, const RECT *box, int enabled)
{
    RECT tr;
    HFONT old;
    TEXTMETRICW tm;
    int th = 12;
    COLORREF tc;

    tr.left = box->left + 3;
    tr.right = box->right - CB_ARROW_W - 2;
    tr.top = box->top + 1;
    tr.bottom = box->bottom - 1;
    if (tr.right <= tr.left)
        return;
    old = (HFONT)__aw_gdi32_SelectObject(hdc, ctl_font(hwnd));
    if (__aw_gdi32_GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0)
        th = tm.tmHeight;
    if (c->cursel >= 0 && c->cursel < c->count) {
        const WCHAR *t = c->items[c->cursel].text;
        int n = ctl_wlen(t);
        tc = (COLORREF)__aw_gdi32_GetTextColor(hdc);
        if (!enabled)
            tc = aw_syscolor(COLOR_GRAYTEXT);
        __aw_gdi32_SetBkMode(hdc, 1);
        if (c->focused) {
            __aw_user32_FillRect(hdc, &tr, __aw_user32_GetSysColorBrush(COLOR_HIGHLIGHT));
            tc = aw_syscolor(COLOR_HIGHLIGHTTEXT);
        }
        __aw_gdi32_SetTextColor(hdc, tc);
        __aw_gdi32_ExtTextOutW(hdc, tr.left, tr.top + (tr.bottom - tr.top - th) / 2, ETO_CLIPPED, &tr, t, n, NULL);
    }
    __aw_gdi32_SelectObject(hdc, old);
}

static void cb_paint(HWND hwnd, cb_t *c)
{
    HDC hdc;
    PAINTSTRUCT ps;
    RECT rc, box, arrow;
    HBRUSH bg;
    int closed = cb_closed_h(hwnd);
    int enabled = __aw_user32_IsWindowEnabled(hwnd);

    if (!ctl_begin(hwnd, WM_CTLCOLORLISTBOX, &hdc, &ps, &rc, &bg))
        return;
    if (!bg)
        ctl_fill(hdc, &rc, __aw_user32_GetSysColorBrush(COLOR_WINDOW));
    box = rc;
    if (box.bottom > closed)
        box.bottom = closed;
    if (c->kind == CBS_DROPDOWNLIST)
        cb_draw_text(hwnd, c, hdc, &box, enabled);
    ctl_frame(hdc, &box, c->focused ? RGB(0x00, 0x78, 0xD7) : RGB(0xAD, 0xAD, 0xAD));
    arrow.left = box.right - CB_ARROW_W;
    arrow.right = box.right - 1;
    arrow.top = box.top + 1;
    arrow.bottom = box.bottom - 1;
    ctl_fill(hdc, &arrow, __aw_user32_GetSysColorBrush(COLOR_BTNFACE));
    cb_draw_arrow(hdc, &arrow, enabled);
    ctl_end(hwnd, &ps);
}

/* ------------------------------------------------------------------ keyboard / mouse */

static LRESULT cb_keydown(HWND hwnd, cb_t *c, WPARAM vk)
{
    int alt = (__aw_user32_GetKeyState(VK_MENU) & 0x8000) != 0;
    switch (vk) {
    case VK_F4:
        if (c->dropped)
            cb_close(hwnd, c, 1);
        else
            cb_open(hwnd, c);
        break;
    case VK_DOWN:
        if (alt) {
            if (c->dropped)
                cb_close(hwnd, c, 1);
            else
                cb_open(hwnd, c);
        } else {
            cb_key_move(hwnd, c, 1);
        }
        break;
    case VK_UP:
        if (alt) {
            if (c->dropped)
                cb_close(hwnd, c, 1);
        } else {
            cb_key_move(hwnd, c, -1);
        }
        break;
    case VK_HOME:
        if (c->count)
            cb_step(hwnd, c, 0);
        break;
    case VK_END:
        if (c->count)
            cb_step(hwnd, c, c->count - 1);
        break;
    case VK_RETURN:
        if (c->dropped)
            cb_close(hwnd, c, 1);
        break;
    case VK_ESCAPE:
        if (c->dropped)
            cb_close(hwnd, c, 0);
        break;
    default:
        break;
    }
    return 0;
}

static void cb_mouse_down(HWND hwnd, cb_t *c, LPARAM lp)
{
    int y = (int)(short)HIWORD(lp);
    int closed = cb_closed_h(hwnd);
    if (!c->focused)
        __aw_user32_SetFocus(hwnd);
    if (y >= closed)
        return;
    if (c->dropped)
        cb_close(hwnd, c, 0);
    else
        cb_open(hwnd, c);
}

/* ------------------------------------------------------------------ CB_* messages */

static LRESULT cb_cb_msg(HWND hwnd, cb_t *c, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    int idx = (int)(intptr_t)wp;
    *handled = 1;
    switch (msg) {
    case CB_ADDSTRING:
    case CB_INSERTSTRING: {
        int at = c->count;
        if (msg == CB_INSERTSTRING && idx >= 0 && idx < c->count)
            at = idx;
        if (c->sorted) {
            at = 0;
            while (at < c->count && cb_cmpi(c->items[at].text, (const WCHAR *)(uintptr_t)lp) <= 0)
                at++;
        }
        {
            int r = cb_insert_at(c, at, (const WCHAR *)(uintptr_t)lp);
            __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
            cb_inval_list(c);
            return r;
        }
    }
    case CB_DELETESTRING: {
        int was;
        if (idx < 0 || idx >= c->count)
            return CB_ERR;
        was = c->cursel;
        cb_remove_at(c, idx);
        if (c->cursel > idx) {
            c->cursel--;
        } else if (was == idx) {
            c->cursel = -1;
            cb_set_edit_text(c, NULL);
        }
        if (c->hot >= c->count)
            c->hot = c->count - 1;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        cb_inval_list(c);
        return c->count;
    }
    case CB_RESETCONTENT:
        cb_clear(c);
        cb_set_edit_text(c, NULL);
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        cb_inval_list(c);
        return 0;
    case CB_GETCOUNT:
        return c->count;
    case CB_GETCURSEL:
        return c->cursel;
    case CB_SETCURSEL:
        if (idx < 0 || idx >= c->count) {
            cb_set_cursel(hwnd, c, -1);
            return CB_ERR;
        }
        cb_set_cursel(hwnd, c, idx);
        return idx;
    case CB_GETLBTEXT: {
        WCHAR *out = (WCHAR *)(uintptr_t)lp;
        int n;
        if (idx < 0 || idx >= c->count || !out)
            return CB_ERR;
        n = ctl_wlen(c->items[idx].text);
        memcpy(out, c->items[idx].text, (size_t)(n + 1) * sizeof(WCHAR));
        return n;
    }
    case CB_GETLBTEXTLEN:
        if (idx < 0 || idx >= c->count)
            return CB_ERR;
        return ctl_wlen(c->items[idx].text);
    case CB_FINDSTRING:
        return cb_find(c, idx, (const WCHAR *)(uintptr_t)lp, 0);
    case CB_FINDSTRINGEXACT:
        return cb_find(c, idx, (const WCHAR *)(uintptr_t)lp, 1);
    case CB_SELECTSTRING: {
        int r = cb_find(c, idx, (const WCHAR *)(uintptr_t)lp, 0);
        if (r >= 0)
            cb_set_cursel(hwnd, c, r);
        return r;
    }
    case CB_GETITEMDATA:
        if (idx < 0 || idx >= c->count)
            return CB_ERR;
        return (LRESULT)c->items[idx].data;
    case CB_SETITEMDATA:
        if (idx < 0 || idx >= c->count)
            return CB_ERR;
        c->items[idx].data = (LPARAM)lp;
        return 0;
    case CB_SHOWDROPDOWN:
        if (wp)
            cb_open(hwnd, c);
        else
            cb_close(hwnd, c, 0);
        return 1;
    case CB_GETDROPPEDSTATE:
        return c->dropped;
    case CB_GETDROPPEDCONTROLRECT: {
        RECT *out = (RECT *)(uintptr_t)lp;
        if (out)
            cb_geom(hwnd, c, out, NULL);
        return 0;
    }
    case CB_SETDROPPEDWIDTH:
        if (idx < 0)
            return CB_ERR;
        c->drop_w = idx;
        return idx;
    case CB_GETDROPPEDWIDTH: {
        RECT cr;
        __aw_user32_GetClientRect(hwnd, &cr);
        return c->drop_w > 0 ? c->drop_w : cr.right - cr.left;
    }
    case CB_SETMINVISIBLE:
        c->min_visible = idx;
        return 1;
    case CB_GETMINVISIBLE:
        return c->min_visible;
    case CB_SETITEMHEIGHT:
        if (idx == -1)
            return 0;
        c->item_h = (int)lp;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case CB_GETITEMHEIGHT:
        if (idx == -1)
            return cb_closed_h(hwnd);
        return cb_itemh(hwnd, c);
    case CB_SETEDITSEL:
        if (!c->edit)
            return CB_ERR;
        __aw_user32_SendMessageW(c->edit, EM_SETSEL, (WPARAM)LOWORD(lp), (LPARAM)HIWORD(lp));
        return 1;
    case CB_GETEDITSEL:
        if (!c->edit)
            return CB_ERR;
        return __aw_user32_SendMessageW(c->edit, EM_GETSEL, 0, 0);
    case CB_LIMITTEXT:
        if (!c->edit)
            return CB_ERR;
        __aw_user32_SendMessageW(c->edit, EM_LIMITTEXT, wp, 0);
        return 0;
    case CB_SETCUEBANNER:
        if (!c->edit)
            return 0;
        return __aw_user32_SendMessageW(c->edit, EM_SETCUEBANNER, wp, lp);
    case CB_GETCUEBANNER:
        if (!c->edit)
            return 0;
        return __aw_user32_SendMessageW(c->edit, CB_EM_GETCUEBANNER, wp, lp);
    case CB_SETEXTENDEDUI:
        c->ext_ui = idx;
        return 0;
    case CB_GETEXTENDEDUI:
        return c->ext_ui;
    case CB_GETTOPINDEX:
        return c->top;
    case CB_SETTOPINDEX:
        c->top = idx;
        cb_show_hot(c);
        cb_inval_list(c);
        return 0;
    case CB_GETHORIZONTALEXTENT:
        return c->hext;
    case CB_SETHORIZONTALEXTENT:
        c->hext = idx;
        return 0;
    case CB_INITSTORAGE:
        return 0;
    case CB_SETLOCALE:
    case CB_GETLOCALE:
        return 0;
    case CB_DIR:
        return CB_ERR;
    case CB_GETCOMBOBOXINFO: {
        COMBOBOXINFO *ci = (COMBOBOXINFO *)(uintptr_t)lp;
        RECT cr;
        int closed = cb_closed_h(hwnd);
        if (!ci)
            return 0;
        __aw_user32_GetClientRect(hwnd, &cr);
        ci->rcItem.left = 1;
        ci->rcItem.top = 1;
        ci->rcItem.right = cr.right - CB_ARROW_W - 1;
        ci->rcItem.bottom = closed - 1;
        ci->rcButton.left = cr.right - CB_ARROW_W;
        ci->rcButton.top = 1;
        ci->rcButton.right = cr.right - 1;
        ci->rcButton.bottom = closed - 1;
        ci->stateButton = 0;
        ci->hwndCombo = hwnd;
        ci->hwndItem = c->edit;
        ci->hwndList = c->list;
        return 1;
    }
    default:
        break;
    }
    *handled = 0;
    return 0;
}

static LRESULT cb_msg(HWND hwnd, cb_t *c, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    int closed;
    *handled = 1;
    switch (msg) {
    case WM_DESTROY:
        c->dropped = 0;
        c->list = NULL;
        if (c->popup) {
            __aw_user32_DestroyWindow(c->popup);
            c->popup = NULL;
        }
        break;
    case WM_PAINT:
        cb_paint(hwnd, c);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        cb_layout(hwnd, c);
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_ENABLE:
        if (c->edit)
            __aw_user32_EnableWindow(c->edit, wp != 0);
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_GETDLGCODE:
        return c->kind == CBS_DROPDOWNLIST ? (DLGC_WANTARROWS | DLGC_WANTCHARS) : 0;
    case WM_LBUTTONDOWN:
        cb_mouse_down(hwnd, c, lp);
        return 0;
    case WM_LBUTTONDBLCLK:
        closed = cb_closed_h(hwnd);
        if (c->kind == CBS_DROPDOWNLIST && (int)(short)HIWORD(lp) < closed)
            ctl_command(hwnd, CBN_DBLCLK);
        return 0;
    case WM_KEYDOWN:
        return cb_keydown(hwnd, c, wp);
    case WM_CHAR:
        if (c->kind == CBS_DROPDOWNLIST && wp >= 0x20) {
            WCHAR pre[2];
            int idx;
            pre[0] = (WCHAR)wp;
            pre[1] = 0;
            idx = cb_find(c, c->cursel, pre, 0);
            if (idx >= 0)
                cb_step(hwnd, c, idx);
        }
        return 0;
    case WM_SETFOCUS:
        if (c->kind != CBS_DROPDOWNLIST && c->edit) {
            __aw_user32_SetFocus(c->edit);
            return 0;
        }
        if (c->focused)
            return 0;
        c->focused = 1;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        ctl_command(hwnd, CBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        /* focus went to our own drop list (activation of the popup): the list stays open */
        if (c->dropped)
            return 0;
        c->focused = 0;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        if (c->kind == CBS_DROPDOWNLIST)
            ctl_command(hwnd, CBN_KILLFOCUS);
        return 0;
    case WM_COMMAND:
        if (c->edit && (HWND)(uintptr_t)lp == c->edit) {
            switch (HIWORD(wp)) {
            case EN_CHANGE:
                if (!c->suppress)
                    ctl_command(hwnd, CBN_EDITCHANGE);
                break;
            case EN_UPDATE:
                if (!c->suppress)
                    ctl_command(hwnd, CBN_EDITUPDATE);
                break;
            case EN_SETFOCUS:
                c->focused = 1;
                ctl_command(hwnd, CBN_SETFOCUS);
                break;
            case EN_KILLFOCUS:
                if (c->dropped)
                    break;
                c->focused = 0;
                ctl_command(hwnd, CBN_KILLFOCUS);
                break;
            default:
                break;
            }
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC: {
        HWND p = __aw_user32_GetParent(hwnd);
        if (!p)
            return 0;
        return __aw_user32_SendMessageW(p, msg, wp, lp);
    }
    case WM_GETTEXT: {
        WCHAR *buf = (WCHAR *)(uintptr_t)lp;
        int max = (int)wp, n;
        if (c->edit)
            return __aw_user32_SendMessageW(c->edit, WM_GETTEXT, wp, lp);
        if (!buf || max <= 0)
            return 0;
        {
            const WCHAR *t = (c->cursel >= 0 && c->cursel < c->count) ? c->items[c->cursel].text : ctl_empty_w;
            n = ctl_wlen(t);
            if (n > max - 1)
                n = max - 1;
            memcpy(buf, t, (size_t)n * sizeof(WCHAR));
            buf[n] = 0;
        }
        return n;
    }
    case WM_GETTEXTLENGTH:
        if (c->edit)
            return __aw_user32_SendMessageW(c->edit, WM_GETTEXTLENGTH, 0, 0);
        return (c->cursel >= 0 && c->cursel < c->count) ? ctl_wlen(c->items[c->cursel].text) : 0;
    case WM_SETTEXT:
        if (c->edit)
            return __aw_user32_SendMessageW(c->edit, WM_SETTEXT, wp, lp);
        return CB_ERR;
    default:
        break;
    }
    *handled = 0;
    return 0;
}

static void cb_free(cb_t *c)
{
    cb_clear(c);
    free(c->items);
    c->items = NULL;
    c->cap = 0;
}

LRESULT WINAPI aw_combo_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    cb_t *c = ctl_get(hwnd);
    LRESULT res;
    int handled = 0;

    if (msg == WM_CREATE) {
        UINT style = (UINT)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
        c = ctl_alloc(hwnd, sizeof(cb_t));
        if (!c)
            return -1;
        c->cursel = -1;
        c->hot = -1;
        c->min_visible = CB_DEF_VISIBLE;
        c->style = (int)style;
        c->kind = (int)(style & CBS_TYPE_MASK);
        if (c->kind == 0)
            c->kind = CBS_DROPDOWN;
        c->sorted = (style & CBS_SORT) ? 1 : 0;
        if (c->kind != CBS_DROPDOWNLIST) {
            c->edit = __aw_user32_CreateWindowExW(0, cb_cls_edit, NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                                                  0, 0, 0, 0, hwnd, (HMENU)(uintptr_t)CB_EDIT_ID, NULL, NULL);
            if (c->edit)
                __aw_user32_SendMessageW(c->edit, CB_MSG_SETCOMBO, 0, 0);
        }
        cb_layout(hwnd, c);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        if (c)
            cb_free(c);
        ctl_free(hwnd);
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!c) {
        if (ctl_common_msg(hwnd, msg, wp, lp, &res))
            return res;
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_SETFONT && c->edit)
        __aw_user32_SendMessageW(c->edit, WM_SETFONT, wp, 1);
    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;

    res = cb_msg(hwnd, c, msg, wp, lp, &handled);
    if (handled)
        return res;
    res = cb_cb_msg(hwnd, c, msg, wp, lp, &handled);
    if (handled)
        return res;
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ ComboLBox (drop list) */

static int lb_row(const lb_t *l, const cb_t *c, int y)
{
    int ih = cb_itemh(l->owner, c);
    int idx;
    if (y < 0 || ih <= 0)
        return -1;
    idx = c->top + y / ih;
    if (idx < 0 || idx >= c->count)
        return -1;
    return idx;
}

static void lb_paint(HWND hwnd, lb_t *l)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rc;
    HBRUSH bg;
    HFONT old;
    cb_t *c = ctl_get(l->owner);
    COLORREF tc;
    int ih, i, y, od;

    hdc = __aw_user32_BeginPaint(hwnd, &ps);
    if (!hdc)
        return;
    __aw_user32_GetClientRect(hwnd, &rc);
    bg = (HBRUSH)(uintptr_t)__aw_user32_SendMessageW(l->owner, WM_CTLCOLORLISTBOX, (WPARAM)(uintptr_t)hdc, (LPARAM)hwnd);
    ctl_fill(hdc, &rc, bg ? bg : __aw_user32_GetSysColorBrush(COLOR_WINDOW));
    if (c) {
        old = (HFONT)__aw_gdi32_SelectObject(hdc, ctl_font(l->owner));
        __aw_gdi32_SetBkMode(hdc, 1);
        tc = (COLORREF)__aw_gdi32_GetTextColor(hdc);
        ih = cb_itemh(l->owner, c);
        od = c->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE);
        for (i = c->top, y = 0; i < c->count && y < rc.bottom; i++, y += ih) {
            RECT r;
            int sel = (i == c->hot);
            r.left = rc.left;
            r.right = rc.right;
            r.top = y;
            r.bottom = y + ih;
            if (od) {
                DRAWITEMSTRUCT dis;
                memset(&dis, 0, sizeof(dis));
                dis.CtlType = ODT_COMBOBOX;
                dis.CtlID = (UINT)__aw_user32_GetDlgCtrlID(l->owner);
                dis.itemID = (UINT)i;
                dis.itemAction = ODA_DRAWENTIRE;
                dis.itemState = sel ? ODS_SELECTED : 0;
                dis.hwndItem = l->owner;
                dis.hDC = hdc;
                dis.rcItem = r;
                dis.itemData = (DWORD_PTR)c->items[i].data;
                ctl_send_parent(l->owner, WM_DRAWITEM, dis.CtlID, (LPARAM)&dis);
                continue;
            }
            if (sel) {
                __aw_user32_FillRect(hdc, &r, __aw_user32_GetSysColorBrush(COLOR_HIGHLIGHT));
                __aw_gdi32_SetTextColor(hdc, aw_syscolor(COLOR_HIGHLIGHTTEXT));
            } else {
                __aw_gdi32_SetTextColor(hdc, tc);
            }
            {
                RECT tr = r;
                tr.left += 2;
                __aw_user32_DrawTextW(hdc, c->items[i].text, -1, &tr,
                                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
        }
        __aw_gdi32_SelectObject(hdc, old);
    }
    __aw_user32_EndPaint(hwnd, &ps);
}

/* hit test from the screen cursor: while the list holds the capture, the coordinates in lParam can be
 * relative to the window the pointer was really over, so they are not used */
static void lb_mouse_row(HWND hwnd, lb_t *l, LPARAM lp, int *inside, int *row)
{
    RECT rc;
    POINT pt;
    int x, y;
    cb_t *c = ctl_get(l->owner);
    (void)lp;
    pt.x = 0;
    pt.y = 0;
    __aw_user32_GetCursorPos(&pt);
    __aw_user32_ScreenToClient(hwnd, &pt);
    x = pt.x;
    y = pt.y;
    __aw_user32_GetClientRect(hwnd, &rc);
    *inside = x >= 0 && x < rc.right && y >= 0 && y < rc.bottom;
    *row = (c && *inside) ? lb_row(l, c, y) : -1;
}

LRESULT WINAPI aw_combolbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    lb_t *l = ctl_get(hwnd);
    LRESULT res;

    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;
    if (msg == WM_CREATE) {
        cb_createw *cs = (cb_createw *)(uintptr_t)lp;
        l = ctl_alloc(hwnd, sizeof(lb_t));
        if (!l)
            return -1;
        l->owner = cs ? (HWND)cs->lpCreateParams : NULL;
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        ctl_free(hwnd);
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!l || !l->owner)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT:
        lb_paint(hwnd, l);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return CB_MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        cb_t *c = ctl_get(l->owner);
        int inside, row;
        if (!c || !c->dropped)
            return 0;
        lb_mouse_row(hwnd, l, lp, &inside, &row);
        if (row >= 0 && row != c->hot) {
            c->hot = row;
            __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        /* the release of the click that opened the list also reaches us (through the capture); it
         * must not cancel, so only a press seen here arms the outside-release cancel */
        l->armed = 1;
        return 0;
    case WM_LBUTTONUP: {
        cb_t *c = ctl_get(l->owner);
        int inside, row;
        if (!c || !c->dropped)
            return 0;
        lb_mouse_row(hwnd, l, lp, &inside, &row);
        if (inside && row >= 0) {
            c->hot = row;
            cb_close(l->owner, c, 1);
        } else if (l->armed) {
            cb_close(l->owner, c, 0);
        }
        l->armed = 0;
        return 0;
    }
    case WM_KEYDOWN:
    case WM_CHAR:
        return __aw_user32_SendMessageW(l->owner, msg, wp, lp);
    case WM_CAPTURECHANGED: {
        cb_t *c = ctl_get(l->owner);
        if (c && c->dropped && c->list == hwnd)
            cb_close(l->owner, c, 0);
        return 0;
    }
    default:
        break;
    }
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}
