/* user32 core - non-client area: insets, hit testing, frame painting (S5).
 * Top-level windows: the X window manager draws the caption and sizing frame outside the X window,
 * so their NC area is the menu bar only (menu.c paints it through the aw_menubar_* hooks).
 * Child windows and popups: WS_BORDER / WS_EX_CLIENTEDGE are painted inside the window rect. */
#include "user32_int.h"

#define NC_HTNOWHERE 0
#define NC_HTCLIENT 1
#define NC_HTMENU 5
#define NC_HTBORDER 18
#define NC_COLOR_WINDOWFRAME 6
#define NC_COLOR_MENU 4
#define NC_COLOR_BTNSHADOW 16
#define NC_COLOR_3DDKSHADOW 21
#define NC_COLOR_3DLIGHT 22
#define NC_COLOR_3DHILIGHT 20

/* ------------------------------------------------------------------ hooks (menu.c / dialog.c override) */
__attribute__((weak)) int aw_menubar_height(HWND hwnd, HMENU menu, int width)
{
    (void)hwnd; (void)menu; (void)width;
    return 0;
}
__attribute__((weak)) void aw_menubar_paint(HWND hwnd, HMENU menu, HDC window_dc, const RECT *bar)
{
    (void)hwnd; (void)menu; (void)window_dc; (void)bar;
}
__attribute__((weak)) void aw_menubar_track(HWND hwnd, HMENU menu, int x, int y, int vk)
{
    (void)hwnd; (void)menu; (void)x; (void)y; (void)vk;
}
__attribute__((weak)) int aw_menu_is_valid(HMENU menu)
{
    (void)menu;
    return 0;
}
__attribute__((weak)) void aw_dialogs_register(void) {}
__attribute__((weak)) void aw_menus_register(void) {}

/* ------------------------------------------------------------------ insets */
int aw_menu_height(aw_win *w)
{
    if (w->parent || !w->menu || !aw_menu_is_valid(w->menu))
        return 0;
    return aw_menubar_height(w->hwnd, w->menu, w->rect.right - w->rect.left);
}

/* Recomputes the client rect (relative to the window origin) from the current rect and styles. */
void aw_nc_calc(aw_win *w)
{
    int cw = w->rect.right - w->rect.left;
    int ch = w->rect.bottom - w->rect.top;
    int l, t, r, b;
    aw_style_insets(w->style, w->exstyle, aw_menu_height(w), &l, &t, &r, &b);
    aw_scroll_extra(w, &r, &b);
    w->client.left = l;
    w->client.top = t;
    w->client.right = cw - r;
    w->client.bottom = ch - b;
    if (w->client.right < w->client.left) w->client.right = w->client.left;
    if (w->client.bottom < w->client.top) w->client.bottom = w->client.top;
}

/* ------------------------------------------------------------------ hit testing */
/* (x, y) in window coordinates. */
int aw_nc_hit(aw_win *w, int x, int y)
{
    int cw = w->rect.right - w->rect.left;
    int ch = w->rect.bottom - w->rect.top;
    if (x >= w->client.left && x < w->client.right && y >= w->client.top && y < w->client.bottom)
        return NC_HTCLIENT;
    if (x < 0 || y < 0 || x >= cw || y >= ch)
        return NC_HTNOWHERE;
    {
        int sb = aw_scroll_hit(w, x, y);
        if (sb)
            return sb;
    }
    if (!w->parent && w->menu && y < w->client.top)
        return NC_HTMENU;
    return NC_HTBORDER;
}

/* ------------------------------------------------------------------ painting */
static uint32_t sys_px(int idx) { return aw_px(aw_syscolor(idx)); }

/* Fills the part of rect r (surface coords) that lies inside region g. */
static void fill_in_region(aw_surface *s, const aw_region *g, const RECT *r, uint32_t px)
{
    for (int i = 0; i < g->n; i++) {
        RECT c = g->r[i];
        if (c.left < r->left) c.left = r->left;
        if (c.top < r->top) c.top = r->top;
        if (c.right > r->right) c.right = r->right;
        if (c.bottom > r->bottom) c.bottom = r->bottom;
        if (c.left >= c.right || c.top >= c.bottom)
            continue;
        if (c.left < 0) c.left = 0;
        if (c.top < 0) c.top = 0;
        if (c.right > s->w) c.right = s->w;
        if (c.bottom > s->h) c.bottom = s->h;
        for (int y = c.top; y < c.bottom; y++) {
            uint32_t *row = surface_row(s, y);
            for (int x = c.left; x < c.right; x++)
                row[x] = px;
        }
        surface_mark_dirty(s, &c);
    }
}

void aw_nc_paint(aw_win *w)
{
    aw_win *top = aw_top_ancestor(w);
    if (!top || !top->surf || !aw_is_visible(w))
        return;
    aw_region vis;
    rgn_init(&vis);
    RECT wr;
    aw_win_surf_rect(w, &wr);
    rgn_set_rect(&vis, &wr);
    aw_clip_chain(w, &vis);
    RECT cr;
    aw_win_client_surf_rect(w, &cr);
    rgn_subtract_rect(&vis, &cr);
    aw_surface *s = top->surf;

    if (!w->parent && w->menu) {
        RECT bar = {0, 0, w->rect.right - w->rect.left, w->client.top};
        if (bar.bottom > 0) {
            RECT sbar = {wr.left, wr.top, wr.left + bar.right, wr.top + bar.bottom};
            fill_in_region(s, &vis, &sbar, sys_px(NC_COLOR_MENU));
            HDC hdc = aw_dc_for(w, &vis, 1);
            if (hdc) {
                aw_menubar_paint(w->hwnd, w->menu, hdc, &bar);
                gdi_delete_window_dc(hdc);
            }
        }
    }
    aw_scroll_nc_paint(w, &vis);
    /* frame: client edge (sunken, 2px) takes precedence over the plain border */
    if (w->parent && (w->exstyle & WS_EX_CLIENTEDGE)) {
        RECT o = wr;
        RECT in = {wr.left + 1, wr.top + 1, wr.right - 1, wr.bottom - 1};
        uint32_t sh = sys_px(NC_COLOR_BTNSHADOW), dk = sys_px(NC_COLOR_3DDKSHADOW);
        uint32_t lt = sys_px(NC_COLOR_3DHILIGHT), lt2 = sys_px(NC_COLOR_3DLIGHT);
        RECT r;
        r = (RECT){o.left, o.top, o.right, o.top + 1}; fill_in_region(s, &vis, &r, sh);
        r = (RECT){o.left, o.top, o.left + 1, o.bottom}; fill_in_region(s, &vis, &r, sh);
        r = (RECT){in.left, in.top, in.right, in.top + 1}; fill_in_region(s, &vis, &r, dk);
        r = (RECT){in.left, in.top, in.left + 1, in.bottom}; fill_in_region(s, &vis, &r, dk);
        r = (RECT){o.left, o.bottom - 1, o.right, o.bottom}; fill_in_region(s, &vis, &r, lt);
        r = (RECT){o.right - 1, o.top, o.right, o.bottom}; fill_in_region(s, &vis, &r, lt);
        r = (RECT){in.left, in.bottom - 1, in.right, in.bottom}; fill_in_region(s, &vis, &r, lt2);
        r = (RECT){in.right - 1, in.top, in.right, in.bottom}; fill_in_region(s, &vis, &r, lt2);
        return;
    }
    if (w->style & WS_BORDER) {
        uint32_t c = sys_px(NC_COLOR_WINDOWFRAME);
        RECT r;
        r = (RECT){wr.left, wr.top, wr.right, wr.top + 1}; fill_in_region(s, &vis, &r, c);
        r = (RECT){wr.left, wr.bottom - 1, wr.right, wr.bottom}; fill_in_region(s, &vis, &r, c);
        r = (RECT){wr.left, wr.top, wr.left + 1, wr.bottom}; fill_in_region(s, &vis, &r, c);
        r = (RECT){wr.right - 1, wr.top, wr.right, wr.bottom}; fill_in_region(s, &vis, &r, c);
    }
    rgn_free(&vis);
}

