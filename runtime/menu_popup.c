/* anywin - popup menus: TrackPopupMenu(Ex), the popup window class "#32768", and the menu loop.
 * Work package G subset (S6). The menu bar hooks (aw_menubar_*) stay deferred and owner-draw items
 * are not supported (no WM_MEASUREITEM/WM_DRAWITEM). */
#include "menu_int.h"
#include <wctype.h>


#ifndef WM_MOUSEACTIVATE
#define WM_MOUSEACTIVATE 0x0021
#endif
#ifndef WM_CANCELMODE
#define WM_CANCELMODE 0x001F
#endif
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_NCRBUTTONDOWN 0x00A4
#define MP_WP(lo, hi) ((WPARAM)((((UINT)(hi)) << 16) | (((UINT)(lo)) & 0xFFFF)))
#define MP_WM_ENTERMENULOOP 0x0211
#define MP_WM_EXITMENULOOP 0x0212
#define MP_TIMER_HOVER 0x4D50
#define MP_HOVER_MS 400
#define MP_MAX_LEVELS 32
#define MP_PAD 3           /* top and bottom padding inside the popup */
#define MP_TEXT_X 26       /* caption starts after the check column */
#define MP_ACCEL_GAP 20
#define MP_ARROW_COL 24    /* room for the submenu arrow on the right */
#define MP_MIN_W 120
#define MP_SEP_H 7

#define MP_RGB(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define MP_C_BG MP_RGB(255, 255, 255)
#define MP_C_FRAME MP_RGB(204, 204, 204)
#define MP_C_TEXT MP_RGB(0, 0, 0)
#define MP_C_GRAY MP_RGB(109, 109, 109)
#define MP_C_SEL MP_RGB(0, 120, 215)
#define MP_C_SEL_TEXT MP_RGB(255, 255, 255)
#define MP_C_SEL_GRAY MP_RGB(230, 230, 230)
#define MP_C_SEP MP_RGB(160, 160, 160)

typedef struct mp_level {
    HMENU menu;
    HWND hwnd;
    int x, y, w, h;   /* screen rect of the popup window */
    int n;            /* item count */
    int sel;          /* selected item index or -1 */
    int *ytop;        /* n+1 tops in window coordinates; item i spans [ytop[i], ytop[i+1]) */
} mp_level;

typedef struct mp_state {
    HWND owner;
    UINT flags;
    int done;
    UINT cmd;         /* chosen command id, 0 when cancelled */
    int nlev;
    mp_level lev[MP_MAX_LEVELS];
    HDC mdc;          /* measuring DC with the menu font selected */
    HGDIOBJ font;
    int text_h;
    int screen_w, screen_h;
} mp_state;

static mp_state *g_mp;
static int g_class_done;

/* ---------------------------------------------------------------- captions */
/* Copies the caption without its '&' mnemonic marks (&& -> &) and stops at the tab that
 * introduces the accelerator text. Returns the length; *accel points at the accelerator text. */
static int mp_caption(const WCHAR *src, WCHAR *out, int cap, const WCHAR **accel)
{
    int n = 0;
    *accel = NULL;
    for (const WCHAR *p = src; p && *p; p++) {
        if (*p == '\t') {
            *accel = p + 1;
            break;
        }
        if (*p == '&') {
            if (p[1] == '&') {
                p++;
            } else {
                continue;
            }
        }
        if (n < cap - 1)
            out[n++] = *p;
    }
    out[n] = 0;
    return n;
}

/* the character of the first mnemonic (&x), lower-cased, or 0 */
static WCHAR mp_mnemonic(const WCHAR *src)
{
    for (const WCHAR *p = src; p && *p; p++) {
        if (*p == '\t')
            break;
        if (*p == '&' && p[1] && p[1] != '&')
            return (WCHAR)towlower(p[1]);
        if (*p == '&' && p[1] == '&')
            p++;
    }
    return 0;
}

static int mp_text_w(HDC dc, const WCHAR *s, int n)
{
    SIZE sz;
    if (!s || n <= 0)
        return 0;
    if (!__aw_gdi32_GetTextExtentPoint32W(dc, s, n, &sz))
        return 0;
    return (int)sz.cx;
}

/* ---------------------------------------------------------------- layout */
static int item_is_sep(const aw_mitem *it) { return (it->fType & MFT_SEPARATOR) != 0; }

static int item_height(const mp_state *s, const aw_mitem *it)
{
    if (item_is_sep(it))
        return MP_SEP_H;
    int h = s->text_h + 6;
    return h < 20 ? 20 : h;
}

static int item_selectable(const aw_mitem *it) { return !item_is_sep(it); }

/* Computes the item tops and the popup size of menu m. Caller frees the ytop array. */
static int mp_layout(mp_state *s, aw_menu *m, mp_level *L)
{
    L->ytop = malloc(((size_t)m->n + 1) * sizeof *L->ytop);
    if (!L->ytop)
        return 0;
    int tw = 0, aw = 0, y = MP_PAD;
    WCHAR buf[512];
    for (int i = 0; i < m->n; i++) {
        const aw_mitem *it = &m->it[i];
        L->ytop[i] = y;
        y += item_height(s, it);
        if (item_is_sep(it) || !it->text)
            continue;
        const WCHAR *acc = NULL;
        int n = mp_caption(it->text, buf, 512, &acc);
        int cw = mp_text_w(s->mdc, buf, n);
        if (cw > tw)
            tw = cw;
        if (acc) {
            int an = (int)aw_wcslen(acc);
            int aaw = mp_text_w(s->mdc, acc, an);
            if (aaw > aw)
                aw = aaw;
        }
    }
    L->ytop[m->n] = y;
    L->n = m->n;
    L->h = y + MP_PAD;
    int w = MP_TEXT_X + tw + (aw ? MP_ACCEL_GAP + aw : 0) + MP_ARROW_COL;
    L->w = w < MP_MIN_W ? MP_MIN_W : w;
    return 1;
}

/* item index at window-relative y, or -1 */
static int level_item_at(const mp_level *L, int y)
{
    for (int i = 0; i < L->n; i++)
        if (y >= L->ytop[i] && y < L->ytop[i + 1])
            return i;
    return -1;
}

/* ---------------------------------------------------------------- painting */
static void fill_rect(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = {l, t, r, b};
    HBRUSH br = __aw_gdi32_CreateSolidBrush(c);
    if (!br)
        return;
    __aw_user32_FillRect(dc, &rc, br);
    __aw_gdi32_DeleteObject(br);
}

/* check mark (a filled tick) or radio bullet at the check column of row [top, bottom) */
static void paint_check(HDC dc, int top, int bottom, int radio, COLORREF c)
{
    int cy = (top + bottom) / 2;
    if (radio) {
        fill_rect(dc, 12, cy - 3, 18, cy + 3, c);
        return;
    }
    POINT pts[6] = {{8, cy}, {9, cy - 1}, {11, cy + 1}, {16, cy - 5}, {17, cy - 4}, {11, cy + 3}};
    HBRUSH br = __aw_gdi32_CreateSolidBrush(c);
    HGDIOBJ old_br = __aw_gdi32_SelectObject(dc, br);
    HGDIOBJ old_pen = __aw_gdi32_SelectObject(dc, __aw_gdi32_GetStockObject(8 /* NULL_PEN */));
    __aw_gdi32_Polygon(dc, pts, 6);
    __aw_gdi32_SelectObject(dc, old_pen);
    __aw_gdi32_SelectObject(dc, old_br);
    __aw_gdi32_DeleteObject(br);
}

static void paint_arrow(HDC dc, int right, int cy, COLORREF c)
{
    POINT pts[3] = {{right - 8, cy - 4}, {right - 4, cy}, {right - 8, cy + 4}};
    HBRUSH br = __aw_gdi32_CreateSolidBrush(c);
    HGDIOBJ old_br = __aw_gdi32_SelectObject(dc, br);
    HGDIOBJ old_pen = __aw_gdi32_SelectObject(dc, __aw_gdi32_GetStockObject(8 /* NULL_PEN */));
    __aw_gdi32_Polygon(dc, pts, 3);
    __aw_gdi32_SelectObject(dc, old_pen);
    __aw_gdi32_SelectObject(dc, old_br);
    __aw_gdi32_DeleteObject(br);
}

static void mp_paint(mp_state *s, mp_level *L, HDC dc)
{
    __aw_gdi32_SelectObject(dc, s->font);
    __aw_gdi32_SetBkMode(dc, 1 /* TRANSPARENT */);
    fill_rect(dc, 0, 0, L->w, L->h, MP_C_BG);
    aw_menu *m = aw_menu_get(L->menu);
    if (!m)
        return;
    WCHAR buf[512];
    for (int i = 0; i < m->n; i++) {
        const aw_mitem *it = &m->it[i];
        int top = L->ytop[i], bot = L->ytop[i + 1];
        if (item_is_sep(it)) {
            fill_rect(dc, 6, top + MP_SEP_H / 2, L->w - 6, top + MP_SEP_H / 2 + 1, MP_C_SEP);
            continue;
        }
        int disabled = (it->fState & MFS_GRAYED) != 0;
        int sel = (i == L->sel);
        COLORREF tc = disabled ? MP_C_GRAY : MP_C_TEXT;
        if (sel) {
            if (disabled)
                fill_rect(dc, 2, top, L->w - 2, bot, MP_C_SEL_GRAY);
            else
                fill_rect(dc, 2, top, L->w - 2, bot, MP_C_SEL);
            if (!disabled)
                tc = MP_C_SEL_TEXT;
        }
        if (it->fState & MFS_CHECKED)
            paint_check(dc, top, bot, (it->fType & MFT_RADIOCHECK) != 0, tc);
        const WCHAR *acc = NULL;
        int ty = top + (bot - top - s->text_h) / 2;
        __aw_gdi32_SetTextColor(dc, tc);
        if (it->text) {
            int n = mp_caption(it->text, buf, 512, &acc);
            __aw_gdi32_TextOutW(dc, MP_TEXT_X, ty, buf, n);
        }
        if (acc && *acc) {
            int an = (int)aw_wcslen(acc);
            int aw = mp_text_w(dc, acc, an);
            __aw_gdi32_TextOutW(dc, L->w - MP_ARROW_COL - aw, ty, acc, an);
        }
        if (it->popup)
            paint_arrow(dc, L->w - 4, (top + bot) / 2, tc);
    }
    /* 1px frame */
    fill_rect(dc, 0, 0, L->w, 1, MP_C_FRAME);
    fill_rect(dc, 0, L->h - 1, L->w, L->h, MP_C_FRAME);
    fill_rect(dc, 0, 0, 1, L->h, MP_C_FRAME);
    fill_rect(dc, L->w - 1, 0, L->w, L->h, MP_C_FRAME);
}

static mp_level *mp_find_hwnd(mp_state *s, HWND h)
{
    if (!s)
        return NULL;
    for (int i = 0; i < s->nlev; i++)
        if (s->lev[i].hwnd == h)
            return &s->lev[i];
    return NULL;
}

static LRESULT AWCALL mp_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return 3; /* MA_NOACTIVATE */
    case WM_PAINT: {
        AW_PAINTSTRUCT ps;
        HDC dc = __aw_user32_BeginPaint(h, &ps);
        mp_level *L = mp_find_hwnd(g_mp, h);
        if (L && dc)
            mp_paint(g_mp, L, dc);
        __aw_user32_EndPaint(h, &ps);
        return 0;
    }
    default:
        return __aw_user32_DefWindowProcW(h, msg, wp, lp);
    }
}

void aw_menus_register(void)
{
    if (g_class_done)
        return;
    g_class_done = 1;
    AW_WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = mp_wndproc;
    wc.lpszClassName = AW_MENU_CLASS;
    __aw_user32_RegisterClassExW(&wc);
}

/* ---------------------------------------------------------------- owner notifications */
static void notify_owner(mp_state *s, UINT msg, WPARAM wp, LPARAM lp)
{
    if (s->owner)
        __aw_user32_SendMessageW(s->owner, msg, wp, lp);
}

static void notify_select(mp_state *s, mp_level *L, int item, int mouse)
{
    aw_menu *m = aw_menu_get(L->menu);
    if (!m)
        return;
    WPARAM wp;
    if (item < 0 || item >= m->n) {
        wp = MP_WP(0, 0xFFFF);
    } else {
        const aw_mitem *it = &m->it[item];
        UINT f = MF_HILITE;
        if (it->popup)
            f |= MF_POPUP;
        if (mouse)
            f |= MF_MOUSESELECT;
        wp = MP_WP(it->popup ? (UINT)item : it->id, f);
    }
    notify_owner(s, WM_MENUSELECT, wp, (LPARAM)L->menu);
}

/* ---------------------------------------------------------------- levels */
static void kill_hover(mp_state *s)
{
    if (s->nlev > 0)
        __aw_user32_KillTimer(s->lev[s->nlev - 1].hwnd, MP_TIMER_HOVER);
}

static void set_capture_top(mp_state *s)
{
    if (s->nlev > 0)
        __aw_user32_SetCapture(s->lev[s->nlev - 1].hwnd);
}

static void level_destroy(mp_state *s, int k)
{
    mp_level *L = &s->lev[k];
    if (L->hwnd)
        __aw_user32_DestroyWindow(L->hwnd);
    free(L->ytop);
    memset(L, 0, sizeof *L);
    (void)s;
}

/* closes the levels above k (k = index that stays open) */
static void close_above(mp_state *s, int k)
{
    kill_hover(s);
    while (s->nlev > k + 1) {
        int top = s->nlev - 1;
        HMENU closing = s->lev[top].menu;
        level_destroy(s, top);
        s->nlev--;
        notify_owner(s, WM_UNINITMENUPOPUP, (WPARAM)closing, 0);
    }
    if (s->nlev > 0 && k >= 0)
        set_capture_top(s);
}

/* creates the popup window for menu at screen position (x, y) and pushes it as the top level */
static int push_level(mp_state *s, HMENU menu, int x, int y, int from_item)
{
    aw_menu *m = aw_menu_get(menu);
    if (!m || s->nlev >= MP_MAX_LEVELS)
        return 0;
    mp_level *L = &s->lev[s->nlev];
    memset(L, 0, sizeof *L);
    L->menu = menu;
    L->sel = -1;
    if (!mp_layout(s, m, L)) {
        memset(L, 0, sizeof *L);
        return 0;
    }
    L->x = x;
    L->y = y;
    L->hwnd = __aw_user32_CreateWindowExW(0x00000080 /* WS_EX_TOOLWINDOW */, AW_MENU_CLASS, NULL, 0x80000000L /* WS_POPUP */,
                                          x, y, L->w, L->h, NULL, NULL, NULL, NULL);
    if (!L->hwnd) {
        free(L->ytop);
        memset(L, 0, sizeof *L);
        return 0;
    }
    s->nlev++;
    __aw_user32_ShowWindow(L->hwnd, 4 /* SW_SHOWNOACTIVATE */);
    __aw_user32_SetCapture(L->hwnd);
    __aw_user32_InvalidateRect(L->hwnd, NULL, TRUE);
    notify_owner(s, WM_INITMENUPOPUP, (WPARAM)menu, MAKELONG(from_item, 0));
    return 1;
}

static int first_selectable(const mp_level *L)
{
    aw_menu *m = aw_menu_get(L->menu);
    if (!m)
        return -1;
    for (int i = 0; i < m->n; i++)
        if (item_selectable(&m->it[i]))
            return i;
    return -1;
}

/* opens the submenu of item `item` of the top level (no-op if it is not a popup or is open) */
static void open_sub(mp_state *s, int item)
{
    int k = s->nlev - 1;
    if (k < 0)
        return;
    mp_level *P = &s->lev[k];
    aw_menu *m = aw_menu_get(P->menu);
    if (!m || item < 0 || item >= m->n || !m->it[item].popup)
        return;
    if (s->nlev > k + 1)
        return;
    HMENU sub = m->it[item].sub;
    aw_menu *sm = aw_menu_get(sub);
    if (!sm)
        return;
    int sw = s->screen_w, sh = s->screen_h;
    /* size of the submenu is needed for placement: lay it out through a temporary level */
    mp_level tmp;
    memset(&tmp, 0, sizeof tmp);
    if (!mp_layout(s, sm, &tmp))
        return;
    int x = P->x + P->w;
    int y = P->y + P->ytop[item] - MP_PAD;
    if (x + tmp.w > sw)
        x = P->x - tmp.w;
    if (x < 0)
        x = 0;
    if (y + tmp.h > sh)
        y = sh - tmp.h;
    if (y < 0)
        y = 0;
    free(tmp.ytop);
    if (!push_level(s, sub, x, y, item))
        return;
    mp_level *L = &s->lev[s->nlev - 1];
    L->sel = first_selectable(L);
    notify_select(s, L, L->sel, 0);
    __aw_user32_InvalidateRect(L->hwnd, NULL, FALSE);
}

/* selects item `item` of level k; mouse selections of submenus wait for the hover delay unless
 * `now` is set (clicks and keyboard) */
static void select_item(mp_state *s, int k, int item, int mouse, int now)
{
    mp_level *L = &s->lev[k];
    aw_menu *m = aw_menu_get(L->menu);
    if (!m)
        return;
    int popup = item >= 0 && item < m->n && m->it[item].popup;
    if (L->sel == item) {
        /* same row: only a click or a key opens its submenu; hover is already handled */
        if (now && popup && s->nlev == k + 1)
            open_sub(s, item);
        return;
    }
    /* the row whose submenu is open loses its selection: collapse that submenu */
    if (k < s->nlev - 1)
        close_above(s, k);
    L->sel = item;
    __aw_user32_InvalidateRect(L->hwnd, NULL, FALSE);
    notify_select(s, L, item, mouse);
    if (popup) {
        if (now)
            open_sub(s, item);
        else
            __aw_user32_SetTimer(L->hwnd, MP_TIMER_HOVER, MP_HOVER_MS, NULL);
    }
}

/* closes the top submenu (Esc / Left) */
static void close_top(mp_state *s)
{
    if (s->nlev > 1)
        close_above(s, s->nlev - 2);
}

static void end_menu(mp_state *s, UINT cmd)
{
    s->cmd = cmd;
    s->done = 1;
}

/* ---------------------------------------------------------------- hit testing */
/* screen point -> level index (top-most first) and item; level -1 when outside every menu */
static int mp_hit(mp_state *s, int sx, int sy, int *item)
{
    *item = -1;
    for (int k = s->nlev - 1; k >= 0; k--) {
        mp_level *L = &s->lev[k];
        if (sx >= L->x && sx < L->x + L->w && sy >= L->y && sy < L->y + L->h) {
            *item = level_item_at(L, sy - L->y);
            return k;
        }
    }
    return -1;
}

static int button_activates(mp_state *s, UINT msg)
{
    int right = (s->flags & 0x0002 /* TPM_RIGHTBUTTON */) != 0;
    if (msg == WM_LBUTTONUP)
        return !right;
    return right;
}

static void mouse_event(mp_state *s, UINT msg, int sx, int sy)
{
    int item;
    int k = mp_hit(s, sx, sy, &item);
    switch (msg) {
    case WM_MOUSEMOVE:
        if (k >= 0 && item >= 0)
            select_item(s, k, item, 1, 0);
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (k < 0) {
            end_menu(s, 0);
            break;
        }
        if (item >= 0 && item_selectable(&aw_menu_get(s->lev[k].menu)->it[item]))
            select_item(s, k, item, 1, 1);
        break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        if (k < 0 || !button_activates(s, msg) || item < 0)
            break;
        aw_menu *m = aw_menu_get(s->lev[k].menu);
        const aw_mitem *it = &m->it[item];
        if (!item_selectable(it))
            break;
        if (it->popup) {
            if (k == s->nlev - 1)
                open_sub(s, item);
            break;
        }
        if (it->fState & MFS_GRAYED)
            break;
        end_menu(s, it->id);
        break;
    }
    default:
        break;
    }
}

static void move_sel(mp_state *s, int dir)
{
    mp_level *L = &s->lev[s->nlev - 1];
    aw_menu *m = aw_menu_get(L->menu);
    if (!m || m->n == 0)
        return;
    int i = L->sel;
    for (int tries = 0; tries < m->n; tries++) {
        if (i < 0)
            i = dir > 0 ? 0 : m->n - 1;
        else
            i = (i + dir + m->n) % m->n;
        if (item_selectable(&m->it[i])) {
            select_item(s, s->nlev - 1, i, 0, 0);
            return;
        }
    }
}

static void key_event(mp_state *s, UINT vk)
{
    int top = s->nlev - 1;
    mp_level *L = &s->lev[top];
    aw_menu *m = aw_menu_get(L->menu);
    switch (vk) {
    case 0x1B: /* VK_ESCAPE */
        if (top > 0)
            close_top(s);
        else
            end_menu(s, 0);
        break;
    case 0x25: /* VK_LEFT */
        if (top > 0)
            close_top(s);
        break;
    case 0x27: /* VK_RIGHT */
        if (m && L->sel >= 0 && m->it[L->sel].popup) {
            open_sub(s, L->sel);
        }
        break;
    case 0x26: /* VK_UP */
        move_sel(s, -1);
        break;
    case 0x28: /* VK_DOWN */
        move_sel(s, 1);
        break;
    case 0x24: /* VK_HOME */
        if (m && m->n) {
            int f = first_selectable(L);
            if (f >= 0)
                select_item(s, top, f, 0, 0);
        }
        break;
    case 0x23: /* VK_END */
        if (m) {
            for (int i = m->n - 1; i >= 0; i--)
                if (item_selectable(&m->it[i])) {
                    select_item(s, top, i, 0, 0);
                    break;
                }
        }
        break;
    case 0x0D: /* VK_RETURN */
        if (m && L->sel >= 0) {
            const aw_mitem *it = &m->it[L->sel];
            if (it->popup)
                open_sub(s, L->sel);
            else if (!(it->fState & MFS_GRAYED))
                end_menu(s, it->id);
        }
        break;
    default:
        break;
    }
}

/* a printable character: selects or activates the item with that mnemonic */
static void char_event(mp_state *s, WCHAR c)
{
    int top = s->nlev - 1;
    mp_level *L = &s->lev[top];
    aw_menu *m = aw_menu_get(L->menu);
    if (!m || c < 0x20)
        return;
    WCHAR want = (WCHAR)towlower(c);
    int matches = 0, first = -1, after = -1;
    for (int i = 0; i < m->n; i++) {
        if (!m->it[i].text || item_is_sep(&m->it[i]))
            continue;
        if (mp_mnemonic(m->it[i].text) == want) {
            matches++;
            if (first < 0)
                first = i;
            if (after < 0 && i > L->sel)
                after = i;
        }
    }
    if (matches == 0) {
        /* let the owner decide (WM_MENUCHAR): MNC_CLOSE ends the menu, MNC_EXECUTE picks an item */
        LRESULT r = 0;
        if (s->owner)
            r = __aw_user32_SendMessageW(s->owner, WM_MENUCHAR, MP_WP(c, MF_HILITE), (LPARAM)L->menu);
        if ((r & 0xFFFF) == 1)
            end_menu(s, 0);
        else if ((r & 0xFFFF) == 2) {
            int idx = (int)((r >> 16) & 0xFFFF);
            if (idx >= 0 && idx < m->n && item_selectable(&m->it[idx]))
                select_item(s, top, idx, 0, 1);
        }
        return;
    }
    int pick = matches == 1 ? first : (after >= 0 ? after : first);
    const aw_mitem *it = &m->it[pick];
    if (matches == 1) {
        if (it->popup)
            select_item(s, top, pick, 0, 1);
        else if (!(it->fState & MFS_GRAYED))
            end_menu(s, it->id);
    } else {
        select_item(s, top, pick, 0, 0);
    }
}

static void hover_timer(mp_state *s, HWND h)
{
    if (s->nlev == 0 || s->lev[s->nlev - 1].hwnd != h)
        return;
    __aw_user32_KillTimer(h, MP_TIMER_HOVER);
    mp_level *L = &s->lev[s->nlev - 1];
    aw_menu *m = aw_menu_get(L->menu);
    if (m && L->sel >= 0 && L->sel < m->n && m->it[L->sel].popup)
        open_sub(s, L->sel);
}

/* the menu loop: owns every message of the thread while the menu is up */
static void menu_loop(mp_state *s)
{
    AW_MSG m;
    while (!s->done) {
        if (!__aw_user32_GetMessageW(&m, NULL, 0, 0)) {
            __aw_user32_PostQuitMessage((int)m.wParam);
            break;
        }
        switch (m.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            __aw_user32_TranslateMessage(&m);
            if (m.message == WM_KEYDOWN)
                key_event(s, (UINT)m.wParam);
            else if (m.wParam == 0x79 /* VK_F10 */ || m.wParam == 0x12 /* VK_MENU */)
                end_menu(s, 0);
            break;
        case WM_CHAR:
            char_event(s, (WCHAR)m.wParam);
            break;
        case WM_SYSCHAR:
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            mouse_event(s, m.message, m.pt.x, m.pt.y);
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            break;
        case WM_TIMER:
            if (m.wParam == MP_TIMER_HOVER)
                hover_timer(s, m.hwnd);
            else {
                __aw_user32_TranslateMessage(&m);
                __aw_user32_DispatchMessageW(&m);
            }
            break;
        case WM_CANCELMODE:
            end_menu(s, 0);
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
            end_menu(s, 0);
            break;
        default:
            __aw_user32_TranslateMessage(&m);
            __aw_user32_DispatchMessageW(&m);
            break;
        }
    }
}

/* ---------------------------------------------------------------- TrackPopupMenu */
static void place_top(mp_state *s, const mp_level *L, int *x, int *y, UINT align, const RECT *excl)
{
    if (align & 0x0004 /* TPM_CENTERALIGN */)
        *x -= L->w / 2;
    else if (align & 0x0008 /* TPM_RIGHTALIGN */)
        *x -= L->w;
    if (align & 0x0010 /* TPM_VCENTERALIGN */)
        *y -= L->h / 2;
    else if (align & 0x0020 /* TPM_BOTTOMALIGN */)
        *y -= L->h;
    if (excl && *x < excl->right && *x + L->w > excl->left && *y < excl->bottom && *y + L->h > excl->top) {
        if (excl->bottom + L->h <= s->screen_h)
            *y = excl->bottom;
        else if (excl->top - L->h >= 0)
            *y = excl->top - L->h;
        else if (excl->right + L->w <= s->screen_w)
            *x = excl->right;
        else if (excl->left - L->w >= 0)
            *x = excl->left - L->w;
    }
    if (*x + L->w > s->screen_w)
        *x = s->screen_w - L->w;
    if (*x < 0)
        *x = 0;
    if (*y + L->h > s->screen_h)
        *y = s->screen_h - L->h;
    if (*y < 0)
        *y = 0;
}

/* Runs the menu. Returns 0 when the menu could not be shown, 1 otherwise; *cmd gets the choice. */
static int track_popup(HMENU hMenu, UINT flags, int x, int y, HWND owner, const RECT *excl, UINT *cmd)
{
    *cmd = 0;
    aw_user_init();
    if (!aw_menu_get(hMenu) || !aw_display)
        return 0;
    mp_state s;
    memset(&s, 0, sizeof s);
    s.owner = owner;
    s.flags = flags;
    aw_menus_register();
    plat_screen_size(&s.screen_w, &s.screen_h);
    s.mdc = __aw_gdi32_CreateCompatibleDC(NULL);
    if (!s.mdc)
        return 0;
    s.font = __aw_gdi32_GetStockObject(17 /* DEFAULT_GUI_FONT */);
    __aw_gdi32_SelectObject(s.mdc, s.font);
    SIZE sz;
    if (__aw_gdi32_GetTextExtentPoint32W(s.mdc, u"Ag", 2, &sz) && sz.cy > 0)
        s.text_h = (int)sz.cy;
    else
        s.text_h = 16;
    /* the top level is laid out first so that it can be placed on screen */
    mp_level probe;
    memset(&probe, 0, sizeof probe);
    if (!mp_layout(&s, aw_menu_get(hMenu), &probe)) {
        __aw_gdi32_DeleteDC(s.mdc);
        return 0;
    }
    place_top(&s, &probe, &x, &y, flags, excl);
    free(probe.ytop);

    mp_state *saved = g_mp; /* a menu may be shown while another one is tracking */
    g_mp = &s;
    notify_owner(&s, MP_WM_ENTERMENULOOP, 0, 0);
    notify_owner(&s, WM_INITMENU, (WPARAM)hMenu, 0);
    if (push_level(&s, hMenu, x, y, 0))
        menu_loop(&s);
    /* tear down: submenus first, then the top level; the owner sees the menu go away */
    kill_hover(&s);
    __aw_user32_ReleaseCapture();
    while (s.nlev > 1) {
        int top = s.nlev - 1;
        HMENU closing = s.lev[top].menu;
        level_destroy(&s, top);
        s.nlev--;
        notify_owner(&s, WM_UNINITMENUPOPUP, (WPARAM)closing, 0);
    }
    if (s.nlev == 1) {
        level_destroy(&s, 0);
        s.nlev = 0;
    }
    notify_owner(&s, WM_MENUSELECT, MP_WP(0, 0xFFFF), 0);
    notify_owner(&s, MP_WM_EXITMENULOOP, 0, 0);
    g_mp = saved;
    __aw_gdi32_DeleteDC(s.mdc);
    *cmd = s.cmd;
    return 1;
}

API(user32, BOOL, TrackPopupMenu, (HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const RECT *prcRect))
{
    (void)nReserved;
    UINT cmd = 0;
    if (!track_popup(hMenu, uFlags, x, y, hWnd, prcRect, &cmd))
        return FALSE;
    if (uFlags & 0x0100 /* TPM_RETURNCMD */)
        return (BOOL)cmd;
    if (cmd && !(uFlags & 0x0080 /* TPM_NONOTIFY */) && hWnd)
        __aw_user32_PostMessageW(hWnd, WM_COMMAND, cmd, 0);
    return TRUE;
}

API(user32, BOOL, EndMenu, (void))
{
    if (!g_mp)
        return FALSE;
    end_menu(g_mp, 0);
    return TRUE;
}
