/* anywin GDI drawing primitives (S4): lines, rectangles, ellipses, polygons, rect fills, ROP-based
 * blits, icon blending, and the user32 rect helpers (FillRect, FrameRect, InvertRect,
 * DrawFocusRect, DrawEdge) that gui.h assigns to the drawing layer.
 *
 * All coordinates are logical on the API side and are mapped to device pixels (surface
 * coordinates) first.  Every write goes through the DC clip region; drawing outside it is never
 * performed.  Dirty rectangles are marked on the surface once per call. */
#include "gdi_int.h"

typedef struct {
    int style, width;
    uint32_t px;
} pen_t;

static RECT rect_and(RECT a, RECT b)
{
    RECT r;
    r.left = a.left > b.left ? a.left : b.left;
    r.top = a.top > b.top ? a.top : b.top;
    r.right = a.right < b.right ? a.right : b.right;
    r.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    if (r.left >= r.right || r.top >= r.bottom)
        r.left = r.top = r.right = r.bottom = 0;
    return r;
}

static RECT rect_make(int l, int t, int r, int b)
{
    RECT q = {l, t, r, b};
    return q;
}

static int rect_nonempty(RECT r) { return r.left < r.right && r.top < r.bottom; }

static void rect_norm(int *l, int *t, int *r, int *b)
{
    if (*l > *r) {
        int x = *l;
        *l = *r;
        *r = x;
    }
    if (*t > *b) {
        int x = *t;
        *t = *b;
        *b = x;
    }
}

/* one device pixel through an explicit ROP2, clip checked */
static void put_r(gdi_dc *d, int x, int y, uint32_t px, int rop)
{
    if ((unsigned)x >= (unsigned)d->surf->w || (unsigned)y >= (unsigned)d->surf->h)
        return;
    if (!gdi_clip_ok(d, x, y))
        return;
    uint32_t *p = surface_row(d->surf, y) + x;
    *p = gdi_rop2(rop, *p, px);
}

void gdi_put(gdi_dc *d, int x, int y, uint32_t px) { put_r(d, x, y, px, d->rop2); }

void gdi_alpha_put(gdi_dc *d, int x, int y, uint32_t px, uint32_t alpha)
{
    if (!alpha || (unsigned)x >= (unsigned)d->surf->w || (unsigned)y >= (unsigned)d->surf->h)
        return;
    if (!gdi_clip_ok(d, x, y))
        return;
    uint32_t *p = surface_row(d->surf, y) + x;
    *p = alpha >= 255 ? px : gdi_blend(*p, px, alpha);
}

/* fill a device rectangle (exclusive right/bottom) through the clip region */
static void fill_dev(gdi_dc *d, RECT r, uint32_t px, int rop)
{
    RECT srf = rect_make(0, 0, d->surf->w, d->surf->h);
    RECT c = rect_and(rect_and(r, d->cb), srf);
    if (!rect_nonempty(c))
        return;
    for (int k = 0; k < d->clip.n; k++) {
        RECT q = rect_and(c, d->clip.r[k]);
        for (int y = q.top; y < q.bottom; y++) {
            uint32_t *row = surface_row(d->surf, y);
            for (int x = q.left; x < q.right; x++)
                row[x] = gdi_rop2(rop, row[x], px);
        }
    }
    gdi_mark(d, &c);
}

void gdi_fill_dev(gdi_dc *d, RECT r, uint32_t px, int rop) { fill_dev(d, r, px, rop); }

static int get_pen(gdi_dc *d, pen_t *p)
{
    gdi_obj *o = gdi_get(d->pen, GDI_OBJ_PEN);
    if (!o)
        return 0;
    p->style = o->u.pen.style;
    p->width = o->u.pen.width > 0 ? o->u.pen.width : 1;
    p->px = gdi_px(gdi_pen_color(d, o));
    return 1;
}

/* 1 when the current brush is solid (px set), 0 for a null brush or an invalid one */
static int get_brush(gdi_dc *d, uint32_t *px)
{
    COLORREF c;
    if (gdi_brush_color(d, d->brush, &c) != 1)
        return 0;
    *px = gdi_px(c);
    return 1;
}

/* square pen stamp of width w centred on (x, y), like a Windows geometric-width cosmetic pen */
static void stamp(gdi_dc *d, int x, int y, int w, uint32_t px)
{
    int lo = -(w - 1) / 2, hi = w / 2;
    for (int dy = lo; dy <= hi; dy++)
        for (int dx = lo; dx <= hi; dx++)
            put_r(d, x + dx, y + dy, px, d->rop2);
}

static int pat_on(int style, int step)
{
    static const unsigned char dash[] = {4, 2}, dot[] = {1, 2}, dashdot[] = {4, 2, 1, 2},
                               dashdotdot[] = {4, 2, 1, 2, 1, 2};
    const unsigned char *pat;
    int n;
    switch (style) {
    case PS_DASH:
        pat = dash, n = 2;
        break;
    case PS_DOT:
        pat = dot, n = 2;
        break;
    case PS_DASHDOT:
        pat = dashdot, n = 4;
        break;
    case PS_DASHDOTDOT:
        pat = dashdotdot, n = 6;
        break;
    default:
        return 1;
    }
    int total = 0;
    for (int i = 0; i < n; i++)
        total += pat[i];
    int pos = step % total;
    for (int i = 0;; i++) {
        if (pos < pat[i % n])
            return i % 2 == 0;
        pos -= pat[i % n];
    }
}

/* Bresenham from (x0,y0) to (x1,y1) in device pixels; the end pixel only when with_end */
static void line_dev(gdi_dc *d, int x0, int y0, int x1, int y1, const pen_t *p, int with_end)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy, step = 0;
    for (;;) {
        if (x0 == x1 && y0 == y1) {
            if (with_end && pat_on(p->style, step))
                stamp(d, x0, y0, p->width, p->px);
            break;
        }
        if (pat_on(p->style, step))
            stamp(d, x0, y0, p->width, p->px);
        step++;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void mark_line(gdi_dc *d, int x0, int y0, int x1, int y1, int w)
{
    RECT r = rect_make((x0 < x1 ? x0 : x1) - w, (y0 < y1 ? y0 : y1) - w, (x0 > x1 ? x0 : x1) + w + 1,
                       (y0 > y1 ? y0 : y1) + w + 1);
    gdi_mark(d, &r);
}

static void line_seg(gdi_dc *d, int x0, int y0, int x1, int y1, const pen_t *p, int with_end)
{
    int ax, ay, bx, by;
    gdi_map(d, x0, y0, &ax, &ay);
    gdi_map(d, x1, y1, &bx, &by);
    line_dev(d, ax, ay, bx, by, p, with_end);
    mark_line(d, ax, ay, bx, by, p->width);
}

/* ---------------------------------------------------------------------- */
/* lines and polylines                                                     */
/* ---------------------------------------------------------------------- */

API(gdi32, BOOL, LineTo, (HDC hdc, int x, int y))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pen_t p;
    if (get_pen(d, &p) && p.style != PS_NULL)
        line_seg(d, d->cur_x, d->cur_y, x, y, &p, 0);
    d->cur_x = x;
    d->cur_y = y;
    return TRUE;
}

API(gdi32, BOOL, Polyline, (HDC hdc, const POINT *pts, int n))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || !pts || n < 2) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pen_t p;
    if (!get_pen(d, &p) || p.style == PS_NULL)
        return TRUE;
    for (int i = 0; i + 1 < n; i++)
        line_seg(d, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, &p, i + 2 == n);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* rectangles and ellipses                                                 */
/* ---------------------------------------------------------------------- */

API(gdi32, BOOL, Rectangle, (HDC hdc, int l, int t, int r, int b))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    rect_norm(&l, &t, &r, &b);
    RECT dr;
    gdi_map_rect(d, &(RECT){l, t, r, b}, &dr);
    if (!rect_nonempty(dr))
        return TRUE;
    uint32_t bpx;
    if (get_brush(d, &bpx))
        fill_dev(d, dr, bpx, d->rop2);
    pen_t p;
    if (!get_pen(d, &p) || p.style == PS_NULL)
        return TRUE;
    if (p.style == PS_SOLID || p.style == PS_INSIDEFRAME) {
        int w = p.width;
        int hw = (dr.right - dr.left) / 2 < w ? (dr.right - dr.left) / 2 : w;
        int hh = (dr.bottom - dr.top) / 2 < w ? (dr.bottom - dr.top) / 2 : w;
        fill_dev(d, rect_make(dr.left, dr.top, dr.right, dr.top + hh), p.px, d->rop2);
        fill_dev(d, rect_make(dr.left, dr.bottom - hh, dr.right, dr.bottom), p.px, d->rop2);
        fill_dev(d, rect_make(dr.left, dr.top + hh, dr.left + hw, dr.bottom - hh), p.px, d->rop2);
        fill_dev(d, rect_make(dr.right - hw, dr.top + hh, dr.right, dr.bottom - hh), p.px, d->rop2);
    } else {
        int x0 = dr.left, y0 = dr.top, x1 = dr.right - 1, y1 = dr.bottom - 1;
        line_dev(d, x0, y0, x1, y0, &p, 1);
        line_dev(d, x1, y0, x1, y1, &p, 1);
        line_dev(d, x1, y1, x0, y1, &p, 1);
        line_dev(d, x0, y1, x0, y0, &p, 1);
        gdi_mark(d, &dr);
    }
    return TRUE;
}

/* inside test for the ellipse with device box (l,t,r,b): centre (cx2/2, cy2/2), axes A x B.
 * Pixel centres are used, so the test is exact in integer arithmetic. */
static int ell_inside(long long X, long long Y, long long A, long long B)
{
    return X * X * B * B + Y * Y * A * A <= A * A * B * B;
}

API(gdi32, BOOL, Ellipse, (HDC hdc, int l, int t, int r, int b))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    rect_norm(&l, &t, &r, &b);
    RECT e;
    gdi_map_rect(d, &(RECT){l, t, r, b}, &e);
    long long A = e.right - e.left, B = e.bottom - e.top;
    if (A <= 0 || B <= 0)
        return TRUE;
    uint32_t bpx = 0, ppx = 0;
    int have_brush = get_brush(d, &bpx);
    pen_t p;
    int have_pen = get_pen(d, &p) && p.style != PS_NULL;
    if (have_pen)
        ppx = p.px;
    long long cx2 = (long long)e.left + e.right, cy2 = (long long)e.top + e.bottom;
    RECT box = rect_and(rect_and(e, d->cb), rect_make(0, 0, d->surf->w, d->surf->h));
    for (int y = box.top; y < box.bottom; y++) {
        for (int x = box.left; x < box.right; x++) {
            long long X = 2LL * x + 1 - cx2, Y = 2LL * y + 1 - cy2;
            if (!ell_inside(X, Y, A, B))
                continue;
            int edge = !ell_inside(X - 2, Y, A, B) || !ell_inside(X + 2, Y, A, B) ||
                       !ell_inside(X, Y - 2, A, B) || !ell_inside(X, Y + 2, A, B);
            if (edge && have_pen)
                put_r(d, x, y, ppx, d->rop2);
            else if (have_brush)
                put_r(d, x, y, bpx, d->rop2);
        }
    }
    gdi_mark(d, &e);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* polygons                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
    double x;
    int dir;
} cross_t;

static int cmp_cross(const void *a, const void *b)
{
    double x = ((const cross_t *)a)->x, y = ((const cross_t *)b)->x;
    return (x > y) - (x < y);
}

static int ceil_i(double v)
{
    int i = (int)v;
    if (v > (double)i)
        i++;
    return i;
}

static void poly_fill(gdi_dc *d, const POINT *dp, int n, uint32_t px, int winding)
{
    int ymin = dp[0].y, ymax = dp[0].y;
    for (int i = 1; i < n; i++) {
        if (dp[i].y < ymin)
            ymin = dp[i].y;
        if (dp[i].y > ymax)
            ymax = dp[i].y;
    }
    cross_t *cr = malloc(sizeof *cr * (size_t)n);
    if (!cr)
        aw_fatal("gdi: out of memory");
    RECT clipbox = rect_and(d->cb, rect_make(0, 0, d->surf->w, d->surf->h));
    if (ymin < clipbox.top)
        ymin = clipbox.top;
    if (ymax > clipbox.bottom)
        ymax = clipbox.bottom;
    for (int y = ymin; y < ymax; y++) {
        double yc = y + 0.5;
        int nc = 0;
        for (int i = 0; i < n; i++) {
            const POINT *a = &dp[i], *b = &dp[(i + 1) % n];
            if (a->y == b->y)
                continue;
            if ((yc >= a->y && yc < b->y) || (yc >= b->y && yc < a->y)) {
                cr[nc].x = a->x + (yc - a->y) * (double)(b->x - a->x) / (double)(b->y - a->y);
                cr[nc].dir = b->y > a->y ? 1 : -1;
                nc++;
            }
        }
        qsort(cr, (size_t)nc, sizeof *cr, cmp_cross);
        if (!winding) {
            for (int k = 0; k + 1 < nc; k += 2) {
                int x0 = ceil_i(cr[k].x - 0.5), x1 = ceil_i(cr[k + 1].x - 0.5);
                if (x1 > x0)
                    fill_dev(d, rect_make(x0, y, x1, y + 1), px, d->rop2);
            }
        } else {
            int w = 0, start = 0;
            for (int k = 0; k < nc; k++) {
                int before = w;
                w += cr[k].dir;
                if (before == 0 && w != 0)
                    start = ceil_i(cr[k].x - 0.5);
                if (before != 0 && w == 0) {
                    int x1 = ceil_i(cr[k].x - 0.5);
                    if (x1 > start)
                        fill_dev(d, rect_make(start, y, x1, y + 1), px, d->rop2);
                }
            }
        }
    }
    free(cr);
}

static void poly_common(gdi_dc *d, const POINT *pts, int n, int closed)
{
    POINT *dp = malloc(sizeof *dp * (size_t)n);
    if (!dp)
        aw_fatal("gdi: out of memory");
    for (int i = 0; i < n; i++)
        gdi_map(d, pts[i].x, pts[i].y, &dp[i].x, &dp[i].y);
    uint32_t bpx;
    if (closed && get_brush(d, &bpx))
        poly_fill(d, dp, n, bpx, d->poly_fill == WINDING);
    pen_t p;
    if (get_pen(d, &p) && p.style != PS_NULL) {
        for (int i = 0; i + 1 < n; i++)
            line_dev(d, dp[i].x, dp[i].y, dp[i + 1].x, dp[i + 1].y, &p, 0);
        line_dev(d, dp[n - 1].x, dp[n - 1].y, dp[0].x, dp[0].y, &p, 0); /* closing edge */
    }
    RECT bb = {dp[0].x, dp[0].y, dp[0].x + 1, dp[0].y + 1};
    for (int i = 1; i < n; i++) {
        if (dp[i].x < bb.left)
            bb.left = dp[i].x;
        if (dp[i].y < bb.top)
            bb.top = dp[i].y;
        if (dp[i].x + 1 > bb.right)
            bb.right = dp[i].x + 1;
        if (dp[i].y + 1 > bb.bottom)
            bb.bottom = dp[i].y + 1;
    }
    gdi_mark(d, &bb);
    free(dp);
}

API(gdi32, BOOL, Polygon, (HDC hdc, const POINT *pts, int n))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || !pts || n < 2) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    poly_common(d, pts, n, 1);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* PatBlt / BitBlt (ternary raster ops over the DC's pixels)               */
/* ---------------------------------------------------------------------- */

static void blit_dev(gdi_dc *d, RECT dr, const uint32_t *src, uint32_t pat, uint32_t rop3)
{
    RECT srf = rect_make(0, 0, d->surf->w, d->surf->h);
    RECT c = rect_and(rect_and(dr, d->cb), srf);
    if (!rect_nonempty(c))
        return;
    int dw = dr.right - dr.left;
    for (int k = 0; k < d->clip.n; k++) {
        RECT q = rect_and(c, d->clip.r[k]);
        for (int y = q.top; y < q.bottom; y++) {
            uint32_t *row = surface_row(d->surf, y);
            for (int x = q.left; x < q.right; x++) {
                uint32_t s = src ? src[(size_t)(y - dr.top) * (size_t)dw + (size_t)(x - dr.left)] : 0;
                row[x] = gdi_rop3(rop3, pat, s, row[x]);
            }
        }
    }
    gdi_mark(d, &c);
}

API(gdi32, BOOL, PatBlt, (HDC hdc, int x, int y, int w, int h, DWORD rop))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (w <= 0 || h <= 0)
        return TRUE;
    uint32_t pat = 0;
    get_brush(d, &pat);
    RECT dr;
    gdi_map_rect(d, &(RECT){x, y, x + w, y + h}, &dr);
    blit_dev(d, dr, NULL, pat, (rop >> 16) & 0xFF);
    return TRUE;
}

API(gdi32, BOOL, BitBlt, (HDC hdc, int x, int y, int w, int h, HDC hsrc, int sx, int sy, DWORD rop))
{
    gdi_dc *d = gdi_get_dc(hdc);
    gdi_dc *s = hsrc ? gdi_get_dc(hsrc) : NULL;
    if (!d || (hsrc && !s)) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (w <= 0 || h <= 0)
        return TRUE;
    uint32_t rop3 = (rop >> 16) & 0xFF;
    uint32_t pat = 0;
    get_brush(d, &pat);
    RECT dr;
    gdi_map_rect(d, &(RECT){x, y, x + w, y + h}, &dr);
    uint32_t *buf = NULL;
    if (s) {
        /* copy the source first so overlapping source and destination are safe */
        buf = malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
        if (!buf)
            aw_fatal("gdi: out of memory");
        int ox, oy;
        gdi_map(s, sx, sy, &ox, &oy);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                int px = ox + i, py = oy + j;
                uint32_t v = 0;
                if ((unsigned)px < (unsigned)s->surf->w && (unsigned)py < (unsigned)s->surf->h)
                    v = surface_row(s->surf, py)[px] & 0x00FFFFFFu;
                buf[(size_t)j * (size_t)w + (size_t)i] = v;
            }
    }
    blit_dev(d, dr, buf, pat, rop3);
    free(buf);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* user32 rect helpers (drawing layer, see gui.h)                          */
/* ---------------------------------------------------------------------- */

API(user32, int, FillRect, (HDC hdc, const RECT *rc, HBRUSH br))
{
    gdi_dc *d = gdi_get_dc(hdc);
    COLORREF c;
    if (!d || !rc) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    int r = gdi_brush_color(d, br, &c);
    if (r < 0) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (r == 0)
        return 1;
    RECT dr;
    gdi_map_rect(d, rc, &dr);
    fill_dev(d, dr, gdi_px(c), R2_COPYPEN);
    return 1;
}

API(user32, int, FrameRect, (HDC hdc, const RECT *rc, HBRUSH br))
{
    gdi_dc *d = gdi_get_dc(hdc);
    COLORREF c;
    if (!d || !rc) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    int r = gdi_brush_color(d, br, &c);
    if (r < 0) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    RECT dr;
    gdi_map_rect(d, rc, &dr);
    if (r == 0 || !rect_nonempty(dr))
        return 1;
    uint32_t px = gdi_px(c);
    fill_dev(d, rect_make(dr.left, dr.top, dr.right, dr.top + 1), px, R2_COPYPEN);
    fill_dev(d, rect_make(dr.left, dr.bottom - 1, dr.right, dr.bottom), px, R2_COPYPEN);
    fill_dev(d, rect_make(dr.left, dr.top, dr.left + 1, dr.bottom), px, R2_COPYPEN);
    fill_dev(d, rect_make(dr.right - 1, dr.top, dr.right, dr.bottom), px, R2_COPYPEN);
    return 1;
}

API(user32, int, InvertRect, (HDC hdc, const RECT *rc))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || !rc) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    RECT dr;
    gdi_map_rect(d, rc, &dr);
    fill_dev(d, dr, 0x00FFFFFFu, R2_XORPEN);
    return 1;
}

API(user32, BOOL, DrawFocusRect, (HDC hdc, const RECT *rc))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || !rc) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    RECT dr;
    gdi_map_rect(d, rc, &dr);
    if (!rect_nonempty(dr))
        return TRUE;
    int l = dr.left, t = dr.top, r = dr.right - 1, b = dr.bottom - 1;
    /* dotted one-pixel frame drawn with XOR, so drawing it twice restores the background */
    for (int x = l; x <= r; x++)
        if (((x - l) & 1) == 0) {
            put_r(d, x, t, 0x00FFFFFFu, R2_XORPEN);
            put_r(d, x, b, 0x00FFFFFFu, R2_XORPEN);
        }
    for (int y = t + 1; y < b; y++)
        if (((y - t) & 1) == 0) {
            put_r(d, l, y, 0x00FFFFFFu, R2_XORPEN);
            put_r(d, r, y, 0x00FFFFFFu, R2_XORPEN);
        }
    gdi_mark(d, &dr);
    return TRUE;
}

/* DrawEdge: BDR_* rings (outer ring, then inner ring) with BF_* sides; Win2k/XP colours */
API(user32, BOOL, DrawEdge, (HDC hdc, RECT *rc, UINT edge, UINT flags))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || !rc) {
        aw_set_last_error(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    enum { BDR_RAISEDOUTER = 1, BDR_SUNKENOUTER = 2, BDR_RAISEDINNER = 4, BDR_SUNKENINNER = 8 };
    enum { BF_LEFT = 1, BF_TOP = 2, BF_RIGHT = 4, BF_BOTTOM = 8, BF_ADJUST = 0x2000, BF_MIDDLE = 0x0800 };
    int outer_on = (edge & (BDR_RAISEDOUTER | BDR_SUNKENOUTER)) != 0;
    int inner_on = (edge & (BDR_RAISEDINNER | BDR_SUNKENINNER)) != 0;
    int sides = flags & 0xF;
    RECT lr = *rc, dr;
    gdi_map_rect(d, &lr, &dr);
    RECT ring = dr; /* shrinks by one pixel after each ring: the middle is what remains */
    for (int k = 0; k < 2; k++) {
        int on = k == 0 ? outer_on : inner_on;
        if (!on)
            continue;
        int raised = k == 0 ? (edge & BDR_RAISEDOUTER) : (edge & BDR_RAISEDINNER);
        COLORREF tl, br;
        if (k == 0) {
            tl = aw_syscolor(raised ? AW_COLOR_3DHILIGHT : AW_COLOR_3DDKSHADOW);
            br = aw_syscolor(raised ? AW_COLOR_3DDKSHADOW : AW_COLOR_3DHILIGHT);
        } else {
            tl = aw_syscolor(raised ? AW_COLOR_3DLIGHT : AW_COLOR_3DSHADOW);
            br = aw_syscolor(raised ? AW_COLOR_3DSHADOW : AW_COLOR_3DLIGHT);
        }
        if (sides & BF_TOP)
            fill_dev(d, rect_make(ring.left, ring.top, ring.right, ring.top + 1), gdi_px(tl), R2_COPYPEN);
        if (sides & BF_LEFT)
            fill_dev(d, rect_make(ring.left, ring.top, ring.left + 1, ring.bottom), gdi_px(tl), R2_COPYPEN);
        if (sides & BF_BOTTOM)
            fill_dev(d, rect_make(ring.left, ring.bottom - 1, ring.right, ring.bottom), gdi_px(br), R2_COPYPEN);
        if (sides & BF_RIGHT)
            fill_dev(d, rect_make(ring.right - 1, ring.top, ring.right, ring.bottom), gdi_px(br), R2_COPYPEN);
        ring.left += 1;
        ring.top += 1;
        ring.right -= 1;
        ring.bottom -= 1;
    }
    if (flags & BF_MIDDLE)
        fill_dev(d, ring, gdi_px(aw_syscolor(AW_COLOR_BTNFACE)), R2_COPYPEN);
    if (flags & BF_ADJUST) {
        int thick = (outer_on ? 1 : 0) + (inner_on ? 1 : 0);
        if (sides & BF_LEFT)
            rc->left += thick;
        if (sides & BF_TOP)
            rc->top += thick;
        if (sides & BF_RIGHT)
            rc->right -= thick;
        if (sides & BF_BOTTOM)
            rc->bottom -= thick;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* icons: alpha-blended scaled ARGB image through the DC                   */
/* ---------------------------------------------------------------------- */

void gdi_draw_argb(HDC hdc, int x, int y, int w, int h, const uint32_t *argb, int sw, int sh)
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || !argb || w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return;
    RECT dr;
    gdi_map_rect(d, &(RECT){x, y, x + w, y + h}, &dr);
    RECT c = rect_and(rect_and(dr, d->cb), rect_make(0, 0, d->surf->w, d->surf->h));
    if (!rect_nonempty(c))
        return;
    for (int yy = c.top; yy < c.bottom; yy++) {
        int sy = (int)((long long)(yy - dr.top) * sh / h);
        if (sy >= sh)
            sy = sh - 1;
        for (int xx = c.left; xx < c.right; xx++) {
            int sx = (int)((long long)(xx - dr.left) * sw / w);
            if (sx >= sw)
                sx = sw - 1;
            uint32_t s = argb[(size_t)sy * (size_t)sw + (size_t)sx];
            gdi_alpha_put(d, xx, yy, s & 0x00FFFFFFu, s >> 24);
        }
    }
    gdi_mark(d, &c);
}
