/* user32 core - X events to messages, activation/focus, capture, cursor, key state, hover (S5). */
#include "user32_int.h"
#include <unistd.h>

#define ERR_INVALID_WINDOW 1400
#define AW_HOVER_DEFAULT_MS 400
#define IDC_ARROW_ID 32512
#define TME_HOVER_FLAG 0x00000001
#define TME_LEAVE_FLAG 0x00000002
#define TME_CANCEL_FLAG 0x80000000u

/* aw_key_state is the key state of the message being processed (GetKeyState, GetKeyboardState): it is
 * copied from each input message when the message is retrieved, as on Windows.  aw_key_live is the state
 * as the X events left it (GetAsyncKeyState); it runs ahead of the messages when they queue up. */
uint8_t aw_key_state[256];   /* bit 0x80: down; bit 0x01: toggled (CAPSLOCK, NUMLOCK, SCROLL) */
uint8_t aw_key_live[256];
static int g_cursor_count;
extern int aw_tray_handle_event(aw_plat_event *ev) __attribute__((weak));

static aw_win *from(HWND h) { return aw_win_from_hwnd(h); }

static aw_queue *cur_q(void) { return aw_queue_current(); }

/* queue of the thread that owns w (messages for w are queued there) */
static aw_queue *queue_of(aw_win *w)
{
    aw_queue *q = aw_queue_for_tid(w->tid);
    return q ? q : cur_q();
}

static uint32_t now_ms(void) { return aw_tick_ms(); }

void aw_key_set(int vk, int down, int toggled)
{
    if (vk < 0 || vk > 255)
        return;
    aw_key_live[vk] = (uint8_t)((down ? 0x80 : 0) | (toggled ? 0x01 : 0));
}

int aw_is_alt_down(void) { return (aw_key_state[0x12] & 0x80) != 0; }

/* ------------------------------------------------------------------ cursor */
void aw_apply_cursor(aw_win *w)
{
    if (!w) return;
    aw_win *top = aw_top_ancestor(w);
    if (!top || !top->xwin)
        return;
    int idc = IDC_ARROW_ID;
    HCURSOR cur = w->cursor ? w->cursor : (w->cls ? w->cls->cursor : NULL);
    aw_icon *ic = aw_icon_from_handle(cur);
    if (ic && ic->idc)
        idc = ic->idc;
    plat_set_cursor(top->xwin, idc);
}

/* ------------------------------------------------------------------ capture */
void aw_capture_set(HWND h)
{
    aw_queue *q = cur_q();
    HWND old = q->capture;
    if (old == h)
        return;
    q->capture = h;
    aw_win *ow = from(old);
    if (ow && ow->xwin) {
        aw_win *ot = aw_top_ancestor(ow);
        if (ot && ot->xwin && aw_window_kind(ot) == PLAT_WIN_POPUP) plat_grab(ot->xwin, 0);
    }
    if (ow)
        aw_call_proc(ow, WM_CAPTURECHANGED, 0, (LPARAM)h, ow->unicode);
    aw_win *nw = from(h);
    if (nw) {
        aw_win *nt = aw_top_ancestor(nw);
        if (nt && nt->xwin && aw_window_kind(nt) == PLAT_WIN_POPUP) plat_grab(nt->xwin, 1);
    }
}

API(user32, HWND, SetCapture, (HWND h))
{
    aw_win *w = from(h);
    if (!w) return NULL;
    HWND prev = cur_q()->capture;
    aw_capture_set(h);
    return prev;
}
API(user32, BOOL, ReleaseCapture, (void))
{
    aw_capture_set(NULL);
    return TRUE;
}
API(user32, HWND, GetCapture, (void)) { return cur_q()->capture; }

/* ------------------------------------------------------------------ activation and focus */
/* Makes top the active top-level (or deactivates it), sending WM_ACTIVATE / WM_SETFOCUS / WM_KILLFOCUS. */
void aw_activate_top(aw_win *top, int active)
{
    if (!top || top->parent || top->is_desktop || top->destroyed)
        return;
    aw_queue *q = queue_of(top);
    if (active) {
        if (q->active == top->hwnd)
            return;
        HWND oldh = q->active;
        if (oldh) {
            aw_win *old = from(oldh);
            if (old) {
                old->active = 0;
                if (q->focus) {
                    aw_win *of = from(q->focus);
                    if (of) aw_call_proc(of, WM_KILLFOCUS, 0, 0, of->unicode);
                }
                aw_win *o2 = from(oldh);
                if (o2) aw_call_proc(o2, WM_ACTIVATE, 0, 0, o2->unicode);
            }
        }
        q->active = top->hwnd;
        top->active = 1;
        HWND f = (top->last_focus && from(top->last_focus)) ? top->last_focus : top->hwnd;
        q->focus = f;
        aw_win *t2 = from(top->hwnd);
        if (t2) aw_call_proc(t2, WM_ACTIVATE, 1 /* WA_ACTIVE */, 0, t2->unicode);
        aw_win *fw = from(f);
        if (fw) aw_call_proc(fw, WM_SETFOCUS, 0, 0, fw->unicode);
        return;
    }
    if (q->active != top->hwnd)
        return;
    if (q->focus) {
        aw_win *fw = from(q->focus);
        top->last_focus = q->focus;
        if (fw) aw_call_proc(fw, WM_KILLFOCUS, 0, 0, fw->unicode);
    }
    q->focus = NULL;
    q->active = NULL;
    top->active = 0;
    aw_win *t2 = from(top->hwnd);
    if (t2) aw_call_proc(t2, WM_ACTIVATE, 0 /* WA_INACTIVE */, 0, t2->unicode);
}

API(user32, HWND, SetFocus, (HWND h))
{
    aw_queue *q = cur_q();
    HWND prev = q->focus;
    if (!h) {
        aw_win *old = from(prev);
        if (q->active && old) {
            q->focus = NULL;
            aw_call_proc(old, WM_KILLFOCUS, 0, 0, old->unicode);
        }
        return prev;
    }
    aw_win *w = from(h);
    if (!w) {
        aw_set_last_error(ERR_INVALID_WINDOW);
        return NULL;
    }
    if (!w->enabled)
        return NULL;
    aw_win *top = aw_top_ancestor(w);
    if (top != w)
        top->last_focus = h;
    if (!top->active || q->active != top->hwnd) {
        /* not active yet: the focus window is restored at activation */
        return prev;
    }
    if (prev == h)
        return prev;
    q->focus = h;
    aw_win *old = from(prev);
    if (old) aw_call_proc(old, WM_KILLFOCUS, (WPARAM)h, 0, old->unicode);
    aw_win *nw = from(h);
    if (nw) aw_call_proc(nw, WM_SETFOCUS, (WPARAM)prev, 0, nw->unicode);
    return prev;
}

API(user32, HWND, GetFocus, (void))
{
    aw_queue *q = cur_q();
    return q->active ? q->focus : NULL;
}

API(user32, HWND, GetActiveWindow, (void)) { return cur_q()->active; }

API(user32, HWND, SetActiveWindow, (HWND h))
{
    aw_queue *q = cur_q();
    HWND prev = q->active;
    aw_win *w = from(h);
    if (!w) return NULL;
    aw_win *top = aw_top_ancestor(w);
    aw_activate_top(top, 1);
    return prev;
}

API(user32, HWND, GetForegroundWindow, (void)) { return cur_q()->active; }

API(user32, BOOL, SetForegroundWindow, (HWND h))
{
    aw_win *w = from(h);
    if (!w) return FALSE;
    aw_win *top = aw_top_ancestor(w);
    if (top->xwin)
        plat_window_activate(top->xwin);
    aw_activate_top(top, 1);
    return TRUE;
}

API(user32, BOOL, FlashWindow, (HWND h, BOOL invert))
{
    (void)h; (void)invert;
    return TRUE;
}

/* ------------------------------------------------------------------ cursor API */
API(user32, HCURSOR, SetCursor, (HCURSOR c))
{
    aw_queue *q = cur_q();
    aw_win *hw = from(q->hover);
    HCURSOR prev = hw ? hw->cursor : NULL;
    if (hw) {
        hw->cursor = c;
        aw_apply_cursor(hw);
    }
    return prev;
}

API(user32, HCURSOR, GetCursor, (void))
{
    aw_win *hw = from(cur_q()->hover);
    return hw ? hw->cursor : NULL;
}

API(user32, int, ShowCursor, (BOOL show))
{
    g_cursor_count += show ? 1 : -1;
    return g_cursor_count;
}

API(user32, BOOL, GetCursorPos, (POINT *pt))
{
    if (!pt) return FALSE;
    int x = 0, y = 0;
    if (aw_display)
        plat_cursor_pos(&x, &y);
    pt->x = x;
    pt->y = y;
    return TRUE;
}

API(user32, BOOL, SetCursorPos, (int x, int y))
{
    (void)x; (void)y;
    return FALSE;
}

/* ------------------------------------------------------------------ keyboard / mouse state */
static int is_toggle_vk(int vk) { return vk == 0x14 || vk == 0x90 || vk == 0x91; }

API(user32, SHORT, GetAsyncKeyState, (int vk))
{
    if (vk < 0 || vk > 255) return 0;
    return (aw_key_live[vk] & 0x80) ? (SHORT)0x8000 : 0;
}

API(user32, SHORT, GetKeyState, (int vk))
{
    if (vk < 0 || vk > 255) return 0;
    SHORT r = (aw_key_state[vk] & 0x80) ? (SHORT)0x8000 : 0;
    if (is_toggle_vk(vk) && (aw_key_state[vk] & 0x01))
        r |= 1;
    return r;
}

API(user32, BOOL, GetKeyboardState, (BYTE *buf))
{
    if (!buf) return FALSE;
    for (int i = 0; i < 256; i++)
        buf[i] = aw_key_state[i];
    return TRUE;
}

API(user32, BOOL, SetKeyboardState, (BYTE *buf))
{
    if (!buf) return FALSE;
    for (int i = 0; i < 256; i++)
        aw_key_state[i] = aw_key_live[i] = buf[i];
    return TRUE;
}

API(user32, UINT, GetDoubleClickTime, (void)) { return 500; }

/* ------------------------------------------------------------------ tracking */
API(user32, BOOL, TrackMouseEvent, (TRACKMOUSEEVENT *tme))
{
    if (!tme || tme->cbSize < sizeof(TRACKMOUSEEVENT)) return FALSE;
    aw_win *w = from(tme->hwndTrack);
    if (!w) return FALSE;
    aw_queue *q = queue_of(w);
    if (tme->dwFlags & TME_CANCEL_FLAG) {
        w->tracking_leave = 0;
        w->tracking_hover = 0;
        return TRUE;
    }
    if (tme->dwFlags & TME_LEAVE_FLAG) {
        if (q->hover != w->hwnd) {
            /* pointer is not over the window: the leave is reported at once */
            aw_call_proc(w, WM_MOUSELEAVE, 0, 0, w->unicode);
        } else {
            w->tracking_leave = 1;
        }
    }
    if (tme->dwFlags & TME_HOVER_FLAG) {
        w->tracking_hover = 1;
        q->hover_tick = now_ms();
        q->hover_x = q->last_pt.x;
        q->hover_y = q->last_pt.y;
    }
    return TRUE;
}

/* Idle check: WM_MOUSEHOVER when the pointer stayed over a tracking window long enough. */
void aw_hover_idle(void)
{
    aw_queue *q = cur_q();
    aw_win *w = from(q->hover);
    if (!w || !w->tracking_hover)
        return;
    if (now_ms() - q->hover_tick < AW_HOVER_DEFAULT_MS)
        return;
    w->tracking_hover = 0;
    int cx, cy;
    aw_screen_client_pos(w, &cx, &cy);
    aw_post_input(w, WM_MOUSEHOVER, 0, MAKELONG(q->hover_x - cx, q->hover_y - cy), now_ms(), q->hover_x, q->hover_y);
}

/* ------------------------------------------------------------------ hit testing (surface coords) */
static aw_win *child_hit(aw_win *p, int sx, int sy)
{
    for (int guard = 0; guard < 64; guard++) {
        aw_win *hit = NULL;
        for (HWND c = p->first_child; c;) {
            aw_win *cw = from(c);
            if (!cw) break;
            if (cw->visible) {
                RECT r;
                aw_win_surf_rect(cw, &r);
                if (sx >= r.left && sx < r.right && sy >= r.top && sy < r.bottom) {
                    /* a window that answers HTTRANSPARENT (a group box) does not take the mouse */
                    LRESULT ht = aw_call_proc(cw, WM_NCHITTEST, 0, MAKELONG(sx, sy), cw->unicode);
                    if (ht != HTTRANSPARENT) {
                        hit = cw;
                        break;
                    }
                }
            }
            c = cw->next_sib;
        }
        if (!hit)
            return p;
        p = hit;
    }
    return p;
}

static uint32_t mk_keys(uint32_t mods)
{
    uint32_t k = 0;
    if (mods & AW_MOD_LBUTTON) k |= 0x0001;
    if (mods & AW_MOD_RBUTTON) k |= 0x0002;
    if (mods & AW_MOD_SHIFT) k |= 0x0004;
    if (mods & AW_MOD_CTRL) k |= 0x0008;
    if (mods & AW_MOD_MBUTTON) k |= 0x0010;
    return k;
}

static UINT button_msg(int btn, int down, int dbl)
{
    if (btn == 1) return dbl ? WM_LBUTTONDBLCLK : down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
    if (btn == 3) return dbl ? WM_RBUTTONDBLCLK : down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
    if (btn == 2) return dbl ? WM_MBUTTONDBLCLK : down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
    return dbl ? WM_XBUTTONDBLCLK : down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
}

static void key_message(aw_plat_event *ev, int down)
{
    aw_win *xw = aw_win_from_xwin(ev->win);
    if (!xw) return;
    aw_win *top = aw_top_ancestor(xw);
    aw_queue *q = queue_of(top);
    if (!q->active)
        aw_activate_top(top, 1);
    HWND target = q->focus && from(q->focus) ? q->focus : top->hwnd;
    aw_win *tw = from(target);
    if (!tw) return;
    int sys = (ev->mods & AW_MOD_ALT) || ev->vk == 0x79 /* VK_F10 */;
    UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    LPARAM lp = 1;
    lp |= (LPARAM)(ev->scancode & 0xFF) << 16;
    if (ev->scancode & 0x100) lp |= (LPARAM)1 << 24;
    if (ev->mods & AW_MOD_ALT) lp |= (LPARAM)1 << 29;
    if (!down) lp |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    else if (ev->is_repeat) lp |= (LPARAM)1 << 30;
    aw_key_set(ev->vk, down, 0);
    if (ev->vk == 0x14 || ev->vk == 0x90 || ev->vk == 0x91) {
        int tog = (ev->vk == 0x14) ? (ev->mods & AW_MOD_CAPSLOCK) != 0
                : (ev->vk == 0x90) ? (ev->mods & AW_MOD_NUMLOCK) != 0 : 0;
        aw_key_set(ev->vk, down, tog);
    }
    aw_post_input_text(tw, msg, (WPARAM)ev->vk, lp, ev->time, 0, 0, down ? ev->text : NULL);
}

static void button_event(aw_plat_event *ev, int down)
{
    aw_win *top = aw_win_from_xwin(ev->win);
    if (!top) return;
    top = aw_top_ancestor(top);
    if (!top || top->destroyed) return;
    int btn = ev->button;
    /* button -> VK_LBUTTON (1), VK_MBUTTON (4), VK_RBUTTON (2), VK_XBUTTON1 (5), VK_XBUTTON2 (6) */
    int vk = btn == 1 ? 1 : btn == 2 ? 4 : btn == 3 ? 2 : (btn == 4 ? 5 : btn == 5 ? 6 : 0);
    if (vk) aw_key_set(vk, down, 0);
    aw_queue *q = queue_of(top);
    if (down) {
        if (!q->active || q->active != top->hwnd)
            aw_activate_top(top, 1);
    }
    /* non-client: the menu bar belongs to menu.c */
    int sx = ev->x, sy = ev->y;
    /* MSG.pt is in screen coordinates; ev->x/y are relative to the top-level's X window */
    int psx = sx + top->rect.left, psy = sy + top->rect.top;
    aw_win *cap = from(q->capture);
    if (!cap && top->menu && !top->parent && sy < top->client.top) {
        if (down)
            aw_menubar_track(top->hwnd, top->menu, sx, sy, 0);
        return;
    }
    aw_win *target = cap ? cap : child_hit(top, sx, sy);
    if (!target || !target->enabled) return;
    /* scroll bars are in the non-client area: they get WM_NCLBUTTONDOWN with the hit code */
    if (down && btn == 1) {
        int ox, oy;
        aw_screen_pos(target, &ox, &oy);
        int hit = aw_scroll_hit(target, psx - ox, psy - oy);
        if (hit) {
            aw_post_input(target, WM_NCLBUTTONDOWN, (WPARAM)hit, MAKELONG(psx, psy), ev->time, psx, psy);
            return;
        }
    }
    int cx0, cy0;
    aw_win_client_origin(target, &cx0, &cy0);
    int lx = sx - cx0, ly = sy - cy0;
    /* X reports the state before a press and including a release, Win32 wants the button that changed
     * as down during its WM_*BUTTONDOWN and up during its WM_*BUTTONUP */
    uint32_t keys = mk_keys(ev->mods);
    uint32_t bit = btn == 1 ? 0x0001 : btn == 3 ? 0x0002 : btn == 2 ? 0x0010 : btn == 4 ? 0x0020 : btn == 5 ? 0x0040 : 0;
    if (down) keys |= bit;
    else keys &= ~bit;
    WPARAM wp = keys;
    if (btn >= 4) wp |= (WPARAM)((btn == 4 ? 1 : 2) << 16); /* XBUTTON1 / XBUTTON2 in the high word */
    UINT msg = button_msg(btn, down, 0);
    /* double click: a second press of the same button close in time and space */
    int dbl = 0;
    if (down && btn < 4 && q->click_hwnd == target->hwnd && q->click_btn == btn &&
        ev->time - q->click_time <= 500 && abs(sx - q->click_x) <= AW_DBLCLK_PX &&
        abs(sy - q->click_y) <= AW_DBLCLK_PX && (target->cls && (target->cls->style & CS_DBLCLKS))) {
        dbl = 1;
        q->click_hwnd = NULL;
    } else if (down && btn < 4) {
        q->click_hwnd = target->hwnd;
        q->click_btn = btn;
        q->click_x = sx;
        q->click_y = sy;
        q->click_time = ev->time;
    }
    if (dbl) msg = button_msg(btn, 1, 1);
    aw_post_input(target, msg, wp, MAKELONG(lx, ly), ev->time, psx, psy);
}

static void motion_event(aw_plat_event *ev)
{
    aw_win *top = aw_win_from_xwin(ev->win);
    if (!top) return;
    top = aw_top_ancestor(top);
    aw_queue *q = queue_of(top);
    aw_win *cap = from(q->capture);
    int sx = ev->x, sy = ev->y;
    int psx = sx + top->rect.left, psy = sy + top->rect.top; /* screen coordinates for MSG.pt */
    aw_win *target = cap ? cap : child_hit(top, sx, sy);
    if (!target) return;
    if (!cap && q->hover != target->hwnd) {
        aw_win *old = from(q->hover);
        if (old) {
            old->tracking_hover = 0;
            if (old->tracking_leave) {
                old->tracking_leave = 0;
                aw_post_input(old, WM_MOUSELEAVE, 0, 0, ev->time, 0, 0);
            }
        }
        q->hover = target->hwnd;
        aw_call_proc(target, WM_SETCURSOR, (WPARAM)target->hwnd, MAKELONG(1 /* HTCLIENT */, WM_MOUSEMOVE),
                     target->unicode);
        target = from(q->hover);
        if (!target) return;
    }
    if (target->tracking_hover) {
        q->hover_tick = now_ms();
        q->hover_x = psx; /* screen coordinates: aw_hover_idle converts them to the window's client */
        q->hover_y = psy;
    }
    if (!target->enabled) return;
    int cx0, cy0;
    aw_win_client_origin(target, &cx0, &cy0);
    aw_post_input(target, WM_MOUSEMOVE, mk_keys(ev->mods), MAKELONG(sx - cx0, sy - cy0), ev->time, psx, psy);
}

static void leave_top(aw_win *top)
{
    aw_queue *q = queue_of(top);
    aw_win *old = from(q->hover);
    if (old && old->tracking_leave) {
        old->tracking_leave = 0;
        aw_post_input(old, WM_MOUSELEAVE, 0, 0, now_ms(), 0, 0);
    }
    if (old) old->tracking_hover = 0;
    q->hover = NULL;
}

static void wheel_event(aw_plat_event *ev)
{
    aw_win *top = aw_win_from_xwin(ev->win);
    if (!top) return;
    top = aw_top_ancestor(top);
    aw_queue *q = queue_of(top);
    aw_win *target = child_hit(top, ev->x, ev->y);
    if (!target) target = from(q->focus);
    if (!target) target = top;
    int sx = ev->root_x, sy = ev->root_y;
    WPARAM wp = (WPARAM)(uint32_t)((int32_t)ev->wheel_delta << 16) | mk_keys(ev->mods);
    aw_post_input(target, ev->wheel_horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL, wp, MAKELONG(sx, sy), ev->time,
                  sx, sy);
}

/* ------------------------------------------------------------------ dispatch of one X event */
void aw_input_dispatch(aw_plat_event *ev)
{
    if (aw_tray_handle_event && aw_tray_handle_event(ev))
        return;
    aw_win *w = aw_win_from_xwin(ev->win);
    switch (ev->type) {
    case PEV_EXPOSE:
        if (w && w->surf && !w->parent) {
            RECT r = {ev->x, ev->y, ev->x + ev->w, ev->y + ev->h};
            plat_put(w->xwin, w->surf, &r);
        }
        break;
    case PEV_CONFIGURE:
        if (w && !w->parent)
            aw_on_configure(w, ev->x, ev->y, ev->w, ev->h);
        break;
    case PEV_CLOSE:
        if (w)
            aw_post_input(w, WM_SYSCOMMAND, SC_CLOSE, 0, ev->time, 0, 0);
        break;
    case PEV_FOCUSIN:
        if (w) aw_activate_top(aw_top_ancestor(w), 1);
        break;
    case PEV_FOCUSOUT:
        if (w) aw_activate_top(aw_top_ancestor(w), 0);
        break;
    case PEV_KEYDOWN:
    case PEV_KEYUP:
        key_message(ev, ev->type == PEV_KEYDOWN);
        break;
    case PEV_BUTTONDOWN:
        button_event(ev, 1);
        break;
    case PEV_BUTTONUP:
        button_event(ev, 0);
        break;
    case PEV_MOTION:
        motion_event(ev);
        break;
    case PEV_WHEEL:
        wheel_event(ev);
        break;
    case PEV_LEAVE:
        if (w) leave_top(aw_top_ancestor(w));
        break;
    default:
        break;
    }
}
