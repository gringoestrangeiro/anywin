/* anywin runtime - internal interfaces between the GUI layers.
 *
 *   x11.c / font.c     platform layer  (plat_*, font_*)
 *   gdi*.c             GDI, regions, surfaces, user32 drawing helpers (DrawText...)
 *   user32*.c          window manager, messages, input, NC area, icons/cursors
 *   menu.c dialog.c    menus, accelerators, dialog manager, MessageBox
 *   controls/ *.c      built-in controls + comctl32
 *
 * Only the functions declared here may be called across layers (besides the
 * public Win32 API itself, which is reachable as __aw_<module>_<Name>).
 * All coordinates are integers in device pixels; DPI is always 96.
 */
#ifndef AW_GUI_H
#define AW_GUI_H
#include "aw.h"

/* ======================================================================
 * Surfaces: 32bpp pixels 0x00RRGGBB (same as X11 TrueColor depth 24/32bpp
 * little endian, and Windows BI_RGB 32bpp).  Implemented in gdi (surface.c).
 * ====================================================================== */
typedef struct aw_surface {
    uint32_t *px;   /* address of row 0 (the TOP row) */
    int w, h;
    int stride;     /* distance between rows in PIXELS; may be negative (bottom-up DIB) */
    int owns_px;    /* px was allocated by surface_new and is freed by surface_free */
    RECT dirty;     /* union of areas modified since the owner last flushed it */
    int has_dirty;
} aw_surface;

aw_surface *surface_new(int w, int h);                 /* zero (black) filled, owns_px */
aw_surface *surface_wrap(uint32_t *px, int w, int h, int stride); /* does not own px */
void surface_free(aw_surface *s);
void surface_resize(aw_surface *s, int w, int h);      /* keeps overlapping content, new area white */
void surface_mark_dirty(aw_surface *s, const RECT *r); /* clipped to the surface */
static inline uint32_t *surface_row(const aw_surface *s, int y) { return s->px + (ptrdiff_t)y * s->stride; }

/* ======================================================================
 * Regions: sets of non-overlapping rectangles (gdi, region.c).
 * Used by user32 for update/visible regions and by GDI for HRGN/clipping.
 * ====================================================================== */
typedef struct aw_region {
    int n, cap;
    RECT *r;        /* n non-overlapping, non-empty rects */
} aw_region;

void rgn_init(aw_region *g);                       /* empty */
void rgn_free(aw_region *g);
void rgn_clear(aw_region *g);
void rgn_copy(aw_region *dst, const aw_region *src);
void rgn_set_rect(aw_region *g, const RECT *r);
void rgn_union_rect(aw_region *g, const RECT *r);
void rgn_subtract_rect(aw_region *g, const RECT *r);
void rgn_intersect_rect(aw_region *g, const RECT *r);
void rgn_union(aw_region *g, const aw_region *o);
void rgn_subtract(aw_region *g, const aw_region *o);
void rgn_intersect(aw_region *g, const aw_region *o);
void rgn_offset(aw_region *g, int dx, int dy);
int  rgn_is_empty(const aw_region *g);
void rgn_bbox(const aw_region *g, RECT *out);      /* {0,0,0,0} when empty */
int  rgn_contains_point(const aw_region *g, int x, int y);
int  rgn_intersects_rect(const aw_region *g, const RECT *r);

/* ======================================================================
 * Platform layer: raw X11 protocol client (x11.c).  No libX11.
 * ====================================================================== */
enum {
    PLAT_WIN_NORMAL = 0, /* managed, decorated top-level window */
    PLAT_WIN_DIALOG = 1, /* managed, transient-for owner, dialog type */
    PLAT_WIN_POPUP = 2,  /* override-redirect (menus, dropdowns, tooltips) */
};

enum {
    PEV_NONE, PEV_EXPOSE, PEV_CONFIGURE, PEV_CLOSE, PEV_KEYDOWN, PEV_KEYUP, PEV_BUTTONDOWN, PEV_BUTTONUP,
    PEV_MOTION, PEV_WHEEL, PEV_ENTER, PEV_LEAVE, PEV_FOCUSIN, PEV_FOCUSOUT, PEV_MAP, PEV_UNMAP
};

/* modifier / button state bits in aw_plat_event.mods */
#define AW_MOD_SHIFT 0x01
#define AW_MOD_CTRL 0x02
#define AW_MOD_ALT 0x04
#define AW_MOD_LBUTTON 0x10
#define AW_MOD_MBUTTON 0x20
#define AW_MOD_RBUTTON 0x40
#define AW_MOD_CAPSLOCK 0x80
#define AW_MOD_NUMLOCK 0x100

typedef struct aw_plat_event {
    int type;
    uint32_t win;      /* X window the event belongs to */
    int x, y;          /* pointer events: window-relative pointer position.
                          EXPOSE: rect origin.  CONFIGURE: root position of the window's top-left */
    int w, h;          /* EXPOSE: rect size.  CONFIGURE: new window size */
    int root_x, root_y;/* pointer events: root (screen) position */
    int button;        /* BUTTONDOWN/UP: 1 left, 2 middle, 3 right, 4 X1, 5 X2 */
    int wheel_delta;   /* PEV_WHEEL: +120 away from user / -120 toward user */
    int wheel_horizontal;
    uint32_t mods;     /* AW_MOD_* state at the time of the event (before this event's change) */
    int vk;            /* KEYDOWN/KEYUP: Windows virtual-key code (VK_*) */
    int scancode;      /* KEYDOWN/KEYUP: PC set-1 scancode (bit 0x100 = extended key) */
    int is_repeat;     /* KEYDOWN: auto-repeat */
    char text[8];      /* KEYDOWN: UTF-8 text the key produces ("" if none; control chars
                          like "\r", "\b", "\t", "\x1b" ARE reported here) */
    uint32_t time;     /* ms timestamp */
} aw_plat_event;

int  plat_init(void);       /* 1 = display available. Idempotent, thread-safe. */
int  plat_fd(void);         /* connection fd (or -1) */
uint32_t plat_window_create(int x, int y, int w, int h, int kind, uint32_t owner);
void plat_window_destroy(uint32_t win);
void plat_window_show(uint32_t win, int show);
void plat_window_move(uint32_t win, int x, int y, int w, int h);  /* screen coords */
void plat_window_set_title(uint32_t win, const char *utf8);
void plat_window_set_icon(uint32_t win, const uint32_t *argb, int w, int h); /* argb 0xAARRGGBB */
void plat_window_set_resizable(uint32_t win, int resizable);
void plat_window_activate(uint32_t win);            /* raise + ask WM/X server for focus */
void plat_window_minimize(uint32_t win);
void plat_window_maximize(uint32_t win, int on);
void plat_window_get_pos(uint32_t win, int *x, int *y); /* screen pos of window top-left */
void plat_put(uint32_t win, const aw_surface *s, const RECT *r); /* copy r (surface==window coords) */
void plat_flush(void);
/* Wait for the next event.  timeout_ms: -1 infinite, 0 poll.  Returns 1 with *ev filled, 0 on
 * timeout or when plat_wakeup() was called.  Must only be called from one thread at a time. */
int  plat_poll(aw_plat_event *ev, int timeout_ms);
void plat_wakeup(void);                              /* thread-safe */
/* Extra descriptors (Winsock sockets) that the blocking wait in plat_poll must watch too.
 * fill() writes up to max pollfd entries (events set, revents 0) and returns how many; when
 * poll() reports any of them, ready() gets those same entries back with revents set.  Both run
 * on the thread that pumps plat_poll.  Up to 4 sources. */
struct pollfd;
void plat_add_fd_source(int (*fill)(struct pollfd *pf, int max), void (*ready)(struct pollfd *pf, int n));
void plat_cursor_pos(int *x, int *y);
void plat_screen_size(int *w, int *h);
void plat_set_cursor(uint32_t win, int idc);         /* IDC_* numeric id (32512 = IDC_ARROW ...) */
int  plat_grab(uint32_t win, int on);                /* pointer+keyboard grab (popup menus) */
void plat_clipboard_set(const char *utf8);           /* take CLIPBOARD ownership */
char *plat_clipboard_get(void);                      /* malloc'd UTF-8 or NULL */
uint32_t plat_tray_add(int *icon_size);              /* XEmbed system tray icon window; 0 if no tray */
void plat_tray_remove(uint32_t win);
void plat_tray_draw(uint32_t win, const uint32_t *argb, int w, int h); /* scaled, alpha over tray bg */
void plat_bell(void);

/* ======================================================================
 * Fonts (font.c): TrueType (glyf) rasterizer with antialiasing, fonts
 * looked up in the usual Linux font directories, built-in fallback font.
 * ====================================================================== */
typedef struct aw_glyph {
    int w, h;           /* bitmap size */
    int left, top;      /* pixel (i,j) goes to (pen_x + left + i, baseline_y - top + j) */
    int advance;        /* integer pixel advance */
    const uint8_t *alpha;/* w*h coverage 0..255 */
} aw_glyph;

typedef struct aw_font_metrics {
    int ascent, descent, height;   /* height = ascent + descent */
    int internal_leading, external_leading;
    int ave_char_width, max_char_width;
    int weight, italic, underline, strikeout;
    int fixed_pitch;
    char face[64];                 /* face name as requested (e.g. "Segoe UI") */
} aw_font_metrics;

typedef struct aw_font aw_font;
/* lfheight uses LOGFONT semantics: <0 em size in px, >0 cell height, 0 default (cell 16px).
 * Cached: identical parameters return the same pointer; never freed; never NULL. */
aw_font *font_open(const char *face, int lfheight, int width, int weight, int italic, int underline,
                   int strikeout);
const aw_font_metrics *font_metrics(aw_font *f);
const aw_glyph *font_glyph(aw_font *f, uint32_t codepoint);  /* never NULL */
int  font_advance(aw_font *f, uint32_t codepoint);

/* ======================================================================
 * GDI internals for user32 / controls (gdi*.c).
 * GDI handles (HDC, HFONT, HBRUSH, ...) are small table indices that fit in
 * 32 bits.  Public GDI API: __aw_gdi32_<Name>.
 * ====================================================================== */
/* Create a DC drawing into `surf`.  Logical (0,0) maps to surface (org_x, org_y).
 * `vis` (surface coordinates, copied) is the visible region: nothing outside it is ever
 * touched.  All drawing marks surf->dirty. */
HDC  gdi_create_window_dc(HWND hwnd, aw_surface *surf, int org_x, int org_y, const aw_region *vis);
int  gdi_is_window_dc(HDC hdc);
void gdi_delete_window_dc(HDC hdc);      /* ReleaseDC / EndPaint */
HWND gdi_dc_window(HDC hdc);             /* WindowFromDC */
aw_font *gdi_dc_font(HDC hdc);           /* font currently selected in the DC */
/* Draw a 0xAARRGGBB image (sw x sh) scaled to (x,y,w,h) with alpha blending, through the DC's
 * clipping and coordinate mapping.  Used for icons/cursors. */
void gdi_draw_argb(HDC hdc, int x, int y, int w, int h, const uint32_t *argb, int sw, int sh);
aw_surface *gdi_bitmap_surface(HBITMAP bmp); /* pixels of a bitmap (NULL if invalid) */

/* ======================================================================
 * user32 internals (user32*.c)
 * ====================================================================== */
/* Icons and cursors.  HICON / HCURSOR are pointers to this struct. */
typedef struct aw_icon {
    uint32_t magic;     /* AW_ICON_MAGIC */
    int w, h;
    uint32_t *argb;     /* 0xAARRGGBB, w*h, top-down */
    int hotx, hoty;
    int is_cursor;
    int idc;            /* for system cursors: IDC_* id, else 0 */
    int shared;         /* loaded with LR_SHARED / system: DestroyIcon is a no-op */
} aw_icon;
#define AW_ICON_MAGIC 0x4e4f4349u
aw_icon *aw_icon_from_handle(HICON h);          /* NULL if not an icon */

/* Lazy GUI init: connects to X11, registers built-in classes. Called by
 * every user32 entry point that needs it. */
void aw_user_init(void);

/* Built-in classes are registered from aw_user_init() through these: */
void aw_controls_register(void);   /* controls/: Button, Static, Edit, ListBox, ComboBox, ... */
void aw_dialogs_register(void);    /* dialog.c: "#32770" dialog class, message box class */
void aw_menus_register(void);      /* menu.c: popup menu window class "#32768" */
/* marks a registered class as a built-in: its windows take the charset of the creating call */
void aw_class_mark_builtin(const WCHAR *name);

/* Menu bar hooks (menu.c), called by user32's non-client code for top-level windows with a menu.
 * Coordinates are relative to the window's top-left (window DC coordinates). */
int  aw_menubar_height(HWND hwnd, HMENU menu, int width);
void aw_menubar_paint(HWND hwnd, HMENU menu, HDC window_dc, const RECT *bar);
void aw_menubar_track(HWND hwnd, HMENU menu, int x, int y, int vk); /* mouse click at x,y in bar
                                       (vk = 0) or keyboard activation (x = y = -1, vk = VK_MENU/F10/key) */
int  aw_menu_is_valid(HMENU menu);

/* Top-level window kind for an X window from its styles (used by user32 core):
 * WS_POPUP without WS_CAPTION -> PLAT_WIN_POPUP; owned WS_POPUP with caption -> PLAT_WIN_DIALOG;
 * everything else PLAT_WIN_NORMAL. */

/* Per-thread message pump helper for modal loops (menus, dialogs, message boxes):
 * runs until *done becomes non-zero or WM_QUIT is seen (which is re-posted). */
void aw_modal_loop(volatile int *done);

/* System colors: GetSysColor(COLOR_*) values (user32 core). */
COLORREF aw_syscolor(int idx);

#endif
