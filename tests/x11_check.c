/* Native X11 harness: drives the public plat_* API on a private Xvfb (see tests/x11_font.sh).
 * Prints READY <xid> once the window is exposed, then one EV line per event until Escape.
 * The shell test injects input with xdotool and takes a screenshot in the meantime. */
#include <stdarg.h>
#include <time.h>

#include "gui.h"

int aw_debug;
void aw_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define W 200
#define H 120
/* The surface is larger than the window: PutImage is split into several requests by the client
 * and clipped by the server.  Pixels are a gradient (R = x, G = y, B = 0x80) plus a green square. */
#define SW 1000
#define SH 400
static uint32_t px[SW * SH];

static void paint(aw_surface *s) {
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            px[y * SW + x] = ((uint32_t)(x & 0xff) << 16) | ((uint32_t)(y & 0xff) << 8) | 0x80;
    for (int y = 10; y < 30; y++)
        for (int x = 10; x < 30; x++) px[y * SW + x] = 0x0000ff00;
    s->px = px;
    s->w = SW;
    s->h = SH;
    s->stride = SW;
}

static const char *evname(int t) {
    static const char *const names[] = {"NONE",   "EXPOSE",  "CONFIGURE", "CLOSE",     "KEYDOWN",
                                        "KEYUP",  "BUTTONDOWN", "BUTTONUP", "MOTION",   "WHEEL",
                                        "ENTER",  "LEAVE",   "FOCUSIN",   "FOCUSOUT",  "MAP",
                                        "UNMAP"};
    return t >= 0 && t < 16 ? names[t] : "?";
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void) {
    if (!plat_init()) {
        printf("RESULT FAIL no display\n");
        return 1;
    }
    uint32_t win = plat_window_create(100, 100, W, H, PLAT_WIN_NORMAL, 0);
    plat_window_set_title(win, "anywin x11 test");
    plat_window_show(win, 1);
    aw_surface s;
    paint(&s);
    plat_flush();

    int exposed = 0, quit = 0, keys = 0, buttons = 0, motions = 0, wheels = 0, configs = 0, maps = 0;
    int64_t deadline = now_ms() + 20000;
    int announced = 0;
    aw_plat_event ev;
    while (!quit && now_ms() < deadline) {
        if (!plat_poll(&ev, 100)) continue;
        if (ev.type == PEV_EXPOSE) {
            printf("EV EXPOSE x=%d y=%d w=%d h=%d\n", ev.x, ev.y, ev.w, ev.h);
            plat_put(win, &s, NULL);
            plat_flush();
            if (!announced) {
                printf("READY %u\n", win);
                fflush(stdout);
                announced = 1;
            }
            exposed = 1;
        }
        switch (ev.type) {
        case PEV_KEYDOWN:
        case PEV_KEYUP:
            printf("EV %s vk=0x%x scan=0x%x text=[%s] rep=%d mods=0x%x\n", evname(ev.type), ev.vk, ev.scancode,
                   ev.text, ev.is_repeat, ev.mods);
            if (ev.type == PEV_KEYDOWN) keys++;
            if (ev.type == PEV_KEYDOWN && ev.vk == 0x1b) quit = 1;
            break;
        case PEV_BUTTONDOWN:
        case PEV_BUTTONUP:
            printf("EV %s button=%d win=%u x=%d y=%d root=%d,%d\n", evname(ev.type), ev.button, ev.win, ev.x, ev.y,
                   ev.root_x, ev.root_y);
            if (ev.type == PEV_BUTTONDOWN) buttons++;
            break;
        case PEV_WHEEL:
            printf("EV WHEEL delta=%d horizontal=%d x=%d y=%d\n", ev.wheel_delta, ev.wheel_horizontal, ev.x, ev.y);
            wheels++;
            break;
        case PEV_MOTION:
            motions++;
            break;
        case PEV_CONFIGURE:
            printf("EV CONFIGURE x=%d y=%d w=%d h=%d\n", ev.x, ev.y, ev.w, ev.h);
            configs++;
            break;
        case PEV_MAP:
            maps++;
            printf("EV MAP\n");
            break;
        case PEV_CLOSE:
            printf("EV CLOSE\n");
            quit = 1;
            break;
        default:
            break;
        }
    }

    int x = -1, y = -1;
    plat_window_get_pos(win, &x, &y);
    printf("POS %d %d\n", x, y);
    plat_window_move(win, 120, 130, W, H);
    plat_flush();
    for (int i = 0; i < 5; i++) plat_poll(&ev, 50);
    plat_window_get_pos(win, &x, &y);
    printf("MOVED %d %d\n", x, y);
    uint32_t icon_px[4 * 4];
    for (int i = 0; i < 16; i++) icon_px[i] = 0xff00ff00u;
    plat_window_set_icon(win, icon_px, 4, 4);
    plat_window_set_resizable(win, 0);
    printf("GRAB %d %d\n", plat_grab(win, 1), plat_grab(win, 0));
    int sw = 0, sh = 0;
    plat_screen_size(&sw, &sh);
    printf("SCREEN %d %d\n", sw, sh);
    int cx = 0, cy = 0;
    plat_cursor_pos(&cx, &cy);
    printf("CURSOR %d %d\n", cx, cy);
    plat_set_cursor(win, 32513);
    plat_clipboard_set("anywin clip \xc3\xa9");
    char *clip = plat_clipboard_get();
    printf("CLIP [%s]\n", clip ? clip : "(null)");
    free(clip);
    int icon_size = 0;
    uint32_t tray = plat_tray_add(&icon_size);
    printf("TRAY_NO_MANAGER %u size=%d\n", tray, icon_size);
    printf("COUNTS keys=%d buttons=%d motions=%d wheels=%d configs=%d maps=%d\n", keys, buttons, motions, wheels,
           configs, maps);
    plat_window_destroy(win);
    plat_flush();
    int ok = exposed && keys > 0;
    printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
