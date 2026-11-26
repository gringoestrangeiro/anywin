/* anywin user32 miscellany (S-MISC): keyboard layout and ToAsciiEx (US layout), OffsetRect,
 * MsgWaitForMultipleObjects(Ex), SystemParametersInfoA, the system menu object and window
 * placement. */
#include "user32_int.h"
#include "aw_api.h"
#include <poll.h>
#include <time.h>

extern DWORD WINAPI __aw_kernel32_WaitForSingleObject(HANDLE h, DWORD ms);
extern BOOL WINAPI __aw_user32_PeekMessageW(MSG *m, HWND h, UINT min, UINT max, UINT remove);

#ifndef AW_INFINITE
#define AW_INFINITE 0xFFFFFFFFu
#endif
#define AW_ERROR_INVALID_PARAMETER 87
#define AW_WAIT_OBJECT_0 0u
#define AW_WAIT_TIMEOUT 258u
#define AW_MWMO_WAITALL 0x1u
#define AW_KLID_US 0x04090409u
#define AW_SC_RESTORE 0xF120
#define AW_SC_MOVE 0xF010
#define AW_SC_SIZE 0xF000
#define AW_SC_MINIMIZE 0xF020
#define AW_SC_MAXIMIZE 0xF030
#define AW_SC_CLOSE 0xF060
#define AW_MF_STRING 0x0000u
#define AW_MF_GRAYED 0x0001u
#define AW_MF_DISABLED 0x0002u
#define AW_MF_SEPARATOR 0x0800u
#define AW_MF_ENABLED 0x0000u
#define AW_MF_BYCOMMAND 0x0000u
#define AW_GWL_STYLE (-16)
#define AW_WS_THICKFRAME 0x00040000u
#define AW_WS_MINIMIZEBOX 0x00020000u
#define AW_WS_MAXIMIZEBOX 0x00010000u
#define AW_WS_CHILD 0x40000000u
#define AW_SW_SHOWNORMAL 1
#define AW_SW_SHOWMINIMIZED 2
#define AW_SW_SHOWMAXIMIZED 3
#define AW_SW_MINIMIZE 6
#define AW_SW_MAXIMIZE 3
#define AW_SW_RESTORE 9
#define AW_SWP_NOZORDER 0x0004u
#define AW_SWP_NOACTIVATE 0x0010u

/* ---------------------------------------------------------------------- */
/* keyboard layout                                                         */
/* ---------------------------------------------------------------------- */

API(user32, HANDLE, GetKeyboardLayout, (DWORD tid))
{
    (void)tid;
    return (HANDLE)(uintptr_t)AW_KLID_US;
}

/* US QWERTY translation core (ToUnicodeEx / ToAsciiEx / ToAscii).  Only a key that is down in
 * ks produces text: a key-up is not a keystroke, and PuTTY calls ToUnicodeEx from WM_KEYUP too.
 * Modifiers are read from ks: Shift, CapsLock (toggle), Ctrl (control codes), NumLock (keypad digits).
 * Returns the number of code units written to out (0 = the key produces no text). */
static int translate_vk(UINT vk, const BYTE *ks, WCHAR out[2])
{
    static const char shifted_digits[] = ")!@#$%^&*(";
    static const struct { UINT vk; char lo, hi; } oem[] = {
        {0xBA, ';', ':'}, {0xBB, '=', '+'}, {0xBC, ',', '<'}, {0xBD, '-', '_'}, {0xBE, '.', '>'},
        {0xBF, '/', '?'}, {0xC0, '`', '~'}, {0xDB, '[', '{'}, {0xDC, '\\', '|'}, {0xDD, ']', '}'},
        {0xDE, '\'', '"'},
    };
    if (!ks || vk > 0xFF || !(ks[vk] & 0x80))
        return 0;
    int shift = (ks[0x10] | ks[0xA0] | ks[0xA1]) & 0x80;
    int ctrl = (ks[0x11] | ks[0xA2] | ks[0xA3]) & 0x80;
    int caps = ks[0x14] & 0x01;
    int numlock = ks[0x90] & 0x01;
    uint32_t ch = 0;
    if (vk >= 'A' && vk <= 'Z') {
        ch = ctrl ? vk - 'A' + 1 : (uint32_t)((shift != caps) ? vk : vk + 32);
    } else if (vk >= '0' && vk <= '9') {
        if (ctrl) return 0;
        ch = shift ? (uint32_t)shifted_digits[vk - '0'] : vk;
    } else if (vk >= 0x60 && vk <= 0x69) {          /* VK_NUMPAD0..9: digits only with NumLock */
        if (!numlock) return 0;
        ch = '0' + (vk - 0x60);
    } else {
        switch (vk) {
        case 0x20: ch = ' '; break;                  /* VK_SPACE */
        case 0x0D: ch = ctrl ? '\n' : '\r'; break;   /* VK_RETURN */
        case 0x09: ch = '\t'; break;                 /* VK_TAB */
        case 0x08: ch = ctrl ? 0x7F : 0x08; break;   /* VK_BACK */
        case 0x1B: ch = 0x1B; break;                 /* VK_ESCAPE */
        case 0x6A: ch = '*'; break;                  /* VK_MULTIPLY */
        case 0x6B: ch = '+'; break;                  /* VK_ADD */
        case 0x6C: ch = ','; break;                  /* VK_SEPARATOR */
        case 0x6D: ch = '-'; break;                  /* VK_SUBTRACT */
        case 0x6E: ch = '.'; break;                  /* VK_DECIMAL */
        case 0x6F: ch = '/'; break;                  /* VK_DIVIDE */
        default: {
            int hit = 0;
            for (size_t i = 0; i < sizeof oem / sizeof *oem; i++)
                if (oem[i].vk == vk) {
                    if (ctrl) {                      /* Ctrl+[ \ ] - _ ? give the C0 controls */
                        if (vk == 0xDB) ch = 0x1B;
                        else if (vk == 0xDC) ch = 0x1C;
                        else if (vk == 0xDD) ch = 0x1D;
                        else if (vk == 0xBD || vk == 0xBF) ch = 0x1F;
                        else return 0;
                    } else {
                        ch = (uint32_t)(shift ? oem[i].hi : oem[i].lo);
                    }
                    hit = 1;
                    break;
                }
            if (!hit)
                return 0;
        }
        }
    }
    if (ch == 0)
        return 0;
    if (ch < 0x10000) {
        out[0] = (WCHAR)ch;
        return 1;
    }
    return 0;
}

API(user32, int, ToUnicodeEx, (UINT vk, UINT scancode, const BYTE *ks, WCHAR *buf, int cch, UINT flags, HANDLE hkl))
{
    (void)scancode;
    (void)flags;
    (void)hkl;
    WCHAR tmp[2];
    if (!buf || cch < 1)
        return 0;
    int n = translate_vk(vk, ks, tmp);
    if (n == 0)
        return 0;
    if (cch < n)
        return -n;   /* Windows reports a too-small buffer as a negative count */
    buf[0] = tmp[0];
    return n;
}

API(user32, int, ToUnicode, (UINT vk, UINT scancode, const BYTE *ks, WCHAR *buf, int cch, UINT flags))
{
    return __aw_user32_ToUnicodeEx(vk, scancode, ks, buf, cch, flags, NULL);
}

API(user32, int, ToAsciiEx, (UINT vk, UINT scancode, const BYTE *ks, WORD *buf, UINT flags, HANDLE hkl))
{
    (void)scancode;
    (void)flags;
    (void)hkl;
    WCHAR tmp[2];
    int n = translate_vk(vk, ks, tmp);
    if (n == 0)
        return 0;
    if (tmp[0] > 0xFF)
        return 0;
    if (buf)
        buf[0] = tmp[0];
    return 1;
}

API(user32, int, ToAscii, (UINT vk, UINT scancode, const BYTE *ks, WORD *buf, UINT flags))
{
    return __aw_user32_ToAsciiEx(vk, scancode, ks, buf, flags, NULL);
}

/* MapVirtualKey: the translations the US layout has (MAPVK_*: 0 vk->scan, 1 scan->vk, 2 vk->char) */
API(user32, UINT, MapVirtualKeyA, (UINT code, UINT type))
{
    if (type == 0) {                       /* vk -> scancode for the letters, digits and a few keys */
        if (code >= 'A' && code <= 'Z') {
            static const UINT row[26] = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                         0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
            return row[code - 'A'];
        }
        if (code >= '1' && code <= '9') return code - '1' + 0x02;
        if (code == '0') return 0x0B;
        switch (code) {
        case 0x20: return 0x39;   /* VK_SPACE */
        case 0x0D: return 0x1C;   /* VK_RETURN */
        case 0x1B: return 0x01;   /* VK_ESCAPE */
        case 0x08: return 0x0E;   /* VK_BACK */
        case 0x09: return 0x0F;   /* VK_TAB */
        case 0x10: case 0xA0: return 0x2A;
        case 0x11: case 0xA2: return 0x1D;
        case 0x12: case 0xA4: return 0x38;
        default: return 0;
        }
    }
    if (type == 1 || type == 3) {          /* scancode -> vk (unshifted, left-hand variants) */
        UINT sc = code & 0xFF;
        if (type == 3) {
            switch (sc) { case 0x2A: return 0xA0; case 0x36: return 0xA1; case 0x1D: return 0xA2; case 0x38: return 0xA4; }
        }
        for (UINT vk = 0x30; vk <= 0x39; vk++)
            if (__aw_user32_MapVirtualKeyA(vk, 0) == sc) return vk;
        for (UINT vk = 'A'; vk <= 'Z'; vk++)
            if (__aw_user32_MapVirtualKeyA(vk, 0) == sc) return vk;
        switch (sc) {
        case 0x39: return 0x20; case 0x1C: return 0x0D; case 0x01: return 0x1B; case 0x0E: return 0x08;
        case 0x0F: return 0x09; case 0x2A: return 0x10; case 0x36: return 0x10; case 0x1D: return 0x11; case 0x38: return 0x12;
        default: return 0;
        }
    }
    if (type == 2) {                       /* vk -> unshifted character, dead bit clear */
        BYTE ks[256];
        WCHAR w[2];
        memset(ks, 0, sizeof ks);
        ks[code & 0xFF] = 0x80;
        if (translate_vk(code, ks, w) == 1 && w[0] < 0x80)
            return (UINT)w[0];
        return 0;
    }
    return 0;
}

API(user32, UINT, MapVirtualKeyW, (UINT code, UINT type)) { return __aw_user32_MapVirtualKeyA(code, type); }

API(user32, SHORT, VkKeyScanA, (char ch))
{
    for (UINT vk = 'A'; vk <= 'Z'; vk++) {
        if ((UINT)ch == vk || (UINT)ch == vk + 32) return (SHORT)(vk | ((UINT)ch == vk ? 0x100 : 0));
    }
    for (UINT vk = '0'; vk <= '9'; vk++) if ((UINT)ch == vk) return (SHORT)vk;
    if (ch == ' ') return 0x20;
    if (ch == '\r') return 0x0D;
    if (ch == '\t') return 0x09;
    return -1;
}
API(user32, SHORT, VkKeyScanW, (WCHAR ch)) { return __aw_user32_VkKeyScanA((char)ch); }

/* ---------------------------------------------------------------------- */
/* monitors: one display, the primary monitor covers the whole screen        */
/* ---------------------------------------------------------------------- */

#define AW_HMONITOR_PRIMARY ((HANDLE)(uintptr_t)1)
#define AW_MONITORINFOF_PRIMARY 1u

static void primary_rect(RECT *r)
{
    r->left = 0;
    r->top = 0;
    r->right = __aw_user32_GetSystemMetrics(0);
    r->bottom = __aw_user32_GetSystemMetrics(1);
}

API(user32, HANDLE, MonitorFromWindow, (HWND h, DWORD flags))
{
    (void)h;
    (void)flags; /* there is always a monitor: the primary one */
    return AW_HMONITOR_PRIMARY;
}

API(user32, HANDLE, MonitorFromPoint, (POINT pt, DWORD flags))
{
    (void)pt;
    (void)flags;
    return AW_HMONITOR_PRIMARY;
}

API(user32, HANDLE, MonitorFromRect, (const RECT *rc, DWORD flags))
{
    (void)rc;
    (void)flags;
    return AW_HMONITOR_PRIMARY;
}

/* MONITORINFO / MONITORINFOEX: the work area is the whole screen (no panels are reserved) */
static BOOL monitor_info(HANDLE mon, DWORD cb, void *info, int wide)
{
    if (mon != AW_HMONITOR_PRIMARY || !info || cb < sizeof(MONITORINFO)) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MONITORINFO *mi = info;
    primary_rect(&mi->rcMonitor);
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = AW_MONITORINFOF_PRIMARY;
    if (cb >= sizeof(MONITORINFOEXA)) {
        if (wide) {
            WCHAR *dev = ((MONITORINFOEXW *)info)->szDevice;
            memcpy(dev, u"DISPLAY1", sizeof u"DISPLAY1");
        } else {
            memcpy(((MONITORINFOEXA *)info)->szDevice, "DISPLAY1", sizeof "DISPLAY1");
        }
    }
    return TRUE;
}

API(user32, BOOL, GetMonitorInfoA, (HANDLE mon, MONITORINFO *info))
{
    return monitor_info(mon, info ? info->cbSize : 0, info, 0);
}

API(user32, BOOL, GetMonitorInfoW, (HANDLE mon, MONITORINFO *info))
{
    return monitor_info(mon, info ? info->cbSize : 0, info, 1);
}

API(user32, BOOL, FlashWindowEx, (void *info))
{
    /* the window is never flashing: the previous state is "not flashing" */
    (void)info;
    return FALSE;
}

/* ---------------------------------------------------------------------- */
/* geometry                                                                */
/* ---------------------------------------------------------------------- */

static void SetRectEmptyOf(RECT *rc)
{
    rc->left = rc->top = rc->right = rc->bottom = 0;
}

static int IsRectEmptyOf(const RECT *rc)
{
    return rc->left >= rc->right || rc->top >= rc->bottom;
}

API(user32, BOOL, SetRect, (RECT *rc, int l, int t, int r, int b))
{
    if (!rc)
        return FALSE;
    rc->left = l;
    rc->top = t;
    rc->right = r;
    rc->bottom = b;
    return TRUE;
}

API(user32, BOOL, SetRectEmpty, (RECT *rc))
{
    if (!rc)
        return FALSE;
    rc->left = rc->top = rc->right = rc->bottom = 0;
    return TRUE;
}

API(user32, BOOL, CopyRect, (RECT *dst, const RECT *src))
{
    if (!dst || !src)
        return FALSE;
    *dst = *src;
    return TRUE;
}

API(user32, BOOL, IsRectEmpty, (const RECT *rc))
{
    return !rc || rc->left >= rc->right || rc->top >= rc->bottom;
}

API(user32, BOOL, EqualRect, (const RECT *a, const RECT *b))
{
    return a && b && a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

API(user32, BOOL, PtInRect, (const RECT *rc, POINT pt))
{
    return rc && pt.x >= rc->left && pt.x < rc->right && pt.y >= rc->top && pt.y < rc->bottom;
}

API(user32, BOOL, InflateRect, (RECT *rc, int dx, int dy))
{
    if (!rc)
        return FALSE;
    rc->left -= dx;
    rc->right += dx;
    rc->top -= dy;
    rc->bottom += dy;
    return TRUE;
}

API(user32, BOOL, IntersectRect, (RECT *dst, const RECT *a, const RECT *b))
{
    RECT r;
    if (!dst || !a || !b)
        return FALSE;
    r.left = a->left > b->left ? a->left : b->left;
    r.top = a->top > b->top ? a->top : b->top;
    r.right = a->right < b->right ? a->right : b->right;
    r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (r.left >= r.right || r.top >= r.bottom) {
        SetRectEmptyOf(dst);
        return FALSE;
    }
    *dst = r;
    return TRUE;
}

API(user32, BOOL, UnionRect, (RECT *dst, const RECT *a, const RECT *b))
{
    if (!dst || !a || !b)
        return FALSE;
    if (IsRectEmptyOf(a) && IsRectEmptyOf(b)) {
        SetRectEmptyOf(dst);
        return FALSE;
    }
    if (IsRectEmptyOf(a)) {
        *dst = *b;
        return TRUE;
    }
    if (IsRectEmptyOf(b)) {
        *dst = *a;
        return TRUE;
    }
    dst->left = a->left < b->left ? a->left : b->left;
    dst->top = a->top < b->top ? a->top : b->top;
    dst->right = a->right > b->right ? a->right : b->right;
    dst->bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return TRUE;
}

API(user32, BOOL, OffsetRect, (RECT *rc, int dx, int dy))
{
    if (!rc)
        return FALSE;
    rc->left += dx;
    rc->right += dx;
    rc->top += dy;
    rc->bottom += dy;
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* MsgWaitForMultipleObjects                                               */
/* ---------------------------------------------------------------------- */

/* Waits for the handles or for a message in the queue.  Messages are looked for by pumping the
 * platform queue (PeekMessage with PM_NOREMOVE).  Between checks the thread sleeps in the message
 * loop's idle wait (aw_msg_idle_wait: X input, a wake-up from another thread, socket events, the
 * next timer), so a posted message is seen as soon as it is queued.  Kernel handles cannot wake
 * that sleep, so with handles the sleep is capped at 10 ms.  The wake mask is not narrowed: any
 * queued message wakes the wait. */
static DWORD msg_wait(DWORD n, const HANDLE *hs, int wait_all, DWORD ms)
{
    uint32_t start = aw_tick_ms();
    extern void aw_net_pump(int timeout_ms); /* ws2_async.c: Winsock socket events into their handles */
    extern void aw_msg_idle_wait(int max_ms); /* user32_msg.c */
    for (;;) {
        aw_net_pump(0);
        if (n) {
            if (wait_all) {
                int all = 1;
                for (DWORD i = 0; i < n && all; i++)
                    all = __aw_kernel32_WaitForSingleObject(hs[i], 0) == AW_WAIT_OBJECT_0;
                if (all)
                    return AW_WAIT_OBJECT_0;
            } else {
                for (DWORD i = 0; i < n; i++)
                    if (__aw_kernel32_WaitForSingleObject(hs[i], 0) == AW_WAIT_OBJECT_0)
                        return AW_WAIT_OBJECT_0 + i;
            }
        }
        MSG m;
        if (__aw_user32_PeekMessageW(&m, NULL, 0, 0, 0 /* PM_NOREMOVE */))
            return AW_WAIT_OBJECT_0 + n;
        int max = n ? 10 : -1;
        if (ms != AW_INFINITE) {
            uint32_t el = aw_tick_ms() - start;
            if (el >= ms)
                return AW_WAIT_TIMEOUT;
            if (max < 0 || (int)(ms - el) < max)
                max = (int)(ms - el);
        }
        aw_msg_idle_wait(max);
    }
}

API(user32, DWORD, MsgWaitForMultipleObjectsEx, (DWORD n, const HANDLE *hs, DWORD ms, DWORD mask, DWORD flags))
{
    (void)mask;
    return msg_wait(n, hs, (flags & AW_MWMO_WAITALL) != 0, ms);
}

API(user32, DWORD, MsgWaitForMultipleObjects, (DWORD n, const HANDLE *hs, BOOL wait_all, DWORD ms, DWORD mask))
{
    (void)mask;
    return msg_wait(n, hs, wait_all != 0, ms);
}

/* ---------------------------------------------------------------------- */
/* SystemParametersInfoA: the ANSI structures differ from the wide ones only in their LOGFONTs */
/* ---------------------------------------------------------------------- */

static void lf_w2a(const LOGFONTW *w, LOGFONTA *a)
{
    memcpy(a, w, offsetof(LOGFONTA, lfFaceName));
    aw_w2u_term(w->lfFaceName, 32, a->lfFaceName, 32);
}

static void lf_a2w(const LOGFONTA *a, LOGFONTW *w)
{
    memcpy(w, a, offsetof(LOGFONTA, lfFaceName));
    aw_u2w_buf(a->lfFaceName, (int)strnlen(a->lfFaceName, 32), w->lfFaceName, 32);
    w->lfFaceName[31] = 0;
}

API(user32, BOOL, SystemParametersInfoA, (UINT action, UINT param, void *pv, UINT winini))
{
    switch (action) {
    case 0x0029: { /* SPI_GETNONCLIENTMETRICS */
        NONCLIENTMETRICSA *a = pv;
        if (!a || a->cbSize != sizeof *a) {
            aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        NONCLIENTMETRICSW w;
        memset(&w, 0, sizeof w);
        w.cbSize = sizeof w;
        if (!__aw_user32_SystemParametersInfoW(action, param, &w, winini))
            return FALSE;
        a->iBorderWidth = w.iBorderWidth;
        a->iScrollWidth = w.iScrollWidth;
        a->iScrollHeight = w.iScrollHeight;
        a->iCaptionWidth = w.iCaptionWidth;
        a->iCaptionHeight = w.iCaptionHeight;
        lf_w2a(&w.lfCaptionFont, &a->lfCaptionFont);
        a->iSmCaptionWidth = w.iSmCaptionWidth;
        a->iSmCaptionHeight = w.iSmCaptionHeight;
        lf_w2a(&w.lfSmCaptionFont, &a->lfSmCaptionFont);
        a->iMenuWidth = w.iMenuWidth;
        a->iMenuHeight = w.iMenuHeight;
        lf_w2a(&w.lfMenuFont, &a->lfMenuFont);
        lf_w2a(&w.lfStatusFont, &a->lfStatusFont);
        lf_w2a(&w.lfMessageFont, &a->lfMessageFont);
        a->iPaddedBorderWidth = w.iPaddedBorderWidth;
        return TRUE;
    }
    case 0x002D: { /* SPI_GETICONMETRICS */
        ICONMETRICSA *a = pv;
        if (!a || a->cbSize != sizeof *a) {
            aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        ICONMETRICSW w;
        memset(&w, 0, sizeof w);
        w.cbSize = sizeof w;
        if (!__aw_user32_SystemParametersInfoW(action, param, &w, winini))
            return FALSE;
        a->iHorzSpacing = w.iHorzSpacing;
        a->iVertSpacing = w.iVertSpacing;
        a->iTitleWrap = w.iTitleWrap;
        lf_w2a(&w.lfFont, &a->lfFont);
        return TRUE;
    }
    case 0x001F: { /* SPI_GETICONTITLELOGFONT */
        LOGFONTA *a = pv;
        if (!a)
            return FALSE;
        LOGFONTW w;
        memset(&w, 0, sizeof w);
        if (!__aw_user32_SystemParametersInfoW(action, param, &w, winini))
            return FALSE;
        lf_w2a(&w, a);
        return TRUE;
    }
    case 0x0022: /* SPI_SETICONTITLELOGFONT: accepted, not persisted (as in the wide variant) */
        if (!pv)
            return FALSE;
        {
            LOGFONTW w;
            lf_a2w(pv, &w);
            return __aw_user32_SystemParametersInfoW(action, param, &w, winini);
        }
    default:
        return __aw_user32_SystemParametersInfoW(action, param, pv, winini);
    }
}

/* ---------------------------------------------------------------------- */
/* system menu                                                             */
/* ---------------------------------------------------------------------- */

static struct {
    HWND hwnd;
    HMENU menu;
} *sysmenus;
static int nsysmenus, capsysmenus;

static HMENU build_sysmenu(HWND h)
{
    LONG style = __aw_user32_GetWindowLongW(h, AW_GWL_STYLE);
    HMENU m = __aw_user32_CreatePopupMenu();
    if (!m)
        return NULL;
    int maxed = __aw_user32_IsZoomed(h) != 0, mined = __aw_user32_IsIconic(h) != 0;
    __aw_user32_AppendMenuA(m, AW_MF_STRING | (maxed || mined ? AW_MF_ENABLED : AW_MF_GRAYED), AW_SC_RESTORE,
                            "&Restore");
    __aw_user32_AppendMenuA(m, AW_MF_STRING, AW_SC_MOVE, "&Move");
    __aw_user32_AppendMenuA(m, AW_MF_STRING | ((style & AW_WS_THICKFRAME) ? AW_MF_ENABLED : AW_MF_GRAYED),
                            AW_SC_SIZE, "&Size");
    __aw_user32_AppendMenuA(m, AW_MF_STRING | ((style & AW_WS_MINIMIZEBOX) ? AW_MF_ENABLED : AW_MF_GRAYED),
                            AW_SC_MINIMIZE, "Mi&nimize");
    __aw_user32_AppendMenuA(m, AW_MF_STRING | ((style & AW_WS_MAXIMIZEBOX) ? AW_MF_ENABLED : AW_MF_GRAYED),
                            AW_SC_MAXIMIZE, "Ma&ximize");
    __aw_user32_AppendMenuA(m, AW_MF_SEPARATOR, 0, NULL);
    __aw_user32_AppendMenuA(m, AW_MF_STRING, AW_SC_CLOSE, "&Close\tAlt+F4");
    return m;
}

API(user32, HMENU, GetSystemMenu, (HWND h, BOOL revert))
{
    int idx = -1;
    for (int i = 0; i < nsysmenus; i++)
        if (sysmenus[i].hwnd == h)
            idx = i;
    if (idx >= 0 && !revert)
        return sysmenus[idx].menu;
    if (!__aw_user32_IsWindow(h))
        return NULL;
    HMENU m = build_sysmenu(h);
    if (!m)
        return NULL;
    if (idx >= 0) {
        __aw_user32_DestroyMenu(sysmenus[idx].menu);
        sysmenus[idx].menu = m;
        return m;
    }
    if (nsysmenus == capsysmenus) {
        int nc = capsysmenus ? capsysmenus * 2 : 8;
        void *p = realloc(sysmenus, sizeof *sysmenus * (size_t)nc);
        if (!p)
            aw_fatal("user32: out of memory");
        sysmenus = p;
        capsysmenus = nc;
    }
    sysmenus[nsysmenus].hwnd = h;
    sysmenus[nsysmenus].menu = m;
    nsysmenus++;
    return m;
}

/* ---------------------------------------------------------------------- */
/* window placement                                                        */
/* ---------------------------------------------------------------------- */

API(user32, BOOL, GetWindowPlacement, (HWND h, WINDOWPLACEMENT *wp))
{
    if (!wp || !__aw_user32_IsWindow(h)) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (wp->length != sizeof *wp) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    wp->flags = 0;
    if (__aw_user32_IsIconic(h))
        wp->showCmd = AW_SW_SHOWMINIMIZED;
    else if (__aw_user32_IsZoomed(h))
        wp->showCmd = AW_SW_SHOWMAXIMIZED;
    else
        wp->showCmd = AW_SW_SHOWNORMAL;
    wp->ptMinPosition.x = wp->ptMinPosition.y = -1;
    wp->ptMaxPosition.x = wp->ptMaxPosition.y = -1;
    RECT r;
    __aw_user32_GetWindowRect(h, &r);
    LONG style = __aw_user32_GetWindowLongW(h, AW_GWL_STYLE);
    HWND parent = (style & AW_WS_CHILD) ? __aw_user32_GetParent(h) : NULL;
    if (parent) {
        /* child windows report their rectangle relative to the parent's client area */
        POINT a = {r.left, r.top}, b = {r.right, r.bottom};
        __aw_user32_ScreenToClient(parent, &a);
        __aw_user32_ScreenToClient(parent, &b);
        r.left = a.x;
        r.top = a.y;
        r.right = b.x;
        r.bottom = b.y;
    }
    wp->rcNormalPosition = r;
    return TRUE;
}

API(user32, BOOL, SetWindowPlacement, (HWND h, const WINDOWPLACEMENT *wp))
{
    if (!wp || !__aw_user32_IsWindow(h) || wp->length != sizeof *wp) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const RECT *r = &wp->rcNormalPosition;
    int w = r->right - r->left, hgt = r->bottom - r->top;
    if (wp->showCmd == AW_SW_SHOWMAXIMIZED || wp->showCmd == AW_SW_SHOWMINIMIZED) {
        __aw_user32_SetWindowPos(h, NULL, r->left, r->top, w, hgt, AW_SWP_NOZORDER | AW_SWP_NOACTIVATE);
        __aw_user32_ShowWindow(h, wp->showCmd == AW_SW_SHOWMAXIMIZED ? AW_SW_MAXIMIZE : AW_SW_MINIMIZE);
    } else {
        __aw_user32_ShowWindow(h, AW_SW_RESTORE);
        __aw_user32_SetWindowPos(h, NULL, r->left, r->top, w, hgt, AW_SWP_NOZORDER | AW_SWP_NOACTIVATE);
    }
    return TRUE;
}
