/* anywin GDI regions (gui.h rgn_*): sets of non-overlapping, non-empty rectangles.
 *
 * Boolean operations sweep horizontal bands cut at every top/bottom edge of both operands.
 * Each band yields sorted, merged x-intervals; an interval that continues unchanged from the
 * band above extends the rectangle above it instead of starting a new one.  The result is
 * canonical: equal point sets give identical rect arrays (ordered by top, then left), so
 * gdi_rgn_equal() is a plain array compare. */
#include "gdi_int.h"

typedef struct {
    int l, r;
} ivl;

static void rgn_reserve(aw_region *g, int need)
{
    if (need <= g->cap)
        return;
    int nc = g->cap > 0 ? g->cap : 4;
    while (nc < need)
        nc *= 2;
    RECT *p = realloc(g->r, (size_t)nc * sizeof *p);
    if (!p)
        aw_fatal("rgn: out of memory");
    g->r = p;
    g->cap = nc;
}

static int rect_empty(const RECT *r) { return r->left >= r->right || r->top >= r->bottom; }

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_ivl(const void *a, const void *b)
{
    int x = ((const ivl *)a)->l, y = ((const ivl *)b)->l;
    return (x > y) - (x < y);
}

/* sort by left edge and merge overlapping or touching intervals; returns the new count */
static int ivl_norm(ivl *v, int n)
{
    if (n <= 1)
        return n;
    qsort(v, (size_t)n, sizeof *v, cmp_ivl);
    int k = 0;
    for (int i = 1; i < n; i++) {
        if (v[i].l <= v[k].r) {
            if (v[i].r > v[k].r)
                v[k].r = v[i].r;
        } else {
            v[++k] = v[i];
        }
    }
    return k + 1;
}

/* x-intervals of the rects of g that cover the whole band [y0, y1) */
static int band_ivls(const aw_region *g, int y0, int y1, ivl *out)
{
    int k = 0;
    for (int i = 0; i < g->n; i++) {
        const RECT *r = &g->r[i];
        if (r->top <= y0 && r->bottom >= y1 && r->left < r->right) {
            out[k].l = r->left;
            out[k].r = r->right;
            k++;
        }
    }
    return ivl_norm(out, k);
}

enum { OP_UNION, OP_SUB, OP_AND };

/* combine two sorted, merged interval lists; res has room for na + nb intervals */
static int ivl_combine(const ivl *a, int na, const ivl *b, int nb, int op, ivl *res)
{
    int k = 0;
    if (op == OP_UNION) {
        for (int i = 0; i < na; i++)
            res[k++] = a[i];
        for (int j = 0; j < nb; j++)
            res[k++] = b[j];
        return ivl_norm(res, k);
    }
    if (op == OP_AND) {
        int i = 0, j = 0;
        while (i < na && j < nb) {
            int l = a[i].l > b[j].l ? a[i].l : b[j].l;
            int r = a[i].r < b[j].r ? a[i].r : b[j].r;
            if (l < r) {
                res[k].l = l;
                res[k].r = r;
                k++;
            }
            if (a[i].r < b[j].r)
                i++;
            else
                j++;
        }
        return k;
    }
    /* OP_SUB: a minus b */
    int js = 0;
    for (int i = 0; i < na; i++) {
        int cur = a[i].l, end = a[i].r;
        while (js < nb && b[js].r <= cur)
            js++;
        for (int j = js; j < nb && cur < end; j++) {
            if (b[j].l >= end)
                break;
            if (b[j].l > cur) {
                res[k].l = cur;
                res[k].r = b[j].l;
                k++;
            }
            if (b[j].r > cur)
                cur = b[j].r;
        }
        if (cur < end) {
            res[k].l = cur;
            res[k].r = end;
            k++;
        }
    }
    return k;
}

static void rgn_combine(aw_region *dst, const aw_region *a, const aw_region *b, int op)
{
    int na = a->n, nb = b->n, total = na + nb;
    int *ys = malloc(sizeof(int) * (size_t)(2 * total + 1));
    ivl *buf = malloc(sizeof(ivl) * (size_t)(3 * (total + 1)));
    int *open = malloc(sizeof(int) * (size_t)(total + 1));
    int *newopen = malloc(sizeof(int) * (size_t)(total + 1));
    if (!ys || !buf || !open || !newopen)
        aw_fatal("rgn: out of memory");
    ivl *ia = buf, *ib = buf + (total + 1), *ires = buf + 2 * (total + 1);

    int ny = 0;
    for (int i = 0; i < na; i++)
        if (!rect_empty(&a->r[i])) {
            ys[ny++] = a->r[i].top;
            ys[ny++] = a->r[i].bottom;
        }
    for (int i = 0; i < nb; i++)
        if (!rect_empty(&b->r[i])) {
            ys[ny++] = b->r[i].top;
            ys[ny++] = b->r[i].bottom;
        }
    qsort(ys, (size_t)ny, sizeof *ys, cmp_int);
    int nys = 0;
    for (int i = 0; i < ny; i++)
        if (nys == 0 || ys[nys - 1] != ys[i])
            ys[nys++] = ys[i];

    aw_region out = {0, 0, NULL};
    int nopen = 0;
    for (int k = 0; k + 1 < nys; k++) {
        int y0 = ys[k], y1 = ys[k + 1];
        int na_b = band_ivls(a, y0, y1, ia);
        int nb_b = band_ivls(b, y0, y1, ib);
        int nres = ivl_combine(ia, na_b, ib, nb_b, op, ires);

        int nnew = 0, j = 0;
        for (int i = 0; i < nres; i++) {
            int l = ires[i].l, r = ires[i].r;
            while (j < nopen && out.r[open[j]].left < l)
                j++;
            if (j < nopen && out.r[open[j]].left == l && out.r[open[j]].right == r &&
                out.r[open[j]].bottom == y0) {
                out.r[open[j]].bottom = y1;
                newopen[nnew++] = open[j];
                j++;
            } else {
                rgn_reserve(&out, out.n + 1);
                RECT *nr = &out.r[out.n];
                nr->left = l;
                nr->top = y0;
                nr->right = r;
                nr->bottom = y1;
                newopen[nnew++] = out.n;
                out.n++;
            }
        }
        int *t = open;
        open = newopen;
        newopen = t;
        nopen = nnew;
    }

    free(dst->r);
    *dst = out;
    free(ys);
    free(buf);
    free(open);
    free(newopen);
}

/* ---------------------------------------------------------------------- */

void rgn_init(aw_region *g)
{
    g->n = 0;
    g->cap = 0;
    g->r = NULL;
}

void rgn_free(aw_region *g)
{
    free(g->r);
    rgn_init(g);
}

void rgn_clear(aw_region *g) { g->n = 0; }

void rgn_copy(aw_region *dst, const aw_region *src)
{
    if (dst == src)
        return;
    if (src->n == 0) {
        dst->n = 0;
        return;
    }
    rgn_reserve(dst, src->n);
    memcpy(dst->r, src->r, sizeof(RECT) * (size_t)src->n);
    dst->n = src->n;
}

void rgn_set_rect(aw_region *g, const RECT *r)
{
    if (rect_empty(r)) {
        g->n = 0;
        return;
    }
    rgn_reserve(g, 1);
    g->r[0] = *r;
    g->n = 1;
}

void rgn_union_rect(aw_region *g, const RECT *r)
{
    if (rect_empty(r))
        return;
    aw_region t = {1, 1, (RECT *)r};
    rgn_combine(g, g, &t, OP_UNION);
}

void rgn_subtract_rect(aw_region *g, const RECT *r)
{
    if (rect_empty(r) || g->n == 0)
        return;
    aw_region t = {1, 1, (RECT *)r};
    rgn_combine(g, g, &t, OP_SUB);
}

void rgn_intersect_rect(aw_region *g, const RECT *r)
{
    if (rect_empty(r)) {
        g->n = 0;
        return;
    }
    aw_region t = {1, 1, (RECT *)r};
    rgn_combine(g, g, &t, OP_AND);
}

void rgn_union(aw_region *g, const aw_region *o) { rgn_combine(g, g, o, OP_UNION); }
void rgn_subtract(aw_region *g, const aw_region *o) { rgn_combine(g, g, o, OP_SUB); }
void rgn_intersect(aw_region *g, const aw_region *o) { rgn_combine(g, g, o, OP_AND); }

void rgn_offset(aw_region *g, int dx, int dy)
{
    for (int i = 0; i < g->n; i++) {
        g->r[i].left += dx;
        g->r[i].right += dx;
        g->r[i].top += dy;
        g->r[i].bottom += dy;
    }
}

int rgn_is_empty(const aw_region *g) { return g->n == 0; }

void rgn_bbox(const aw_region *g, RECT *out)
{
    if (g->n == 0) {
        out->left = out->top = out->right = out->bottom = 0;
        return;
    }
    *out = g->r[0];
    for (int i = 1; i < g->n; i++) {
        const RECT *r = &g->r[i];
        if (r->left < out->left)
            out->left = r->left;
        if (r->top < out->top)
            out->top = r->top;
        if (r->right > out->right)
            out->right = r->right;
        if (r->bottom > out->bottom)
            out->bottom = r->bottom;
    }
}

int rgn_contains_point(const aw_region *g, int x, int y)
{
    for (int i = 0; i < g->n; i++) {
        const RECT *r = &g->r[i];
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom)
            return 1;
    }
    return 0;
}

int rgn_intersects_rect(const aw_region *g, const RECT *q)
{
    if (rect_empty(q))
        return 0;
    for (int i = 0; i < g->n; i++) {
        const RECT *r = &g->r[i];
        if (r->left < q->right && q->left < r->right && r->top < q->bottom && q->top < r->bottom)
            return 1;
    }
    return 0;
}

/* canonical form makes equality an element-wise compare (gdi.c EqualRgn) */
int gdi_rgn_equal(const aw_region *a, const aw_region *b)
{
    if (a->n != b->n)
        return 0;
    return a->n == 0 || memcmp(a->r, b->r, sizeof(RECT) * (size_t)a->n) == 0;
}
