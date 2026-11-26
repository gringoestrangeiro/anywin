/* user32 core - icons and cursors: procedural system icons, RT_GROUP_ICON/RT_ICON and .ico/.cur
 * decoding (BMP DIB entries), LoadIcon/LoadCursor/LoadImage, DrawIconEx (S5).
 * PNG-compressed icon entries are not decoded yet: such entries are skipped. */
#include "user32_int.h"

#define IDI_FIRST 32512
#define IDI_LAST 32518
#define IDC_FIRST 32512
#define IDC_LAST 32650
#define LR_LOADFROMFILE_F 0x0010
#define LR_SHARED_F 0x8000
#define IMAGE_BITMAP_T 0
#define IMAGE_ICON_T 1
#define IMAGE_CURSOR_T 2
#define DI_NORMAL_F 3
#define DI_IMAGE_F 2

static aw_icon *system_cache[64];
static aw_icon *cursor_cache[64];

/* ------------------------------------------------------------------ objects */
aw_icon *aw_icon_alloc(int w, int h, int cursor)
{
    aw_icon *ic = calloc(1, sizeof *ic);
    if (!ic)
        return NULL;
    ic->magic = AW_ICON_MAGIC;
    ic->w = w;
    ic->h = h;
    ic->is_cursor = cursor;
    if (w > 0 && h > 0) {
        ic->argb = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
        if (!ic->argb) {
            free(ic);
            return NULL;
        }
    }
    return ic;
}

void aw_icon_free(aw_icon *ic)
{
    if (!ic || ic->shared)
        return;
    free(ic->argb);
    memset(ic, 0, sizeof *ic);
    free(ic);
}

aw_icon *aw_icon_from_handle(HICON h)
{
    if (!h)
        return NULL;
    aw_icon *ic = (aw_icon *)h;
    return ic->magic == AW_ICON_MAGIC ? ic : NULL;
}

/* ------------------------------------------------------------------ procedural system icons */
static void put(aw_icon *ic, int x, int y, uint32_t argb)
{
    if (x >= 0 && y >= 0 && x < ic->w && y < ic->h)
        ic->argb[y * ic->w + x] = argb;
}

/* fills a disc of colour c (opaque) with an inner glyph colour g drawn by the caller-chosen rule */
static void draw_disc(aw_icon *ic, uint32_t c, int mode, uint32_t g)
{
    int n = ic->w;
    double mid = (n - 1) / 2.0, rad = n / 2.0 - 1.0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            double dx = x - mid, dy = y - mid;
            if (dx * dx + dy * dy > rad * rad)
                continue;
            uint32_t p = 0xFF000000u | c;
            int inner = (x >= n / 2 - 2 && x <= n / 2 + 1);
            if (mode == 1 && inner && (y < n / 2 - 6 || y > n / 2 + 6))
                p = 0xFF000000u | g; /* exclamation / info stem */
            if (mode == 2 && inner && y > n / 2 + 6 && y < n / 2 + 9)
                p = 0xFF000000u | g;
            if (mode == 3 && (abs(x - y) <= 1 || abs(x + y - (n - 1)) <= 1) && x > n / 4 && x < 3 * n / 4 &&
                y > n / 4 && y < 3 * n / 4)
                p = 0xFF000000u | g; /* error cross */
            put(ic, x, y, p);
        }
}

aw_icon *aw_icon_system(int id, int cursor)
{
    if (cursor) {
        if (id < IDC_FIRST || id > IDC_LAST)
            return NULL;
        int slot = id - IDC_FIRST;
        if (slot >= 64) return NULL;
        if (!cursor_cache[slot]) {
            aw_icon *ic = aw_icon_alloc(0, 0, 1);
            if (!ic) return NULL;
            ic->idc = id;
            ic->shared = 1;
            cursor_cache[slot] = ic;
        }
        return cursor_cache[slot];
    }
    if (id < IDI_FIRST || id > IDI_LAST)
        return NULL;
    int slot = id - IDI_FIRST;
    if (system_cache[slot])
        return system_cache[slot];
    aw_icon *ic = aw_icon_alloc(32, 32, 0);
    if (!ic)
        return NULL;
    ic->shared = 1;
    ic->hotx = 0;
    ic->hoty = 0;
    switch (id) {
    case 32513: draw_disc(ic, 0xD32F2F, 3, 0xFFFFFF); break;   /* IDI_ERROR */
    case 32514: draw_disc(ic, 0x1976D2, 2, 0xFFFFFF); break;   /* IDI_QUESTION */
    case 32515: draw_disc(ic, 0xF9A825, 1, 0x000000); break;   /* IDI_WARNING */
    case 32516: draw_disc(ic, 0x1976D2, 1, 0xFFFFFF); break;   /* IDI_INFORMATION */
    case 32517:                                                /* IDI_WINLOGO */
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                uint32_t c = x < 16 ? (y < 16 ? 0xF25022 : 0x00A4EF) : (y < 16 ? 0x7FBA00 : 0xFFB900);
                put(ic, x, y, 0xFF000000u | c);
            }
        break;
    case 32518:                                                /* IDI_SHIELD */
        for (int y = 4; y < 28; y++)
            for (int x = 6; x < 26; x++)
                put(ic, x, y, 0xFF5B7DB1u);
        break;
    default:                                                   /* IDI_APPLICATION */
        for (int y = 2; y < 30; y++)
            for (int x = 2; x < 30; x++)
                put(ic, x, y, (y < 8 ? 0xFF1976D2u : 0xFFFFFFFFu));
        for (int x = 0; x < 32; x++) { put(ic, x, 1, 0xFF404040u); put(ic, x, 30, 0xFF404040u); }
        for (int y = 0; y < 32; y++) { put(ic, 1, y, 0xFF404040u); put(ic, 30, y, 0xFF404040u); }
        break;
    }
    system_cache[slot] = ic;
    return ic;
}

/* ------------------------------------------------------------------ DIB decoding */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* BITMAPINFOHEADER + pixels (XOR then AND mask), as stored in RT_ICON / RT_CURSOR and .ico files. */
aw_icon *aw_icon_from_dib(const uint8_t *d, uint32_t size, int cursor, int wantw, int wanth, int from_group)
{
    (void)wantw; (void)wanth; (void)from_group;
    if (!d || size < 40 || rd32(d) < 40)
        return NULL;
    if (d[0] == 0x89 && d[1] == 'P') /* PNG: not decoded yet */
        return NULL;
    int32_t bw = (int32_t)rd32(d + 4), bh = (int32_t)rd32(d + 8);
    int bpp = rd16(d + 14), comp = (int)rd32(d + 16);
    uint32_t hdr = rd32(d);
    if (bw <= 0 || bw > 512 || comp != 0)
        return NULL;
    int h = bh / 2;
    if (h <= 0 || h > 512)
        return NULL;
    int colors = 0;
    if (bpp <= 8) {
        colors = (int)rd32(d + 32);
        if (colors == 0) colors = 1 << bpp;
    }
    const uint8_t *pal = d + hdr;
    const uint8_t *px = pal + (size_t)colors * 4;
    int xstride = ((bw * bpp + 31) / 32) * 4;
    int mstride = ((bw + 31) / 32) * 4;
    size_t need = (size_t)hdr + (size_t)colors * 4 + (size_t)xstride * h + (size_t)mstride * h;
    if (need > size) {
        /* no AND mask: allowed for 32bpp */
        if (bpp != 32 || (size_t)hdr + (size_t)colors * 4 + (size_t)xstride * h > size)
            return NULL;
    }
    aw_icon *ic = aw_icon_alloc(bw, h, cursor);
    if (!ic) return NULL;
    const uint8_t *mask = px + (size_t)xstride * h;
    int have_mask = need <= size;
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)(h - 1 - y) * xstride; /* bottom-up */
        const uint8_t *mrow = mask + (size_t)(h - 1 - y) * mstride;
        for (int x = 0; x < bw; x++) {
            uint32_t b = 0, g = 0, r = 0, a = 0xFF;
            if (bpp == 32) {
                b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; a = row[x * 4 + 3];
                if (a) any_alpha = 1;
            } else if (bpp == 24) {
                b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2];
            } else if (bpp == 8 || bpp == 4 || bpp == 1) {
                int idx;
                if (bpp == 8) idx = row[x];
                else if (bpp == 4) idx = (row[x / 2] >> ((x & 1) ? 0 : 4)) & 0xF;
                else idx = (row[x / 8] >> (7 - (x & 7))) & 1;
                if (idx >= colors) idx = 0;
                b = pal[idx * 4]; g = pal[idx * 4 + 1]; r = pal[idx * 4 + 2];
            } else {
                aw_icon_free(ic);
                return NULL;
            }
            if (bpp < 32 && have_mask) {
                int bit = (mrow[x / 8] >> (7 - (x & 7))) & 1;
                a = bit ? 0 : 0xFF;
            }
            ic->argb[y * bw + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    if (bpp == 32 && !any_alpha) {
        /* 32bpp without alpha: the AND mask decides transparency */
        for (int y = 0; y < h && have_mask; y++) {
            const uint8_t *mrow = mask + (size_t)(h - 1 - y) * mstride;
            for (int x = 0; x < bw; x++) {
                int bit = (mrow[x / 8] >> (7 - (x & 7))) & 1;
                ic->argb[y * bw + x] = (ic->argb[y * bw + x] & 0x00FFFFFFu) | (bit ? 0 : 0xFF000000u);
            }
        }
        if (!have_mask)
            for (int i = 0; i < bw * h; i++) ic->argb[i] |= 0xFF000000u;
    }
    return ic;
}

/* ------------------------------------------------------------------ resources and files */
/* GRPICONDIR / GRPCURSORDIR: pick the entry closest to the wanted size (ties: deeper colour). */
aw_icon *aw_icon_from_resource(HINSTANCE hinst, const void *name, int cursor, int cx, int cy)
{
    (void)cy;
    uint32_t gsz = 0;
    const uint8_t *grp = aw_find_resource(hinst, cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON, name, &gsz);
    if (!grp || gsz < 6)
        return NULL;
    int count = rd16(grp + 4);
    if (count <= 0 || 6 + (size_t)count * 14 > gsz)
        return NULL;
    int want = cx > 0 ? cx : 32;
    int best = -1, best_d = 1 << 30, best_bpp = -1;
    for (int i = 0; i < count; i++) {
        const uint8_t *e = grp + 6 + i * 14;
        int w = cursor ? rd16(e) : (e[0] ? e[0] : 256); /* GRPCURSORDIR stores 16-bit sizes */
        int bpp = rd16(e + 6);
        int d = abs(w - want);
        if (d < best_d || (d == best_d && bpp > best_bpp)) {
            best = i;
            best_d = d;
            best_bpp = bpp;
        }
    }
    const uint8_t *e = grp + 6 + best * 14;
    uint16_t id = rd16(e + 12);
    uint32_t sz = 0;
    const uint8_t *data = aw_find_resource(hinst, cursor ? RT_CURSOR : RT_ICON, (const void *)(uintptr_t)id, &sz);
    if (!data || sz < 4)
        return NULL;
    if (cursor) {
        int hx = rd16(data), hy = rd16(data + 2);
        aw_icon *ic = aw_icon_from_dib(data + 4, sz - 4, 1, 0, 0, 1);
        if (ic) {
            ic->hotx = hx;
            ic->hoty = hy;
        }
        return ic;
    }
    return aw_icon_from_dib(data, sz, 0, 0, 0, 1);
}

aw_icon *aw_icon_from_file(const char *path, int cursor, int cx, int cy)
{
    (void)cy;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 22 || n > (64L << 20)) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    aw_icon *out = NULL;
    int count = rd16(buf + 4);
    if (rd16(buf) == 0 && count > 0 && 6 + (long)count * 16 <= n) {
        int want = cx > 0 ? cx : 32, best = -1, best_d = 1 << 30;
        for (int i = 0; i < count; i++) {
            const uint8_t *e = buf + 6 + i * 16;
            int w = e[0] ? e[0] : 256;
            int d = abs(w - want);
            if (d < best_d) { best = i; best_d = d; }
        }
        const uint8_t *e = buf + 6 + best * 16;
        uint32_t sz = rd32(e + 8), off = rd32(e + 12);
        if (off + sz <= (uint32_t)n) {
            out = aw_icon_from_dib(buf + off, sz, cursor, 0, 0, 0);
            if (out && cursor) {
                out->hotx = rd16(e + 4);
                out->hoty = rd16(e + 6);
            }
        }
    }
    free(buf);
    return out;
}

/* ------------------------------------------------------------------ LoadIcon / LoadCursor / LoadImage */
static int is_id(const void *p) { return IS_INTRESOURCE(p); }

static aw_icon *load_icon(HINSTANCE hi, const void *name, int cursor, int cx, int cy)
{
    if (!name)
        return NULL;
    if (is_id(name)) {
        uint32_t id = (uint32_t)(uintptr_t)name;
        if (!hi && (cursor ? (id >= IDC_FIRST && id <= IDC_LAST) : (id >= IDI_FIRST && id <= IDI_LAST)))
            return aw_icon_system((int)id, cursor);
    }
    return aw_icon_from_resource(hi, name, cursor, cx, cy);
}

API(user32, HICON, LoadIconA, (HINSTANCE hi, LPCSTR name))
{
    return (HICON)load_icon(hi, name, 0, 32, 32);
}
API(user32, HICON, LoadIconW, (HINSTANCE hi, LPCWSTR name))
{
    return (HICON)load_icon(hi, name, 0, 32, 32);
}
API(user32, HCURSOR, LoadCursorA, (HINSTANCE hi, LPCSTR name))
{
    return (HCURSOR)load_icon(hi, name, 1, 32, 32);
}
API(user32, HCURSOR, LoadCursorW, (HINSTANCE hi, LPCWSTR name))
{
    return (HCURSOR)load_icon(hi, name, 1, 32, 32);
}

static HANDLE load_image(HINSTANCE hi, const void *name, UINT type, int cx, int cy, UINT flags)
{
    if (type == IMAGE_BITMAP_T)
        return NULL; /* bitmaps belong to GDI */
    int cursor = type == IMAGE_CURSOR_T;
    if (cx == 0 || (flags & 0x40 /* LR_DEFAULTSIZE */)) {
        cx = aw_system_metric(cursor ? 13 : 11);
        cy = aw_system_metric(cursor ? 14 : 12);
    }
    aw_icon *ic;
    if (flags & LR_LOADFROMFILE_F) {
        ic = aw_icon_from_file((const char *)name, cursor, cx, cy);
    } else {
        ic = load_icon(hi, name, cursor, cx, cy);
    }
    if (ic && (flags & LR_SHARED_F))
        ic->shared = 1;
    return ic;
}

API(user32, HANDLE, LoadImageA, (HINSTANCE hi, LPCSTR name, UINT type, int cx, int cy, UINT flags))
{
    return load_image(hi, name, type, cx, cy, flags);
}
API(user32, HANDLE, LoadImageW, (HINSTANCE hi, LPCWSTR name, UINT type, int cx, int cy, UINT flags))
{
    if (flags & LR_LOADFROMFILE_F) {
        if (!name || is_id(name))
            return NULL;
        char *u = aw_w2u(name, -1);
        HANDLE r = load_image(hi, u, type, cx, cy, flags);
        free(u);
        return r;
    }
    return load_image(hi, name, type, cx, cy, flags);
}

API(user32, BOOL, DestroyIcon, (HICON h))
{
    aw_icon *ic = aw_icon_from_handle(h);
    if (!ic) return FALSE;
    aw_icon_free(ic);
    return TRUE;
}
API(user32, BOOL, DestroyCursor, (HCURSOR h))
{
    aw_icon *ic = aw_icon_from_handle(h);
    if (!ic) return FALSE;
    aw_icon_free(ic);
    return TRUE;
}

API(user32, HICON, CopyIcon, (HICON h))
{
    aw_icon *src = aw_icon_from_handle(h);
    if (!src) return NULL;
    aw_icon *ic = aw_icon_alloc(src->w, src->h, src->is_cursor);
    if (!ic) return NULL;
    if (src->argb && ic->argb)
        memcpy(ic->argb, src->argb, (size_t)src->w * (size_t)src->h * sizeof(uint32_t));
    ic->hotx = src->hotx;
    ic->hoty = src->hoty;
    ic->idc = src->idc;
    return (HICON)ic;
}

API(user32, BOOL, DrawIconEx, (HDC hdc, int x, int y, HICON h, int cxw, int cyh, UINT step, HBRUSH br, UINT flags))
{
    (void)step; (void)br;
    aw_icon *ic = aw_icon_from_handle(h);
    if (!ic) return FALSE;
    if (!(flags & (DI_NORMAL_F | DI_IMAGE_F)) || !ic->argb)
        return TRUE;
    int w = cxw > 0 ? cxw : ic->w;
    int hh = cyh > 0 ? cyh : ic->h;
    gdi_draw_argb(hdc, x, y, w, hh, ic->argb, ic->w, ic->h);
    return TRUE;
}

API(user32, BOOL, DrawIcon, (HDC hdc, int x, int y, HICON h))
{
    return __aw_user32_DrawIconEx(hdc, x, y, h, 0, 0, 0, NULL, DI_NORMAL_F);
}
