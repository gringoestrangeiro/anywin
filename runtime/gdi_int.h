/* anywin runtime - private header of the GDI package (S4): surface.c region.c gdi.c gdi_draw.c
 * drawtext.c and their native tests.  Not included by user32 or controls, and not part of the
 * public Win32 surface.  Public GDI/user32 entry points are declared by API() in their own files. */
#ifndef AW_GDI_INT_H
#define AW_GDI_INT_H
#include "gui.h"

/* object types, Windows OBJ_* values; they sit in the top byte of every GDI handle */
enum {
    GDI_OBJ_PEN = 1,
    GDI_OBJ_BRUSH = 2,
    GDI_OBJ_DC = 3,
    GDI_OBJ_PAL = 5,
    GDI_OBJ_FONT = 6,
    GDI_OBJ_BITMAP = 7,
    GDI_OBJ_REGION = 8,
    GDI_OBJ_MEMDC = 10
};

#ifndef R2_BLACK
#define R2_BLACK 1
#define R2_NOT 6
#define R2_XORPEN 7
#define R2_NOP 11
#define R2_COPYPEN 13
#define R2_WHITE 16
#endif
#ifndef BS_SOLID
#define BS_SOLID 0
#define BS_NULL 1
#define BS_HATCHED 2
#endif
#ifndef PS_SOLID
#define PS_SOLID 0
#define PS_DASH 1
#define PS_DOT 2
#define PS_DASHDOT 3
#define PS_DASHDOTDOT 4
#define PS_NULL 5
#define PS_INSIDEFRAME 6
#endif
#ifndef TRANSPARENT
#define TRANSPARENT 1
#define OPAQUE 2
#endif
#ifndef ALTERNATE
#define ALTERNATE 1
#define WINDING 2
#endif
#ifndef MM_TEXT
#define MM_TEXT 1
#endif
#ifndef TA_NOUPDATECP
#define TA_NOUPDATECP 0
#define TA_UPDATECP 1
#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_TOP 0
#define TA_BOTTOM 8
#define TA_BASELINE 24
#endif
#ifndef NULLREGION
#define NULLREGION 1
#define SIMPLEREGION 2
#define COMPLEXREGION 3
#endif
#ifndef RGN_AND
#define RGN_AND 1
#define RGN_OR 2
#define RGN_XOR 3
#define RGN_DIFF 4
#define RGN_COPY 5
#endif
#define AW_ERROR_INVALID_HANDLE 6
#define AW_ERROR_INVALID_PARAMETER 87
/* system color indices (Windows values) used by the drawing helpers */
#define AW_COLOR_3DSHADOW 16
#define AW_COLOR_BTNFACE 15
#define AW_COLOR_3DHILIGHT 20
#define AW_COLOR_3DDKSHADOW 21
#define AW_COLOR_3DLIGHT 22

/* stock object ids (GetStockObject) */
enum {
    AW_STOCK_WHITE_BRUSH = 0,
    AW_STOCK_BLACK_BRUSH = 4,
    AW_STOCK_NULL_BRUSH = 5,
    AW_STOCK_WHITE_PEN = 6,
    AW_STOCK_BLACK_PEN = 7,
    AW_STOCK_NULL_PEN = 8,
    AW_STOCK_OEM_FIXED_FONT = 10,
    AW_STOCK_ANSI_FIXED_FONT = 11,
    AW_STOCK_ANSI_VAR_FONT = 12,
    AW_STOCK_SYSTEM_FONT = 13,
    AW_STOCK_DEVICE_DEFAULT_FONT = 14,
    AW_STOCK_SYSTEM_FIXED_FONT = 16,
    AW_STOCK_DEFAULT_GUI_FONT = 17,
    AW_STOCK_DC_BRUSH = 18,
    AW_STOCK_DC_PEN = 19
};

/* TEXTMETRIC layouts (Win32 ABI) */
typedef struct aw_textmetricA {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    LONG tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang;
    LONG tmDigitizedAspectX, tmDigitizedAspectY;
    BYTE tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} aw_textmetricA;

typedef struct aw_textmetricW {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    LONG tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang;
    LONG tmDigitizedAspectX, tmDigitizedAspectY;
    WCHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} aw_textmetricW;

typedef struct gdi_dc gdi_dc;
typedef struct gdi_obj gdi_obj;

struct gdi_obj {
    int type;     /* GDI_OBJ_* */
    int slot;     /* index in the handle table */
    int stock;    /* stock id + 1 (0 = not a stock object; stock objects are never freed) */
    int selcount; /* DCs that currently select this object */
    int doomed;   /* DeleteObject while selected: freed when the last selection is dropped */
    union {
        struct {
            int style, width;
            COLORREF color;
        } pen;
        struct {
            int style;
            COLORREF color;
        } brush;
        aw_font *font;
        aw_surface *bmp;
        aw_region rgn;
        gdi_dc *dc;
    } u;
};

struct gdi_dc {
    HDC handle;
    HWND hwnd;               /* window DC owner (may be NULL) */
    int is_window;           /* window DC over a surface owned by user32 */
    aw_surface *surf;        /* drawing target: the window surface or the selected bitmap */
    HGDIOBJ def_bmp;         /* memory DC: the 1x1 bitmap created with it (freed with it) */
    aw_region vis;           /* visible region, device coordinates, fixed at creation */
    aw_region clip;          /* clip region, device coordinates, always a subset of vis */
    RECT cb;                 /* bounding box of clip (fast reject while drawing) */
    int vx, vy, wx, wy;      /* device = logical - window origin + viewport origin (MM_TEXT) */
    int map_mode;
    HGDIOBJ pen, brush, font, bmp;
    COLORREF text_color, bk_color, dc_brush, dc_pen;
    int bk_mode, rop2, text_align, poly_fill;
    int cur_x, cur_y;        /* current position, logical (MoveToEx, LineTo, TA_UPDATECP) */
};

/* ---- gdi.c: handle table and DC services ------------------------------------ */
gdi_obj *gdi_get(HGDIOBJ h, int type);   /* live object of that type (0 = any); else NULL */
gdi_dc *gdi_get_dc(HDC h);                /* live DC (window or memory); else NULL */
HGDIOBJ gdi_stock(int id);                /* GetStockObject; created on first use */
void gdi_map(const gdi_dc *d, int lx, int ly, int *dx, int *dy); /* logical -> device point */
void gdi_map_rect(const gdi_dc *d, const RECT *lr, RECT *dr);     /* logical -> device corners (no normalize) */
void gdi_clip_update(gdi_dc *d);          /* refresh cb after clip changes */
void gdi_mark(gdi_dc *d, const RECT *dev); /* mark device rect dirty on the DC's surface */
int gdi_rgn_equal(const aw_region *a, const aw_region *b);   /* region.c */
COLORREF gdi_pen_color(const gdi_dc *d, const gdi_obj *pen); /* stock DC_PEN resolved from the DC */
int gdi_brush_color(const gdi_dc *d, HGDIOBJ h, COLORREF *out); /* 1 solid, 0 null, -1 invalid */
aw_font *gdi_default_font(void);          /* DEFAULT_GUI_FONT, used when a DC has none */

/* ---- colours ------------------------------------------------------------------ */
/* COLORREF 0x00BBGGRR -> surface pixel 0x00RRGGBB */
static inline uint32_t gdi_px(COLORREF c) { return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu); }

/* alpha blend: a in 0..255, src over dst; weights scaled to 256 so a=255 is exact */
static inline uint32_t gdi_blend(uint32_t d, uint32_t s, uint32_t a)
{
    uint32_t wa = a + (a >> 7), iw = 256 - wa;
    uint32_t rb = (d & 0xFF00FFu) * iw + (s & 0xFF00FFu) * wa;
    uint32_t g = (d & 0x00FF00u) * iw + (s & 0x00FF00u) * wa;
    return (((rb >> 8) & 0xFF00FFu) | ((g >> 8) & 0x00FF00u));
}

/* raster op 2 (R2_*): the 4-bit truth table over (pen, dst) bits, same encoding as Windows */
static inline uint32_t gdi_rop2(int rop, uint32_t d, uint32_t p)
{
    uint32_t t = (uint32_t)(rop - 1) & 15u, nd = ~d, np = ~p, r = 0;
    if (t & 1)
        r |= np & nd;
    if (t & 2)
        r |= np & d;
    if (t & 4)
        r |= p & nd;
    if (t & 8)
        r |= p & d;
    return r & 0x00FFFFFFu;
}

/* ternary raster op (BitBlt/PatBlt): ROP3 truth table over (pattern, source, dst) bits */
static inline uint32_t gdi_rop3(uint32_t rop3, uint32_t p, uint32_t s, uint32_t d)
{
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (!((rop3 >> i) & 1))
            continue;
        uint32_t mp = (i & 4) ? p : ~p, ms = (i & 2) ? s : ~s, md = (i & 1) ? d : ~d;
        r |= mp & ms & md;
    }
    return r & 0x00FFFFFFu;
}

/* ---- clipping ----------------------------------------------------------------- */
static inline int gdi_clip_ok(const gdi_dc *d, int x, int y)
{
    if (x < d->cb.left || x >= d->cb.right || y < d->cb.top || y >= d->cb.bottom)
        return 0;
    return d->clip.n == 1 || rgn_contains_point(&d->clip, x, y);
}

/* ---- gdi_draw.c: device-space pixel services (clip + ROP2, no dirty marking) -- */
void gdi_fill_dev(gdi_dc *d, RECT r, uint32_t px, int rop); /* device rect through clip and ROP2 */
void gdi_put(gdi_dc *d, int x, int y, uint32_t px);      /* one pixel through ROP2, clip checked */
void gdi_alpha_put(gdi_dc *d, int x, int y, uint32_t px, uint32_t alpha); /* AA write, clip checked */

#endif
