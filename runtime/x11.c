/* anywin platform layer (gui.h plat_*): raw X11 core-protocol client over a unix socket.
 * No libX11 / libxcb.  Scope and deferrals: PLAN-CORE.md 3.8 (no TCP, BIG-REQUESTS, MIT-SHM,
 * INCR, ARGB tray visual, tray re-dock).  Requests are buffered; plat_flush / plat_poll /
 * blocking replies send them.  Nothing here is application specific. */
#include "gui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "x11.c assumes a little-endian host"
#endif

/* ======================================================================
 * Protocol constants (X11 core protocol, opcodes / event codes / masks)
 * ====================================================================== */
enum {
    OP_CreateWindow = 1, OP_ChangeWindowAttributes = 2, OP_DestroyWindow = 4, OP_ReparentWindow = 7,
    OP_MapWindow = 8, OP_UnmapWindow = 10, OP_ConfigureWindow = 12, OP_GetGeometry = 14,
    OP_InternAtom = 16, OP_ChangeProperty = 18, OP_DeleteProperty = 19, OP_GetProperty = 20,
    OP_SetSelectionOwner = 22, OP_GetSelectionOwner = 23, OP_ConvertSelection = 24, OP_SendEvent = 25,
    OP_GrabPointer = 26, OP_UngrabPointer = 27, OP_GrabKeyboard = 31, OP_UngrabKeyboard = 32,
    OP_QueryPointer = 38, OP_TranslateCoordinates = 40, OP_SetInputFocus = 42, OP_OpenFont = 45,
    OP_CreateGC = 55, OP_PutImage = 72, OP_GetImage = 73, OP_CreateGlyphCursor = 94, OP_FreeCursor = 95,
    OP_GetKeyboardMapping = 101, OP_Bell = 104, OP_GetModifierMapping = 119
};

enum { EV_KeyPress = 2, EV_KeyRelease, EV_ButtonPress, EV_ButtonRelease, EV_MotionNotify, EV_EnterNotify,
       EV_LeaveNotify, EV_FocusIn, EV_FocusOut, EV_Expose = 12, EV_UnmapNotify = 18, EV_MapNotify,
       EV_ConfigureNotify = 22, EV_PropertyNotify = 28, EV_SelectionClear, EV_SelectionRequest,
       EV_SelectionNotify, EV_ClientMessage = 33, EV_MappingNotify };

#define MASK_KeyPress 0x1
#define MASK_KeyRelease 0x2
#define MASK_ButtonPress 0x4
#define MASK_ButtonRelease 0x8
#define MASK_EnterWindow 0x10
#define MASK_LeaveWindow 0x20
#define MASK_PointerMotion 0x40
#define MASK_Exposure 0x8000
#define MASK_StructureNotify 0x20000
#define MASK_SubstructureNotify 0x80000
#define MASK_SubstructureRedirect 0x100000
#define MASK_FocusChange 0x200000

#define CW_BackPixmap 0x1
#define CW_BackPixel 0x2
#define CW_OverrideRedirect 0x200
#define CW_EventMask 0x800
#define CW_Cursor 0x4000
#define CFG_X 0x1
#define CFG_Y 0x2
#define CFG_W 0x4
#define CFG_H 0x8
#define CFG_StackMode 0x40

enum {
    A_WM_PROTOCOLS, A_WM_DELETE_WINDOW, A_WM_CHANGE_STATE, A_WM_NAME, A_WM_CLASS, A_WM_NORMAL_HINTS,
    A_WM_SIZE_HINTS, A_WM_HINTS, A_WM_TRANSIENT_FOR, A_NET_WM_NAME, A_UTF8_STRING, A_NET_WM_ICON,
    A_NET_WM_WINDOW_TYPE, A_NET_WM_WINDOW_TYPE_NORMAL, A_NET_WM_WINDOW_TYPE_DIALOG, A_NET_WM_STATE,
    A_NET_WM_STATE_MAXIMIZED_VERT, A_NET_WM_STATE_MAXIMIZED_HORZ, A_NET_ACTIVE_WINDOW, A_NET_WM_PID,
    A_NET_MOVERESIZE_WINDOW, A_NET_SUPPORTING_WM_CHECK, A_CLIPBOARD, A_TARGETS, A_STRING, A_ATOM,
    A_CARDINAL, A_WINDOW, A_NET_SYSTEM_TRAY_OPCODE, A_XEMBED_INFO, A_ANYWIN_SEL, A_COUNT
};
static const char *const atom_names[A_COUNT] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_CHANGE_STATE", "WM_NAME", "WM_CLASS", "WM_NORMAL_HINTS",
    "WM_SIZE_HINTS", "WM_HINTS", "WM_TRANSIENT_FOR", "_NET_WM_NAME", "UTF8_STRING", "_NET_WM_ICON",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_STATE",
    "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_ACTIVE_WINDOW", "_NET_WM_PID",
    "_NET_MOVERESIZE_WINDOW", "_NET_SUPPORTING_WM_CHECK", "CLIPBOARD", "TARGETS", "STRING", "ATOM",
    "CARDINAL", "WINDOW", "_NET_SYSTEM_TRAY_OPCODE", "_XEMBED_INFO", "ANYWIN_SELECTION"};
#define A(x) X.atoms[(x)]

#define MAX_TRAY 8
#define QCAP 1024

/* ======================================================================
 * State
 * ====================================================================== */
static struct {
    volatile int state; /* 0 new, 1 connecting, 2 ready, 3 unavailable */
    int fd, wake_r, wake_w;
    int display_num, screen_num;
    uint32_t rid_base, rid_mask, rid_count;
    uint32_t root, root_visual, root_w, root_h;
    int root_depth, img_msb, have_wm;
    uint32_t max_req;      /* maximum request length, 4-byte units */
    uint8_t min_kc, max_kc;
    uint32_t gc, req_no;
    uint32_t atoms[A_COUNT];
    uint32_t kspk;         /* keysyms per keycode */
    uint32_t *ksym;
    unsigned alt_mask, numlock_mask, level3_mask;
    int kbd_dirty;
    uint32_t cursor_font;
    struct { int idc; uint32_t cid; } cursors[32];
    int ncursors;
    uint32_t clip_win;
    char *clip_text;
    volatile int sel_done;
    uint32_t sel_prop;
    struct { uint32_t win; int size; uint32_t *bg; } tray[MAX_TRAY];
    int ntray;
} X;

/* output buffer */
static uint8_t *ob;
static size_t obn, obcap, rq_at;
/* input buffer */
static uint8_t *ib;
static size_t ibpos, iblen, ibcap;

/* event queue */
static aw_plat_event qev[QCAP];
static uint8_t qxl[QCAP];
static int qhead, qcount;

/* window table (size / state needed by user32 calls) */
struct xwin { uint32_t id; int kind, mapped, w, h, x, y, resizable; };
static struct xwin *wins;
static int nwins, capwins;

static void x_fatal(const char *why) {
    fprintf(stderr, "anywin: X11 connection failed: %s\n", why);
    exit(1);
}

/* ======================================================================
 * Byte helpers and request builder
 * ====================================================================== */
static uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int s16(const uint8_t *p) { return (int16_t)u16(p); }

static void ob_grow(size_t n) {
    if (obn + n <= obcap) return;
    size_t c = obcap ? obcap : 8192;
    while (c < obn + n) c *= 2;
    uint8_t *nb = realloc(ob, c);
    if (!nb) x_fatal("out of memory");
    ob = nb;
    obcap = c;
}
static void w8(uint32_t v) { ob_grow(1); ob[obn++] = (uint8_t)v; }
static void w16(uint32_t v) { ob_grow(2); ob[obn++] = (uint8_t)v; ob[obn++] = (uint8_t)(v >> 8); }
static void w32(uint32_t v) {
    ob_grow(4);
    ob[obn++] = (uint8_t)v;
    ob[obn++] = (uint8_t)(v >> 8);
    ob[obn++] = (uint8_t)(v >> 16);
    ob[obn++] = (uint8_t)(v >> 24);
}
static void wbytes(const void *p, size_t n) {
    ob_grow(n);
    memcpy(ob + obn, p, n);
    obn += n;
}

/* Start a request; returns nothing, the caller finishes with rq_end(). Never nested. */
static void rq_begin(uint8_t op, uint8_t data) {
    rq_at = obn;
    w8(op);
    w8(data);
    w16(0);
    X.req_no++;
}
static uint32_t rq_end(void) {
    while (obn & 3) w8(0);
    uint32_t units = (uint32_t)((obn - rq_at) / 4);
    ob[rq_at + 2] = (uint8_t)units;
    ob[rq_at + 3] = (uint8_t)(units >> 8);
    return X.req_no;
}

static void write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            x_fatal("write");
        }
        b += w;
        n -= (size_t)w;
    }
}
static void read_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) x_fatal("read during setup");
        b += r;
        n -= (size_t)r;
    }
}

/* Read whatever the server sent (the caller guarantees readiness or accepts blocking). */
static void ib_fill(void) {
    if (ibpos > 0 && ibpos == iblen) {
        ibpos = iblen = 0;
    } else if (ibpos > 0 && ibcap - iblen < 4096) {
        memmove(ib, ib + ibpos, iblen - ibpos);
        iblen -= ibpos;
        ibpos = 0;
    }
    if (ibcap - iblen < 4096) {
        size_t c = ibcap ? ibcap * 2 : 65536;
        while (c - iblen < 4096) c *= 2;
        uint8_t *nb = realloc(ib, c);
        if (!nb) x_fatal("out of memory");
        ib = nb;
        ibcap = c;
    }
    ssize_t n = read(X.fd, ib + iblen, ibcap - iblen);
    if (n > 0) {
        iblen += (size_t)n;
        return;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
    x_fatal("connection closed by the X server");
}

/* Send all buffered requests, reading incoming data meanwhile so that neither side blocks. */
static void flush_out(void) {
    size_t off = 0;
    while (off < obn) {
        struct pollfd pf = {X.fd, POLLOUT | POLLIN, 0};
        if (poll(&pf, 1, -1) < 0) {
            if (errno == EINTR) continue;
            x_fatal("poll");
        }
        if (pf.revents & POLLIN) ib_fill();
        if (pf.revents & POLLOUT) {
            ssize_t w = write(X.fd, ob + off, obn - off);
            if (w > 0) off += (size_t)w;
            else if (w < 0 && errno != EAGAIN && errno != EINTR) x_fatal("write");
        }
    }
    obn = 0;
}

/* Size of the next complete packet in the input buffer, 0 if none. */
static size_t pkt_ready(void) {
    if (iblen - ibpos < 32) return 0;
    const uint8_t *p = ib + ibpos;
    size_t n = 32;
    if (p[0] == 1) n += 4 * (size_t)u32(p + 4);
    if (iblen - ibpos < n) return 0;
    return n;
}

static void dispatch(const uint8_t *p);

/* Process every complete packet already buffered.  Returns 1 if any was processed. */
static int drain_packets(void) {
    int any = 0;
    size_t n;
    while ((n = pkt_ready()) != 0) {
        uint8_t *p = malloc(n);
        if (!p) x_fatal("out of memory");
        memcpy(p, ib + ibpos, n);
        ibpos += n;
        if (p[0] == 0) TRACE("X error code %u major %u seq %u\n", p[1], p[10], u16(p + 2));
        else if (p[0] != 1) dispatch(p);
        free(p);
        any = 1;
    }
    return any;
}

/* Wait for the reply (or error) to request `req`.  Returns a malloc'd packet or NULL on error. */
static uint8_t *xreply(uint32_t req, size_t *len) {
    uint16_t seq = (uint16_t)req;
    for (;;) {
        size_t n = pkt_ready();
        if (!n) {
            flush_out(); /* may read data too, so look again before blocking */
            if (!pkt_ready()) ib_fill();
            continue;
        }
        uint8_t *p = malloc(n);
        if (!p) x_fatal("out of memory");
        memcpy(p, ib + ibpos, n);
        ibpos += n;
        if (p[0] == 1) {
            if (u16(p + 2) == seq) {
                if (len) *len = n;
                return p;
            }
            TRACE("unexpected reply seq %u\n", u16(p + 2));
        } else if (p[0] == 0) {
            TRACE("X error code %u major %u seq %u\n", p[1], p[10], u16(p + 2));
            if (u16(p + 2) == seq) {
                free(p);
                return NULL;
            }
        } else {
            dispatch(p);
        }
        free(p);
    }
}

/* ======================================================================
 * Resource ids, atoms, properties, client messages
 * ====================================================================== */
static uint32_t rid_alloc(void) {
    if (!X.rid_count) X.rid_count = 1;
    return X.rid_base | (X.rid_count++ & X.rid_mask);
}

static uint32_t intern(const char *name, int only_if_exists) {
    size_t n = strlen(name);
    rq_begin(OP_InternAtom, only_if_exists ? 1 : 0);
    w16((uint32_t)n);
    w16(0);
    wbytes(name, n);
    uint32_t seq = rq_end();
    uint8_t *r = xreply(seq, NULL);
    uint32_t a = r ? u32(r + 8) : 0;
    free(r);
    return a;
}

/* ChangeProperty, split into requests no larger than the server accepts. */
static void prop_write(uint32_t win, uint32_t prop, uint32_t type, int fmt, const void *data, uint32_t n) {
    size_t esz = (size_t)fmt / 8;
    size_t maxb = (size_t)X.max_req * 4;
    size_t per = (maxb > 64 ? maxb - 64 : 4) / esz;
    const uint8_t *d = data;
    uint32_t off = 0;
    int first = 1;
    do {
        uint32_t cnt = n - off;
        if (cnt > per) cnt = (uint32_t)per;
        rq_begin(OP_ChangeProperty, first ? 0 : 2);
        w32(win);
        w32(prop);
        w32(type);
        w8((uint32_t)fmt);
        w8(0);
        w8(0);
        w8(0);
        w32(cnt);
        wbytes(d + (size_t)off * esz, (size_t)cnt * esz);
        rq_end();
        off += cnt;
        first = 0;
    } while (off < n);
}

static void prop_set_cardinal(uint32_t win, uint32_t prop, const uint32_t *v, uint32_t n) {
    prop_write(win, prop, A(A_CARDINAL), 32, v, n);
}

static void send_cm(uint32_t dest, uint32_t mask, uint32_t win, uint32_t type, const uint32_t d[5]) {
    rq_begin(OP_SendEvent, 0);
    w32(dest);
    w32(mask);
    w8(EV_ClientMessage);
    w8(32);
    w16(0);
    w32(win);
    w32(type);
    for (int i = 0; i < 5; i++) w32(d[i]);
    rq_end();
}

/* Latin-1 (or '?') rendering of a UTF-8 string, for STRING / WM_NAME. */
static char *utf8_to_latin1(const char *s, size_t *outlen) {
    size_t n = strlen(s), k = 0;
    char *o = malloc(n + 1);
    if (!o) x_fatal("out of memory");
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xc0 && c < 0xe0 && i + 1 < n) {
            cp = ((c & 0x1f) << 6) | ((unsigned char)s[i + 1] & 0x3f);
            len = 2;
        } else if (c >= 0xe0 && c < 0xf0 && i + 2 < n) {
            cp = ((c & 0x0f) << 12) | (((unsigned char)s[i + 1] & 0x3f) << 6) | ((unsigned char)s[i + 2] & 0x3f);
            len = 3;
        } else if (c >= 0xf0 && c < 0xf8 && i + 3 < n) {
            cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3f) << 12) |
                 (((unsigned char)s[i + 2] & 0x3f) << 6) | ((unsigned char)s[i + 3] & 0x3f);
            len = 4;
        }
        o[k++] = cp < 256 ? (char)cp : '?';
        i += len;
    }
    o[k] = 0;
    *outlen = k;
    return o;
}

/* ======================================================================
 * Window table
 * ====================================================================== */
static struct xwin *win_find(uint32_t id) {
    for (int i = 0; i < nwins; i++)
        if (wins[i].id == id) return &wins[i];
    return NULL;
}
static struct xwin *win_add(uint32_t id, int kind) {
    if (nwins == capwins) {
        int c = capwins ? capwins * 2 : 32;
        struct xwin *nw = realloc(wins, sizeof *wins * (size_t)c);
        if (!nw) x_fatal("out of memory");
        wins = nw;
        capwins = c;
    }
    struct xwin *w = &wins[nwins++];
    memset(w, 0, sizeof *w);
    w->id = id;
    w->kind = kind;
    w->resizable = 1;
    return w;
}
static void win_remove(uint32_t id) {
    for (int i = 0; i < nwins; i++)
        if (wins[i].id == id) {
            wins[i] = wins[--nwins];
            return;
        }
}

/* WM_NORMAL_HINTS: position + size, and fixed size when not resizable. */
static void write_size_hints(uint32_t win, int x, int y, int w, int h, int resizable) {
    uint32_t v[18];
    memset(v, 0, sizeof v);
    v[0] = 0x4 | 0x8 | (resizable ? 0 : (0x10 | 0x20));
    v[1] = (uint32_t)x;
    v[2] = (uint32_t)y;
    v[3] = (uint32_t)w;
    v[4] = (uint32_t)h;
    if (!resizable) {
        v[5] = v[7] = (uint32_t)w;
        v[6] = v[8] = (uint32_t)h;
    }
    prop_write(win, A(A_WM_NORMAL_HINTS), A(A_WM_SIZE_HINTS), 32, v, 18);
}

/* ======================================================================
 * Keyboard and key translation
 * ====================================================================== */
static uint32_t keysym_at(unsigned kc, unsigned col) {
    if (!X.ksym || kc < X.min_kc || kc > X.max_kc || col >= X.kspk) return 0;
    return X.ksym[(kc - X.min_kc) * X.kspk + col];
}

static void refresh_keyboard(void) {
    X.kbd_dirty = 0;
    uint32_t cnt = X.max_kc >= X.min_kc ? (uint32_t)(X.max_kc - X.min_kc + 1) : 0;
    rq_begin(OP_GetKeyboardMapping, 0);
    w8(X.min_kc);
    w8(cnt);
    w16(0);
    uint32_t seq = rq_end();
    size_t len = 0;
    uint8_t *r = xreply(seq, &len);
    if (r) {
        X.kspk = r[1];
        free(X.ksym);
        size_t nk = X.kspk * cnt;
        if (len < 32 + nk * 4) nk = (len - 32) / 4;
        X.ksym = malloc((nk + 1) * sizeof *X.ksym);
        if (!X.ksym) x_fatal("out of memory");
        for (size_t i = 0; i < nk; i++) X.ksym[i] = u32(r + 32 + i * 4);
        free(r);
    }
    rq_begin(OP_GetModifierMapping, 0);
    seq = rq_end();
    r = xreply(seq, &len);
    X.alt_mask = 0x08;
    X.numlock_mask = 0x10;
    X.level3_mask = 0x80;
    if (r) {
        unsigned kpm = r[1];
        int alt_set = 0, nl_set = 0, l3_set = 0;
        for (unsigned m = 0; m < 8; m++)
            for (unsigned j = 0; j < kpm && 32 + (m * kpm + j) < len; j++) {
                unsigned kc = r[32 + m * kpm + j];
                uint32_t ks = keysym_at(kc, 0);
                if (!kc) continue;
                if ((ks == 0xffe9 || ks == 0xffea || ks == 0xffe7 || ks == 0xffe8) && !alt_set) {
                    X.alt_mask = 1u << m;
                    alt_set = 1;
                } else if (ks == 0xff7f && !nl_set) {
                    X.numlock_mask = 1u << m;
                    nl_set = 1;
                } else if (ks == 0xfe03 && !l3_set) {
                    X.level3_mask = 1u << m;
                    l3_set = 1;
                }
            }
        free(r);
    }
}

static uint32_t mods_of(unsigned st) {
    uint32_t m = 0;
    if (st & 0x01) m |= AW_MOD_SHIFT;
    if (st & 0x04) m |= AW_MOD_CTRL;
    if (st & X.alt_mask) m |= AW_MOD_ALT;
    if (st & 0x02) m |= AW_MOD_CAPSLOCK;
    if (st & X.numlock_mask) m |= AW_MOD_NUMLOCK;
    if (st & 0x100) m |= AW_MOD_LBUTTON;
    if (st & 0x200) m |= AW_MOD_MBUTTON;
    if (st & 0x400) m |= AW_MOD_RBUTTON;
    return m;
}

/* Windows virtual-key code for a keysym (unshifted level).  *ext: extended-key flag. */
static int vk_of(uint32_t ks, int *ext) {
    *ext = 0;
    if (ks >= 'a' && ks <= 'z') return (int)ks - 32;
    if ((ks >= 'A' && ks <= 'Z') || (ks >= '0' && ks <= '9')) return (int)ks;
    if (ks >= 0xffb0 && ks <= 0xffb9) return 0x60 + (int)(ks - 0xffb0);
    if (ks >= 0xffbe && ks <= 0xffd5) return 0x70 + (int)(ks - 0xffbe);
    switch (ks) {
    case ' ': case 0xff80: return 0x20;
    case ';': return 0xBA;
    case '=': return 0xBB;
    case ',': return 0xBC;
    case '-': return 0xBD;
    case '.': return 0xBE;
    case '/': return 0xBF;
    case '`': return 0xC0;
    case '[': return 0xDB;
    case '\\': return 0xDC;
    case ']': return 0xDD;
    case '\'': return 0xDE;
    case 0xff08: return 0x08;
    case 0xff09: case 0xfe20: return 0x09;
    case 0xff0d: return 0x0D;
    case 0xff8d: *ext = 1; return 0x0D;
    case 0xff1b: return 0x1B;
    case 0xff13: return 0x13;
    case 0xff14: return 0x91;
    case 0xff61: *ext = 1; return 0x2C;
    case 0xff7f: *ext = 1; return 0x90;
    case 0xffe5: return 0x14;
    case 0xffe1: case 0xffe2: return 0x10;
    case 0xffe3: return 0x11;
    case 0xffe4: *ext = 1; return 0x11;
    case 0xffe9: case 0xffe7: case 0xffe8: return 0x12;
    case 0xffea: *ext = 1; return 0x12;
    case 0xffeb: return 0x5B;
    case 0xffec: *ext = 1; return 0x5C;
    case 0xff67: *ext = 1; return 0x5D;
    case 0xff50: case 0xff95: *ext = 1; return 0x24;
    case 0xff51: case 0xff96: *ext = 1; return 0x25;
    case 0xff52: case 0xff97: *ext = 1; return 0x26;
    case 0xff53: case 0xff98: *ext = 1; return 0x27;
    case 0xff54: case 0xff99: *ext = 1; return 0x28;
    case 0xff55: case 0xff9a: *ext = 1; return 0x21;
    case 0xff56: case 0xff9b: *ext = 1; return 0x22;
    case 0xff57: case 0xff9c: *ext = 1; return 0x23;
    case 0xff9d: return 0x0C;
    case 0xff63: case 0xff9e: *ext = 1; return 0x2D;
    case 0xffff: case 0xff9f: *ext = 1; return 0x2E;
    case 0xffaa: return 0x6A;
    case 0xffab: return 0x6B;
    case 0xffac: return 0x6C;
    case 0xffad: return 0x6D;
    case 0xffae: return 0x6E;
    case 0xffaf: *ext = 1; return 0x6F;
    default: return 0;
    }
}

/* Keysym whose level (shift / AltGr / caps lock) gives the character for a key. */
static uint32_t text_keysym(unsigned kc, unsigned st) {
    int shift = (st & 0x01) != 0, lock = (st & 0x02) != 0;
    unsigned col = shift ? 1 : 0;
    if ((st & X.level3_mask) && X.kspk >= 3) col = shift ? 3 : 2;
    uint32_t ks = keysym_at(kc, col);
    if (!ks) ks = keysym_at(kc, col & 1);
    if (lock && ((ks >= 'a' && ks <= 'z') || (ks >= 'A' && ks <= 'Z'))) ks ^= 0x20;
    return ks;
}

static size_t utf8_encode(uint32_t cp, char *o) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xc0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* UTF-8 text a key press produces (control characters included), "" if none. */
static void key_text(uint32_t ks, unsigned st, char out[8]) {
    uint32_t cp = 0;
    out[0] = 0;
    if (ks == 0xff08) cp = '\b';
    else if (ks == 0xff09) cp = '\t';
    else if (ks == 0xff0d || ks == 0xff8d) cp = '\r';
    else if (ks == 0xff1b) cp = 0x1b;
    else if (ks == 0xff80) cp = ' ';
    else if (ks >= 0xffb0 && ks <= 0xffb9) cp = '0' + (ks - 0xffb0);
    else if (ks == 0xffaa) cp = '*';
    else if (ks == 0xffab) cp = '+';
    else if (ks == 0xffad) cp = '-';
    else if (ks == 0xffae) cp = '.';
    else if (ks == 0xffaf) cp = '/';
    else if (ks < 0x100) cp = ks >= 0x20 && ks != 0x7f ? ks : 0;
    else if (ks >= 0x01000000 && ks <= 0x0110ffff) cp = ks - 0x01000000;
    if (!cp) return;
    if ((st & 0x04) && ((cp >= 0x40 && cp <= 0x5f) || (cp >= 0x61 && cp <= 0x7a))) cp &= 0x1f;
    out[utf8_encode(cp, out)] = 0;
}

/* ======================================================================
 * Event queue
 * ====================================================================== */
static void q_push(const aw_plat_event *e, int xlate) {
    if (qcount && e->type == PEV_MOTION) {
        int last = (qhead + qcount - 1) % QCAP;
        if (qev[last].type == PEV_MOTION && qev[last].win == e->win) {
            qev[last] = *e;
            return;
        }
    }
    if (qcount == QCAP) return;
    int idx = (qhead + qcount) % QCAP;
    qev[idx] = *e;
    qxl[idx] = (uint8_t)xlate;
    qcount++;
}

static int q_pop(aw_plat_event *out) {
    if (!qcount) return 0;
    *out = qev[qhead];
    int xl = qxl[qhead];
    qhead = (qhead + 1) % QCAP;
    qcount--;
    if (xl) {
        /* configure events from a reparenting WM carry parent-relative coordinates */
        rq_begin(OP_TranslateCoordinates, 0);
        w32(out->win);
        w32(X.root);
        w16(0);
        w16(0);
        uint32_t seq = rq_end();
        size_t len = 0;
        uint8_t *r = xreply(seq, &len);
        if (r) {
            out->x = s16(r + 12);
            out->y = s16(r + 14);
            free(r);
        }
    }
    return 1;
}

static void ev_push_simple(int type, uint32_t win) {
    aw_plat_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.win = win;
    q_push(&e, 0);
}

/* ======================================================================
 * Event dispatch.  Must not issue blocking requests (it runs inside reply waits).
 * ====================================================================== */
static void key_event(const uint8_t *p, int press) {
    unsigned kc = p[1];
    unsigned st = u16(p + 28);
    aw_plat_event e;
    memset(&e, 0, sizeof e);
    e.win = u32(p + 12);
    e.time = u32(p + 4);
    e.x = s16(p + 24);
    e.y = s16(p + 26);
    e.root_x = s16(p + 20);
    e.root_y = s16(p + 22);
    e.mods = mods_of(st);
    int ext = 0;
    e.vk = vk_of(keysym_at(kc, 0), &ext);
    e.scancode = (int)(kc >= 8 ? kc - 8 : 0) | (ext ? 0x100 : 0);
    if (press) {
        e.type = PEV_KEYDOWN;
        key_text(text_keysym(kc, st), st, e.text);
        q_push(&e, 0);
        return;
    }
    /* Auto-repeat: a KeyRelease immediately followed by a KeyPress with the same time. */
    int repeat = 0;
    if (iblen - ibpos < 32) {
        struct pollfd pf = {X.fd, POLLIN, 0};
        if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) ib_fill();
    }
    if (iblen - ibpos >= 32) {
        const uint8_t *nx = ib + ibpos;
        if ((nx[0] & 0x7f) == EV_KeyPress && nx[1] == kc && u32(nx + 4) == e.time && pkt_ready()) {
            ibpos += 32;
            repeat = 1;
        }
    }
    if (repeat) {
        e.type = PEV_KEYDOWN;
        e.is_repeat = 1;
        key_text(text_keysym(kc, st), st, e.text);
    } else {
        e.type = PEV_KEYUP;
    }
    q_push(&e, 0);
}

static void button_event(const uint8_t *p, int press) {
    unsigned btn = p[1];
    aw_plat_event e;
    memset(&e, 0, sizeof e);
    e.win = u32(p + 12);
    e.time = u32(p + 4);
    e.x = s16(p + 24);
    e.y = s16(p + 26);
    e.root_x = s16(p + 20);
    e.root_y = s16(p + 22);
    e.mods = mods_of(u16(p + 28));
    if (btn >= 4 && btn <= 7) {
        if (!press) return;
        e.type = PEV_WHEEL;
        e.wheel_horizontal = btn >= 6;
        e.wheel_delta = (btn == 4 || btn == 7) ? 120 : -120;
        q_push(&e, 0);
        return;
    }
    if (btn == 8 || btn == 9) btn = btn - 4; /* X1 / X2 */
    else if (btn > 9 || btn == 0) return;
    e.type = press ? PEV_BUTTONDOWN : PEV_BUTTONUP;
    e.button = (int)btn;
    q_push(&e, 0);
}

static void dispatch(const uint8_t *p) {
    aw_plat_event e;
    switch (p[0] & 0x7f) {
    case EV_KeyPress:
        key_event(p, 1);
        break;
    case EV_KeyRelease:
        key_event(p, 0);
        break;
    case EV_ButtonPress:
        button_event(p, 1);
        break;
    case EV_ButtonRelease:
        button_event(p, 0);
        break;
    case EV_MotionNotify:
        memset(&e, 0, sizeof e);
        e.type = PEV_MOTION;
        e.win = u32(p + 12);
        e.time = u32(p + 4);
        e.x = s16(p + 24);
        e.y = s16(p + 26);
        e.root_x = s16(p + 20);
        e.root_y = s16(p + 22);
        e.mods = mods_of(u16(p + 28));
        q_push(&e, 0);
        break;
    case EV_EnterNotify:
    case EV_LeaveNotify:
        if (p[30] != 0) break; /* grab / ungrab crossings are not reported */
        memset(&e, 0, sizeof e);
        e.type = (p[0] & 0x7f) == EV_EnterNotify ? PEV_ENTER : PEV_LEAVE;
        e.win = u32(p + 12);
        e.time = u32(p + 4);
        e.x = s16(p + 24);
        e.y = s16(p + 26);
        e.root_x = s16(p + 20);
        e.root_y = s16(p + 22);
        e.mods = mods_of(u16(p + 28));
        q_push(&e, 0);
        break;
    case EV_FocusIn:
    case EV_FocusOut:
        if (p[8] != 0) break; /* NotifyNormal only */
        ev_push_simple((p[0] & 0x7f) == EV_FocusIn ? PEV_FOCUSIN : PEV_FOCUSOUT, u32(p + 4));
        break;
    case EV_Expose:
        memset(&e, 0, sizeof e);
        e.type = PEV_EXPOSE;
        e.win = u32(p + 4);
        e.x = (int)u16(p + 8);
        e.y = (int)u16(p + 10);
        e.w = (int)u16(p + 12);
        e.h = (int)u16(p + 14);
        q_push(&e, 0);
        break;
    case EV_MapNotify:
        ev_push_simple(PEV_MAP, u32(p + 8));
        break;
    case EV_UnmapNotify:
        ev_push_simple(PEV_UNMAP, u32(p + 8));
        break;
    case EV_ConfigureNotify: {
        struct xwin *w = win_find(u32(p + 8));
        int synthetic = (p[0] & 0x80) != 0;
        if (w) {
            w->w = (int)u16(p + 20);
            w->h = (int)u16(p + 22);
        }
        memset(&e, 0, sizeof e);
        e.type = PEV_CONFIGURE;
        e.win = u32(p + 8);
        e.x = s16(p + 16);
        e.y = s16(p + 18);
        e.w = (int)u16(p + 20);
        e.h = (int)u16(p + 22);
        q_push(&e, !synthetic);
        break;
    }
    case EV_SelectionClear:
        if (u32(p + 12) == A(A_CLIPBOARD)) {
            free(X.clip_text);
            X.clip_text = NULL;
        }
        break;
    case EV_SelectionRequest: {
        uint32_t requestor = u32(p + 12), selection = u32(p + 16), target = u32(p + 20);
        uint32_t prop = u32(p + 24), time = u32(p + 4);
        uint32_t out = 0;
        if (prop == 0) prop = target;
        if (selection == A(A_CLIPBOARD) && X.clip_text) {
            if (target == A(A_TARGETS)) {
                uint32_t t[3] = {A(A_TARGETS), A(A_UTF8_STRING), A(A_STRING)};
                prop_write(requestor, prop, A(A_ATOM), 32, t, 3);
                out = prop;
            } else if (target == A(A_UTF8_STRING)) {
                prop_write(requestor, prop, A(A_UTF8_STRING), 8, X.clip_text, (uint32_t)strlen(X.clip_text));
                out = prop;
            } else if (target == A(A_STRING)) {
                size_t n = 0;
                char *l1 = utf8_to_latin1(X.clip_text, &n);
                prop_write(requestor, prop, A(A_STRING), 8, l1, (uint32_t)n);
                free(l1);
                out = prop;
            }
        }
        rq_begin(OP_SendEvent, 0);
        w32(requestor);
        w32(0);
        w8(EV_SelectionNotify);
        w8(0);
        w16(0);
        w32(time);
        w32(requestor);
        w32(selection);
        w32(target);
        w32(out);
        w32(0);
        w32(0);
        rq_end();
        break;
    }
    case EV_SelectionNotify:
        X.sel_prop = u32(p + 20);
        X.sel_done = 1;
        break;
    case EV_ClientMessage:
#ifdef AW_X11_TEST_CLIENTMSG
        AW_X11_TEST_CLIENTMSG(p);
#endif
        if (p[1] == 32 && u32(p + 8) == A(A_WM_PROTOCOLS) && u32(p + 12) == A(A_WM_DELETE_WINDOW))
            ev_push_simple(PEV_CLOSE, u32(p + 4));
        break;
    case EV_MappingNotify:
        if (p[4] == 0 || p[4] == 1) X.kbd_dirty = 1;
        break;
    default:
        break;
    }
}

/* Process buffered packets, flush, wait for data or the wake pipe. */
static int wait_packets(int timeout_ms) {
    struct pollfd pf[2] = {{X.fd, POLLIN, 0}, {X.wake_r, POLLIN, 0}};
    int n = poll(pf, 2, timeout_ms);
    if (n <= 0) return 0;
    if (pf[1].revents & POLLIN) {
        char junk[64];
        while (read(X.wake_r, junk, sizeof junk) > 0) {
        }
        return 2;
    }
    if (pf[0].revents) ib_fill();
    return 1;
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Waits (pumping events into the queue) until *flag is set or ms elapse. */
static void wait_flag(volatile int *flag, int ms) {
    int64_t end = now_ms() + ms;
    for (;;) {
        drain_packets(); /* may queue replies (e.g. SelectionNotify): flush them before waiting */
        flush_out();
        if (*flag) break;
        int64_t rem = end - now_ms();
        if (rem <= 0) break;
        if (wait_packets((int)rem) == 0) break;
    }
}

/* ======================================================================
 * Connection setup
 * ====================================================================== */
static int connect_display(void) {
    const char *d = getenv("DISPLAY");
    if (!d || !*d) return -1;
    const char *colon = strrchr(d, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - d);
    if (hl != 0 && !(hl == 4 && !strncmp(d, "unix", 4))) return -1; /* TCP displays are out of scope */
    const char *q = colon + 1;
    if (!isdigit((unsigned char)*q)) return -1;
    int num = 0;
    while (isdigit((unsigned char)*q)) num = num * 10 + (*q++ - '0');
    X.display_num = num;
    X.screen_num = (*q == '.') ? atoi(q + 1) : 0;

    /* abstract namespace first, then the filesystem path */
    struct sockaddr_un sa;
    char name[64];
    int n = snprintf(name, sizeof name, "/tmp/.X11-unix/X%d", num);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path + 1, name, (size_t)n);
    if (connect(fd, (struct sockaddr *)&sa, offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n) == 0)
        return fd;
    close(fd);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, name, (size_t)n + 1);
    if (connect(fd, (struct sockaddr *)&sa, offsetof(struct sockaddr_un, sun_path) + (size_t)n + 1) == 0)
        return fd;
    close(fd);
    return -1;
}

static int file_read_all(const char *path, uint8_t **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t cap = 4096, n = 0;
    uint8_t *b = malloc(cap);
    while (b && !feof(f)) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) break;
        }
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    if (!b) return 0;
    *out = b;
    *len = n;
    return 1;
}

/* Finds an MIT-MAGIC-COOKIE-1 entry for this display in XAUTHORITY or ~/.Xauthority. */
static uint8_t *auth_lookup(size_t *datalen) {
    const char *path = getenv("XAUTHORITY");
    char def[512];
    if (!path || !*path) {
        const char *home = getenv("HOME");
        if (!home) return NULL;
        snprintf(def, sizeof def, "%s/.Xauthority", home);
        path = def;
    }
    uint8_t *buf;
    size_t len;
    if (!file_read_all(path, &buf, &len)) return NULL;
    char num[16];
    snprintf(num, sizeof num, "%d", X.display_num);
    uint8_t *found = NULL;
    size_t p = 0;
    while (p + 2 <= len) {
        unsigned family = (unsigned)(buf[p] << 8 | buf[p + 1]);
        p += 2;
        size_t fields[4];
        const uint8_t *vals[4];
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++) {
            if (p + 2 > len) {
                ok = 0;
                break;
            }
            size_t l = (size_t)(buf[p] << 8 | buf[p + 1]);
            p += 2;
            if (p + l > len) {
                ok = 0;
                break;
            }
            vals[i] = buf + p;
            fields[i] = l;
            p += l;
        }
        if (!ok) break;
        int fam_ok = family == 256 || family == 0xffff;
        int num_ok = fields[1] == 0 || (fields[1] == strlen(num) && !memcmp(vals[1], num, fields[1]));
        int name_ok = fields[2] == 18 && !memcmp(vals[2], "MIT-MAGIC-COOKIE-1", 18);
        if (fam_ok && num_ok && name_ok) {
            found = malloc(fields[3] + 4); /* room for the padding write */
            if (found) {
                memcpy(found, vals[3], fields[3]);
                *datalen = fields[3];
            }
            break;
        }
    }
    free(buf);
    return found;
}

static int handshake(int fd) {
    size_t datalen = 0;
    uint8_t *authdata = auth_lookup(&datalen);
    size_t namelen = authdata ? 18 : 0;
    uint8_t hd[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    hd[6] = (uint8_t)namelen;
    hd[7] = (uint8_t)(namelen >> 8);
    hd[8] = (uint8_t)datalen;
    hd[9] = (uint8_t)(datalen >> 8);
    write_all(fd, hd, sizeof hd);
    if (authdata) {
        static const char cookie[20] = "MIT-MAGIC-COOKIE-1";
        write_all(fd, cookie, 20);
        write_all(fd, authdata, datalen + ((4 - datalen % 4) & 3));
        free(authdata);
    }
    uint8_t h8[8];
    read_all(fd, h8, 8);
    if (h8[0] != 1) {
        size_t rl = h8[1] + ((4 - h8[1] % 4) & 3);
        uint8_t *reason = malloc(rl + 1);
        if (reason) {
            read_all(fd, reason, rl);
            reason[h8[1]] = 0;
            fprintf(stderr, "anywin: X server refused the connection: %s\n", (char *)reason);
            free(reason);
        }
        return -1;
    }
    size_t extra = (size_t)u16(h8 + 6) * 4;
    uint8_t *sb = malloc(8 + extra);
    if (!sb) return -1;
    memcpy(sb, h8, 8);
    read_all(fd, sb + 8, extra);
    int rc = -1;
    size_t len = 8 + extra;
    if (len < 40) goto out;
    X.rid_base = u32(sb + 12);
    X.rid_mask = u32(sb + 16);
    X.max_req = u16(sb + 26);
    unsigned nscr = sb[28], nfmt = sb[29];
    X.img_msb = sb[30] == 1;
    X.min_kc = sb[34];
    X.max_kc = sb[35];
    size_t off = 40 + ((u16(sb + 24) + 3) & ~3u);
    size_t fmt_off = off;
    off += 8 * (size_t)nfmt;
    if (off > len) goto out;
    int root_found = 0, vis_ok = 0;
    for (unsigned i = 0; i < nscr; i++) {
        if (off + 40 > len) goto out;
        uint32_t root = u32(sb + off), vis = u32(sb + off + 32);
        int rdepth = sb[off + 38], ndepth = sb[off + 39];
        int mine = (int)i == X.screen_num;
        if (mine) {
            X.root = root;
            X.root_visual = vis;
            X.root_w = u16(sb + off + 20);
            X.root_h = u16(sb + off + 22);
            X.root_depth = rdepth;
            root_found = 1;
        }
        size_t p = off + 40;
        for (int d = 0; d < ndepth; d++) {
            if (p + 8 > len) goto out;
            unsigned nv = (unsigned)u16(sb + p + 2);
            p += 8;
            for (unsigned v = 0; v < nv; v++) {
                if (p + 24 > len) goto out;
                if (mine && u32(sb + p) == vis)
                    vis_ok = sb[p + 4] == 4 && u32(sb + p + 8) == 0xff0000 && u32(sb + p + 12) == 0xff00 &&
                             u32(sb + p + 16) == 0xff;
                p += 24;
            }
        }
        off = p;
    }
    if (!root_found || !vis_ok) goto out;
    /* the root depth must have a 32 bpp pixel format */
    int fmt_ok = 0;
    for (unsigned i = 0; i < nfmt; i++) {
        const uint8_t *f = sb + fmt_off + 8 * i;
        if (f[0] == X.root_depth && f[1] == 32) fmt_ok = 1;
    }
    if (!fmt_ok) goto out;
    rc = 0;
out:
    free(sb);
    return rc;
}

static void refresh_wm_presence(void) {
    rq_begin(OP_GetProperty, 0);
    w32(X.root);
    w32(A(A_NET_SUPPORTING_WM_CHECK));
    w32(0);
    w32(0);
    w32(1);
    uint32_t seq = rq_end();
    uint8_t *r = xreply(seq, NULL);
    X.have_wm = r && r[1] == 32 && u32(r + 16) >= 1;
    free(r);
}

static void intern_all(void) {
    for (int i = 0; i < A_COUNT; i++) X.atoms[i] = intern(atom_names[i], 0);
}

static int x_setup(void) {
    int fd = connect_display();
    if (fd < 0) return 0;
    X.fd = fd;
    if (handshake(fd) != 0) {
        close(fd);
        return 0;
    }
    int wp[2];
    if (pipe2(wp, O_CLOEXEC | O_NONBLOCK) != 0) return 0;
    X.wake_r = wp[0];
    X.wake_w = wp[1];
    intern_all();
    X.gc = rid_alloc();
    rq_begin(OP_CreateGC, 0);
    w32(X.gc);
    w32(X.root);
    w32(0);
    rq_end();
    refresh_keyboard();
    refresh_wm_presence();
    return 1;
}

/* Lazily creates the input-only window used as selection owner / requestor. */
static void ensure_clip_win(void) {
    if (X.clip_win) return;
    X.clip_win = rid_alloc();
    rq_begin(OP_CreateWindow, 0);
    w32(X.clip_win);
    w32(X.root);
    w16(0);
    w16(0);
    w16(1);
    w16(1);
    w16(0);
    w16(2); /* InputOnly */
    w32(0);
    w32(0);
    rq_end();
}

/* ======================================================================
 * gui.h: platform API
 * ====================================================================== */
int plat_init(void) {
    for (;;) {
        int st = __atomic_load_n(&X.state, __ATOMIC_ACQUIRE);
        if (st == 2) return 1;
        if (st == 3) return 0;
        if (st == 0 && __sync_bool_compare_and_swap(&X.state, 0, 1)) {
            int ok = x_setup();
            __atomic_store_n(&X.state, ok ? 2 : 3, __ATOMIC_RELEASE);
            return ok;
        }
        if (st == 1) {
            struct timespec t = {0, 1000000};
            nanosleep(&t, NULL);
        }
    }
}

int plat_fd(void) { return X.state == 2 ? X.fd : -1; }

uint32_t plat_window_create(int x, int y, int w, int h, int kind, uint32_t owner) {
    if (!plat_init()) return 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    uint32_t id = rid_alloc();
    int popup = kind == PLAT_WIN_POPUP;
    uint32_t mask = MASK_Exposure | MASK_StructureNotify | MASK_KeyPress | MASK_KeyRelease |
                    MASK_ButtonPress | MASK_ButtonRelease | MASK_PointerMotion | MASK_EnterWindow |
                    MASK_LeaveWindow | MASK_FocusChange;
    uint32_t vmask = CW_BackPixmap | CW_EventMask | (popup ? CW_OverrideRedirect : 0);
    rq_begin(OP_CreateWindow, 0);
    w32(id);
    w32(X.root);
    w16((uint32_t)(int16_t)x);
    w16((uint32_t)(int16_t)y);
    w16((uint32_t)w);
    w16((uint32_t)h);
    w16(0);
    w16(1); /* InputOutput */
    w32(0); /* CopyFromParent visual */
    w32(vmask);
    w32(0);                       /* background None: the app paints on expose */
    if (popup) w32(1);            /* override-redirect */
    w32(mask);
    rq_end();

    struct xwin *rec = win_add(id, kind);
    rec->w = w;
    rec->h = h;
    rec->x = x;
    rec->y = y;
    if (popup) return id;

    static const char cls[] = "anywin\0anywin";
    prop_write(id, A(A_WM_CLASS), A(A_STRING), 8, cls, sizeof cls - 1);
    uint32_t proto = A(A_WM_DELETE_WINDOW);
    prop_write(id, A(A_WM_PROTOCOLS), A(A_ATOM), 32, &proto, 1);
    uint32_t pid = (uint32_t)getpid();
    prop_set_cardinal(id, A(A_NET_WM_PID), &pid, 1);
    uint32_t wtype = A(kind == PLAT_WIN_DIALOG ? A_NET_WM_WINDOW_TYPE_DIALOG : A_NET_WM_WINDOW_TYPE_NORMAL);
    prop_write(id, A(A_NET_WM_WINDOW_TYPE), A(A_ATOM), 32, &wtype, 1);
    if (kind == PLAT_WIN_DIALOG && owner) {
        uint32_t o = owner;
        prop_write(id, A(A_WM_TRANSIENT_FOR), A(A_WINDOW), 32, &o, 1);
    }
    uint32_t hints[9] = {3, 0, 1, 0, 0, 0, 0, 0, 0}; /* InputHint|StateHint, input, NormalState */
    prop_write(id, A(A_WM_HINTS), A(A_WM_HINTS), 32, hints, 9);
    write_size_hints(id, x, y, w, h, 1);
    return id;
}

void plat_window_destroy(uint32_t win) {
    if (X.state != 2 || !win) return;
    for (int i = 0; i < X.ntray; i++)
        if (X.tray[i].win == win) {
            free(X.tray[i].bg);
            X.tray[i] = X.tray[--X.ntray];
            memset(&X.tray[X.ntray], 0, sizeof X.tray[0]);
            break;
        }
    win_remove(win);
    rq_begin(OP_DestroyWindow, 0);
    w32(win);
    rq_end();
}

void plat_window_show(uint32_t win, int show) {
    if (X.state != 2 || !win) return;
    struct xwin *w = win_find(win);
    if (w) w->mapped = show != 0;
    rq_begin(show ? OP_MapWindow : OP_UnmapWindow, 0);
    w32(win);
    rq_end();
}

/* Screen coordinates.  A managed, mapped window under a WM moves through _NET_MOVERESIZE_WINDOW
 * (static gravity: x/y are the client area's top-left); otherwise ConfigureWindow directly. */
void plat_window_move(uint32_t win, int x, int y, int w, int h) {
    if (X.state != 2 || !win) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    struct xwin *rec = win_find(win);
    if (rec) {
        rec->x = x;
        rec->y = y;
        rec->w = w;
        rec->h = h;
    }
    if (X.have_wm && rec && rec->mapped && rec->kind != PLAT_WIN_POPUP) {
        uint32_t d[5] = {10 | 0xf00, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h};
        send_cm(X.root, MASK_SubstructureRedirect | MASK_SubstructureNotify, win, A(A_NET_MOVERESIZE_WINDOW), d);
        return;
    }
    rq_begin(OP_ConfigureWindow, 0);
    w32(win);
    w16(CFG_X | CFG_Y | CFG_W | CFG_H);
    w16(0);
    w32((uint32_t)x);
    w32((uint32_t)y);
    w32((uint32_t)w);
    w32((uint32_t)h);
    rq_end();
}

void plat_window_set_title(uint32_t win, const char *utf8) {
    if (X.state != 2 || !win) return;
    if (!utf8) utf8 = "";
    prop_write(win, A(A_NET_WM_NAME), A(A_UTF8_STRING), 8, utf8, (uint32_t)strlen(utf8));
    size_t n = 0;
    char *l1 = utf8_to_latin1(utf8, &n);
    prop_write(win, A(A_WM_NAME), A(A_STRING), 8, l1, (uint32_t)n);
    free(l1);
}

void plat_window_set_icon(uint32_t win, const uint32_t *argb, int w, int h) {
    if (X.state != 2 || !win) return;
    if (!argb || w <= 0 || h <= 0) {
        rq_begin(OP_DeleteProperty, 0);
        w32(win);
        w32(A(A_NET_WM_ICON));
        rq_end();
        return;
    }
    size_t n = (size_t)w * (size_t)h;
    uint32_t *v = malloc((n + 2) * 4);
    if (!v) x_fatal("out of memory");
    v[0] = (uint32_t)w;
    v[1] = (uint32_t)h;
    memcpy(v + 2, argb, n * 4);
    prop_write(win, A(A_NET_WM_ICON), A(A_CARDINAL), 32, v, (uint32_t)(n + 2));
    free(v);
}

void plat_window_set_resizable(uint32_t win, int resizable) {
    if (X.state != 2 || !win) return;
    struct xwin *r = win_find(win);
    if (!r) return;
    r->resizable = resizable != 0;
    write_size_hints(win, r->x, r->y, r->w, r->h, r->resizable);
}

void plat_window_activate(uint32_t win) {
    if (X.state != 2 || !win) return;
    if (X.have_wm) {
        uint32_t d[5] = {1, 0, 0, 0, 0}; /* source: application; no timestamp; no active window */
        send_cm(X.root, MASK_SubstructureRedirect | MASK_SubstructureNotify, win, A(A_NET_ACTIVE_WINDOW), d);
    }
    rq_begin(OP_ConfigureWindow, 0);
    w32(win);
    w16(CFG_StackMode);
    w16(0);
    w32(0); /* Above */
    rq_end();
    rq_begin(OP_SetInputFocus, 1); /* RevertToPointerRoot */
    w32(win);
    w32(0);
    rq_end();
}

void plat_window_minimize(uint32_t win) {
    if (X.state != 2 || !win || !X.have_wm) return;
    uint32_t d[5] = {3, 0, 0, 0, 0}; /* IconicState */
    send_cm(X.root, MASK_SubstructureRedirect | MASK_SubstructureNotify, win, A(A_WM_CHANGE_STATE), d);
}

void plat_window_maximize(uint32_t win, int on) {
    if (X.state != 2 || !win || !X.have_wm) return;
    uint32_t d[5] = {on ? 1u : 0u, A(A_NET_WM_STATE_MAXIMIZED_VERT), A(A_NET_WM_STATE_MAXIMIZED_HORZ), 1, 0};
    send_cm(X.root, MASK_SubstructureRedirect | MASK_SubstructureNotify, win, A(A_NET_WM_STATE), d);
}

void plat_window_get_pos(uint32_t win, int *x, int *y) {
    struct xwin *r = win_find(win);
    int rx = r ? r->x : 0, ry = r ? r->y : 0;
    if (X.state == 2 && win) {
        rq_begin(OP_TranslateCoordinates, 0);
        w32(win);
        w32(X.root);
        w16(0);
        w16(0);
        uint32_t seq = rq_end();
        uint8_t *rp = xreply(seq, NULL);
        if (rp) {
            rx = s16(rp + 12);
            ry = s16(rp + 14);
            free(rp);
        }
    }
    *x = rx;
    *y = ry;
}

static void put_rect(uint32_t win, int x, int y, int w, int h, const uint32_t *base, ptrdiff_t stride);

/* Uploads one ZPixmap block; pixels are 0x00RRGGBB, written in the server's image byte order. */
static void put_block(uint32_t win, int x, int y, int w, int h, const uint32_t *base, ptrdiff_t stride) {
    rq_begin(OP_PutImage, 2); /* ZPixmap */
    w32(win);
    w32(X.gc);
    w16((uint32_t)w);
    w16((uint32_t)h);
    w16((uint32_t)x);
    w16((uint32_t)y);
    w8(0);
    w8((uint32_t)X.root_depth);
    w16(0);
    for (int r = 0; r < h; r++) {
        const uint32_t *row = base + (ptrdiff_t)r * stride;
        for (int i = 0; i < w; i++) {
            uint32_t v = row[i];
            if (X.img_msb) {
                w8(v >> 24);
                w8(v >> 16);
                w8(v >> 8);
                w8(v);
            } else {
                w32(v);
            }
        }
    }
    rq_end();
}

static void put_rect(uint32_t win, int x, int y, int w, int h, const uint32_t *base, ptrdiff_t stride) {
    size_t maxb = (size_t)X.max_req * 4;
    size_t rowb = (size_t)w * 4;
    if (w <= 0 || h <= 0) return;
    if (rowb + 24 <= maxb) {
        int rows = (int)((maxb - 24) / rowb);
        for (int yy = 0; yy < h; yy += rows) {
            int hh = h - yy < rows ? h - yy : rows;
            put_block(win, x, y + yy, w, hh, base + (ptrdiff_t)yy * stride, stride);
        }
    } else {
        int cols = (int)((maxb - 24) / 4);
        if (cols < 1) return;
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx += cols) {
                int ww = w - xx < cols ? w - xx : cols;
                put_block(win, x + xx, y + yy, ww, 1, base + (ptrdiff_t)yy * stride + xx, stride);
            }
    }
}

void plat_put(uint32_t win, const aw_surface *s, const RECT *r) {
    if (X.state != 2 || !win || !s) return;
    int x0 = 0, y0 = 0, x1 = s->w, y1 = s->h;
    if (r) {
        if (r->left > x0) x0 = (int)r->left;
        if (r->top > y0) y0 = (int)r->top;
        if (r->right < x1) x1 = (int)r->right;
        if (r->bottom < y1) y1 = (int)r->bottom;
    }
    if (x0 >= x1 || y0 >= y1) return;
    put_rect(win, x0, y0, x1 - x0, y1 - y0, surface_row(s, y0) + x0, s->stride);
}

void plat_flush(void) {
    if (X.state == 2) flush_out();
}

int plat_poll(aw_plat_event *ev, int timeout_ms) {
    if (X.state != 2) return 0;
    int64_t end = timeout_ms < 0 ? -1 : now_ms() + timeout_ms;
    for (;;) {
        if (q_pop(ev)) return 1;
        if (X.kbd_dirty) refresh_keyboard();
        if (drain_packets()) continue;
        int rem = -1;
        if (end >= 0) {
            int64_t r = end - now_ms();
            rem = r > 0 ? (int)r : 0;
        }
        flush_out(); /* requests must reach the server before we block for events */
        int w = wait_packets(rem);
        if (w == 2) return 0;
        if (w == 0) return q_pop(ev);
    }
}

void plat_wakeup(void) {
    if (X.state != 2) return;
    char c = 1;
    ssize_t r = write(X.wake_w, &c, 1);
    (void)r;
}

void plat_cursor_pos(int *x, int *y) {
    *x = 0;
    *y = 0;
    if (X.state != 2) return;
    rq_begin(OP_QueryPointer, 0);
    w32(X.root);
    uint32_t seq = rq_end();
    uint8_t *r = xreply(seq, NULL);
    if (r) {
        *x = s16(r + 16);
        *y = s16(r + 18);
        free(r);
    }
}

void plat_screen_size(int *w, int *h) {
    *w = X.state == 2 ? (int)X.root_w : 0;
    *h = X.state == 2 ? (int)X.root_h : 0;
}

/* X "cursor" font glyph for each IDC_* value (the classic core cursors). */
static uint32_t cursor_glyph(int idc) {
    switch (idc) {
    case 32512: return 68;   /* IDC_ARROW   left_ptr */
    case 32513: return 152;  /* IDC_IBEAM   xterm */
    case 32514: return 150;  /* IDC_WAIT    watch */
    case 32515: return 34;   /* IDC_CROSS   crosshair */
    case 32516: return 114;  /* IDC_UPARROW sb_up_arrow */
    case 32640: return 52;   /* IDC_SIZE    fleur */
    case 32641: return 68;   /* IDC_ICON */
    case 32642: return 134;  /* IDC_SIZENWSE top_left_corner */
    case 32643: return 136;  /* IDC_SIZENESW top_right_corner */
    case 32644: return 108;  /* IDC_SIZEWE sb_h_double_arrow */
    case 32645: return 116;  /* IDC_SIZENS sb_v_double_arrow */
    case 32646: return 52;   /* IDC_SIZEALL fleur */
    case 32648: return 0;    /* IDC_NO      X_cursor */
    case 32649: return 60;   /* IDC_HAND    hand2 */
    case 32650: return 150;  /* IDC_APPSTARTING watch */
    case 32651: return 92;   /* IDC_HELP    question_arrow */
    default: return 68;
    }
}

static uint32_t cursor_for(int idc) {
    for (int i = 0; i < X.ncursors; i++)
        if (X.cursors[i].idc == idc) return X.cursors[i].cid;
    if (!X.cursor_font) {
        X.cursor_font = rid_alloc();
        rq_begin(OP_OpenFont, 0);
        w32(X.cursor_font);
        w16(6);
        w16(0);
        wbytes("cursor", 6);
        rq_end();
    }
    uint32_t g = cursor_glyph(idc);
    uint32_t cid = rid_alloc();
    rq_begin(OP_CreateGlyphCursor, 0);
    w32(cid);
    w32(X.cursor_font);
    w32(X.cursor_font);
    w16(g);
    w16(g + 1);
    w16(0);
    w16(0);
    w16(0);
    w16(0xffff);
    w16(0xffff);
    w16(0xffff);
    rq_end();
    if (X.ncursors < (int)(sizeof X.cursors / sizeof X.cursors[0])) {
        X.cursors[X.ncursors].idc = idc;
        X.cursors[X.ncursors].cid = cid;
        X.ncursors++;
    }
    return cid;
}

void plat_set_cursor(uint32_t win, int idc) {
    if (X.state != 2 || !win) return;
    uint32_t cid = idc ? cursor_for(idc) : 0;
    rq_begin(OP_ChangeWindowAttributes, 0);
    w32(win);
    w32(CW_Cursor);
    w32(cid);
    rq_end();
}

int plat_grab(uint32_t win, int on) {
    if (X.state != 2) return 0;
    if (!on) {
        rq_begin(OP_UngrabPointer, 0);
        w32(0);
        rq_end();
        rq_begin(OP_UngrabKeyboard, 0);
        w32(0);
        rq_end();
        return 1;
    }
    rq_begin(OP_GrabPointer, 1); /* owner_events */
    w32(win);
    w16(MASK_ButtonPress | MASK_ButtonRelease | MASK_PointerMotion | MASK_EnterWindow | MASK_LeaveWindow);
    w8(1);
    w8(1);
    w32(0);
    w32(0);
    w32(0);
    uint32_t seq = rq_end();
    uint8_t *r = xreply(seq, NULL);
    int ok = r && r[1] == 0;
    free(r);
    if (!ok) return 0;
    rq_begin(OP_GrabKeyboard, 1);
    w32(win);
    w32(0);
    w8(1);
    w8(1);
    w16(0);
    seq = rq_end();
    r = xreply(seq, NULL);
    ok = r && r[1] == 0;
    free(r);
    return ok;
}

void plat_clipboard_set(const char *utf8) {
    if (X.state != 2) return;
    free(X.clip_text);
    X.clip_text = utf8 ? strdup(utf8) : NULL;
    if (X.clip_text) ensure_clip_win();
    rq_begin(OP_SetSelectionOwner, 0);
    w32(X.clip_text ? X.clip_win : 0);
    w32(A(A_CLIPBOARD));
    w32(0);
    rq_end();
}

/* ConvertSelection to our own property, then read it.  Used when another client owns CLIPBOARD. */
static char *clipboard_fetch_remote(void) {
    ensure_clip_win();
    X.sel_done = 0;
    X.sel_prop = 0;
    rq_begin(OP_ConvertSelection, 0);
    w32(X.clip_win);
    w32(A(A_CLIPBOARD));
    w32(A(A_UTF8_STRING));
    w32(A(A_ANYWIN_SEL));
    w32(0);
    rq_end();
    wait_flag(&X.sel_done, 1000);
    if (!X.sel_done || !X.sel_prop) return NULL;
    rq_begin(OP_GetProperty, 1); /* delete */
    w32(X.clip_win);
    w32(X.sel_prop);
    w32(0);
    w32(0);
    w32(0x100000);
    uint32_t seq = rq_end();
    size_t len = 0;
    uint8_t *r = xreply(seq, &len);
    if (!r) return NULL;
    if (u32(r + 8) == intern("INCR", 1)) { /* incremental transfer: out of scope (3.8) */
        free(r);
        return NULL;
    }
    size_t n = (size_t)u32(r + 16);
    if (r[1] != 8 || 32 + n > len) n = r[1] == 8 && len > 32 ? len - 32 : 0;
    char *out = malloc(n + 1);
    if (out) {
        memcpy(out, r + 32, n);
        out[n] = 0;
    }
    free(r);
    return out;
}

char *plat_clipboard_get(void) {
    if (X.state != 2) return NULL;
    if (X.clip_text) return strdup(X.clip_text);
    return clipboard_fetch_remote();
}

/* ---- system tray (XEmbed dock, opaque icon window) ---- */
static int tray_index(uint32_t win) {
    for (int i = 0; i < X.ntray; i++)
        if (X.tray[i].win == win) return i;
    return -1;
}

uint32_t plat_tray_add(int *icon_size) {
    if (!plat_init() || X.ntray == MAX_TRAY) return 0;
    char name[48];
    snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", X.screen_num);
    uint32_t sel = intern(name, 1);
    if (!sel) return 0;
    rq_begin(OP_GetSelectionOwner, 0);
    w32(sel);
    uint32_t seq = rq_end();
    uint8_t *r = xreply(seq, NULL);
    uint32_t owner = r ? u32(r + 8) : 0;
    free(r);
    if (!owner) return 0;

    const int sz = 22;
    uint32_t icon = rid_alloc();
    uint32_t mask = MASK_Exposure | MASK_StructureNotify | MASK_ButtonPress | MASK_ButtonRelease |
                    MASK_PointerMotion | MASK_EnterWindow | MASK_LeaveWindow;
    rq_begin(OP_CreateWindow, 0);
    w32(icon);
    w32(X.root);
    w16(0);
    w16(0);
    w16((uint32_t)sz);
    w16((uint32_t)sz);
    w16(0);
    w16(1);
    w32(0);
    w32(CW_BackPixmap | CW_EventMask);
    w32(1); /* ParentRelative: the tray's background shows through until we draw */
    w32(mask);
    rq_end();
    uint32_t xe[2] = {0, 1}; /* XEMBED version 0, XEMBED_MAPPED */
    prop_write(icon, A(A_XEMBED_INFO), A(A_XEMBED_INFO), 32, xe, 2);
    uint32_t d[5] = {0, 0, icon, 0, 0}; /* CurrentTime, SYSTEM_TRAY_REQUEST_DOCK, icon */
    send_cm(owner, 0, owner, A(A_NET_SYSTEM_TRAY_OPCODE), d);
    /* XEmbed: the embedder reparents and maps the icon (XEMBED_MAPPED in _XEMBED_INFO).  Mapping
     * a root child ourselves makes the window manager manage it as a regular top-level window,
     * which later breaks its window stack when the tray steals the window. */

    struct xwin *rec = win_add(icon, PLAT_WIN_POPUP);
    rec->w = sz;
    rec->h = sz;
    rec->mapped = 0;
    X.tray[X.ntray].win = icon;
    X.tray[X.ntray].size = sz;
    X.tray[X.ntray].bg = NULL;
    X.ntray++;
    if (icon_size) *icon_size = sz;
    return icon;
}

void plat_tray_remove(uint32_t win) { plat_window_destroy(win); }

/* Composites the ARGB icon over the tray background (sampled once from the icon window, which
 * shows the tray's ParentRelative background before the first draw) and scales it to the icon
 * size with area averaging. */
void plat_tray_draw(uint32_t win, const uint32_t *argb, int w, int h) {
    if (X.state != 2 || !argb || w <= 0 || h <= 0) return;
    int ti = tray_index(win);
    if (ti < 0) return;
    int dw = X.tray[ti].size, dh = X.tray[ti].size;
    size_t npx = (size_t)dw * (size_t)dh;
    uint32_t *bg = X.tray[ti].bg;
    uint32_t *tmp = NULL;
    if (!bg) {
        uint32_t *got = malloc(npx * 4);
        if (!got) return;
        rq_begin(OP_GetImage, 2);
        w32(win);
        w16(0);
        w16(0);
        w16((uint32_t)dw);
        w16((uint32_t)dh);
        w32(0xffffffffu);
        uint32_t seq = rq_end();
        size_t len = 0;
        uint8_t *r = xreply(seq, &len);
        if (r && len >= 32 + npx * 4) {
            for (size_t i = 0; i < npx; i++) {
                uint32_t v = u32(r + 32 + i * 4);
                got[i] = X.img_msb ? __builtin_bswap32(v) : v;
            }
            X.tray[ti].bg = bg = got;
        } else {
            free(got);
            bg = NULL;
        }
        free(r);
        if (!bg) {
            tmp = malloc(npx * 4);
            if (!tmp) return;
            for (size_t i = 0; i < npx; i++) tmp[i] = 0x00c0c0c0u;
            bg = tmp;
        }
    }
    uint32_t *out = malloc(npx * 4);
    if (!out) {
        free(tmp);
        return;
    }
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = dy * h / dh, sy1 = (dy + 1) * h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = dx * w / dw, sx1 = (dx + 1) * w / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            uint64_t sa = 0, sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    uint32_t p = argb[(size_t)sy * w + sx];
                    uint64_t a = p >> 24;
                    sa += a;
                    sr += a * ((p >> 16) & 0xff);
                    sg += a * ((p >> 8) & 0xff);
                    sb += a * (p & 0xff);
                    n++;
                }
            uint32_t b = bg[(size_t)dy * dw + dx];
            uint32_t alpha = (uint32_t)(sa / (uint64_t)n);
            uint32_t cr = 0, cg = 0, cb = 0;
            if (sa) {
                cr = (uint32_t)(sr / sa);
                cg = (uint32_t)(sg / sa);
                cb = (uint32_t)(sb / sa);
            }
            uint32_t br = (b >> 16) & 0xff, bgc = (b >> 8) & 0xff, bb = b & 0xff;
            uint32_t orr = (cr * alpha + br * (255 - alpha) + 127) / 255;
            uint32_t og = (cg * alpha + bgc * (255 - alpha) + 127) / 255;
            uint32_t ob2 = (cb * alpha + bb * (255 - alpha) + 127) / 255;
            out[(size_t)dy * dw + dx] = (orr << 16) | (og << 8) | ob2;
        }
    }
    put_rect(win, 0, 0, dw, dh, out, dw);
    free(out);
    free(tmp);
}

void plat_bell(void) {
    if (X.state != 2) return;
    rq_begin(OP_Bell, 0);
    rq_end();
}
