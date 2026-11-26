/* anywin runtime - Edit control, single line (PLAN-CORE 3.6, PLAN 6-H).
 * Text is UTF-16 in the instance state (the control owns it; WM_GETTEXT/WM_SETTEXT go through it).
 * Selection is [min(anchor,caret), max(anchor,caret)). Mouse: click, drag, double-click word, Shift+click.
 * Keys: arrows, Ctrl+arrows, Home/End, Shift+movement, Backspace/Delete (+Ctrl), Ctrl+A/C/X/V,
 * Shift+Delete, Ctrl+Insert, Shift+Insert. Horizontal scroll with ES_AUTOHSCROLL; without it input is
 * refused once the text no longer fits. The caret is drawn by the control (no system caret, no blink).
 * ES_MULTILINE is accepted but behaves as single-line (documented gap). No undo buffer, no context menu.
 * Notifications: EN_UPDATE before the repaint, EN_CHANGE after it (also after WM_SETTEXT), EN_MAXTEXT,
 * EN_SETFOCUS/KILLFOCUS. A read-only or disabled edit asks the parent for WM_CTLCOLORSTATIC. */
#include "ctl.h"

#undef EN_MAXTEXT
#define EN_MAXTEXT 0x0501 /* ctl.h carries 0x0601 (= EN_HSCROLL); the SDK value is 0x0501 */
#define ED_LIMIT_DEFAULT 30000
#define ED_MSG_SETCOMBO (WM_USER + 0x1F00) /* private: a ComboBox marks its edit child (value shared with combo.c) */
#define ED_EC_LEFTMARGIN 1
#define ED_EC_RIGHTMARGIN 2
#define ED_EC_USEFONTINFO 0xFFFF

#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215
#endif
#ifndef ETO_CLIPPED
#define ETO_CLIPPED 0x0004
#endif

typedef struct {
    WCHAR *text;     /* UTF-16; text[len] == 0 */
    int len, cap;
    int anchor, caret;
    int limit;
    int scroll_x;    /* pixels scrolled left, single-line AUTOHSCROLL only */
    int focused, dragging, modified;
    int combo_child; /* the parent is a ComboBox: arrows/F4/Enter/Esc are forwarded to it */
    int margin_l, margin_r;
    WCHAR pw;        /* password character (ES_PASSWORD), 0 = show the text */
    WCHAR *cue;
    int cue_focus;
} edit_t;

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
} ed_createw;

/* ------------------------------------------------------------------ helpers */

static UINT ed_style(HWND hwnd)
{
    return (UINT)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
}

static int ed_border(HWND hwnd)
{
    LONG ex = __aw_user32_GetWindowLongW(hwnd, GWL_EXSTYLE);
    return (ed_style(hwnd) & WS_BORDER) || (ex & WS_EX_CLIENTEDGE) ? 1 : 0;
}

static int ed_lo(const edit_t *e)
{
    return e->anchor < e->caret ? e->anchor : e->caret;
}

static int ed_hi(const edit_t *e)
{
    return e->anchor > e->caret ? e->anchor : e->caret;
}

static int ed_isword(WCHAR c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

static int ed_is_high(WCHAR c)
{
    return c >= 0xD800 && c <= 0xDBFF;
}

static int ed_is_low(WCHAR c)
{
    return c >= 0xDC00 && c <= 0xDFFF;
}

/* a caret position between the two halves of a surrogate pair is not valid */
static int ed_valid_boundary(const edit_t *e, int i)
{
    return !(i > 0 && i < e->len && ed_is_low(e->text[i]) && ed_is_high(e->text[i - 1]));
}

static int ed_prev(const edit_t *e, int i)
{
    if (i <= 0)
        return 0;
    if (i >= 2 && ed_is_low(e->text[i - 1]) && ed_is_high(e->text[i - 2]))
        return i - 2;
    return i - 1;
}

static int ed_next(const edit_t *e, int i)
{
    if (i >= e->len)
        return e->len;
    if (ed_is_high(e->text[i]) && i + 1 < e->len && ed_is_low(e->text[i + 1]))
        return i + 2;
    return i + 1;
}

static int ed_word_left(const edit_t *e, int i)
{
    while (i > 0 && !ed_isword(e->text[i - 1]))
        i--;
    while (i > 0 && ed_isword(e->text[i - 1]))
        i--;
    return i;
}

static int ed_word_right(const edit_t *e, int i)
{
    while (i < e->len && ed_isword(e->text[i]))
        i++;
    while (i < e->len && !ed_isword(e->text[i]))
        i++;
    return i;
}

static WCHAR ed_upper(WCHAR c)
{
    if (c >= 'a' && c <= 'z')
        return (WCHAR)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return (WCHAR)(c - 32);
    return c;
}

static WCHAR ed_lower(WCHAR c)
{
    if (c >= 'A' && c <= 'Z')
        return (WCHAR)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return (WCHAR)(c + 32);
    return c;
}

static HDC ed_dc_begin(HWND hwnd, HFONT *old)
{
    HDC hdc = __aw_user32_GetDC(hwnd);
    *old = NULL;
    if (!hdc)
        return NULL;
    *old = (HFONT)__aw_gdi32_SelectObject(hdc, ctl_font(hwnd));
    return hdc;
}

static void ed_dc_end(HWND hwnd, HDC hdc, HFONT old)
{
    if (!hdc)
        return;
    __aw_gdi32_SelectObject(hdc, old);
    __aw_user32_ReleaseDC(hwnd, hdc);
}

static int ed_width(HDC hdc, const WCHAR *s, int n)
{
    SIZE sz;
    if (n <= 0 || !hdc)
        return 0;
    sz.cx = 0;
    sz.cy = 0;
    if (!__aw_gdi32_GetTextExtentPoint32W(hdc, s, n, &sz))
        return 0;
    return sz.cx;
}

/* the text as drawn and measured: the stored text, or a masked copy for passwords (*tmp is malloc'd) */
static const WCHAR *ed_disp(HWND hwnd, const edit_t *e, WCHAR **tmp)
{
    int i;
    *tmp = NULL;
    if (e->len <= 0)
        return ctl_empty_w;
    if (!((ed_style(hwnd) & ES_PASSWORD) && e->pw))
        return e->text;
    *tmp = malloc((size_t)e->len * sizeof(WCHAR));
    if (!*tmp)
        return e->text;
    for (i = 0; i < e->len; i++)
        (*tmp)[i] = e->pw;
    return *tmp;
}

static int ed_pwmode(HWND hwnd, const edit_t *e)
{
    return (ed_style(hwnd) & ES_PASSWORD) && e->pw ? 1 : 0;
}

/* the formatting rectangle in client coordinates */
static void ed_textrect(HWND hwnd, const edit_t *e, RECT *tr)
{
    RECT c;
    int b = ed_border(hwnd);
    __aw_user32_GetClientRect(hwnd, &c);
    tr->left = c.left + b + e->margin_l;
    tr->right = c.right - b - e->margin_r;
    tr->top = c.top + b;
    tr->bottom = c.bottom - b;
    if (tr->right < tr->left)
        tr->right = tr->left;
    if (tr->bottom < tr->top)
        tr->bottom = tr->top;
}

/* x where the text starts: alignment applies only while the text fits, otherwise it is scrolled */
static int ed_origin(UINT style, const RECT *tr, const edit_t *e, int total)
{
    int vw = tr->right - tr->left;
    if (total >= vw)
        return tr->left - e->scroll_x;
    switch (style & ES_ALIGNMASK) {
    case ES_CENTER:
        return tr->left + (vw - total) / 2;
    case ES_RIGHT:
        return tr->right - total;
    default:
        return tr->left;
    }
}

static void ed_fix_scroll(HWND hwnd, edit_t *e, HDC hdc)
{
    RECT tr;
    WCHAR *tmp;
    const WCHAR *d;
    int vw, total, cx;

    ed_textrect(hwnd, e, &tr);
    vw = tr.right - tr.left;
    d = ed_disp(hwnd, e, &tmp);
    total = ed_width(hdc, d, e->len);
    if (!(ed_style(hwnd) & ES_AUTOHSCROLL) || total <= vw) {
        e->scroll_x = 0;
    } else {
        cx = ed_width(hdc, d, e->caret);
        if (e->scroll_x > cx)
            e->scroll_x = cx;
        if (cx - e->scroll_x > vw - 1)
            e->scroll_x = cx - (vw - 1);
        if (e->scroll_x > total - vw + 1)
            e->scroll_x = total - vw + 1;
        if (e->scroll_x < 0)
            e->scroll_x = 0;
    }
    free(tmp);
}

static void ed_scroll_caret(HWND hwnd, edit_t *e)
{
    HFONT f;
    HDC hdc = ed_dc_begin(hwnd, &f);
    ed_fix_scroll(hwnd, e, hdc);
    ed_dc_end(hwnd, hdc, f);
}

/* the caret position nearest to client x (only code point boundaries) */
static int ed_index_at(HWND hwnd, const edit_t *e, HDC hdc, int x)
{
    RECT tr;
    WCHAR *tmp;
    const WCHAR *d;
    int total, origin, rel, i, best = 0, bestd = 0x7FFFFFFF;

    ed_textrect(hwnd, e, &tr);
    d = ed_disp(hwnd, e, &tmp);
    total = ed_width(hdc, d, e->len);
    origin = ed_origin(ed_style(hwnd), &tr, e, total);
    rel = x - origin;
    for (i = 0; i <= e->len; i++) {
        int w, dist;
        if (!ed_valid_boundary(e, i))
            continue;
        w = ed_width(hdc, d, i);
        dist = rel - w;
        if (dist < 0)
            dist = -dist;
        if (dist < bestd) {
            bestd = dist;
            best = i;
        }
    }
    free(tmp);
    return best;
}

/* ------------------------------------------------------------------ text mutation */

static int ed_reserve(edit_t *e, int n)
{
    int nc;
    WCHAR *p;
    if (n + 1 <= e->cap)
        return 1;
    nc = e->cap > 0 ? e->cap : 16;
    while (nc < n + 1)
        nc *= 2;
    p = realloc(e->text, (size_t)nc * sizeof(WCHAR));
    if (!p)
        return 0;
    e->text = p;
    e->cap = nc;
    return 1;
}

/* replaces [s,t) with n units from ins; returns the new length of the inserted run or -1 */
static int ed_splice(edit_t *e, int s, int t, const WCHAR *ins, int n)
{
    int newlen = e->len - (t - s) + n;
    if (!ed_reserve(e, newlen))
        return -1;
    if (e->len > t)
        memmove(e->text + s + n, e->text + t, (size_t)(e->len - t) * sizeof(WCHAR));
    if (n > 0)
        memcpy(e->text + s, ins, (size_t)n * sizeof(WCHAR));
    e->len = newlen;
    e->text[newlen] = 0;
    return n;
}

/* the single mutation path: applies the limit when asked, moves the caret, notifies the parent */
static int ed_replace(HWND hwnd, edit_t *e, int s, int t, const WCHAR *ins, int n, int limited)
{
    int maxed = 0, got;
    if (limited) {
        int room = e->limit - (e->len - (t - s));
        if (room < 0)
            room = 0;
        if (n > room) {
            n = room;
            maxed = 1;
        }
    }
    if (t <= s && n <= 0) {
        if (maxed)
            ctl_command(hwnd, EN_MAXTEXT);
        return 1;
    }
    got = ed_splice(e, s, t, ins, n);
    if (got < 0) {
        ctl_command(hwnd, 0x0500 /* EN_ERRSPACE */);
        return 0;
    }
    e->anchor = e->caret = s + got;
    e->modified = 1;
    ed_scroll_caret(hwnd, e);
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
    if (maxed)
        ctl_command(hwnd, EN_MAXTEXT);
    ctl_command(hwnd, EN_UPDATE);
    ctl_command(hwnd, EN_CHANGE);
    return 1;
}

/* user input (typing, paste): filters characters, applies case/number rules, refuses overflow */
static void ed_user_insert(HWND hwnd, edit_t *e, const WCHAR *src, int n)
{
    UINT style = ed_style(hwnd);
    WCHAR *ins;
    int k = 0, i, lo, hi;

    if (n <= 0 || (style & ES_READONLY))
        return;
    ins = malloc((size_t)n * sizeof(WCHAR));
    if (!ins)
        return;
    for (i = 0; i < n; i++) {
        WCHAR c = src[i];
        if (c < 0x20 || c == 0x7F)
            continue;
        if ((style & ES_NUMBER) && (c < '0' || c > '9'))
            continue;
        if (style & ES_UPPERCASE)
            c = ed_upper(c);
        else if (style & ES_LOWERCASE)
            c = ed_lower(c);
        ins[k++] = c;
    }
    lo = ed_lo(e);
    hi = ed_hi(e);
    if (k > 0 && !(style & ES_AUTOHSCROLL)) {
        HFONT f;
        HDC hdc = ed_dc_begin(hwnd, &f);
        RECT tr;
        int w;
        ed_textrect(hwnd, e, &tr);
        w = ed_width(hdc, e->text, lo) + ed_width(hdc, ins, k) + ed_width(hdc, e->text + hi, e->len - hi);
        ed_dc_end(hwnd, hdc, f);
        if (w > tr.right - tr.left) {
            free(ins);
            return;
        }
    }
    ed_replace(hwnd, e, lo, hi, ins, k, 1);
    free(ins);
}

static void ed_set_all(edit_t *e, const WCHAR *s, int n)
{
    if (n < 0)
        n = 0;
    if (!ed_reserve(e, n))
        return;
    if (n > 0)
        memmove(e->text, s, (size_t)n * sizeof(WCHAR));
    e->len = n;
    e->text[n] = 0;
    e->anchor = e->caret = 0;
    e->scroll_x = 0;
}

static void ed_move(HWND hwnd, edit_t *e, int pos, int extend)
{
    if (extend)
        e->caret = pos;
    else
        e->anchor = e->caret = pos;
    ed_scroll_caret(hwnd, e);
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
}

static void ed_copy(HWND hwnd, edit_t *e)
{
    char *u;
    int lo = ed_lo(e), hi = ed_hi(e);
    if (lo == hi || ed_pwmode(hwnd, e))
        return;
    u = aw_w2u(e->text + lo, hi - lo);
    if (u) {
        plat_clipboard_set(u);
        free(u);
    }
}

static void ed_cut(HWND hwnd, edit_t *e)
{
    int lo = ed_lo(e), hi = ed_hi(e);
    if (lo == hi || ed_pwmode(hwnd, e) || (ed_style(hwnd) & ES_READONLY))
        return;
    ed_copy(hwnd, e);
    ed_replace(hwnd, e, lo, hi, NULL, 0, 0);
}

static void ed_paste(HWND hwnd, edit_t *e)
{
    char *u;
    WCHAR *w;
    int i, n;
    if (ed_style(hwnd) & ES_READONLY)
        return;
    u = plat_clipboard_get();
    if (!u)
        return;
    w = aw_u2w(u, -1);
    free(u);
    if (!w)
        return;
    n = ctl_wlen(w);
    for (i = 0; i < n; i++) { /* a single-line edit drops line breaks */
        if (w[i] == '\r' || w[i] == '\n') {
            memmove(w + i, w + i + 1, (size_t)(n - i) * sizeof(WCHAR));
            n--;
            i--;
        }
    }
    ed_user_insert(hwnd, e, w, n);
    free(w);
}

/* ------------------------------------------------------------------ painting */

static void ed_draw_text(HWND hwnd, const edit_t *e, HDC hdc, const RECT *tr, int th, COLORREF tc)
{
    UINT style = ed_style(hwnd);
    WCHAR *tmp;
    const WCHAR *d;
    int total, origin, y, lo, hi, showsel;
    RECT clip = *tr;

    y = tr->top + (tr->bottom - tr->top - th) / 2;
    if (y < tr->top)
        y = tr->top;
    d = ed_disp(hwnd, e, &tmp);
    total = ed_width(hdc, d, e->len);
    origin = ed_origin(style, tr, e, total);
    lo = ed_lo(e);
    hi = ed_hi(e);
    showsel = lo != hi && (e->focused || !(style & ES_NOHIDESEL));

    if (e->len == 0 && e->cue && (!e->focused || e->cue_focus)) {
        int cn = ctl_wlen(e->cue);
        int cw = ed_width(hdc, e->cue, cn);
        __aw_gdi32_SetTextColor(hdc, aw_syscolor(COLOR_GRAYTEXT));
        __aw_gdi32_ExtTextOutW(hdc, ed_origin(style, tr, e, cw), y, ETO_CLIPPED, &clip, e->cue, cn, NULL);
    } else if (e->len > 0) {
        __aw_gdi32_SetTextColor(hdc, tc);
        if (!showsel) {
            __aw_gdi32_ExtTextOutW(hdc, origin, y, ETO_CLIPPED, &clip, d, e->len, NULL);
        } else {
            int x1 = origin + ed_width(hdc, d, lo);
            int x2 = origin + ed_width(hdc, d, hi);
            RECT hl;
            HBRUSH br = __aw_user32_GetSysColorBrush(COLOR_HIGHLIGHT);
            if (lo > 0)
                __aw_gdi32_ExtTextOutW(hdc, origin, y, ETO_CLIPPED, &clip, d, lo, NULL);
            if (hi < e->len)
                __aw_gdi32_ExtTextOutW(hdc, x2, y, ETO_CLIPPED, &clip, d + hi, e->len - hi, NULL);
            hl.left = x1 < tr->left ? tr->left : x1;
            hl.right = x2 > tr->right ? tr->right : x2;
            hl.top = tr->top;
            hl.bottom = tr->bottom;
            if (hl.right > hl.left && br)
                __aw_user32_FillRect(hdc, &hl, br);
            __aw_gdi32_SetTextColor(hdc, aw_syscolor(COLOR_HIGHLIGHTTEXT));
            __aw_gdi32_ExtTextOutW(hdc, x1, y, ETO_CLIPPED, &clip, d + lo, hi - lo, NULL);
        }
    }
    if (e->focused) {
        int cx = origin + ed_width(hdc, d, e->caret);
        if (cx >= tr->left && cx <= tr->right) {
            RECT cr;
            HBRUSH cb = __aw_gdi32_CreateSolidBrush(tc);
            cr.left = cx;
            cr.right = cx + 1;
            cr.top = y;
            cr.bottom = y + th;
            if (cr.bottom > tr->bottom)
                cr.bottom = tr->bottom;
            if (cb) {
                __aw_user32_FillRect(hdc, &cr, cb);
                __aw_gdi32_DeleteObject(cb);
            }
        }
    }
    free(tmp);
}

static void ed_draw(HWND hwnd, edit_t *e, HDC hdc, const RECT *rc)
{
    RECT tr;
    HFONT old;
    TEXTMETRICW tm;
    int th = 12;
    COLORREF tc;

    if (ed_border(hwnd))
        ctl_frame(hdc, rc, e->focused ? RGB(0x00, 0x78, 0xD7) : RGB(0xAD, 0xAD, 0xAD));
    old = (HFONT)__aw_gdi32_SelectObject(hdc, ctl_font(hwnd));
    if (__aw_gdi32_GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0)
        th = tm.tmHeight;
    __aw_gdi32_SetBkMode(hdc, 1 /* TRANSPARENT */);
    tc = (COLORREF)__aw_gdi32_GetTextColor(hdc);
    if (!__aw_user32_IsWindowEnabled(hwnd))
        tc = aw_syscolor(COLOR_GRAYTEXT);
    ed_textrect(hwnd, e, &tr);
    ed_fix_scroll(hwnd, e, hdc);
    ed_draw_text(hwnd, e, hdc, &tr, th, tc);
    __aw_gdi32_SelectObject(hdc, old);
}

static int ed_text_msg(HWND hwnd, const edit_t *e)
{
    return ((ed_style(hwnd) & ES_READONLY) || !__aw_user32_IsWindowEnabled(hwnd)) ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT;
}

static void ed_paint(HWND hwnd, edit_t *e)
{
    HDC hdc;
    PAINTSTRUCT ps;
    RECT rc;
    HBRUSH bg;

    if (!ctl_begin(hwnd, ed_text_msg(hwnd, e), &hdc, &ps, &rc, &bg))
        return;
    if (!bg)
        ctl_fill(hdc, &rc, __aw_user32_GetSysColorBrush(COLOR_WINDOW));
    ed_draw(hwnd, e, hdc, &rc);
    ctl_end(hwnd, &ps);
}

/* ------------------------------------------------------------------ keyboard and mouse */

static LRESULT ed_keydown(HWND hwnd, edit_t *e, WPARAM vk, LPARAM lp)
{
    int shift = (__aw_user32_GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int ctrl = (__aw_user32_GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int ro = (ed_style(hwnd) & ES_READONLY) != 0;
    int lo = ed_lo(e), hi = ed_hi(e), i;

    if (e->combo_child && (vk == VK_UP || vk == VK_DOWN || vk == VK_F4 || vk == VK_RETURN || vk == VK_ESCAPE)) {
        HWND p = __aw_user32_GetParent(hwnd);
        if (p)
            __aw_user32_SendMessageW(p, WM_KEYDOWN, vk, lp);
        return 0;
    }
    switch (vk) {
    case VK_LEFT:
        i = ctrl ? ed_word_left(e, e->caret) : ed_prev(e, e->caret);
        if (!shift && lo != hi)
            i = lo;
        ed_move(hwnd, e, i, shift);
        break;
    case VK_RIGHT:
        i = ctrl ? ed_word_right(e, e->caret) : ed_next(e, e->caret);
        if (!shift && lo != hi)
            i = hi;
        ed_move(hwnd, e, i, shift);
        break;
    case VK_HOME:
        ed_move(hwnd, e, 0, shift);
        break;
    case VK_END:
        ed_move(hwnd, e, e->len, shift);
        break;
    case VK_BACK:
        if (ro)
            break;
        if (lo != hi)
            ed_replace(hwnd, e, lo, hi, NULL, 0, 0);
        else if (e->caret > 0) {
            i = ctrl ? ed_word_left(e, e->caret) : ed_prev(e, e->caret);
            ed_replace(hwnd, e, i, e->caret, NULL, 0, 0);
        }
        break;
    case VK_DELETE:
        if (shift) {
            ed_cut(hwnd, e);
            break;
        }
        if (ro)
            break;
        if (lo != hi)
            ed_replace(hwnd, e, lo, hi, NULL, 0, 0);
        else if (e->caret < e->len) {
            i = ctrl ? ed_word_right(e, e->caret) : ed_next(e, e->caret);
            ed_replace(hwnd, e, e->caret, i, NULL, 0, 0);
        }
        break;
    case VK_INSERT:
        if (ctrl)
            ed_copy(hwnd, e);
        else if (shift)
            ed_paste(hwnd, e);
        break;
    default:
        if (!ctrl)
            break;
        if (vk == 'A') {
            e->anchor = 0;
            ed_move(hwnd, e, e->len, 1);
        } else if (vk == 'C')
            ed_copy(hwnd, e);
        else if (vk == 'X')
            ed_cut(hwnd, e);
        else if (vk == 'V')
            ed_paste(hwnd, e);
        break;
    }
    return 0;
}

static void ed_select_word(HWND hwnd, edit_t *e, int idx)
{
    int s = idx, t = idx, cls;
    if (e->len == 0)
        return;
    if (idx >= e->len)
        idx = e->len - 1;
    cls = ed_isword(e->text[idx]);
    s = idx;
    while (s > 0 && ed_isword(e->text[s - 1]) == cls)
        s--;
    t = idx;
    while (t < e->len && ed_isword(e->text[t]) == cls)
        t++;
    e->anchor = s;
    e->caret = t;
    ed_scroll_caret(hwnd, e);
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
}

static void ed_mouse_to(HWND hwnd, edit_t *e, LPARAM lp, int extend)
{
    HFONT f;
    HDC hdc = ed_dc_begin(hwnd, &f);
    int x = (int)(short)LOWORD(lp);
    int idx = ed_index_at(hwnd, e, hdc, x);
    ed_dc_end(hwnd, hdc, f);
    ed_move(hwnd, e, idx, extend);
}

static void ed_set_focus(HWND hwnd, edit_t *e)
{
    if (e->focused)
        return;
    e->focused = 1;
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
    ctl_command(hwnd, EN_SETFOCUS);
}

static void ed_kill_focus(HWND hwnd, edit_t *e)
{
    e->focused = 0;
    e->dragging = 0;
    if (__aw_user32_GetCapture() == hwnd)
        __aw_user32_ReleaseCapture();
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
    ctl_command(hwnd, EN_KILLFOCUS);
}

/* ------------------------------------------------------------------ EM_* messages */

static LRESULT ed_em(HWND hwnd, edit_t *e, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    *handled = 1;
    switch (msg) {
    case EM_GETSEL: {
        int lo = ed_lo(e), hi = ed_hi(e);
        if (wp)
            *(DWORD *)(uintptr_t)wp = (DWORD)lo;
        if (lp)
            *(DWORD *)(uintptr_t)lp = (DWORD)hi;
        return (LRESULT)((DWORD)lo | ((DWORD)hi << 16));
    }
    case EM_SETSEL: {
        int s = (int)(intptr_t)wp, t = (int)(intptr_t)lp;
        if (s == -1) {
            e->anchor = e->caret;
        } else {
            if (t == -1)
                t = e->len;
            if (s < 0)
                s = 0;
            if (t < 0)
                t = 0;
            if (s > e->len)
                s = e->len;
            if (t > e->len)
                t = e->len;
            e->anchor = s;
            e->caret = t;
        }
        ed_scroll_caret(hwnd, e);
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case EM_REPLACESEL:
        ed_replace(hwnd, e, ed_lo(e), ed_hi(e), (const WCHAR *)(uintptr_t)lp,
                   ctl_wlen((const WCHAR *)(uintptr_t)lp), 1);
        return 0;
    case EM_GETRECT: {
        RECT *out = (RECT *)(uintptr_t)lp;
        if (out)
            ed_textrect(hwnd, e, out);
        return 0;
    }
    case EM_SETRECT:
    case EM_SETRECTNP:
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case EM_SCROLLCARET:
        ed_scroll_caret(hwnd, e);
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case EM_LINESCROLL:
    case EM_SCROLL:
    case EM_GETTHUMB:
    case EM_FMTLINES:
    case EM_GETFIRSTVISIBLELINE:
    case EM_LINEFROMCHAR:
    case EM_CANUNDO:
    case EM_UNDO:
    case EM_GETHANDLE:
    case EM_SETHANDLE:
    case EM_SETWORDBREAKPROC:
    case EM_GETWORDBREAKPROC:
    case EM_SETTABSTOPS:
    case EM_SHOWBALLOONTIP:
    case EM_HIDEBALLOONTIP:
        if (msg == EM_SETTABSTOPS)
            return 1;
        return 0;
    case EM_EMPTYUNDOBUFFER:
        return 0;
    case EM_LIMITTEXT:
        e->limit = wp ? (int)wp : ED_LIMIT_DEFAULT;
        return 0;
    case EM_GETLIMITTEXT:
        return e->limit;
    case EM_GETLINECOUNT:
        return 1;
    case EM_LINEINDEX:
        return ((int)(intptr_t)wp == 0 || (int)(intptr_t)wp == -1) ? 0 : -1;
    case EM_LINELENGTH:
        return e->len;
    case EM_GETLINE: {
        WCHAR *buf = (WCHAR *)(uintptr_t)lp;
        int maxc, n;
        if (!buf || (int)(intptr_t)wp != 0)
            return 0;
        maxc = (int)*(WORD *)buf;
        n = e->len < maxc ? e->len : maxc;
        if (n > 0)
            memcpy(buf, e->text, (size_t)n * sizeof(WCHAR));
        return n;
    }
    case EM_GETMODIFY:
        return e->modified;
    case EM_SETMODIFY:
        e->modified = wp ? 1 : 0;
        return 0;
    case EM_SETREADONLY: {
        UINT st = ed_style(hwnd);
        st = wp ? (st | ES_READONLY) : (st & ~(UINT)ES_READONLY);
        __aw_user32_SetWindowLongW(hwnd, GWL_STYLE, (LONG)st);
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 1;
    }
    case EM_SETMARGINS: {
        int l = (int)(short)LOWORD(lp), r = (int)(short)HIWORD(lp);
        if (wp == ED_EC_USEFONTINFO) {
            e->margin_l = e->margin_r = 0;
        } else {
            if (wp & ED_EC_LEFTMARGIN)
                e->margin_l = l;
            if (wp & ED_EC_RIGHTMARGIN)
                e->margin_r = r;
        }
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case EM_GETMARGINS:
        return (LRESULT)(DWORD)((WORD)e->margin_l | ((DWORD)(WORD)e->margin_r << 16));
    case EM_SETPASSWORDCHAR:
        e->pw = (WCHAR)wp;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case EM_GETPASSWORDCHAR:
        return e->pw;
    case EM_POSFROMCHAR: {
        RECT tr;
        HFONT f;
        HDC hdc = ed_dc_begin(hwnd, &f);
        WCHAR *tmp;
        const WCHAR *d;
        int idx = (int)(intptr_t)wp, total, origin, y;
        if (idx < 0)
            idx = 0;
        if (idx > e->len)
            idx = e->len;
        ed_textrect(hwnd, e, &tr);
        d = ed_disp(hwnd, e, &tmp);
        total = ed_width(hdc, d, e->len);
        origin = ed_origin(ed_style(hwnd), &tr, e, total) + ed_width(hdc, d, idx);
        y = tr.top;
        free(tmp);
        ed_dc_end(hwnd, hdc, f);
        return (LRESULT)(DWORD)((WORD)origin | ((DWORD)(WORD)y << 16));
    }
    case EM_CHARFROMPOS: {
        HFONT f;
        HDC hdc = ed_dc_begin(hwnd, &f);
        POINT pt;
        int idx;
        pt.x = (int)(short)LOWORD(lp);
        pt.y = (int)(short)HIWORD(lp);
        idx = ed_index_at(hwnd, e, hdc, pt.x);
        ed_dc_end(hwnd, hdc, f);
        return (LRESULT)(DWORD)((WORD)idx);
    }
    case EM_SETCUEBANNER: {
        const WCHAR *s = (const WCHAR *)(uintptr_t)lp;
        if (s == NULL) {
            free(e->cue);
            e->cue = NULL;
            return 1;
        }
        free(e->cue);
        e->cue = ctl_wdup(s);
        e->cue_focus = wp ? 1 : 0;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return e->cue != NULL;
    }
    case 0x1502 /* EM_GETCUEBANNER */: {
        WCHAR *buf = (WCHAR *)(uintptr_t)wp;
        int max = (int)lp;
        if (!buf || max <= 0)
            return 0;
        buf[0] = 0;
        if (!e->cue)
            return 0;
        {
            int n = ctl_wlen(e->cue);
            if (n > max - 1)
                n = max - 1;
            memcpy(buf, e->cue, (size_t)n * sizeof(WCHAR));
            buf[n] = 0;
        }
        return 1;
    }
    default:
        break;
    }
    *handled = 0;
    return 0;
}

static LRESULT ed_msg(HWND hwnd, edit_t *e, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    *handled = 1;
    switch (msg) {
    case WM_PAINT:
        ed_paint(hwnd, e);
        return 0;
    case WM_PRINTCLIENT: {
        HDC dc = (HDC)(uintptr_t)wp;
        RECT rc;
        HBRUSH bg = (HBRUSH)ctl_ctlcolor(hwnd, ed_text_msg(hwnd, e), dc);
        __aw_user32_GetClientRect(hwnd, &rc);
        ctl_fill(dc, &rc, bg ? bg : __aw_user32_GetSysColorBrush(COLOR_WINDOW));
        ed_draw(hwnd, e, dc, &rc);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_HASSETSEL | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        ed_set_focus(hwnd, e);
        return 0;
    case WM_KILLFOCUS:
        ed_kill_focus(hwnd, e);
        return 0;
    case WM_KEYDOWN:
        return ed_keydown(hwnd, e, wp, lp);
    case WM_CHAR:
        if (wp >= 0x20 && wp != 0x7F) {
            WCHAR c = (WCHAR)wp;
            ed_user_insert(hwnd, e, &c, 1);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!e->focused)
            __aw_user32_SetFocus(hwnd);
        ed_mouse_to(hwnd, e, lp, (wp & MK_SHIFT) != 0);
        e->dragging = 1;
        __aw_user32_SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (e->dragging) {
            if (!(wp & MK_LBUTTON)) {
                e->dragging = 0;
                if (__aw_user32_GetCapture() == hwnd)
                    __aw_user32_ReleaseCapture();
                return 0;
            }
            ed_mouse_to(hwnd, e, lp, 1);
        }
        return 0;
    case WM_LBUTTONUP:
        if (e->dragging) {
            e->dragging = 0;
            if (__aw_user32_GetCapture() == hwnd)
                __aw_user32_ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        HFONT f;
        HDC hdc = ed_dc_begin(hwnd, &f);
        int idx = ed_index_at(hwnd, e, hdc, (int)(short)LOWORD(lp));
        ed_dc_end(hwnd, hdc, f);
        ed_select_word(hwnd, e, idx);
        return 0;
    }
    case WM_GETTEXT: {
        WCHAR *buf = (WCHAR *)(uintptr_t)lp;
        int max = (int)wp, n;
        if (!buf || max <= 0)
            return 0;
        n = e->len < max - 1 ? e->len : max - 1;
        if (n > 0)
            memcpy(buf, e->text, (size_t)n * sizeof(WCHAR));
        buf[n] = 0;
        return n;
    }
    case WM_GETTEXTLENGTH:
        return e->len;
    case WM_SETTEXT: {
        const WCHAR *s = (const WCHAR *)(uintptr_t)lp;
        int n = s ? ctl_wlen(s) : 0;
        ed_set_all(e, s ? s : ctl_empty_w, n);
        e->modified = 0;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        ctl_command(hwnd, EN_UPDATE);
        ctl_command(hwnd, EN_CHANGE);
        return 1;
    }
    case WM_ENABLE:
    case WM_SIZE:
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case ED_MSG_SETCOMBO:
        e->combo_child = 1;
        return 0;
    default:
        break;
    }
    return ed_em(hwnd, e, msg, wp, lp, handled);
}

static void ed_free(edit_t *e)
{
    free(e->text);
    free(e->cue);
    e->text = NULL;
    e->cue = NULL;
}

LRESULT WINAPI aw_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    edit_t *e = ctl_get(hwnd);
    LRESULT res;
    int handled = 0;

    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;

    if (msg == WM_CREATE) {
        ed_createw *cs = (ed_createw *)(uintptr_t)lp;
        e = ctl_alloc(hwnd, sizeof(edit_t));
        if (!e)
            return -1;
        e->limit = ED_LIMIT_DEFAULT;
        e->pw = '*';
        e->margin_l = 2;
        e->margin_r = 2;
        if (cs && cs->lpszName && cs->lpszName[0]) {
            int n = ctl_wlen(cs->lpszName);
            ed_set_all(e, cs->lpszName, n);
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        if (e)
            ed_free(e);
        ctl_free(hwnd);
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!e)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);

    res = ed_msg(hwnd, e, msg, wp, lp, &handled);
    if (handled)
        return res;
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}
