/* White-box XEmbed test (tests/x11_font.sh).  This one process plays the application and the
 * system tray manager: it owns _NET_SYSTEM_TRAY_S0, reparents the docked icon into its own window
 * when the dock request arrives, and the shell test checks the composited pixels on screen.
 * Compiled with the runtime's x11.c included so the manager side can use the internal requests. */
#include "../runtime/gui.h" /* first: defines _GNU_SOURCE for x11.c's system headers */
#include <stdarg.h>
#include <time.h>

static void tray_hook(const uint8_t *p);
#define AW_X11_TEST_CLIENTMSG(p) tray_hook(p)
#include "../runtime/x11.c"

int aw_debug;
void aw_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint32_t mgr_win, dock_icon, icon_exposed;

/* manager side of the dock: SYSTEM_TRAY_REQUEST_DOCK (opcode 0) carries the icon in data[2] */
static void tray_hook(const uint8_t *p) {
    if (u32(p + 8) != A(A_NET_SYSTEM_TRAY_OPCODE)) return;
    if (u32(p + 16) != 0) return;
    dock_icon = u32(p + 20);
    printf("DOCK_REQUEST icon=%u\n", dock_icon);
    rq_begin(OP_ReparentWindow, 0);
    w32(dock_icon);
    w32(mgr_win);
    w16(0);
    w16(0);
    rq_end();
    rq_begin(OP_MapWindow, 0);
    w32(dock_icon);
    rq_end();
}

#define MW 120
#define MH 80
static uint32_t bg_px[MW * MH];

int main(void) {
    if (!plat_init()) {
        printf("RESULT FAIL no display\n");
        return 1;
    }
    mgr_win = plat_window_create(300, 200, MW, MH, PLAT_WIN_NORMAL, 0);
    plat_window_show(mgr_win, 1);
    aw_surface bg = {bg_px, MW, MH, MW, 0, {0}, 0};
    for (int i = 0; i < MW * MH; i++) bg_px[i] = 0x00102030;
    plat_flush();

    /* wait for the manager window to be exposed, then paint its background */
    int64_t end = now_ms() + 5000;
    aw_plat_event ev;
    int exposed = 0;
    while (!exposed && now_ms() < end) {
        if (plat_poll(&ev, 100) && ev.type == PEV_EXPOSE && ev.win == mgr_win) {
            plat_put(mgr_win, &bg, NULL);
            plat_flush();
            exposed = 1;
        }
    }

    /* claim the tray selection on our own window */
    uint32_t sel = intern("_NET_SYSTEM_TRAY_S0", 0);
    rq_begin(OP_SetSelectionOwner, 0);
    w32(mgr_win);
    w32(sel);
    w32(0);
    rq_end();
    plat_flush();

    /* CLIPBOARD round trip through the selection protocol (owner is this client, so the
     * request is answered by the same connection): ConvertSelection, SelectionRequest, property */
    plat_clipboard_set("remote \xc3\xa9");
    char *rc = clipboard_fetch_remote();
    printf("CLIP_REMOTE [%s]\n", rc ? rc : "(null)");
    free(rc);

    int size = 0;
    uint32_t icon = plat_tray_add(&size);
    printf("TRAY icon=%u size=%d\n", icon, size);
    if (!icon) {
        printf("RESULT FAIL no icon\n");
        return 1;
    }

    /* the dock request is answered by tray_hook(); wait for the icon's first expose */
    end = now_ms() + 5000;
    while (!icon_exposed && now_ms() < end) {
        if (plat_poll(&ev, 100) && ev.type == PEV_EXPOSE && ev.win == icon) icon_exposed = 1;
    }
    printf("DOCKED %s\n", dock_icon == icon && icon_exposed ? "OK" : "MISSING");

    /* 16x16 source: opaque red centre, fully transparent border */
    static uint32_t argb[16 * 16];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            argb[y * 16 + x] = (x >= 4 && x < 12 && y >= 4 && y < 12) ? 0xffff0000u : 0x00000000u;
    plat_tray_draw(icon, argb, 16, 16);
    plat_flush();
    printf("READY %u\n", icon);
    fflush(stdout);

    /* keep the icon alive for the screenshot; redraw on every expose of the icon */
    end = now_ms() + 6000;
    while (now_ms() < end) {
        if (plat_poll(&ev, 100) && ev.type == PEV_EXPOSE && ev.win == icon) {
            plat_tray_draw(icon, argb, 16, 16);
            plat_flush();
        }
    }
    plat_tray_remove(icon);
    plat_window_destroy(mgr_win);
    plat_flush();
    int ok = exposed && dock_icon == icon && icon_exposed;
    printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
