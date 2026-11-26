/* anywin surfaces (gui.h surface_*): 32bpp 0x00RRGGBB pixel buffers with a dirty rectangle.
 * Rectangles follow Win32 conventions: right and bottom are exclusive. */
#include "gui.h"

static void dirty_union(aw_surface *s, const RECT *c)
{
    if (!s->has_dirty) {
        s->dirty = *c;
        s->has_dirty = 1;
        return;
    }
    if (c->left < s->dirty.left)
        s->dirty.left = c->left;
    if (c->top < s->dirty.top)
        s->dirty.top = c->top;
    if (c->right > s->dirty.right)
        s->dirty.right = c->right;
    if (c->bottom > s->dirty.bottom)
        s->dirty.bottom = c->bottom;
}

aw_surface *surface_new(int w, int h)
{
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    aw_surface *s = calloc(1, sizeof *s);
    if (!s)
        aw_fatal("surface: out of memory");
    s->px = calloc((size_t)w * (size_t)h + 1, sizeof(uint32_t));
    if (!s->px)
        aw_fatal("surface: out of memory");
    s->w = w;
    s->h = h;
    s->stride = w;
    s->owns_px = 1;
    return s;
}

aw_surface *surface_wrap(uint32_t *px, int w, int h, int stride)
{
    aw_surface *s = calloc(1, sizeof *s);
    if (!s)
        aw_fatal("surface: out of memory");
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    s->owns_px = 0;
    return s;
}

void surface_free(aw_surface *s)
{
    if (!s)
        return;
    if (s->owns_px)
        free(s->px);
    free(s);
}

void surface_resize(aw_surface *s, int w, int h)
{
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    if (!s->owns_px) {
        /* wrapped memory keeps its stride; the owner guarantees the storage */
        s->w = w;
        s->h = h;
        return;
    }
    uint32_t *np = malloc(((size_t)w * (size_t)h + 1) * sizeof(uint32_t));
    if (!np)
        aw_fatal("surface: out of memory");
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        np[i] = 0x00FFFFFFu;
    int cw = s->w < w ? s->w : w, ch = s->h < h ? s->h : h;
    for (int y = 0; y < ch; y++)
        memcpy(np + (size_t)y * (size_t)w, surface_row(s, y), sizeof(uint32_t) * (size_t)cw);
    free(s->px);
    s->px = np;
    s->w = w;
    s->h = h;
    s->stride = w;
    if (s->has_dirty) {
        RECT all = {0, 0, w, h};
        RECT d = s->dirty;
        if (d.left < all.left)
            d.left = all.left;
        if (d.top < all.top)
            d.top = all.top;
        if (d.right > all.right)
            d.right = all.right;
        if (d.bottom > all.bottom)
            d.bottom = all.bottom;
        if (d.left < d.right && d.top < d.bottom)
            s->dirty = d;
        else
            s->has_dirty = 0;
    }
}

void surface_mark_dirty(aw_surface *s, const RECT *r)
{
    if (!s || !r)
        return;
    RECT c = *r;
    if (c.left < 0)
        c.left = 0;
    if (c.top < 0)
        c.top = 0;
    if (c.right > s->w)
        c.right = s->w;
    if (c.bottom > s->h)
        c.bottom = s->h;
    if (c.left >= c.right || c.top >= c.bottom)
        return;
    dirty_union(s, &c);
}
