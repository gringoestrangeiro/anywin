/* user32 core - geometry, visibility, z-order, show/hide, coordinate mapping (S5).
 *
 * Coordinates:
 *   - top-level rect: screen coords; its surface covers the whole rect, origin (0,0).
 *   - child rect: client coords of its parent.
 *   - client rect: relative to the window's own top-left.
 *   - "surface" coords: top-level back-buffer pixels; every drawing region is in these. */
#include "user32_int.h"

#define ERR_INVALID_WINDOW 1400

static aw_win *from(HWND h) { return aw_win_from_hwnd(h); }

void aw_win_origin(const aw_win *w, int *x, int *y)
{
    *x = 0;
    *y = 0;
    if (!w->parent)
        return;
    aw_win *p = from(w->parent);
    if (!p)
        return;
    int cx, cy;
    aw_win_client_origin(p, &cx, &cy);
    *x = cx + w->rect.left;
    *y = cy + w->rect.top;
}

void aw_win_client_origin(const aw_win *w, int *x, int *y)
{
    int ox, oy;
    aw_win_origin(w, &ox, &oy);
    *x = ox + w->client.left;
    *y = oy + w->client.top;
}

void aw_win_surf_rect(const aw_win *w, RECT *r)
{
    int ox, oy;
    aw_win_origin(w, &ox, &oy);
    r->left = ox;
    r->top = oy;
    r->right = ox + (w->rect.right - w->rect.left);
    r->bottom = oy + (w->rect.bottom - w->rect.top);
}

void aw_win_client_surf_rect(const aw_win *w, RECT *r)
{
    int ox, oy;
    aw_win_origin(w, &ox, &oy);
    r->left = ox + w->client.left;
    r->top = oy + w->client.top;
    r->right = ox + w->client.right;
    r->bottom = oy + w->client.bottom;
    if (r->right < r->left) r->right = r->left;
    if (r->bottom < r->top) r->bottom = r->top;
}

/* Screen position of the window's top-left corner. */
void aw_screen_pos(const aw_win *w, int *x, int *y)
{
    if (!w->parent) {
        *x = w->rect.left;
        *y = w->rect.top;
        return;
    }
    aw_win *p = from(w->parent);
    if (!p) {
        *x = w->rect.left;
        *y = w->rect.top;
        return;
    }
    int px, py;
    aw_screen_pos(p, &px, &py);
    *x = px + p->client.left + w->rect.left;
    *y = py + p->client.top + w->rect.top;
}

void aw_screen_client_pos(const aw_win *w, int *x, int *y)
{
    aw_screen_pos(w, x, y);
    *x += w->client.left;
    *y += w->client.top;
}

/* Intersect g (surface coords) with the client areas of the ancestors of w and subtract the
 * siblings stacked above w.  Children of w itself are not handled here. */
void aw_clip_chain(const aw_win *w, aw_region *g)
{
    const aw_win *cur = w;
    for (int guard = 0; guard < AW_MAX_WINDOWS && cur->parent; guard++) {
        aw_win *p = from(cur->parent);
        if (!p)
            break;
        RECT pc;
        aw_win_client_surf_rect(p, &pc);
        rgn_intersect_rect(g, &pc);
        /* siblings above cur hide it only when cur has WS_CLIPSIBLINGS (as on Windows: dialog controls
         * do not have it, so a transparent group box stacked above its controls does not hide them) */
        for (HWND s = p->first_child; s && s != cur->hwnd && (cur->style & WS_CLIPSIBLINGS);) {
            aw_win *sw = from(s);
            if (!sw)
                break;
            if (sw->visible) {
                RECT sr;
                aw_win_surf_rect(sw, &sr);
                rgn_subtract_rect(g, &sr);
            }
            s = sw->next_sib;
        }
        cur = p;
    }
}

int aw_is_visible(const aw_win *w)
{
    for (int guard = 0; w && guard < AW_MAX_WINDOWS; guard++) {
        if (!w->visible)
            return 0;
        if (!w->parent)
            return 1;
        w = from(w->parent);
    }
    return 0;
}

/* Visible part of w's client area, surface coords (children are always clipped out). */
void aw_visible_region(const aw_win *w, aw_region *out)
{
    rgn_clear(out);
    if (!aw_is_visible(w))
        return;
    RECT cr;
    aw_win_client_surf_rect(w, &cr);
    rgn_set_rect(out, &cr);
    aw_clip_chain(w, out);
    /* children are excluded only with WS_CLIPCHILDREN: otherwise the parent's background shows under
     * its transparent children (group boxes, static text), and the children paint over it */
    if (!(w->style & WS_CLIPCHILDREN))
        return;
    for (HWND c = w->first_child; c;) {
        aw_win *cw = from(c);
        if (!cw)
            break;
        if (cw->visible) {
            RECT r;
            aw_win_surf_rect(cw, &r);
            rgn_subtract_rect(out, &r);
        }
        c = cw->next_sib;
    }
}

/* ------------------------------------------------------------------ show / hide */
enum {
    SWC_HIDE = 0, SWC_SHOWNORMAL = 1, SWC_SHOWMINIMIZED = 2, SWC_SHOWMAXIMIZED = 3, SWC_SHOWNOACTIVATE = 4,
    SWC_SHOW = 5, SWC_MINIMIZE = 6, SWC_SHOWMINNOACTIVE = 7, SWC_SHOWNA = 8, SWC_RESTORE = 9,
    SWC_SHOWDEFAULT = 10, SWC_FORCEMINIMIZE = 11
};

/* A child's visible area changed (shown, hidden, moved, raised): siblings overlapping `area`
 * (parent-client coords) may now be exposed or covered, so they repaint. */
static void invalidate_siblings(aw_win *w, const RECT *area)
{
    if (!w->parent)
        return;
    aw_win *p = from(w->parent);
    if (!p)
        return;
    for (HWND s = p->first_child; s;) {
        aw_win *sw = from(s);
        if (!sw)
            break;
        if (sw != w && sw->visible) {
            const RECT *r = &sw->rect;
            if (r->left < area->right && area->left < r->right && r->top < area->bottom && area->top < r->bottom)
                aw_invalidate(sw, NULL, 1);
        }
        s = sw->next_sib;
    }
}

BOOL aw_show_window(aw_win *w, int cmd)
{
    if (w->is_desktop)
        return FALSE;
    int was = w->visible;
    int want = 1;
    int activate = 0;
    switch (cmd) {
    case SWC_HIDE:
        want = 0;
        break;
    case SWC_SHOWMINIMIZED:
    case SWC_MINIMIZE:
    case SWC_SHOWMINNOACTIVE:
    case SWC_FORCEMINIMIZE:
        w->minimized = 1;
        w->maximized = 0;
        break;
    case SWC_SHOWMAXIMIZED:
        w->maximized = 1;
        w->minimized = 0;
        activate = 1;
        break;
    case SWC_SHOWNOACTIVATE:
    case SWC_SHOWNA:
        w->minimized = 0;
        break;
    default: /* SW_SHOWNORMAL, SW_SHOW, SW_RESTORE, SW_SHOWDEFAULT */
        w->minimized = 0;
        w->maximized = 0;
        activate = 1;
        break;
    }
    if (!w->parent && w->xwin) {
        if (want) {
            plat_window_show(w->xwin, 1);
            if (w->minimized) plat_window_minimize(w->xwin);
            else if (w->maximized) plat_window_maximize(w->xwin, 1);
        } else {
            plat_window_show(w->xwin, 0);
        }
    }
    w->visible = want ? 1 : 0;
    if (want && !was) {
        int first_show = !w->shown_once;
        w->shown_once = 1;
        w->nc_invalid = 1;
        aw_invalidate(w, NULL, 1);
        if (w->parent) {
            aw_win *p = from(w->parent);
            if (p) {
                /* the child covers part of the parent: the parent may have to repaint under it */
                aw_invalidate(p, &w->rect, 0);
            }
            invalidate_siblings(w, &w->rect);
        }
        if (first_show) {
            /* the size and position reach the window when it first appears (as on Windows);
             * WM_SIZE carries the client area size, never the outer size */
            int cw = w->client.right - w->client.left, ch = w->client.bottom - w->client.top;
            aw_call_proc(w, WM_SIZE, SIZE_RESTORED, MAKELONG(cw, ch), w->unicode);
            aw_call_proc(w, WM_MOVE, 0, MAKELONG(w->rect.left, w->rect.top), w->unicode);
        }
    }
    if (!want && was && w->parent) {
        aw_win *p = from(w->parent);
        if (p)
            aw_invalidate(p, &w->rect, 1);
        invalidate_siblings(w, &w->rect);
    }
    if (!w->parent && want && activate && w->xwin)
        aw_activate_top(w, 1);
    if (was != want) {
        aw_call_proc(w, WM_SHOWWINDOW, (WPARAM)want, 0, w->unicode);
    }
    return was ? TRUE : FALSE;
}

API(user32, BOOL, ShowWindow, (HWND h, int cmd))
{
    aw_win *w = from(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return FALSE;
    }
    return aw_show_window(w, cmd);
}

API(user32, BOOL, ShowWindowAsync, (HWND h, int cmd))
{
    aw_win *w = from(h);
    if (!w) return FALSE;
    return aw_show_window(w, cmd);
}

API(user32, BOOL, IsWindowVisible, (HWND h))
{
    aw_win *w = from(h);
    return w ? aw_is_visible(w) : 0;
}
API(user32, BOOL, IsIconic, (HWND h))
{
    aw_win *w = from(h);
    return w ? w->minimized : 0;
}
API(user32, BOOL, IsZoomed, (HWND h))
{
    aw_win *w = from(h);
    return w ? w->maximized : 0;
}

/* ------------------------------------------------------------------ geometry */
/* Applies a new rect (WINDOWPOS semantics) and the side effects of size/position changes. */
static void apply_geometry(aw_win *w, int x, int y, int cx, int cy, UINT flags, int from_x)
{
    RECT old = w->rect;
    RECT oldclient = w->client;
    int oldw = old.right - old.left, oldh = old.bottom - old.top;
    int nx = (flags & SWP_NOMOVE) ? old.left : x;
    int ny = (flags & SWP_NOMOVE) ? old.top : y;
    int nw = (flags & SWP_NOSIZE) ? oldw : cx;
    int nh = (flags & SWP_NOSIZE) ? oldh : cy;
    if (nw < 0) nw = 0;
    if (nh < 0) nh = 0;
    int moved = nx != old.left || ny != old.top;
    int sized = nw != oldw || nh != oldh;
    w->rect.left = nx;
    w->rect.top = ny;
    w->rect.right = nx + nw;
    w->rect.bottom = ny + nh;
    if (!w->parent && w->xwin) {
        if (sized && w->surf && nw > 0 && nh > 0)
            surface_resize(w->surf, nw, nh);
        if (!from_x && (moved || sized))
            plat_window_move(w->xwin, nx, ny, nw > 0 ? nw : 1, nh > 0 ? nh : 1);
    }
    aw_nc_calc(w);
    if (sized || (flags & SWP_FRAMECHANGED)) {
        NCCALCSIZE_PARAMS p;
        memset(&p, 0, sizeof p);
        p.rgrc[0] = w->rect;
        aw_call_proc(w, WM_NCCALCSIZE, 1, (LPARAM)&p, w->unicode);
        aw_win *ww = from(w->hwnd);
        if (!ww)
            return;
        ww->client.left = p.rgrc[0].left - ww->rect.left;
        ww->client.top = p.rgrc[0].top - ww->rect.top;
        ww->client.right = p.rgrc[0].right - ww->rect.left;
        ww->client.bottom = p.rgrc[0].bottom - ww->rect.top;
    }
    aw_win *cur = from(w->hwnd);
    if (!cur)
        return;
    if (sized || (flags & SWP_FRAMECHANGED))
        cur->nc_invalid = 1;
    if (cur->parent) {
        aw_win *p = from(cur->parent);
        if (p && (moved || sized))
            aw_invalidate(p, &old, 1); /* where the child was, the parent repaints */
        if (moved)
            aw_invalidate(cur, NULL, 1); /* a moved child paints itself at its new place */
        if (moved || sized) {
            invalidate_siblings(cur, &old);
            invalidate_siblings(cur, &cur->rect);
        }
    }
    if (sized) {
        int ncw = cur->client.right - cur->client.left, nch = cur->client.bottom - cur->client.top;
        int ocw = oldclient.right - oldclient.left, och = oldclient.bottom - oldclient.top;
        int redraw_all = (cur->cls && (cur->cls->style & CS_HREDRAW) && ncw != ocw) ||
                         (cur->cls && (cur->cls->style & CS_VREDRAW) && nch != och);
        if (redraw_all) {
            aw_invalidate(cur, NULL, 1);
        } else {
            RECT r;
            if (ncw > ocw) { /* newly exposed right strip */
                r.left = ocw; r.top = 0; r.right = ncw; r.bottom = nch;
                aw_invalidate(cur, &r, 1);
            }
            if (nch > och) { /* newly exposed bottom strip */
                r.left = 0; r.top = och; r.right = ncw; r.bottom = nch;
                aw_invalidate(cur, &r, 1);
            }
        }
    }
}

void aw_set_pos(aw_win *w, int x, int y, int cx, int cy, UINT flags)
{
    apply_geometry(w, x, y, cx, cy, flags, 0);
}

/* X window moved or resized by the window manager (or the server). */
void aw_on_configure(aw_win *w, int x, int y, int cx, int cy)
{
    if (w->destroyed || w->parent)
        return;
    int oldx = w->rect.left, oldy = w->rect.top;
    int oldw = w->rect.right - w->rect.left, oldh = w->rect.bottom - w->rect.top;
    if (x == oldx && y == oldy && cx == oldw && cy == oldh)
        return;
    apply_geometry(w, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE, 1);
    WINDOWPOS wp;
    memset(&wp, 0, sizeof wp);
    wp.hwnd = w->hwnd;
    wp.x = x;
    wp.y = y;
    wp.cx = cx;
    wp.cy = cy;
    wp.flags = SWP_NOZORDER | SWP_NOACTIVATE | ((x == oldx && y == oldy) ? SWP_NOMOVE : 0) |
               ((cx == oldw && cy == oldh) ? SWP_NOSIZE : 0);
    aw_call_proc(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&wp, w->unicode);
}

API(user32, BOOL, SetWindowPos, (HWND h, HWND after, int x, int y, int cx, int cy, UINT flags))
{
    aw_win *w = from(h);
    if (!w || w->is_desktop) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return FALSE;
    }
    WINDOWPOS wp;
    memset(&wp, 0, sizeof wp);
    wp.hwnd = h;
    wp.hwndInsertAfter = after;
    wp.x = x;
    wp.y = y;
    wp.cx = cx;
    wp.cy = cy;
    wp.flags = flags;
    if (flags & SWP_NOMOVE) {
        wp.x = w->rect.left;
        wp.y = w->rect.top;
    }
    if (flags & SWP_NOSIZE) {
        wp.cx = w->rect.right - w->rect.left;
        wp.cy = w->rect.bottom - w->rect.top;
    }
    aw_call_proc(w, WM_WINDOWPOSCHANGING, 0, (LPARAM)&wp, w->unicode);
    w = from(h);
    if (!w)
        return FALSE;
    flags = wp.flags;
    int ocw = w->client.right - w->client.left, och = w->client.bottom - w->client.top;
    apply_geometry(w, wp.x, wp.y, wp.cx, wp.cy, flags, 0);
    w = from(h);
    if (!w)
        return FALSE;
    /* the client area may change size without the window size changing (a scroll bar appears
     * or vanishes): WM_SIZE is sent whenever the client size changes, even with SWP_NOSIZE */
    if (w->client.right - w->client.left != ocw || w->client.bottom - w->client.top != och)
        flags &= ~(UINT)SWP_NOSIZE;
    wp.flags = flags;
    if (!(flags & SWP_NOZORDER) && w->parent) {
        aw_win *p = from(w->parent);
        if (p) {
            aw_unlink_child(w);
            aw_link_child(p, w, wp.hwndInsertAfter);
            aw_invalidate(w, NULL, 1);
            invalidate_siblings(w, &w->rect);
        }
    }
    if ((flags & SWP_SHOWWINDOW) && !w->visible)
        aw_show_window(w, SWC_SHOW);
    else if ((flags & SWP_HIDEWINDOW) && w->visible)
        aw_show_window(w, SWC_HIDE);
    if (!(flags & SWP_NOACTIVATE) && !w->parent && (flags & SWP_SHOWWINDOW) && w->xwin)
        aw_activate_top(w, 1);
    wp.flags = flags;
    aw_call_proc(w, WM_WINDOWPOSCHANGED, 0, (LPARAM)&wp, w->unicode);
    return TRUE;
}

API(user32, BOOL, MoveWindow, (HWND h, int x, int y, int cx, int cy, BOOL repaint))
{
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW);
    return __aw_user32_SetWindowPos(h, NULL, x, y, cx, cy, flags);
}

API(user32, BOOL, BringWindowToTop, (HWND h))
{
    aw_win *w = from(h);
    if (!w) return FALSE;
    if (!w->parent) {
        if (w->xwin) {
            plat_window_activate(w->xwin);
        }
        return TRUE;
    }
    return __aw_user32_SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------ rects and points */
API(user32, BOOL, GetWindowRect, (HWND h, RECT *rc))
{
    aw_win *w = from(h);
    if (!w || !rc) return FALSE;
    int x, y;
    aw_screen_pos(w, &x, &y);
    rc->left = x;
    rc->top = y;
    rc->right = x + (w->rect.right - w->rect.left);
    rc->bottom = y + (w->rect.bottom - w->rect.top);
    return TRUE;
}

API(user32, BOOL, GetClientRect, (HWND h, RECT *rc))
{
    aw_win *w = from(h);
    if (!w || !rc) return FALSE;
    rc->left = 0;
    rc->top = 0;
    rc->right = w->client.right - w->client.left;
    rc->bottom = w->client.bottom - w->client.top;
    if (rc->right < 0) rc->right = 0;
    if (rc->bottom < 0) rc->bottom = 0;
    return TRUE;
}

API(user32, BOOL, ClientToScreen, (HWND h, POINT *pt))
{
    aw_win *w = from(h);
    if (!w || !pt) return FALSE;
    int x, y;
    aw_screen_client_pos(w, &x, &y);
    pt->x += x;
    pt->y += y;
    return TRUE;
}

API(user32, BOOL, ScreenToClient, (HWND h, POINT *pt))
{
    aw_win *w = from(h);
    if (!w || !pt) return FALSE;
    int x, y;
    aw_screen_client_pos(w, &x, &y);
    pt->x -= x;
    pt->y -= y;
    return TRUE;
}

API(user32, int, MapWindowPoints, (HWND hf, HWND ht, POINT *pts, UINT n))
{
    aw_win *f = hf ? from(hf) : NULL;
    aw_win *t = ht ? from(ht) : NULL;
    int dx = 0, dy = 0;
    if (f) { int x, y; aw_screen_client_pos(f, &x, &y); dx += x; dy += y; }
    if (t) { int x, y; aw_screen_client_pos(t, &x, &y); dx -= x; dy -= y; }
    for (UINT i = 0; pts && i < n; i++) {
        pts[i].x += dx;
        pts[i].y += dy;
    }
    return MAKELONG(dx, dy);
}

/* Child hit testing: topmost visible child containing (x,y) in parent-client coords. */
static aw_win *child_at(aw_win *p, int x, int y, int skip_disabled)
{
    for (HWND c = p->first_child; c;) {
        aw_win *cw = from(c);
        if (!cw)
            break;
        if (cw->visible && !(skip_disabled && !cw->enabled)) {
            if (x >= cw->rect.left && x < cw->rect.right && y >= cw->rect.top && y < cw->rect.bottom)
                return cw;
        }
        c = cw->next_sib;
    }
    return NULL;
}

API(user32, HWND, ChildWindowFromPoint, (HWND p, POINT pt))
{
    aw_win *w = from(p);
    if (!w) return NULL;
    aw_win *c = child_at(w, pt.x - w->client.left, pt.y - w->client.top, 0);
    return c ? c->hwnd : p;
}

API(user32, HWND, ChildWindowFromPointEx, (HWND p, POINT pt, UINT flags))
{
    aw_win *w = from(p);
    if (!w) return NULL;
    aw_win *c = child_at(w, pt.x - w->client.left, pt.y - w->client.top, flags & 1);
    return c ? c->hwnd : p;
}

API(user32, HWND, WindowFromPoint, (POINT pt))
{
    for (int i = 0; i < AW_MAX_WINDOWS; i++) {
        aw_win *t = aw_win_slot(i);
        if (!t->used || t->destroyed || t->parent || t->is_desktop || t->is_message || !t->visible)
            continue;
        if (pt.x < t->rect.left || pt.x >= t->rect.right || pt.y < t->rect.top || pt.y >= t->rect.bottom)
            continue;
        /* deepest child under the point, using screen coords for each child */
        aw_win *cur = t;
        for (int guard = 0; guard < 64; guard++) {
            aw_win *hit = NULL;
            for (HWND c = cur->first_child; c;) {
                aw_win *cw = from(c);
                if (!cw) break;
                if (cw->visible) {
                    int sx, sy;
                    aw_screen_pos(cw, &sx, &sy);
                    if (pt.x >= sx && pt.x < sx + (cw->rect.right - cw->rect.left) && pt.y >= sy &&
                        pt.y < sy + (cw->rect.bottom - cw->rect.top)) {
                        hit = cw;
                        break;
                    }
                }
                c = cw->next_sib;
            }
            if (!hit) break;
            cur = hit;
        }
        return cur->hwnd;
    }
    return NULL;
}

/* ------------------------------------------------------------------ AdjustWindowRect */
/* Non-client insets of a window with these styles.  Managed top-levels get only the menu bar (the
 * X window manager draws the frame outside the X window); everything else gets its border. */
void aw_style_insets(DWORD style, DWORD exstyle, int menu_h, int *l, int *t, int *r, int *b)
{
    int managed_top = !(style & WS_CHILD) && (!(style & WS_POPUP) || (style & WS_CAPTION));
    int bw = 0;
    if (!managed_top) {
        if (exstyle & WS_EX_CLIENTEDGE) bw = 2;
        else if (style & WS_BORDER) bw = 1;
    }
    *l = *r = bw;
    *t = bw + ((!(style & WS_CHILD)) ? menu_h : 0);
    *b = bw;
}

static void adjust_rect(RECT *rc, DWORD style, BOOL menu, DWORD ex)
{
    int l, t, r, b;
    aw_style_insets(style, ex, menu ? 20 /* SM_CYMENU */ : 0, &l, &t, &r, &b);
    rc->left -= l;
    rc->top -= t;
    rc->right += r;
    rc->bottom += b;
}

API(user32, BOOL, AdjustWindowRect, (RECT *rc, DWORD style, BOOL menu))
{
    if (!rc) return FALSE;
    adjust_rect(rc, style, menu, 0);
    return TRUE;
}
API(user32, BOOL, AdjustWindowRectEx, (RECT *rc, DWORD style, BOOL menu, DWORD ex))
{
    if (!rc) return FALSE;
    adjust_rect(rc, style, menu, ex);
    return TRUE;
}
API(user32, BOOL, AdjustWindowRectExForDpi, (RECT *rc, DWORD style, BOOL menu, DWORD ex, UINT dpi))
{
    (void)dpi;
    if (!rc) return FALSE;
    adjust_rect(rc, style, menu, ex);
    return TRUE;
}
