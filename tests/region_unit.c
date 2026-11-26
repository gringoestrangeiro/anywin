/* Region unit test (S4): random rect-list operations checked against a bitmap oracle.
 * White-box: includes runtime/region.c directly.  Usage: region_unit [seed] [trials].
 * Every operation result must (a) match the bitmap cell by cell, (b) be canonical: rects are
 * non-empty, pairwise disjoint, ordered by (top, left) and never two vertically adjacent
 * rects with the same x-extent; (c) equal a region rebuilt from the bitmap, which fails
 * if any operation left a non-canonical representation behind.  Exit 0 = PASS. */
#include "../runtime/region.c"

#include <stdarg.h>

void aw_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

#define G 64 /* bitmap size; coordinates are generated in [8, 40) so offsets stay inside */
static unsigned char bm[G][G], tmp[G][G];
static unsigned long checks;
static int failures;

#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            if (failures <= 10) {                                                                  \
                fprintf(stderr, "FAIL line %d: ", __LINE__);                                       \
                fprintf(stderr, __VA_ARGS__);                                                      \
                fputc('\n', stderr);                                                               \
            }                                                                                      \
        }                                                                                          \
    } while (0)

static uint32_t rng_state = 1;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}
static int rndr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo)); }

static RECT rand_rect(void)
{
    RECT r;
    r.left = rndr(8, 40);
    r.top = rndr(8, 40);
    r.right = r.left + rndr(1, 12);
    r.bottom = r.top + rndr(1, 12);
    if (r.right > 40)
        r.right = 40;
    if (r.bottom > 40)
        r.bottom = 40;
    return r;
}

/* ---- oracle helpers --------------------------------------------------- */

static void bm_clear(unsigned char b[G][G]) { memset(b, 0, sizeof bm); }

static void bm_fill(unsigned char b[G][G], const RECT *r, int v)
{
    for (int y = r->top; y < r->bottom; y++)
        for (int x = r->left; x < r->right; x++)
            b[y][x] = (unsigned char)v;
}

static void bm_combine(unsigned char out[G][G], unsigned char a[G][G], unsigned char b[G][G], int op)
{
    for (int y = 0; y < G; y++)
        for (int x = 0; x < G; x++) {
            int p = a[y][x], q = b[y][x];
            out[y][x] = (unsigned char)(op == OP_UNION ? (p || q) : op == OP_AND ? (p && q) : (p && !q));
        }
}

static void bm_copy(unsigned char dst[G][G], unsigned char src[G][G]) { memcpy(dst, src, sizeof bm); }

/* region rebuilt from scratch: union of the horizontal runs of each bitmap row */
static void region_from_bitmap(aw_region *g, unsigned char b[G][G])
{
    rgn_init(g);
    for (int y = 0; y < G; y++) {
        int x = 0;
        while (x < G) {
            if (!b[y][x]) {
                x++;
                continue;
            }
            int x0 = x;
            while (x < G && b[y][x])
                x++;
            RECT r = {x0, y, x, y + 1};
            rgn_union_rect(g, &r);
        }
    }
}

/* ---- invariant + oracle check ----------------------------------------- */

static void check_state(const aw_region *g, unsigned char b[G][G], const char *what)
{
    int any = 0, bx0 = G, by0 = G, bx1 = 0, by1 = 0;
    for (int y = 0; y < G; y++)
        for (int x = 0; x < G; x++)
            if (b[y][x]) {
                any = 1;
                if (x < bx0)
                    bx0 = x;
                if (x + 1 > bx1)
                    bx1 = x + 1;
                if (y < by0)
                    by0 = y;
                if (y + 1 > by1)
                    by1 = y + 1;
            }
    for (int i = 0; i < g->n; i++) {
        const RECT *r = &g->r[i];
        CHECK(r->left < r->right && r->top < r->bottom, "%s: empty rect %d", what, i);
        for (int j = i + 1; j < g->n; j++) {
            const RECT *q = &g->r[j];
            CHECK(!(r->left < q->right && q->left < r->right && r->top < q->bottom && q->top < r->bottom),
                  "%s: rects %d and %d overlap", what, i, j);
            CHECK(r->top < q->top || (r->top == q->top && r->left < q->left),
                  "%s: rect order broken at %d,%d", what, i, j);
            CHECK(!(r->left == q->left && r->right == q->right && r->bottom == q->top),
                  "%s: rects %d,%d should be merged", what, i, j);
        }
    }
    for (int y = 0; y < G; y++)
        for (int x = 0; x < G; x++)
            CHECK(rgn_contains_point(g, x, y) == b[y][x], "%s: cell (%d,%d) got %d want %d", what, x, y,
                  rgn_contains_point(g, x, y), b[y][x]);
    CHECK(rgn_is_empty(g) == !any, "%s: is_empty mismatch", what);
    RECT bb;
    rgn_bbox(g, &bb);
    if (any)
        CHECK(bb.left == bx0 && bb.top == by0 && bb.right == bx1 && bb.bottom == by1,
              "%s: bbox mismatch", what);
    else
        CHECK(bb.left == 0 && bb.top == 0 && bb.right == 0 && bb.bottom == 0, "%s: empty bbox", what);
    aw_region ref;
    region_from_bitmap(&ref, b);
    CHECK(gdi_rgn_equal(g, &ref), "%s: not canonical (n=%d, reference n=%d)", what, g->n, ref.n);
    rgn_free(&ref);
}

static void check_probes(const aw_region *g, unsigned char b[G][G])
{
    for (int k = 0; k < 8; k++) {
        RECT q = rand_rect();
        int want = 0;
        for (int y = q.top; y < q.bottom; y++)
            for (int x = q.left; x < q.right; x++)
                want |= b[y][x];
        CHECK(rgn_intersects_rect(g, &q) == want, "intersects_rect mismatch");
    }
}

/* ---- deterministic cases ----------------------------------------------- */

static void fixed_cases(void)
{
    aw_region g;
    rgn_init(&g);
    RECT empty = {5, 5, 5, 9};
    rgn_set_rect(&g, &empty);
    CHECK(rgn_is_empty(&g), "empty rect gives empty region");
    RECT a = {0, 0, 10, 10}, b = {10, 0, 20, 10};
    rgn_set_rect(&g, &a);
    rgn_union_rect(&g, &b);
    CHECK(g.n == 1 && g.r[0].right == 20, "adjacent rects must merge into one");
    CHECK(rgn_contains_point(&g, 19, 9) && !rgn_contains_point(&g, 20, 9), "right edge exclusive");
    CHECK(!rgn_contains_point(&g, 5, 10), "bottom edge exclusive");
    RECT mid = {5, 0, 15, 10};
    rgn_subtract_rect(&g, &mid);
    CHECK(g.n == 2, "middle cut leaves two rects, got %d", g.n);
    rgn_subtract_rect(&g, &g.r[0]);
    rgn_subtract_rect(&g, &g.r[0]);
    CHECK(rgn_is_empty(&g), "subtracting everything empties");
    RECT big = {0, 0, 100, 100};
    rgn_set_rect(&g, &big);
    rgn_intersect_rect(&g, &a);
    RECT ob;
    rgn_bbox(&g, &ob);
    CHECK(ob.left == 0 && ob.right == 10 && ob.bottom == 10, "intersect clips to rect");
    rgn_offset(&g, 3, -2);
    CHECK(g.r[0].left == 3 && g.r[0].top == -2, "offset moves rects");
    aw_region c;
    rgn_init(&c);
    rgn_copy(&c, &g);
    CHECK(gdi_rgn_equal(&c, &g), "copy equals source");
    rgn_clear(&c);
    CHECK(rgn_is_empty(&c) && !gdi_rgn_equal(&c, &g), "clear empties");
    rgn_free(&c);
    rgn_free(&g);
}

int main(int argc, char **argv)
{
    unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 12345u;
    int trials = argc > 2 ? atoi(argv[2]) : 400;
    rng_state = seed ? seed : 1;

    fixed_cases();

    for (int t = 0; t < trials; t++) {
        aw_region g, o, scratch;
        rgn_init(&g);
        rgn_init(&o);
        rgn_init(&scratch);
        bm_clear(bm);
        check_state(&g, bm, "initial");
        for (int step = 0; step < 14; step++) {
            int op = (int)(rnd() % 10);
            RECT r = rand_rect();
            char what[64];
            snprintf(what, sizeof what, "trial %d step %d op %d", t, step, op);
            switch (op) {
            case 0: /* set_rect */
                rgn_set_rect(&g, &r);
                bm_clear(bm);
                bm_fill(bm, &r, 1);
                break;
            case 1: /* union_rect */
                rgn_union_rect(&g, &r);
                bm_fill(bm, &r, 1);
                break;
            case 2: /* subtract_rect */
                rgn_subtract_rect(&g, &r);
                bm_fill(bm, &r, 0);
                break;
            case 3: /* intersect_rect */
                rgn_intersect_rect(&g, &r);
                for (int y = 0; y < G; y++)
                    for (int x = 0; x < G; x++)
                        tmp[y][x] = bm[y][x] && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
                bm_copy(bm, tmp);
                break;
            case 4: /* union with a second random region */
            case 5: /* subtract with a second random region */
            case 6: { /* intersect with a second random region */
                rgn_clear(&o);
                unsigned char ob[G][G];
                bm_clear(ob);
                int k = rndr(1, 4);
                for (int i = 0; i < k; i++) {
                    RECT q = rand_rect();
                    rgn_union_rect(&o, &q);
                    bm_fill(ob, &q, 1);
                }
                if (op == 4) {
                    rgn_union(&g, &o);
                    bm_combine(tmp, bm, ob, OP_UNION);
                } else if (op == 5) {
                    rgn_subtract(&g, &o);
                    bm_combine(tmp, bm, ob, OP_SUB);
                } else {
                    rgn_intersect(&g, &o);
                    bm_combine(tmp, bm, ob, OP_AND);
                }
                bm_copy(bm, tmp);
                check_state(&o, ob, what);
                break;
            }
            case 7: { /* offset, limited so the content stays inside the oracle grid */
                int x0 = G, y0 = G, x1 = 0, y1 = 0;
                for (int y = 0; y < G; y++)
                    for (int x = 0; x < G; x++)
                        if (bm[y][x]) {
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                            if (y < y0) y0 = y;
                            if (y > y1) y1 = y;
                        }
                int lo_x = -6 > 1 - x0 ? -6 : 1 - x0, hi_x = 6 < G - 2 - x1 ? 6 : G - 2 - x1;
                int lo_y = -6 > 1 - y0 ? -6 : 1 - y0, hi_y = 6 < G - 2 - y1 ? 6 : G - 2 - y1;
                int dx = x0 > x1 ? 0 : rndr(lo_x, hi_x + 1), dy = y0 > y1 ? 0 : rndr(lo_y, hi_y + 1);
                rgn_offset(&g, dx, dy);
                memset(tmp, 0, sizeof tmp);
                for (int y = 0; y < G; y++)
                    for (int x = 0; x < G; x++)
                        if (bm[y][x] && x + dx >= 0 && x + dx < G && y + dy >= 0 && y + dy < G)
                            tmp[y + dy][x + dx] = 1;
                bm_copy(bm, tmp);
                break;
            }
            case 8: { /* copy into scratch and back */
                rgn_copy(&scratch, &g);
                rgn_clear(&g);
                rgn_copy(&g, &scratch);
                break;
            }
            default: { /* union with itself and subtract from itself */
                aw_region self;
                rgn_init(&self);
                rgn_copy(&self, &g);
                rgn_union(&g, &self);
                rgn_subtract(&self, &g);
                CHECK(rgn_is_empty(&self), "%s: g minus g not empty", what);
                rgn_free(&self);
                break;
            }
            }
            check_state(&g, bm, what);
        }
        check_probes(&g, bm);
        rgn_free(&g);
        rgn_free(&o);
        rgn_free(&scratch);
    }

    if (failures) {
        printf("region_unit: FAIL (%d failures in %lu checks)\n", failures, checks);
        return 1;
    }
    printf("region_unit: PASS (%d trials, %lu checks, seed %u)\n", trials, checks, seed);
    return 0;
}
