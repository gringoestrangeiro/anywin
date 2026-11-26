/* anywin runtime - SysListView32 (report, list, small-icon and icon views).
 *
 * Two layers in one file:
 *  - the model (rows, sub-item text, state bits, selection/focus policy, stable sort) makes no
 *    Win32 calls; tests/listview_unit.c includes this file with AW_LV_UNIT_TEST defined and
 *    checks the model directly;
 *  - the view (geometry, painting, mouse/keyboard, scrolling, LVM_* messages, notifications).
 * Column data (text, width, format, order) lives in the SysHeader32 child (header.c); this file
 * reads it through aw_hdr_* and changes it with HDM_* messages, as Windows does. */
#include "ctl.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* constants and layouts that ctl.h does not provide                  */
/* ------------------------------------------------------------------ */
#ifndef LVM_SETVIEW
#define LVM_SETVIEW (LVM_FIRST + 142)
#define LVM_GETVIEW (LVM_FIRST + 143)
#endif
#ifndef WM_CAPTURECHANGED
#define WM_CAPTURECHANGED 0x0215
#endif
#ifndef MK_SHIFT
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#endif
#ifndef LVFI_PARAM
#define LVFI_PARAM 0x0001
#define LVFI_STRING 0x0002
#define LVFI_PARTIAL 0x0008
#endif
#ifndef CDIS_SELECTED
#define CDIS_SELECTED 0x0001
#define CDIS_FOCUS 0x0010
#endif

#define AW_LV_VIEW_ICON 0
#define AW_LV_VIEW_DETAILS 1
#define AW_LV_VIEW_SMALLICON 2
#define AW_LV_VIEW_LIST 3

#define LV_TEXT_MAX 260
#define LV_CB ((WCHAR *)(intptr_t)-1) /* sub-item text comes from LVN_GETDISPINFO */
#define LV_PAD 6                      /* text inset inside a cell */
#define LV_CHECK_W 13                 /* checkbox square */
#define LV_CHECK_IN 18                /* space the checkbox takes before the label */
#define LV_SB_W 17                    /* scroll bar thickness */
#define LV_ICON_CELL_W 96
#define LV_ICON_CELL_H 48
#define LV_WHEEL_LINES 3
#define LV_WHEEL_PX 40
#define LV_CLR_TEXT_DEFAULT CLR_DEFAULT

static const WCHAR lv_empty[1] = {0};

/* NMITEMACTIVATE / NMLISTVIEW share this layout (x64: lParam at 0x38, uKeyFlags at 0x40) */
typedef struct {
    NMHDR hdr;
    int iItem;
    int iSubItem;
    UINT uNewState;
    UINT uOldState;
    UINT uChanged;
    POINT ptAction;
    LPARAM lParam;
    UINT uKeyFlags;
} lv_nmitem_t;

typedef int(AWCALL *lv_cmp_fn)(LPARAM, LPARAM, LPARAM);

/* LVFINDINFO layout (x64): flags 0, psz 8, lParam 0x10, pt 0x18, vkDirection 0x20 */
typedef struct {
    UINT flags;
    const void *psz;
    LPARAM lParam;
    POINT pt;
    UINT vkDirection;
} lv_findinfo_t;

/* SysHeader32 accessors (header.c, same package) */
int aw_hdr_count(HWND hwnd);
int aw_hdr_index_at_pos(HWND hwnd, int pos);
int aw_hdr_width(HWND hwnd, int idx);
int aw_hdr_fmt(HWND hwnd, int idx);
const WCHAR *aw_hdr_text(HWND hwnd, int idx);
int aw_hdr_height(HWND hwnd);
int aw_hdr_total_width(HWND hwnd);

/* ------------------------------------------------------------------ */
/* model                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    LPARAM lparam;
    UINT state;  /* LVIS_*; state image index in bits 12..15 */
    int image;
    int indent;
    int nsub;    /* slots allocated in sub[] */
    WCHAR **sub; /* sub[0] = label; NULL = empty; LV_CB = callback */
} lv_row;

/* state hooks: changing() returning nonzero vetoes the change; changed() runs after it */
typedef struct {
    void *ctx;
    int (*changing)(void *ctx, int idx, UINT oldst, UINT newst);
    void (*changed)(void *ctx, int idx, UINT oldst, UINT newst);
} lv_ops;

struct lv {
    /* model */
    lv_row **rows;
    int nrows, caprows;
    int focus;     /* row that holds LVIS_FOCUSED, or -1 */
    int anchor;    /* selection mark: base of shift-selection, or -1 */
    int sortmode;  /* 0 none, 1 ascending, 2 descending (LVS_SORT*) */
    int ownerdata; /* LVS_OWNERDATA: rows are placeholders; text comes from callbacks */

    /* view */
    HWND hwnd;
    HWND header;
    DWORD style;
    DWORD exstyle;  /* LVS_EX_* */
    int view;       /* AW_LV_VIEW_* (same numbers as LVS_ICON..LVS_LIST) */
    int top;        /* report: first visible row */
    int scroll_x;   /* horizontal scroll, pixels */
    int has_focus;
    int mdown;      /* left button went down inside the control (NM_CLICK needs it) */
    int wheel;      /* wheel delta not yet turned into lines */
    int sbv, sbh;   /* scroll bars currently shown */
    int cellw_cache;
    COLORREF clr_bk, clr_text, clr_textbk;
    HANDLE imagelists[4];
    HANDLE hotcursor;
    HWND tooltips;
    int hotitem;
    int iconspacing;
    DWORD ccver;
};

typedef struct {
    RECT cli;      /* client rectangle, scroll bars excluded */
    int hh;        /* header height, 0 when hidden */
    int rowh;      /* report row height */
    int cellw;     /* grid cell width */
    int cellh;     /* grid cell height */
    int per;       /* grid items per column */
    int content_w; /* scrollable width */
    int vis;       /* report: rows visible (partly); grid: items per column */
} lv_geo;

static lv_row *lv_row_new(void)
{
    return calloc(1, sizeof(lv_row));
}

static void lv_row_free(lv_row *r)
{
    if (!r)
        return;
    for (int i = 0; i < r->nsub; i++)
        if (r->sub[i] != LV_CB)
            free(r->sub[i]);
    free(r->sub);
    free(r);
}

/* takes ownership of owned (NULL and LV_CB are allowed) */
static int lv_row_set_sub(lv_row *r, int sub, WCHAR *owned)
{
    if (sub < 0) {
        if (owned != LV_CB)
            free(owned);
        return -1;
    }
    if (sub >= r->nsub) {
        int nn = sub + 1;
        WCHAR **ns = realloc(r->sub, (size_t)nn * sizeof *ns);
        if (!ns) {
            if (owned != LV_CB)
                free(owned);
            return -1;
        }
        for (int i = r->nsub; i < nn; i++)
            ns[i] = NULL;
        r->sub = ns;
        r->nsub = nn;
    }
    if (r->sub[sub] != LV_CB)
        free(r->sub[sub]);
    r->sub[sub] = owned;
    return 0;
}

static const WCHAR *lv_row_text(const lv_row *r, int sub)
{
    if (sub < 0 || sub >= r->nsub)
        return NULL;
    return r->sub[sub];
}

/* opens an empty sub-item slot at col; later sub-items move one place right */
static int lv_row_insert_sub(lv_row *r, int col)
{
    WCHAR **ns;
    if (col < 0 || col >= r->nsub)
        return 0;
    ns = realloc(r->sub, (size_t)(r->nsub + 1) * sizeof *ns);
    if (!ns)
        return -1;
    r->sub = ns;
    memmove(&r->sub[col + 1], &r->sub[col], (size_t)(r->nsub - col) * sizeof *ns);
    r->sub[col] = NULL;
    r->nsub++;
    return 0;
}

static void lv_row_delete_sub(lv_row *r, int col)
{
    if (col < 0 || col >= r->nsub)
        return;
    if (r->sub[col] != LV_CB)
        free(r->sub[col]);
    memmove(&r->sub[col], &r->sub[col + 1], (size_t)(r->nsub - col - 1) * sizeof *r->sub);
    r->nsub--;
}

static WCHAR *lv_wdup(const WCHAR *s)
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

static int lv_wlen(const WCHAR *s)
{
    int n = 0;
    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

static WCHAR lv_lower(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
}

/* case-insensitive order (ASCII folding); NULL sorts as empty */
static int lv_wcmp(const WCHAR *a, const WCHAR *b)
{
    if (!a)
        a = lv_empty;
    if (!b)
        b = lv_empty;
    while (*a && lv_lower(*a) == lv_lower(*b))
        a++, b++;
    return (int)lv_lower(*a) - (int)lv_lower(*b);
}

static int lv_wprefix(const WCHAR *s, const WCHAR *pre)
{
    if (!s || !pre)
        return 0;
    for (; *pre; s++, pre++)
        if (!*s || lv_lower(*s) != lv_lower(*pre))
            return 0;
    return 1;
}

/* label of a row for sorting and searching; callback text is not available here */
static const WCHAR *lv_label_for_model(const lv_row *r)
{
    const WCHAR *t = lv_row_text(r, 0);
    return t == LV_CB ? NULL : t;
}

static int lv_reserve(struct lv *lv, int n)
{
    lv_row **nr;
    int nc;
    if (n <= lv->caprows)
        return 0;
    nc = lv->caprows ? lv->caprows * 2 : 16;
    if (nc < n)
        nc = n;
    nr = realloc(lv->rows, (size_t)nc * sizeof *nr);
    if (!nr)
        return -1;
    lv->rows = nr;
    lv->caprows = nc;
    return 0;
}

static int lv_index_of_row(const struct lv *lv, const lv_row *r)
{
    for (int i = 0; r && i < lv->nrows; i++)
        if (lv->rows[i] == r)
            return i;
    return -1;
}

/* inserts r at idx (clamped to the end); focus and anchor stay on their rows */
static int lv_insert_row(struct lv *lv, int idx, lv_row *r)
{
    if (idx < 0 || idx > lv->nrows)
        idx = lv->nrows;
    if (lv_reserve(lv, lv->nrows + 1))
        return -1;
    memmove(&lv->rows[idx + 1], &lv->rows[idx], (size_t)(lv->nrows - idx) * sizeof *lv->rows);
    lv->rows[idx] = r;
    lv->nrows++;
    if (lv->focus >= idx)
        lv->focus++;
    if (lv->anchor >= idx)
        lv->anchor++;
    return idx;
}

/* removes the row at idx and returns it; the caller frees it */
static lv_row *lv_remove_row(struct lv *lv, int idx)
{
    lv_row *r;
    if (idx < 0 || idx >= lv->nrows)
        return NULL;
    r = lv->rows[idx];
    memmove(&lv->rows[idx], &lv->rows[idx + 1], (size_t)(lv->nrows - idx - 1) * sizeof *lv->rows);
    lv->nrows--;
    if (lv->focus == idx)
        lv->focus = -1;
    else if (lv->focus > idx)
        lv->focus--;
    if (lv->anchor == idx)
        lv->anchor = -1;
    else if (lv->anchor > idx)
        lv->anchor--;
    return r;
}

/* first position where a new label belongs in the LVS_SORT* order */
static int lv_sorted_pos(const struct lv *lv, const WCHAR *text)
{
    int i;
    for (i = 0; i < lv->nrows; i++) {
        const WCHAR *t = lv_label_for_model(lv->rows[i]);
        if (lv->sortmode == 1 && lv_wcmp(t, text) > 0)
            break;
        if (lv->sortmode == 2 && lv_wcmp(t, text) < 0)
            break;
    }
    return i;
}

/* stable bottom-up merge sort of the row pointers; the comparator is the program's (ms_abi).
 * lv->rows stays untouched until the end so the comparator can query items by index
 * (LVM_SORTITEMSEX passes item indices, LVM_SORTITEMS passes item lParams).
 * The left item moves after the right one only when cmp(left, right) > 0, so boolean-style
 * comparators work as they do on Windows. */
static int lv_sort_rows(struct lv *lv, lv_cmp_fn cmp, LPARAM sp, int by_index)
{
    int n = lv->nrows;
    lv_row **a = lv->rows;
    lv_row *fr = (lv->focus >= 0 && lv->focus < n) ? a[lv->focus] : NULL;
    lv_row *ar = (lv->anchor >= 0 && lv->anchor < n) ? a[lv->anchor] : NULL;
    lv_row **cur, **tmp;
    int *ix, *tix;

    if (n < 2)
        return 1;
    cur = malloc((size_t)n * sizeof *cur);
    tmp = malloc((size_t)n * sizeof *tmp);
    ix = malloc((size_t)n * sizeof *ix);
    tix = malloc((size_t)n * sizeof *tix);
    if (!cur || !tmp || !ix || !tix) {
        free(cur); free(tmp); free(ix); free(tix);
        return 0;
    }
    memcpy(cur, a, (size_t)n * sizeof *cur);
    for (int i = 0; i < n; i++)
        ix[i] = i;
    for (int w = 1; w < n; w *= 2) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n;
            int hi = lo + 2 * w < n ? lo + 2 * w : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                LPARAM l = by_index ? (LPARAM)ix[i] : cur[i]->lparam;
                LPARAM r = by_index ? (LPARAM)ix[j] : cur[j]->lparam;
                if (cmp(l, r, sp) > 0) {
                    tix[k] = ix[j]; tmp[k++] = cur[j++];
                } else {
                    tix[k] = ix[i]; tmp[k++] = cur[i++];
                }
            }
            while (i < mid) { tix[k] = ix[i]; tmp[k++] = cur[i++]; }
            while (j < hi) { tix[k] = ix[j]; tmp[k++] = cur[j++]; }
            memcpy(&cur[lo], &tmp[lo], (size_t)(hi - lo) * sizeof *cur);
            memcpy(&ix[lo], &tix[lo], (size_t)(hi - lo) * sizeof *ix);
        }
    }
    memcpy(a, cur, (size_t)n * sizeof *a);
    free(cur); free(tmp); free(ix); free(tix);
    lv->focus = lv_index_of_row(lv, fr);
    lv->anchor = lv_index_of_row(lv, ar);
    return 1;
}

/* applies (bits & mask) to row idx through the hooks; keeps lv->focus in step with LVIS_FOCUSED */
static int lv_set_state(struct lv *lv, const lv_ops *ops, int idx, UINT bits, UINT mask)
{
    lv_row *r;
    UINT oldst, newst;
    if (idx < 0 || idx >= lv->nrows || !mask)
        return 0;
    r = lv->rows[idx];
    oldst = r->state;
    newst = (oldst & ~mask) | (bits & mask);
    if (newst == oldst)
        return 0;
    if (ops && ops->changing && ops->changing(ops->ctx, idx, oldst, newst))
        return 0;
    /* a hook may have changed the list: find the row again before storing */
    if (idx >= lv->nrows)
        return 0;
    r = lv->rows[idx];
    oldst = r->state;
    newst = (oldst & ~mask) | (bits & mask);
    if (newst == oldst)
        return 0;
    r->state = newst;
    if ((newst & LVIS_FOCUSED) && !(oldst & LVIS_FOCUSED))
        lv->focus = idx;
    else if (!(newst & LVIS_FOCUSED) && (oldst & LVIS_FOCUSED) && lv->focus == idx)
        lv->focus = -1;
    if (ops && ops->changed)
        ops->changed(ops->ctx, idx, oldst, newst);
    return 1;
}

static int lv_set_focus(struct lv *lv, const lv_ops *ops, int idx)
{
    int c = 0;
    if (idx < 0 || idx >= lv->nrows)
        return 0;
    if (lv->focus >= 0 && lv->focus != idx && lv->focus < lv->nrows)
        c += lv_set_state(lv, ops, lv->focus, 0, LVIS_FOCUSED);
    c += lv_set_state(lv, ops, idx, LVIS_FOCUSED, LVIS_FOCUSED);
    return c;
}

/* selects idx alone (idx < 0 clears the selection); deselections come first, as in Windows */
static int lv_select_only(struct lv *lv, const lv_ops *ops, int idx)
{
    int c = 0;
    for (int i = 0; i < lv->nrows; i++)
        if (i != idx && (lv->rows[i]->state & LVIS_SELECTED))
            c += lv_set_state(lv, ops, i, 0, LVIS_SELECTED);
    if (idx >= 0)
        c += lv_set_state(lv, ops, idx, LVIS_SELECTED, LVIS_SELECTED);
    return c;
}

/* selects [a..b] in either order and clears everything outside it */
static int lv_select_range(struct lv *lv, const lv_ops *ops, int a, int b)
{
    int c = 0, lo = a < b ? a : b, hi = a < b ? b : a;
    for (int i = 0; i < lv->nrows; i++) {
        UINT bits = (i >= lo && i <= hi) ? LVIS_SELECTED : 0;
        c += lv_set_state(lv, ops, i, bits, LVIS_SELECTED);
    }
    return c;
}

static int lv_selected_count(const struct lv *lv)
{
    int n = 0;
    for (int i = 0; i < lv->nrows; i++)
        if (lv->rows[i]->state & LVIS_SELECTED)
            n++;
    return n;
}

/* LVM_GETNEXTITEM for the FOCUSED/SELECTED filters; -1 when none follows start */
static int lv_next_item(const struct lv *lv, int start, UINT flags)
{
    for (int i = start + 1; i < lv->nrows; i++) {
        UINT st = lv->rows[i]->state;
        if ((flags & LVNI_FOCUSED) && !(st & LVIS_FOCUSED))
            continue;
        if ((flags & LVNI_SELECTED) && !(st & LVIS_SELECTED))
            continue;
        return i;
    }
    return -1;
}

/* LVM_FINDITEM by lParam, or by label (exact, or prefix with LVFI_PARTIAL); searches from start+1 and wraps */
static int lv_find_item(const struct lv *lv, int start, UINT flags, const WCHAR *text, LPARAM param)
{
    int n = lv->nrows;
    for (int k = 1; k <= n; k++) {
        int i = (start + k) % n;
        const lv_row *r = lv->rows[i];
        if (flags & LVFI_PARAM) {
            if (r->lparam == param)
                return i;
        } else if (text) {
            const WCHAR *t = lv_label_for_model(r);
            if (flags & LVFI_PARTIAL) {
                if (lv_wprefix(t ? t : lv_empty, text))
                    return i;
            } else if (t && lv_wcmp(t, text) == 0) {
                return i;
            }
        }
    }
    return -1;
}

/* keyboard focus move; mods are MK_SHIFT / MK_CONTROL bits */
static void lv_move_focus(struct lv *lv, const lv_ops *ops, int idx, UINT mods, int singlesel)
{
    if (idx < 0 || idx >= lv->nrows)
        return;
    if (singlesel) {
        lv_set_focus(lv, ops, idx);
        lv_select_only(lv, ops, idx);
        lv->anchor = idx;
        return;
    }
    if (mods & MK_CONTROL) { /* focus moves, selection stays */
        lv_set_focus(lv, ops, idx);
        return;
    }
    if (mods & MK_SHIFT) { /* range from the anchor */
        int a = (lv->anchor >= 0 && lv->anchor < lv->nrows) ? lv->anchor : idx;
        lv_select_range(lv, ops, a, idx);
        lv_set_focus(lv, ops, idx);
        return;
    }
    lv_set_focus(lv, ops, idx);
    lv_select_only(lv, ops, idx);
    lv->anchor = idx;
}

/* mouse press on an item (not on its checkbox) */
static void lv_click_item(struct lv *lv, const lv_ops *ops, int idx, UINT mods, int singlesel)
{
    if (idx < 0 || idx >= lv->nrows)
        return;
    if (singlesel) {
        lv_move_focus(lv, ops, idx, 0, 1);
        return;
    }
    if (mods & MK_CONTROL) { /* toggles this item and makes it the focus */
        UINT on = (lv->rows[idx]->state & LVIS_SELECTED) ? 0 : LVIS_SELECTED;
        lv_set_focus(lv, ops, idx);
        lv_set_state(lv, ops, idx, on, LVIS_SELECTED);
        lv->anchor = idx;
        return;
    }
    lv_move_focus(lv, ops, idx, mods, 0);
}

/* mouse press on empty space clears the selection unless Ctrl or Shift is held */
static void lv_click_blank(struct lv *lv, const lv_ops *ops, UINT mods)
{
    if (!(mods & (MK_CONTROL | MK_SHIFT)))
        lv_select_only(lv, ops, -1);
}

/* checkbox state image: 1 = unchecked, 2 = checked */
static void lv_toggle_check(struct lv *lv, const lv_ops *ops, int idx)
{
    UINT cur;
    if (idx < 0 || idx >= lv->nrows)
        return;
    cur = (lv->rows[idx]->state & LVIS_STATEIMAGEMASK) >> 12;
    lv_set_state(lv, ops, idx, (cur == 2 ? 1u : 2u) << 12, LVIS_STATEIMAGEMASK);
}

#ifndef AW_LV_UNIT_TEST
/* ================================================================== */
/* view                                                                */
/* ================================================================== */

static int lv_uni(struct lv *lv)
{
    return ctl_is_unicode_notify(lv->hwnd);
}

/* sends a WM_NOTIFY with the given code to the parent */
static LRESULT lv_send(struct lv *lv, NMHDR *n, UINT code)
{
    HWND parent = __aw_user32_GetParent(lv->hwnd);
    if (!parent)
        return 0;
    n->hwndFrom = lv->hwnd;
    n->idFrom = (UINT_PTR)__aw_user32_GetDlgCtrlID(lv->hwnd);
    n->code = code;
    return __aw_user32_SendMessageW(parent, WM_NOTIFY, n->idFrom, (LPARAM)n);
}

static LPARAM lv_param_of(const struct lv *lv, int idx)
{
    return (idx >= 0 && idx < lv->nrows) ? lv->rows[idx]->lparam : 0;
}

static int lv_cb_changing(void *ctx, int idx, UINT oldst, UINT newst)
{
    struct lv *lv = ctx;
    lv_nmitem_t n;
    memset(&n, 0, sizeof n);
    n.iItem = idx;
    n.uOldState = oldst;
    n.uNewState = newst;
    n.uChanged = LVIF_STATE;
    n.lParam = lv_param_of(lv, idx);
    return lv_send(lv, &n.hdr, LVN_ITEMCHANGING) != 0;
}

static void lv_cb_changed(void *ctx, int idx, UINT oldst, UINT newst)
{
    struct lv *lv = ctx;
    lv_nmitem_t n;
    memset(&n, 0, sizeof n);
    n.iItem = idx;
    n.uOldState = oldst;
    n.uNewState = newst;
    n.uChanged = LVIF_STATE;
    n.lParam = lv_param_of(lv, idx);
    lv_send(lv, &n.hdr, LVN_ITEMCHANGED);
    __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
}

static lv_ops lv_make_ops(struct lv *lv)
{
    lv_ops ops;
    ops.ctx = lv;
    ops.changing = lv_cb_changing;
    ops.changed = lv_cb_changed;
    return ops;
}

/* text the app passed in: LPSTR_TEXTCALLBACK sets *cb; otherwise an owned UTF-16 copy (A text is UTF-8) */
static WCHAR *lv_text_in(const void *p, int uni, int *cb)
{
    *cb = 0;
    if (!p)
        return NULL;
    if ((intptr_t)p == -1) {
        *cb = 1;
        return NULL;
    }
    return uni ? lv_wdup((const WCHAR *)p) : aw_u2w((const char *)p, -1);
}

/* text out to an app buffer; cap is characters for W and bytes for A; never splits a UTF-8 sequence */
static void lv_text_out(const WCHAR *src, void *dst, int cap, int uni)
{
    if (!dst || cap <= 0)
        return;
    if (uni) {
        int k = lv_wlen(src ? src : lv_empty);
        if (k > cap - 1)
            k = cap - 1;
        if (src)
            memcpy(dst, src, (size_t)k * sizeof(WCHAR));
        ((WCHAR *)dst)[k] = 0;
    } else {
        char *u = aw_w2u(src ? src : lv_empty, -1);
        size_t len = strlen(u), n = len;
        if (n > (size_t)cap - 1)
            n = (size_t)cap - 1;
        while (n > 0 && n < len && ((unsigned char)u[n] & 0xC0) == 0x80)
            n--;
        memcpy(dst, u, n);
        ((char *)dst)[n] = 0;
        free(u);
    }
}

static int lv_hdr_height(struct lv *lv)
{
    if (!lv->header || lv->view != AW_LV_VIEW_DETAILS || (lv->style & LVS_NOCOLUMNHEADER))
        return 0;
    return aw_hdr_height(lv->header);
}

static int lv_row_height(struct lv *lv)
{
    int h = ctl_font_height(lv->hwnd) + 3;
    return h < 16 ? 16 : h;
}

static int lv_ncols(struct lv *lv)
{
    return lv->header ? aw_hdr_count(lv->header) : 0;
}

/* logical column at display position pos */
static int lv_col_at_pos(struct lv *lv, int pos)
{
    return lv->header ? aw_hdr_index_at_pos(lv->header, pos) : -1;
}

static int lv_pos_of_col(struct lv *lv, int col)
{
    int n = lv_ncols(lv);
    for (int p = 0; p < n; p++)
        if (lv_col_at_pos(lv, p) == col)
            return p;
    return -1;
}

/* content x of the left edge of display position pos */
static int lv_col_x(struct lv *lv, int pos)
{
    int x = 0;
    for (int p = 0; p < pos; p++)
        x += aw_hdr_width(lv->header, lv_col_at_pos(lv, p));
    return x;
}

static int lv_text_width(struct lv *lv, const WCHAR *s)
{
    SIZE sz;
    if (!s || !*s)
        return 0;
    sz = ctl_measure(lv->hwnd, ctl_font(lv->hwnd), s, lv_wlen(s));
    return sz.cx;
}

/* the text of a cell as displayed; callback text is fetched into buf */
static const WCHAR *lv_text_for(struct lv *lv, int idx, int sub, WCHAR *buf, int cap);

static void lv_touch(struct lv *lv)
{
    lv->cellw_cache = 0;
}

static void lv_get_geo(struct lv *lv, lv_geo *g)
{
    int avail_h;
    __aw_user32_GetClientRect(lv->hwnd, &g->cli);
    g->hh = lv_hdr_height(lv);
    g->rowh = lv_row_height(lv);
    avail_h = g->cli.bottom - g->hh;
    if (avail_h < 1)
        avail_h = 1;
    if (lv->view == AW_LV_VIEW_DETAILS) {
        g->cellw = g->cellh = 0;
        g->per = 0;
        g->content_w = lv->header ? aw_hdr_total_width(lv->header) : 0;
        g->vis = avail_h / g->rowh;
        if (g->vis < 1)
            g->vis = 1;
        return;
    }
    if (lv->view == AW_LV_VIEW_ICON) {
        g->cellw = LV_ICON_CELL_W;
        g->cellh = LV_ICON_CELL_H;
    } else {
        if (lv->cellw_cache == 0) {
            WCHAR buf[LV_TEXT_MAX];
            int wmax = 0;
            for (int i = 0; i < lv->nrows; i++) {
                int w = lv_text_width(lv, lv_text_for(lv, i, 0, buf, LV_TEXT_MAX));
                if (w > wmax)
                    wmax = w;
            }
            lv->cellw_cache = wmax + 24 < 64 ? 64 : wmax + 24;
        }
        g->cellw = lv->cellw_cache;
        g->cellh = g->rowh;
    }
    g->per = avail_h / g->cellh;
    if (g->per < 1)
        g->per = 1;
    g->content_w = ((lv->nrows + g->per - 1) / g->per) * g->cellw;
    g->vis = g->per;
}

static void lv_row_rect(struct lv *lv, const lv_geo *g, int i, RECT *rc)
{
    rc->left = -lv->scroll_x;
    rc->right = g->content_w - lv->scroll_x;
    rc->top = g->hh + (i - lv->top) * g->rowh;
    rc->bottom = rc->top + g->rowh;
}

static void lv_grid_rect(struct lv *lv, const lv_geo *g, int i, RECT *rc)
{
    int col = i / g->per, r = i % g->per;
    rc->left = col * g->cellw - lv->scroll_x;
    rc->right = rc->left + g->cellw;
    rc->top = r * g->cellh;
    rc->bottom = rc->top + g->cellh;
}

static void lv_item_rect(struct lv *lv, const lv_geo *g, int i, RECT *rc)
{
    if (lv->view == AW_LV_VIEW_DETAILS)
        lv_row_rect(lv, g, i, rc);
    else
        lv_grid_rect(lv, g, i, rc);
}

/* label cell: column 0 of a report row, or the whole cell in grid views */
static void lv_label_rect(struct lv *lv, const lv_geo *g, int i, RECT *rc)
{
    int p;
    if (lv->view != AW_LV_VIEW_DETAILS) {
        lv_grid_rect(lv, g, i, rc);
        return;
    }
    lv_row_rect(lv, g, i, rc);
    p = lv_pos_of_col(lv, 0);
    rc->left = (p >= 0 ? lv_col_x(lv, p) : 0) - lv->scroll_x;
    rc->right = rc->left + (p >= 0 ? aw_hdr_width(lv->header, 0) : 0);
}

/* highlight and focus rectangle: the full row with LVS_EX_FULLROWSELECT, else the label */
static void lv_select_rect(struct lv *lv, const lv_geo *g, int i, RECT *rc)
{
    if (lv->view == AW_LV_VIEW_DETAILS && (lv->exstyle & LVS_EX_FULLROWSELECT))
        lv_row_rect(lv, g, i, rc);
    else
        lv_label_rect(lv, g, i, rc);
}

static void lv_clamp_scroll(struct lv *lv, const lv_geo *g)
{
    int maxtop = lv->nrows - g->vis;
    int maxx = g->content_w - g->cli.right;
    if (maxtop < 0)
        maxtop = 0;
    if (maxx < 0)
        maxx = 0;
    if (lv->top > maxtop)
        lv->top = maxtop;
    if (lv->top < 0)
        lv->top = 0;
    if (lv->scroll_x > maxx)
        lv->scroll_x = maxx;
    if (lv->scroll_x < 0)
        lv->scroll_x = 0;
}

/* positions the header child: hidden outside report view, scrolled left with the list */
static void lv_place_header(struct lv *lv)
{
    RECT cli;
    int w, hh;
    if (!lv->header)
        return;
    if (lv->view != AW_LV_VIEW_DETAILS || (lv->style & LVS_NOCOLUMNHEADER)) {
        __aw_user32_ShowWindow(lv->header, SW_HIDE);
        return;
    }
    __aw_user32_GetClientRect(lv->hwnd, &cli);
    hh = aw_hdr_height(lv->header);
    w = aw_hdr_total_width(lv->header);
    if (w < cli.right)
        w = cli.right;
    __aw_user32_ShowWindow(lv->header, SW_SHOW);
    __aw_user32_MoveWindow(lv->header, -lv->scroll_x, 0, w, hh, TRUE);
}

static void lv_set_scroll_info(struct lv *lv, int bar, int nmax, int page, int pos)
{
    SCROLLINFO si;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = nmax < 0 ? 0 : nmax;
    si.nPage = page < 1 ? 1 : page;
    si.nPos = pos;
    __aw_user32_SetScrollInfo(lv->hwnd, bar, &si, TRUE);
}

/* works out which bars are needed, shows or hides them, then sets their ranges and the header */
static void lv_update_scrollbars(struct lv *lv)
{
    lv_geo g;
    int hneed, vneed;
    if (!lv->hwnd)
        return;
    if (lv->style & LVS_NOSCROLL) {
        lv->top = 0;
        lv->scroll_x = 0;
        lv_place_header(lv);
        return;
    }
    lv_get_geo(lv, &g);
    if (lv->view == AW_LV_VIEW_DETAILS) {
        hneed = g.content_w > g.cli.right;
        vneed = lv->nrows * g.rowh > g.cli.bottom - g.hh - (hneed ? LV_SB_W : 0);
        if (vneed && !hneed)
            hneed = g.content_w > g.cli.right - LV_SB_W;
    } else {
        hneed = g.content_w > g.cli.right;
        vneed = 0;
    }
    /* record the state before showing: ShowScrollBar may re-enter through WM_SIZE */
    if (vneed != lv->sbv) {
        lv->sbv = vneed;
        __aw_user32_ShowScrollBar(lv->hwnd, SB_VERT, vneed);
    }
    if (hneed != lv->sbh) {
        lv->sbh = hneed;
        __aw_user32_ShowScrollBar(lv->hwnd, SB_HORZ, hneed);
    }
    lv_get_geo(lv, &g); /* the bars may have changed the client size */
    lv_clamp_scroll(lv, &g);
    if (lv->view == AW_LV_VIEW_DETAILS)
        lv_set_scroll_info(lv, SB_VERT, lv->nrows - 1, g.vis, lv->top);
    lv_set_scroll_info(lv, SB_HORZ, g.content_w - 1, g.cli.right, lv->scroll_x);
    lv_place_header(lv);
}

static void lv_layout_changed(struct lv *lv)
{
    lv_touch(lv);
    lv_update_scrollbars(lv);
    __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
}

/* scrolls the item into view (report: rows; grid: columns) */
static void lv_ensure_visible(struct lv *lv, int idx)
{
    lv_geo g;
    if (idx < 0 || idx >= lv->nrows)
        return;
    lv_get_geo(lv, &g);
    if (lv->view == AW_LV_VIEW_DETAILS) {
        if (idx < lv->top)
            lv->top = idx;
        else if (idx >= lv->top + g.vis)
            lv->top = idx - g.vis + 1;
    } else {
        int left = (idx / g.per) * g.cellw;
        if (left < lv->scroll_x)
            lv->scroll_x = left;
        else if (left + g.cellw > lv->scroll_x + g.cli.right)
            lv->scroll_x = left + g.cellw - g.cli.right;
    }
    lv_update_scrollbars(lv);
}

/* ------------------------------------------------------------------ */
/* hit testing                                                         */
/* ------------------------------------------------------------------ */
/* item index under (x, y) or -1; *sub gets the logical column, *flags the LVHT_* bits */
static int lv_hit(struct lv *lv, int x, int y, int *sub, UINT *flags)
{
    lv_geo g;
    *sub = -1;
    *flags = LVHT_NOWHERE;
    lv_get_geo(lv, &g);
    if (lv->view == AW_LV_VIEW_DETAILS) {
        int row, xc;
        if (y < g.hh)
            return -1;
        row = lv->top + (y - g.hh) / g.rowh;
        if (row < 0 || row >= lv->nrows)
            return -1;
        xc = x + lv->scroll_x;
        if (xc < 0 || xc >= g.content_w)
            return -1;
        for (int p = 0; p < lv_ncols(lv); p++) {
            int left = lv_col_x(lv, p);
            int col = lv_col_at_pos(lv, p);
            if (xc >= left && xc < left + aw_hdr_width(lv->header, col)) {
                *sub = col;
                *flags = LVHT_ONITEMLABEL;
                if ((lv->exstyle & LVS_EX_CHECKBOXES) && col == 0 && xc >= left + 4 && xc < left + 4 + LV_CHECK_W)
                    *flags = LVHT_ONITEMSTATEICON;
                return row;
            }
        }
        return -1;
    } else {
        int col, r, idx;
        if (x + lv->scroll_x < 0 || y < 0)
            return -1;
        col = (x + lv->scroll_x) / g.cellw;
        r = y / g.cellh;
        if (r >= g.per)
            return -1;
        idx = col * g.per + r;
        if (idx < 0 || idx >= lv->nrows)
            return -1;
        *sub = 0;
        *flags = LVHT_ONITEMLABEL;
        return idx;
    }
}

/* ------------------------------------------------------------------ */
/* text access (callback text is fetched through LVN_GETDISPINFO)     */
/* ------------------------------------------------------------------ */
static const WCHAR *lv_callback_text(struct lv *lv, int idx, int sub, WCHAR *buf, int cap)
{
    buf[0] = 0;
    if (lv_uni(lv)) {
        NMLVDISPINFOW di;
        memset(&di, 0, sizeof di);
        di.item.mask = LVIF_TEXT;
        di.item.iItem = idx;
        di.item.iSubItem = sub;
        di.item.pszText = buf;
        di.item.cchTextMax = cap;
        lv_send(lv, &di.hdr, LVN_GETDISPINFOW);
        if (di.item.pszText && (intptr_t)di.item.pszText != -1)
            return di.item.pszText; /* borrowed: valid until the next call */
        return buf;
    } else {
        NMLVDISPINFOA di;
        char abuf[LV_TEXT_MAX * 4];
        const char *src;
        int k;
        memset(&di, 0, sizeof di);
        abuf[0] = 0;
        di.item.mask = LVIF_TEXT;
        di.item.iItem = idx;
        di.item.iSubItem = sub;
        di.item.pszText = abuf;
        di.item.cchTextMax = (int)sizeof abuf;
        lv_send(lv, &di.hdr, LVN_GETDISPINFOA);
        src = (di.item.pszText && (intptr_t)di.item.pszText != -1) ? di.item.pszText : abuf;
        k = aw_u2w_buf(src, -1, buf, cap - 1);
        if (k < 0)
            k = 0;
        if (k > cap - 1)
            k = cap - 1;
        buf[k] = 0;
        return buf;
    }
}

static const WCHAR *lv_text_for(struct lv *lv, int idx, int sub, WCHAR *buf, int cap)
{
    if (idx < 0 || idx >= lv->nrows)
        return NULL;
    if (lv->ownerdata)
        return lv_callback_text(lv, idx, sub, buf, cap);
    {
        const WCHAR *t = lv_row_text(lv->rows[idx], sub);
        if (t == LV_CB)
            return lv_callback_text(lv, idx, sub, buf, cap);
        return t;
    }
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    HDC hdc;
    HFONT font;
    HBRUSH bg, sel, selu, grid, cbfill, cbline, cbmark;
    COLORREF txt, bk;
} lv_pc;

static int lv_dt_align(int fmt)
{
    switch (fmt & LVCFMT_JUSTIFYMASK) {
    case LVCFMT_RIGHT:
        return DT_RIGHT;
    case LVCFMT_CENTER:
        return DT_CENTER;
    default:
        return DT_LEFT;
    }
}

static void lv_fill(lv_pc *pc, const RECT *rc, HBRUSH br)
{
    if (br && rc->right > rc->left && rc->bottom > rc->top)
        __aw_user32_FillRect(pc->hdc, rc, br);
}

static void lv_draw_check(lv_pc *pc, int x, int y, int checked)
{
    RECT b, in, mk;
    b.left = x;
    b.top = y;
    b.right = x + LV_CHECK_W;
    b.bottom = y + LV_CHECK_W;
    lv_fill(pc, &b, pc->cbline);
    in.left = b.left + 1;
    in.top = b.top + 1;
    in.right = b.right - 1;
    in.bottom = b.bottom - 1;
    lv_fill(pc, &in, pc->cbfill);
    if (checked) {
        mk.left = x + 3;
        mk.top = y + 3;
        mk.right = x + 10;
        mk.bottom = y + 10;
        lv_fill(pc, &mk, pc->cbmark);
    }
}

/* the custom-draw notification; fg/bk are in-out colours, NULL when not applicable */
static LRESULT lv_customdraw(struct lv *lv, lv_pc *pc, DWORD stage, int idx, int sub, const RECT *rc, UINT st,
                             COLORREF *fg, COLORREF *bk)
{
    NMLVCUSTOMDRAW cd;
    LRESULT r;
    memset(&cd, 0, sizeof cd);
    cd.nmcd.dwDrawStage = stage;
    cd.nmcd.hdc = pc->hdc;
    if (rc)
        cd.nmcd.rc = *rc;
    cd.nmcd.dwItemSpec = (DWORD_PTR)idx;
    cd.nmcd.uItemState = st;
    cd.nmcd.lItemlParam = lv_param_of(lv, idx);
    cd.clrText = fg ? *fg : CLR_DEFAULT;
    cd.clrTextBk = bk ? *bk : CLR_DEFAULT;
    cd.iSubItem = sub;
    r = lv_send(lv, &cd.nmcd.hdr, NM_CUSTOMDRAW);
    if (fg && cd.clrText != CLR_DEFAULT)
        *fg = cd.clrText;
    if (bk && cd.clrTextBk != CLR_DEFAULT)
        *bk = cd.clrTextBk;
    return r;
}

static void lv_paint_cell_text(struct lv *lv, lv_pc *pc, int idx, int col, const RECT *cell, COLORREF color)
{
    WCHAR buf[LV_TEXT_MAX];
    const WCHAR *t = lv_text_for(lv, idx, col, buf, LV_TEXT_MAX);
    RECT trc;
    int align, inset = 0;
    if (!t || !*t)
        return;
    if (lv->view == AW_LV_VIEW_DETAILS) {
        align = lv_dt_align(aw_hdr_fmt(lv->header, col));
        if (col == 0 && (lv->exstyle & LVS_EX_CHECKBOXES))
            inset = LV_CHECK_IN;
    } else {
        align = lv->view == AW_LV_VIEW_ICON ? DT_CENTER : DT_LEFT;
    }
    trc = *cell;
    trc.left += LV_PAD + inset;
    trc.right -= LV_PAD;
    if (trc.right <= trc.left)
        return;
    ctl_text(pc->hdc, pc->font, t, trc, (UINT)(align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS),
             color);
}

/* one report row: highlight, cells (checkbox, text, sub-item custom draw), gridlines, focus rect */
static void lv_paint_row(struct lv *lv, lv_pc *pc, const lv_geo *g, int i, int notify_sub)
{
    const lv_row *r = lv->rows[i];
    int selected = (r->state & LVIS_SELECTED) != 0;
    int focused = (r->state & LVIS_FOCUSED) != 0;
    HBRUSH hl = NULL;
    RECT row, sel, cell;
    COLORREF fg = pc->txt;
    int n = lv_ncols(lv);
    UINT st = (selected ? CDIS_SELECTED : 0) | ((focused && lv->has_focus) ? CDIS_FOCUS : 0);

    lv_row_rect(lv, g, i, &row);
    if (selected) {
        if (lv->has_focus)
            hl = pc->sel;
        else if (lv->style & LVS_SHOWSELALWAYS)
            hl = pc->selu;
    }
    if (hl) {
        lv_select_rect(lv, g, i, &sel);
        lv_fill(pc, &sel, hl);
        if (lv->has_focus)
            fg = __aw_user32_GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    if (selected && !lv->has_focus && (lv->style & LVS_SHOWSELALWAYS) == 0)
        fg = pc->txt;
    for (int p = 0; p < n; p++) {
        int col = lv_col_at_pos(lv, p);
        int left = lv_col_x(lv, p) - lv->scroll_x;
        int w = aw_hdr_width(lv->header, col);
        COLORREF cfg = fg;
        int skip = 0;
        if (left + w <= 0 || left >= g->cli.right)
            continue;
        cell.left = left;
        cell.right = left + w;
        cell.top = row.top;
        cell.bottom = row.bottom;
        if (notify_sub) {
            COLORREF cbk = pc->bk;
            LRESULT cr = lv_customdraw(lv, pc, CDDS_ITEMPREPAINT | CDDS_SUBITEM, i, col, &cell, st, &cfg, &cbk);
            if (cr & CDRF_SKIPDEFAULT)
                skip = 1;
        }
        if (!skip) {
            if (col == 0 && (lv->exstyle & LVS_EX_CHECKBOXES))
                lv_draw_check(pc, cell.left + 4, cell.top + (cell.bottom - cell.top - LV_CHECK_W) / 2,
                              ((r->state & LVIS_STATEIMAGEMASK) >> 12) == 2);
            lv_paint_cell_text(lv, pc, i, col, &cell, cfg);
        }
        if (lv->exstyle & LVS_EX_GRIDLINES)
            lv_fill(pc, &(RECT){cell.right - 1, row.top, cell.right, row.bottom}, pc->grid);
    }
    if (lv->exstyle & LVS_EX_GRIDLINES) {
        RECT bl;
        bl.left = row.left < 0 ? 0 : row.left;
        bl.right = row.right < g->cli.right ? row.right : g->cli.right;
        bl.top = row.bottom - 1;
        bl.bottom = row.bottom;
        lv_fill(pc, &bl, pc->grid);
    }
    if (focused && lv->has_focus) {
        lv_select_rect(lv, g, i, &sel);
        __aw_user32_DrawFocusRect(pc->hdc, &sel);
    }
}

/* one grid cell (list, small icon, icon): highlight, label and focus rect */
static void lv_paint_grid_item(struct lv *lv, lv_pc *pc, const lv_geo *g, int i, int notify)
{
    const lv_row *r = lv->rows[i];
    int selected = (r->state & LVIS_SELECTED) != 0;
    int focused = (r->state & LVIS_FOCUSED) != 0;
    RECT cell, hi;
    HBRUSH hl = NULL;
    COLORREF fg = pc->txt;
    UINT st = (selected ? CDIS_SELECTED : 0) | ((focused && lv->has_focus) ? CDIS_FOCUS : 0);
    int skip = 0;
    (void)notify;
    lv_grid_rect(lv, g, i, &cell);
    if (selected) {
        if (lv->has_focus)
            hl = pc->sel;
        else if (lv->style & LVS_SHOWSELALWAYS)
            hl = pc->selu;
    }
    if (hl) {
        lv_fill(pc, &cell, hl);
        if (lv->has_focus)
            fg = __aw_user32_GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    if (notify) {
        COLORREF cbk = pc->bk;
        LRESULT cr = lv_customdraw(lv, pc, CDDS_ITEMPREPAINT, i, 0, &cell, st, &fg, &cbk);
        if (cr & CDRF_SKIPDEFAULT)
            skip = 1;
    }
    if (!skip) {
        hi = cell;
        if (lv->view == AW_LV_VIEW_ICON)
            hi.top = cell.top + 2;
        lv_paint_cell_text(lv, pc, i, 0, &hi, fg);
    }
    if (focused && lv->has_focus)
        __aw_user32_DrawFocusRect(pc->hdc, &cell);
}

static void lv_paint(struct lv *lv, HDC hdc)
{
    lv_geo g;
    lv_pc pc;
    RECT rr;
    LRESULT cr;
    int notify_items = 0, notify_sub = 0;
    int first, last;
    COLORREF bkc = lv->clr_bk == CLR_NONE ? __aw_user32_GetSysColor(COLOR_WINDOW) : lv->clr_bk;

    memset(&pc, 0, sizeof pc);
    pc.hdc = hdc;
    pc.font = ctl_font(lv->hwnd);
    pc.bk = bkc;
    pc.txt = lv->clr_text == CLR_DEFAULT ? __aw_user32_GetSysColor(COLOR_WINDOWTEXT) : lv->clr_text;
    pc.bg = __aw_gdi32_CreateSolidBrush(bkc);
    pc.sel = __aw_gdi32_CreateSolidBrush(__aw_user32_GetSysColor(COLOR_HIGHLIGHT));
    pc.selu = __aw_gdi32_CreateSolidBrush(RGB(0xD9, 0xD9, 0xD9));
    pc.grid = __aw_gdi32_CreateSolidBrush(RGB(0xF0, 0xF0, 0xF0));
    pc.cbfill = __aw_gdi32_CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    pc.cbline = __aw_gdi32_CreateSolidBrush(RGB(0x80, 0x80, 0x80));
    pc.cbmark = __aw_gdi32_CreateSolidBrush(RGB(0x33, 0x33, 0x33));

    lv_get_geo(lv, &g);
    rr.left = 0;
    rr.top = g.hh;
    rr.right = g.cli.right;
    rr.bottom = g.cli.bottom;
    lv_fill(&pc, &rr, pc.bg);

    cr = lv_customdraw(lv, &pc, CDDS_PREPAINT, -1, -1, &g.cli, 0, NULL, NULL);
    if (!(cr & CDRF_SKIPDEFAULT)) {
        notify_items = (cr & CDRF_NOTIFYITEMDRAW) != 0;
        notify_sub = (cr & CDRF_NOTIFYSUBITEMDRAW) != 0;
        if (lv->view == AW_LV_VIEW_DETAILS) {
            first = lv->top;
            last = lv->top + g.vis + 1;
            if (last > lv->nrows)
                last = lv->nrows;
            for (int i = first; i < last; i++) {
                RECT ir;
                lv_row_rect(lv, &g, i, &ir);
                if (ir.bottom <= rr.top || ir.top >= rr.bottom)
                    continue;
                if (notify_items) {
                    UINT st = ((lv->rows[i]->state & LVIS_SELECTED) ? CDIS_SELECTED : 0);
                    COLORREF fgc = pc.txt, bkc2 = CLR_DEFAULT;
                    LRESULT ir2 = lv_customdraw(lv, &pc, CDDS_ITEMPREPAINT, i, -1, &ir, st, &fgc, &bkc2);
                    if (ir2 & CDRF_SKIPDEFAULT)
                        continue;
                    if (bkc2 != CLR_DEFAULT) {
                        HBRUSH b = __aw_gdi32_CreateSolidBrush(bkc2);
                        lv_fill(&pc, &ir, b);
                        __aw_gdi32_DeleteObject(b);
                    }
                    pc.txt = fgc;
                    lv_paint_row(lv, &pc, &g, i, notify_sub);
                    pc.txt = lv->clr_text == CLR_DEFAULT ? __aw_user32_GetSysColor(COLOR_WINDOWTEXT) : lv->clr_text;
                } else {
                    lv_paint_row(lv, &pc, &g, i, notify_sub);
                }
            }
        } else {
            int firstcol = g.cellw ? lv->scroll_x / g.cellw : 0;
            int lastcol = (lv->scroll_x + g.cli.right) / g.cellw + 1;
            first = firstcol * g.per;
            last = (lastcol + 1) * g.per;
            if (last > lv->nrows)
                last = lv->nrows;
            for (int i = first; i < last; i++) {
                RECT ir;
                lv_grid_rect(lv, &g, i, &ir);
                if (ir.right <= 0 || ir.left >= g.cli.right)
                    continue;
                lv_paint_grid_item(lv, &pc, &g, i, notify_items);
            }
        }
    }
    if (pc.bg)
        __aw_gdi32_DeleteObject(pc.bg);
    if (pc.sel)
        __aw_gdi32_DeleteObject(pc.sel);
    if (pc.selu)
        __aw_gdi32_DeleteObject(pc.selu);
    if (pc.grid)
        __aw_gdi32_DeleteObject(pc.grid);
    if (pc.cbfill)
        __aw_gdi32_DeleteObject(pc.cbfill);
    if (pc.cbline)
        __aw_gdi32_DeleteObject(pc.cbline);
    if (pc.cbmark)
        __aw_gdi32_DeleteObject(pc.cbmark);
}

static void lv_on_paint(struct lv *lv)
{
    PAINTSTRUCT ps;
    HDC hdc = __aw_user32_BeginPaint(lv->hwnd, &ps);
    if (hdc)
        lv_paint(lv, hdc);
    __aw_user32_EndPaint(lv->hwnd, &ps);
}

/* ------------------------------------------------------------------ */
/* items                                                               */
/* ------------------------------------------------------------------ */
/* normalized LVITEM (A and W layouts differ only in the text pointer type) */
typedef struct {
    UINT mask;
    int item, sub;
    UINT state, statemask;
    void *text;
    int cch;
    int image;
    LPARAM lparam;
    int indent;
} lv_item_io;

static void lv_item_in(lv_item_io *io, const void *p)
{
    const LVITEMW *w = p;
    io->mask = w->mask;
    io->item = w->iItem;
    io->sub = w->iSubItem;
    io->state = w->state;
    io->statemask = w->stateMask;
    io->text = w->pszText;
    io->cch = w->cchTextMax;
    io->image = w->iImage;
    io->lparam = w->lParam;
    io->indent = w->iIndent;
}

static void lv_item_out(const lv_item_io *io, void *p)
{
    LVITEMW *w = p;
    if (io->mask & LVIF_STATE)
        w->state = io->state;
    if (io->mask & LVIF_IMAGE)
        w->iImage = io->image;
    if (io->mask & LVIF_PARAM)
        w->lParam = io->lparam;
    if (io->mask & LVIF_INDENT)
        w->iIndent = io->indent;
}

static int lv_insert_item(struct lv *lv, const lv_item_io *io, int uni)
{
    lv_row *r;
    int cb = 0, idx = io->item;
    WCHAR *txt = NULL;
    UINT initial = 0, imask = 0;
    lv_ops ops;

    if (lv->ownerdata)
        return -1;
    r = lv_row_new();
    if (!r)
        return -1;
    if (io->mask & LVIF_TEXT) {
        txt = lv_text_in(io->text, uni, &cb);
        lv_row_set_sub(r, 0, cb ? LV_CB : txt);
    }
    if (io->mask & LVIF_PARAM)
        r->lparam = io->lparam;
    if (io->mask & LVIF_IMAGE)
        r->image = io->image;
    if (io->mask & LVIF_INDENT)
        r->indent = io->indent;
    if (lv->exstyle & LVS_EX_CHECKBOXES)
        r->state |= (UINT)1 << 12; /* unchecked by default */
    if (io->mask & LVIF_STATE) {
        imask = io->statemask;
        initial = io->state & io->statemask;
    }
    if (lv->sortmode)
        idx = lv_sorted_pos(lv, lv_label_for_model(r));
    if (idx < 0 || idx > lv->nrows)
        idx = lv->nrows;
    if (lv_insert_row(lv, idx, r) < 0) {
        lv_row_free(r);
        return -1;
    }
    {
        lv_nmitem_t n;
        memset(&n, 0, sizeof n);
        n.iItem = idx;
        n.lParam = r->lparam;
        lv_send(lv, &n.hdr, LVN_INSERTITEM);
    }
    if (imask && idx < lv->nrows) {
        ops = lv_make_ops(lv);
        lv_set_state(lv, &ops, idx, initial, imask);
    }
    lv_layout_changed(lv);
    return idx;
}

static int lv_set_item(struct lv *lv, const lv_item_io *io, int uni)
{
    lv_row *r;
    int idx = io->item, cb = 0;
    lv_ops ops;
    if (idx < 0 || idx >= lv->nrows)
        return 0;
    r = lv->rows[idx];
    if (io->mask & LVIF_TEXT) {
        WCHAR *t = lv_text_in(io->text, uni, &cb);
        lv_row_set_sub(r, io->sub, cb ? LV_CB : t);
    }
    if (io->mask & LVIF_PARAM)
        r->lparam = io->lparam;
    if (io->mask & LVIF_IMAGE)
        r->image = io->image;
    if (io->mask & LVIF_INDENT)
        r->indent = io->indent;
    if (io->mask & LVIF_STATE) {
        ops = lv_make_ops(lv);
        lv_set_state(lv, &ops, idx, io->state, io->statemask);
    }
    lv_layout_changed(lv);
    return 1;
}

static int lv_get_item(struct lv *lv, lv_item_io *io, int uni)
{
    lv_row *r;
    int idx = io->item;
    if (idx < 0 || idx >= lv->nrows)
        return 0;
    r = lv->rows[idx];
    if (io->mask & LVIF_TEXT) {
        WCHAR buf[LV_TEXT_MAX];
        const WCHAR *t = lv_text_for(lv, idx, io->sub, buf, LV_TEXT_MAX);
        lv_text_out(t, io->text, io->cch, uni);
    }
    if (io->mask & LVIF_PARAM)
        io->lparam = r->lparam;
    if (io->mask & LVIF_STATE)
        io->state = r->state & io->statemask;
    if (io->mask & LVIF_IMAGE)
        io->image = r->image;
    if (io->mask & LVIF_INDENT)
        io->indent = r->indent;
    return 1;
}

static void lv_delete_all(struct lv *lv)
{
    lv_nmitem_t n;
    LRESULT keep;
    memset(&n, 0, sizeof n);
    n.iItem = -1;
    keep = lv_send(lv, &n.hdr, LVN_DELETEALLITEMS);
    while (lv->nrows > 0) {
        lv_row *r;
        if (!keep) {
            lv_nmitem_t d;
            memset(&d, 0, sizeof d);
            d.iItem = 0;
            d.lParam = lv_param_of(lv, 0);
            lv_send(lv, &d.hdr, LVN_DELETEITEM);
        }
        r = lv_remove_row(lv, lv->nrows - 1);
        lv_row_free(r);
    }
    lv->focus = -1;
    lv->anchor = -1;
    lv_layout_changed(lv);
}

static int lv_delete_item(struct lv *lv, int idx)
{
    lv_nmitem_t n;
    lv_row *r;
    if (idx < 0 || idx >= lv->nrows)
        return 0;
    memset(&n, 0, sizeof n);
    n.iItem = idx;
    n.lParam = lv_param_of(lv, idx);
    lv_send(lv, &n.hdr, LVN_DELETEITEM);
    if (idx >= lv->nrows)
        return 0;
    {
        int was_focus = lv->focus == idx;
        r = lv_remove_row(lv, idx);
        lv_row_free(r);
        if (was_focus && lv->nrows > 0) {
            lv_ops ops = lv_make_ops(lv);
            lv_set_focus(lv, &ops, idx < lv->nrows ? idx : lv->nrows - 1);
        }
    }
    lv_layout_changed(lv);
    return 1;
}

static void lv_set_all_states(struct lv *lv, int idx, UINT bits, UINT mask)
{
    lv_ops ops = lv_make_ops(lv);
    if (idx >= 0) {
        lv_set_state(lv, &ops, idx, bits, mask);
        return;
    }
    for (int i = 0; i < lv->nrows; i++)
        lv_set_state(lv, &ops, i, bits, mask);
}

/* ------------------------------------------------------------------ */
/* columns (the header owns them)                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    UINT mask;
    int fmt;
    int cx;
    void *text;
    int cch;
    int sub;
    int image;
    int order;
} lv_col_io;

static void lv_col_in(lv_col_io *io, const void *p)
{
    const LVCOLUMNW *c = p;
    io->mask = c->mask;
    io->fmt = c->fmt;
    io->cx = c->cx;
    io->text = c->pszText;
    io->cch = c->cchTextMax;
    io->sub = c->iSubItem;
    io->image = c->iImage;
    io->order = c->iOrder;
}

static void lv_col_out(const lv_col_io *io, void *p)
{
    LVCOLUMNW *c = p;
    if (io->mask & LVCF_FMT)
        c->fmt = io->fmt;
    if (io->mask & LVCF_WIDTH)
        c->cx = io->cx;
    if (io->mask & LVCF_SUBITEM)
        c->iSubItem = io->sub;
    if (io->mask & LVCF_IMAGE)
        c->iImage = io->image;
    if (io->mask & LVCF_ORDER)
        c->iOrder = io->order;
}

static int lv_get_column(struct lv *lv, int col, lv_col_io *io, int uni)
{
    HDITEMW hi;
    WCHAR buf[LV_TEXT_MAX];
    if (col < 0 || col >= lv_ncols(lv))
        return 0;
    memset(&hi, 0, sizeof hi);
    hi.mask = HDI_FORMAT | HDI_WIDTH | HDI_TEXT | HDI_IMAGE | HDI_ORDER;
    hi.pszText = buf;
    hi.cchTextMax = LV_TEXT_MAX;
    __aw_user32_SendMessageW(lv->header, HDM_GETITEMW, (WPARAM)col, (LPARAM)&hi);
    io->fmt = hi.fmt;
    io->cx = hi.cxy;
    io->image = hi.iImage;
    io->order = lv_pos_of_col(lv, col);
    io->sub = col;
    if (io->mask & LVCF_TEXT)
        lv_text_out(buf, io->text, io->cch, uni);
    return 1;
}

static LRESULT lv_set_column(struct lv *lv, int col, const lv_col_io *io, int uni)
{
    HDITEMW hi;
    WCHAR *txt = NULL;
    int cb = 0;
    LRESULT r;
    if (col < 0 || col >= lv_ncols(lv))
        return 0;
    memset(&hi, 0, sizeof hi);
    if (io->mask & LVCF_FMT) {
        hi.mask |= HDI_FORMAT;
        hi.fmt = io->fmt;
    }
    if (io->mask & LVCF_WIDTH) {
        hi.mask |= HDI_WIDTH;
        hi.cxy = io->cx;
    }
    if (io->mask & LVCF_TEXT) {
        hi.mask |= HDI_TEXT;
        txt = lv_text_in(io->text, uni, &cb);
        hi.pszText = txt;
    }
    if (io->mask & LVCF_IMAGE) {
        hi.mask |= HDI_IMAGE;
        hi.iImage = io->image;
    }
    if (io->mask & LVCF_ORDER) {
        hi.mask |= HDI_ORDER;
        hi.iOrder = io->order;
    }
    r = __aw_user32_SendMessageW(lv->header, HDM_SETITEMW, (WPARAM)col, (LPARAM)&hi);
    free(txt);
    lv_layout_changed(lv);
    return r;
}

static int lv_insert_column(struct lv *lv, int idx, const lv_col_io *io, int uni)
{
    HDITEMW hi;
    WCHAR *txt = NULL;
    int cb = 0, got;
    memset(&hi, 0, sizeof hi);
    hi.mask = HDI_FORMAT;
    hi.fmt = io->fmt;
    if (io->mask & LVCF_WIDTH) {
        hi.mask |= HDI_WIDTH;
        hi.cxy = io->cx;
    }
    if (io->mask & LVCF_TEXT) {
        hi.mask |= HDI_TEXT;
        txt = lv_text_in(io->text, uni, &cb);
        hi.pszText = txt;
    }
    if (io->mask & LVCF_IMAGE) {
        hi.mask |= HDI_IMAGE;
        hi.iImage = io->image;
    }
    if (io->mask & LVCF_ORDER) {
        hi.mask |= HDI_ORDER;
        hi.iOrder = io->order;
    }
    got = (int)__aw_user32_SendMessageW(lv->header, HDM_INSERTITEMW, (WPARAM)(intptr_t)idx, (LPARAM)&hi);
    free(txt);
    if (got < 0)
        return -1;
    {
        /* sub-items of the new column start empty in every row */
        for (int i = 0; i < lv->nrows; i++)
            lv_row_insert_sub(lv->rows[i], got);
    }
    lv_layout_changed(lv);
    return got;
}

static int lv_delete_column(struct lv *lv, int col)
{
    if (col < 0 || col >= lv_ncols(lv))
        return 0;
    __aw_user32_SendMessageW(lv->header, HDM_DELETEITEM, (WPARAM)col, 0);
    for (int i = 0; i < lv->nrows; i++)
        lv_row_delete_sub(lv->rows[i], col);
    lv_layout_changed(lv);
    return 1;
}

/* LVSCW_AUTOSIZE / _USEHEADER width: widest cell text (and header text), plus the padding */
static int lv_autosize_width(struct lv *lv, int col, int use_header)
{
    WCHAR buf[LV_TEXT_MAX];
    int w = 0;
    for (int i = 0; i < lv->nrows; i++) {
        int tw = lv_text_width(lv, lv_text_for(lv, i, col, buf, LV_TEXT_MAX));
        if (tw > w)
            w = tw;
    }
    if (use_header) {
        int hw = lv_text_width(lv, aw_hdr_text(lv->header, col));
        if (aw_hdr_fmt(lv->header, col) & (HDF_SORTUP | HDF_SORTDOWN))
            hw += 12;
        if (hw > w)
            w = hw;
    }
    w += 2 * LV_PAD;
    if (col == 0 && (lv->exstyle & LVS_EX_CHECKBOXES))
        w += LV_CHECK_IN;
    return w;
}

static void lv_header_set_width(struct lv *lv, int col, int cx)
{
    HDITEMW hi;
    memset(&hi, 0, sizeof hi);
    hi.mask = HDI_WIDTH;
    hi.cxy = cx;
    __aw_user32_SendMessageW(lv->header, HDM_SETITEMW, (WPARAM)col, (LPARAM)&hi);
    lv_layout_changed(lv);
}

/* ------------------------------------------------------------------ */
/* scrolling, wheel, keyboard                                          */
/* ------------------------------------------------------------------ */
static void lv_scroll_by(struct lv *lv, int horiz, int delta)
{
    if (horiz || lv->view != AW_LV_VIEW_DETAILS)
        lv->scroll_x += delta;
    else
        lv->top += delta;
    lv_update_scrollbars(lv);
    __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
}

static void lv_wheel_msg(struct lv *lv, int delta, int horiz)
{
    int steps;
    lv->wheel += delta;
    steps = lv->wheel / 120;
    lv->wheel -= steps * 120;
    if (!steps)
        return;
    if (horiz)
        lv_scroll_by(lv, 1, steps * LV_WHEEL_PX);
    else if (lv->view == AW_LV_VIEW_DETAILS)
        lv_scroll_by(lv, 0, -steps * LV_WHEEL_LINES);
    else
        lv_scroll_by(lv, 1, -steps * LV_WHEEL_PX);
}

static void lv_scroll_msg(struct lv *lv, int horiz, WPARAM wp)
{
    lv_geo g;
    SCROLLINFO si;
    int code = LOWORD(wp), base, step, page, target, is_abs = 0;
    int max;
    lv_get_geo(lv, &g);
    horiz = horiz || lv->view != AW_LV_VIEW_DETAILS;
    base = horiz ? lv->scroll_x : lv->top;
    step = horiz ? LV_WHEEL_PX / 2 : 1;
    page = horiz ? g.cli.right : g.vis;
    max = horiz ? g.content_w - g.cli.right : lv->nrows - g.vis;
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
        __aw_user32_GetScrollInfo(lv->hwnd, horiz ? SB_HORZ : SB_VERT, &si);
        target = si.nTrackPos;
        is_abs = 1;
        break;
    case SB_TOP:
        target = 0;
        is_abs = 1;
        break;
    case SB_BOTTOM:
        target = max;
        is_abs = 1;
        break;
    default:
        return;
    }
    (void)is_abs;
    if (target < 0)
        target = 0;
    if (target > max)
        target = max;
    if (horiz)
        lv->scroll_x = target;
    else
        lv->top = target;
    lv_update_scrollbars(lv);
    __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
}

static void lv_key_down(struct lv *lv, UINT vk, LPARAM flags)
{
    lv_geo g;
    lv_ops ops;
    int cur, target = -1, singlesel = (lv->style & LVS_SINGLESEL) != 0;
    UINT mods = 0;
    NMLVKEYDOWN kd;
    lv_nmitem_t act;
    NMHDR nh;

    memset(&kd, 0, sizeof kd);
    kd.wVKey = (WORD)vk;
    kd.flags = (UINT)flags;
    lv_send(lv, &kd.hdr, LVN_KEYDOWN);

    if (__aw_user32_GetKeyState(VK_SHIFT) & 0x8000)
        mods |= MK_SHIFT;
    if (__aw_user32_GetKeyState(VK_CONTROL) & 0x8000)
        mods |= MK_CONTROL;
    lv_get_geo(lv, &g);
    ops = lv_make_ops(lv);
    cur = lv->focus >= 0 ? lv->focus : 0;
    if (lv->nrows == 0)
        return;
    switch (vk) {
    case VK_UP:
        target = lv->view == AW_LV_VIEW_ICON ? cur - g.per : cur - 1;
        break;
    case VK_DOWN:
        target = lv->view == AW_LV_VIEW_ICON ? cur + g.per : cur + 1;
        break;
    case VK_LEFT:
        if (lv->view == AW_LV_VIEW_DETAILS)
            return;
        target = lv->view == AW_LV_VIEW_ICON ? cur - 1 : cur - g.per;
        break;
    case VK_RIGHT:
        if (lv->view == AW_LV_VIEW_DETAILS)
            return;
        target = lv->view == AW_LV_VIEW_ICON ? cur + 1 : cur + g.per;
        break;
    case VK_HOME:
        target = 0;
        break;
    case VK_END:
        target = lv->nrows - 1;
        break;
    case VK_PRIOR:
        target = cur - g.vis;
        break;
    case VK_NEXT:
        target = cur + g.vis;
        break;
    case VK_SPACE:
        if (mods & MK_CONTROL) {
            if (lv->focus >= 0)
                lv_set_state(lv, &ops, lv->focus, (lv->rows[lv->focus]->state & LVIS_SELECTED) ? 0 : LVIS_SELECTED,
                             LVIS_SELECTED);
        } else if (lv->exstyle & LVS_EX_CHECKBOXES) {
            lv_toggle_check(lv, &ops, lv->focus);
        }
        return;
    case 'A':
        if ((mods & MK_CONTROL) && !singlesel) {
            lv_select_range(lv, &ops, 0, lv->nrows - 1);
            __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
        }
        return;
    case VK_RETURN:
        memset(&nh, 0, sizeof nh);
        lv_send(lv, &nh, NM_RETURN);
        if (lv->focus >= 0) {
            memset(&act, 0, sizeof act);
            act.iItem = lv->focus;
            act.uKeyFlags = mods;
            lv_send(lv, &act.hdr, LVN_ITEMACTIVATE);
        }
        return;
    default:
        return;
    }
    if (target < 0)
        target = 0;
    if (target > lv->nrows - 1)
        target = lv->nrows - 1;
    lv_move_focus(lv, &ops, target, mods, singlesel);
    lv_ensure_visible(lv, target);
    __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* mouse                                                               */
/* ------------------------------------------------------------------ */
static UINT lv_mods_of(WPARAM wp)
{
    return (UINT)wp & (MK_SHIFT | MK_CONTROL);
}

static void lv_lbutton_down(struct lv *lv, WPARAM wp, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp), sub = -1, idx;
    UINT flags, mods = lv_mods_of(wp);
    lv_ops ops;
    if (!lv->has_focus)
        __aw_user32_SetFocus(lv->hwnd);
    idx = lv_hit(lv, x, y, &sub, &flags);
    lv->mdown = 1;
    ops = lv_make_ops(lv);
    if (idx >= 0) {
        if (flags & LVHT_ONITEMSTATEICON)
            lv_toggle_check(lv, &ops, idx);
        else
            lv_click_item(lv, &ops, idx, mods, (lv->style & LVS_SINGLESEL) != 0);
    } else {
        lv_click_blank(lv, &ops, mods);
    }
    __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
}

/* sends NM_CLICK / NM_DBLCLK / NM_RCLICK / NM_RDBLCLK with an NMITEMACTIVATE */
static void lv_mouse_notify(struct lv *lv, UINT code, LPARAM lp, WPARAM wp)
{
    lv_nmitem_t n;
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp), sub = -1;
    UINT flags;
    int idx = lv_hit(lv, x, y, &sub, &flags);
    memset(&n, 0, sizeof n);
    n.iItem = idx;
    n.iSubItem = sub;
    n.ptAction.x = x;
    n.ptAction.y = y;
    n.uKeyFlags = (UINT)wp;
    if (idx >= 0)
        n.lParam = lv_param_of(lv, idx);
    lv_send(lv, &n.hdr, code);
}

static void lv_lbutton_up(struct lv *lv, WPARAM wp, LPARAM lp)
{
    if (!lv->mdown)
        return;
    lv->mdown = 0;
    lv_mouse_notify(lv, NM_CLICK, lp, wp);
}

static void lv_lbutton_dblclk(struct lv *lv, WPARAM wp, LPARAM lp)
{
    lv_nmitem_t n;
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp), sub = -1;
    UINT flags;
    int idx = lv_hit(lv, x, y, &sub, &flags);
    lv_mouse_notify(lv, NM_DBLCLK, lp, wp);
    if (idx >= 0) {
        memset(&n, 0, sizeof n);
        n.iItem = idx;
        n.iSubItem = sub;
        n.uKeyFlags = (UINT)wp;
        n.ptAction.x = x;
        n.ptAction.y = y;
        n.lParam = lv_param_of(lv, idx);
        lv_send(lv, &n.hdr, LVN_ITEMACTIVATE);
    }
}

/* the header's notifications (the header is our child, so they arrive here, not at the app) */
static LRESULT lv_header_notify(struct lv *lv, const NMHDR *n)
{
    const NMHEADERW *nh = (const NMHEADERW *)n;
    switch (n->code) {
    case HDN_ITEMCLICKW:
    case HDN_ITEMCLICKA: {
        lv_nmitem_t nm;
        memset(&nm, 0, sizeof nm);
        nm.iItem = -1;
        nm.iSubItem = nh->iItem;
        lv_send(lv, &nm.hdr, LVN_COLUMNCLICK);
        return 0;
    }
    case HDN_DIVIDERDBLCLICKW:
    case HDN_DIVIDERDBLCLICKA:
        lv_header_set_width(lv, nh->iItem, lv_autosize_width(lv, nh->iItem, 1));
        return 0;
    case HDN_ITEMCHANGEDW:
    case HDN_ITEMCHANGEDA:
    case HDN_TRACKW:
    case HDN_TRACKA:
    case HDN_ENDTRACKW:
    case HDN_ENDTRACKA:
        lv_layout_changed(lv);
        return 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* LVM_* and CCM_* messages                                            */
/* ------------------------------------------------------------------ */
static LRESULT lv_lvm(struct lv *lv, UINT msg, WPARAM wp, LPARAM lp, int *handled)
{
    int uni = 0;
    *handled = 1;
    switch (msg) {
    /* ---- items ---- */
    case LVM_GETITEMCOUNT:
        return lv->nrows;
    case LVM_INSERTITEMA:
    case LVM_INSERTITEMW: {
        lv_item_io io;
        uni = msg == LVM_INSERTITEMW;
        lv_item_in(&io, (const void *)lp);
        return lv_insert_item(lv, &io, uni);
    }
    case LVM_SETITEMA:
    case LVM_SETITEMW: {
        lv_item_io io;
        uni = msg == LVM_SETITEMW;
        lv_item_in(&io, (const void *)lp);
        return lv_set_item(lv, &io, uni);
    }
    case LVM_GETITEMA:
    case LVM_GETITEMW: {
        lv_item_io io;
        uni = msg == LVM_GETITEMW;
        lv_item_in(&io, (const void *)lp);
        if (!lv_get_item(lv, &io, uni))
            return 0;
        lv_item_out(&io, (void *)lp);
        return 1;
    }
    case LVM_SETITEMTEXTA:
    case LVM_SETITEMTEXTW: {
        lv_item_io io;
        uni = msg == LVM_SETITEMTEXTW;
        lv_item_in(&io, (const void *)lp);
        io.item = (int)wp; /* the index is wParam for the *ITEMTEXT messages */
        io.mask = LVIF_TEXT;
        return lv_set_item(lv, &io, uni);
    }
    case LVM_GETITEMTEXTA:
    case LVM_GETITEMTEXTW: {
        lv_item_io io;
        uni = msg == LVM_GETITEMTEXTW;
        lv_item_in(&io, (const void *)lp);
        io.item = (int)wp;
        io.mask = LVIF_TEXT;
        return lv_get_item(lv, &io, uni);
    }
    case LVM_DELETEITEM:
        return lv_delete_item(lv, (int)wp);
    case LVM_DELETEALLITEMS:
        lv_delete_all(lv);
        return 1;
    case LVM_GETITEMSTATE: {
        if ((int)wp < 0 || (int)wp >= lv->nrows)
            return 0;
        return lv->rows[(int)wp]->state & (UINT)lp;
    }
    case LVM_SETITEMSTATE: {
        const LVITEMW *it = (const LVITEMW *)lp;
        if (!it)
            return 0;
        lv_set_all_states(lv, (int)wp, it->state, it->stateMask);
        lv_layout_changed(lv);
        return 1;
    }
    case LVM_GETSELECTEDCOUNT:
        return lv_selected_count(lv);
    case LVM_GETNEXTITEM:
        return lv_next_item(lv, (int)wp, (UINT)lp);
    case LVM_FINDITEMA:
    case LVM_FINDITEMW: {
        const lv_findinfo_t *fi = (const lv_findinfo_t *)lp;
        WCHAR *s = NULL;
        int r;
        uni = msg == LVM_FINDITEMW;
        if (!fi)
            return -1;
        if (!(fi->flags & LVFI_PARAM) && fi->psz)
            s = uni ? lv_wdup((const WCHAR *)fi->psz) : aw_u2w((const char *)fi->psz, -1);
        r = lv_find_item(lv, (int)wp, fi->flags, s, fi->lParam);
        free(s);
        return r;
    }
    case LVM_SETITEMCOUNT:
        if (lv->ownerdata) {
            int want = (int)wp;
            while (lv->nrows > want) {
                lv_row_free(lv_remove_row(lv, lv->nrows - 1));
            }
            while (lv->nrows < want) {
                lv_row *r = lv_row_new();
                if (!r || lv_insert_row(lv, lv->nrows, r) < 0) {
                    lv_row_free(r);
                    break;
                }
            }
            lv_layout_changed(lv);
        }
        return 1;
    case LVM_SORTITEMS:
    case LVM_SORTITEMSEX: {
        lv_cmp_fn cmp = (lv_cmp_fn)(intptr_t)lp;
        if (!cmp)
            return 0;
        if (!lv_sort_rows(lv, cmp, (LPARAM)wp, msg == LVM_SORTITEMSEX))
            return 0;
        if (aw_debug)
            for (int i = 0; i < lv->nrows; i++) {
                const WCHAR *rt = lv_row_text(lv->rows[i], 1);
                static const WCHAR none[1] = {0};
                char *t = aw_w2u(rt ? rt : none, -1);
                TRACE("listview sorted row %d title=\"%s\"", i, t ? t : "");
                free(t);
            }
        lv_layout_changed(lv);
        return 1;
    }
    case LVM_GETITEMRECT: {
        RECT *rc = (RECT *)lp;
        lv_geo g;
        int code = rc ? (int)rc->left : LVIR_BOUNDS, i = (int)wp;
        RECT r;
        if (!rc || i < 0 || i >= lv->nrows)
            return 0;
        lv_get_geo(lv, &g);
        lv_item_rect(lv, &g, i, &r);
        if (code == LVIR_LABEL) {
            lv_label_rect(lv, &g, i, &r);
        } else if (code == LVIR_SELECTBOUNDS) {
            lv_select_rect(lv, &g, i, &r);
        } else if (code == LVIR_ICON) {
            r.right = r.left + 16 + 4;
            if (lv->view == AW_LV_VIEW_ICON)
                r.bottom = r.top + (r.bottom - r.top) / 2;
        }
        *rc = r;
        return 1;
    }
    case LVM_GETSUBITEMRECT: {
        RECT *rc = (RECT *)lp;
        lv_geo g;
        int i = (int)wp, sub, p;
        if (!rc || i < 0 || i >= lv->nrows || lv->view != AW_LV_VIEW_DETAILS)
            return 0;
        sub = (int)rc->top; /* input: sub-item in top, LVIR_* code in left */
        p = lv_pos_of_col(lv, sub);
        if (p < 0)
            return 0;
        lv_get_geo(lv, &g);
        lv_row_rect(lv, &g, i, rc);
        rc->left = lv_col_x(lv, p) - lv->scroll_x;
        rc->right = rc->left + aw_hdr_width(lv->header, sub);
        return 1;
    }
    case LVM_GETITEMPOSITION: {
        POINT *pt = (POINT *)lp;
        lv_geo g;
        RECT r;
        if (!pt || (int)wp < 0 || (int)wp >= lv->nrows)
            return 0;
        lv_get_geo(lv, &g);
        lv_item_rect(lv, &g, (int)wp, &r);
        pt->x = r.left;
        pt->y = r.top;
        return 1;
    }
    case LVM_SETITEMPOSITION:
    case LVM_SETITEMPOSITION32:
        return 1; /* positions are computed by the view */
    case LVM_HITTEST:
    case LVM_SUBITEMHITTEST: {
        LVHITTESTINFO *ht = (LVHITTESTINFO *)lp;
        int sub = -1, idx;
        UINT flags;
        if (!ht)
            return -1;
        idx = lv_hit(lv, ht->pt.x, ht->pt.y, &sub, &flags);
        ht->flags = flags;
        ht->iItem = idx;
        ht->iSubItem = idx >= 0 ? sub : 0;
        if (idx < 0) {
            ht->flags = LVHT_NOWHERE;
            ht->iSubItem = 0;
            return -1;
        }
        return idx;
    }
    case LVM_ENSUREVISIBLE:
        lv_ensure_visible(lv, (int)wp);
        return 1;
    case LVM_GETTOPINDEX: {
        lv_geo g;
        lv_get_geo(lv, &g);
        if (lv->view == AW_LV_VIEW_DETAILS)
            return lv->top;
        return (lv->scroll_x / g.cellw) * g.per;
    }
    case LVM_GETCOUNTPERPAGE: {
        lv_geo g;
        lv_get_geo(lv, &g);
        return lv->view == AW_LV_VIEW_DETAILS ? g.vis : g.per * (g.cli.right / g.cellw + 1);
    }
    case LVM_GETORIGIN: {
        POINT *pt = (POINT *)lp;
        if (!pt)
            return 0;
        pt->x = lv->scroll_x;
        pt->y = lv->view == AW_LV_VIEW_DETAILS ? lv->top * lv_row_height(lv) : 0;
        return 1;
    }
    case LVM_GETVIEWRECT: {
        RECT *rc = (RECT *)lp;
        lv_geo g;
        if (!rc)
            return 0;
        lv_get_geo(lv, &g);
        rc->left = 0;
        rc->top = 0;
        rc->right = g.content_w;
        rc->bottom = lv->view == AW_LV_VIEW_DETAILS ? lv->nrows * g.rowh + g.hh : g.per * g.cellh;
        return 1;
    }
    case LVM_APPROXIMATEVIEWRECT: {
        lv_geo g;
        int cnt = (int)wp, w, h;
        lv_get_geo(lv, &g);
        if (cnt < 0)
            cnt = lv->nrows;
        if (lv->view == AW_LV_VIEW_DETAILS) {
            w = g.content_w;
            h = cnt * g.rowh;
        } else {
            w = ((cnt + g.per - 1) / g.per) * g.cellw;
            h = g.per * g.cellh;
        }
        if (lp != 0 && (int)(lp & 0xFFFF) > 0)
            w = (int)(lp & 0xFFFF);
        return (LRESULT)(((UINT)h << 16) | ((UINT)w & 0xFFFF));
    }
    case LVM_SCROLL: { /* wParam = dx pixels, lParam = dy pixels */
        lv_geo g;
        lv_get_geo(lv, &g);
        if (lv->view == AW_LV_VIEW_DETAILS)
            lv->top += (int)lp / g.rowh;
        else
            lv->scroll_x += (int)lp;
        lv->scroll_x += (lv->view == AW_LV_VIEW_DETAILS) ? (int)wp : 0;
        lv_update_scrollbars(lv);
        __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
        return 1;
    }
    case LVM_REDRAWITEMS:
    case LVM_UPDATE:
        __aw_user32_InvalidateRect(lv->hwnd, NULL, FALSE);
        return 1;
    case LVM_ARRANGE:
        lv_layout_changed(lv);
        return 1;
    case LVM_GETSTRINGWIDTHA:
    case LVM_GETSTRINGWIDTHW: {
        WCHAR *s;
        int w;
        uni = msg == LVM_GETSTRINGWIDTHW;
        s = uni ? lv_wdup((const WCHAR *)lp) : aw_u2w((const char *)lp, -1);
        w = lv_text_width(lv, s);
        free(s);
        return w;
    }
    case LVM_GETISEARCHSTRINGA:
    case LVM_GETISEARCHSTRINGW:
        return 0;
    case LVM_EDITLABELA:
    case LVM_EDITLABELW:
    case LVM_GETEDITCONTROL:
        return 0;
    case LVM_GETSELECTIONMARK:
        return lv->anchor;
    case LVM_SETSELECTIONMARK: {
        int old = lv->anchor;
        lv->anchor = (int)lp;
        return old;
    }
    case LVM_GETCALLBACKMASK:
    case LVM_SETCALLBACKMASK:
        return 0;

    /* ---- columns ---- */
    case LVM_INSERTCOLUMNA:
    case LVM_INSERTCOLUMNW: {
        lv_col_io io;
        uni = msg == LVM_INSERTCOLUMNW;
        lv_col_in(&io, (const void *)lp);
        return lv_insert_column(lv, (int)wp, &io, uni);
    }
    case LVM_GETCOLUMNA:
    case LVM_GETCOLUMNW: {
        lv_col_io io;
        uni = msg == LVM_GETCOLUMNW;
        lv_col_in(&io, (const void *)lp);
        if (!lv_get_column(lv, (int)wp, &io, uni))
            return 0;
        lv_col_out(&io, (void *)lp);
        return 1;
    }
    case LVM_SETCOLUMNA:
    case LVM_SETCOLUMNW: {
        lv_col_io io;
        uni = msg == LVM_SETCOLUMNW;
        lv_col_in(&io, (const void *)lp);
        return lv_set_column(lv, (int)wp, &io, uni) != 0;
    }
    case LVM_DELETECOLUMN:
        return lv_delete_column(lv, (int)wp);
    case LVM_GETCOLUMNWIDTH:
        return aw_hdr_width(lv->header, (int)wp);
    case LVM_SETCOLUMNWIDTH: {
        int col = (int)wp, cx = (int)lp, w = cx;
        if (col < 0 || col >= lv_ncols(lv))
            return 0;
        if (cx == LVSCW_AUTOSIZE)
            w = lv_autosize_width(lv, col, 0);
        else if (cx == LVSCW_AUTOSIZE_USEHEADER)
            w = lv_autosize_width(lv, col, 1);
        lv_header_set_width(lv, col, w);
        return 1;
    }
    case LVM_GETHEADER:
        return (LRESULT)(intptr_t)lv->header;
    case LVM_GETCOLUMNORDERARRAY:
        return __aw_user32_SendMessageW(lv->header, HDM_GETORDERARRAY, wp, lp);
    case LVM_SETCOLUMNORDERARRAY:
        lv_layout_changed(lv);
        return __aw_user32_SendMessageW(lv->header, HDM_SETORDERARRAY, wp, lp);

    /* ---- appearance and styles ---- */
    case LVM_GETBKCOLOR:
        return lv->clr_bk;
    case LVM_SETBKCOLOR:
        lv->clr_bk = (COLORREF)lp;
        __aw_user32_InvalidateRect(lv->hwnd, NULL, TRUE);
        return 1;
    case LVM_GETTEXTCOLOR:
        return lv->clr_text;
    case LVM_SETTEXTCOLOR:
        lv->clr_text = (COLORREF)lp;
        __aw_user32_InvalidateRect(lv->hwnd, NULL, TRUE);
        return 1;
    case LVM_GETTEXTBKCOLOR:
        return lv->clr_textbk;
    case LVM_SETTEXTBKCOLOR:
        lv->clr_textbk = (COLORREF)lp;
        __aw_user32_InvalidateRect(lv->hwnd, NULL, TRUE);
        return 1;
    case LVM_GETIMAGELIST: {
        int t = (int)wp;
        return (t >= 0 && t < 4) ? (LRESULT)(intptr_t)lv->imagelists[t] : 0;
    }
    case LVM_SETIMAGELIST: {
        int t = (int)wp;
        HANDLE old = NULL;
        if (t >= 0 && t < 4) {
            old = lv->imagelists[t];
            lv->imagelists[t] = (HANDLE)lp;
        }
        __aw_user32_InvalidateRect(lv->hwnd, NULL, TRUE);
        return (LRESULT)(intptr_t)old;
    }
    case LVM_GETEXTENDEDLISTVIEWSTYLE:
        return lv->exstyle;
    case LVM_SETEXTENDEDLISTVIEWSTYLE: {
        DWORD old = lv->exstyle;
        DWORD mask = (DWORD)wp ? (DWORD)wp : 0xFFFFFFFFu;
        lv->exstyle = (lv->exstyle & ~mask) | ((DWORD)lp & mask);
        lv_layout_changed(lv);
        return old;
    }
    case LVM_GETITEMSPACING: {
        lv_geo g;
        lv_get_geo(lv, &g);
        return (LRESULT)(((UINT)g.cellh << 16) | ((UINT)g.cellw & 0xFFFF));
    }
    case LVM_SETICONSPACING: {
        int old = lv->iconspacing;
        lv->iconspacing = (int)lp;
        return old;
    }
    case LVM_GETHOTITEM:
        return lv->hotitem;
    case LVM_SETHOTITEM: {
        int old = lv->hotitem;
        lv->hotitem = (int)wp;
        return old;
    }
    case LVM_GETHOTCURSOR:
        return (LRESULT)(intptr_t)lv->hotcursor;
    case LVM_SETHOTCURSOR: {
        HANDLE old = lv->hotcursor;
        lv->hotcursor = (HANDLE)lp;
        return (LRESULT)(intptr_t)old;
    }
    case LVM_GETTOOLTIPS:
        return (LRESULT)(intptr_t)lv->tooltips;
    case LVM_SETTOOLTIPS: {
        HWND old = lv->tooltips;
        lv->tooltips = (HWND)(intptr_t)wp;
        return (LRESULT)(intptr_t)old;
    }
    case LVM_SETVIEW: {
        int v = (int)wp;
        if (v == 4) /* LV_VIEW_TILE: shown as list */
            v = AW_LV_VIEW_LIST;
        if (v < 0 || v > 3)
            return -1;
        lv->view = v;
        lv->top = 0;
        lv->scroll_x = 0;
        lv_layout_changed(lv);
        return 0;
    }
    case LVM_GETVIEW:
        return lv->view;
    case CCM_SETVERSION: {
        DWORD old = lv->ccver;
        lv->ccver = (DWORD)wp;
        return old;
    }
    case CCM_GETVERSION:
        return lv->ccver;
    case CCM_SETUNICODEFORMAT: {
        BOOL old = ctl_is_unicode_notify(lv->hwnd);
        ctl_set_notify_unicode(lv->hwnd, (int)wp);
        return old;
    }
    case CCM_GETUNICODEFORMAT:
        return ctl_is_unicode_notify(lv->hwnd);
    case CCM_SETBKCOLOR:
        lv->clr_bk = (COLORREF)lp;
        __aw_user32_InvalidateRect(lv->hwnd, NULL, TRUE);
        return 1;
    default:
        break;
    }
    *handled = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */
static int lv_create(HWND hwnd, struct lv *lv)
{
    static const WCHAR hdr_class[] = {'S', 'y', 's', 'H', 'e', 'a', 'd', 'e', 'r', '3', '2', 0};
    DWORD hs = WS_CHILD | HDS_HORZ;
    lv->hwnd = hwnd;
    lv->style = (DWORD)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
    lv->view = (int)(lv->style & LVS_TYPEMASK);
    lv->focus = -1;
    lv->anchor = -1;
    lv->hotitem = -1;
    lv->clr_bk = CLR_NONE;
    lv->clr_text = CLR_DEFAULT;
    lv->clr_textbk = CLR_NONE;
    lv->sortmode = (lv->style & LVS_SORTASCENDING) ? 1 : (lv->style & LVS_SORTDESCENDING) ? 2 : 0;
    lv->ownerdata = (lv->style & LVS_OWNERDATA) != 0;
    if (!(lv->style & LVS_NOSORTHEADER))
        hs |= HDS_BUTTONS;
    lv->header = __aw_user32_CreateWindowExW(0, hdr_class, NULL, hs, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
    if (!lv->header)
        return -1;
    lv_place_header(lv);
    return 0;
}

static void lv_destroy(struct lv *lv)
{
    for (int i = 0; i < lv->nrows; i++)
        lv_row_free(lv->rows[i]);
    free(lv->rows);
    lv->rows = NULL;
    lv->nrows = lv->caprows = 0;
}

LRESULT WINAPI aw_listview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct lv *lv = ctl_get(hwnd);
    LRESULT res;

    if (aw_debug && msg == LVM_SETITEMTEXTA && lp && ((const LVITEMA *)lp)->iSubItem == 1)
        TRACE("listview row %d title=\"%s\"", (int)wp,
              ((const LVITEMA *)lp)->pszText ? ((const LVITEMA *)lp)->pszText : "");

    if (msg == WM_CREATE) {
        lv = ctl_alloc(hwnd, sizeof(struct lv));
        if (!lv)
            return -1;
        return lv_create(hwnd, lv) ? -1 : 0;
    }
    if (msg == WM_NCDESTROY) {
        if (lv)
            lv_destroy(lv);
        ctl_free(hwnd);
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!lv)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_SETFONT) {
        if (lv->header)
            __aw_user32_SendMessageW(lv->header, WM_SETFONT, wp, lp);
        lv_layout_changed(lv);
        return 0;
    }
    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;

    switch (msg) {
    case WM_PAINT:
        lv_on_paint(lv);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
    case WM_STYLECHANGED:
        lv->style = (DWORD)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
        lv_layout_changed(lv);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        lv->has_focus = 1;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        {
            NMHDR n;
            memset(&n, 0, sizeof n);
            lv_send(lv, &n, NM_SETFOCUS);
        }
        return 0;
    case WM_KILLFOCUS:
        lv->has_focus = 0;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        {
            NMHDR n;
            memset(&n, 0, sizeof n);
            lv_send(lv, &n, NM_KILLFOCUS);
        }
        return 0;
    case WM_CAPTURECHANGED:
        lv->mdown = 0;
        return 0;
    case WM_KEYDOWN:
        lv_key_down(lv, (UINT)wp, lp);
        return 0;
    case WM_LBUTTONDOWN:
        lv_lbutton_down(lv, wp, lp);
        return 0;
    case WM_LBUTTONUP:
        lv_lbutton_up(lv, wp, lp);
        return 0;
    case WM_LBUTTONDBLCLK:
        lv_lbutton_dblclk(lv, wp, lp);
        return 0;
    case WM_RBUTTONUP:
        lv_mouse_notify(lv, NM_RCLICK, lp, wp);
        return 0;
    case WM_RBUTTONDBLCLK:
        lv_mouse_notify(lv, NM_RDBLCLK, lp, wp);
        return 0;
    case WM_MOUSEWHEEL:
        lv_wheel_msg(lv, (short)HIWORD(wp), 0);
        return 0;
    case WM_MOUSEHWHEEL:
        lv_wheel_msg(lv, (short)HIWORD(wp), 1);
        return 0;
    case WM_VSCROLL:
        lv_scroll_msg(lv, 0, wp);
        return 0;
    case WM_HSCROLL:
        lv_scroll_msg(lv, 1, wp);
        return 0;
    case WM_NOTIFY: {
        const NMHDR *n = (const NMHDR *)lp;
        if (lv->header && n->hwndFrom == lv->header)
            return lv_header_notify(lv, n);
        return 0;
    }
    default:
        break;
    }
    {
        int handled = 0;
        LRESULT r = lv_lvm(lv, msg, wp, lp, &handled);
        if (handled)
            return r;
    }
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}

/* the header is the child that owns column data */
#endif /* AW_LV_UNIT_TEST */
