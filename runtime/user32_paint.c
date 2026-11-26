/* user32 core - update regions, paint messages, DCs, back-buffer flushing (S5).
 * Update regions are in client coordinates (origin at the client's top-left), like Windows. */
#include "user32_int.h"

#define ERR_INVALID_WINDOW 1400
#define RDW_INVALIDATE 0x0001
#define RDW_INTERNALPAINT 0x0002
#define RDW_ERASE 0x0004
#define RDW_VALIDATE 0x0008
#define RDW_NOERASE 0x0020
#define RDW_ALLCHILDREN 0x0080
#define RDW_UPDATENOW 0x0100
#define RDW_ERASENOW 0x0200
#define DCX_WINDOW 0x0001

static aw_win *from(HWND h) { return aw_win_from_hwnd(h); }

static void client_size(const aw_win *w, int *cw, int *ch)
{
    *cw = w->client.right - w->client.left;
    *ch = w->client.bottom - w->client.top;
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

static RECT client_rect(const aw_win *w)
{
    int cw, ch;
    client_size(w, &cw, &ch);
    RECT r = {0, 0, cw, ch};
    return r;
}

/* ------------------------------------------------------------------ invalidation */
void aw_invalidate(aw_win *w, const RECT *r, int erase)
{
    if (!w || w->destroyed)
        return;
    RECT cr = client_rect(w), rc;
    if (r) {
        rc = *r;
        if (rc.left < cr.left) rc.left = cr.left;
        if (rc.top < cr.top) rc.top = cr.top;
        if (rc.right > cr.right) rc.right = cr.right;
        if (rc.bottom > cr.bottom) rc.bottom = cr.bottom;
    } else {
        rc = cr;
    }
    if (rc.left >= rc.right || rc.top >= rc.bottom)
        return;
    rgn_union_rect(&w->update, &rc);
    if (erase)
        w->erase_pending = 1;
}

void aw_invalidate_rgn(aw_win *w, const aw_region *r, int erase)
{
    if (!w || !r)
        return;
    for (int i = 0; i < r->n; i++)
        aw_invalidate(w, &r->r[i], erase);
}

static void validate(aw_win *w, const RECT *r)
{
    if (!r) {
        rgn_clear(&w->update);
        w->erase_pending = 0;
        return;
    }
    rgn_subtract_rect(&w->update, r);
    if (rgn_is_empty(&w->update))
        w->erase_pending = 0;
}

/* ------------------------------------------------------------------ WM_PAINT generation */
static int paint_pending(aw_win *w)
{
    return !w->destroyed && w->used && !w->is_message && (!rgn_is_empty(&w->update) || w->erase_pending) &&
           aw_is_visible(w);
}

/* Windows with a pending paint whose ancestors have none; parents are painted before children. */
int aw_gen_paint(HWND *out)
{
    DWORD me = aw_current_tid();
    for (int i = 0; i < AW_MAX_WINDOWS; i++) {
        aw_win *w = aw_win_slot(i);
        if (!w->used || w->tid != me || !paint_pending(w))
            continue;
        int blocked = 0;
        for (aw_win *p = w->parent ? from(w->parent) : NULL; p && !blocked; p = p->parent ? from(p->parent) : NULL)
            if (paint_pending(p))
                blocked = 1;
        if (!blocked) {
            *out = w->hwnd;
            return 1;
        }
    }
    return 0;
}

/* Runs WM_PAINT now (UpdateWindow) and drops whatever the handler left invalid. */
void aw_paint_now(aw_win *w)
{
    if (!w || !paint_pending(w))
        return;
    if (w->nc_invalid) {
        w->nc_invalid = 0;
        aw_nc_paint(w);
    }
    aw_call_proc(w, WM_PAINT, 0, 0, w->unicode);
    aw_win *ww = from(w->hwnd);
    if (ww) {
        rgn_clear(&ww->update);
        ww->erase_pending = 0;
    }
}

/* ------------------------------------------------------------------ DCs */
/* A GDI DC on the window's back buffer, clipped to vis (surface coords). */
HDC aw_dc_for(aw_win *w, const aw_region *vis, int window_dc)
{
    aw_win *top = aw_top_ancestor(w);
    if (!top || !top->surf)
        return NULL;
    int ox, oy;
    if (window_dc)
        aw_win_origin(w, &ox, &oy);
    else
        aw_win_client_origin(w, &ox, &oy);
    return gdi_create_window_dc(w->hwnd, top->surf, ox, oy, vis);
}

/* Background erase of clip (surface coords) with a brush: system colours go straight to pixels. */
void aw_erase_bkgnd(aw_win *w, aw_win *top, const aw_region *clip, HBRUSH brush)
{
    if (!top || !top->surf || !clip)
        return;
    if (IS_INTRESOURCE(brush)) {
        uint32_t px = aw_px(aw_syscolor((int)(uintptr_t)brush - 1));
        aw_surface *s = top->surf;
        for (int i = 0; i < clip->n; i++) {
            RECT c = clip->r[i];
            if (c.left < 0) c.left = 0;
            if (c.top < 0) c.top = 0;
            if (c.right > s->w) c.right = s->w;
            if (c.bottom > s->h) c.bottom = s->h;
            if (c.left >= c.right || c.top >= c.bottom)
                continue;
            for (int y = c.top; y < c.bottom; y++) {
                uint32_t *row = surface_row(s, y);
                for (int x = c.left; x < c.right; x++)
                    row[x] = px;
            }
            surface_mark_dirty(s, &c);
        }
        return;
    }
    HDC hdc = aw_dc_for(w, clip, 0);
    if (!hdc)
        return;
    RECT cr = client_rect(w);
    __aw_user32_FillRect(hdc, &cr, brush);
    gdi_delete_window_dc(hdc);
}

API(user32, HDC, BeginPaint, (HWND h, PAINTSTRUCT *ps))
{
    aw_win *w = from(h);
    if (!w || !ps)
        return NULL;
    memset(ps, 0, sizeof *ps);
    aw_win *top = aw_top_ancestor(w);
    if (!top || !top->surf)
        return NULL;
    aw_region vis;
    rgn_init(&vis);
    aw_visible_region(w, &vis);
    int ox, oy;
    aw_win_client_origin(w, &ox, &oy);
    /* update region in surface coords, intersected with what is visible */
    aw_region upd;
    rgn_init(&upd);
    for (int i = 0; i < w->update.n; i++) {
        RECT r = w->update.r[i];
        r.left += ox; r.right += ox; r.top += oy; r.bottom += oy;
        rgn_union_rect(&upd, &r);
    }
    rgn_intersect(&vis, &upd);
    rgn_free(&upd);
    RECT bb = {0, 0, 0, 0};
    rgn_bbox(&w->update, &bb);
    ps->rcPaint = bb;
    ps->fErase = w->erase_pending ? TRUE : FALSE;
    rgn_copy(&w->clip, &vis);
    w->clip_active = 1;
    HDC hdc = gdi_create_window_dc(w->hwnd, top->surf, ox, oy, &vis);
    rgn_free(&vis);
    ps->hdc = hdc;
    validate(w, NULL);
    if (ps->fErase) {
        aw_call_proc(w, WM_ERASEBKGND, (WPARAM)(uintptr_t)hdc, 0, w->unicode);
    }
    return hdc;
}

API(user32, BOOL, EndPaint, (HWND h, const PAINTSTRUCT *ps))
{
    aw_win *w = from(h);
    if (ps && ps->hdc && gdi_is_window_dc(ps->hdc))
        gdi_delete_window_dc(ps->hdc);
    if (w) {
        w->clip_active = 0;
        rgn_clear(&w->clip);
    }
    return TRUE;
}

API(user32, HDC, GetDCEx, (HWND h, HRGN clip, DWORD flags))
{
    (void)clip;
    aw_win *w = from(h);
    if (!w)
        return NULL;
    aw_region vis;
    rgn_init(&vis);
    if (flags & DCX_WINDOW) {
        RECT wr;
        aw_win_surf_rect(w, &wr);
        rgn_set_rect(&vis, &wr);
        aw_clip_chain(w, &vis);
        HDC hdc = aw_dc_for(w, &vis, 1);
        rgn_free(&vis);
        return hdc;
    }
    aw_visible_region(w, &vis);
    HDC hdc = aw_dc_for(w, &vis, 0);
    rgn_free(&vis);
    return hdc;
}

API(user32, HDC, GetDC, (HWND h))
{
    if (!h)
        return NULL;
    return __aw_user32_GetDCEx(h, NULL, 0);
}

API(user32, HDC, GetWindowDC, (HWND h))
{
    if (!h)
        return NULL;
    return __aw_user32_GetDCEx(h, NULL, DCX_WINDOW);
}

API(user32, int, ReleaseDC, (HWND h, HDC hdc))
{
    (void)h;
    if (!hdc || !gdi_is_window_dc(hdc))
        return 0;
    gdi_delete_window_dc(hdc);
    return 1;
}

API(user32, HWND, WindowFromDC, (HDC hdc))
{
    return gdi_is_window_dc(hdc) ? gdi_dc_window(hdc) : NULL;
}

/* ------------------------------------------------------------------ invalidation API */
API(user32, BOOL, InvalidateRect, (HWND h, const RECT *r, BOOL erase))
{
    aw_win *w = from(h);
    if (!w)
        return FALSE;
    aw_invalidate(w, r, erase);
    return TRUE;
}

API(user32, BOOL, InvalidateRgn, (HWND h, HRGN rgn, BOOL erase))
{
    (void)rgn;
    aw_win *w = from(h);
    if (!w)
        return FALSE;
    aw_invalidate(w, NULL, erase); /* region handles are not read; the whole client is invalidated */
    return TRUE;
}

API(user32, BOOL, ValidateRect, (HWND h, const RECT *r))
{
    aw_win *w = from(h);
    if (!w)
        return FALSE;
    if (!r) {
        validate(w, NULL);
        return TRUE;
    }
    validate(w, r);
    return TRUE;
}

API(user32, BOOL, ValidateRgn, (HWND h, HRGN rgn))
{
    (void)rgn;
    aw_win *w = from(h);
    if (!w)
        return FALSE;
    validate(w, NULL);
    return TRUE;
}

API(user32, BOOL, UpdateWindow, (HWND h))
{
    aw_win *w = from(h);
    if (!w)
        return FALSE;
    aw_paint_now(w);
    return TRUE;
}

static void redraw_children(aw_win *w, const RECT *r, int erase)
{
    for (HWND c = w->first_child; c;) {
        aw_win *cw = from(c);
        if (!cw)
            break;
        aw_invalidate(cw, NULL, erase);
        redraw_children(cw, NULL, erase);
        c = cw->next_sib;
    }
    (void)r;
}

API(user32, BOOL, RedrawWindow, (HWND h, const RECT *r, HRGN rgn, UINT flags))
{
    (void)rgn;
    aw_win *w = from(h);
    if (!w)
        return FALSE;
    if (flags & RDW_VALIDATE)
        validate(w, r);
    if (flags & RDW_INVALIDATE)
        aw_invalidate(w, r, (flags & RDW_NOERASE) ? 0 : (flags & RDW_ERASE) ? 1 : 0);
    if (flags & RDW_ALLCHILDREN)
        redraw_children(w, r, (flags & RDW_NOERASE) ? 0 : 1);
    if (flags & (RDW_UPDATENOW | RDW_ERASENOW)) {
        aw_win *ww = from(h);
        if (ww) aw_paint_now(ww);
    }
    return TRUE;
}

API(user32, int, GetUpdateRect, (HWND h, RECT *r, BOOL erase))
{
    (void)erase;
    aw_win *w = from(h);
    if (!w)
        return 0;
    int pending = !rgn_is_empty(&w->update);
    if (r) {
        RECT bb = {0, 0, 0, 0};
        rgn_bbox(&w->update, &bb);
        *r = bb;
    }
    return pending;
}

API(user32, int, GetUpdateRgn, (HWND h, HRGN rgn, BOOL erase))
{
    (void)rgn; (void)erase;
    aw_win *w = from(h);
    return w ? !rgn_is_empty(&w->update) : 0;
}

API(user32, BOOL, LockWindowUpdate, (HWND h))
{
    (void)h;
    return TRUE;
}

/* ------------------------------------------------------------------ back-buffer flush */
/* Sends the dirty part of every visible top-level back buffer to its X window. */
void aw_flush_all(void)
{
    int any = 0;
    for (int i = 0; i < AW_MAX_WINDOWS; i++) {
        aw_win *w = aw_win_slot(i);
        if (!w->used || w->destroyed || w->parent || !w->surf || !w->xwin || !w->visible)
            continue;
        if (!w->surf->has_dirty)
            continue;
        plat_put(w->xwin, w->surf, &w->surf->dirty);
        w->surf->has_dirty = 0;
        memset(&w->surf->dirty, 0, sizeof w->surf->dirty);
        any = 1;
    }
    if (any)
        plat_flush();
}

void aw_atexit_gui_paint(void) { aw_flush_all(); }
