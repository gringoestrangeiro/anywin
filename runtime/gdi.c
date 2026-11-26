/* anywin GDI core (S4): handle table, stock objects, DCs and their state, fonts/pens/brushes,
 * clip regions and HRGN objects.  Drawing lives in gdi_draw.c and drawtext.c.
 *
 * Handles are (type << 24) | (slot + 1): non-zero, 32 bit, with the object type checked on every
 * lookup.  Objects selected into a DC are reference counted; DeleteObject on a selected object
 * dooms it and the memory is released when the last selection is dropped. */
#include "gdi_int.h"

#define GDI_MAX_STOCK 32

static gdi_obj **slots;
static uint32_t nslots, capslots;
static HGDIOBJ stock_tab[GDI_MAX_STOCK];

static void set_err(DWORD e) { aw_set_last_error(e); }

static HGDIOBJ obj_handle(const gdi_obj *o)
{
    return (HGDIOBJ)(uintptr_t)(((uint32_t)o->type << 24) | (uint32_t)(o->slot + 1));
}

gdi_obj *gdi_get(HGDIOBJ h, int type)
{
    uint32_t v = (uint32_t)(uintptr_t)h;
    uint32_t idx = v & 0x00FFFFFFu;
    int t = (int)(v >> 24);
    if (!idx || idx > nslots)
        return NULL;
    gdi_obj *o = slots[idx - 1];
    if (!o || o->type != t)
        return NULL;
    if (type && t != type)
        return NULL;
    return o;
}

gdi_dc *gdi_get_dc(HDC h)
{
    gdi_obj *o = gdi_get(h, 0);
    if (!o || (o->type != GDI_OBJ_DC && o->type != GDI_OBJ_MEMDC))
        return NULL;
    return o->u.dc;
}

static gdi_obj *obj_new(int type)
{
    uint32_t i = 0;
    while (i < nslots && slots[i])
        i++;
    if (i == nslots) {
        if (nslots == capslots) {
            uint32_t nc = capslots ? capslots * 2 : 64;
            gdi_obj **p = realloc(slots, sizeof *p * nc);
            if (!p)
                aw_fatal("gdi: out of memory");
            slots = p;
            capslots = nc;
        }
        slots[nslots++] = NULL;
    }
    gdi_obj *o = calloc(1, sizeof *o);
    if (!o)
        aw_fatal("gdi: out of memory");
    o->type = type;
    o->slot = (int)i;
    slots[i] = o;
    return o;
}

/* releases the object's payload and its table slot (DCs: see dc_delete) */
static void obj_free(gdi_obj *o)
{
    switch (o->type) {
    case GDI_OBJ_BITMAP:
        surface_free(o->u.bmp);
        break;
    case GDI_OBJ_REGION:
        rgn_free(&o->u.rgn);
        break;
    default:
        break;
    }
    slots[o->slot] = NULL;
    free(o);
}

static void select_ref(HGDIOBJ h)
{
    gdi_obj *o = gdi_get(h, 0);
    if (o)
        o->selcount++;
}

static void deselect_ref(HGDIOBJ h)
{
    gdi_obj *o = gdi_get(h, 0);
    if (!o)
        return;
    if (o->selcount > 0)
        o->selcount--;
    if (o->selcount == 0 && o->doomed && !o->stock)
        obj_free(o);
}

/* ---------------------------------------------------------------------- */
/* stock objects                                                           */
/* ---------------------------------------------------------------------- */

static aw_font *stock_font(int id)
{
    switch (id) {
    case AW_STOCK_OEM_FIXED_FONT:
    case AW_STOCK_ANSI_FIXED_FONT:
    case AW_STOCK_SYSTEM_FIXED_FONT:
        return font_open("Courier New", -13, 0, 400, 0, 0, 0);
    case AW_STOCK_ANSI_VAR_FONT:
    case AW_STOCK_SYSTEM_FONT:
        return font_open("MS Shell Dlg", -11, 0, 400, 0, 0, 0);
    default: /* DEVICE_DEFAULT_FONT, DEFAULT_GUI_FONT */
        return font_open("Segoe UI", -12, 0, 400, 0, 0, 0);
    }
}

HGDIOBJ gdi_stock(int id)
{
    if (id < 0 || id >= GDI_MAX_STOCK)
        return NULL;
    if (stock_tab[id])
        return stock_tab[id];
    gdi_obj *o = NULL;
    switch (id) {
    case AW_STOCK_WHITE_BRUSH:
    case 1:
    case 2:
    case 3:
    case AW_STOCK_BLACK_BRUSH:
        o = obj_new(GDI_OBJ_BRUSH);
        o->u.brush.style = BS_SOLID;
        o->u.brush.color = id == AW_STOCK_WHITE_BRUSH ? 0x00FFFFFFu
                           : id == 1                  ? 0x00C0C0C0u
                           : id == 2                  ? 0x00808080u
                           : id == 3                  ? 0x00404040u
                                                      : 0u;
        break;
    case AW_STOCK_NULL_BRUSH:
        o = obj_new(GDI_OBJ_BRUSH);
        o->u.brush.style = BS_NULL;
        break;
    case AW_STOCK_WHITE_PEN:
    case AW_STOCK_BLACK_PEN:
        o = obj_new(GDI_OBJ_PEN);
        o->u.pen.style = PS_SOLID;
        o->u.pen.width = 1;
        o->u.pen.color = id == AW_STOCK_WHITE_PEN ? 0x00FFFFFFu : 0u;
        break;
    case AW_STOCK_NULL_PEN:
        o = obj_new(GDI_OBJ_PEN);
        o->u.pen.style = PS_NULL;
        o->u.pen.width = 1;
        break;
    case AW_STOCK_DC_BRUSH: /* colour comes from the DC (SetDCBrushColor state) */
        o = obj_new(GDI_OBJ_BRUSH);
        o->u.brush.style = BS_SOLID;
        break;
    case AW_STOCK_DC_PEN:
        o = obj_new(GDI_OBJ_PEN);
        o->u.pen.style = PS_SOLID;
        o->u.pen.width = 1;
        break;
    case AW_STOCK_OEM_FIXED_FONT:
    case AW_STOCK_ANSI_FIXED_FONT:
    case AW_STOCK_ANSI_VAR_FONT:
    case AW_STOCK_SYSTEM_FONT:
    case AW_STOCK_DEVICE_DEFAULT_FONT:
    case AW_STOCK_SYSTEM_FIXED_FONT:
    case AW_STOCK_DEFAULT_GUI_FONT:
        o = obj_new(GDI_OBJ_FONT);
        o->u.font = stock_font(id);
        break;
    default:
        return NULL;
    }
    o->stock = id + 1;
    stock_tab[id] = obj_handle(o);
    return stock_tab[id];
}

aw_font *gdi_default_font(void)
{
    gdi_obj *f = gdi_get(gdi_stock(AW_STOCK_DEFAULT_GUI_FONT), GDI_OBJ_FONT);
    return f ? f->u.font : NULL;
}

/* ---------------------------------------------------------------------- */
/* DC services used by gdi_draw.c / drawtext.c                             */
/* ---------------------------------------------------------------------- */

void gdi_map(const gdi_dc *d, int lx, int ly, int *dx, int *dy)
{
    *dx = lx - d->wx + d->vx;
    *dy = ly - d->wy + d->vy;
}

void gdi_map_rect(const gdi_dc *d, const RECT *lr, RECT *dr)
{
    gdi_map(d, lr->left, lr->top, &dr->left, &dr->top);
    gdi_map(d, lr->right, lr->bottom, &dr->right, &dr->bottom);
}

void gdi_clip_update(gdi_dc *d) { rgn_bbox(&d->clip, &d->cb); }

void gdi_mark(gdi_dc *d, const RECT *dev) { surface_mark_dirty(d->surf, dev); }

COLORREF gdi_pen_color(const gdi_dc *d, const gdi_obj *pen)
{
    return pen->stock == AW_STOCK_DC_PEN + 1 ? d->dc_pen : pen->u.pen.color;
}

int gdi_brush_color(const gdi_dc *d, HGDIOBJ h, COLORREF *out)
{
    uintptr_t v = (uintptr_t)h;
    if (v >= 1 && v <= 31) { /* (HBRUSH)(COLOR_xxx + 1): system colour brush */
        *out = aw_syscolor((int)v - 1);
        return 1;
    }
    gdi_obj *o = gdi_get(h, GDI_OBJ_BRUSH);
    if (!o)
        return -1;
    if (o->stock == AW_STOCK_DC_BRUSH + 1) {
        *out = d->dc_brush;
        return 1;
    }
    if (o->u.brush.style == BS_NULL)
        return 0;
    *out = o->u.brush.color;
    return 1;
}

/* ---------------------------------------------------------------------- */
/* DC lifecycle                                                            */
/* ---------------------------------------------------------------------- */

static void select_default_objects(gdi_dc *d)
{
    d->pen = gdi_stock(AW_STOCK_BLACK_PEN);
    d->brush = gdi_stock(AW_STOCK_WHITE_BRUSH);
    d->font = gdi_stock(AW_STOCK_DEFAULT_GUI_FONT);
    select_ref(d->pen);
    select_ref(d->brush);
    select_ref(d->font);
    d->text_color = 0;
    d->bk_color = 0x00FFFFFFu;
    d->dc_brush = 0x00FFFFFFu;
    d->dc_pen = 0;
    d->bk_mode = OPAQUE;
    d->rop2 = R2_COPYPEN;
    d->text_align = TA_LEFT | TA_TOP;
    d->poly_fill = ALTERNATE;
    d->map_mode = MM_TEXT;
    d->vx = d->vy = d->wx = d->wy = 0;
    d->cur_x = d->cur_y = 0;
}

static gdi_obj *dc_alloc(int type, gdi_dc **out)
{
    gdi_obj *o = obj_new(type);
    gdi_dc *d = calloc(1, sizeof *d);
    if (!d)
        aw_fatal("gdi: out of memory");
    o->u.dc = d;
    d->handle = obj_handle(o);
    select_default_objects(d);
    *out = d;
    return o;
}

/* releases everything a DC owns; the table slot is freed by the caller */
static void dc_release(gdi_dc *d)
{
    deselect_ref(d->pen);
    deselect_ref(d->brush);
    deselect_ref(d->font);
    deselect_ref(d->bmp);
    if (d->def_bmp) {
        gdi_obj *b = gdi_get(d->def_bmp, GDI_OBJ_BITMAP);
        if (b && b->selcount == 0)
            obj_free(b);
        else if (b)
            b->doomed = 1;
    }
    rgn_free(&d->vis);
    rgn_free(&d->clip);
    free(d);
}

static void dc_delete(gdi_obj *o)
{
    dc_release(o->u.dc);
    slots[o->slot] = NULL;
    free(o);
}

HDC gdi_create_window_dc(HWND hwnd, aw_surface *surf, int org_x, int org_y, const aw_region *vis)
{
    gdi_dc *d;
    gdi_obj *o = dc_alloc(GDI_OBJ_DC, &d);
    d->is_window = 1;
    d->hwnd = hwnd;
    d->surf = surf;
    if (vis) {
        rgn_copy(&d->vis, vis);
    } else {
        RECT all = {0, 0, surf ? surf->w : 0, surf ? surf->h : 0};
        rgn_set_rect(&d->vis, &all);
    }
    rgn_copy(&d->clip, &d->vis);
    d->vx = org_x;
    d->vy = org_y;
    gdi_clip_update(d);
    return (HDC)obj_handle(o);
}

int gdi_is_window_dc(HDC hdc)
{
    gdi_dc *d = gdi_get_dc(hdc);
    return d && d->is_window;
}

void gdi_delete_window_dc(HDC hdc)
{
    gdi_obj *o = gdi_get(hdc, 0);
    if (o && (o->type == GDI_OBJ_DC || o->type == GDI_OBJ_MEMDC))
        dc_delete(o);
}

HWND gdi_dc_window(HDC hdc)
{
    gdi_dc *d = gdi_get_dc(hdc);
    return d ? d->hwnd : NULL;
}

aw_font *gdi_dc_font(HDC hdc)
{
    gdi_dc *d = gdi_get_dc(hdc);
    gdi_obj *f = d ? gdi_get(d->font, GDI_OBJ_FONT) : NULL;
    return f ? f->u.font : gdi_default_font();
}

aw_surface *gdi_bitmap_surface(HBITMAP bmp)
{
    gdi_obj *o = gdi_get(bmp, GDI_OBJ_BITMAP);
    return o ? o->u.bmp : NULL;
}

/* ---------------------------------------------------------------------- */
/* public API: DCs and objects                                             */
/* ---------------------------------------------------------------------- */

API(gdi32, HDC, CreateCompatibleDC, (HDC ref))
{
    (void)ref;
    gdi_dc *d;
    gdi_obj *o = dc_alloc(GDI_OBJ_MEMDC, &d);
    d->is_window = 0;
    d->surf = surface_new(1, 1);
    gdi_obj *b = obj_new(GDI_OBJ_BITMAP);
    b->u.bmp = d->surf;
    d->def_bmp = obj_handle(b);
    select_ref(d->def_bmp);
    d->bmp = d->def_bmp;
    RECT one = {0, 0, 1, 1};
    rgn_set_rect(&d->vis, &one);
    rgn_set_rect(&d->clip, &one);
    gdi_clip_update(d);
    return (HDC)obj_handle(o);
}

API(gdi32, BOOL, DeleteDC, (HDC hdc))
{
    gdi_obj *o = gdi_get(hdc, 0);
    if (!o || o->type != GDI_OBJ_MEMDC) {
        set_err(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    dc_delete(o);
    return TRUE;
}

API(gdi32, HBITMAP, CreateCompatibleBitmap, (HDC hdc, int w, int h))
{
    (void)hdc;
    gdi_obj *o = obj_new(GDI_OBJ_BITMAP);
    o->u.bmp = surface_new(w > 0 ? w : 1, h > 0 ? h : 1);
    return (HBITMAP)obj_handle(o);
}

API(gdi32, HBRUSH, CreateSolidBrush, (COLORREF color))
{
    gdi_obj *o = obj_new(GDI_OBJ_BRUSH);
    o->u.brush.style = BS_SOLID;
    o->u.brush.color = color & 0x00FFFFFFu;
    return (HBRUSH)obj_handle(o);
}

API(gdi32, HPEN, CreatePen, (int style, int width, COLORREF color))
{
    gdi_obj *o = obj_new(GDI_OBJ_PEN);
    o->u.pen.style = style;
    o->u.pen.width = width > 0 ? width : 1;
    o->u.pen.color = color & 0x00FFFFFFu;
    return (HPEN)obj_handle(o);
}

static HFONT new_font(const char *face, int height, int width, int weight, int italic, int underline,
                      int strikeout)
{
    aw_font *f = font_open(face, height, width, weight, italic, underline, strikeout);
    gdi_obj *o = obj_new(GDI_OBJ_FONT);
    o->u.font = f;
    return (HFONT)obj_handle(o);
}

API(gdi32, HFONT, CreateFontA,
    (int height, int width, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strikeout,
     DWORD charset, DWORD outprec, DWORD clipprec, DWORD quality, DWORD pitch, LPCSTR face))
{
    (void)esc;
    (void)orient;
    (void)charset;
    (void)outprec;
    (void)clipprec;
    (void)quality;
    (void)pitch;
    char name[32] = "";
    if (face)
        for (int i = 0; i < 31 && face[i]; i++)
            name[i] = face[i];
    return new_font(name, height, width, weight, italic != 0, underline != 0, strikeout != 0);
}

API(gdi32, HFONT, CreateFontW,
    (int height, int width, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strikeout,
     DWORD charset, DWORD outprec, DWORD clipprec, DWORD quality, DWORD pitch, LPCWSTR face))
{
    (void)esc;
    (void)orient;
    (void)charset;
    (void)outprec;
    (void)clipprec;
    (void)quality;
    (void)pitch;
    char name[128] = "";
    if (face) {
        WCHAR units[32];
        int i = 0;
        for (; i < 31 && face[i]; i++)
            units[i] = face[i];
        units[i] = 0;
        int n = aw_w2u_buf(units, i, name, (int)sizeof name - 1); /* at most 31 units = 93 bytes */
        name[n > 0 ? n : 0] = 0;
    }
    return new_font(name, height, width, weight, italic != 0, underline != 0, strikeout != 0);
}

API(gdi32, HGDIOBJ, GetStockObject, (int id)) { return gdi_stock(id); }

API(gdi32, BOOL, DeleteObject, (HGDIOBJ h))
{
    gdi_obj *o = gdi_get(h, 0);
    if (!o) {
        set_err(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (o->type == GDI_OBJ_DC || o->type == GDI_OBJ_MEMDC)
        return FALSE;
    if (o->stock)
        return TRUE;
    if (o->selcount > 0) {
        o->doomed = 1;
        return TRUE;
    }
    obj_free(o);
    return TRUE;
}

API(gdi32, HGDIOBJ, SelectObject, (HDC hdc, HGDIOBJ h))
{
    gdi_dc *d = gdi_get_dc(hdc);
    gdi_obj *o = gdi_get(h, 0);
    if (!d || !o) {
        set_err(AW_ERROR_INVALID_HANDLE);
        return NULL;
    }
    HGDIOBJ *slot;
    switch (o->type) {
    case GDI_OBJ_PEN:
        slot = &d->pen;
        break;
    case GDI_OBJ_BRUSH:
        slot = &d->brush;
        break;
    case GDI_OBJ_FONT:
        slot = &d->font;
        break;
    case GDI_OBJ_BITMAP:
        if (d->is_window)
            return NULL;
        slot = &d->bmp;
        break;
    default:
        return NULL;
    }
    HGDIOBJ old = *slot;
    o->selcount++;
    *slot = h;
    deselect_ref(old);
    if (o->type == GDI_OBJ_BITMAP) {
        /* a memory DC sees exactly the selected bitmap */
        d->surf = o->u.bmp;
        RECT full = {0, 0, d->surf->w, d->surf->h};
        rgn_set_rect(&d->vis, &full);
        rgn_copy(&d->clip, &d->vis);
        gdi_clip_update(d);
    }
    return old;
}

/* ---------------------------------------------------------------------- */
/* public API: DC state                                                    */
/* ---------------------------------------------------------------------- */

#define DC_OR_FAIL(d, hdc, fail)                                                                   \
    gdi_dc *d = gdi_get_dc(hdc);                                                                   \
    if (!d) {                                                                                      \
        set_err(AW_ERROR_INVALID_HANDLE);                                                          \
        return (fail);                                                                             \
    }

API(gdi32, COLORREF, SetTextColor, (HDC hdc, COLORREF c))
{
    DC_OR_FAIL(d, hdc, 0xFFFFFFFFu)
    COLORREF old = d->text_color;
    d->text_color = c & 0x00FFFFFFu;
    return old;
}

API(gdi32, COLORREF, GetTextColor, (HDC hdc))
{
    DC_OR_FAIL(d, hdc, 0xFFFFFFFFu)
    return d->text_color;
}

API(gdi32, COLORREF, SetBkColor, (HDC hdc, COLORREF c))
{
    DC_OR_FAIL(d, hdc, 0xFFFFFFFFu)
    COLORREF old = d->bk_color;
    d->bk_color = c & 0x00FFFFFFu;
    return old;
}

API(gdi32, COLORREF, GetBkColor, (HDC hdc))
{
    DC_OR_FAIL(d, hdc, 0xFFFFFFFFu)
    return d->bk_color;
}

API(gdi32, int, SetBkMode, (HDC hdc, int mode))
{
    DC_OR_FAIL(d, hdc, 0)
    int old = d->bk_mode;
    if (mode == TRANSPARENT || mode == OPAQUE)
        d->bk_mode = mode;
    return old;
}

API(gdi32, int, GetBkMode, (HDC hdc))
{
    DC_OR_FAIL(d, hdc, 0)
    return d->bk_mode;
}

API(gdi32, int, SetROP2, (HDC hdc, int rop))
{
    DC_OR_FAIL(d, hdc, 0)
    int old = d->rop2;
    if (rop >= R2_BLACK && rop <= R2_WHITE)
        d->rop2 = rop;
    return old;
}

API(gdi32, int, GetROP2, (HDC hdc))
{
    DC_OR_FAIL(d, hdc, 0)
    return d->rop2;
}

API(gdi32, UINT, SetTextAlign, (HDC hdc, UINT align))
{
    DC_OR_FAIL(d, hdc, 0xFFFFFFFFu)
    UINT old = (UINT)d->text_align;
    d->text_align = (int)align;
    return old;
}

API(gdi32, UINT, GetTextAlign, (HDC hdc))
{
    DC_OR_FAIL(d, hdc, 0xFFFFFFFFu)
    return (UINT)d->text_align;
}

API(gdi32, int, SetPolyFillMode, (HDC hdc, int mode))
{
    DC_OR_FAIL(d, hdc, 0)
    int old = d->poly_fill;
    if (mode == ALTERNATE || mode == WINDING)
        d->poly_fill = mode;
    return old;
}

API(gdi32, BOOL, MoveToEx, (HDC hdc, int x, int y, POINT *old))
{
    DC_OR_FAIL(d, hdc, FALSE)
    if (old) {
        old->x = d->cur_x;
        old->y = d->cur_y;
    }
    d->cur_x = x;
    d->cur_y = y;
    return TRUE;
}

API(gdi32, BOOL, SetViewportOrgEx, (HDC hdc, int x, int y, POINT *old))
{
    DC_OR_FAIL(d, hdc, FALSE)
    if (old) {
        old->x = d->vx;
        old->y = d->vy;
    }
    d->vx = x;
    d->vy = y;
    return TRUE;
}

API(gdi32, BOOL, SetWindowOrgEx, (HDC hdc, int x, int y, POINT *old))
{
    DC_OR_FAIL(d, hdc, FALSE)
    if (old) {
        old->x = d->wx;
        old->y = d->wy;
    }
    d->wx = x;
    d->wy = y;
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* public API: regions and clipping                                        */
/* ---------------------------------------------------------------------- */

static int region_type(const aw_region *g)
{
    return g->n == 0 ? NULLREGION : g->n == 1 ? SIMPLEREGION : COMPLEXREGION;
}

static HRGN new_rgn(const RECT *rc)
{
    gdi_obj *o = obj_new(GDI_OBJ_REGION);
    rgn_init(&o->u.rgn);
    rgn_set_rect(&o->u.rgn, rc);
    return (HRGN)obj_handle(o);
}

API(gdi32, HRGN, CreateRectRgn, (int l, int t, int r, int b))
{
    RECT rc = {l, t, r, b};
    return new_rgn(&rc);
}

API(gdi32, HRGN, CreateRectRgnIndirect, (const RECT *rc))
{
    RECT z = {0, 0, 0, 0};
    return new_rgn(rc ? rc : &z);
}

API(gdi32, BOOL, SetRectRgn, (HRGN h, int l, int t, int r, int b))
{
    gdi_obj *o = gdi_get(h, GDI_OBJ_REGION);
    if (!o) {
        set_err(AW_ERROR_INVALID_HANDLE);
        return FALSE;
    }
    RECT rc = {l, t, r, b};
    rgn_set_rect(&o->u.rgn, &rc);
    return TRUE;
}

API(gdi32, int, CombineRgn, (HRGN dst, HRGN s1, HRGN s2, int mode))
{
    gdi_obj *D = gdi_get(dst, GDI_OBJ_REGION), *A = gdi_get(s1, GDI_OBJ_REGION);
    gdi_obj *B = mode == RGN_COPY ? A : gdi_get(s2, GDI_OBJ_REGION);
    if (!D || !A || !B) {
        set_err(AW_ERROR_INVALID_HANDLE);
        return 0;
    }
    aw_region t1, t2, x1, x2;
    rgn_init(&t1);
    rgn_init(&t2);
    rgn_init(&x1);
    rgn_init(&x2);
    rgn_copy(&t1, &A->u.rgn);
    rgn_copy(&t2, &B->u.rgn);
    switch (mode) {
    case RGN_AND:
        rgn_intersect(&t1, &t2);
        break;
    case RGN_OR:
        rgn_union(&t1, &t2);
        break;
    case RGN_DIFF:
        rgn_subtract(&t1, &t2);
        break;
    case RGN_XOR:
        rgn_copy(&x1, &t1);
        rgn_copy(&x2, &t2);
        rgn_subtract(&x1, &t2);
        rgn_subtract(&x2, &t1);
        rgn_union(&x1, &x2);
        rgn_copy(&t1, &x1);
        break;
    case RGN_COPY:
        break;
    default:
        rgn_free(&t1);
        rgn_free(&t2);
        rgn_free(&x1);
        rgn_free(&x2);
        set_err(AW_ERROR_INVALID_PARAMETER);
        return 0;
    }
    rgn_copy(&D->u.rgn, &t1);
    rgn_free(&t1);
    rgn_free(&t2);
    rgn_free(&x1);
    rgn_free(&x2);
    return region_type(&D->u.rgn);
}

API(gdi32, int, OffsetRgn, (HRGN h, int dx, int dy))
{
    gdi_obj *o = gdi_get(h, GDI_OBJ_REGION);
    if (!o)
        return 0;
    rgn_offset(&o->u.rgn, dx, dy);
    return region_type(&o->u.rgn);
}

API(gdi32, int, GetRgnBox, (HRGN h, RECT *out))
{
    gdi_obj *o = gdi_get(h, GDI_OBJ_REGION);
    if (!o)
        return 0;
    rgn_bbox(&o->u.rgn, out);
    return region_type(&o->u.rgn);
}

API(gdi32, BOOL, PtInRegion, (HRGN h, int x, int y))
{
    gdi_obj *o = gdi_get(h, GDI_OBJ_REGION);
    return o ? rgn_contains_point(&o->u.rgn, x, y) != 0 : FALSE;
}

API(gdi32, BOOL, RectInRegion, (HRGN h, const RECT *rc))
{
    gdi_obj *o = gdi_get(h, GDI_OBJ_REGION);
    return (o && rc) ? rgn_intersects_rect(&o->u.rgn, rc) != 0 : FALSE;
}

API(gdi32, BOOL, EqualRgn, (HRGN a, HRGN b))
{
    gdi_obj *A = gdi_get(a, GDI_OBJ_REGION), *B = gdi_get(b, GDI_OBJ_REGION);
    return (A && B) ? gdi_rgn_equal(&A->u.rgn, &B->u.rgn) : FALSE;
}

API(gdi32, int, SelectClipRgn, (HDC hdc, HRGN h))
{
    DC_OR_FAIL(d, hdc, 0)
    if (!h) {
        rgn_copy(&d->clip, &d->vis);
    } else {
        gdi_obj *o = gdi_get(h, GDI_OBJ_REGION);
        if (!o) {
            set_err(AW_ERROR_INVALID_HANDLE);
            return 0;
        }
        rgn_copy(&d->clip, &o->u.rgn);
        rgn_intersect(&d->clip, &d->vis);
    }
    gdi_clip_update(d);
    return region_type(&d->clip);
}

API(gdi32, int, IntersectClipRect, (HDC hdc, int l, int t, int r, int b))
{
    DC_OR_FAIL(d, hdc, 0)
    RECT lr = {l, t, r, b}, dr;
    gdi_map_rect(d, &lr, &dr);
    rgn_intersect_rect(&d->clip, &dr);
    gdi_clip_update(d);
    return region_type(&d->clip);
}

API(gdi32, int, ExcludeClipRect, (HDC hdc, int l, int t, int r, int b))
{
    DC_OR_FAIL(d, hdc, 0)
    RECT lr = {l, t, r, b}, dr;
    gdi_map_rect(d, &lr, &dr);
    rgn_subtract_rect(&d->clip, &dr);
    gdi_clip_update(d);
    return region_type(&d->clip);
}

API(gdi32, int, GetClipBox, (HDC hdc, RECT *out))
{
    DC_OR_FAIL(d, hdc, 0)
    RECT dev;
    rgn_bbox(&d->clip, &dev);
    out->left = dev.left - d->vx + d->wx;
    out->top = dev.top - d->vy + d->wy;
    out->right = dev.right - d->vx + d->wx;
    out->bottom = dev.bottom - d->vy + d->wy;
    if (d->clip.n == 0)
        out->left = out->top = out->right = out->bottom = 0;
    return region_type(&d->clip);
}
