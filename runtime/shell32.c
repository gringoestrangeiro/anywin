/* anywin runtime - shell32 notification area icons (S8).
 *
 * Shell_NotifyIconA/W keep one tray icon per (hWnd, uID). The icon is an XEmbed window created
 * by plat_tray_add() and painted from the HICON's ARGB pixels with plat_tray_draw(). Pointer
 * events on that window become callback messages on the owner window, packed as Windows does:
 * legacy (version 0/3): wParam = uID, lParam = mouse message (WM_LBUTTONDOWN ...);
 * NOTIFYICON_VERSION_4: wParam = MAKEWPARAM(x, y), lParam = MAKELPARAM(event, uID), where a
 * left click is NIN_SELECT and a right click is WM_CONTEXTMENU. Without a tray manager the calls
 * still succeed (TRUE), the icon is kept, and NIM_MODIFY retries the docking. Balloon tips and
 * tooltips are accepted and ignored. */
#include "aw.h"
#include "gui.h"
#include <pthread.h>
#include <stddef.h>

extern BOOL WINAPI __aw_user32_PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);

#define NIM_ADD 0
#define NIM_MODIFY 1
#define NIM_DELETE 2
#define NIM_SETFOCUS 3
#define NIM_SETVERSION 4
#define NIF_MESSAGE 0x1
#define NIF_ICON 0x2
#define NOTIFYICON_VERSION_4 4
#define NIN_SELECT (0x400)
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_LBUTTONDBLCLK 0x203
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205
#define WM_MBUTTONDOWN 0x207
#define WM_MBUTTONUP 0x208
#define WM_CONTEXTMENU 0x7b
#define DOUBLE_CLICK_MS 500

typedef struct {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} GUID_;

/* NOTIFYICONDATAA / NOTIFYICONDATAW, x64 layout (only the leading fields and uVersion are read) */
typedef struct {
    DWORD cbSize;
    HWND hWnd;
    UINT uID, uFlags, uCallbackMessage;
    HICON hIcon;
    char szTip[128];
    DWORD dwState, dwStateMask;
    char szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    } u;
    char szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID_ guidItem;
    HICON hBalloonIcon;
} NID_A;

typedef struct {
    DWORD cbSize;
    HWND hWnd;
    UINT uID, uFlags, uCallbackMessage;
    HICON hIcon;
    WCHAR szTip[128];
    DWORD dwState, dwStateMask;
    WCHAR szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    } u;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID_ guidItem;
    HICON hBalloonIcon;
} NID_W;

/* the fields of either structure that the icon logic uses; has_* tells whether cbSize covers them */
struct nid {
    HWND hwnd;
    UINT id, flags, callback;
    HICON icon;
    UINT version;
    int has_flags, has_version;
};

#define NID_HAS(n, sz, field) ((sz) >= offsetof(__typeof__(*(n)), field) + sizeof((n)->field))

static void nid_read_a(const NID_A *n, struct nid *o)
{
    o->hwnd = n->hWnd;
    o->id = n->uID;
    o->flags = NID_HAS(n, n->cbSize, uFlags) ? n->uFlags : 0;
    o->callback = NID_HAS(n, n->cbSize, uCallbackMessage) ? n->uCallbackMessage : 0;
    o->icon = NID_HAS(n, n->cbSize, hIcon) ? n->hIcon : NULL;
    o->version = NID_HAS(n, n->cbSize, u) ? n->u.uVersion : 0;
    o->has_flags = NID_HAS(n, n->cbSize, uFlags);
    o->has_version = NID_HAS(n, n->cbSize, u);
}

static void nid_read_w(const NID_W *n, struct nid *o)
{
    o->hwnd = n->hWnd;
    o->id = n->uID;
    o->flags = NID_HAS(n, n->cbSize, uFlags) ? n->uFlags : 0;
    o->callback = NID_HAS(n, n->cbSize, uCallbackMessage) ? n->uCallbackMessage : 0;
    o->icon = NID_HAS(n, n->cbSize, hIcon) ? n->hIcon : NULL;
    o->version = NID_HAS(n, n->cbSize, u) ? n->u.uVersion : 0;
    o->has_flags = NID_HAS(n, n->cbSize, uFlags);
    o->has_version = NID_HAS(n, n->cbSize, u);
}

/* ---- icons ---------------------------------------------------------------------- */
/* generic icon used when the program gives none: a grey disc with a dark rim */
static uint32_t fallback_px[16 * 16];
static int fallback_ready;

static void fallback_init(void)
{
    if (fallback_ready)
        return;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int dx = 2 * x - 15, dy = 2 * y - 15, d2 = dx * dx + dy * dy;
            uint32_t c = 0;
            if (d2 <= 13 * 13)
                c = d2 <= 10 * 10 ? 0xffc8c8c8u : 0xff404040u;
            fallback_px[y * 16 + x] = c;
        }
    fallback_ready = 1;
}

/* ---- the icon table ------------------------------------------------------------- */
struct tray {
    HWND hwnd;
    UINT id, callback;
    uint32_t xwin; /* plat_tray_add window; 0 while no tray manager is running */
    int size, version;
    uint32_t *argb; /* last painted pixels, repainted on EXPOSE */
    int w, h;
    uint32_t last_down; /* ms time of the last left button press, for double clicks */
    struct tray *next;
};

static struct tray *trays;
static pthread_mutex_t tray_mu = PTHREAD_MUTEX_INITIALIZER;

static struct tray *tray_find(HWND hwnd, UINT id)
{
    for (struct tray *t = trays; t; t = t->next)
        if (t->hwnd == hwnd && t->id == id)
            return t;
    return NULL;
}

static struct tray *tray_by_xwin(uint32_t win)
{
    for (struct tray *t = trays; t; t = t->next)
        if (t->xwin == win)
            return t;
    return NULL;
}

/* the icon's pixels (its own when HICON is an icon object, else the generic one) */
static void icon_pixels(HICON icon, const uint32_t **px, int *w, int *h)
{
    aw_icon *ic = aw_icon_from_handle(icon);
    if (ic && ic->argb && ic->w > 0 && ic->h > 0) {
        *px = ic->argb;
        *w = ic->w;
        *h = ic->h;
        return;
    }
    fallback_init();
    *px = fallback_px;
    *w = 16;
    *h = 16;
}

/* docks the icon if no window is docked yet; the tray size is fixed by the tray manager */
static void tray_dock(struct tray *t)
{
    if (t->xwin)
        return;
    int size = 0;
    t->xwin = plat_tray_add(&size);
    t->size = t->xwin ? size : 0;
}

static void tray_paint(struct tray *t, const uint32_t *px, int w, int h)
{
    uint32_t *copy = malloc((size_t)w * h * sizeof *copy);
    memcpy(copy, px, (size_t)w * h * sizeof *copy);
    free(t->argb);
    t->argb = copy;
    t->w = w;
    t->h = h;
    if (t->xwin)
        plat_tray_draw(t->xwin, t->argb, w, h);
}

/* ---- NIM_* -------------------------------------------------------------------------- */
static BOOL nim_add(const struct nid *n)
{
    pthread_mutex_lock(&tray_mu);
    BOOL ok = FALSE;
    if (!tray_find(n->hwnd, n->id)) {
        struct tray *t = calloc(1, sizeof *t);
        t->hwnd = n->hwnd;
        t->id = n->id;
        t->callback = (n->flags & NIF_MESSAGE) ? n->callback : 0;
        tray_dock(t);
        const uint32_t *px;
        int w, h;
        icon_pixels((n->flags & NIF_ICON) ? n->icon : NULL, &px, &w, &h);
        tray_paint(t, px, w, h);
        t->next = trays;
        trays = t;
        ok = TRUE;
    }
    pthread_mutex_unlock(&tray_mu);
    return ok;
}

static BOOL nim_modify(const struct nid *n)
{
    pthread_mutex_lock(&tray_mu);
    struct tray *t = tray_find(n->hwnd, n->id);
    if (t) {
        if (n->flags & NIF_MESSAGE)
            t->callback = n->callback;
        tray_dock(t); /* a tray manager may have started since NIM_ADD */
        if (n->flags & NIF_ICON) {
            const uint32_t *px;
            int w, h;
            icon_pixels(n->icon, &px, &w, &h);
            tray_paint(t, px, w, h);
        } else if (t->argb && t->xwin) {
            plat_tray_draw(t->xwin, t->argb, t->w, t->h);
        }
    }
    pthread_mutex_unlock(&tray_mu);
    return t != NULL;
}

static BOOL nim_delete(const struct nid *n)
{
    pthread_mutex_lock(&tray_mu);
    BOOL ok = FALSE;
    for (struct tray **pp = &trays; *pp; pp = &(*pp)->next)
        if ((*pp)->hwnd == n->hwnd && (*pp)->id == n->id) {
            struct tray *t = *pp;
            *pp = t->next;
            if (t->xwin)
                plat_tray_remove(t->xwin);
            free(t->argb);
            free(t);
            ok = TRUE;
            break;
        }
    pthread_mutex_unlock(&tray_mu);
    return ok;
}

static BOOL nim_dispatch(DWORD msg, const struct nid *n)
{
    switch (msg) {
    case NIM_ADD:
        return nim_add(n);
    case NIM_MODIFY:
        return nim_modify(n);
    case NIM_DELETE:
        return nim_delete(n);
    case NIM_SETFOCUS:
    case NIM_SETVERSION: {
        pthread_mutex_lock(&tray_mu);
        struct tray *t = tray_find(n->hwnd, n->id);
        if (t && msg == NIM_SETVERSION && n->has_version)
            t->version = (int)n->version;
        pthread_mutex_unlock(&tray_mu);
        return t != NULL;
    }
    default:
        return FALSE;
    }
}

API(shell32, BOOL, Shell_NotifyIconA, (DWORD msg, NID_A *n))
{
    if (!n)
        return FALSE;
    struct nid x;
    nid_read_a(n, &x);
    return nim_dispatch(msg, &x);
}

API(shell32, BOOL, Shell_NotifyIconW, (DWORD msg, NID_W *n))
{
    if (!n)
        return FALSE;
    struct nid x;
    nid_read_w(n, &x);
    return nim_dispatch(msg, &x);
}

/* ---- pointer events on the tray windows -------------------------------------- */
/* Called by user32's event dispatcher before it routes an X event. Returns 1 when the event
 * belongs to a tray icon (consumed). */
int aw_tray_handle_event(aw_plat_event *ev)
{
    if (!ev || !ev->win)
        return 0;
    pthread_mutex_lock(&tray_mu);
    struct tray *t = tray_by_xwin(ev->win);
    if (!t) {
        pthread_mutex_unlock(&tray_mu);
        return 0;
    }
    HWND hwnd = t->hwnd;
    UINT id = t->id, cb = t->callback;
    int v4 = t->version >= NOTIFYICON_VERSION_4;
    UINT msg = 0;
    switch (ev->type) {
    case PEV_EXPOSE:
        if (t->argb && t->xwin)
            plat_tray_draw(t->xwin, t->argb, t->w, t->h);
        break;
    case PEV_BUTTONDOWN:
        if (ev->button == 1) {
            int dbl = t->last_down && ev->time - t->last_down < DOUBLE_CLICK_MS;
            t->last_down = dbl ? 0 : ev->time;
            if (!v4)
                msg = dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN;
        } else if (ev->button == 2 && !v4) {
            msg = WM_MBUTTONDOWN;
        } else if (ev->button == 3 && !v4) {
            msg = WM_RBUTTONDOWN;
        }
        break;
    case PEV_BUTTONUP:
        if (ev->button == 1)
            msg = v4 ? NIN_SELECT : WM_LBUTTONUP;
        else if (ev->button == 2 && !v4)
            msg = WM_MBUTTONUP;
        else if (ev->button == 3)
            msg = v4 ? WM_CONTEXTMENU : WM_RBUTTONUP;
        break;
    case PEV_MOTION:
        if (!v4)
            msg = WM_MOUSEMOVE;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&tray_mu);

    if (msg && cb && hwnd) {
        WPARAM wp;
        LPARAM lp;
        if (v4) {
            wp = (WPARAM)(uint16_t)ev->root_x | ((WPARAM)(uint16_t)ev->root_y << 16);
            lp = (LPARAM)(((uint32_t)msg & 0xffff) | ((uint32_t)id << 16));
        } else {
            wp = id;
            lp = msg;
        }
        __aw_user32_PostMessageA(hwnd, cb, wp, lp);
    }
    return 1;
}
