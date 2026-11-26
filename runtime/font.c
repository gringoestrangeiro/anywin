/* anywin font layer (gui.h font_*): TrueType (glyf) rasterizer with 8-bit antialiasing.
 * Scope and deferrals: PLAN-CORE.md 3.8 (no CFF/OTF, TTC, CJK chains, kerning, hinting).
 * Faces are looked up by name alias (Segoe UI -> sans, Courier New -> mono, ...) and by file
 * basename in the usual font directories.  Without any usable file a built-in 5x7 bitmap font
 * is used.  No libm: floor/ceil/sqrt are implemented locally. */
#include "gui.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_FACES 128
#define MAX_FONTS 512
#define GLYPH_BUCKETS 128
#define MAX_FALLBACK 6

/* ---------------- small math (no libm) ---------------- */
static double dfloor(double x) {
    long long i = (long long)x;
    if (x < 0 && (double)i != x) i--;
    return (double)i;
}
static double dceil(double x) {
    long long i = (long long)x;
    if (x > 0 && (double)i != x) i++;
    return (double)i;
}
static double dsqrt(double x) {
    if (x <= 0) return 0;
    double g = x > 1 ? x : 1;
    for (int i = 0; i < 40; i++) g = 0.5 * (g + x / g);
    return g;
}
static double dabs(double x) { return x < 0 ? -x : x; }
static int iround(double x) { return (int)dfloor(x + 0.5); }

/* ---------------- faces (parsed TrueType files, cached forever) ---------------- */
typedef struct face {
    char *path;
    uint8_t *d;
    size_t len;
    int ok;
    int upem, nglyphs, nhm, loca_long, avg_w, max_adv, win_asc, win_desc, fixed;
    size_t loca, glyf, hmtx, cmap_sub;
    int cmap_fmt;
} face;

static face *faces[MAX_FACES];
static int nfaces;

static uint32_t rdb(const face *f, size_t o) { return o < f->len ? f->d[o] : 0; }
static uint32_t rd16(const face *f, size_t o) {
    if (o + 2 > f->len) return 0;
    return (uint32_t)f->d[o] << 8 | f->d[o + 1];
}
static uint32_t rd32(const face *f, size_t o) {
    if (o + 4 > f->len) return 0;
    return (uint32_t)f->d[o] << 24 | (uint32_t)f->d[o + 1] << 16 | (uint32_t)f->d[o + 2] << 8 | f->d[o + 3];
}
static int rds16(const face *f, size_t o) { return (int16_t)rd16(f, o); }

static int parse_face(face *f) {
    uint32_t tag = rd32(f, 0);
    if (tag != 0x00010000u && tag != 0x74727565u) return 0; /* 'true' */
    unsigned nt = rd16(f, 4);
    size_t head = 0, maxp = 0, loca = 0, glyf = 0, cmap = 0, hhea = 0, hmtx = 0, os2 = 0, post = 0;
    for (unsigned i = 0; i < nt && i < 256; i++) {
        size_t r = 12 + 16 * (size_t)i;
        uint32_t t = rd32(f, r);
        size_t off = rd32(f, r + 8);
        if (off == 0 || off >= f->len) continue;
        switch (t) {
        case 0x68656164: head = off; break;  /* head */
        case 0x6d617870: maxp = off; break;  /* maxp */
        case 0x6c6f6361: loca = off; break;  /* loca */
        case 0x676c7966: glyf = off; break;  /* glyf */
        case 0x636d6170: cmap = off; break;  /* cmap */
        case 0x68686561: hhea = off; break;  /* hhea */
        case 0x686d7478: hmtx = off; break;  /* hmtx */
        case 0x4f532f32: os2 = off; break;   /* OS/2 */
        case 0x706f7374: post = off; break;  /* post */
        default: break;
        }
    }
    if (!head || !maxp || !loca || !glyf || !cmap || !hhea || !hmtx) return 0;
    f->upem = (int)rd16(f, head + 18);
    if (f->upem < 16 || f->upem > 16384) return 0;
    f->loca_long = rds16(f, head + 50) != 0;
    f->nglyphs = (int)rd16(f, maxp + 4);
    f->nhm = (int)rd16(f, hhea + 34);
    f->max_adv = rds16(f, hhea + 10);
    f->loca = loca;
    f->glyf = glyf;
    f->hmtx = hmtx;
    if (os2 && os2 + 78 <= f->len) {
        f->avg_w = rds16(f, os2 + 2);
        f->win_asc = rds16(f, os2 + 74);
        f->win_desc = rds16(f, os2 + 76);
    }
    if (f->win_asc + f->win_desc <= 0) {
        f->win_asc = rds16(f, hhea + 4);
        f->win_desc = -rds16(f, hhea + 6);
    }
    f->fixed = post ? rd32(f, post + 12) != 0 : 0;

    /* cmap: format 12 beats format 4; Unicode platform / encoding preferred */
    unsigned ne = rd16(f, cmap + 2);
    int best = 0;
    for (unsigned i = 0; i < ne && i < 64; i++) {
        size_t r = cmap + 4 + 8 * (size_t)i;
        unsigned pid = (unsigned)rd16(f, r), eid = (unsigned)rd16(f, r + 2);
        size_t off = cmap + rd32(f, r + 4);
        unsigned sf = (unsigned)rd16(f, off);
        int score = 0;
        if (sf == 12 && (pid == 0 || (pid == 3 && eid == 10))) score = 3;
        else if (sf == 12) score = 2;
        else if (sf == 4 && (pid == 0 || (pid == 3 && (eid == 1 || eid == 10)))) score = 1;
        if (score > best) {
            best = score;
            f->cmap_sub = off;
            f->cmap_fmt = (int)sf;
        }
    }
    return best && f->nglyphs > 0;
}

static face *face_load(const char *path) {
    for (int i = 0; i < nfaces; i++)
        if (!strcmp(faces[i]->path, path)) return faces[i]->ok ? faces[i] : NULL;
    if (nfaces == MAX_FACES) return NULL;
    face *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->path = strdup(path);
    faces[nfaces++] = f;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    rewind(fp);
    if (n > 12 && n < 64L * 1024 * 1024) {
        f->d = malloc((size_t)n);
        if (f->d && fread(f->d, 1, (size_t)n, fp) == (size_t)n) {
            f->len = (size_t)n;
            if (parse_face(f)) f->ok = 1;
        }
    }
    fclose(fp);
    if (!f->ok) {
        free(f->d);
        f->d = NULL;
        return NULL;
    }
    return f;
}

/* ---------------- face index: normalized basename -> path ---------------- */
typedef struct {
    char *path;
    char norm[96];
} fentry;
static fentry *idx;
static int nidx, capidx, idx_built;

static void norm_name(const char *s, char *o, size_t cap) {
    size_t k = 0;
    for (; *s && k + 1 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o[k++] = (char)c;
    }
    o[k] = 0;
}

static void index_add(const char *path, const char *base, size_t blen) {
    if (nidx == capidx) {
        int c = capidx ? capidx * 2 : 64;
        fentry *n = realloc(idx, sizeof *idx * (size_t)c);
        if (!n) return;
        idx = n;
        capidx = c;
    }
    char tmp[256];
    if (blen >= sizeof tmp) blen = sizeof tmp - 1;
    memcpy(tmp, base, blen);
    tmp[blen] = 0;
    idx[nidx].path = strdup(path);
    norm_name(tmp, idx[nidx].norm, sizeof idx[nidx].norm);
    nidx++;
}

static void index_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 6) index_dir(path, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcasecmp(e->d_name + n - 4, ".ttf")) index_add(path, e->d_name, n - 4);
        }
    }
    closedir(d);
}

static void build_index(void) {
    if (idx_built) return;
    idx_built = 1;
    const char *fd = getenv("ANYWIN_FONT_DIR");
    if (fd && *fd) index_dir(fd, 0);
    const char *home = getenv("HOME");
    char p[600];
    if (home && *home) {
        snprintf(p, sizeof p, "%s/.local/share/fonts", home);
        index_dir(p, 0);
        snprintf(p, sizeof p, "%s/.fonts", home);
        index_dir(p, 0);
    }
    index_dir("/usr/local/share/fonts", 0);
    index_dir("/usr/share/fonts", 0);
}

/* first indexed file whose normalized name is fam+suffix, suffixes in preference order */
static const char *const suf_regular[] = {"regular", "", NULL};
static const char *const suf_bold[] = {"bold", NULL};
static const char *const suf_italic[] = {"italic", "oblique", NULL};
static const char *const suf_bolditalic[] = {"bolditalic", "boldoblique", NULL};

static const char *find_file(const char *fam, const char *const *sufs) {
    char want[128];
    for (int s = 0; sufs[s]; s++) {
        snprintf(want, sizeof want, "%s%s", fam, sufs[s]);
        for (int i = 0; i < nidx; i++)
            if (!strcmp(idx[i].norm, want)) return idx[i].path;
    }
    return NULL;
}

/* family alias tables (normalized names) */
static const char *const sans_chain[] = {"notosans", "dejavusans", "liberationsans", NULL};
static const char *const arial_chain[] = {"liberationsans", "notosans", "dejavusans", NULL};
static const char *const mono_chain[] = {"dejavusansmono", "liberationmono", "notosansmono", NULL};
static const char *const serif_chain[] = {"liberationserif", "dejavuserif", "notoserif", NULL};
static const char *const sans_names[] = {"segoeui", "tahoma", "msshelldlg", "msshelldlg2",
                                         "microsoftsansserif", "verdana", "calibri", "system",
                                         "sans", "sansserif", "", NULL};
static const char *const arial_names[] = {"arial", "helvetica", NULL};
static const char *const mono_names[] = {"couriernew", "consolas", "lucidaconsole", "terminal",
                                         "fixedsys", "monospace", "courier", NULL};
static const char *const serif_names[] = {"timesnewroman", "georgia", "cambria", "times", "serif", NULL};

static int in_list(const char *s, const char *const *list) {
    for (int i = 0; list[i]; i++)
        if (!strcmp(s, list[i])) return 1;
    return 0;
}

/* Family candidates for a requested face name: the name itself (if it is not an alias), then
 * the alias chain.  Returns the number of entries written to ch. */
static int build_chain(const char *norm, const char **ch, int max) {
    int n = 0;
    const char *const *src;
    if (in_list(norm, arial_names)) src = arial_chain;
    else if (in_list(norm, sans_names)) src = sans_chain;
    else if (in_list(norm, mono_names)) src = mono_chain;
    else if (in_list(norm, serif_names)) src = serif_chain;
    else {
        if (norm[0] && n < max) ch[n++] = norm;
        if (strstr(norm, "mono") || strstr(norm, "console") || strstr(norm, "courier")) src = mono_chain;
        else if (strstr(norm, "serif") && !strstr(norm, "sans")) src = serif_chain;
        else src = sans_chain;
    }
    for (int i = 0; src[i] && n < max; i++) ch[n++] = src[i];
    return n;
}

/* ---------------- glyph lookup and outline loading ---------------- */
static uint32_t cmap_lookup(const face *f, uint32_t cp) {
    size_t sub = f->cmap_sub;
    if (f->cmap_fmt == 12) {
        uint32_t ng = rd32(f, sub + 12);
        for (uint32_t i = 0; i < ng && i < 65536; i++) {
            size_t r = sub + 16 + 12 * (size_t)i;
            uint32_t s = rd32(f, r), e = rd32(f, r + 4);
            if (cp >= s && cp <= e) return rd32(f, r + 8) + (cp - s);
            if (cp < s) break; /* groups are sorted by start code */
        }
        return 0;
    }
    if (cp > 0xffff) return 0;
    uint32_t segx2 = rd16(f, sub + 6);
    size_t endc = sub + 14, startc = endc + segx2 + 2, delta = startc + segx2, ro = delta + segx2;
    for (uint32_t i = 0; i < segx2 / 2; i++) {
        uint32_t e = rd16(f, endc + 2 * i);
        if (cp > e) continue;
        uint32_t s = rd16(f, startc + 2 * i);
        if (cp < s) return 0;
        uint32_t idd = rd16(f, delta + 2 * i), rov = rd16(f, ro + 2 * i);
        uint32_t g;
        if (!rov) g = (cp + idd) & 0xffff;
        else {
            g = rd16(f, ro + 2 * i + rov + 2 * (cp - s));
            if (g) g = (g + idd) & 0xffff;
        }
        return g;
    }
    return 0;
}

static uint32_t hmtx_advance(const face *f, uint32_t gid) {
    if (f->nhm <= 0) return 0;
    uint32_t idx2 = gid < (uint32_t)f->nhm ? gid : (uint32_t)f->nhm - 1;
    return rd16(f, f->hmtx + 4 * (size_t)idx2);
}

typedef struct {
    double x, y;
    int on;
} opt;
typedef struct {
    opt *p;
    int np, cap;
    int *ends;
    int ne, cape;
} outline;

static int ol_pt(outline *o, double x, double y, int on) {
    if (o->np == o->cap) {
        int c = o->cap ? o->cap * 2 : 256;
        opt *n = realloc(o->p, sizeof *n * (size_t)c);
        if (!n) return 0;
        o->p = n;
        o->cap = c;
    }
    o->p[o->np].x = x;
    o->p[o->np].y = y;
    o->p[o->np].on = on;
    o->np++;
    return 1;
}
static int ol_end(outline *o, int idx) {
    if (o->ne == o->cape) {
        int c = o->cape ? o->cape * 2 : 32;
        int *n = realloc(o->ends, sizeof *n * (size_t)c);
        if (!n) return 0;
        o->ends = n;
        o->cape = c;
    }
    o->ends[o->ne++] = idx;
    return 1;
}

static void glyph_span(const face *f, uint32_t gid, size_t *start, size_t *end) {
    *start = *end = 0;
    if (gid >= (uint32_t)f->nglyphs) return;
    size_t s, e;
    if (f->loca_long) {
        s = rd32(f, f->loca + 4 * (size_t)gid);
        e = rd32(f, f->loca + 4 * (size_t)gid + 4);
    } else {
        s = 2 * (size_t)rd16(f, f->loca + 2 * (size_t)gid);
        e = 2 * (size_t)rd16(f, f->loca + 2 * (size_t)gid + 2);
    }
    if (e <= s) return;
    *start = f->glyf + s;
    *end = f->glyf + e;
}

/* m = {a,b,c,d,e,f}: X = a*x + c*y + e, Y = b*x + d*y + f (font units) */
static void load_glyph_units(const face *f, uint32_t gid, const double m[6], outline *o, int depth);

static void load_simple(const face *f, size_t g, int nc, const double m[6], outline *o) {
    int *ends = malloc(sizeof(int) * (size_t)nc);
    if (!ends) return;
    int prev = -1;
    for (int c = 0; c < nc; c++) {
        ends[c] = (int)rd16(f, g + 10 + 2 * (size_t)c);
        if (ends[c] <= prev) {
            free(ends);
            return;
        }
        prev = ends[c];
    }
    int npts = ends[nc - 1] + 1;
    if (npts > 65000) {
        free(ends);
        return;
    }
    size_t p = g + 10 + 2 * (size_t)nc;
    p += 2 + rd16(f, p);
    uint8_t *fl = malloc((size_t)npts);
    int *xv = malloc(sizeof(int) * (size_t)npts), *yv = malloc(sizeof(int) * (size_t)npts);
    if (!fl || !xv || !yv) goto done;
    int i = 0;
    while (i < npts) {
        uint8_t fv = (uint8_t)rdb(f, p++);
        fl[i++] = fv;
        if (fv & 8) {
            int rep = (int)rdb(f, p++);
            while (rep-- && i < npts) fl[i++] = fv;
        }
    }
    int x = 0;
    for (i = 0; i < npts; i++) {
        uint8_t fv = fl[i];
        if (fv & 2) {
            int dx = (int)rdb(f, p++);
            x += (fv & 16) ? dx : -dx;
        } else if (!(fv & 16)) {
            x += rds16(f, p);
            p += 2;
        }
        xv[i] = x;
    }
    int y = 0;
    for (i = 0; i < npts; i++) {
        uint8_t fv = fl[i];
        if (fv & 4) {
            int dy = (int)rdb(f, p++);
            y += (fv & 32) ? dy : -dy;
        } else if (!(fv & 32)) {
            y += rds16(f, p);
            p += 2;
        }
        yv[i] = y;
    }
    int base = o->np;
    for (i = 0; i < npts; i++) {
        double px = xv[i], py = yv[i];
        ol_pt(o, m[0] * px + m[2] * py + m[4], m[1] * px + m[3] * py + m[5], fl[i] & 1);
    }
    for (int c = 0; c < nc; c++) ol_end(o, base + ends[c]);
done:
    free(fl);
    free(xv);
    free(yv);
    free(ends);
}

static double f2dot14(const face *f, size_t o) { return rds16(f, o) / 16384.0; }

static void load_composite(const face *f, size_t g, const double m[6], outline *o, int depth) {
    size_t p = g + 10;
    for (int guard = 0; guard < 256; guard++) {
        unsigned flags = rd16(f, p);
        uint32_t cgid = rd16(f, p + 2);
        p += 4;
        double dx, dy;
        if (flags & 0x1) {
            dx = rds16(f, p);
            dy = rds16(f, p + 2);
            p += 4;
        } else {
            dx = (int8_t)rdb(f, p);
            dy = (int8_t)rdb(f, p + 1);
            p += 2;
        }
        double a = 1, b = 0, c = 0, d = 1;
        if (flags & 0x8) {
            a = d = f2dot14(f, p);
            p += 2;
        } else if (flags & 0x40) {
            a = f2dot14(f, p);
            d = f2dot14(f, p + 2);
            p += 4;
        } else if (flags & 0x80) {
            a = f2dot14(f, p);
            b = f2dot14(f, p + 2);
            c = f2dot14(f, p + 4);
            d = f2dot14(f, p + 6);
            p += 8;
        }
        if (flags & 0x2) { /* ARGS_ARE_XY_VALUES; point matching is not supported */
            double cm[6] = {
                m[0] * a + m[2] * b,
                m[1] * a + m[3] * b,
                m[0] * c + m[2] * d,
                m[1] * c + m[3] * d,
                m[0] * dx + m[2] * dy + m[4],
                m[1] * dx + m[3] * dy + m[5],
            };
            load_glyph_units(f, cgid, cm, o, depth + 1);
        }
        if (!(flags & 0x20)) break;
    }
}

static void load_glyph_units(const face *f, uint32_t gid, const double m[6], outline *o, int depth) {
    if (depth > 8) return;
    size_t s, e;
    glyph_span(f, gid, &s, &e);
    if (!s || e <= s) return;
    int nc = rds16(f, s);
    if (nc > 0) load_simple(f, s, nc, m, o);
    else if (nc < 0) load_composite(f, s, m, o, depth);
}

static const uint8_t no_alpha[1] = {0};

/* ---------------- flattening and coverage accumulation ---------------- */
typedef struct {
    double x0, y0, x1, y1;
} seg;
typedef struct {
    seg *s;
    int n, cap;
} segs;

static int seg_add(segs *S, double x0, double y0, double x1, double y1) {
    if (S->n == S->cap) {
        int c = S->cap ? S->cap * 2 : 256;
        seg *n = realloc(S->s, sizeof *n * (size_t)c);
        if (!n) return 0;
        S->s = n;
        S->cap = c;
    }
    S->s[S->n].x0 = x0;
    S->s[S->n].y0 = y0;
    S->s[S->n].x1 = x1;
    S->s[S->n].y1 = y1;
    S->n++;
    return 1;
}

typedef struct {
    double x, y;
    int on;
} ppt;

/* One closed contour (pts[0..n)) into line segments in pixel space (y up).  Implied on-curve
 * points between consecutive off-curve points; quadratic arcs become short lines. */
static void flatten_contour(const opt *pts, int n, double sx, double sy, double shear, segs *S) {
    if (n < 1) return;
    ppt *ex = malloc(sizeof *ex * (size_t)n * 2 + sizeof *ex);
    if (!ex) return;
    int m = 0;
    for (int i = 0; i < n; i++) {
        const opt *a = &pts[i], *b = &pts[(i + 1) % n];
        ex[m].x = a->x * sx + shear * a->y * sy;
        ex[m].y = a->y * sy;
        ex[m].on = a->on;
        m++;
        if (!a->on && !b->on) {
            double mx = (a->x + b->x) / 2, my = (a->y + b->y) / 2;
            ex[m].x = mx * sx + shear * my * sy;
            ex[m].y = my * sy;
            ex[m].on = 1;
            m++;
        }
    }
    int k = 0;
    while (k < m && !ex[k].on) k++;
    if (k >= m) {
        free(ex);
        return;
    }
    ppt start = ex[k];
    ppt cur = start;
    for (int j = 1; j <= m;) {
        int idx = (k + j) % m;
        if (ex[idx].on) {
            seg_add(S, cur.x, cur.y, ex[idx].x, ex[idx].y);
            cur = ex[idx];
            j++;
        } else {
            ppt c = ex[idx], p1 = ex[(k + j + 1) % m];
            double ddx = cur.x - 2 * c.x + p1.x, ddy = cur.y - 2 * c.y + p1.y;
            int steps = (int)dceil(dsqrt(dabs(ddx) + dabs(ddy)) * 1.8);
            if (steps < 1) steps = 1;
            if (steps > 48) steps = 48;
            ppt prev = cur;
            for (int t = 1; t <= steps; t++) {
                double u = (double)t / steps, v = 1 - u;
                double qx = v * v * cur.x + 2 * v * u * c.x + u * u * p1.x;
                double qy = v * v * cur.y + 2 * v * u * c.y + u * u * p1.y;
                seg_add(S, prev.x, prev.y, qx, qy);
                prev.x = qx;
                prev.y = qy;
            }
            cur = p1;
            j += 2;
        }
    }
    free(ex);
}

/* font-rs style signed-area accumulation.  Row stride is w+2 so that the spill of a segment
 * at its right edge never reaches the next row. */
static void acc_line(double *acc, int stride, int h, double p0x, double p0y, double p1x, double p1y) {
    if (p0y == p1y) return;
    double dir = 1;
    if (p0y > p1y) {
        double t;
        t = p0x; p0x = p1x; p1x = t;
        t = p0y; p0y = p1y; p1y = t;
        dir = -1;
    }
    double dxdy = (p1x - p0x) / (p1y - p0y);
    double x = p0x;
    int y0 = (int)dfloor(p0y);
    if (y0 < 0) {
        x -= dxdy * p0y;
        y0 = 0;
    }
    int ylast = (int)dceil(p1y);
    if (ylast > h) ylast = h;
    for (int y = y0; y < ylast; y++) {
        int ls = y * stride;
        double ya = p0y > y ? p0y : y;
        double yb = p1y < y + 1 ? p1y : y + 1;
        double dy = yb - ya;
        double xnext = x + dxdy * dy;
        double d = dy * dir;
        double x0 = x < xnext ? x : xnext, x1 = x < xnext ? xnext : x;
        int x0i = (int)dfloor(x0), x1i = (int)dceil(x1);
        if (x0i < 0) x0i = 0;
        if (x1i > stride - 2) x1i = stride - 2;
        if (x1i <= x0i + 1) {
            double xmf = 0.5 * (x + xnext) - x0i;
            acc[ls + x0i] += d - d * xmf;
            acc[ls + x0i + 1] += d * xmf;
        } else {
            double s = 1.0 / (x1 - x0);
            double x0f = x0 - x0i;
            double a0 = 0.5 * s * (1 - x0f) * (1 - x0f);
            double x1f = x1 - x1i + 1;
            double am = 0.5 * s * x1f * x1f;
            acc[ls + x0i] += d * a0;
            if (x1i == x0i + 2) {
                acc[ls + x0i + 1] += d * (1 - a0 - am);
            } else {
                double a1 = s * (1.5 - x0f);
                acc[ls + x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) acc[ls + xi] += d * s;
                double a2 = a1 + (x1i - x0i - 3) * s;
                acc[ls + x1i - 1] += d * (1 - a2 - am);
            }
            acc[ls + x1i] += d * am;
        }
        x = xnext;
    }
}

/* Rasterizes one glyph outline into an 8-bit coverage bitmap. */
static void render_outline(const outline *o, double sx, double sy, double shear, int bold, aw_glyph *g) {
    segs S = {0};
    int s = 0;
    for (int c = 0; c < o->ne; c++) {
        int e = o->ends[c];
        flatten_contour(o->p + s, e - s + 1, sx, sy, shear, &S);
        s = e + 1;
    }
    g->w = g->h = g->left = g->top = 0;
    g->alpha = no_alpha;
    if (!S.n) {
        free(S.s);
        return;
    }
    double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
    for (int i = 0; i < S.n; i++) {
        double xs[2] = {S.s[i].x0, S.s[i].x1}, ys[2] = {S.s[i].y0, S.s[i].y1};
        for (int k = 0; k < 2; k++) {
            if (xs[k] < xmin) xmin = xs[k];
            if (xs[k] > xmax) xmax = xs[k];
            if (ys[k] < ymin) ymin = ys[k];
            if (ys[k] > ymax) ymax = ys[k];
        }
    }
    int gx = (int)dfloor(xmin), gtop = (int)dceil(ymax), gbot = (int)dfloor(ymin);
    int w = (int)dceil(xmax) - gx, h = gtop - gbot;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        free(S.s);
        return;
    }
    int stride = w + 2;
    double *acc = calloc((size_t)stride * (size_t)h, sizeof *acc);
    uint8_t *alpha = malloc((size_t)w * (size_t)h);
    if (!acc || !alpha) {
        free(acc);
        free(alpha);
        free(S.s);
        return;
    }
    for (int i = 0; i < S.n; i++)
        acc_line(acc, stride, h, S.s[i].x0 - gx, gtop - S.s[i].y0, S.s[i].x1 - gx, gtop - S.s[i].y1);
    for (int j = 0; j < h; j++) {
        double sum = 0;
        for (int i = 0; i < w; i++) {
            sum += acc[(size_t)j * stride + i];
            double v = dabs(sum);
            if (v > 1) v = 1;
            alpha[(size_t)j * w + i] = (uint8_t)(v * 255 + 0.5);
        }
    }
    free(acc);
    free(S.s);
    if (bold) { /* synthetic emboldening: one pixel smear to the right */
        uint8_t *b2 = malloc((size_t)(w + 1) * (size_t)h);
        if (b2) {
            for (int j = 0; j < h; j++)
                for (int i = 0; i <= w; i++) {
                    int a = i < w ? alpha[(size_t)j * w + i] : 0;
                    int pr = i > 0 ? alpha[(size_t)j * w + i - 1] : 0;
                    int v = a + pr;
                    b2[(size_t)j * (w + 1) + i] = (uint8_t)(v > 255 ? 255 : v);
                }
            free(alpha);
            alpha = b2;
            w++;
        }
    }
    g->w = w;
    g->h = h;
    g->left = gx;
    g->top = gtop;
    g->alpha = alpha;
}

/* ---------------- built-in 5x7 bitmap font (ASCII 32..126) ---------------- */
static const uint8_t builtin5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
};

static int builtin_k(int lfheight) {
    int cell = lfheight > 0 ? lfheight : lfheight < 0 ? -lfheight * 5 / 4 : 16;
    int k = (cell + 4) / 8;
    return k < 1 ? 1 : k;
}

/* ---------------- font objects ---------------- */
struct gent {
    uint32_t cp;
    aw_glyph g;
    struct gent *next;
};

struct aw_font {
    aw_font_metrics m;
    face *f;                 /* primary face, NULL = built-in bitmap */
    face *fb[MAX_FALLBACK];  /* faces tried for codepoints missing from f */
    int nfb;
    double ppem;
    int synth_bold, synth_italic;
    int bi_k;
    struct gent *bucket[GLYPH_BUCKETS];
};

static aw_font *fonts[MAX_FONTS];
static int nfonts;
static const aw_glyph empty_glyph = {0, 0, 0, 0, 0, no_alpha};

static void box_glyph(aw_font *f, aw_glyph *g) {
    int top = iround(f->ppem * 0.75);
    if (top < 2) top = 2;
    int w = iround(f->ppem * 0.5);
    if (w < 2) w = 2;
    uint8_t *a = calloc((size_t)w * (size_t)top, 1);
    if (!a) {
        *g = empty_glyph;
        return;
    }
    for (int j = 0; j < top; j++)
        for (int i = 0; i < w; i++)
            if (i == 0 || i == w - 1 || j == 0 || j == top - 1) a[(size_t)j * w + i] = 255;
    g->w = w;
    g->h = top;
    g->left = 1;
    g->top = top;
    g->advance = w + 2;
    g->alpha = a;
}

static void builtin_glyph(aw_font *f, uint32_t cp, aw_glyph *g) {
    int k = f->bi_k;
    if (cp < 32 || cp > 126) {
        g->advance = 6 * k;
        box_glyph(f, g);
        return;
    }
    int w = 5 * k, h = 7 * k;
    uint8_t *a = calloc((size_t)w * (size_t)h, 1);
    if (!a) {
        *g = empty_glyph;
        return;
    }
    const uint8_t *cols = builtin5x7[cp - 32];
    for (int c = 0; c < 5; c++)
        for (int r = 0; r < 7; r++)
            if ((cols[c] >> r) & 1)
                for (int ky = 0; ky < k; ky++)
                    for (int kx = 0; kx < k; kx++)
                        a[(size_t)(r * k + ky) * w + c * k + kx] = 255;
    g->w = w;
    g->h = h;
    g->left = 0;
    g->top = h;
    g->advance = 6 * k;
    g->alpha = a;
}

static void glyph_make(aw_font *f, uint32_t cp, aw_glyph *g) {
    if (!f->f) {
        builtin_glyph(f, cp, g);
        return;
    }
    face *use = f->f;
    uint32_t gid = cmap_lookup(use, cp);
    for (int i = 0; !gid && i < f->nfb; i++) {
        uint32_t g2 = cmap_lookup(f->fb[i], cp);
        if (g2) {
            use = f->fb[i];
            gid = g2;
        }
    }
    if (!gid) {
        box_glyph(f, g);
        return;
    }
    int primary = use == f->f;
    double scale = f->ppem / use->upem;
    outline o = {0};
    double ident[6] = {1, 0, 0, 1, 0, 0};
    load_glyph_units(use, gid, ident, &o, 0);
    /* synthetic italic: about 11 degrees (x += 0.2 * y); only for the primary face */
    render_outline(&o, scale, scale, primary && f->synth_italic ? 0.2 : 0.0, primary && f->synth_bold, g);
    free(o.p);
    free(o.ends);
    g->advance = iround(hmtx_advance(use, gid) * scale);
}

const aw_font_metrics *font_metrics(aw_font *f) { return &f->m; }

const aw_glyph *font_glyph(aw_font *f, uint32_t cp) {
    unsigned b = cp % GLYPH_BUCKETS;
    for (struct gent *e = f->bucket[b]; e; e = e->next)
        if (e->cp == cp) return &e->g;
    struct gent *e = calloc(1, sizeof *e);
    /* calloc failure: return a valid empty glyph (not cached) */
    if (!e) return &empty_glyph;
    e->cp = cp;
    glyph_make(f, cp, &e->g);
    e->next = f->bucket[b];
    f->bucket[b] = e;
    return &e->g;
}

int font_advance(aw_font *f, uint32_t codepoint) { return font_glyph(f, codepoint)->advance; }

aw_font *font_open(const char *facename, int lfheight, int width, int weight, int italic, int underline,
                   int strikeout) {
    if (!facename) facename = "";
    if (weight <= 0) weight = 400;
    italic = italic != 0;
    underline = underline != 0;
    strikeout = strikeout != 0;
    /* cache key: face, lfheight, width (ignored by the rasterizer, kept in the key), weight, flags */
    static int last_lfh[MAX_FONTS], last_width[MAX_FONTS];
    for (int i = 0; i < nfonts; i++)
        if (fonts[i]->m.weight == weight && fonts[i]->m.italic == italic && fonts[i]->m.underline == underline &&
            fonts[i]->m.strikeout == strikeout && !strcmp(fonts[i]->m.face, facename) && last_lfh[i] == lfheight &&
            last_width[i] == width)
            return fonts[i];

    build_index();
    char norm[96];
    norm_name(facename, norm, sizeof norm);
    const char *ch[16];
    int nch = build_chain(norm, ch, 16);
    int want_bold = weight >= 600;
    const char *const *style = want_bold && italic ? suf_bolditalic : want_bold ? suf_bold : italic ? suf_italic : suf_regular;

    aw_font *f = calloc(1, sizeof *f);
    if (!f) return fonts[0] ? fonts[0] : NULL;
    for (int i = 0; i < nch && !f->f; i++) {
        const char *p = find_file(ch[i], style);
        if (p) f->f = face_load(p);
    }
    for (int i = 0; i < nch && !f->f; i++) {
        const char *p = find_file(ch[i], suf_regular);
        if (p) f->f = face_load(p);
        if (f->f) {
            f->synth_bold = want_bold;
            f->synth_italic = italic;
        }
    }
    for (int i = 0; i < nch && f->nfb < MAX_FALLBACK; i++) {
        const char *p = find_file(ch[i], suf_regular);
        face *g = p ? face_load(p) : NULL;
        if (g && g != f->f) f->fb[f->nfb++] = g;
    }

    strncpy(f->m.face, facename, sizeof f->m.face - 1);
    f->m.weight = weight;
    f->m.italic = italic;
    f->m.underline = underline;
    f->m.strikeout = strikeout;
    if (f->f) {
        face *pf = f->f;
        double denom = (double)(pf->win_asc + pf->win_desc);
        if (denom <= 0) denom = pf->upem;
        double ppem;
        if (lfheight < 0) ppem = -lfheight;
        else if (lfheight > 0) ppem = lfheight * (double)pf->upem / denom;
        else ppem = 16.0 * pf->upem / denom;
        f->ppem = ppem;
        double s = ppem / pf->upem;
        int asc = (int)dceil(pf->win_asc * s), desc = (int)dceil(pf->win_desc * s);
        f->m.ascent = asc;
        f->m.descent = desc;
        f->m.height = asc + desc;
        int leading = f->m.height - iround(ppem);
        f->m.internal_leading = leading < 0 ? 0 : leading;
        f->m.external_leading = 0;
        f->m.ave_char_width = pf->avg_w > 0 ? iround(pf->avg_w * s) : 0;
        f->m.max_char_width = iround(pf->max_adv * s);
        f->m.fixed_pitch = pf->fixed;
        if (!f->m.ave_char_width) f->m.ave_char_width = iround(hmtx_advance(pf, cmap_lookup(pf, 'x')) * s);
    } else {
        f->bi_k = builtin_k(lfheight);
        f->ppem = 8.0 * f->bi_k;
        f->m.ascent = 7 * f->bi_k;
        f->m.descent = f->bi_k;
        f->m.height = 8 * f->bi_k;
        f->m.internal_leading = 0;
        f->m.external_leading = 0;
        f->m.ave_char_width = 6 * f->bi_k;
        f->m.max_char_width = 6 * f->bi_k;
        f->m.fixed_pitch = 0;
    }
    if (nfonts < MAX_FONTS) {
        last_lfh[nfonts] = lfheight;
        last_width[nfonts] = width;
        fonts[nfonts++] = f;
        return f;
    }
    return fonts[0] ? fonts[0] : f;
}
