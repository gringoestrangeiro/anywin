/* anywin GDI misc (S-GDI2): bitmaps from bits, DIB readback, object queries, device capabilities,
 * single pixels, mapping mode and palettes.
 *
 * The display is 32bpp TrueColor, so palettes are bookkeeping only: they hold entries, are
 * selected per DC and never change the device colours (RealizePalette reports 0 changes). */
#include "gdi_int.h"
#include "user32_int.h"
#include "aw_api.h"

#define AW_ERR_INVALID_PARAMETER 87
#define AW_CLR_INVALID 0xFFFFFFFFu

/* device-caps indices (wingdi.h) */
enum {
    AW_DC_DRIVERVERSION = 0, AW_DC_TECHNOLOGY = 2, AW_DC_HORZSIZE = 4, AW_DC_VERTSIZE = 6,
    AW_DC_HORZRES = 8, AW_DC_VERTRES = 10, AW_DC_BITSPIXEL = 12, AW_DC_PLANES = 14,
    AW_DC_NUMBRUSHES = 16, AW_DC_NUMPENS = 18, AW_DC_NUMMARKERS = 20, AW_DC_NUMFONTS = 22,
    AW_DC_NUMCOLORS = 24, AW_DC_PDEVICESIZE = 26, AW_DC_CURVECAPS = 28, AW_DC_LINECAPS = 30,
    AW_DC_POLYGONALCAPS = 32, AW_DC_TEXTCAPS = 34, AW_DC_CLIPCAPS = 36, AW_DC_RASTERCAPS = 38,
    AW_DC_ASPECTX = 40, AW_DC_ASPECTY = 42, AW_DC_ASPECTXY = 44, AW_DC_LOGPIXELSX = 88,
    AW_DC_LOGPIXELSY = 90, AW_DC_SIZEPALETTE = 104, AW_DC_NUMRESERVED = 106, AW_DC_COLORRES = 108,
    AW_DC_PHYSICALWIDTH = 110, AW_DC_PHYSICALHEIGHT = 111, AW_DC_PHYSICALOFFSETX = 112,
    AW_DC_PHYSICALOFFSETY = 113, AW_DC_SCALINGFACTORX = 114, AW_DC_SCALINGFACTORY = 115,
    AW_DC_VREFRESH = 116, AW_DC_DESKTOPVERTRES = 117, AW_DC_DESKTOPHORZRES = 118,
    AW_DC_BLTALIGNMENT = 119, AW_DC_SHADEBLENDCAPS = 120, AW_DC_COLORMGMTCAPS = 121
};

/* LOGPEN / LOGBRUSH / BITMAP / LOGPALETTE layouts (Win32 ABI) */
typedef struct { UINT lopnStyle; POINT lopnWidth; COLORREF lopnColor; } aw_logpen;
typedef struct { UINT lbStyle; COLORREF lbColor; ULONG_PTR lbHatch; } aw_logbrush;
typedef struct {
    LONG bmType, bmWidth, bmHeight, bmWidthBytes;
    WORD bmPlanes, bmBitsPixel;
    LPVOID bmBits;
} aw_bitmap;
typedef struct { WORD palVersion, palNumEntries; aw_palentry palPalEntry[1]; } aw_logpalette;
typedef struct {
    DWORD biSize;
    LONG biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} aw_bmi_header;
typedef struct { aw_bmi_header bmiHeader; DWORD bmiColors[1]; } aw_bmi;

static gdi_obj *bitmap_obj(HBITMAP h) { return gdi_get(h, GDI_OBJ_BITMAP); }

/* the bitmap currently selected in a memory DC (NULL for window DCs) */
static gdi_obj *dc_bitmap(gdi_dc *d) { return d->is_window ? NULL : gdi_get(d->bmp, GDI_OBJ_BITMAP); }

/* ---------------------------------------------------------------------- */
/* bitmaps                                                                 */
/* ---------------------------------------------------------------------- */

/* gray level of a palette-less index of `bpp` bits */
static uint32_t gray_of(uint32_t v, int bpp)
{
    uint32_t max = (1u << bpp) - 1u;
    uint32_t g = (v * 255u + max / 2) / max;
    return (g << 16) | (g << 8) | g;
}

API(gdi32, HBITMAP, CreateBitmap, (int w, int h, UINT planes, UINT bpp, const void *bits))
{
    if (w <= 0 || h <= 0 || planes == 0 || bpp == 0) {
        aw_set_last_error(AW_ERR_INVALID_PARAMETER);
        return NULL;
    }
    aw_surface *s = surface_new(w, h);
    gdi_obj *o = gdi_alloc_obj(GDI_OBJ_BITMAP);
    o->u.bmp = s;
    if (bits) {
        /* DDB scan lines are WORD aligned, top row first; planes are taken as one bit field */
        int depth = (int)(bpp * planes);
        size_t stride = (((size_t)w * depth + 15) / 16) * 2;
        const uint8_t *src = bits;
        for (int y = 0; y < h; y++) {
            uint32_t *dst = surface_row(s, y);
            const uint8_t *row = src + stride * (size_t)y;
            for (int x = 0; x < w; x++) {
                uint32_t px = 0;
                switch (depth) {
                case 1:
                    px = ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 0x00FFFFFFu : 0;
                    break;
                case 4:
                    px = gray_of((row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF, 4);
                    break;
                case 8:
                    px = gray_of(row[x], 8);
                    break;
                case 16: {
                    uint16_t v = (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8));
                    uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                    px = ((r * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31);
                    break;
                }
                case 24:
                    px = ((uint32_t)row[3 * x + 2] << 16) | ((uint32_t)row[3 * x + 1] << 8) | row[3 * x];
                    break;
                default: /* 32: B G R X */
                    px = ((uint32_t)row[4 * x + 2] << 16) | ((uint32_t)row[4 * x + 1] << 8) | row[4 * x];
                    break;
                }
                dst[x] = px;
            }
        }
    }
    return (HBITMAP)gdi_obj_handle(o);
}

API(gdi32, int, GetObjectA, (HGDIOBJ h, int count, void *buf))
{
    gdi_obj *o = gdi_get(h, 0);
    if (!o) {
        aw_set_last_error(AW_ERR_INVALID_PARAMETER);
        return 0;
    }
    /* sizes of what each object type reports (GetObject semantics) */
    switch (o->type) {
    case GDI_OBJ_PEN: {
        aw_logpen p = {o->u.pen.style, {o->u.pen.width, 0}, o->u.pen.color};
        if (!buf) return sizeof p;
        memcpy(buf, &p, count < (int)sizeof p ? (size_t)count : sizeof p);
        return count < (int)sizeof p ? count : (int)sizeof p;
    }
    case GDI_OBJ_BRUSH: {
        aw_logbrush b = {(UINT)o->u.brush.style, o->u.brush.color, 0};
        if (!buf) return sizeof b;
        memcpy(buf, &b, count < (int)sizeof b ? (size_t)count : sizeof b);
        return count < (int)sizeof b ? count : (int)sizeof b;
    }
    case GDI_OBJ_FONT: {
        LOGFONTA lf;
        memset(&lf, 0, sizeof lf);
        const aw_font_metrics *m = font_metrics(o->u.font);
        lf.lfHeight = m->height;
        lf.lfWeight = m->weight;
        lf.lfItalic = (BYTE)(m->italic != 0);
        lf.lfUnderline = (BYTE)(m->underline != 0);
        lf.lfStrikeOut = (BYTE)(m->strikeout != 0);
        lf.lfCharSet = 1; /* DEFAULT_CHARSET */
        lf.lfOutPrecision = 0;
        lf.lfClipPrecision = 0;
        lf.lfQuality = 0;
        lf.lfPitchAndFamily = (BYTE)(m->fixed_pitch ? 1 : 2);
        memcpy(lf.lfFaceName, m->face, sizeof lf.lfFaceName - 1);
        if (!buf) return sizeof lf;
        memcpy(buf, &lf, count < (int)sizeof lf ? (size_t)count : sizeof lf);
        return count < (int)sizeof lf ? count : (int)sizeof lf;
    }
    case GDI_OBJ_BITMAP: {
        aw_bitmap bm;
        memset(&bm, 0, sizeof bm);
        bm.bmWidth = o->u.bmp->w;
        bm.bmHeight = o->u.bmp->h;
        bm.bmWidthBytes = o->u.bmp->w * 4;
        bm.bmPlanes = 1;
        bm.bmBitsPixel = 32;
        if (!buf) return sizeof bm;
        memcpy(buf, &bm, count < (int)sizeof bm ? (size_t)count : sizeof bm);
        return count < (int)sizeof bm ? count : (int)sizeof bm;
    }
    case GDI_OBJ_PAL: {
        WORD n = (WORD)o->u.pal.n;
        if (!buf) return sizeof n;
        if (count < (int)sizeof n)
            return 0;
        memcpy(buf, &n, sizeof n);
        return sizeof n;
    }
    default:
        aw_set_last_error(AW_ERR_INVALID_PARAMETER);
        return 0;
    }
}

API(gdi32, HGDIOBJ, GetCurrentObject, (HDC hdc, UINT type))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d) {
        aw_set_last_error(6);
        return NULL;
    }
    switch (type) {
    case 1: /* OBJ_PEN */
    case 11: /* OBJ_EXTPEN */
        return d->pen;
    case 2: /* OBJ_BRUSH */
        return d->brush;
    case 5: /* OBJ_PAL */
        return d->pal;
    case 6: /* OBJ_FONT */
        return d->font;
    case 7: /* OBJ_BITMAP */
        return d->bmp;
    default:
        return NULL;
    }
}

/* ---------------------------------------------------------------------- */
/* DIB readback                                                            */
/* ---------------------------------------------------------------------- */

API(gdi32, int, GetDIBits, (HDC hdc, HBITMAP hbm, UINT start, UINT lines, void *bits, aw_bmi *bmi, UINT usage))
{
    (void)hdc;
    (void)usage;
    aw_surface *s = gdi_bitmap_surface(hbm);
    if (!s || !bmi) {
        aw_set_last_error(6);
        return 0;
    }
    int bottom_up = bmi->bmiHeader.biHeight >= 0;
    if (!bits) {
        /* query: describe the bitmap in the caller's header, 32bpp BI_RGB */
        memset(&bmi->bmiHeader, 0, sizeof bmi->bmiHeader);
        bmi->bmiHeader.biSize = sizeof bmi->bmiHeader;
        bmi->bmiHeader.biWidth = s->w;
        bmi->bmiHeader.biHeight = s->h;
        bmi->bmiHeader.biPlanes = 1;
        bmi->bmiHeader.biBitCount = 32;
        bmi->bmiHeader.biCompression = 0;
        bmi->bmiHeader.biSizeImage = (DWORD)(s->w * 4 * s->h);
        return s->h;
    }
    int bpp = bmi->bmiHeader.biBitCount;
    if (bpp != 24 && bpp != 32) {
        aw_set_last_error(AW_ERR_INVALID_PARAMETER);
        return 0;
    }
    if (start >= (UINT)s->h)
        return 0;
    if (start + lines > (UINT)s->h)
        lines = (UINT)s->h - start;
    size_t stride = (((size_t)s->w * (size_t)bpp + 31) / 32) * 4;
    uint8_t *out = bits;
    for (UINT i = 0; i < lines; i++) {
        UINT dib_row = start + i;
        int y = bottom_up ? s->h - 1 - (int)dib_row : (int)dib_row;
        const uint32_t *src = surface_row(s, y);
        uint8_t *dst = out + stride * i;
        for (int x = 0; x < s->w; x++) {
            uint32_t px = src[x];
            dst[x * (bpp / 8) + 0] = (uint8_t)px;
            dst[x * (bpp / 8) + 1] = (uint8_t)(px >> 8);
            dst[x * (bpp / 8) + 2] = (uint8_t)(px >> 16);
            if (bpp == 32)
                dst[x * 4 + 3] = 0;
        }
    }
    return (int)lines;
}

/* ---------------------------------------------------------------------- */
/* device capabilities                                                     */
/* ---------------------------------------------------------------------- */

API(gdi32, int, GetDeviceCaps, (HDC hdc, int idx))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d) {
        aw_set_last_error(6);
        return 0;
    }
    int sw = 0, sh = 0;
    plat_screen_size(&sw, &sh);
    if (sw <= 0) sw = 1024;
    if (sh <= 0) sh = 768;
    gdi_obj *bo = dc_bitmap(d);
    int w = bo ? bo->u.bmp->w : sw, h = bo ? bo->u.bmp->h : sh;
    int mmx = (sw * 254 + 480) / 960, mmy = (sh * 254 + 480) / 960; /* 96 dpi */
    if (mmx <= 0) mmx = 1;
    if (mmy <= 0) mmy = 1;
    uint32_t rc = 0x0001 /* RC_BITBLT */ | 0x0008 /* RC_BITMAP64 */ | 0x0080 /* RC_DI_BITMAP */ |
                  0x0200 /* RC_DIBTODEV */ | 0x0800 /* RC_STRETCHBLT */ | 0x1000 /* RC_FLOODFILL */ |
                  0x2000 /* RC_STRETCHDIB */;
    switch (idx) {
    case AW_DC_DRIVERVERSION: return 0x0100;
    case AW_DC_TECHNOLOGY: return 2 /* DT_RASDISPLAY */;
    case AW_DC_HORZSIZE: return mmx;
    case AW_DC_VERTSIZE: return mmy;
    case AW_DC_HORZRES:
    case AW_DC_DESKTOPHORZRES:
        return w;
    case AW_DC_VERTRES:
    case AW_DC_DESKTOPVERTRES:
        return h;
    case AW_DC_BITSPIXEL: return 32;
    case AW_DC_PLANES: return 1;
    case AW_DC_NUMBRUSHES: return -1;
    case AW_DC_NUMPENS: return -1;
    case AW_DC_NUMMARKERS: return 0;
    case AW_DC_NUMFONTS: return 0;
    case AW_DC_NUMCOLORS: return -1;
    case AW_DC_PDEVICESIZE: return 0;
    case AW_DC_CURVECAPS: return 0x1FF; /* CC_CIRCLES..CC_ROUNDRECT */
    case AW_DC_LINECAPS: return 0xFE;
    case AW_DC_POLYGONALCAPS: return 0xFF;
    case AW_DC_TEXTCAPS: return 0x0002; /* TC_RA_ABLE */
    case AW_DC_CLIPCAPS: return 1; /* CP_RECTANGLE */
    case AW_DC_RASTERCAPS: return (int)rc;
    case AW_DC_ASPECTX: return 36;
    case AW_DC_ASPECTY: return 36;
    case AW_DC_ASPECTXY: return 51;
    case AW_DC_LOGPIXELSX: return 96;
    case AW_DC_LOGPIXELSY: return 96;
    case AW_DC_SIZEPALETTE: return 0;
    case AW_DC_NUMRESERVED: return 0;
    case AW_DC_COLORRES: return 24;
    case AW_DC_PHYSICALWIDTH: return w;
    case AW_DC_PHYSICALHEIGHT: return h;
    case AW_DC_PHYSICALOFFSETX:
    case AW_DC_PHYSICALOFFSETY:
        return 0;
    case AW_DC_SCALINGFACTORX:
    case AW_DC_SCALINGFACTORY:
        return 0;
    case AW_DC_VREFRESH: return 0;
    case AW_DC_BLTALIGNMENT: return 0;
    case AW_DC_SHADEBLENDCAPS: return 0;
    case AW_DC_COLORMGMTCAPS: return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------- */
/* single pixels and mapping mode                                          */
/* ---------------------------------------------------------------------- */

/* device pixel of a DC, or NULL when it is outside the surface or the clip */
static uint32_t *dc_pixel(gdi_dc *d, int x, int y, int *inside)
{
    int dx, dy;
    gdi_map(d, x, y, &dx, &dy);
    *inside = 0;
    if (!d->surf || dx < 0 || dy < 0 || dx >= d->surf->w || dy >= d->surf->h)
        return NULL;
    if (!gdi_clip_ok(d, dx, dy))
        return NULL;
    *inside = 1;
    return surface_row(d->surf, dy) + dx;
}

API(gdi32, COLORREF, GetPixel, (HDC hdc, int x, int y))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d)
        return AW_CLR_INVALID;
    int in;
    uint32_t *p = dc_pixel(d, x, y, &in);
    if (!p)
        return AW_CLR_INVALID;
    uint32_t px = *p & 0x00FFFFFFu; /* 0x00RRGGBB -> COLORREF 0x00BBGGRR */
    return ((px & 0xFF) << 16) | (px & 0xFF00) | ((px >> 16) & 0xFF);
}

API(gdi32, COLORREF, SetPixel, (HDC hdc, int x, int y, COLORREF color))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d)
        return AW_CLR_INVALID;
    int in;
    uint32_t *p = dc_pixel(d, x, y, &in);
    if (!p)
        return AW_CLR_INVALID;
    *p = gdi_px(color & 0x00FFFFFFu);
    RECT r;
    int dx, dy;
    gdi_map(d, x, y, &dx, &dy);
    r.left = dx;
    r.top = dy;
    r.right = dx + 1;
    r.bottom = dy + 1;
    gdi_mark(d, &r);
    return color & 0x00FFFFFFu;
}

API(gdi32, int, SetMapMode, (HDC hdc, int mode))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d) {
        aw_set_last_error(6);
        return 0;
    }
    int old = d->map_mode;
    /* only MM_TEXT mapping is implemented; other modes are recorded and reported back */
    d->map_mode = mode;
    return old;
}

/* ---------------------------------------------------------------------- */
/* palettes                                                                */
/* ---------------------------------------------------------------------- */

static gdi_obj *pal_obj(HPALETTE h) { return gdi_get(h, GDI_OBJ_PAL); }

API(gdi32, HPALETTE, CreatePalette, (const aw_logpalette *lp))
{
    if (!lp || lp->palNumEntries == 0) {
        aw_set_last_error(AW_ERR_INVALID_PARAMETER);
        return NULL;
    }
    gdi_obj *o = gdi_alloc_obj(GDI_OBJ_PAL);
    o->u.pal.n = lp->palNumEntries;
    o->u.pal.e = malloc(sizeof(aw_palentry) * (size_t)o->u.pal.n);
    if (!o->u.pal.e)
        aw_fatal("gdi: out of memory");
    memcpy(o->u.pal.e, lp->palPalEntry, sizeof(aw_palentry) * (size_t)o->u.pal.n);
    return (HPALETTE)gdi_obj_handle(o);
}

API(gdi32, HGDIOBJ, SelectPalette, (HDC hdc, HPALETTE hpal, BOOL force_bkgd))
{
    (void)force_bkgd;
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d || (hpal && !pal_obj(hpal))) {
        aw_set_last_error(6);
        return NULL;
    }
    HGDIOBJ old = d->pal;
    if (hpal)
        gdi_ref_select(hpal);
    d->pal = hpal;
    gdi_ref_deselect(old);
    return old;
}

API(gdi32, UINT, RealizePalette, (HDC hdc))
{
    gdi_dc *d = gdi_get_dc(hdc);
    if (!d)
        return 0;
    /* truecolor device: the system palette never changes */
    return 0;
}

API(gdi32, UINT, SetPaletteEntries, (HPALETTE hpal, UINT start, UINT count, const aw_palentry *e))
{
    gdi_obj *o = pal_obj(hpal);
    if (!o || !e)
        return 0;
    if (start >= (UINT)o->u.pal.n)
        return 0;
    if (start + count > (UINT)o->u.pal.n)
        count = (UINT)o->u.pal.n - start;
    memcpy(o->u.pal.e + start, e, sizeof(aw_palentry) * count);
    return count;
}

API(gdi32, UINT, GetPaletteEntries, (HPALETTE hpal, UINT start, UINT count, aw_palentry *e))
{
    gdi_obj *o = pal_obj(hpal);
    if (!o)
        return 0;
    if (!e)
        return (UINT)o->u.pal.n;
    if (start >= (UINT)o->u.pal.n)
        return 0;
    if (start + count > (UINT)o->u.pal.n)
        count = (UINT)o->u.pal.n - start;
    memcpy(e, o->u.pal.e + start, sizeof(aw_palentry) * count);
    return count;
}

API(gdi32, BOOL, UnrealizeObject, (HGDIOBJ h))
{
    (void)h;
    return TRUE;
}

API(gdi32, int, UpdateColors, (HDC hdc))
{
    (void)hdc;
    return 0; /* truecolor device: no colours change */
}
