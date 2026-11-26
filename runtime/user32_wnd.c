/* user32 core - window objects: table, creation/destruction, text, data, hierarchy, lookup (S5).
 * Top-level windows own an X window and a back buffer; child windows live inside their top-level's
 * back buffer (see user32_geom.c for coordinates and clipping). */
#include "user32_int.h"
#include <unistd.h>

pthread_mutex_t aw_wnd_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t aw_send_cond = PTHREAD_COND_INITIALIZER;
int aw_display;
HWND aw_desktop_hwnd;

#define ERR_INVALID_WINDOW 1400
#define ERR_CLASS_DOES_NOT_EXIST 1412
#define ERR_NOT_ENOUGH_MEMORY 8
#define ERR_INVALID_PARAMETER 87
#define ERR_GEN_FAILURE 31

static aw_win g_windows[AW_MAX_WINDOWS];
static int g_cascade;
static int g_init_state; /* 0 not started, 1 running (recursive calls return), 2 done */

aw_win *aw_win_slot(int i)
{
    return (i >= 0 && i < AW_MAX_WINDOWS) ? &g_windows[i] : NULL;
}

aw_win *aw_win_from_hwnd(HWND h)
{
    uintptr_t v = (uintptr_t)h;
    if (!v || (v & 0xF) != 8 || (v >> 32))
        return NULL;
    aw_win *w = &g_windows[(v >> 4) & 0xFFF];
    if (w->used && !w->destroyed && w->hwnd == h)
        return w;
    return NULL;
}

aw_win *aw_win_from_xwin(uint32_t xwin)
{
    if (!xwin)
        return NULL;
    for (int i = 0; i < AW_MAX_WINDOWS; i++)
        if (g_windows[i].used && !g_windows[i].destroyed && g_windows[i].xwin == xwin)
            return &g_windows[i];
    return NULL;
}

int aw_hwnd_valid(HWND h) { return aw_win_from_hwnd(h) != NULL; }

aw_win *aw_top_ancestor(aw_win *w)
{
    for (int guard = 0; w && w->parent && guard < AW_MAX_WINDOWS; guard++) {
        aw_win *p = aw_win_from_hwnd(w->parent);
        if (!p)
            break;
        w = p;
    }
    return w;
}

aw_win *aw_win_alloc(void)
{
    pthread_mutex_lock(&aw_wnd_lock);
    for (int i = 0; i < AW_MAX_WINDOWS; i++) {
        aw_win *w = &g_windows[i];
        if (w->used)
            continue;
        uint16_t gen = (uint16_t)(w->gen + 1);
        if (!gen)
            gen = 1;
        memset(w, 0, sizeof *w);
        w->gen = gen;
        w->used = 1;
        w->hwnd = (HWND)(uintptr_t)(((uint32_t)gen << 16) | ((uint32_t)i << 4) | 8u);
        w->tid = aw_current_tid();
        w->enabled = 1;
        rgn_init(&w->update);
        rgn_init(&w->clip);
        pthread_mutex_unlock(&aw_wnd_lock);
        return w;
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    return NULL;
}

void aw_win_free(aw_win *w)
{
    aw_scroll_free(w);
    struct aw_prop *p = w->props;
    while (p) {
        struct aw_prop *n = p->next;
        free(p->name);
        free(p);
        p = n;
    }
    free(w->text);
    free(w->extra);
    rgn_free(&w->update);
    rgn_free(&w->clip);
    if (w->surf)
        surface_free(w->surf);
    if (w->xwin)
        plat_window_destroy(w->xwin);
    uint16_t gen = w->gen;
    memset(w, 0, sizeof *w);
    w->gen = gen; /* keeps the next allocation of this slot distinct from stale handles */
}

/* PLAT_WIN_* kind of a top-level window (gui.h rules). */
int aw_window_kind(const aw_win *w)
{
    if ((w->style & WS_POPUP) && !(w->style & WS_CAPTION))
        return PLAT_WIN_POPUP;
    if (w->owner && (w->style & WS_POPUP) && (w->style & WS_CAPTION))
        return PLAT_WIN_DIALOG;
    return PLAT_WIN_NORMAL;
}

/* ------------------------------------------------------------------ hierarchy */
void aw_unlink_child(aw_win *w)
{
    aw_win *p = w->parent ? aw_win_from_hwnd(w->parent) : NULL;
    aw_win *pv = w->prev_sib ? aw_win_from_hwnd(w->prev_sib) : NULL;
    aw_win *nx = w->next_sib ? aw_win_from_hwnd(w->next_sib) : NULL;
    if (pv)
        pv->next_sib = w->next_sib;
    if (nx)
        nx->prev_sib = w->prev_sib;
    if (p) {
        if (p->first_child == w->hwnd)
            p->first_child = w->next_sib;
        if (p->last_child == w->hwnd)
            p->last_child = w->prev_sib;
    }
    w->parent = NULL;
    w->prev_sib = w->next_sib = NULL;
}

/* Child z-order: first_child is the topmost sibling. `after` follows SetWindowPos/CreateWindow rules. */
void aw_link_child(aw_win *p, aw_win *w, HWND after)
{
    w->parent = p->hwnd;
    w->prev_sib = w->next_sib = NULL;
    aw_win *a = NULL;
    if (after == HWND_BOTTOM) {
        a = p->last_child ? aw_win_from_hwnd(p->last_child) : NULL;
    } else if (after && after != HWND_TOP && after != HWND_TOPMOST && after != HWND_NOTOPMOST) {
        a = aw_win_from_hwnd(after);
        if (a && a->parent != p->hwnd)
            a = NULL;
    }
    if (a && a != w) {
        w->prev_sib = a->hwnd;
        w->next_sib = a->next_sib;
        aw_win *nx = a->next_sib ? aw_win_from_hwnd(a->next_sib) : NULL;
        if (nx)
            nx->prev_sib = w->hwnd;
        else
            p->last_child = w->hwnd;
        a->next_sib = w->hwnd;
        return;
    }
    /* front (topmost) */
    w->next_sib = p->first_child;
    if (p->first_child) {
        aw_win *f = aw_win_from_hwnd(p->first_child);
        if (f)
            f->prev_sib = w->hwnd;
    }
    p->first_child = w->hwnd;
    if (!p->last_child)
        p->last_child = w->hwnd;
}

/* ------------------------------------------------------------------ text */
void aw_win_set_text(aw_win *w, const WCHAR *s, int len)
{
    if (!s)
        len = 0;
    else if (len < 0)
        len = (int)aw_wcslen(s);
    if (len + 1 > w->text_cap) {
        WCHAR *nt = realloc(w->text, (size_t)(len + 1) * sizeof(WCHAR));
        if (!nt)
            return;
        w->text = nt;
        w->text_cap = len + 1;
    }
    if (len)
        memcpy(w->text, s, (size_t)len * sizeof(WCHAR));
    w->text[len] = 0;
    w->text_len = len;
}

void aw_update_x_title(aw_win *w)
{
    if (!w->xwin)
        return;
    char *u = aw_w2u(w->text ? w->text : aw_wempty, -1);
    if (u) {
        plat_window_set_title(w->xwin, u);
        free(u);
    }
}

/* ------------------------------------------------------------------ creation */
static WCHAR *cname_of(const void *p, int unicode, ATOM *atom)
{
    *atom = 0;
    if (!p)
        return NULL;
    if (IS_INTRESOURCE(p)) {
        *atom = (ATOM)(uintptr_t)p;
        return NULL;
    }
    if (unicode)
        return aw_u2w(aw_w2u((const WCHAR *)p, -1), -1);
    return aw_u2w((const char *)p, -1);
}

/* CREATESTRUCT in the charset the target procedure expects. */
typedef struct {
    CREATESTRUCTA a;
    CREATESTRUCTW w;
    char *name_a, *class_a;
} create_blk;

static void build_cs(create_blk *b, aw_win *w, const WCHAR *cls, void *param, int x, int y, int cx, int cy,
                     HWND parent, HMENU menu, HINSTANCE hinst, DWORD style, DWORD exstyle)
{
    memset(b, 0, sizeof *b);
    if (w->unicode) {
        b->w.lpCreateParams = param;
        b->w.hInstance = hinst;
        b->w.hMenu = menu;
        b->w.hwndParent = parent;
        b->w.cy = cy;
        b->w.cx = cx;
        b->w.y = y;
        b->w.x = x;
        b->w.style = (LONG)style;
        b->w.lpszName = w->text;
        b->w.lpszClass = cls;
        b->w.dwExStyle = exstyle;
    } else {
        b->name_a = aw_w2u(w->text ? w->text : aw_wempty, -1);
        b->class_a = aw_w2u(cls ? cls : aw_wempty, -1);
        b->a.lpCreateParams = param;
        b->a.hInstance = hinst;
        b->a.hMenu = menu;
        b->a.hwndParent = parent;
        b->a.cy = cy;
        b->a.cx = cx;
        b->a.y = y;
        b->a.x = x;
        b->a.style = (LONG)style;
        b->a.lpszName = b->name_a;
        b->a.lpszClass = b->class_a;
        b->a.dwExStyle = exstyle;
    }
}

static void free_cs(create_blk *b)
{
    free(b->name_a);
    free(b->class_a);
    b->name_a = b->class_a = NULL;
}

static void *cs_ptr(create_blk *b, int unicode) { return unicode ? (void *)&b->w : (void *)&b->a; }

/* Child rect/size from CW_USEDEFAULT rules; top-level windows cascade. */
static void default_top_rect(int *x, int *y, int *cx, int *cy)
{
    int sw = 1024, sh = 768;
    if (aw_display)
        plat_screen_size(&sw, &sh);
    if (*cx == CW_USEDEFAULT)
        *cx = sw * 3 / 4;
    if (*cy == CW_USEDEFAULT)
        *cy = sh * 3 / 4;
    if (*x == CW_USEDEFAULT || *y == CW_USEDEFAULT) {
        int step = g_cascade++ % 10;
        *x = 50 + step * 26;
        *y = 50 + step * 26;
    }
    if (*cx < 0) *cx = 0;
    if (*cy < 0) *cy = 0;
}

static HWND create_window(DWORD exstyle, const void *clsp, const void *name, DWORD style, int x, int y, int cx,
                          int cy, HWND parent, HMENU menu, HINSTANCE hinst, void *param, int unicode)
{
    aw_user_init();
    ATOM catom;
    WCHAR *cname = cname_of(clsp, unicode, &catom);
    aw_wndclass *c;
    pthread_mutex_lock(&aw_wnd_lock);
    c = aw_class_find(cname, catom);
    pthread_mutex_unlock(&aw_wnd_lock);
    if (!c) {
        free(cname);
        aw_set_last_error(ERR_CLASS_DOES_NOT_EXIST);
        return NULL;
    }
    int is_msg = parent == HWND_MESSAGE;
    int is_child = !is_msg && (style & WS_CHILD) != 0;
    aw_win *pw = NULL;
    if (is_child) {
        pw = aw_win_from_hwnd(parent);
        if (!pw) {
            free(cname);
            aw_set_last_error(ERR_INVALID_WINDOW);
            return NULL;
        }
    }
    if (!is_child && !is_msg && !aw_display) {
        free(cname);
        aw_set_last_error(ERR_GEN_FAILURE);
        return NULL;
    }
    aw_win *owner = NULL;
    if (!is_child && parent && !is_msg)
        owner = aw_win_from_hwnd(parent);

    aw_win *w = aw_win_alloc();
    if (!w) {
        free(cname);
        aw_set_last_error(ERR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    w->cls = c;
    w->proc = c->proc;
    w->unicode = c->unicode;
    w->style = style;
    w->exstyle = exstyle;
    w->hinst = hinst ? hinst : c->hinst;
    w->in_create = 1;
    w->nc_invalid = 1;
    w->enabled = (style & WS_DISABLED) ? 0 : 1;
    w->cursor = c->cursor;
    w->owner = owner ? owner->hwnd : NULL;
    w->is_message = is_msg;
    w->cur_dpi = 96;
    if (is_child)
        w->id = (INT_PTR)menu;
    else
        w->menu = menu;
    if (c->cb_wnd > 0) {
        w->extra_size = c->cb_wnd;
        w->extra = calloc(1, (size_t)c->cb_wnd);
    }
    if (name) {
        if (unicode) {
            aw_win_set_text(w, (const WCHAR *)name, -1);
        } else {
            WCHAR *u = aw_u2w(name, -1);
            aw_win_set_text(w, u, -1);
            free(u);
        }
    }

    /* geometry: a top-level keeps its screen rect in rect; a child keeps it in parent-client coords */
    if (is_child) {
        if (x == CW_USEDEFAULT) x = 0;
        if (y == CW_USEDEFAULT) y = 0;
        if (cx == CW_USEDEFAULT) cx = 0;
        if (cy == CW_USEDEFAULT) cy = 0;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
    } else {
        default_top_rect(&x, &y, &cx, &cy);
    }
    w->rect.left = x;
    w->rect.top = y;
    w->rect.right = x + cx;
    w->rect.bottom = y + cy;
    if (is_child)
        aw_link_child(pw, w, HWND_TOP);
    aw_nc_calc(w);

    create_blk cs;
    build_cs(&cs, w, cname, param, x, y, cx, cy, parent, menu, w->hinst, style, exstyle);
    /* WM_GETMINMAXINFO for top-levels: clamp the requested size */
    if (!is_child && !is_msg) {
        MINMAXINFO mmi;
        memset(&mmi, 0, sizeof mmi);
        int sw = 1024, sh = 768;
        plat_screen_size(&sw, &sh);
        mmi.ptMaxTrackSize.x = sw;
        mmi.ptMaxTrackSize.y = sh;
        aw_call_proc(w, WM_GETMINMAXINFO, 0, (LPARAM)&mmi, w->unicode);
        aw_win *ww = aw_win_from_hwnd(w->hwnd);
        if (ww) {
            if (cx > mmi.ptMaxTrackSize.x && mmi.ptMaxTrackSize.x > 0) cx = mmi.ptMaxTrackSize.x;
            if (cy > mmi.ptMaxTrackSize.y && mmi.ptMaxTrackSize.y > 0) cy = mmi.ptMaxTrackSize.y;
            if (cx < mmi.ptMinTrackSize.x) cx = mmi.ptMinTrackSize.x;
            if (cy < mmi.ptMinTrackSize.y) cy = mmi.ptMinTrackSize.y;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            w->rect.right = w->rect.left + cx;
            w->rect.bottom = w->rect.top + cy;
        }
    }
    aw_nc_calc(w);

    /* WM_NCCREATE -> WM_NCCALCSIZE -> WM_CREATE */
    if (!aw_call_proc(w, WM_NCCREATE, 0, (LPARAM)cs_ptr(&cs, w->unicode), w->unicode)) {
        free_cs(&cs);
        free(cname);
        if (is_child)
            aw_unlink_child(w);
        aw_win_free(w);
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return NULL;
    }
    {
        NCCALCSIZE_PARAMS p;
        memset(&p, 0, sizeof p);
        p.rgrc[0].left = w->rect.left;
        p.rgrc[0].top = w->rect.top;
        p.rgrc[0].right = w->rect.right;
        p.rgrc[0].bottom = w->rect.bottom;
        aw_call_proc(w, WM_NCCALCSIZE, 1, (LPARAM)&p, w->unicode);
        aw_win *ww = aw_win_from_hwnd(w->hwnd);
        if (ww && !is_msg) {
            ww->client.left = p.rgrc[0].left - ww->rect.left;
            ww->client.top = p.rgrc[0].top - ww->rect.top;
            ww->client.right = p.rgrc[0].right - ww->rect.left;
            ww->client.bottom = p.rgrc[0].bottom - ww->rect.top;
        }
    }

    /* top-level: X window + back buffer */
    if (!is_child && !is_msg) {
        uint32_t ownx = owner ? owner->xwin : 0;
        int kind = aw_window_kind(w);
        int cw = w->rect.right - w->rect.left, ch = w->rect.bottom - w->rect.top;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        w->xwin = plat_window_create(w->rect.left, w->rect.top, cw, ch, kind, ownx);
        if (!w->xwin) {
            free_cs(&cs);
            free(cname);
            aw_destroy_window_internal(w);
            aw_set_last_error(ERR_GEN_FAILURE);
            return NULL;
        }
        w->surf = surface_new(cw, ch);
        aw_update_x_title(w);
        aw_invalidate(w, NULL, 1);
    }
    HWND h = w->hwnd;
    if (aw_call_proc(w, WM_CREATE, 0, (LPARAM)cs_ptr(&cs, w->unicode), w->unicode) == -1) {
        free_cs(&cs);
        free(cname);
        aw_destroy_window_internal(w);
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return NULL;
    }
    free_cs(&cs);
    free(cname);
    aw_win *ww = aw_win_from_hwnd(h);
    if (!ww)
        return NULL;
    ww->in_create = 0;
    if (is_child && pw && !(exstyle & WS_EX_NOPARENTNOTIFY)) {
        aw_win *par = aw_win_from_hwnd(ww->parent);
        if (par)
            aw_call_proc(par, WM_PARENTNOTIFY, (WPARAM)(WM_CREATE | ((uintptr_t)ww->id << 16)), (LPARAM)h,
                         par->unicode);
    }
    if (!is_msg) {
        int cw = ww->rect.right - ww->rect.left, ch = ww->rect.bottom - ww->rect.top;
        aw_call_proc(ww, WM_SIZE, SIZE_RESTORED, MAKELONG(cw, ch), ww->unicode);
        aw_call_proc(ww, WM_MOVE, 0, MAKELONG(ww->rect.left, ww->rect.top), ww->unicode);
    }
    if (style & WS_VISIBLE) {
        aw_win *vw = aw_win_from_hwnd(h);
        if (vw)
            aw_show_window(vw, SW_SHOWNORMAL);
    }
    return h;
}

API(user32, HWND, CreateWindowExA,
    (DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int cx, int cy, HWND parent, HMENU menu,
     HINSTANCE hi, LPVOID param))
{
    return create_window(ex, cls, name, style, x, y, cx, cy, parent, menu, hi, param, 0);
}
API(user32, HWND, CreateWindowExW,
    (DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int cx, int cy, HWND parent, HMENU menu,
     HINSTANCE hi, LPVOID param))
{
    return create_window(ex, cls, name, style, x, y, cx, cy, parent, menu, hi, param, 1);
}

/* ------------------------------------------------------------------ destruction */
void aw_timers_kill_for(HWND h);

void aw_destroy_owned(HWND owner)
{
    for (int i = 0; i < AW_MAX_WINDOWS; i++) {
        aw_win *w = &g_windows[i];
        if (w->used && !w->destroyed && !w->parent && w->owner == owner && !w->is_desktop)
            aw_destroy_window_internal(w);
    }
}

void aw_destroy_window_internal(aw_win *w)
{
    if (w->destroyed)
        return;
    w->destroyed = 1;
    HWND h = w->hwnd;
    aw_call_proc(w, WM_DESTROY, 0, 0, w->unicode);
    for (int guard = 0; guard < AW_MAX_WINDOWS; guard++) {
        aw_win *c = w->first_child ? aw_win_from_hwnd(w->first_child) : NULL;
        if (!c) {
            if (w->first_child) {
                /* a stale link; drop it */
                w->first_child = NULL;
                w->last_child = NULL;
            }
            break;
        }
        aw_destroy_window_internal(c);
    }
    if (!w->parent)
        aw_destroy_owned(h);
    aw_timers_kill_for(h);
    aw_queue *q = aw_queue_for_tid(w->tid);
    if (q) {
        if (q->focus == h)
            q->focus = NULL;
        if (q->capture == h)
            aw_capture_set(NULL);
        if (q->active == h)
            q->active = NULL;
        if (q->hover == h)
            q->hover = NULL;
    }
    if (w->parent)
        aw_unlink_child(w);
    aw_call_proc(w, WM_NCDESTROY, 0, 0, w->unicode);
    aw_win_free(w);
}

API(user32, BOOL, DestroyWindow, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w || w->is_desktop) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return FALSE;
    }
    aw_destroy_window_internal(w);
    return TRUE;
}

API(user32, BOOL, CloseWindow, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) return FALSE;
    aw_show_window(w, SW_MINIMIZE);
    return TRUE;
}

API(user32, BOOL, IsWindow, (HWND h)) { return aw_hwnd_valid(h) || h == aw_desktop_hwnd; }
API(user32, BOOL, IsWindowUnicode, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    return w ? w->unicode : 0;
}

API(user32, HWND, GetDesktopWindow, (void)) { return aw_desktop_hwnd; }
API(user32, HWND, GetShellWindow, (void)) { return aw_desktop_hwnd; }

API(user32, HWND, GetParent, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) return NULL;
    if (w->parent) return w->parent;
    return w->owner;
}

API(user32, HWND, GetAncestor, (HWND h, UINT flags))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) return NULL;
    if (flags == 1) /* GA_PARENT */
        return w->parent ? w->parent : aw_desktop_hwnd;
    if (flags == 2) /* GA_ROOT */
        return aw_top_ancestor(w)->hwnd;
    if (flags == 3) { /* GA_ROOTOWNER */
        aw_win *r = aw_top_ancestor(w);
        for (int g = 0; g < AW_MAX_WINDOWS && r->owner; g++) {
            aw_win *o = aw_win_from_hwnd(r->owner);
            if (!o) break;
            r = aw_top_ancestor(o);
        }
        return r->hwnd;
    }
    return NULL;
}

API(user32, HWND, SetParent, (HWND h, HWND np))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return NULL;
    }
    HWND old = w->parent ? w->parent : aw_desktop_hwnd;
    if (!w->parent || !(w->style & WS_CHILD)) {
        /* top-level <-> child conversion is not supported yet */
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return NULL;
    }
    aw_win *p = aw_win_from_hwnd(np);
    if (!p) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return NULL;
    }
    aw_unlink_child(w);
    aw_link_child(p, w, HWND_TOP);
    aw_invalidate(w, NULL, 1);
    aw_invalidate(p, NULL, 1);
    return old;
}

API(user32, HWND, GetWindow, (HWND h, UINT cmd))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) return NULL;
    switch (cmd) {
    case 0: /* GW_HWNDFIRST */
        if (w->parent) {
            aw_win *p = aw_win_from_hwnd(w->parent);
            return p ? p->first_child : NULL;
        }
        return NULL;
    case 1: /* GW_HWNDLAST */
        if (w->parent) {
            aw_win *p = aw_win_from_hwnd(w->parent);
            return p ? p->last_child : NULL;
        }
        return NULL;
    case 2: return w->next_sib;
    case 3: return w->prev_sib;
    case 4: return w->owner;
    case 5: return w->first_child;
    default: return NULL;
    }
}

API(user32, HWND, GetTopWindow, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    return w ? w->first_child : NULL;
}

API(user32, BOOL, IsChild, (HWND p, HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    for (int g = 0; w && w->parent && g < AW_MAX_WINDOWS; g++) {
        if (w->parent == p)
            return TRUE;
        w = aw_win_from_hwnd(w->parent);
    }
    return FALSE;
}

API(user32, int, GetDlgCtrlID, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    return w ? (int)w->id : 0;
}

API(user32, DWORD, GetWindowThreadProcessId, (HWND h, LPDWORD pid))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (pid)
        *pid = (DWORD)getpid();
    return w ? w->tid : 0;
}

/* ------------------------------------------------------------------ text (WM_SETTEXT / WM_GETTEXT paths) */
API(user32, BOOL, SetWindowTextA, (HWND h, LPCSTR s))
{
    if (!aw_hwnd_valid(h)) return FALSE;
    aw_send_msg(h, WM_SETTEXT, 0, (LPARAM)(s ? s : ""), 0);
    return TRUE;
}
API(user32, BOOL, SetWindowTextW, (HWND h, LPCWSTR s))
{
    if (!aw_hwnd_valid(h)) return FALSE;
    static const WCHAR empty[] = {0};
    aw_send_msg(h, WM_SETTEXT, 0, (LPARAM)(s ? s : empty), 1);
    return TRUE;
}
API(user32, int, GetWindowTextA, (HWND h, LPSTR buf, int max))
{
    if (!buf || max <= 0) return 0;
    buf[0] = 0;
    if (!aw_hwnd_valid(h)) return 0;
    return (int)aw_send_msg(h, WM_GETTEXT, (WPARAM)max, (LPARAM)buf, 0);
}
API(user32, int, GetWindowTextW, (HWND h, LPWSTR buf, int max))
{
    if (!buf || max <= 0) return 0;
    buf[0] = 0;
    if (!aw_hwnd_valid(h)) return 0;
    return (int)aw_send_msg(h, WM_GETTEXT, (WPARAM)max, (LPARAM)buf, 1);
}
API(user32, int, GetWindowTextLengthA, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) return 0;
    if (!w->unicode)
        return (int)aw_send_msg(h, WM_GETTEXTLENGTH, 0, 0, 0);
    /* a W window reports UTF-16 units; the A caller wants UTF-8 bytes */
    int units = (int)aw_send_msg(h, WM_GETTEXTLENGTH, 0, 0, 1);
    WCHAR *tmp = calloc((size_t)units + 1, sizeof(WCHAR));
    if (!tmp) return units;
    aw_send_msg(h, WM_GETTEXT, (WPARAM)(units + 1), (LPARAM)tmp, 1);
    char *u = aw_w2u(tmp, -1);
    int bytes = u ? (int)strlen(u) : units;
    free(u);
    free(tmp);
    return bytes;
}
API(user32, int, GetWindowTextLengthW, (HWND h))
{
    if (!aw_hwnd_valid(h)) return 0;
    return (int)aw_send_msg(h, WM_GETTEXTLENGTH, 0, 0, 1);
}

/* ------------------------------------------------------------------ window longs */
static LONG_PTR get_wl(HWND h, int idx, int size)
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return 0;
    }
    switch (idx) {
    case -16: return (LONG_PTR)w->style;        /* GWL_STYLE */
    case -20: return (LONG_PTR)w->exstyle;      /* GWL_EXSTYLE */
    case -4: return (LONG_PTR)w->proc;          /* GWLP_WNDPROC */
    case -6: return (LONG_PTR)w->hinst;         /* GWLP_HINSTANCE */
    case -8: return (LONG_PTR)(w->parent ? w->parent : w->owner); /* GWLP_HWNDPARENT */
    case -12: return (LONG_PTR)w->id;           /* GWLP_ID */
    case -21: return w->userdata;               /* GWLP_USERDATA */
    default: break;
    }
    if (idx >= 0 && idx + size <= w->extra_size) {
        LONG_PTR v = 0;
        memcpy(&v, w->extra + idx, (size_t)size);
        return size == 4 ? (LONG_PTR)(int32_t)v : v;
    }
    aw_set_last_error(ERR_INVALID_PARAMETER);
    return 0;
}

static LONG_PTR set_wl(HWND h, int idx, LONG_PTR val, int size, int unicode)
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return 0;
    }
    LONG_PTR old = get_wl(h, idx, size);
    switch (idx) {
    case -16:
    case -20: {
        STYLESTRUCT ss;
        ss.styleOld = idx == -16 ? w->style : w->exstyle;
        ss.styleNew = (DWORD)val;
        aw_call_proc(w, WM_STYLECHANGING, (WPARAM)(LONG_PTR)idx, (LPARAM)&ss, w->unicode);
        if (idx == -16) w->style = ss.styleNew; else w->exstyle = ss.styleNew;
        aw_call_proc(w, WM_STYLECHANGED, (WPARAM)(LONG_PTR)idx, (LPARAM)&ss, w->unicode);
        return old;
    }
    case -4:
        w->proc = (WNDPROC)val;
        w->unicode = unicode;
        return old;
    case -6: w->hinst = (HINSTANCE)val; return old;
    case -8: {
        aw_win *np = aw_win_from_hwnd((HWND)val);
        if (w->parent) {
            if (np) {
                aw_unlink_child(w);
                aw_link_child(np, w, HWND_TOP);
            }
        } else {
            w->owner = (HWND)val;
        }
        return old;
    }
    case -12: w->id = val; return old;
    case -21: w->userdata = val; return old;
    default: break;
    }
    if (idx >= 0 && idx + size <= w->extra_size) {
        memcpy(w->extra + idx, &val, (size_t)size);
        return old;
    }
    aw_set_last_error(ERR_INVALID_PARAMETER);
    return 0;
}

API(user32, LONG, GetWindowLongA, (HWND h, int idx)) { return (LONG)get_wl(h, idx, 4); }
API(user32, LONG, GetWindowLongW, (HWND h, int idx)) { return (LONG)get_wl(h, idx, 4); }
API(user32, LONG_PTR, GetWindowLongPtrA, (HWND h, int idx)) { return get_wl(h, idx, 8); }
API(user32, LONG_PTR, GetWindowLongPtrW, (HWND h, int idx)) { return get_wl(h, idx, 8); }
API(user32, LONG, SetWindowLongA, (HWND h, int idx, LONG v)) { return (LONG)set_wl(h, idx, v, 4, 0); }
API(user32, LONG, SetWindowLongW, (HWND h, int idx, LONG v)) { return (LONG)set_wl(h, idx, v, 4, 1); }
API(user32, LONG_PTR, SetWindowLongPtrA, (HWND h, int idx, LONG_PTR v)) { return set_wl(h, idx, v, 8, 0); }
API(user32, LONG_PTR, SetWindowLongPtrW, (HWND h, int idx, LONG_PTR v)) { return set_wl(h, idx, v, 8, 1); }

API(user32, WORD, GetWindowWord, (HWND h, int idx)) { return (WORD)get_wl(h, idx, 2); }
API(user32, WORD, SetWindowWord, (HWND h, int idx, WORD v)) { return (WORD)set_wl(h, idx, v, 2, 0); }

/* ------------------------------------------------------------------ properties */
static struct aw_prop *prop_find(aw_win *w, const WCHAR *key)
{
    for (struct aw_prop *p = w->props; p; p = p->next)
        if (!aw_wcmp_ci(p->name, key))
            return p;
    return NULL;
}

static WCHAR *prop_key(const void *name, int unicode)
{
    if (IS_INTRESOURCE(name)) {
        WCHAR *k = malloc(16 * sizeof(WCHAR));
        if (!k) return NULL;
        uint32_t v = (uint32_t)(uintptr_t)name;
        int n = 0;
        WCHAR tmp[12];
        do { tmp[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v && n < 11);
        k[0] = '#';
        for (int i = 0; i < n; i++) k[1 + i] = tmp[n - 1 - i];
        k[1 + n] = 0;
        return k;
    }
    return unicode ? aw_u2w(aw_w2u((const WCHAR *)name, -1), -1) : aw_u2w((const char *)name, -1);
}

static BOOL set_prop(HWND h, const void *name, HANDLE data, int unicode)
{
    aw_win *w = aw_win_from_hwnd(h);
    WCHAR *k = prop_key(name, unicode);
    if (!w || !k) { free(k); return FALSE; }
    struct aw_prop *p = prop_find(w, k);
    if (p) {
        p->data = data;
        free(k);
        return TRUE;
    }
    p = calloc(1, sizeof *p);
    if (!p) { free(k); return FALSE; }
    p->name = k;
    p->data = data;
    p->next = w->props;
    w->props = p;
    return TRUE;
}

static HANDLE get_prop(HWND h, const void *name, int unicode)
{
    aw_win *w = aw_win_from_hwnd(h);
    WCHAR *k = prop_key(name, unicode);
    if (!w || !k) { free(k); return NULL; }
    struct aw_prop *p = prop_find(w, k);
    free(k);
    return p ? p->data : NULL;
}

static HANDLE remove_prop(HWND h, const void *name, int unicode)
{
    aw_win *w = aw_win_from_hwnd(h);
    WCHAR *k = prop_key(name, unicode);
    HANDLE r = NULL;
    if (w && k) {
        struct aw_prop **pp = &w->props;
        while (*pp) {
            if (!aw_wcmp_ci((*pp)->name, k)) {
                struct aw_prop *d = *pp;
                r = d->data;
                *pp = d->next;
                free(d->name);
                free(d);
                break;
            }
            pp = &(*pp)->next;
        }
    }
    free(k);
    return r;
}

API(user32, BOOL, SetPropA, (HWND h, LPCSTR n, HANDLE d)) { return set_prop(h, n, d, 0); }
API(user32, BOOL, SetPropW, (HWND h, LPCWSTR n, HANDLE d)) { return set_prop(h, n, d, 1); }
API(user32, HANDLE, GetPropA, (HWND h, LPCSTR n)) { return get_prop(h, n, 0); }
API(user32, HANDLE, GetPropW, (HWND h, LPCWSTR n)) { return get_prop(h, n, 1); }
API(user32, HANDLE, RemovePropA, (HWND h, LPCSTR n)) { return remove_prop(h, n, 0); }
API(user32, HANDLE, RemovePropW, (HWND h, LPCWSTR n)) { return remove_prop(h, n, 1); }

/* ------------------------------------------------------------------ lookup and enable */
static int top_matches(aw_win *w, const void *cls, const WCHAR *title, int cls_unicode, const void *cls_atom)
{
    (void)cls_atom;
    if (cls) {
        ATOM ca;
        WCHAR *cn = cname_of(cls, cls_unicode, &ca);
        int ok = 0;
        if (cn) {
            ok = w->cls && !aw_wcmp_ci(w->cls->name, cn);
            free(cn);
        } else if (ca) {
            ok = w->cls && w->cls->atom == ca;
        }
        if (!ok) return 0;
    }
    if (title) {
        if (!w->text || aw_wcslen(title) != (size_t)w->text_len || memcmp(w->text, title, (size_t)w->text_len * 2))
            return 0;
    }
    return 1;
}

static HWND find_window(HWND parent, HWND after, const void *cls, const WCHAR *title, int cls_unicode)
{
    aw_user_init();
    aw_win *start = after ? aw_win_from_hwnd(after) : NULL;
    if (parent) {
        aw_win *p = aw_win_from_hwnd(parent);
        if (!p) return NULL;
        HWND cur = start ? start->next_sib : p->first_child;
        while (cur) {
            aw_win *c = aw_win_from_hwnd(cur);
            if (!c) break;
            if (top_matches(c, cls, title, cls_unicode, NULL)) return c->hwnd;
            cur = c->next_sib;
        }
        return NULL;
    }
    int from = 0;
    if (start) {
        for (int i = 0; i < AW_MAX_WINDOWS; i++)
            if (&g_windows[i] == start) { from = i + 1; break; }
    }
    for (int i = from; i < AW_MAX_WINDOWS; i++) {
        aw_win *w = &g_windows[i];
        if (!w->used || w->destroyed || w->parent || w->is_desktop || w->is_message) continue;
        if (top_matches(w, cls, title, cls_unicode, NULL)) return w->hwnd;
    }
    return NULL;
}

API(user32, HWND, FindWindowA, (LPCSTR cls, LPCSTR title))
{
    WCHAR *t = title ? aw_u2w(title, -1) : NULL;
    HWND r = find_window(NULL, NULL, cls, t, 0);
    free(t);
    return r;
}
API(user32, HWND, FindWindowW, (LPCWSTR cls, LPCWSTR title)) { return find_window(NULL, NULL, cls, title, 1); }
API(user32, HWND, FindWindowExA, (HWND p, HWND after, LPCSTR cls, LPCSTR title))
{
    WCHAR *t = title ? aw_u2w(title, -1) : NULL;
    HWND r = find_window(p, after, cls, t, 0);
    free(t);
    return r;
}
API(user32, HWND, FindWindowExW, (HWND p, HWND after, LPCWSTR cls, LPCWSTR title))
{
    return find_window(p, after, cls, title, 1);
}

API(user32, BOOL, IsWindowEnabled, (HWND h))
{
    aw_win *w = aw_win_from_hwnd(h);
    return w ? w->enabled : 0;
}

API(user32, BOOL, EnableWindow, (HWND h, BOOL enable))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return FALSE;
    }
    BOOL was_disabled = !w->enabled;
    if (!!enable == !was_disabled)
        return was_disabled;
    w->enabled = enable ? 1 : 0;
    aw_queue *q = aw_queue_for_tid(w->tid);
    if (!enable && q) {
        if (q->capture == h) aw_capture_set(NULL);
        if (q->focus == h) {
            q->focus = NULL;
            aw_call_proc(w, WM_KILLFOCUS, 0, 0, w->unicode);
        }
    }
    aw_call_proc(w, WM_ENABLE, enable ? 1 : 0, 0, w->unicode);
    return was_disabled;
}

/* ------------------------------------------------------------------ init */
static void atexit_gui_flush(void)
{
    if (aw_display)
        plat_flush();
}

void aw_atexit_gui(void) { atexit_gui_flush(); }

void aw_user_init(void)
{
    if (g_init_state)
        return;
    g_init_state = 1;
    aw_display = plat_init();
    int sw = 1024, sh = 768;
    if (aw_display)
        plat_screen_size(&sw, &sh);
    aw_win *d = aw_win_alloc();
    if (d) {
        d->is_desktop = 1;
        d->visible = 1;
        d->rect.left = 0;
        d->rect.top = 0;
        d->rect.right = sw;
        d->rect.bottom = sh;
        d->client = d->rect;
        aw_desktop_hwnd = d->hwnd;
    }
    aw_controls_register();
    aw_dialogs_register();
    aw_menus_register();
    aw_gui_atexit = aw_atexit_gui;
    g_init_state = 2;
}
