/* anywin runtime - SysHeader32 (column header; child of SysListView32 in report view).
 *
 * The header owns column text/width/format/order.  SysListView32 reads them through the
 * aw_hdr_* accessors below and changes them with the ordinary HDM_* messages, as in Windows.
 * Notifications (HDN_*) go to the parent window (the listview), which turns item clicks into
 * LVN_COLUMNCLICK and tracks width changes. */
#include "ctl.h"
#include <stdlib.h>
#include <string.h>

#define HDR_MIN_W 4     /* narrowest width a divider drag can produce */
#define HDR_PAD 6       /* text inset on each side */
#define HDR_SLOP_L 3    /* pixels left of a divider that still grab it */
#define HDR_SLOP_R 2    /* pixels right of a divider that still grab it */
#define HDR_ARROW_W 12  /* space reserved for a sort arrow */
#define HDR_RET(v) do { *res = (LRESULT)(v); return 1; } while (0)

#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215
#endif

/* SWP_* and HDLAYOUT/WINDOWPOS are not in ctl.h with Windows layout; define what HDM_LAYOUT needs. */
typedef struct {
    HWND hwnd;
    HWND hwndInsertAfter;
    int x, y, cx, cy;
    UINT flags;
} hdr_windowpos_t;

typedef struct {
    RECT *prc;
    hdr_windowpos_t *pwpos;
} hdr_layout_t;

typedef struct {
    WCHAR *text; /* owned, NULL = empty */
    int cxy;
    int fmt;
    LPARAM lparam;
    int image;
    UINT state;
} hdr_item;

typedef struct {
    hdr_item *items; /* logical index order */
    int n, cap;
    int *order;      /* display position -> logical index */
    int pressed;     /* item under a left-button press, or -1 */
    int div;         /* item whose right-hand divider is being dragged, or -1 */
    int drag_x0;
    int drag_w0;
    HIMAGELIST imagelist;
    int bitmap_margin;
} hdr_t;

static hdr_t *hdr_of(HWND hwnd)
{
    return (hdr_t *)ctl_get(hwnd);
}

static int hdr_height(HWND hwnd)
{
    int h = ctl_font_height(hwnd) + 6;
    return h < 17 ? 17 : h;
}

static WCHAR *hdr_wdup(const WCHAR *s)
{
    size_t n = 0;
    WCHAR *d;
    if (!s)
        return NULL;
    while (s[n])
        n++;
    d = malloc((n + 1) * sizeof(WCHAR));
    if (d)
        memcpy(d, s, (n + 1) * sizeof(WCHAR));
    return d;
}

/* text supplied by the app: UTF-16 for W messages, UTF-8 for A messages (runtime convention) */
static WCHAR *hdr_text_in(const void *p, int uni)
{
    if (!p || (intptr_t)p == -1)
        return NULL;
    return uni ? hdr_wdup((const WCHAR *)p) : ctl_wstr((const char *)p);
}

static int hdr_pos_of(const hdr_t *h, int idx)
{
    for (int p = 0; p < h->n; p++)
        if (h->order[p] == idx)
            return p;
    return -1;
}

static int hdr_x_of_pos(const hdr_t *h, int pos)
{
    int x = 0;
    for (int p = 0; p < pos && p < h->n; p++)
        x += h->items[h->order[p]].cxy;
    return x;
}

static int hdr_total_width(const hdr_t *h)
{
    int x = 0;
    for (int p = 0; p < h->n; p++)
        x += h->items[h->order[p]].cxy;
    return x;
}

/* logical item under x (client coords), or -1 */
static int hdr_at(const hdr_t *h, int x)
{
    int edge = 0;
    for (int p = 0; p < h->n; p++) {
        int i = h->order[p];
        edge += h->items[i].cxy;
        if (x < edge)
            return i;
    }
    return -1;
}

/* logical item whose right divider is at x, or -1 */
static int hdr_divider_at(const hdr_t *h, int x)
{
    int edge = 0;
    for (int p = 0; p < h->n; p++) {
        int i = h->order[p];
        edge += h->items[i].cxy;
        if (x >= edge - HDR_SLOP_L && x <= edge + HDR_SLOP_R)
            return i;
    }
    return -1;
}

static LRESULT hdr_notify(HWND hwnd, int uni, UINT wcode, UINT acode, int idx, int button, const HDITEMW *pit)
{
    NMHEADERW nh;
    HWND parent = __aw_user32_GetParent(hwnd);
    if (!parent)
        return 0;
    memset(&nh, 0, sizeof nh);
    nh.hdr.hwndFrom = hwnd;
    nh.hdr.idFrom = (UINT_PTR)__aw_user32_GetDlgCtrlID(hwnd);
    nh.hdr.code = uni ? wcode : acode;
    nh.iItem = idx;
    nh.iButton = button;
    if (pit) {
        nh.pitem = (HDITEMW *)pit;
    } else {
        static HDITEMW none; /* mask 0: nothing to read */
        nh.pitem = &none;
    }
    return __aw_user32_SendMessageW(parent, WM_NOTIFY, nh.hdr.idFrom, (LPARAM)&nh);
}

/* a width-only HDITEM for divider notifications (pszText stays NULL so A apps can read it) */
static void hdr_width_item(HDITEMW *t, int cxy)
{
    memset(t, 0, sizeof *t);
    t->mask = HDI_WIDTH;
    t->cxy = cxy;
}

/* changes one column width with the veto-able ITEMCHANGING and the ITEMCHANGED notifications */
static int hdr_set_width(HWND hwnd, hdr_t *h, int idx, int cxy)
{
    hdr_item *it = &h->items[idx];
    HDITEMW tmp;
    int uni = ctl_is_unicode_notify(hwnd);
    if (cxy < 0)
        cxy = 0;
    if (cxy == it->cxy)
        return 1;
    hdr_width_item(&tmp, cxy);
    if (hdr_notify(hwnd, uni, HDN_ITEMCHANGINGW, HDN_ITEMCHANGINGA, idx, 0, &tmp))
        return 0;
    it->cxy = cxy;
    hdr_notify(hwnd, uni, HDN_ITEMCHANGEDW, HDN_ITEMCHANGEDA, idx, 0, &tmp);
    __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
    return 1;
}

static int hdr_grow(hdr_t *h)
{
    int nc;
    hdr_item *ni;
    int *no;
    if (h->n < h->cap)
        return 0;
    nc = h->cap ? h->cap * 2 : 8;
    ni = realloc(h->items, (size_t)nc * sizeof *ni);
    if (!ni)
        return -1;
    h->items = ni;
    no = realloc(h->order, (size_t)nc * sizeof *no);
    if (!no)
        return -1;
    h->order = no;
    h->cap = nc;
    return 0;
}

/* copies the fields selected by src->mask (except width and order, which the callers handle) */
static void hdr_apply_fields(hdr_item *it, const HDITEMW *src, int uni)
{
    if (src->mask & HDI_TEXT) {
        WCHAR *t = hdr_text_in(src->pszText, uni);
        free(it->text);
        it->text = t;
    }
    if (src->mask & HDI_FORMAT)
        it->fmt = src->fmt;
    if (src->mask & HDI_LPARAM)
        it->lparam = src->lParam;
    if (src->mask & HDI_IMAGE)
        it->image = src->iImage;
}

static int hdr_insert(HWND hwnd, hdr_t *h, int idx, const HDITEMW *src, int uni)
{
    hdr_item *it;
    int pos;
    if (idx < 0 || idx > h->n)
        idx = h->n;
    if (hdr_grow(h))
        return -1;
    /* logical indices >= idx move up by one */
    for (int p = 0; p < h->n; p++)
        if (h->order[p] >= idx)
            h->order[p]++;
    memmove(&h->items[idx + 1], &h->items[idx], (size_t)(h->n - idx) * sizeof *h->items);
    it = &h->items[idx];
    memset(it, 0, sizeof *it);
    it->cxy = 0;
    if (src->mask & HDI_WIDTH)
        it->cxy = src->cxy < 0 ? 0 : src->cxy;
    hdr_apply_fields(it, src, uni);
    h->n++;
    /* display position: HDI_ORDER if given, else the same as the logical index */
    pos = (src->mask & HDI_ORDER) ? src->iOrder : idx;
    if (pos < 0 || pos > h->n - 1)
        pos = h->n - 1;
    memmove(&h->order[pos + 1], &h->order[pos], (size_t)(h->n - 1 - pos) * sizeof *h->order);
    h->order[pos] = idx;
    __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
    return idx;
}

static int hdr_delete(HWND hwnd, hdr_t *h, int idx)
{
    int p;
    if (idx < 0 || idx >= h->n)
        return 0;
    free(h->items[idx].text);
    memmove(&h->items[idx], &h->items[idx + 1], (size_t)(h->n - idx - 1) * sizeof *h->items);
    p = hdr_pos_of(h, idx);
    if (p >= 0)
        memmove(&h->order[p], &h->order[p + 1], (size_t)(h->n - p - 1) * sizeof *h->order);
    h->n--;
    for (p = 0; p < h->n; p++)
        if (h->order[p] > idx)
            h->order[p]--;
    if (h->pressed == idx)
        h->pressed = -1;
    else if (h->pressed > idx)
        h->pressed--;
    h->div = -1;
    __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
    return 1;
}

/* fills an A or W HDITEM from the item according to dst->mask */
static void hdr_fill_out(const hdr_item *it, int pos, HDITEMW *dst, int uni)
{
    if (dst->mask & HDI_WIDTH)
        dst->cxy = it->cxy;
    if (dst->mask & HDI_FORMAT)
        dst->fmt = it->fmt;
    if (dst->mask & HDI_LPARAM)
        dst->lParam = it->lparam;
    if (dst->mask & HDI_IMAGE)
        dst->iImage = it->image;
    if (dst->mask & HDI_ORDER)
        dst->iOrder = pos;
    if (dst->mask & HDI_TEXT) {
        int cap = dst->cchTextMax;
        if (!dst->pszText || cap <= 0)
            return;
        if (uni) {
            int k = ctl_wlen(it->text ? it->text : ctl_empty_w);
            if (k > cap - 1)
                k = cap - 1;
            if (it->text)
                memcpy(dst->pszText, it->text, (size_t)k * sizeof(WCHAR));
            ((WCHAR *)dst->pszText)[k] = 0;
        } else {
            char *u = aw_w2u(it->text ? it->text : ctl_empty_w, -1);
            size_t len = strlen(u), n = len;
            if (n > (size_t)cap - 1)
                n = (size_t)cap - 1;
            while (n > 0 && n < len && ((unsigned char)u[n] & 0xC0) == 0x80) /* keep UTF-8 whole */
                n--;
            memcpy(dst->pszText, u, n);
            ((char *)dst->pszText)[n] = 0;
            free(u);
        }
    }
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */
static void hdr_arrow(HDC hdc, HBRUSH br, int cx, int cy, int up)
{
    for (int k = 0; k < 5; k++) {
        int half = up ? k : 4 - k;
        RECT r;
        r.left = cx - half;
        r.right = cx + half + 1;
        r.top = cy - 2 + k;
        r.bottom = r.top + 1;
        __aw_user32_FillRect(hdc, &r, br);
    }
}

static void hdr_paint(HWND hwnd, hdr_t *h)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT cr;
    HBRUSH bg, sep, arrow;
    HFONT font = ctl_font(hwnd);
    int x = 0;

    hdc = __aw_user32_BeginPaint(hwnd, &ps);
    if (!hdc)
        return;
    __aw_user32_GetClientRect(hwnd, &cr);
    bg = __aw_gdi32_CreateSolidBrush(__aw_user32_GetSysColor(COLOR_BTNFACE));
    sep = __aw_gdi32_CreateSolidBrush(RGB(0xC8, 0xC8, 0xC8));
    arrow = __aw_gdi32_CreateSolidBrush(RGB(0x40, 0x40, 0x40));
    if (bg)
        __aw_user32_FillRect(hdc, &cr, bg);
    for (int p = 0; p < h->n; p++) {
        const hdr_item *it = &h->items[h->order[p]];
        RECT rc, trc;
        int dpressed = (h->order[p] == h->pressed);
        rc.left = x;
        rc.top = cr.top;
        rc.right = x + it->cxy;
        rc.bottom = cr.bottom;
        if (dpressed && bg) {
            HBRUSH pb = __aw_gdi32_CreateSolidBrush(RGB(0xD8, 0xD8, 0xD8));
            RECT pr = rc;
            if (pb) {
                __aw_user32_FillRect(hdc, &pr, pb);
                __aw_gdi32_DeleteObject(pb);
            }
        }
        if (sep && it->cxy > 0) {
            RECT s;
            s.left = rc.right - 1;
            s.right = rc.right;
            s.top = rc.top + 2;
            s.bottom = rc.bottom - 2;
            __aw_user32_FillRect(hdc, &s, sep);
        }
        trc.left = rc.left + HDR_PAD;
        trc.top = rc.top;
        trc.bottom = rc.bottom;
        trc.right = rc.right - HDR_PAD;
        if (it->fmt & (HDF_SORTUP | HDF_SORTDOWN))
            trc.right -= HDR_ARROW_W;
        if (trc.right > trc.left && it->text)
            ctl_text(hdc, font, it->text, trc,
                     DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (UINT)(it->fmt & HDF_JUSTIFYMASK),
                     __aw_user32_GetSysColor(COLOR_WINDOWTEXT));
        if ((it->fmt & (HDF_SORTUP | HDF_SORTDOWN)) && arrow && it->cxy > HDR_ARROW_W) {
            int cy = (rc.top + rc.bottom) / 2;
            hdr_arrow(hdc, arrow, rc.right - HDR_PAD - 4, cy, (it->fmt & HDF_SORTUP) != 0);
        }
        x += it->cxy;
    }
    if (bg)
        __aw_gdi32_DeleteObject(bg);
    if (sep)
        __aw_gdi32_DeleteObject(sep);
    if (arrow)
        __aw_gdi32_DeleteObject(arrow);
    __aw_user32_EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------ */
/* mouse                                                               */
/* ------------------------------------------------------------------ */
static void hdr_lbutton_down(HWND hwnd, hdr_t *h, int x)
{
    int d = hdr_divider_at(h, x);
    int uni = ctl_is_unicode_notify(hwnd);
    if (d >= 0 && !(__aw_user32_GetWindowLongW(hwnd, GWL_STYLE) & HDS_NOSIZING)) {
        HDITEMW tmp;
        hdr_width_item(&tmp, h->items[d].cxy);
        if (hdr_notify(hwnd, uni, HDN_BEGINTRACKW, HDN_BEGINTRACKA, d, 0, &tmp) == 0) {
            h->div = d;
            h->drag_x0 = x;
            h->drag_w0 = h->items[d].cxy;
            __aw_user32_SetCapture(hwnd);
        }
        return;
    }
    if (d < 0 && (__aw_user32_GetWindowLongW(hwnd, GWL_STYLE) & HDS_BUTTONS)) {
        int i = hdr_at(h, x);
        if (i >= 0) {
            h->pressed = i;
            __aw_user32_SetCapture(hwnd);
            __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        }
    }
}

static void hdr_mouse_move(HWND hwnd, hdr_t *h, int x)
{
    if (h->div >= 0) {
        int nw = h->drag_w0 + (x - h->drag_x0);
        HDITEMW tmp;
        if (nw < HDR_MIN_W)
            nw = HDR_MIN_W;
        if (nw != h->items[h->div].cxy) {
            hdr_width_item(&tmp, nw);
            hdr_notify(hwnd, ctl_is_unicode_notify(hwnd), HDN_TRACKW, HDN_TRACKA, h->div, 0, &tmp);
            hdr_set_width(hwnd, h, h->div, nw);
        }
    }
}

static void hdr_lbutton_up(HWND hwnd, hdr_t *h, int x)
{
    int uni = ctl_is_unicode_notify(hwnd);
    if (h->div >= 0) {
        HDITEMW tmp;
        hdr_width_item(&tmp, h->items[h->div].cxy);
        hdr_notify(hwnd, uni, HDN_ENDTRACKW, HDN_ENDTRACKA, h->div, 0, &tmp);
        h->div = -1;
        __aw_user32_ReleaseCapture();
        return;
    }
    if (h->pressed >= 0) {
        int i = h->pressed;
        h->pressed = -1;
        __aw_user32_ReleaseCapture();
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        if (hdr_at(h, x) == i)
            hdr_notify(hwnd, uni, HDN_ITEMCLICKW, HDN_ITEMCLICKA, i, 0, NULL);
    }
}

/* returns 1 when the message was handled and stores its result in *res */
static int hdr_msg(HWND hwnd, hdr_t *h, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res)
{
    switch (msg) {
    case WM_PAINT:
        hdr_paint(hwnd, h);
        HDR_RET(0);
    case WM_ERASEBKGND:
        HDR_RET(1);
    case WM_LBUTTONDOWN:
        hdr_lbutton_down(hwnd, h, (short)LOWORD(lp));
        HDR_RET(0);
    case WM_MOUSEMOVE:
        hdr_mouse_move(hwnd, h, (short)LOWORD(lp));
        HDR_RET(0);
    case WM_LBUTTONUP:
        hdr_lbutton_up(hwnd, h, (short)LOWORD(lp));
        HDR_RET(0);
    case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(lp);
        int d = hdr_divider_at(h, x);
        if (d >= 0)
            hdr_notify(hwnd, ctl_is_unicode_notify(hwnd), HDN_DIVIDERDBLCLICKW, HDN_DIVIDERDBLCLICKA, d, 0, NULL);
        else if (hdr_at(h, x) >= 0)
            hdr_notify(hwnd, ctl_is_unicode_notify(hwnd), HDN_ITEMDBLCLICKW, HDN_ITEMDBLCLICKA, hdr_at(h, x), 0, NULL);
        HDR_RET(0);
    }
    case WM_RBUTTONUP: {
        int i = hdr_at(h, (short)LOWORD(lp));
        if (i >= 0)
            hdr_notify(hwnd, ctl_is_unicode_notify(hwnd), HDN_ITEMCLICKW, HDN_ITEMCLICKA, i, 1, NULL);
        HDR_RET(0);
    }
    case WM_CAPTURECHANGED:
        if (h->div >= 0)
            h->div = -1;
        h->pressed = -1;
        HDR_RET(0);

    case HDM_GETITEMCOUNT:
        HDR_RET(h->n);

    case HDM_INSERTITEMA:
    case HDM_INSERTITEMW: {
        int uni = (msg == HDM_INSERTITEMW);
        HDR_RET(hdr_insert(hwnd, h, (int)wp, (const HDITEMW *)lp, uni));
    }
    case HDM_DELETEITEM:
        HDR_RET(hdr_delete(hwnd, h, (int)wp));

    case HDM_GETITEMA:
    case HDM_GETITEMW: {
        HDITEMW *dst = (HDITEMW *)lp;
        int i = (int)wp;
        if (!dst || i < 0 || i >= h->n)
            HDR_RET(0);
        hdr_fill_out(&h->items[i], hdr_pos_of(h, i), dst, msg == HDM_GETITEMW);
        HDR_RET(1);
    }
    case HDM_SETITEMA:
    case HDM_SETITEMW: {
        int uni = (msg == HDM_SETITEMW);
        const HDITEMW *src = (const HDITEMW *)lp;
        int i = (int)wp;
        if (!src || i < 0 || i >= h->n)
            HDR_RET(0);
        if (src->mask & HDI_WIDTH) {
            if (!hdr_set_width(hwnd, h, i, src->cxy))
                HDR_RET(0);
        }
        hdr_apply_fields(&h->items[i], src, uni);
        if (src->mask & HDI_ORDER) {
            int from = hdr_pos_of(h, i);
            int to = src->iOrder;
            if (to < 0)
                to = 0;
            if (to > h->n - 1)
                to = h->n - 1;
            if (from >= 0 && from != to) {
                int v = h->order[from];
                if (from < to)
                    memmove(&h->order[from], &h->order[from + 1], (size_t)(to - from) * sizeof *h->order);
                else
                    memmove(&h->order[to + 1], &h->order[to], (size_t)(from - to) * sizeof *h->order);
                h->order[to] = v;
            }
        }
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        HDR_RET(1);
    }
    case HDM_GETITEMRECT: {
        RECT *rc = (RECT *)lp;
        int i = (int)wp;
        int p;
        if (!rc || i < 0 || i >= h->n)
            HDR_RET(0);
        p = hdr_pos_of(h, i);
        rc->left = hdr_x_of_pos(h, p);
        rc->right = rc->left + h->items[i].cxy;
        rc->top = 0;
        rc->bottom = hdr_height(hwnd);
        HDR_RET(1);
    }
    case HDM_HITTEST: {
        HDHITTESTINFO *ht = (HDHITTESTINFO *)lp;
        int x, i, flags = 0;
        if (!ht)
            HDR_RET(-1);
        x = ht->pt.x;
        if (ht->pt.y < 0 || ht->pt.y >= hdr_height(hwnd)) {
            ht->flags = HHT_NOWHERE;
            ht->iItem = -1;
            HDR_RET(-1);
        }
        i = hdr_at(h, x);
        if (i < 0) {
            flags = HHT_NOWHERE;
        } else {
            flags = HHT_ONHEADER;
            if (hdr_divider_at(h, x) >= 0)
                flags = HHT_ONDIVIDER;
        }
        ht->flags = (UINT)flags;
        ht->iItem = i;
        HDR_RET(i);
    }
    case HDM_ORDERTOINDEX: {
        int pos = (int)wp;
        if (pos < 0 || pos >= h->n)
            HDR_RET(-1);
        HDR_RET(h->order[pos]);
    }
    case HDM_GETORDERARRAY: {
        int *out = (int *)lp;
        if (!out || (int)wp < h->n)
            HDR_RET(0);
        memcpy(out, h->order, (size_t)h->n * sizeof *out);
        HDR_RET(1);
    }
    case HDM_SETORDERARRAY: {
        const int *in = (const int *)lp;
        char *seen;
        int ok = 1;
        if (!in || (int)wp != h->n)
            HDR_RET(0);
        seen = calloc((size_t)h->n + 1, 1);
        if (!seen)
            HDR_RET(0);
        for (int p = 0; p < h->n && ok; p++) {
            if (in[p] < 0 || in[p] >= h->n || seen[in[p]])
                ok = 0;
            else
                seen[in[p]] = 1;
        }
        free(seen);
        if (!ok)
            HDR_RET(0);
        memcpy(h->order, in, (size_t)h->n * sizeof *h->order);
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        HDR_RET(1);
    }
    case HDM_LAYOUT: {
        hdr_layout_t *lay = (hdr_layout_t *)lp;
        if (!lay || !lay->prc)
            HDR_RET(0);
        {
            int hh = hdr_height(hwnd);
            if (lay->pwpos) {
                lay->pwpos->x = lay->prc->left;
                lay->pwpos->y = lay->prc->top;
                lay->pwpos->cx = lay->prc->right - lay->prc->left;
                lay->pwpos->cy = hh;
                lay->pwpos->flags = SWP_NOZORDER | SWP_SHOWWINDOW;
            }
            lay->prc->top += hh;
        }
        HDR_RET(1);
    }
    case HDM_GETIMAGELIST:
        HDR_RET((LRESULT)(intptr_t)h->imagelist);
    case HDM_SETIMAGELIST: {
        HIMAGELIST old = h->imagelist;
        h->imagelist = (HIMAGELIST)lp;
        HDR_RET((LRESULT)(intptr_t)old);
    }
    case HDM_GETBITMAPMARGIN:
        HDR_RET(h->bitmap_margin);
    case HDM_SETHOTDIVIDER:
    case HDM_CREATEDRAGIMAGE:
        HDR_RET(0);

    case WM_NCDESTROY:
        for (int i = 0; i < h->n; i++)
            free(h->items[i].text);
        free(h->items);
        free(h->order);
        h->items = NULL;
        h->order = NULL;
        h->n = h->cap = 0;
        HDR_RET(0);
    default:
        break;
    }
    HDR_RET(0);
}

LRESULT WINAPI aw_header_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    hdr_t *h = hdr_of(hwnd);
    LRESULT res;

    if (msg == WM_CREATE) {
        h = ctl_alloc(hwnd, sizeof(hdr_t));
        if (!h)
            return -1;
        h->div = -1;
        h->pressed = -1;
        h->bitmap_margin = 4;
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        if (h) {
            LRESULT unused;
            hdr_msg(hwnd, h, msg, wp, lp, &unused); /* frees the items */
        }
        ctl_free(hwnd);                    /* frees the state block itself */
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!h)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;
    if (msg == WM_GETDLGCODE)
        return 0;
    if (hdr_msg(hwnd, h, msg, wp, lp, &res))
        return res;
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* accessors used by SysListView32 (same package)                      */
/* ------------------------------------------------------------------ */
int aw_hdr_count(HWND hwnd)
{
    hdr_t *h = hdr_of(hwnd);
    return h ? h->n : 0;
}

/* logical item index at display position pos, or -1 */
int aw_hdr_index_at_pos(HWND hwnd, int pos)
{
    hdr_t *h = hdr_of(hwnd);
    if (!h || pos < 0 || pos >= h->n)
        return -1;
    return h->order[pos];
}

int aw_hdr_width(HWND hwnd, int idx)
{
    hdr_t *h = hdr_of(hwnd);
    if (!h || idx < 0 || idx >= h->n)
        return 0;
    return h->items[idx].cxy;
}

int aw_hdr_fmt(HWND hwnd, int idx)
{
    hdr_t *h = hdr_of(hwnd);
    if (!h || idx < 0 || idx >= h->n)
        return 0;
    return h->items[idx].fmt;
}

/* borrowed UTF-16 text (valid until the next header change), or NULL */
const WCHAR *aw_hdr_text(HWND hwnd, int idx)
{
    hdr_t *h = hdr_of(hwnd);
    if (!h || idx < 0 || idx >= h->n)
        return NULL;
    return h->items[idx].text;
}

int aw_hdr_height(HWND hwnd)
{
    return hdr_height(hwnd);
}

int aw_hdr_total_width(HWND hwnd)
{
    hdr_t *h = hdr_of(hwnd);
    return h ? hdr_total_width(h) : 0;
}
