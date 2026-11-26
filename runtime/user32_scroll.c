/* user32 - standard scroll bars (WS_VSCROLL / WS_HSCROLL) and the scroll bar API (S6 part A).
 *
 * The bars live in the non-client area at the right (vertical) and bottom (horizontal) inside the
 * window's border. Their thickness is taken out of the client rect by aw_nc_calc (aw_scroll_extra),
 * hit testing returns HTVSCROLL/HTHSCROLL (aw_scroll_hit), painting goes through a window DC
 * (aw_scroll_nc_paint), and a click is posted as WM_NCLBUTTONDOWN whose DefWindowProc handler runs a
 * nested tracking loop (aw_scroll_track) that owns the mouse until the button is released.
 * Per-window state is allocated on first use and freed with the window (aw_scroll_free). */
#include "user32_int.h"

extern UINT_PTR WINAPI __aw_user32_SetTimer(HWND, UINT_PTR, UINT, void *);
extern BOOL WINAPI __aw_user32_KillTimer(HWND, UINT_PTR);

extern HGDIOBJ WINAPI __aw_gdi32_SelectObject(HDC, HGDIOBJ);
extern BOOL WINAPI __aw_gdi32_DeleteObject(HGDIOBJ);
extern BOOL WINAPI __aw_gdi32_Polygon(HDC, const POINT *, int);

#define SB_BAR_W 17          /* SM_CXVSCROLL / SM_CYHSCROLL */
#define SB_TIMER_ID 0x5B1
#define SB_DELAY_MS 400      /* auto-repeat: first repeat after this, then every SB_REPEAT_MS */
#define SB_REPEAT_MS 50
#define SB_PART_LINE_UP 0
#define SB_PART_LINE_DOWN 1
#define SB_PART_PAGE_UP 2
#define SB_PART_PAGE_DOWN 3
#define SB_PART_THUMB 4
#define SB_BAR_HORZ 0
#define SB_BAR_VERT 1

#define SB_RGB(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define SB_C_TRACK SB_RGB(241, 241, 241)
#define SB_C_GLYPH SB_RGB(96, 96, 96)
#define SB_C_GLYPH_OFF SB_RGB(191, 191, 191)
#define SB_C_THUMB SB_RGB(205, 205, 205)
#define SB_C_THUMB_DRAG SB_RGB(96, 96, 96)
#define SB_C_THUMB_OFF SB_RGB(230, 230, 230)

typedef struct sb_bar {
    int min, max, page, pos, trackpos;
    unsigned visible : 1;    /* valid when vis_set; otherwise the WS_xSCROLL style decides */
    unsigned vis_set : 1;
    unsigned esb : 2;        /* ESB_DISABLE_*: bit 0 up/left arrow, bit 1 down/right arrow */
    unsigned noscroll : 1;   /* SIF_DISABLENOSCROLL and nothing to scroll */
    unsigned dragging : 1;   /* thumb drag in progress */
} sb_bar;

typedef struct sb_state {
    sb_bar b[2];             /* [SB_BAR_HORZ], [SB_BAR_VERT] */
} sb_state;

static const sb_bar sb_default = {0, 100, 0, 0, 0, 0, 0, 0, 0, 0};

static sb_state *sb_state_of(const aw_win *w) { return (sb_state *)w->sb; }

/* the bar for reading: the default range when the window never had scroll state */
static const sb_bar *sb_cur(const aw_win *w, int b)
{
    const sb_state *s = sb_state_of(w);
    return s ? &s->b[b] : &sb_default;
}

static sb_bar *sb_for_write(aw_win *w, int b)
{
    sb_state *s = sb_state_of(w);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s)
            return NULL;
        s->b[0] = sb_default;
        s->b[1] = sb_default;
        w->sb = s;
    }
    return &s->b[b];
}

static int bar_visible(const aw_win *w, int b)
{
    const sb_state *s;
    /* a combo box's own window has no scroll bars: WS_VSCROLL / WS_HSCROLL style its drop-down list */
    if (w->cls && aw_wcmp_ci(w->cls->name, u"ComboBox") == 0)
        return 0;
    s = sb_state_of(w);
    if (s && s->b[b].vis_set)
        return s->b[b].visible;
    return (w->style & (b == SB_BAR_VERT ? WS_VSCROLL : WS_HSCROLL)) != 0;
}

/* screen-independent geometry: the bars sit inside the borders, the vertical one above the
 * horizontal one and vice versa, so they never overlap */
static void sb_bars(aw_win *w, RECT *hb, RECT *vb, int *hv, int *vv)
{
    int cw = w->rect.right - w->rect.left;
    int ch = w->rect.bottom - w->rect.top;
    int l, t, r, b;
    aw_style_insets(w->style, w->exstyle, aw_menu_height(w), &l, &t, &r, &b);
    *hv = bar_visible(w, SB_BAR_HORZ);
    *vv = bar_visible(w, SB_BAR_VERT);
    int inner_r = cw - r, inner_b = ch - b;
    memset(vb, 0, sizeof *vb);
    memset(hb, 0, sizeof *hb);
    if (*vv) {
        vb->left = inner_r - SB_BAR_W;
        vb->top = t;
        vb->right = inner_r;
        vb->bottom = inner_b - (*hv ? SB_BAR_W : 0);
    }
    if (*hv) {
        hb->left = l;
        hb->top = inner_b - SB_BAR_W;
        hb->right = inner_r - (*vv ? SB_BAR_W : 0);
        hb->bottom = inner_b;
    }
}

/* ------------------------------------------------------------------ geometry */
typedef struct sb_geo {
    int len;        /* bar length along the axis */
    int tlen;       /* track length between the arrows */
    int thumb_len;
    int thumb0;     /* thumb start, bar-relative */
    long room;      /* positions that the thumb can travel: min .. min+room */
} sb_geo;

static void sb_geo_calc(const sb_bar *B, const RECT *bar, int vert, sb_geo *g)
{
    g->len = vert ? bar->bottom - bar->top : bar->right - bar->left;
    g->tlen = g->len - 2 * SB_BAR_W;
    if (g->tlen < 0)
        g->tlen = 0;
    long span = (long)B->max - B->min + 1;
    if (span < 1)
        span = 1;
    long page = B->page > 0 ? B->page : 0;
    long room = (long)B->max - B->min - (page > 0 ? page - 1 : 0);
    if (room < 0)
        room = 0;
    g->room = room;
    int thumb = page > 0 ? (int)((long)g->tlen * page / span) : SB_BAR_W;
    if (thumb < SB_BAR_W)
        thumb = SB_BAR_W;
    if (thumb > g->tlen)
        thumb = g->tlen;
    if (room <= 0)
        thumb = g->tlen;
    g->thumb_len = thumb;
    long p = (long)B->pos - B->min;
    if (p < 0) p = 0;
    if (p > room) p = room;
    int avail = g->tlen - thumb;
    g->thumb0 = SB_BAR_W + (room > 0 ? (int)((p * avail + room / 2) / room) : 0);
}

/* scroll position for a thumb whose top is at bar-relative `top` */
static int thumb_pos_for(const sb_bar *B, const sb_geo *g, int top)
{
    int avail = g->tlen - g->thumb_len;
    int rel = top - SB_BAR_W;
    if (rel < 0) rel = 0;
    if (rel > avail) rel = avail;
    if (avail <= 0 || g->room <= 0)
        return B->min;
    return B->min + (int)(((long)rel * g->room + avail / 2) / avail);
}

/* ------------------------------------------------------------------ painting */
static void sb_poly(HDC dc, const POINT *pts, int n, COLORREF c)
{
    HBRUSH br = __aw_gdi32_CreateSolidBrush(c);
    if (!br)
        return;
    HGDIOBJ old_br = __aw_gdi32_SelectObject(dc, br);
    HGDIOBJ old_pen = __aw_gdi32_SelectObject(dc, __aw_gdi32_GetStockObject(8 /* NULL_PEN */));
    __aw_gdi32_Polygon(dc, pts, n);
    __aw_gdi32_SelectObject(dc, old_pen);
    __aw_gdi32_SelectObject(dc, old_br);
    __aw_gdi32_DeleteObject(br);
}

static void sb_fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = {l, t, r, b};
    HBRUSH br = __aw_gdi32_CreateSolidBrush(c);
    if (!br)
        return;
    __aw_user32_FillRect(dc, &rc, br);
    __aw_gdi32_DeleteObject(br);
}

/* arrow glyph inside the square [l,t)-(l+SB_BAR_W, t+SB_BAR_W); dir = SB_PART_LINE_UP/DOWN, vertical flag */
static void sb_arrow(HDC dc, int l, int t, int vert, int down, COLORREF c)
{
    int cx = l + SB_BAR_W / 2, cy = t + SB_BAR_W / 2;
    POINT p[3];
    if (vert) {
        if (down) {
            p[0] = (POINT){cx - 4, cy - 2}; p[1] = (POINT){cx + 4, cy - 2}; p[2] = (POINT){cx, cy + 2};
        } else {
            p[0] = (POINT){cx - 4, cy + 2}; p[1] = (POINT){cx + 4, cy + 2}; p[2] = (POINT){cx, cy - 2};
        }
    } else {
        if (down) {
            p[0] = (POINT){cx - 2, cy - 4}; p[1] = (POINT){cx - 2, cy + 4}; p[2] = (POINT){cx + 2, cy};
        } else {
            p[0] = (POINT){cx + 2, cy - 4}; p[1] = (POINT){cx + 2, cy + 4}; p[2] = (POINT){cx - 2, cy};
        }
    }
    sb_poly(dc, p, 3, c);
}

static void sb_paint_bar(HDC dc, const aw_win *w, int b, const RECT *r)
{
    const sb_bar *B = sb_cur(w, b);
    int vert = b == SB_BAR_VERT;
    sb_geo g;
    sb_geo_calc(B, r, vert, &g);
    sb_fill(dc, r->left, r->top, r->right, r->bottom, SB_C_TRACK);
    int disabled_all = (B->esb == 3);
    int up_off = (B->esb & 1) || disabled_all;
    int dn_off = (B->esb & 2) || disabled_all;
    if (vert) {
        sb_arrow(dc, r->left, r->top, 1, 0, up_off ? SB_C_GLYPH_OFF : SB_C_GLYPH);
        sb_arrow(dc, r->left, r->bottom - SB_BAR_W, 1, 1, dn_off ? SB_C_GLYPH_OFF : SB_C_GLYPH);
    } else {
        sb_arrow(dc, r->left, r->top, 0, 0, up_off ? SB_C_GLYPH_OFF : SB_C_GLYPH);
        sb_arrow(dc, r->right - SB_BAR_W, r->top, 0, 1, dn_off ? SB_C_GLYPH_OFF : SB_C_GLYPH);
    }
    if (g.tlen <= 0)
        return;
    COLORREF tc = disabled_all || B->noscroll || g.room <= 0 ? SB_C_THUMB_OFF
                  : B->dragging                              ? SB_C_THUMB_DRAG
                                                             : SB_C_THUMB;
    if (vert)
        sb_fill(dc, r->left + 2, r->top + g.thumb0, r->right - 2, r->top + g.thumb0 + g.thumb_len, tc);
    else
        sb_fill(dc, r->left + g.thumb0, r->top + 2, r->left + g.thumb0 + g.thumb_len, r->bottom - 2, tc);
}

void aw_scroll_nc_paint(aw_win *w, const aw_region *vis)
{
    RECT hb, vb;
    int hv, vv;
    sb_bars(w, &hb, &vb, &hv, &vv);
    if (!hv && !vv)
        return;
    HDC dc = aw_dc_for(w, vis, 1);
    if (!dc)
        return;
    if (hv && vv)
        sb_fill(dc, vb.left, hb.top, vb.right, hb.bottom, SB_C_TRACK); /* corner box */
    if (vv)
        sb_paint_bar(dc, w, SB_BAR_VERT, &vb);
    if (hv)
        sb_paint_bar(dc, w, SB_BAR_HORZ, &hb);
    gdi_delete_window_dc(dc);
}

/* ------------------------------------------------------------------ NC integration */
void aw_scroll_extra(aw_win *w, int *r, int *b)
{
    if (bar_visible(w, SB_BAR_VERT))
        *r += SB_BAR_W;
    if (bar_visible(w, SB_BAR_HORZ))
        *b += SB_BAR_W;
}

int aw_scroll_hit(aw_win *w, int x, int y)
{
    RECT hb, vb;
    int hv, vv;
    sb_bars(w, &hb, &vb, &hv, &vv);
    if (vv && x >= vb.left && x < vb.right && y >= vb.top && y < vb.bottom)
        return HTVSCROLL;
    if (hv && x >= hb.left && x < hb.right && y >= hb.top && y < hb.bottom)
        return HTHSCROLL;
    return 0;
}

void aw_scroll_free(aw_win *w)
{
    free(w->sb);
    w->sb = NULL;
}

static void sb_redraw(aw_win *w)
{
    aw_nc_paint(w);
}

/* the window's frame changed (a bar appeared or vanished): NC calc, WM_NCCALCSIZE, WM_SIZE */
static void sb_frame_changed(aw_win *w)
{
    __aw_user32_SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    sb_redraw(w);
}

/* ------------------------------------------------------------------ tracking */
static void send_scroll(aw_win *w, int vert, int code, int pos)
{
    WPARAM wp = (WPARAM)(((UINT)(pos & 0xFFFF) << 16) | (UINT)(code & 0xFFFF));
    aw_send_msg(w->hwnd, vert ? WM_VSCROLL : WM_HSCROLL, wp, 0, w->unicode);
}

static int part_code(int part)
{
    switch (part) {
    case SB_PART_LINE_UP: return SB_LINEUP;
    case SB_PART_LINE_DOWN: return SB_LINEDOWN;
    case SB_PART_PAGE_UP: return SB_PAGEUP;
    case SB_PART_PAGE_DOWN: return SB_PAGEDOWN;
    default: return SB_ENDSCROLL;
    }
}

/* Called by DefWindowProc for WM_NCLBUTTONDOWN with HTVSCROLL/HTHSCROLL. (sx, sy) is the screen
 * position of the press. Runs the tracking loop until the button comes up. */
void aw_scroll_track(aw_win *w, int hit, int sx, int sy)
{
    int b = hit == HTHSCROLL ? SB_BAR_HORZ : SB_BAR_VERT;
    int vert = b == SB_BAR_VERT;
    RECT hb, vb;
    int hv, vv;
    sb_bars(w, &hb, &vb, &hv, &vv);
    if (!(vert ? vv : hv))
        return;
    RECT *br = vert ? &vb : &hb;
    sb_bar *B = sb_for_write(w, b);
    if (!B || B->esb == 3)
        return;
    int ox, oy;
    aw_screen_pos(w, &ox, &oy);
    int along = (vert ? sy - oy : sx - ox) - (vert ? br->top : br->left);

    sb_geo g;
    sb_geo_calc(B, br, vert, &g);
    int part;
    if (along < SB_BAR_W)
        part = SB_PART_LINE_UP;
    else if (along >= g.len - SB_BAR_W)
        part = SB_PART_LINE_DOWN;
    else if (along >= g.thumb0 && along < g.thumb0 + g.thumb_len)
        part = SB_PART_THUMB;
    else
        part = along < g.thumb0 ? SB_PART_PAGE_UP : SB_PART_PAGE_DOWN;
    if ((part == SB_PART_LINE_UP && (B->esb & 1)) || (part == SB_PART_LINE_DOWN && (B->esb & 2)))
        return;
    if (part == SB_PART_THUMB && g.room <= 0)
        return;

    int grab = along - g.thumb0; /* where in the thumb the press happened */
    int auto_repeat = part != SB_PART_THUMB;
    aw_queue *q = aw_queue_current();
    aw_capture_set(w->hwnd);
    if (part == SB_PART_THUMB) {
        B->dragging = 1;
        B->trackpos = B->pos;
    } else {
        send_scroll(w, vert, part_code(part), B->pos);
        __aw_user32_SetTimer(w->hwnd, SB_TIMER_ID, SB_DELAY_MS, NULL);
    }
    for (;;) {
        MSG m;
        aw_get_message(q, &m, NULL, 0, 0, 1, 1);
        if (m.message == WM_QUIT) {
            aw_post_thread_msg_q(q, WM_QUIT, m.wParam, 0);
            break;
        }
        if (m.message == WM_LBUTTONUP || m.message == WM_NCLBUTTONUP)
            break;
        if (m.message == WM_MOUSEMOVE && B->dragging && m.hwnd == w->hwnd) {
            int a = (vert ? m.pt.y - oy : m.pt.x - ox) - (vert ? br->top : br->left);
            int np = thumb_pos_for(B, &g, a - grab);
            if (np != B->trackpos) {
                B->trackpos = np;
                sb_redraw(w);
                send_scroll(w, vert, SB_THUMBTRACK, np);
            }
            continue;
        }
        if (m.message == WM_TIMER && m.wParam == SB_TIMER_ID && m.hwnd == w->hwnd && auto_repeat) {
            /* page clicks stop once the thumb has reached the pointer */
            sb_geo cur;
            sb_geo_calc(B, br, vert, &cur);
            int a = (vert ? m.pt.y - oy : m.pt.x - ox) - (vert ? br->top : br->left);
            if (part == SB_PART_PAGE_UP && a >= cur.thumb0)
                continue;
            if (part == SB_PART_PAGE_DOWN && a < cur.thumb0 + cur.thumb_len)
                continue;
            send_scroll(w, vert, part_code(part), B->pos);
            __aw_user32_SetTimer(w->hwnd, SB_TIMER_ID, SB_REPEAT_MS, NULL);
            continue;
        }
        __aw_user32_TranslateMessage(&m);
        __aw_user32_DispatchMessageA(&m);
    }
    __aw_user32_KillTimer(w->hwnd, SB_TIMER_ID);
    aw_capture_set(NULL);
    if (B->dragging) {
        B->dragging = 0;
        B->pos = B->trackpos;
        send_scroll(w, vert, SB_THUMBPOSITION, B->trackpos);
    }
    send_scroll(w, vert, SB_ENDSCROLL, 0);
    sb_redraw(w);
}

/* ------------------------------------------------------------------ public API */
static int bar_index(int bar)
{
    if (bar == SB_HORZ) return SB_BAR_HORZ;
    if (bar == SB_VERT) return SB_BAR_VERT;
    return -1;
}

API(user32, int, SetScrollInfo, (HWND h, int bar, const SCROLLINFO *si, BOOL redraw))
{
    aw_win *w = aw_win_from_hwnd(h);
    int b = bar_index(bar);
    if (!w || b < 0 || !si)
        return 0;
    sb_bar *B = sb_for_write(w, b);
    if (!B)
        return 0;
    if (si->fMask & SIF_RANGE) {
        B->min = si->nMin;
        B->max = si->nMax;
    }
    if (si->fMask & SIF_PAGE)
        B->page = (int)si->nPage;
    if (si->fMask & SIF_POS)
        B->pos = si->nPos;
    if (si->fMask & SIF_DISABLENOSCROLL) {
        long room = (long)B->max - B->min - (B->page > 0 ? B->page - 1 : 0);
        B->noscroll = room <= 0;
    }
    if (redraw)
        sb_redraw(w);
    return B->pos;
}

API(user32, BOOL, GetScrollInfo, (HWND h, int bar, SCROLLINFO *si))
{
    aw_win *w = aw_win_from_hwnd(h);
    int b = bar_index(bar);
    if (!w || b < 0 || !si)
        return FALSE;
    const sb_bar *B = sb_cur(w, b);
    if (si->fMask & SIF_RANGE) {
        si->nMin = B->min;
        si->nMax = B->max;
    }
    if (si->fMask & SIF_PAGE)
        si->nPage = (UINT)(B->page > 0 ? B->page : 0);
    if (si->fMask & SIF_POS)
        si->nPos = B->pos;
    if (si->fMask & SIF_TRACKPOS)
        si->nTrackPos = B->dragging ? B->trackpos : B->pos;
    return TRUE;
}

API(user32, int, SetScrollPos, (HWND h, int bar, int pos, BOOL redraw))
{
    aw_win *w = aw_win_from_hwnd(h);
    int b = bar_index(bar);
    if (!w || b < 0)
        return 0;
    sb_bar *B = sb_for_write(w, b);
    if (!B)
        return 0;
    int prev = B->pos;
    B->pos = pos;
    if (redraw)
        sb_redraw(w);
    return prev;
}

API(user32, int, GetScrollPos, (HWND h, int bar))
{
    aw_win *w = aw_win_from_hwnd(h);
    int b = bar_index(bar);
    return (w && b >= 0) ? sb_cur(w, b)->pos : 0;
}

API(user32, BOOL, SetScrollRange, (HWND h, int bar, int min, int max, BOOL redraw))
{
    aw_win *w = aw_win_from_hwnd(h);
    int b = bar_index(bar);
    if (!w || b < 0)
        return FALSE;
    sb_bar *B = sb_for_write(w, b);
    if (!B)
        return FALSE;
    B->min = min;
    B->max = max;
    if (redraw)
        sb_redraw(w);
    return TRUE;
}

API(user32, BOOL, GetScrollRange, (HWND h, int bar, int *minp, int *maxp))
{
    aw_win *w = aw_win_from_hwnd(h);
    int b = bar_index(bar);
    if (!w || b < 0)
        return FALSE;
    const sb_bar *B = sb_cur(w, b);
    if (minp)
        *minp = B->min;
    if (maxp)
        *maxp = B->max;
    return TRUE;
}

API(user32, BOOL, ShowScrollBar, (HWND h, int bar, BOOL show))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w || (bar != SB_HORZ && bar != SB_VERT && bar != SB_BOTH))
        return FALSE;
    for (int b = 0; b < 2; b++) {
        if (bar != SB_BOTH && bar != (b == SB_BAR_VERT ? SB_VERT : SB_HORZ))
            continue;
        sb_bar *B = sb_for_write(w, b);
        if (!B)
            return FALSE;
        B->vis_set = 1;
        B->visible = show ? 1 : 0;
    }
    sb_frame_changed(w);
    return TRUE;
}

API(user32, BOOL, EnableScrollBar, (HWND h, int bar, UINT flags))
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w || (bar != SB_HORZ && bar != SB_VERT && bar != SB_BOTH))
        return FALSE;
    for (int b = 0; b < 2; b++) {
        if (bar != SB_BOTH && bar != (b == SB_BAR_VERT ? SB_VERT : SB_HORZ))
            continue;
        sb_bar *B = sb_for_write(w, b);
        if (!B)
            return FALSE;
        B->esb = flags & 3;
    }
    sb_redraw(w);
    return TRUE;
}
