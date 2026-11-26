/* anywin runtime - SysTreeView32 (tree view common control).
 *
 * Model: a parent/child/sibling tree of tv_node.  A synthetic root (tv->root, level -1) holds the
 * top-level items.  Rows are the visible nodes in pre-order (children of collapsed nodes are
 * skipped); the row list is rebuilt lazily when the tree or an expansion state changes.
 *
 * View: one row per visible node, dotted gutter lines (TVS_HASLINES / TVS_LINESATROOT), 9x9
 * +/- buttons (TVS_HASBUTTONS), selection highlight (TVS_SHOWSELALWAYS / TVS_FULLROWSELECT),
 * focus rect, vertical scrolling in rows and horizontal scrolling in pixels.  Geometry follows
 * comctl32's classic layout: a node at level L has its icon column at gutter + L*indent and its
 * button centred half an indent to the left of that column.
 *
 * Notifications go to the parent with WM_NOTIFY, in the A or W variant the parent asked for
 * (WM_NOTIFYFORMAT, see ctl_is_unicode_notify).  No image list is implemented: TVM_SET/GETIMAGELIST
 * only store the handle and images are never drawn.  Label editing is not implemented
 * (TVM_EDITLABEL returns NULL).
 */
#include "ctl.h"
#include <stdlib.h>
#include <string.h>

#define TV_TEXT_MAX 260
#define TV_CB ((WCHAR *)(intptr_t)-1) /* LPSTR_TEXTCALLBACK: text comes from TVN_GETDISPINFO */
#define TV_SB_W 17                    /* scroll bar thickness (listview uses the same) */
#define TV_DEF_INDENT 19
#define TV_WHEEL_ROWS 3
#define TV_HSTEP 16 /* horizontal line scroll, pixels */
#define TVHNDL(p) ((HANDLE)(void *)(p))
#define TVNODE(h) ((tv_node *)(void *)(h))

#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif
#ifndef TVSIL_NORMAL
#define TVSIL_NORMAL 0
#define TVSIL_STATE 2
#endif
#ifndef TVC_UNKNOWN
#define TVC_UNKNOWN 0
#define TVC_BYMOUSE 1
#define TVC_BYKEYBOARD 2
#endif
#ifndef I_CHILDRENCALLBACK
#define I_CHILDRENCALLBACK (-1)
#endif
#ifndef VK_UP
#define VK_UP 0x26
#define VK_DOWN 0x28
#define VK_RIGHT 0x27
#define VK_END 0x23
#endif
#ifndef VK_ADD
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_MULTIPLY 0x6A
#endif
#ifndef SB_HORZ
#define SB_HORZ 0
#define SB_VERT 1
#endif
#ifndef SB_LINEDOWN
#define SB_LINEDOWN 1
#define SB_PAGEUP 2
#define SB_PAGEDOWN 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_BOTTOM 7
#endif

/* ------------------------------------------------------------------ */
/* structures                                                          */
/* ------------------------------------------------------------------ */
typedef struct tv_node {
    struct tv_node *parent, *first, *last, *next, *prev;
    WCHAR *text; /* owned; NULL = empty; TV_CB = callback */
    UINT state;  /* TVIS_* bits; TVIS_SELECTED is kept in step with tv->caret */
    int image, selimage;
    int cchildren; /* TVITEM cChildren as the application set it, or I_CHILDRENCALLBACK */
    LPARAM param;
    int level; /* 0 for top-level items; the synthetic root is -1 */
} tv_node;

typedef int(AWCALL *tv_user_cmp)(LPARAM, LPARAM, LPARAM); /* PFNTVCOMPARE */

typedef struct {
    HANDLE hParent;
    tv_user_cmp lpfnCompare;
    LPARAM lParam;
} tv_sortcb_t; /* TVSORTCB */

typedef struct {
    tv_user_cmp fn;
    LPARAM lp;
} tv_cbctx;

typedef struct {
    NMHDR hdr;
    TVITEMW item; /* NMTVDISPINFOW; the A variant has the same layout */
} tv_dispinfo_t;

typedef struct {
    RECT cli; /* client area without the scroll bars that are shown */
    int rowh, page, nvis, content_w, vbar, hbar;
} tv_geo_t;

struct tv {
    HWND hwnd;
    DWORD style;
    tv_node root;
    tv_node *caret; /* TVGN_CARET (the selected item) */
    tv_node *drop;  /* TVGN_DROPHILITE */
    int count;
    tv_node **vis;
    int nvis, capvis;
    int dirty; /* row list needs a rebuild */
    int cw;    /* cached content width in pixels, -1 = stale */
    int top;      /* first visible row */
    int scroll_x; /* horizontal offset, pixels */
    int itemh;    /* 0 = derived from the font */
    int indent;
    int has_focus, mdown, sbv, sbh;
    COLORREF clr_bk, clr_text;
    HANDLE imagelist[2];
    HWND tooltips;
    int scrolltime;
};

/* ------------------------------------------------------------------ */
/* strings and notifications                                           */
/* ------------------------------------------------------------------ */
static int tv_uni(struct tv *tv)
{
    return ctl_is_unicode_notify(tv->hwnd);
}

static LRESULT tv_notify(struct tv *tv, NMHDR *n, UINT code)
{
    HWND parent = __aw_user32_GetParent(tv->hwnd);
    if (!parent)
        return 0;
    n->hwndFrom = tv->hwnd;
    n->idFrom = (UINT_PTR)__aw_user32_GetDlgCtrlID(tv->hwnd);
    n->code = code;
    return __aw_user32_SendMessageW(parent, WM_NOTIFY, n->idFrom, (LPARAM)n);
}

/* TVN_GETDISPINFO for one node: fills buf for TVIF_TEXT, returns the cChildren it reports */
static int tv_dispinfo(struct tv *tv, tv_node *n, UINT mask, WCHAR *buf, int cap)
{
    tv_dispinfo_t di;
    char *ab = NULL;
    int uni = tv_uni(tv);
    memset(&di, 0, sizeof di);
    di.item.mask = mask | TVIF_HANDLE | TVIF_PARAM;
    di.item.hItem = TVHNDL(n);
    di.item.lParam = n->param;
    di.item.cChildren = n->cchildren;
    if (mask & TVIF_TEXT) {
        buf[0] = 0;
        if (uni) {
            di.item.pszText = buf;
        } else {
            ab = calloc((size_t)cap * 3 + 4, 1);
            if (ab)
                di.item.pszText = (WCHAR *)(void *)ab;
        }
        di.item.cchTextMax = cap;
    }
    tv_notify(tv, &di.hdr, uni ? TVN_GETDISPINFOW : TVN_GETDISPINFOA);
    if (mask & TVIF_TEXT) {
        if (!uni && ab)
            aw_u2w_buf(ab, -1, buf, cap);
        buf[cap - 1] = 0;
    }
    free(ab);
    return di.item.cChildren;
}

/* text of a node into buf (cap characters including the terminator) */
static void tv_text(struct tv *tv, tv_node *n, WCHAR *buf, int cap)
{
    int i;
    if (n->text == TV_CB) {
        tv_dispinfo(tv, n, TVIF_TEXT, buf, cap);
        return;
    }
    if (!n->text) {
        buf[0] = 0;
        return;
    }
    for (i = 0; i < cap - 1 && n->text[i]; i++)
        buf[i] = n->text[i];
    buf[i] = 0;
}

/* text from an application TVITEM (A or W); owned WCHAR copy, or TV_CB */
static WCHAR *tv_text_in(const void *p, int uni)
{
    if (!p)
        return NULL;
    if (p == (const void *)(intptr_t)-1)
        return TV_CB;
    return uni ? ctl_wdup((const WCHAR *)p) : ctl_wstr((const char *)p);
}

/* fills a TVITEMW (layout shared with TVITEMA) for a notification; text goes into wbuf or abuf */
static void tv_fill_item(struct tv *tv, TVITEMW *ti, tv_node *n, int uni, WCHAR *wbuf, char *abuf)
{
    tv_text(tv, n, wbuf, TV_TEXT_MAX);
    ti->mask = TVIF_HANDLE | TVIF_STATE | TVIF_PARAM | TVIF_CHILDREN | TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    ti->hItem = TVHNDL(n);
    ti->state = n->state;
    ti->stateMask = 0xFFFFFFFFu;
    ti->lParam = n->param;
    ti->cChildren = n->first ? 1 : n->cchildren;
    ti->iImage = n->image;
    ti->iSelectedImage = n->selimage;
    if (uni) {
        ti->pszText = wbuf;
    } else {
        aw_w2u_term(wbuf, -1, abuf, TV_TEXT_MAX * 3);
        ti->pszText = (WCHAR *)(void *)abuf;
    }
    ti->cchTextMax = TV_TEXT_MAX;
}

/* TVN_SELCHANGING/ED, TVN_ITEMEXPANDING/ED and friends: NMTREEVIEW with old and new items */
static LRESULT tv_fire_tree(struct tv *tv, UINT codeA, UINT codeW, UINT action, tv_node *oldn, tv_node *newn)
{
    NMTREEVIEWW nm;
    WCHAR wo[TV_TEXT_MAX], wn[TV_TEXT_MAX];
    char ao[TV_TEXT_MAX * 3], an[TV_TEXT_MAX * 3];
    int uni = tv_uni(tv);
    memset(&nm, 0, sizeof nm);
    nm.action = action;
    if (oldn)
        tv_fill_item(tv, &nm.itemOld, oldn, uni, wo, ao);
    if (newn)
        tv_fill_item(tv, &nm.itemNew, newn, uni, wn, an);
    return tv_notify(tv, &nm.hdr, uni ? codeW : codeA);
}

static void tv_fire_nm(struct tv *tv, UINT code)
{
    NMHDR nh;
    memset(&nh, 0, sizeof nh);
    tv_notify(tv, &nh, code);
}

/* ------------------------------------------------------------------ */
/* tree structure                                                      */
/* ------------------------------------------------------------------ */
static void tv_touch(struct tv *tv)
{
    tv->dirty = 1;
    tv->cw = -1;
}

static void tv_changed(struct tv *tv)
{
    tv_touch(tv);
    __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
}

static void tv_unlink(tv_node *n)
{
    tv_node *p = n->parent;
    if (!p)
        return;
    if (n->prev)
        n->prev->next = n->next;
    else
        p->first = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        p->last = n->prev;
    n->parent = n->next = n->prev = NULL;
}

/* links n as the child of p after sibling ref (NULL = first child) */
static void tv_link_after(tv_node *p, tv_node *ref, tv_node *n)
{
    n->parent = p;
    if (!ref) {
        n->prev = NULL;
        n->next = p->first;
        if (p->first)
            p->first->prev = n;
        else
            p->last = n;
        p->first = n;
    } else {
        n->prev = ref;
        n->next = ref->next;
        if (ref->next)
            ref->next->prev = n;
        else
            p->last = n;
        ref->next = n;
    }
}

static int tv_in_subtree(tv_node *n, tv_node *anc)
{
    for (; n; n = n->parent)
        if (n == anc)
            return 1;
    return 0;
}

static void tv_free_tree(struct tv *tv, tv_node *n);

static void tv_clear_children(struct tv *tv, tv_node *p)
{
    while (p->first) {
        tv_node *c = p->first;
        tv_unlink(c);
        tv_free_tree(tv, c);
    }
    tv_touch(tv);
}

/* frees n and its subtree; TVN_DELETEITEM is sent for every node, children first */
static void tv_free_tree(struct tv *tv, tv_node *n)
{
    NMTREEVIEWW nm;
    while (n->first) {
        tv_node *c = n->first;
        tv_unlink(c);
        tv_free_tree(tv, c);
    }
    memset(&nm, 0, sizeof nm);
    nm.itemOld.mask = TVIF_HANDLE | TVIF_PARAM;
    nm.itemOld.hItem = TVHNDL(n);
    nm.itemOld.lParam = n->param;
    tv_notify(tv, &nm.hdr, tv_uni(tv) ? TVN_DELETEITEMW : TVN_DELETEITEMA);
    if (tv->caret == n)
        tv->caret = NULL;
    if (tv->drop == n)
        tv->drop = NULL;
    if (n->text && n->text != TV_CB)
        free(n->text);
    free(n);
    tv->count--;
}

static void tv_delete_node(struct tv *tv, tv_node *n)
{
    if (n == &tv->root) {
        tv_clear_children(tv, &tv->root);
    } else {
        tv_unlink(n);
        tv_free_tree(tv, n);
    }
    tv_changed(tv);
}

static int tv_haschild(struct tv *tv, tv_node *n)
{
    if (n->first)
        return 1;
    if (n->cchildren == I_CHILDRENCALLBACK)
        return tv_dispinfo(tv, n, TVIF_CHILDREN, NULL, 0) != 0;
    return n->cchildren > 0;
}

static int tv_cmp_ci(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (WCHAR)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (WCHAR)(y + 32);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
        a++;
        b++;
    }
}

static int tv_cmp_text(struct tv *tv, tv_node *a, tv_node *b, void *ctx)
{
    WCHAR ba[TV_TEXT_MAX], bb[TV_TEXT_MAX];
    (void)ctx;
    tv_text(tv, a, ba, TV_TEXT_MAX);
    tv_text(tv, b, bb, TV_TEXT_MAX);
    return tv_cmp_ci(ba, bb);
}

static int tv_cmp_user(struct tv *tv, tv_node *a, tv_node *b, void *ctx)
{
    tv_cbctx *c = (tv_cbctx *)ctx;
    (void)tv;
    return c->fn(a->param, b->param, c->lp);
}

/* stable insertion sort of one child list */
typedef int (*tv_cmp_t)(struct tv *, tv_node *, tv_node *, void *);

static void tv_sort_children(struct tv *tv, tv_node *p, tv_cmp_t cmp, void *ctx)
{
    tv_node *c;
    tv_node **arr;
    int m = 0, i, j;
    for (c = p->first; c; c = c->next)
        m++;
    if (m < 2)
        return;
    arr = malloc((size_t)m * sizeof *arr);
    if (!arr)
        return;
    for (i = 0, c = p->first; c; c = c->next, i++)
        arr[i] = c;
    for (i = 1; i < m; i++) {
        tv_node *x = arr[i];
        for (j = i - 1; j >= 0 && cmp(tv, arr[j], x, ctx) > 0; j--)
            arr[j + 1] = arr[j];
        arr[j + 1] = x;
    }
    p->first = arr[0];
    p->last = arr[m - 1];
    arr[0]->prev = NULL;
    for (i = 0; i < m; i++) {
        arr[i]->parent = p;
        arr[i]->prev = i ? arr[i - 1] : NULL;
        arr[i]->next = i + 1 < m ? arr[i + 1] : NULL;
    }
    free(arr);
    tv_touch(tv);
}

static void tv_sort_rec(struct tv *tv, tv_node *p, int recurse)
{
    tv_node *c;
    tv_sort_children(tv, p, tv_cmp_text, NULL);
    if (recurse)
        for (c = p->first; c; c = c->next)
            tv_sort_rec(tv, c, 1);
}

/* ------------------------------------------------------------------ */
/* rows and geometry                                                   */
/* ------------------------------------------------------------------ */
static void tv_collect(struct tv *tv, tv_node *p)
{
    tv_node *c;
    for (c = p->first; c; c = c->next) {
        if (tv->nvis == tv->capvis) {
            int cap = tv->capvis ? tv->capvis * 2 : 64;
            tv_node **nv = realloc(tv->vis, (size_t)cap * sizeof *nv);
            if (!nv)
                return;
            tv->vis = nv;
            tv->capvis = cap;
        }
        tv->vis[tv->nvis++] = c;
        if (c->state & TVIS_EXPANDED)
            tv_collect(tv, c);
    }
}

static void tv_rows(struct tv *tv)
{
    if (!tv->dirty)
        return;
    tv->nvis = 0;
    tv_collect(tv, &tv->root);
    tv->dirty = 0;
}

static int tv_row_of(struct tv *tv, tv_node *n)
{
    int i;
    if (!n)
        return -1;
    tv_rows(tv);
    for (i = 0; i < tv->nvis; i++)
        if (tv->vis[i] == n)
            return i;
    return -1;
}

static int tv_rowh(struct tv *tv)
{
    int h;
    if (tv->itemh > 0)
        return tv->itemh;
    h = ctl_font_height(tv->hwnd) + 3;
    return h < 16 ? 16 : h;
}

static int tv_gutter(struct tv *tv)
{
    return (tv->style & (TVS_HASBUTTONS | TVS_HASLINES)) ? tv->indent : 0;
}

/* icon column of a level (content coordinates) */
static int tv_cx(struct tv *tv, int level)
{
    return tv_gutter(tv) + level * tv->indent;
}

/* x of the connector column, which is also the centre of the +/- button */
static int tv_vcol(struct tv *tv, int level)
{
    return tv_cx(tv, level) - tv->indent / 2;
}

/* width of the label of n; buf receives the text */
static int tv_label(struct tv *tv, HDC hdc, tv_node *n, WCHAR *buf)
{
    SIZE sz;
    tv_text(tv, n, buf, TV_TEXT_MAX);
    ctl_get_text_size(hdc, ctl_font(tv->hwnd), buf, -1, &sz);
    return sz.cx;
}

static int tv_content_w(struct tv *tv, HDC hdc)
{
    int i, w = 0;
    WCHAR buf[TV_TEXT_MAX];
    for (i = 0; i < tv->nvis; i++) {
        tv_node *n = tv->vis[i];
        int r = tv_cx(tv, n->level) + 2 + tv_label(tv, hdc, n, buf) + 3;
        if (r > w)
            w = r;
    }
    return w;
}

/* computes the geometry and clamps top and scroll_x to it; no window side effects */
static void tv_geo(struct tv *tv, HDC hdc, tv_geo_t *g)
{
    RECT full;
    int rowh, vn = 0, hn = 0, k, maxtop, maxsx;
    tv_rows(tv);
    __aw_user32_GetClientRect(tv->hwnd, &full);
    rowh = tv_rowh(tv);
    if (tv->cw < 0)
        tv->cw = tv_content_w(tv, hdc);
    for (k = 0; k < 2; k++) {
        vn = !(tv->style & TVS_NOSCROLL) && tv->nvis * rowh > full.bottom - (hn ? TV_SB_W : 0);
        hn = !(tv->style & (TVS_NOSCROLL | TVS_NOHSCROLL)) && tv->cw > full.right - (vn ? TV_SB_W : 0);
    }
    g->cli.left = 0;
    g->cli.top = 0;
    g->cli.right = full.right - (vn ? TV_SB_W : 0);
    g->cli.bottom = full.bottom - (hn ? TV_SB_W : 0);
    if (g->cli.right < 0)
        g->cli.right = 0;
    if (g->cli.bottom < 0)
        g->cli.bottom = 0;
    g->rowh = rowh;
    g->nvis = tv->nvis;
    g->content_w = tv->cw;
    g->vbar = vn;
    g->hbar = hn;
    g->page = g->cli.bottom / rowh;
    if (g->page < 1)
        g->page = 1;
    maxtop = tv->nvis - g->page;
    if (maxtop < 0)
        maxtop = 0;
    if (!vn)
        tv->top = 0;
    else if (tv->top > maxtop)
        tv->top = maxtop;
    if (tv->top < 0)
        tv->top = 0;
    maxsx = tv->cw - g->cli.right;
    if (!hn || maxsx < 0)
        maxsx = 0;
    if (tv->scroll_x > maxsx)
        tv->scroll_x = maxsx;
    if (tv->scroll_x < 0)
        tv->scroll_x = 0;
}

/* applies the geometry to the window: bar visibility and scroll ranges */
static tv_geo_t tv_update(struct tv *tv)
{
    tv_geo_t g;
    SCROLLINFO si;
    HDC hdc = __aw_user32_GetDC(tv->hwnd);
    memset(&g, 0, sizeof g);
    if (!hdc)
        return g;
    tv_geo(tv, hdc, &g);
    __aw_user32_ReleaseDC(tv->hwnd, hdc);
    /* record the state before showing: ShowScrollBar may re-enter through WM_SIZE */
    if (g.vbar != tv->sbv) {
        tv->sbv = g.vbar;
        __aw_user32_ShowScrollBar(tv->hwnd, SB_VERT, g.vbar);
    }
    if (g.hbar != tv->sbh) {
        tv->sbh = g.hbar;
        __aw_user32_ShowScrollBar(tv->hwnd, SB_HORZ, g.hbar);
    }
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = tv->nvis > 0 ? tv->nvis - 1 : 0;
    si.nPage = (UINT)g.page;
    si.nPos = tv->top;
    __aw_user32_SetScrollInfo(tv->hwnd, SB_VERT, &si, TRUE);
    si.nMax = g.content_w > 0 ? g.content_w - 1 : 0;
    si.nPage = g.cli.right > 0 ? (UINT)g.cli.right : 1;
    si.nPos = tv->scroll_x;
    __aw_user32_SetScrollInfo(tv->hwnd, SB_HORZ, &si, TRUE);
    return g;
}

/* node under (x,y) in client coordinates; flags are TVHT_* */
static tv_node *tv_hit(struct tv *tv, HDC hdc, int x, int y, UINT *flags)
{
    tv_geo_t g;
    tv_node *n;
    WCHAR buf[TV_TEXT_MAX];
    int row, xc, cx, vc, tw;
    *flags = 0;
    tv_geo(tv, hdc, &g);
    if (y < 0) {
        *flags = TVHT_ABOVE;
        return NULL;
    }
    if (y >= g.cli.bottom) {
        *flags = TVHT_BELOW;
        return NULL;
    }
    if (x < 0) {
        *flags = TVHT_TOLEFT;
        return NULL;
    }
    if (x >= g.cli.right) {
        *flags = TVHT_TORIGHT;
        return NULL;
    }
    row = tv->top + y / g.rowh;
    if (row >= tv->nvis) {
        *flags = TVHT_NOWHERE;
        return NULL;
    }
    n = tv->vis[row];
    xc = x + tv->scroll_x;
    cx = tv_cx(tv, n->level);
    vc = tv_vcol(tv, n->level);
    if ((tv->style & TVS_HASBUTTONS) && xc >= vc - 5 && xc <= vc + 5 && tv_haschild(tv, n)) {
        *flags = TVHT_ONITEMBUTTON;
        return n;
    }
    if (xc < cx) {
        *flags = TVHT_ONITEMINDENT;
        return n;
    }
    tw = tv_label(tv, hdc, n, buf);
    *flags = (xc < cx + 2 + tw + 3) ? TVHT_ONITEMLABEL : TVHT_ONITEMRIGHT;
    return n;
}

/* ------------------------------------------------------------------ */
/* scrolling                                                           */
/* ------------------------------------------------------------------ */
/* scrolls so that row is on screen, and the label of n horizontally */
static void tv_show_item(struct tv *tv, tv_node *n)
{
    HDC hdc;
    tv_geo_t g;
    WCHAR buf[TV_TEXT_MAX];
    int row, left, right, tw;
    row = tv_row_of(tv, n);
    if (row < 0)
        return;
    hdc = __aw_user32_GetDC(tv->hwnd);
    if (!hdc)
        return;
    tv_geo(tv, hdc, &g);
    if (row < tv->top)
        tv->top = row;
    else if (row >= tv->top + g.page)
        tv->top = row - g.page + 1;
    if (g.hbar) {
        tw = tv_label(tv, hdc, n, buf);
        left = tv_cx(tv, n->level);
        right = left + 2 + tw + 3;
        if (right > tv->scroll_x + g.cli.right)
            tv->scroll_x = right - g.cli.right;
        if (left < tv->scroll_x)
            tv->scroll_x = left;
    }
    __aw_user32_ReleaseDC(tv->hwnd, hdc);
    tv_update(tv);
    __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
}

static void tv_scroll_msg(struct tv *tv, int horiz, WPARAM wp)
{
    tv_geo_t g;
    SCROLLINFO si;
    HDC hdc;
    int code = LOWORD(wp), base, step, page, max, target = 0;
    hdc = __aw_user32_GetDC(tv->hwnd);
    if (!hdc)
        return;
    tv_geo(tv, hdc, &g);
    __aw_user32_ReleaseDC(tv->hwnd, hdc);
    if (horiz) {
        base = tv->scroll_x;
        step = TV_HSTEP;
        page = g.cli.right;
        max = g.content_w - g.cli.right;
    } else {
        base = tv->top;
        step = 1;
        page = g.page;
        max = tv->nvis - g.page;
    }
    if (max < 0)
        max = 0;
    switch (code) {
    case SB_LINEUP:
        target = base - step;
        break;
    case SB_LINEDOWN:
        target = base + step;
        break;
    case SB_PAGEUP:
        target = base - page;
        break;
    case SB_PAGEDOWN:
        target = base + page;
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_TRACKPOS;
        __aw_user32_GetScrollInfo(tv->hwnd, horiz ? SB_HORZ : SB_VERT, &si);
        target = si.nTrackPos;
        break;
    case SB_TOP:
        target = 0;
        break;
    case SB_BOTTOM:
        target = max;
        break;
    default:
        return;
    }
    if (target < 0)
        target = 0;
    if (target > max)
        target = max;
    if (horiz)
        tv->scroll_x = target;
    else
        tv->top = target;
    tv_update(tv);
    __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
}

static void tv_wheel(struct tv *tv, int delta)
{
    tv->top -= (delta / WHEEL_DELTA) * TV_WHEEL_ROWS;
    tv_update(tv);
    __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* selection and expansion                                             */
/* ------------------------------------------------------------------ */
/* changes the caret; n = NULL clears it.  Returns FALSE when TVN_SELCHANGING vetoes */
static BOOL tv_select(struct tv *tv, tv_node *n, UINT how)
{
    tv_node *old = tv->caret;
    if (n == old)
        return TRUE;
    if (tv_fire_tree(tv, TVN_SELCHANGINGA, TVN_SELCHANGINGW, how, old, n))
        return FALSE;
    if (tv->caret)
        tv->caret->state &= ~TVIS_SELECTED;
    tv->caret = n;
    if (n)
        n->state |= TVIS_SELECTED;
    tv_fire_tree(tv, TVN_SELCHANGEDA, TVN_SELCHANGEDW, how, old, n);
    __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
    return TRUE;
}

/* act: TVE_EXPAND, TVE_COLLAPSE or TVE_TOGGLE; reset: TVE_COLLAPSERESET */
static BOOL tv_expand(struct tv *tv, tv_node *n, UINT act, int reset)
{
    int was, want;
    if (!n || n == &tv->root)
        return FALSE;
    was = (n->state & TVIS_EXPANDED) != 0;
    if (act == TVE_EXPAND)
        want = 1;
    else if (act == TVE_COLLAPSE)
        want = 0;
    else if (act == TVE_TOGGLE)
        want = !was;
    else
        return FALSE;
    if (want == was) {
        if (!want && reset)
            tv_clear_children(tv, n);
        return TRUE;
    }
    if (want && !tv_haschild(tv, n))
        return FALSE;
    act = want ? TVE_EXPAND : TVE_COLLAPSE;
    if (tv_fire_tree(tv, TVN_ITEMEXPANDINGA, TVN_ITEMEXPANDINGW, act, NULL, n))
        return FALSE;
    if (want) {
        n->state |= TVIS_EXPANDED | TVIS_EXPANDEDONCE;
    } else {
        n->state &= ~TVIS_EXPANDED;
        /* a selected item inside a collapsed branch moves to the branch head */
        if (tv->caret && tv->caret != n && tv_in_subtree(tv->caret, n))
            tv_select(tv, n, TVC_UNKNOWN);
        if (reset)
            tv_clear_children(tv, n);
    }
    tv_touch(tv);
    tv_fire_tree(tv, TVN_ITEMEXPANDEDA, TVN_ITEMEXPANDEDW, act, NULL, n);
    tv_changed(tv);
    return TRUE;
}

static void tv_expand_ancestors(struct tv *tv, tv_node *n)
{
    tv_node *p;
    for (p = n->parent; p && p != &tv->root; p = p->parent)
        if (!(p->state & TVIS_EXPANDED))
            tv_expand(tv, p, TVE_EXPAND, 0);
}

static void tv_expand_all(struct tv *tv, tv_node *n)
{
    tv_node *c;
    if (n != &tv->root)
        tv_expand(tv, n, TVE_EXPAND, 0);
    for (c = n->first; c; c = c->next)
        tv_expand_all(tv, c);
}

/* ------------------------------------------------------------------ */
/* mouse and keyboard                                                  */
/* ------------------------------------------------------------------ */
static void tv_lbutton_down(struct tv *tv, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    UINT flags = 0;
    HDC hdc;
    tv_node *n;
    if (!tv->has_focus)
        __aw_user32_SetFocus(tv->hwnd);
    tv->mdown = 1;
    hdc = __aw_user32_GetDC(tv->hwnd);
    if (!hdc)
        return;
    n = tv_hit(tv, hdc, x, y, &flags);
    __aw_user32_ReleaseDC(tv->hwnd, hdc);
    if (!n)
        return;
    if (flags & TVHT_ONITEMBUTTON) {
        tv_expand(tv, n, TVE_TOGGLE, 0);
        return;
    }
    if ((flags & (TVHT_ONITEMLABEL | TVHT_ONITEMICON)) ||
        ((tv->style & TVS_FULLROWSELECT) && (flags & (TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT))))
        tv_select(tv, n, TVC_BYMOUSE);
}

static void tv_lbutton_dblclk(struct tv *tv, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    UINT flags = 0;
    HDC hdc = __aw_user32_GetDC(tv->hwnd);
    tv_node *n = NULL;
    if (hdc) {
        n = tv_hit(tv, hdc, x, y, &flags);
        __aw_user32_ReleaseDC(tv->hwnd, hdc);
    }
    if (n && (flags & (TVHT_ONITEMBUTTON | TVHT_ONITEMLABEL | TVHT_ONITEMICON | TVHT_ONITEMRIGHT)))
        tv_expand(tv, n, TVE_TOGGLE, 0);
    tv_fire_nm(tv, NM_DBLCLK);
}

/* selects row (clamped) from the keyboard and keeps it on screen */
static void tv_key_row(struct tv *tv, int row)
{
    if (tv->nvis == 0)
        return;
    if (row < 0)
        row = 0;
    if (row >= tv->nvis)
        row = tv->nvis - 1;
    if (tv_select(tv, tv->vis[row], TVC_BYKEYBOARD))
        tv_show_item(tv, tv->vis[row]);
}

static void tv_key_down(struct tv *tv, UINT vk, LPARAM flags)
{
    NMTVKEYDOWN kd;
    tv_node *c = tv->caret;
    tv_geo_t g;
    HDC hdc;
    int row;
    memset(&kd, 0, sizeof kd);
    kd.wVKey = (WORD)vk;
    kd.flags = (UINT)flags;
    if (tv_notify(tv, &kd.hdr, TVN_KEYDOWN))
        return;
    hdc = __aw_user32_GetDC(tv->hwnd);
    if (!hdc)
        return;
    tv_geo(tv, hdc, &g);
    __aw_user32_ReleaseDC(tv->hwnd, hdc);
    row = tv_row_of(tv, c);
    switch (vk) {
    case VK_UP:
        tv_key_row(tv, row < 0 ? 0 : row - 1);
        break;
    case VK_DOWN:
        tv_key_row(tv, row < 0 ? 0 : row + 1);
        break;
    case VK_HOME:
        tv_key_row(tv, 0);
        break;
    case VK_END:
        tv_key_row(tv, tv->nvis - 1);
        break;
    case VK_PRIOR:
        tv_key_row(tv, (row < 0 ? 0 : row) - g.page);
        break;
    case VK_NEXT:
        tv_key_row(tv, (row < 0 ? 0 : row) + g.page);
        break;
    case VK_LEFT:
        if (!c)
            break;
        if ((c->state & TVIS_EXPANDED) && tv_haschild(tv, c)) {
            tv_expand(tv, c, TVE_COLLAPSE, 0);
        } else if (c->parent && c->parent != &tv->root) {
            if (tv_select(tv, c->parent, TVC_BYKEYBOARD))
                tv_show_item(tv, c->parent);
        }
        break;
    case VK_RIGHT:
        if (!c)
            break;
        if (!(c->state & TVIS_EXPANDED) && tv_haschild(tv, c)) {
            tv_expand(tv, c, TVE_EXPAND, 0);
        } else if ((c->state & TVIS_EXPANDED) && c->first) {
            if (tv_select(tv, c->first, TVC_BYKEYBOARD))
                tv_show_item(tv, c->first);
        }
        break;
    case VK_ADD:
        if (c)
            tv_expand(tv, c, TVE_EXPAND, 0);
        break;
    case VK_SUBTRACT:
        if (c)
            tv_expand(tv, c, TVE_COLLAPSE, 0);
        break;
    case VK_MULTIPLY:
        if (c)
            tv_expand_all(tv, c);
        break;
    default:
        return;
    }
    if (tv->caret)
        tv_show_item(tv, tv->caret);
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */
static void tv_dot_v(HDC hdc, HBRUSH br, int x, int y1, int y2)
{
    RECT r;
    int y;
    for (y = y1; y < y2; y += 2) {
        r.left = x;
        r.right = x + 1;
        r.top = y;
        r.bottom = y + 1;
        __aw_user32_FillRect(hdc, &r, br);
    }
}

static void tv_dot_h(HDC hdc, HBRUSH br, int x1, int x2, int y)
{
    RECT r;
    int x;
    for (x = x1; x < x2; x += 2) {
        r.left = x;
        r.right = x + 1;
        r.top = y;
        r.bottom = y + 1;
        __aw_user32_FillRect(hdc, &r, br);
    }
}

static void tv_rect(HDC hdc, int l, int t, int r, int b, HBRUSH br)
{
    RECT rc;
    rc.left = l;
    rc.top = t;
    rc.right = r;
    rc.bottom = b;
    __aw_user32_FillRect(hdc, &rc, br);
}

/* classic 9x9 button: grey frame, white face, black minus (and plus when collapsed) */
static void tv_button(HDC hdc, HBRUSH grey, HBRUSH white, HBRUSH black, int vc, int mid, int plus)
{
    tv_rect(hdc, vc - 4, mid - 4, vc + 5, mid + 5, grey);
    tv_rect(hdc, vc - 3, mid - 3, vc + 4, mid + 4, white);
    tv_rect(hdc, vc - 2, mid, vc + 3, mid + 1, black);
    if (plus)
        tv_rect(hdc, vc, mid - 2, vc + 1, mid + 3, black);
}

static void tv_paint(struct tv *tv, HDC hdc)
{
    tv_geo_t g;
    RECT all, tr, lab, hr, fr;
    HFONT font = ctl_font(tv->hwnd);
    COLORREF bk = tv->clr_bk == CLR_DEFAULT ? __aw_user32_GetSysColor(COLOR_WINDOW) : tv->clr_bk;
    COLORREF tc = tv->clr_text == CLR_DEFAULT ? __aw_user32_GetSysColor(COLOR_WINDOWTEXT) : tv->clr_text;
    COLORREF hl = __aw_user32_GetSysColor(COLOR_HIGHLIGHT);
    COLORREF hlt = __aw_user32_GetSysColor(COLOR_HIGHLIGHTTEXT);
    HBRUSH bkb = __aw_gdi32_CreateSolidBrush(bk), hlb = __aw_gdi32_CreateSolidBrush(hl);
    HBRUSH grey = __aw_gdi32_CreateSolidBrush(RGB(128, 128, 128));
    HBRUSH white = __aw_gdi32_CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH black = __aw_gdi32_CreateSolidBrush(RGB(0, 0, 0));
    int show_sel = tv->caret && (tv->has_focus || (tv->style & TVS_SHOWSELALWAYS));
    int lines = (tv->style & TVS_HASLINES) != 0, lroot = (tv->style & TVS_LINESATROOT) != 0;
    int i, end;

    g = tv_update(tv);
    __aw_user32_GetClientRect(tv->hwnd, &all);
    __aw_user32_FillRect(hdc, &all, bkb);
    end = tv->top + g.page + 1;
    if (end > tv->nvis)
        end = tv->nvis;
    for (i = tv->top; i < end; i++) {
        tv_node *n = tv->vis[i];
        int y = (i - tv->top) * g.rowh, mid = y + g.rowh / 2, sx = -tv->scroll_x;
        int cx = sx + tv_cx(tv, n->level), vc = sx + tv_vcol(tv, n->level), tx = cx + 2;
        int sel = (n == tv->caret) && show_sel, tw;
        WCHAR buf[TV_TEXT_MAX];
        if (y >= g.cli.bottom)
            break;
        if (lines && (n->level >= 1 || lroot)) {
            tv_dot_h(hdc, grey, vc, cx, mid);
            tv_dot_v(hdc, grey, vc, y, n->next ? y + g.rowh : mid + 1);
        }
        if (lines) {
            tv_node *p;
            for (p = n->parent; p && p != &tv->root; p = p->parent)
                if (p->next && (p->level >= 1 || lroot))
                    tv_dot_v(hdc, grey, sx + tv_vcol(tv, p->level), y, y + g.rowh);
        }
        if ((tv->style & TVS_HASBUTTONS) && tv_haschild(tv, n))
            tv_button(hdc, grey, white, black, vc, mid, !(n->state & TVIS_EXPANDED));
        tw = tv_label(tv, hdc, n, buf);
        lab.left = cx;
        lab.top = y;
        lab.right = tx + tw + 3;
        lab.bottom = y + g.rowh;
        if (sel) {
            hr = lab;
            if (tv->style & TVS_FULLROWSELECT) {
                hr.left = cx;
                hr.right = g.cli.right;
            }
            __aw_user32_FillRect(hdc, &hr, hlb);
        }
        tr.left = tx;
        tr.top = y;
        tr.right = g.cli.right;
        tr.bottom = y + g.rowh;
        ctl_text(hdc, font, buf, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, sel ? hlt : tc);
        if (n == tv->drop)
            __aw_user32_FrameRect(hdc, &lab, black);
        if (n == tv->caret && tv->has_focus) {
            fr = lab;
            if (fr.right > g.cli.right)
                fr.right = g.cli.right;
            __aw_user32_DrawFocusRect(hdc, &fr);
        }
    }
    __aw_gdi32_DeleteObject(bkb);
    __aw_gdi32_DeleteObject(hlb);
    __aw_gdi32_DeleteObject(grey);
    __aw_gdi32_DeleteObject(white);
    __aw_gdi32_DeleteObject(black);
}

static void tv_on_paint(struct tv *tv)
{
    PAINTSTRUCT ps;
    HDC hdc = __aw_user32_BeginPaint(tv->hwnd, &ps);
    if (hdc)
        tv_paint(tv, hdc);
    __aw_user32_EndPaint(tv->hwnd, &ps);
}

/* ------------------------------------------------------------------ */
/* TVM_* messages                                                      */
/* ------------------------------------------------------------------ */
static tv_node *tv_parent_arg(struct tv *tv, HANDLE h)
{
    if (!h || h == TVI_ROOT)
        return &tv->root;
    return TVNODE(h);
}

static tv_node *tv_insert(struct tv *tv, const TVINSERTSTRUCTW *is, int uni)
{
    const TVITEMW *it = &is->item;
    tv_node *p = tv_parent_arg(tv, is->hParent);
    tv_node *n, *ref = NULL, *s;
    HANDLE after = is->hInsertAfter;
    n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->level = p->level + 1;
    if (it->mask & TVIF_TEXT)
        n->text = tv_text_in(it->pszText, uni);
    if (it->mask & TVIF_STATE)
        n->state = it->state & it->stateMask & ~TVIS_SELECTED;
    if (it->mask & TVIF_IMAGE)
        n->image = it->iImage;
    if (it->mask & TVIF_SELECTEDIMAGE)
        n->selimage = it->iSelectedImage;
    if (it->mask & TVIF_CHILDREN)
        n->cchildren = it->cChildren;
    if (it->mask & TVIF_PARAM)
        n->param = it->lParam;
    if (after == TVI_LAST) {
        ref = p->last;
    } else if (after == TVI_SORT) {
        for (s = p->first; s; s = s->next) {
            if (tv_cmp_text(tv, s, n, NULL) <= 0)
                ref = s;
            else
                break;
        }
    } else if (after == TVI_FIRST || !after) {
        ref = NULL;
    } else {
        ref = TVNODE(after);
        if (ref->parent != p)
            ref = p->last;
    }
    tv_link_after(p, ref, n);
    tv->count++;
    tv_changed(tv);
    return n;
}

static LRESULT tv_getitemrect(struct tv *tv, WPARAM textonly, RECT *rc)
{
    tv_node *n = TVNODE(*(HANDLE *)(void *)rc);
    tv_geo_t g;
    HDC hdc;
    WCHAR buf[TV_TEXT_MAX];
    int row, cx, tx, tw, y;
    hdc = __aw_user32_GetDC(tv->hwnd);
    if (!hdc)
        return FALSE;
    tv_geo(tv, hdc, &g);
    row = tv_row_of(tv, n);
    if (row < 0 || row < tv->top || row >= tv->top + g.page) {
        __aw_user32_ReleaseDC(tv->hwnd, hdc);
        return FALSE;
    }
    tw = tv_label(tv, hdc, n, buf);
    __aw_user32_ReleaseDC(tv->hwnd, hdc);
    y = (row - tv->top) * g.rowh;
    cx = tv_cx(tv, n->level) - tv->scroll_x;
    tx = cx + 2;
    rc->left = textonly ? tx : cx;
    rc->right = textonly ? tx + tw : tx + tw + 3;
    rc->top = y;
    rc->bottom = y + g.rowh;
    return TRUE;
}

static LRESULT tv_getnext(struct tv *tv, WPARAM which, tv_node *n)
{
    tv_node *r = NULL;
    int row;
    switch (which) {
    case TVGN_ROOT:
        r = tv->root.first;
        break;
    case TVGN_NEXT:
        r = n ? n->next : NULL;
        break;
    case TVGN_PREVIOUS:
        r = n ? n->prev : NULL;
        break;
    case TVGN_PARENT:
        r = (n && n->parent != &tv->root) ? n->parent : NULL;
        break;
    case TVGN_CHILD:
        r = n ? n->first : NULL;
        break;
    case TVGN_FIRSTVISIBLE:
        tv_rows(tv);
        r = tv->top < tv->nvis ? tv->vis[tv->top] : NULL;
        break;
    case TVGN_NEXTVISIBLE:
    case TVGN_PREVIOUSVISIBLE:
        row = tv_row_of(tv, n);
        if (row >= 0) {
            int t = which == TVGN_NEXTVISIBLE ? row + 1 : row - 1;
            r = (t >= 0 && t < tv->nvis) ? tv->vis[t] : NULL;
        }
        break;
    case TVGN_LASTVISIBLE:
        tv_rows(tv);
        r = tv->nvis > 0 ? tv->vis[tv->nvis - 1] : NULL;
        break;
    case TVGN_DROPHILITE:
        r = tv->drop;
        break;
    case TVGN_CARET:
        r = tv->caret;
        break;
    default:
        break;
    }
    return (LRESULT)(intptr_t)r;
}

static BOOL tv_selectitem(struct tv *tv, WPARAM which, tv_node *n)
{
    switch (which) {
    case TVGN_CARET: {
        BOOL ok;
        if (n)
            tv_expand_ancestors(tv, n);
        ok = n ? tv_select(tv, n, TVC_UNKNOWN) : tv_select(tv, NULL, TVC_UNKNOWN);
        if (ok && n)
            tv_show_item(tv, n);
        return ok;
    }
    case TVGN_FIRSTVISIBLE: {
        int row = tv_row_of(tv, n);
        if (row < 0)
            return FALSE;
        tv->top = row;
        tv_update(tv);
        __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
        return TRUE;
    }
    case TVGN_DROPHILITE:
        if (tv->drop)
            tv->drop->state &= ~TVIS_DROPHILITED;
        tv->drop = n;
        if (n)
            n->state |= TVIS_DROPHILITED;
        __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
        return TRUE;
    default:
        return FALSE;
    }
}

/* number of children reported by TVM_GETITEM (1 when there are children) */
static int tv_children_out(struct tv *tv, tv_node *n)
{
    if (n->first)
        return 1;
    if (n->cchildren == I_CHILDRENCALLBACK)
        return tv_dispinfo(tv, n, TVIF_CHILDREN, NULL, 0);
    return n->cchildren > 0 ? 1 : 0;
}

static BOOL tv_getitem(struct tv *tv, TVITEMW *ti, int uni)
{
    tv_node *n = TVNODE(ti->hItem);
    UINT m = ti->mask;
    WCHAR buf[TV_TEXT_MAX];
    if (!n)
        return FALSE;
    if ((m & TVIF_TEXT) && ti->pszText && ti->cchTextMax > 0) {
        int cap = ti->cchTextMax;
        tv_text(tv, n, buf, TV_TEXT_MAX);
        if (uni) {
            int i;
            for (i = 0; i < cap - 1 && buf[i]; i++)
                ti->pszText[i] = buf[i];
            ti->pszText[i] = 0;
        } else {
            aw_w2u_term(buf, -1, (char *)(void *)ti->pszText, cap);
        }
    }
    if (m & TVIF_IMAGE)
        ti->iImage = n->image;
    if (m & TVIF_SELECTEDIMAGE)
        ti->iSelectedImage = n->selimage;
    if (m & TVIF_CHILDREN)
        ti->cChildren = tv_children_out(tv, n);
    if (m & TVIF_PARAM)
        ti->lParam = n->param;
    if (m & TVIF_STATE)
        ti->state = n->state & ti->stateMask;
    return TRUE;
}

static BOOL tv_setitem(struct tv *tv, const TVITEMW *ti, int uni)
{
    tv_node *n = TVNODE(ti->hItem);
    UINT m = ti->mask;
    if (!n)
        return FALSE;
    if (m & TVIF_TEXT) {
        WCHAR *t = tv_text_in(ti->pszText, uni);
        if (n->text && n->text != TV_CB)
            free(n->text);
        n->text = t;
    }
    if (m & TVIF_STATE) {
        UINT sm = ti->stateMask & ~TVIS_SELECTED;
        n->state = (n->state & ~sm) | (ti->state & sm);
    }
    if (m & TVIF_IMAGE)
        n->image = ti->iImage;
    if (m & TVIF_SELECTEDIMAGE)
        n->selimage = ti->iSelectedImage;
    if (m & TVIF_CHILDREN)
        n->cchildren = ti->cChildren;
    if (m & TVIF_PARAM)
        n->param = ti->lParam;
    tv_changed(tv);
    return TRUE;
}

static int tv_user_sort_cmp(struct tv *tv, tv_node *a, tv_node *b, void *ctx)
{
    return tv_cmp_user(tv, a, b, ctx);
}

static LRESULT tv_tvm(struct tv *tv, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    *handled = 1;
    switch (msg) {
    case TVM_INSERTITEMA:
    case TVM_INSERTITEMW:
        return (LRESULT)(intptr_t)tv_insert(tv, (const TVINSERTSTRUCTW *)(void *)lp, msg == TVM_INSERTITEMW);
    case TVM_DELETEITEM:
        tv_delete_node(tv, (!lp || lp == (LPARAM)TVI_ROOT) ? &tv->root : TVNODE(lp));
        return TRUE;
    case TVM_EXPAND:
        return tv_expand(tv, TVNODE(lp), (UINT)(wp & 0x0003), (wp & TVE_COLLAPSERESET) != 0);
    case TVM_GETITEMRECT:
        return tv_getitemrect(tv, wp, (RECT *)(void *)lp);
    case TVM_GETCOUNT:
        return tv->count;
    case TVM_GETINDENT:
        return tv->indent;
    case TVM_SETINDENT:
        tv->indent = (int)wp < 0 ? 0 : (int)wp;
        tv_changed(tv);
        return 0;
    case TVM_GETIMAGELIST:
        return (LRESULT)(intptr_t)tv->imagelist[wp == TVSIL_STATE ? 1 : 0];
    case TVM_SETIMAGELIST: {
        int k = wp == TVSIL_STATE ? 1 : 0;
        HANDLE old = tv->imagelist[k];
        tv->imagelist[k] = (HANDLE)lp;
        tv_changed(tv);
        return (LRESULT)(intptr_t)old;
    }
    case TVM_GETNEXTITEM:
        return tv_getnext(tv, wp, TVNODE(lp));
    case TVM_SELECTITEM:
        return tv_selectitem(tv, wp, TVNODE(lp));
    case TVM_GETITEMA:
    case TVM_GETITEMW:
        return tv_getitem(tv, (TVITEMW *)(void *)lp, msg == TVM_GETITEMW);
    case TVM_SETITEMA:
    case TVM_SETITEMW:
        return tv_setitem(tv, (const TVITEMW *)(void *)lp, msg == TVM_SETITEMW);
    case TVM_EDITLABELA:
    case TVM_EDITLABELW:
    case TVM_GETEDITCONTROL:
    case TVM_CREATEDRAGIMAGE:
    case TVM_ENDEDITLABELNOW:
    case TVM_GETISEARCHSTRINGA:
    case TVM_GETISEARCHSTRINGW:
        return 0;
    case TVM_GETVISIBLECOUNT: {
        tv_geo_t g;
        HDC hdc = __aw_user32_GetDC(tv->hwnd);
        if (!hdc)
            return 0;
        tv_geo(tv, hdc, &g);
        __aw_user32_ReleaseDC(tv->hwnd, hdc);
        return g.page;
    }
    case TVM_HITTEST: {
        TVHITTESTINFO *hi = (TVHITTESTINFO *)(void *)lp;
        HDC hdc = __aw_user32_GetDC(tv->hwnd);
        tv_node *n = NULL;
        UINT flags = TVHT_NOWHERE;
        if (hdc) {
            n = tv_hit(tv, hdc, hi->pt.x, hi->pt.y, &flags);
            __aw_user32_ReleaseDC(tv->hwnd, hdc);
        }
        hi->flags = flags;
        hi->hItem = TVHNDL(n);
        return (LRESULT)(intptr_t)n;
    }
    case TVM_ENSUREVISIBLE: {
        tv_node *n = TVNODE(lp);
        if (!n)
            return FALSE;
        tv_expand_ancestors(tv, n);
        if (tv_row_of(tv, n) < 0)
            return FALSE;
        tv_show_item(tv, n);
        return TRUE;
    }
    case TVM_SORTCHILDREN:
        tv_sort_rec(tv, tv_parent_arg(tv, (HANDLE)(intptr_t)lp), (int)wp);
        return TRUE;
    case TVM_SORTCHILDRENCB: {
        const tv_sortcb_t *sc = (const tv_sortcb_t *)(void *)lp;
        tv_cbctx ctx;
        ctx.fn = sc->lpfnCompare;
        ctx.lp = sc->lParam;
        if (!ctx.fn)
            return FALSE;
        tv_sort_children(tv, tv_parent_arg(tv, sc->hParent), tv_user_sort_cmp, &ctx);
        return TRUE;
    }
    case TVM_SETITEMHEIGHT:
        {
            int old = tv_rowh(tv);
            int h = (int)(short)wp;
            tv->itemh = h > 0 ? h : 0;
            tv_changed(tv);
            return old;
        }
    case TVM_GETITEMHEIGHT:
        return tv_rowh(tv);
    case TVM_SETBKCOLOR: {
        COLORREF old = tv->clr_bk;
        tv->clr_bk = (COLORREF)lp;
        __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
        return (LRESULT)old;
    }
    case TVM_GETBKCOLOR:
        return (LRESULT)tv->clr_bk;
    case TVM_SETTEXTCOLOR: {
        COLORREF old = tv->clr_text;
        tv->clr_text = (COLORREF)lp;
        __aw_user32_InvalidateRect(tv->hwnd, NULL, FALSE);
        return (LRESULT)old;
    }
    case TVM_GETTEXTCOLOR:
        return (LRESULT)tv->clr_text;
    case TVM_SETSCROLLTIME: {
        int old = tv->scrolltime;
        tv->scrolltime = (int)wp;
        return old;
    }
    case TVM_GETSCROLLTIME:
        return tv->scrolltime;
    case TVM_SETINSERTMARKCOLOR:
    case TVM_GETINSERTMARKCOLOR:
        return (LRESULT)CLR_DEFAULT;
    case TVM_SETINSERTMARK:
        return FALSE;
    case TVM_SETTOOLTIPS: {
        HWND old = tv->tooltips;
        tv->tooltips = (HWND)(void *)wp;
        return (LRESULT)(intptr_t)old;
    }
    case TVM_GETTOOLTIPS:
        return (LRESULT)(intptr_t)tv->tooltips;
    default:
        break;
    }
    *handled = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */
static void tv_clear_all(struct tv *tv)
{
    tv_clear_children(tv, &tv->root);
    tv->caret = NULL;
    tv->drop = NULL;
    free(tv->vis);
    tv->vis = NULL;
    tv->nvis = tv->capvis = 0;
}

static void tv_init(struct tv *tv, HWND hwnd)
{
    tv->hwnd = hwnd;
    tv->style = (DWORD)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
    tv->root.level = -1;
    tv->indent = TV_DEF_INDENT;
    tv->clr_bk = CLR_DEFAULT;
    tv->clr_text = CLR_DEFAULT;
    tv->dirty = 1;
    tv->cw = -1;
    tv->sbv = tv->sbh = 0;
}

LRESULT WINAPI aw_treeview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct tv *tv = ctl_get(hwnd);
    LRESULT res;
    int handled = 0;

    if (msg == WM_CREATE) {
        tv = ctl_alloc(hwnd, sizeof(struct tv));
        if (!tv)
            return -1;
        tv_init(tv, hwnd);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        if (tv) {
            tv_clear_all(tv);
            ctl_free(hwnd);
        }
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!tv)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_SETFONT)
        tv_touch(tv);
    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;

    switch (msg) {
    case WM_PAINT:
        tv_on_paint(tv);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        tv_update(tv);
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_STYLECHANGED:
        tv->style = (DWORD)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
        tv_changed(tv);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        tv->has_focus = 1;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        tv_fire_nm(tv, NM_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        tv->has_focus = 0;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        tv_fire_nm(tv, NM_KILLFOCUS);
        return 0;
    case WM_CAPTURECHANGED:
        tv->mdown = 0;
        return 0;
    case WM_KEYDOWN:
        tv_key_down(tv, (UINT)wp, lp);
        return 0;
    case WM_LBUTTONDOWN:
        tv_lbutton_down(tv, lp);
        return 0;
    case WM_LBUTTONUP:
        if (tv->mdown) {
            tv->mdown = 0;
            tv_fire_nm(tv, NM_CLICK);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        tv_lbutton_dblclk(tv, lp);
        return 0;
    case WM_RBUTTONUP:
        tv_fire_nm(tv, NM_RCLICK);
        return 0;
    case WM_MOUSEWHEEL:
        tv_wheel(tv, (short)HIWORD(wp));
        return 0;
    case WM_VSCROLL:
        tv_scroll_msg(tv, 0, wp);
        return 0;
    case WM_HSCROLL:
        tv_scroll_msg(tv, 1, wp);
        return 0;
    default:
        break;
    }
    res = tv_tvm(tv, msg, wp, lp, &handled);
    if (handled)
        return res;
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}
