/* anywin caret (S-CARET): one system caret per thread, as in Win32.
 *
 * The caret is drawn by inverting its rectangle (or XOR-ing its bitmap) on the owner window's
 * surface, so drawing it a second time erases it.  A TIMER on the owner toggles the blink phase
 * every GetCaretBlinkTime() milliseconds.  A new caret starts hidden (hide count 1). */
#include "user32_int.h"
#include "aw_api.h"

#define AW_CARET_BLINK_MS 530
#define AW_CARET_TIMER_ID 0x7C41u
#define AW_SRCINVERT 0x00660046u
#define AW_DSTINVERT 0x00550009u

static struct {
    HWND owner;
    HBITMAP bmp;     /* NULL: solid block; (HBITMAP)1: gray block; else bitmap XOR-ed in */
    int w, h, x, y;
    int hide;        /* > 0: hidden; a new caret starts with 1 */
    int blink_on;    /* blink phase */
    int drawn;       /* currently XOR-ed onto the owner surface */
} cr;

/* XOR the caret onto the owner window at its current position */
static void caret_xor(void)
{
    HDC dc = __aw_user32_GetDC(cr.owner);
    if (!dc)
        return;
    if (cr.bmp && cr.bmp != (HBITMAP)(intptr_t)1) {
        HDC mem = __aw_gdi32_CreateCompatibleDC(dc);
        HGDIOBJ old = __aw_gdi32_SelectObject(mem, cr.bmp);
        __aw_gdi32_BitBlt(dc, cr.x, cr.y, cr.w, cr.h, mem, 0, 0, AW_SRCINVERT);
        __aw_gdi32_SelectObject(mem, old);
        __aw_gdi32_DeleteDC(mem);
    } else {
        __aw_gdi32_PatBlt(dc, cr.x, cr.y, cr.w, cr.h, AW_DSTINVERT);
    }
    __aw_user32_ReleaseDC(cr.owner, dc);
}

/* brings the drawn state in line with (owner && visible && blink on) */
static void caret_sync(void)
{
    int want = cr.owner && cr.hide == 0 && cr.blink_on;
    if (want != cr.drawn) {
        caret_xor();
        cr.drawn = want;
    }
}

static void AWCALL caret_timer(HWND h, UINT msg, UINT_PTR id, DWORD t)
{
    (void)h;
    (void)msg;
    (void)id;
    (void)t;
    cr.blink_on = !cr.blink_on;
    caret_sync();
}

static void caret_forget(void)
{
    if (cr.drawn)
        caret_xor();
    cr.drawn = 0;
    if (cr.owner)
        __aw_user32_KillTimer(cr.owner, AW_CARET_TIMER_ID);
    cr.owner = NULL;
    cr.bmp = NULL;
}

API(user32, BOOL, CreateCaret, (HWND h, HBITMAP bmp, int w, int hgt))
{
    if (cr.owner)
        caret_forget();
    cr.owner = h;
    cr.bmp = bmp;
    cr.w = w > 0 ? w : __aw_user32_GetSystemMetrics(5 /* SM_CXBORDER */);
    cr.h = hgt > 0 ? hgt : 16;
    if (cr.w < 1)
        cr.w = 1;
    cr.x = cr.y = 0;
    cr.hide = 1;
    cr.blink_on = 1;
    cr.drawn = 0;
    if (h)
        __aw_user32_SetTimer(h, AW_CARET_TIMER_ID, AW_CARET_BLINK_MS, caret_timer);
    return TRUE;
}

API(user32, BOOL, DestroyCaret, (void))
{
    if (!cr.owner)
        return FALSE;
    caret_forget();
    return TRUE;
}

API(user32, BOOL, ShowCaret, (HWND h))
{
    (void)h;
    if (!cr.owner)
        return FALSE;
    if (cr.hide > 0)
        cr.hide--;
    if (cr.hide == 0)
        cr.blink_on = 1;
    caret_sync();
    return TRUE;
}

API(user32, BOOL, HideCaret, (HWND h))
{
    (void)h;
    if (!cr.owner)
        return FALSE;
    cr.hide++;
    caret_sync();
    return TRUE;
}

API(user32, BOOL, SetCaretPos, (int x, int y))
{
    if (!cr.owner)
        return FALSE;
    if (cr.drawn) {
        caret_xor();
        cr.drawn = 0;
    }
    cr.x = x;
    cr.y = y;
    caret_sync();
    return TRUE;
}

API(user32, BOOL, GetCaretPos, (POINT *pt))
{
    if (!pt || !cr.owner)
        return FALSE;
    pt->x = cr.x;
    pt->y = cr.y;
    return TRUE;
}

API(user32, UINT, GetCaretBlinkTime, (void)) { return AW_CARET_BLINK_MS; }
