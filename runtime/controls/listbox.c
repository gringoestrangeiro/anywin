/* anywin runtime - ListBox control (the "ListBox" window class).
 *
 * Items are kept as UTF-16 strings with their item data; the LB_* messages follow the Win32 rules:
 * LBS_SORT (case-insensitive order), LBS_NOTIFY (LBN_SELCHANGE / LBN_DBLCLK / LBN_SETFOCUS /
 * LBN_KILLFOCUS through WM_COMMAND), single selection, LBS_MULTIPLESEL (toggle per item) and
 * LBS_EXTENDEDSEL (click, shift and ctrl selection), keyboard (arrows, page, home/end, space,
 * type-ahead), mouse, vertical scrolling through the window's scroll bar (WM_VSCROLL), owner draw
 * (WM_MEASUREITEM / WM_DRAWITEM for LBS_OWNERDRAWFIXED and LBS_OWNERDRAWVARIABLE), tab stops and
 * the background from WM_CTLCOLORLISTBOX.  ANSI callers are converted by the runtime (the string
 * messages are listed in user32_msg.c), so this procedure only ever sees UTF-16.
 *
 * Not implemented: LBS_MULTICOLUMN layout (columns are accepted, the list stays one column),
 * LB_DIR (returns LB_ERR), LBS_NODATA. */
#include "ctl.h"

#define LB_DT_TYPEAHEAD_MS 1000
#define LB_WHEEL_LINES 3
#define LB_TEXT_INSET 2
#define LB_MAX_TABS 32

/* same message set and styles as the Win32 header; ctl.h defines the ones used here */
#ifndef LB_GETLISTBOXINFO
#define LB_GETLISTBOXINFO 0x01B2
#endif
#ifndef LBS_NOSEL
#define LBS_NOSEL 0x4000
#endif
#ifndef LB_SETLOCALE
#define LB_SETLOCALE 0x01A5
#endif

typedef struct {
    WCHAR *text;      /* malloc'd, NUL-terminated (empty string for an empty item) */
    int len;          /* UTF-16 units */
    LPARAM data;
    int sel;          /* selected (multiple / extended selection, or the single selection) */
    int height;       /* owner-draw variable height (0 = use the list's item height) */
} lb_item;

typedef struct {
    HWND hwnd;
    lb_item *items;
    int count, cap;
    int style;
    int cursel;           /* single selection, or -1 */
    int caret;            /* item with the focus rectangle, or -1 */
    int anchor;           /* extended selection anchor, or -1 */
    int top;              /* first visible item */
    int item_h;           /* height of a row without variable heights */
    int focused;
    int noredraw;
    int hext;             /* LB_SETHORIZONTALEXTENT */
    int colw;             /* LB_SETCOLUMNWIDTH */
    int ntabs;
    int tabs[LB_MAX_TABS];
    int locale;
    int ownerdraw;        /* LBS_OWNERDRAWFIXED or LBS_OWNERDRAWVARIABLE */
    DWORD typetime;
    int typelen;
    WCHAR typebuf[64];
    int lbutton_down;
} lb_t;

static int lb_is_multi(const lb_t *l) { return (l->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0; }

static int lb_visible_height(const lb_t *l)
{
    RECT rc;
    __aw_user32_GetClientRect(l->hwnd, &rc);
    return rc.bottom - rc.top;
}

static int lb_height_of(const lb_t *l, int i)
{
    if (l->ownerdraw && l->items[i].height > 0)
        return l->items[i].height;
    return l->item_h > 0 ? l->item_h : 13;
}

/* the first index whose row starts at or below y (client coords) */
static int lb_index_at(const lb_t *l, int y)
{
    int pos = 0;
    for (int i = l->top; i < l->count; i++) {
        int h = lb_height_of(l, i);
        if (y >= pos && y < pos + h)
            return i;
        pos += h;
    }
    return -1;
}

/* rows that fit from top (at least one) */
static int lb_page(const lb_t *l)
{
    int avail = lb_visible_height(l), pos = 0, n = 0;
    for (int i = l->top; i < l->count && pos < avail; i++) {
        pos += lb_height_of(l, i);
        n++;
    }
    return n < 1 ? 1 : n;
}

static void lb_update_scroll(lb_t *l)
{
    SCROLLINFO si;
    if (!(l->style & WS_VSCROLL))
        return;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = l->count > 0 ? l->count - 1 : 0;
    si.nPage = lb_page(l);
    si.nPos = l->top;
    __aw_user32_SetScrollInfo(l->hwnd, SB_VERT, &si, TRUE);
}

static void lb_invalidate(lb_t *l)
{
    if (!l->noredraw)
        __aw_user32_InvalidateRect(l->hwnd, NULL, TRUE);
}

static void lb_set_top(lb_t *l, int top)
{
    int max = l->count > 0 ? l->count - 1 : 0;
    if (top < 0)
        top = 0;
    if (top > max)
        top = max;
    l->top = top;
    lb_update_scroll(l);
    lb_invalidate(l);
}

/* scrolls the minimum needed to show item i */
static void lb_ensure_visible(lb_t *l, int i)
{
    if (i < 0 || i >= l->count)
        return;
    if (i < l->top) {
        lb_set_top(l, i);
        return;
    }
    int avail = lb_visible_height(l);
    /* the rows from top through i must fit; otherwise move top down */
    for (;;) {
        int pos = 0;
        for (int k = l->top; k <= i; k++)
            pos += lb_height_of(l, k);
        if (pos <= avail || l->top >= i)
            break;
        l->top++;
    }
    lb_update_scroll(l);
    lb_invalidate(l);
}

static void lb_notify(lb_t *l, UINT code) { ctl_command(l->hwnd, code); }

/* ------------------------------------------------------------------ items */

static int lb_grow(lb_t *l, int need)
{
    if (need <= l->cap)
        return 1;
    int cap = l->cap ? l->cap * 2 : 16;
    while (cap < need)
        cap *= 2;
    lb_item *n = realloc(l->items, (size_t)cap * sizeof *n);
    if (!n)
        return 0;
    memset(n + l->cap, 0, (size_t)(cap - l->cap) * sizeof *n);
    l->items = n;
    l->cap = cap;
    return 1;
}

static int wcase_cmp(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (WCHAR)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (WCHAR)(y + 32);
        if (x != y)
            return (int)x - (int)y;
        if (!x)
            return 0;
    }
}

static int wcase_prefix(const WCHAR *s, const WCHAR *prefix, int plen)
{
    for (int i = 0; i < plen; i++) {
        WCHAR x = s[i], y = prefix[i];
        if (x >= 'A' && x <= 'Z') x = (WCHAR)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (WCHAR)(y + 32);
        if (x != y)
            return 0;
    }
    return 1;
}

/* the position a new item takes in an LBS_SORT list (after equal strings) */
static int lb_sorted_pos(const lb_t *l, const WCHAR *text)
{
    int i = 0;
    while (i < l->count && wcase_cmp(l->items[i].text, text) <= 0)
        i++;
    return i;
}

static int lb_insert(lb_t *l, int index, const WCHAR *text, LPARAM data)
{
    if (!lb_grow(l, l->count + 1))
        return LB_ERRSPACE;
    if (l->style & LBS_SORT)
        index = lb_sorted_pos(l, text);
    if (index < 0 || index > l->count)
        index = l->count;
    lb_item it;
    memset(&it, 0, sizeof it);
    int len = ctl_wlen(text);
    it.text = (WCHAR *)malloc((size_t)(len + 1) * sizeof(WCHAR));
    if (!it.text)
        return LB_ERRSPACE;
    memcpy(it.text, text, (size_t)len * sizeof(WCHAR));
    it.text[len] = 0;
    it.len = len;
    it.data = data;
    if (l->ownerdraw & LBS_OWNERDRAWVARIABLE) {
        MEASUREITEMSTRUCT mi;
        memset(&mi, 0, sizeof mi);
        mi.CtlType = ODT_LISTBOX;
        mi.CtlID = (UINT)__aw_user32_GetDlgCtrlID(l->hwnd);
        mi.itemID = (UINT)index;
        mi.itemHeight = (UINT)(l->item_h > 0 ? l->item_h : 13);
        mi.itemData = data;
        ctl_send_parent(l->hwnd, WM_MEASUREITEM, mi.CtlID, (LPARAM)&mi);
        it.height = (int)mi.itemHeight;
    }
    memmove(&l->items[index + 1], &l->items[index], (size_t)(l->count - index) * sizeof *l->items);
    l->items[index] = it;
    l->count++;
    /* indexes at or after the insertion point move up */
    if (l->cursel >= index)
        l->cursel++;
    if (l->caret >= index)
        l->caret++;
    if (l->anchor >= index)
        l->anchor++;
    if (l->top > index)
        l->top++;
    lb_update_scroll(l);
    lb_invalidate(l);
    return index;
}

static void lb_free_item(lb_item *it)
{
    free(it->text);
    it->text = NULL;
}

static int lb_delete(lb_t *l, int index)
{
    if (index < 0 || index >= l->count)
        return LB_ERR;
    lb_free_item(&l->items[index]);
    memmove(&l->items[index], &l->items[index + 1], (size_t)(l->count - index - 1) * sizeof *l->items);
    l->count--;
    memset(&l->items[l->count], 0, sizeof l->items[0]);
    if (l->cursel == index)
        l->cursel = -1;
    else if (l->cursel > index)
        l->cursel--;
    if (l->caret > index)
        l->caret--;
    else if (l->caret == index)
        l->caret = l->count > 0 ? (index < l->count ? index : l->count - 1) : -1;
    if (l->anchor > index)
        l->anchor--;
    if (l->top > index)
        l->top--;
    if (l->top >= l->count && l->top > 0)
        l->top = l->count - 1;
    lb_update_scroll(l);
    lb_invalidate(l);
    return l->count;
}

static void lb_reset(lb_t *l)
{
    for (int i = 0; i < l->count; i++)
        lb_free_item(&l->items[i]);
    l->count = 0;
    l->cursel = -1;
    l->caret = -1;
    l->anchor = -1;
    l->top = 0;
    lb_update_scroll(l);
    lb_invalidate(l);
}

/* ------------------------------------------------------------------ selection */

static int lb_count_sel(const lb_t *l)
{
    int n = 0;
    for (int i = 0; i < l->count; i++)
        n += l->items[i].sel ? 1 : 0;
    return n;
}

/* sets the single selection (single list) or the caret item (multiple lists) */
static void lb_set_cursel(lb_t *l, int index)
{
    if (!lb_is_multi(l)) {
        for (int i = 0; i < l->count; i++)
            l->items[i].sel = 0;
        if (index >= 0 && index < l->count)
            l->items[index].sel = 1;
        l->cursel = index >= 0 && index < l->count ? index : -1;
    }
    l->caret = index;
    l->anchor = index;
}

static void lb_select_range(lb_t *l, int from, int to, int on)
{
    if (from > to) {
        int t = from;
        from = to;
        to = t;
    }
    if (from < 0)
        from = 0;
    if (to >= l->count)
        to = l->count - 1;
    for (int i = from; i <= to; i++)
        l->items[i].sel = on;
}

/* extended list: plain move selects one item; shift extends from the anchor; ctrl only moves */
static void lb_extended_move(lb_t *l, int index, int shift, int ctrl)
{
    if (index < 0 || index >= l->count)
        return;
    if (l->anchor < 0 || l->anchor >= l->count)
        l->anchor = index;
    if (shift) {
        lb_select_range(l, 0, l->count - 1, 0);
        lb_select_range(l, l->anchor, index, 1);
    } else if (!ctrl) {
        lb_select_range(l, 0, l->count - 1, 0);
        l->items[index].sel = 1;
        l->anchor = index;
    }
    l->caret = index;
    if (!shift && !ctrl)
        l->cursel = index;
    lb_ensure_visible(l, index);
    lb_invalidate(l);
}

/* the user changed the selection: notify the parent of every change made by input */
static void lb_changed(lb_t *l)
{
    if (l->style & LBS_NOTIFY)
        lb_notify(l, LBN_SELCHANGE);
}

/* ------------------------------------------------------------------ painting */

static COLORREF lb_color(int idx) { return (COLORREF)aw_syscolor(idx); }

static int lb_has_tab(const WCHAR *s)
{
    for (; *s; s++)
        if (*s == '\t')
            return 1;
    return 0;
}

static void lb_draw_item(lb_t *l, HDC hdc, HFONT font, int i, RECT rc, int focused_list)
{
    lb_item *it = &l->items[i];
    int selected = it->sel;
    int focus = focused_list && i == l->caret;
    if (l->ownerdraw) {
        DRAWITEMSTRUCT dis;
        memset(&dis, 0, sizeof dis);
        dis.CtlType = ODT_LISTBOX;
        dis.CtlID = (UINT)__aw_user32_GetDlgCtrlID(l->hwnd);
        dis.itemID = (UINT)i;
        dis.itemAction = ODA_DRAWENTIRE | (focus ? ODA_FOCUS : 0);
        dis.itemState = (selected ? ODS_SELECTED : 0) | (focus ? ODS_FOCUS : 0);
        dis.hwndItem = l->hwnd;
        dis.hDC = hdc;
        dis.rcItem = rc;
        dis.itemData = it->data;
        ctl_send_parent(l->hwnd, WM_DRAWITEM, dis.CtlID, (LPARAM)&dis);
        return;
    }
    HBRUSH bg = selected ? __aw_user32_GetSysColorBrush(COLOR_HIGHLIGHT) : NULL;
    if (bg)
        ctl_fill(hdc, &rc, bg);
    COLORREF tc = selected ? lb_color(COLOR_HIGHLIGHTTEXT) : lb_color(COLOR_WINDOWTEXT);
    RECT tr = rc;
    tr.left += LB_TEXT_INSET;
    tr.right -= LB_TEXT_INSET;
    if (l->ntabs > 0 && lb_has_tab(it->text)) {
        /* tab stops: each field starts at its stop (dialog units were converted in LB_SETTABSTOPS) */
        int x = tr.left, t = 0;
        const WCHAR *p = it->text;
        while (*p) {
            const WCHAR *end = p;
            while (*end && *end != '\t')
                end++;
            RECT fr = tr;
            fr.left = x;
            WCHAR seg[256];
            int n = (int)(end - p);
            if (n > 255) n = 255;
            memcpy(seg, p, (size_t)n * sizeof(WCHAR));
            seg[n] = 0;
            ctl_text(hdc, font, seg, fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, tc);
            if (!*end)
                break;
            p = end + 1;
            while (t < l->ntabs && l->tabs[t] <= x - tr.left)
                t++;
            x = tr.left + (t < l->ntabs ? l->tabs[t] : (x - tr.left) + 60);
            t++;
        }
    } else {
        ctl_text(hdc, font, it->text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, tc);
    }
    if (focus)
        ctl_draw_focus(hdc, &rc);
}

static void lb_paint(HWND hwnd, lb_t *l)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rc, row;
    HBRUSH bg;
    HFONT font;
    int pos = 0;

    hdc = __aw_user32_BeginPaint(hwnd, &ps);
    if (!hdc)
        return;
    __aw_user32_GetClientRect(hwnd, &rc);
    bg = ctl_ctlcolor(hwnd, WM_CTLCOLORLISTBOX, hdc);
    ctl_fill(hdc, &rc, bg ? bg : __aw_user32_GetSysColorBrush(COLOR_WINDOW));
    font = ctl_font(hwnd);
    for (int i = l->top; i < l->count && pos < rc.bottom; i++) {
        int h = lb_height_of(l, i);
        row.left = rc.left;
        row.right = rc.right;
        row.top = pos;
        row.bottom = pos + h;
        lb_draw_item(l, hdc, font, i, row, l->focused);
        pos += h;
    }
    __aw_user32_EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------ input */

static void lb_click(lb_t *l, WPARAM wp, LPARAM lp)
{
    int y = (short)HIWORD(lp);
    int shift = (wp & MK_SHIFT) != 0, ctrl = (wp & MK_CONTROL) != 0;
    int i = lb_index_at(l, y);
    if (i < 0 || i >= l->count)
        return;
    if (l->style & LBS_NOSEL) {
        l->caret = i;
        return;
    }
    if (l->style & LBS_MULTIPLESEL) {
        l->items[i].sel = !l->items[i].sel;
        l->caret = i;
        l->anchor = i;
        if (l->items[i].sel)
            l->cursel = i;
    } else if (l->style & LBS_EXTENDEDSEL) {
        lb_extended_move(l, i, shift, ctrl);
    } else {
        lb_set_cursel(l, i);
        lb_ensure_visible(l, i);
    }
    lb_invalidate(l);
    lb_changed(l);
}

static void lb_move(lb_t *l, int to, int shift, int ctrl)
{
    if (to < 0)
        to = 0;
    if (to >= l->count)
        to = l->count - 1;
    if (l->count == 0)
        return;
    if (lb_is_multi(l) && (l->style & LBS_EXTENDEDSEL)) {
        lb_extended_move(l, to, shift, ctrl);
        lb_changed(l);
        return;
    }
    if (l->style & LBS_MULTIPLESEL) {
        l->caret = to;
        lb_ensure_visible(l, to);
        lb_invalidate(l);
        return;
    }
    lb_set_cursel(l, to);
    lb_ensure_visible(l, to);
    lb_invalidate(l);
    lb_changed(l);
}

static void lb_key(lb_t *l, WPARAM vk)
{
    int shift = (__aw_user32_GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int ctrl = (__aw_user32_GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int cur = l->caret >= 0 ? l->caret : (l->cursel >= 0 ? l->cursel : 0);
    int page = lb_page(l) - 1;
    if (page < 1)
        page = 1;
    switch (vk) {
    case VK_UP:
    case VK_LEFT:
        lb_move(l, cur - 1, shift, ctrl);
        break;
    case VK_DOWN:
    case VK_RIGHT:
        lb_move(l, cur + 1, shift, ctrl);
        break;
    case VK_PRIOR:
        lb_move(l, cur - page, shift, ctrl);
        break;
    case VK_NEXT:
        lb_move(l, cur + page, shift, ctrl);
        break;
    case VK_HOME:
        lb_move(l, 0, shift, ctrl);
        break;
    case VK_END:
        lb_move(l, l->count - 1, shift, ctrl);
        break;
    case VK_SPACE:
        if (lb_is_multi(l) && cur >= 0 && cur < l->count) {
            l->items[cur].sel = !l->items[cur].sel;
            if (l->style & LBS_EXTENDEDSEL)
                l->anchor = cur;
            l->cursel = l->items[cur].sel ? cur : l->cursel;
            lb_invalidate(l);
            lb_changed(l);
        }
        break;
    default:
        break;
    }
}

/* type-ahead: letters typed within a second accumulate into a prefix that selects an item */
static void lb_typeahead(lb_t *l, WCHAR ch)
{
    DWORD now = (DWORD)__aw_kernel32_GetTickCount();
    if (l->count == 0 || ch < 32)
        return;
    if (now - l->typetime > LB_DT_TYPEAHEAD_MS || l->typelen >= 63)
        l->typelen = 0;
    l->typetime = now;
    /* a new burst of typing that starts with one letter cycles through the matches after the
     * current item; a longer prefix is matched from the current item itself */
    int first_key = l->typelen == 0;
    l->typebuf[l->typelen++] = ch;
    l->typebuf[l->typelen] = 0;
    int cur = l->caret >= 0 ? l->caret : l->cursel;
    for (int k = 0; k < l->count; k++) {
        int off = first_key ? k + 1 : k;
        int i = ((cur + off) % l->count + l->count) % l->count;
        if (wcase_prefix(l->items[i].text, l->typebuf, l->typelen)) {
            lb_move(l, i, 0, 0);
            return;
        }
    }
}

/* ------------------------------------------------------------------ messages */

static LRESULT lb_find(lb_t *l, int start, const WCHAR *s, int exact)
{
    if (!s || l->count == 0)
        return LB_ERR;
    int slen = ctl_wlen(s);
    /* the search begins after start and wraps; start -1 begins at the first item */
    for (int k = 1; k <= l->count; k++) {
        int i = start < 0 ? k - 1 : (start + k) % l->count;
        if (exact ? wcase_cmp(l->items[i].text, s) == 0 : wcase_prefix(l->items[i].text, s, slen))
            return i;
    }
    return LB_ERR;
}

static LRESULT lb_getsel_items(lb_t *l, WPARAM max, LPARAM lp)
{
    int n = 0;
    int *out = (int *)lp;
    for (int i = 0; i < l->count && (int)n < (int)max; i++)
        if (l->items[i].sel)
            out[n++] = i;
    return n;
}

static LRESULT lb_tabstops(lb_t *l, WPARAM n, LPARAM lp)
{
    int *in = (int *)lp;
    if (n > LB_MAX_TABS)
        n = LB_MAX_TABS;
    l->ntabs = 0;
    if (n == 0 && !in) {
        l->tabs[0] = 32 * 4; /* default: 32 dialog units */
        l->ntabs = 1;
        return TRUE;
    }
    for (WPARAM i = 0; i < n && in; i++)
        l->tabs[l->ntabs++] = in[i];
    return TRUE;
}

static LRESULT lb_msg(lb_t *l, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    *handled = 1;
    switch (msg) {
    case LB_ADDSTRING:
        if (!lp) return LB_ERR;
        return lb_insert(l, -1, (const WCHAR *)lp, 0);
    case LB_INSERTSTRING:
        if (!lp) return LB_ERR;
        return lb_insert(l, (int)wp, (const WCHAR *)lp, 0);
    case LB_DELETESTRING:
        return lb_delete(l, (int)wp);
    case LB_RESETCONTENT:
        lb_reset(l);
        return 0;
    case LB_GETCOUNT:
        return l->count;
    case LB_SETCOUNT:
        if ((int)wp < l->count) {
            while (l->count > (int)wp)
                lb_delete(l, l->count - 1);
        } else if (lb_grow(l, (int)wp)) {
            while (l->count < (int)wp)
                lb_insert(l, -1, u"", 0);
        }
        return 0;
    case LB_INITSTORAGE:
        lb_grow(l, (int)wp);
        return (LRESULT)wp;
    case LB_GETTEXTLEN:
        if ((int)wp < 0 || (int)wp >= l->count) return LB_ERR;
        return l->items[wp].len;
    case LB_GETTEXT: {
        if ((int)wp < 0 || (int)wp >= l->count || !lp) return LB_ERR;
        lb_item *it = &l->items[wp];
        memcpy((WCHAR *)lp, it->text, (size_t)(it->len + 1) * sizeof(WCHAR));
        return it->len;
    }
    case LB_GETCURSEL:
        if (lb_is_multi(l)) return l->caret >= 0 ? l->caret : LB_ERR;
        return l->cursel >= 0 ? l->cursel : LB_ERR;
    case LB_SETCURSEL:
        if (lb_is_multi(l)) return LB_ERR;
        lb_set_cursel(l, (int)wp);
        if ((int)wp >= 0) lb_ensure_visible(l, (int)wp);
        lb_invalidate(l);
        return (int)wp >= 0 && (int)wp < l->count ? (int)wp : LB_ERR;
    case LB_GETSEL:
        if ((int)wp < 0 || (int)wp >= l->count) return LB_ERR;
        return l->items[wp].sel ? 1 : 0;
    case LB_SETSEL: {
        if (!lb_is_multi(l)) return LB_ERR;
        int on = (int)wp != 0;
        int idx = (int)lp;
        if (idx < 0) {
            lb_select_range(l, 0, l->count - 1, on);
        } else if (idx < l->count) {
            l->items[idx].sel = on;
        } else {
            return LB_ERR;
        }
        lb_invalidate(l);
        lb_changed(l);
        return 0;
    }
    case LB_SELITEMRANGE:
        if (!lb_is_multi(l)) return LB_ERR;
        lb_select_range(l, LOWORD(lp), HIWORD(lp), (int)wp != 0);
        lb_invalidate(l);
        lb_changed(l);
        return 0;
    case LB_SELITEMRANGEEX: {
        if (!lb_is_multi(l)) return LB_ERR;
        int from = (int)wp, to = (int)lp;
        if (from > to) {
            /* deselect when the range is given backwards (the Win32 convention) */
            lb_select_range(l, to, from, 0);
        } else {
            lb_select_range(l, from, to, 1);
        }
        lb_invalidate(l);
        lb_changed(l);
        return 0;
    }
    case LB_GETSELCOUNT:
        if (!lb_is_multi(l)) return LB_ERR;
        return lb_count_sel(l);
    case LB_GETSELITEMS:
        if (!lb_is_multi(l)) return LB_ERR;
        return lb_getsel_items(l, wp, lp);
    case LB_GETTOPINDEX:
        return l->top;
    case LB_SETTOPINDEX:
        lb_set_top(l, (int)wp);
        return 0;
    case LB_FINDSTRING:
        return lb_find(l, (int)wp, (const WCHAR *)lp, 0);
    case LB_FINDSTRINGEXACT:
        return lb_find(l, (int)wp, (const WCHAR *)lp, 1);
    case LB_SELECTSTRING: {
        LRESULT idx = lb_find(l, (int)wp, (const WCHAR *)lp, 0);
        if (idx != LB_ERR) {
            lb_set_cursel(l, (int)idx);
            lb_ensure_visible(l, (int)idx);
            lb_invalidate(l);
        }
        return idx;
    }
    case LB_GETITEMDATA:
        if ((int)wp < 0 || (int)wp >= l->count) return LB_ERR;
        return (LRESULT)l->items[wp].data;
    case LB_SETITEMDATA:
        if ((int)wp < 0 || (int)wp >= l->count) return LB_ERR;
        l->items[wp].data = lp;
        return 0;
    case LB_GETITEMRECT: {
        RECT *out = (RECT *)lp, cr;
        int y = 0;
        if (!out || (int)wp < 0 || (int)wp >= l->count) return LB_ERR;
        if ((int)wp >= l->top) {
            for (int i = l->top; i < (int)wp; i++) y += lb_height_of(l, i);
        } else {
            for (int i = (int)wp; i < l->top; i++) y -= lb_height_of(l, i);
        }
        __aw_user32_GetClientRect(l->hwnd, &cr);
        out->left = cr.left;
        out->right = cr.right;
        out->top = y;
        out->bottom = y + lb_height_of(l, (int)wp);
        return (y >= 0 && y < cr.bottom) ? 0 : 1;
    }
    case LB_ITEMFROMPOINT: {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)}; /* the point is in lParam */
        int i = lb_index_at(l, p.y);
        RECT cr;
        __aw_user32_GetClientRect(l->hwnd, &cr);
        int outside = !(p.x >= cr.left && p.x < cr.right && p.y >= cr.top && p.y < cr.bottom);
        if (i < 0)
            i = l->count > 0 ? l->count - 1 : -1;
        return (LRESULT)(((DWORD)(outside ? 1 : 0) << 16) | (WORD)(i < 0 ? 0xFFFF : i));
    }
    case LB_SETITEMHEIGHT:
        if (l->ownerdraw & LBS_OWNERDRAWVARIABLE) {
            if ((int)wp < 0 || (int)wp >= l->count) return LB_ERR;
            l->items[wp].height = (int)LOWORD(lp);
        } else {
            l->item_h = (int)LOWORD(lp);
        }
        lb_update_scroll(l);
        lb_invalidate(l);
        return 0;
    case LB_GETITEMHEIGHT:
        if (l->ownerdraw & LBS_OWNERDRAWVARIABLE) {
            if ((int)wp < 0 || (int)wp >= l->count) return LB_ERR;
            return lb_height_of(l, (int)wp);
        }
        return lb_height_of(l, 0);
    case LB_SETHORIZONTALEXTENT:
        l->hext = (int)wp;
        return 0;
    case LB_GETHORIZONTALEXTENT:
        return l->hext;
    case LB_SETCOLUMNWIDTH:
        l->colw = (int)wp;
        return 0;
    case LB_SETTABSTOPS:
        return lb_tabstops(l, wp, lp);
    case LB_SETANCHORINDEX:
        l->anchor = (int)wp;
        return 0;
    case LB_GETANCHORINDEX:
        return l->anchor;
    case LB_SETCARETINDEX:
        if ((int)wp >= 0 && (int)wp < l->count) {
            l->caret = (int)wp;
            if (!lp) lb_ensure_visible(l, (int)wp);
            lb_invalidate(l);
        }
        return 0;
    case LB_GETCARETINDEX:
        return l->caret >= 0 ? l->caret : 0;
    case LB_SETLOCALE: {
        LRESULT old = l->locale;
        l->locale = (int)wp;
        return old;
    }
    case LB_GETLOCALE:
        return l->locale;
    case LB_GETLISTBOXINFO:
        return l->count;
    case LB_DIR:
    case LB_ADDFILE:
        return LB_ERR;
    default:
        *handled = 0;
        return 0;
    }
}

static int lb_wnd_style(HWND hwnd)
{
    return (int)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
}

LRESULT WINAPI aw_listbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    lb_t *l = ctl_get(hwnd);
    LRESULT res;

    if (msg == WM_CREATE) {
        l = ctl_alloc(hwnd, sizeof(lb_t));
        if (!l)
            return -1;
        l->hwnd = hwnd;
        l->style = lb_wnd_style(hwnd);
        l->cursel = -1;
        l->caret = -1;
        l->anchor = -1;
        l->item_h = ctl_font_height(hwnd);
        if (l->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))
            l->ownerdraw = l->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE);
        if (l->style & LBS_OWNERDRAWFIXED) {
            MEASUREITEMSTRUCT mi;
            memset(&mi, 0, sizeof mi);
            mi.CtlType = ODT_LISTBOX;
            mi.CtlID = (UINT)__aw_user32_GetDlgCtrlID(hwnd);
            mi.itemHeight = (UINT)l->item_h;
            ctl_send_parent(hwnd, WM_MEASUREITEM, mi.CtlID, (LPARAM)&mi);
            if (mi.itemHeight > 0)
                l->item_h = (int)mi.itemHeight;
        }
        return 0;
    }
    if (!l)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);

    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;

    switch (msg) {
    case WM_NCDESTROY:
        lb_reset(l);
        free(l->items);
        l->items = NULL;
        ctl_free(hwnd);
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (!l->noredraw)
            lb_paint(hwnd, l);
        else
            __aw_user32_ValidateRect(hwnd, NULL);
        return 0;
    case WM_SETREDRAW:
        l->noredraw = wp ? 0 : 1;
        if (!l->noredraw)
            lb_invalidate(l);
        return 0;
    case WM_SIZE:
        lb_update_scroll(l);
        lb_invalidate(l);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
        l->focused = 1;
        if (l->caret < 0)
            l->caret = l->cursel >= 0 ? l->cursel : (l->count > 0 ? l->top : -1);
        lb_invalidate(l);
        if (l->style & LBS_NOTIFY)
            lb_notify(l, LBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        l->focused = 0;
        lb_invalidate(l);
        if (l->style & LBS_NOTIFY)
            lb_notify(l, LBN_KILLFOCUS);
        return 0;
    case WM_LBUTTONDOWN:
        __aw_user32_SetFocus(hwnd);
        l->lbutton_down = 1;
        lb_click(l, wp, lp);
        return 0;
    case WM_LBUTTONUP:
        l->lbutton_down = 0;
        return 0;
    case WM_LBUTTONDBLCLK:
        __aw_user32_SetFocus(hwnd);
        lb_click(l, wp, lp);
        if (l->style & LBS_NOTIFY)
            lb_notify(l, LBN_DBLCLK);
        return 0;
    case WM_KEYDOWN:
        lb_key(l, wp);
        return 0;
    case WM_CHAR:
        lb_typeahead(l, (WCHAR)wp);
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = (short)HIWORD(wp);
        int lines = delta / 120 * LB_WHEEL_LINES;
        lb_set_top(l, l->top - lines);
        return 0;
    }
    case WM_VSCROLL: {
        int code = LOWORD(wp), page = lb_page(l), pos = l->top, max = l->count > 0 ? l->count - 1 : 0;
        switch (code) {
        case SB_LINEUP: pos--; break;
        case SB_LINEDOWN: pos++; break;
        case SB_PAGEUP: pos -= page; break;
        case SB_PAGEDOWN: pos += page; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = max; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: {
            SCROLLINFO si;
            memset(&si, 0, sizeof si);
            si.cbSize = sizeof si;
            si.fMask = SIF_TRACKPOS;
            __aw_user32_GetScrollInfo(hwnd, SB_VERT, &si);
            pos = si.nTrackPos;
            break;
        }
        default: return 0;
        }
        lb_set_top(l, pos);
        return 0;
    }
    default:
        break;
    }
    {
        int handled = 0;
        LRESULT r = lb_msg(l, msg, wp, lp, &handled);
        if (handled)
            return r;
    }
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}

/* comctl32 drag lists (PuTTY uses one for its algorithm lists). MakeDragList accepts a list box as
 * a drag source; this runtime does not reorder dragged items, so no DL_* notifications are sent. */
API(comctl32, BOOL, MakeDragList, (HWND list))
{
    return __aw_user32_IsWindow(list) ? TRUE : FALSE;
}

/* the index of the item at pt (list box client coordinates), -1 outside the list box */
API(comctl32, int, LBItemFromPt, (HWND list, POINT pt, BOOL autoscroll))
{
    LRESULT r;
    (void)autoscroll;
    if (!__aw_user32_IsWindow(list))
        return -1;
    r = __aw_user32_SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
    if (HIWORD(r))
        return -1;
    return (int)LOWORD(r);
}

/* the insertion marker of a drag list is not drawn by this runtime */
API(comctl32, void, DrawInsert, (HWND parent, HWND list, int item))
{
    (void)parent;
    (void)list;
    (void)item;
}
