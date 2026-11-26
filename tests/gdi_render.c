/* GDI native render harness (S4).  Links the GDI objects directly (no X server, no user32 code).
 * Draws a scene into a window-style DC over a 320x200 surface, checks API results in C, writes the
 * surface as a PPM, and writes an expectation file of pixel probes that tests/gdi_render.sh checks
 * independently with python3.
 *
 * Usage: gdi_render <out.ppm> <expect.txt>
 * Expectation lines: "PIX x y rrggbb tol" and "COUNT x0 y0 x1 y1 min" (non-white pixels). */
#include "../runtime/gdi_int.h"

#include <stdarg.h>

/* ---- environment stubs (normally util.c, start.c and the user32 colour table) ---- */
static DWORD last_error;
void aw_set_last_error(DWORD e) { last_error = e; }
void aw_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}
#define RGBC(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
COLORREF aw_syscolor(int idx)
{
    switch (idx) {
    case 15: return RGBC(90, 91, 92);    /* COLOR_BTNFACE */
    case 16: return RGBC(100, 101, 102); /* COLOR_3DSHADOW */
    case 20: return RGBC(200, 201, 202); /* COLOR_3DHILIGHT */
    case 21: return RGBC(10, 20, 30);    /* COLOR_3DDKSHADOW */
    case 22: return RGBC(150, 151, 152); /* COLOR_3DLIGHT */
    default: return RGBC(1, 2, 3);
    }
}

/* ---- Win32 entry points under test (WINAPI = ms_abi) ----------------------------- */
#define G32(ret, name, args) extern ret WINAPI __aw_gdi32_##name args
#define U32(ret, name, args) extern ret WINAPI __aw_user32_##name args
G32(HBRUSH, CreateSolidBrush, (COLORREF));
G32(HPEN, CreatePen, (int, int, COLORREF));
G32(HFONT, CreateFontA, (int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR));
G32(BOOL, DeleteObject, (HGDIOBJ));
G32(HGDIOBJ, GetStockObject, (int));
G32(HGDIOBJ, SelectObject, (HDC, HGDIOBJ));
G32(COLORREF, SetTextColor, (HDC, COLORREF));
G32(COLORREF, SetBkColor, (HDC, COLORREF));
G32(int, SetBkMode, (HDC, int));
G32(BOOL, TextOutA, (HDC, int, int, LPCSTR, int));
G32(BOOL, GetTextExtentPoint32A, (HDC, LPCSTR, int, SIZE *));
G32(BOOL, GetTextMetricsA, (HDC, aw_textmetricA *));
G32(BOOL, MoveToEx, (HDC, int, int, POINT *));
G32(BOOL, LineTo, (HDC, int, int));
G32(BOOL, Rectangle, (HDC, int, int, int, int));
G32(BOOL, Ellipse, (HDC, int, int, int, int));
G32(BOOL, Polygon, (HDC, const POINT *, int));
G32(BOOL, BitBlt, (HDC, int, int, int, int, HDC, int, int, DWORD));
G32(HDC, CreateCompatibleDC, (HDC));
G32(HBITMAP, CreateCompatibleBitmap, (HDC, int, int));
G32(BOOL, DeleteDC, (HDC));
G32(HRGN, CreateRectRgn, (int, int, int, int));
G32(int, CombineRgn, (HRGN, HRGN, HRGN, int));
G32(BOOL, PtInRegion, (HRGN, int, int));
G32(int, GetRgnBox, (HRGN, RECT *));
G32(int, SelectClipRgn, (HDC, HRGN));
U32(int, FillRect, (HDC, const RECT *, HBRUSH));
U32(int, FrameRect, (HDC, const RECT *, HBRUSH));
U32(int, InvertRect, (HDC, const RECT *));
U32(BOOL, DrawFocusRect, (HDC, const RECT *));
U32(BOOL, DrawEdge, (HDC, RECT *, UINT, UINT));
U32(int, DrawTextA, (HDC, LPCSTR, int, RECT *, UINT));

#define CreateSolidBrush __aw_gdi32_CreateSolidBrush
#define CreatePen __aw_gdi32_CreatePen
#define CreateFontA __aw_gdi32_CreateFontA
#define DeleteObject __aw_gdi32_DeleteObject
#define GetStockObject __aw_gdi32_GetStockObject
#define SelectObject __aw_gdi32_SelectObject
#define SetTextColor __aw_gdi32_SetTextColor
#define SetBkColor __aw_gdi32_SetBkColor
#define SetBkMode __aw_gdi32_SetBkMode
#define TextOutA __aw_gdi32_TextOutA
#define GetTextExtentPoint32A __aw_gdi32_GetTextExtentPoint32A
#define GetTextMetricsA __aw_gdi32_GetTextMetricsA
#define MoveToEx __aw_gdi32_MoveToEx
#define LineTo __aw_gdi32_LineTo
#define Rectangle __aw_gdi32_Rectangle
#define Ellipse __aw_gdi32_Ellipse
#define Polygon __aw_gdi32_Polygon
#define BitBlt __aw_gdi32_BitBlt
#define CreateCompatibleDC __aw_gdi32_CreateCompatibleDC
#define CreateCompatibleBitmap __aw_gdi32_CreateCompatibleBitmap
#define DeleteDC __aw_gdi32_DeleteDC
#define CreateRectRgn __aw_gdi32_CreateRectRgn
#define CombineRgn __aw_gdi32_CombineRgn
#define PtInRegion __aw_gdi32_PtInRegion
#define GetRgnBox __aw_gdi32_GetRgnBox
#define SelectClipRgn __aw_gdi32_SelectClipRgn

/* ---- harness bookkeeping ------------------------------------------------------------ */
static int fails, checks;
static FILE *expect_f;
static aw_surface *surf;

#define CHECK(c, name)                                                                             \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("CHECK FAIL %s (line %d)\n", name, __LINE__);                                   \
        }                                                                                          \
    } while (0)

static void expect_pix(int x, int y, COLORREF c, int tol)
{
    fprintf(expect_f, "PIX %d %d %06x %d\n", x, y, (unsigned)gdi_px(c), tol);
}
static void expect_count(int x0, int y0, int x1, int y1, int min)
{
    fprintf(expect_f, "COUNT %d %d %d %d %d\n", x0, y0, x1, y1, min);
}

#define RGBW RGBC(255, 255, 255)
#define RGBK RGBC(0, 0, 0)
#define RGBR RGBC(255, 0, 0)
#define RGBG RGBC(0, 255, 0)
#define RGBB RGBC(0, 0, 255)
#define RGBY RGBC(255, 255, 0)

static RECT R(int l, int t, int r, int b)
{
    RECT q = {l, t, r, b};
    return q;
}

static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fprintf(f, "P6\n%d %d\n255\n", surf->w, surf->h);
    for (int y = 0; y < surf->h; y++)
        for (int x = 0; x < surf->w; x++) {
            uint32_t p = surface_row(surf, y)[x];
            unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: gdi_render out.ppm expect.txt\n");
        return 2;
    }
    expect_f = fopen(argv[2], "w");
    if (!expect_f)
        aw_fatal("cannot write %s", argv[2]);

    surf = surface_new(320, 200);
    RECT all = R(0, 0, 320, 200);
    HDC hdc = gdi_create_window_dc(NULL, surf, 0, 0, NULL);
    CHECK(gdi_is_window_dc(hdc), "window dc");

    /* 1. background */
    HBRUSH white = (HBRUSH)GetStockObject(0);
    CHECK(__aw_user32_FillRect(hdc, &all, white) == 1, "FillRect white");

    /* 2. solid fill; COLORREF is 0x00BBGGRR, the surface pixel is 0x00RRGGBB */
    HBRUSH red = CreateSolidBrush(RGBR);
    RECT rr = R(10, 10, 30, 20);
    CHECK(__aw_user32_FillRect(hdc, &rr, red) == 1, "FillRect red");
    expect_pix(10, 10, RGBR, 0);
    expect_pix(29, 19, RGBR, 0);
    expect_pix(30, 10, RGBW, 0);
    expect_pix(10, 20, RGBW, 0);

    /* 3. FrameRect: one pixel border inside the rectangle */
    HBRUSH blue = CreateSolidBrush(RGBB);
    RECT fr = R(40, 10, 60, 20);
    CHECK(__aw_user32_FrameRect(hdc, &fr, blue) == 1, "FrameRect");
    expect_pix(40, 10, RGBB, 0);
    expect_pix(59, 19, RGBB, 0);
    expect_pix(50, 10, RGBB, 0);
    expect_pix(41, 11, RGBW, 0);
    expect_pix(60, 10, RGBW, 0);

    /* 4. Rectangle: green pen outline, yellow interior */
    HPEN green_pen = CreatePen(PS_SOLID, 1, RGBG);
    HBRUSH yellow = CreateSolidBrush(RGBY);
    HGDIOBJ old_pen = SelectObject(hdc, green_pen);
    HGDIOBJ old_brush = SelectObject(hdc, yellow);
    CHECK(old_pen && old_brush, "select pen and brush");
    CHECK(Rectangle(hdc, 70, 10, 90, 30), "Rectangle");
    expect_pix(70, 10, RGBG, 0);
    expect_pix(89, 29, RGBG, 0);
    expect_pix(80, 29, RGBG, 0);
    expect_pix(80, 20, RGBY, 0);
    expect_pix(71, 11, RGBY, 0);
    expect_pix(90, 20, RGBW, 0);

    /* 5. Ellipse: black pen edge, blue interior */
    HPEN black_pen = CreatePen(PS_SOLID, 1, RGBK);
    SelectObject(hdc, black_pen);
    SelectObject(hdc, blue);
    CHECK(Ellipse(hdc, 100, 10, 130, 40), "Ellipse");
    expect_pix(115, 25, RGBB, 0);
    expect_pix(100, 25, RGBK, 0);
    expect_pix(115, 10, RGBK, 0);
    expect_pix(99, 25, RGBW, 0);

    /* 6. MoveToEx/LineTo: LineTo does not draw its end point */
    HPEN red_pen = CreatePen(PS_SOLID, 1, RGBR);
    SelectObject(hdc, red_pen);
    POINT old_pt;
    CHECK(MoveToEx(hdc, 140, 10, &old_pt), "MoveToEx");
    CHECK(LineTo(hdc, 180, 10), "LineTo");
    expect_pix(140, 10, RGBR, 0);
    expect_pix(179, 10, RGBR, 0);
    expect_pix(180, 10, RGBW, 0);
    MoveToEx(hdc, 140, 50, NULL);
    LineTo(hdc, 140, 80);
    expect_pix(140, 50, RGBR, 0);
    expect_pix(140, 79, RGBR, 0);
    expect_pix(140, 80, RGBW, 0);

    /* 7. Polygon with the stock NULL pen: alternate fill */
    HGDIOBJ null_pen = GetStockObject(8);
    HBRUSH grn = CreateSolidBrush(RGBG);
    SelectObject(hdc, null_pen);
    SelectObject(hdc, grn);
    POINT tri[3] = {{200, 10}, {240, 10}, {220, 40}};
    CHECK(Polygon(hdc, tri, 3), "Polygon");
    expect_pix(210, 12, RGBG, 0);
    expect_pix(220, 20, RGBG, 0);
    expect_pix(200, 12, RGBW, 0);
    expect_pix(250, 20, RGBW, 0);

    /* 8. clip region: only pixels inside the clip are touched */
    HRGN clip = CreateRectRgn(250, 150, 300, 190);
    CHECK(SelectClipRgn(hdc, clip) == SIMPLEREGION, "SelectClipRgn simple");
    HBRUSH black = CreateSolidBrush(RGBK);
    RECT big = R(240, 140, 320, 200);
    __aw_user32_FillRect(hdc, &big, black);
    expect_pix(249, 150, RGBW, 0);
    expect_pix(250, 150, RGBK, 0);
    expect_pix(299, 189, RGBK, 0);
    expect_pix(300, 149, RGBW, 0); /* above the clip: untouched */
    CHECK(SelectClipRgn(hdc, NULL) == SIMPLEREGION, "clip reset");
    DeleteObject(clip);

    /* 9. transparent text: glyph ink only, background left alone */
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGBK);
    CHECK(TextOutA(hdc, 10, 100, "Hello", 5), "TextOutA transparent");
    expect_count(10, 90, 80, 120, 100);
    expect_pix(9, 99, RGBW, 0);

    /* 10. opaque text: the character cell takes the background colour */
    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, RGBY);
    aw_textmetricA tm;
    CHECK(GetTextMetricsA(hdc, &tm), "GetTextMetricsA");
    CHECK(tm.tmHeight > 0 && tm.tmAscent + tm.tmDescent == tm.tmHeight, "metrics sum");
    SIZE sz;
    CHECK(GetTextExtentPoint32A(hdc, "Hi", 2, &sz) && sz.cx > 0 && sz.cy == tm.tmHeight, "extent");
    CHECK(TextOutA(hdc, 10, 130, "Hi", 2), "TextOutA opaque");
    /* TA_TOP: the anchor is the top of the cell, the baseline is one ascent below it */
    expect_pix(10, 130, RGBY, 0);
    expect_pix(10 + sz.cx - 1, 130 + tm.tmHeight - 1, RGBY, 0);
    SetBkMode(hdc, TRANSPARENT);

    /* 11. DrawEdge EDGE_RAISED (outer light/dark, inner light/shadow) */
    RECT edge = R(5, 160, 45, 190);
    CHECK(__aw_user32_DrawEdge(hdc, &edge, 5, 0xF), "DrawEdge");
    expect_pix(5, 160, RGBC(200, 201, 202), 0);
    expect_pix(44, 189, RGBC(10, 20, 30), 0);
    expect_pix(6, 161, RGBC(150, 151, 152), 0);

    /* 12. focus rect: XOR dotted frame; a second draw restores the background */
    RECT focus = R(150, 160, 190, 180);
    CHECK(__aw_user32_DrawFocusRect(hdc, &focus), "DrawFocusRect");
    CHECK(surface_row(surf, 160)[150] == 0x00000000u && surface_row(surf, 160)[151] == 0x00FFFFFFu,
          "focus rect XOR on first dot");
    __aw_user32_DrawFocusRect(hdc, &focus);
    expect_pix(150, 160, RGBW, 0);
    expect_pix(151, 160, RGBW, 0);

    /* 13. InvertRect */
    RECT inv = R(300, 5, 310, 15);
    CHECK(__aw_user32_InvertRect(hdc, &inv) == 1, "InvertRect");
    expect_pix(300, 5, RGBK, 0);

    /* 14. ARGB blending (alpha 0x80 red over white, through gdi_draw_argb) */
    uint32_t icon[4] = {0x80FF0000u, 0x80FF0000u, 0x80FF0000u, 0x80FF0000u};
    gdi_draw_argb(hdc, 290, 100, 2, 2, icon, 2, 2);
    expect_pix(290, 100, RGBC(255, 126, 126), 3);

    /* 15. DrawText: centred single line with mnemonic; CALCRECT wrapping and single line */
    RECT dt = R(260, 60, 320, 90);
    int lh = tm.tmHeight;
    CHECK(__aw_user32_DrawTextA(hdc, "Hi&there", -1, &dt, 0x1 | 0x4 | 0x20) == lh, "DrawText single");
    expect_count(260, 60, 320, 90, 20);
    RECT calc = R(0, 0, 60, 0);
    int h = __aw_user32_DrawTextA(hdc, "Hello world again", -1, &calc, 0x400 | 0x10);
    CHECK(h >= 2 * lh && h == calc.bottom - calc.top && calc.right <= 60, "DrawText CALCRECT wrap");
    RECT one = R(0, 0, 0, 0);
    CHECK(__aw_user32_DrawTextA(hdc, "Hello", -1, &one, 0x400 | 0x20) == lh && one.right > 0,
          "CALCRECT single");

    /* 16. handles: a selected object is doomed, not freed; stale handles are rejected */
    HGDIOBJ font = GetStockObject(17);
    CHECK(font != NULL, "stock font");
    CHECK(SelectObject(hdc, font) != NULL, "select font");
    HBRUSH tmp = CreateSolidBrush(RGBC(7, 7, 7));
    HGDIOBJ prev = SelectObject(hdc, tmp);
    CHECK(DeleteObject(tmp), "DeleteObject of selected brush");
    SelectObject(hdc, prev);
    CHECK(SelectObject(hdc, tmp) == NULL, "stale handle rejected by SelectObject");
    CHECK(DeleteObject(tmp) == FALSE, "stale handle rejected by DeleteObject");

    /* 17. regions: XOR of two overlapping rectangles */
    HRGN ra = CreateRectRgn(0, 0, 10, 10), rb_ = CreateRectRgn(5, 5, 15, 15), rc = CreateRectRgn(0, 0, 1, 1);
    CHECK(CombineRgn(rc, ra, rb_, RGN_XOR) == COMPLEXREGION, "XOR is complex");
    CHECK(PtInRegion(rc, 2, 2) && !PtInRegion(rc, 7, 7) && PtInRegion(rc, 12, 12), "XOR membership");
    RECT box;
    CHECK(GetRgnBox(rc, &box) == COMPLEXREGION && box.left == 0 && box.right == 15 && box.bottom == 15,
          "region bounding box");
    DeleteObject(ra);
    DeleteObject(rb_);
    DeleteObject(rc);

    /* 18. dirty tracking: only the touched area is marked */
    surf->has_dirty = 0;
    RECT dirty_r = R(100, 60, 120, 70);
    __aw_user32_FillRect(hdc, &dirty_r, black);
    CHECK(surf->has_dirty && surf->dirty.left == 100 && surf->dirty.top == 60 && surf->dirty.right == 120 &&
              surf->dirty.bottom == 70,
          "dirty rect");

    /* 19. memory DC and BitBlt (SRCCOPY) */
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, 40, 20);
    HGDIOBJ mold = SelectObject(mem, bmp);
    CHECK(mold != NULL, "select bitmap into memory dc");
    HBRUSH g2 = CreateSolidBrush(RGBG);
    RECT mr = R(0, 0, 40, 20);
    __aw_user32_FillRect(mem, &mr, g2);
    CHECK(BitBlt(hdc, 280, 150, 40, 20, mem, 0, 0, 0x00CC0020u), "BitBlt SRCCOPY");
    expect_pix(280, 150, RGBG, 0);
    expect_pix(319, 169, RGBG, 0);
    expect_pix(279, 145, RGBW, 0); /* left of and above the copied area */
    SelectObject(mem, mold);
    CHECK(DeleteObject(bmp), "delete bitmap after deselect");
    CHECK(DeleteDC(mem), "DeleteDC");

    /* 20. underline changes the raster (two memory renders compared pixel by pixel) */
    HFONT ufont = CreateFontA(-14, 0, 0, 0, 400, 0, 1, 0, 0, 0, 0, 0, 0, "Segoe UI");
    HFONT pfont = CreateFontA(-14, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI");
    HDC m1 = CreateCompatibleDC(hdc), m2 = CreateCompatibleDC(hdc);
    HBITMAP b1 = CreateCompatibleBitmap(hdc, 80, 30), b2 = CreateCompatibleBitmap(hdc, 80, 30);
    SelectObject(m1, b1);
    SelectObject(m2, b2);
    RECT cell = R(0, 0, 80, 30);
    __aw_user32_FillRect(m1, &cell, white);
    __aw_user32_FillRect(m2, &cell, white);
    SelectObject(m1, ufont);
    SelectObject(m2, pfont);
    TextOutA(m1, 4, 4, "Hi", 2);
    TextOutA(m2, 4, 4, "Hi", 2);
    int differ = 0;
    aw_surface *s1 = gdi_bitmap_surface(b1), *s2 = gdi_bitmap_surface(b2);
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 80; x++)
            if (surface_row(s1, y)[x] != surface_row(s2, y)[x])
                differ++;
    CHECK(differ > 0, "underline changes the raster");
    DeleteDC(m1);
    DeleteDC(m2);
    DeleteObject(b1);
    DeleteObject(b2);
    DeleteObject(ufont);
    DeleteObject(pfont);

    /* 21. FillRect with a system colour brush handle (COLOR_BTNFACE + 1) */
    RECT sys = R(300, 180, 310, 190);
    CHECK(__aw_user32_FillRect(hdc, &sys, (HBRUSH)(uintptr_t)(15 + 1)) == 1, "FillRect system colour");
    expect_pix(300, 180, aw_syscolor(15), 0);

    gdi_delete_window_dc(hdc);
    if (!write_ppm(argv[1]))
        aw_fatal("cannot write %s", argv[1]);
    fclose(expect_f);
    surface_free(surf);
    printf("gdi_render: %s (%d checks, %d failed)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
