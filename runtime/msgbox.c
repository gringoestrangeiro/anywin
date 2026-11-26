/* anywin - MessageBox (work package G subset, S6).
 * The message box is one managed DIALOG-kind window (owned, WS_POPUP|WS_CAPTION) that paints its
 * text, icon and buttons itself. Keeping the buttons inside the window (no child controls) keeps the
 * keyboard routing simple: the box itself has the focus and receives Enter, Esc, Tab and arrows.
 * The owner is disabled while the box runs; aw_modal_loop (user32_msg.c) pumps the messages. */
#include "menu_int.h"
#include <stdio.h>

UD(int, DrawTextW, (HDC, LPCWSTR, int, RECT *, UINT));
UD(HICON, LoadIconW, (HINSTANCE, LPCWSTR));
UD(BOOL, DrawIconEx, (HDC, int, int, HICON, int, int, UINT, HBRUSH, UINT));
UD(BOOL, IsWindowEnabled, (HWND));

#define MB_WND_CLASS u"AnywinMessageBox"
#define MB_DT_LEFT 0x0000
#define MB_DT_CENTER 0x0001
#define MB_DT_WORDBREAK 0x0010
#define MB_DT_SINGLELINE 0x0020
#define MB_DT_VCENTER 0x0004
#define MB_DT_CALCRECT 0x0400
#define MB_DI_NORMAL 3
#define MB_MARGIN 16
#define MB_BTN_H 23
#define MB_BTN_MIN_W 75
#define MB_BTN_GAP 6
#define MB_MAX_BTNS 4
#define MB_WS_CAPTION_DLG ((DWORD)0x80000000L | (DWORD)0x00C00000L | (DWORD)0x00080000L) /* POPUP|CAPTION|SYSMENU */

#define MB_RGB(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define MB_RGB_WHITE MB_RGB(255, 255, 255)
#define MB_RGB_TEXT MB_RGB(0, 0, 0)
#define MB_RGB_BTN MB_RGB(225, 225, 225)
#define MB_RGB_BTN_DOWN MB_RGB(204, 228, 247)
#define MB_RGB_BTN_EDGE MB_RGB(173, 173, 173)
#define MB_RGB_DEFAULT MB_RGB(0, 120, 215)
#define MB_RGB_FOCUS MB_RGB(0, 0, 0)

typedef struct mb_btn {
    int id;
    const WCHAR *label;
    int w;
    RECT r;
} mb_btn;

typedef struct mb_set_item {
    int id;
    const WCHAR *label;
} mb_set_item;

typedef struct mb_state {
    HWND owner, hwnd;
    const WCHAR *text;
    UINT type;
    int nbtn;
    mb_btn btn[MB_MAX_BTNS];
    int defbtn;      /* index of the default button */
    int focus;       /* index of the keyboard focus (starts at the default) */
    int pressed;     /* index of a button under a mouse press, -1 none */
    int done;
    int result;
    int icon;        /* IDI_* id, 0 none */
    RECT text_r;
    int client_w, client_h;
} mb_state;

static mb_state *g_mb;
static int g_mb_class_done;

/* ------------------------------------------------------------------ button sets */
static int mb_build_buttons(UINT type, mb_btn *b)
{
    static const WCHAR ok[] = u"OK", cancel[] = u"Cancel", abort_[] = u"Abort", retry[] = u"Retry",
                       ignore_[] = u"Ignore", yes[] = u"Yes", no[] = u"No", try_[] = u"Try Again",
                       cont[] = u"Continue", help[] = u"Help";
    mb_set_item set[MB_MAX_BTNS];
    int k = 0;
    switch (type & MB_TYPEMASK) {
    case 1: /* MB_OKCANCEL */
        set[k++] = (mb_set_item){IDOK, ok};
        set[k++] = (mb_set_item){IDCANCEL, cancel};
        break;
    case 2: /* MB_ABORTRETRYIGNORE */
        set[k++] = (mb_set_item){IDABORT, abort_};
        set[k++] = (mb_set_item){IDRETRY, retry};
        set[k++] = (mb_set_item){IDIGNORE, ignore_};
        break;
    case 3: /* MB_YESNOCANCEL */
        set[k++] = (mb_set_item){IDYES, yes};
        set[k++] = (mb_set_item){IDNO, no};
        set[k++] = (mb_set_item){IDCANCEL, cancel};
        break;
    case 4: /* MB_YESNO */
        set[k++] = (mb_set_item){IDYES, yes};
        set[k++] = (mb_set_item){IDNO, no};
        break;
    case 5: /* MB_RETRYCANCEL */
        set[k++] = (mb_set_item){IDRETRY, retry};
        set[k++] = (mb_set_item){IDCANCEL, cancel};
        break;
    case 6: /* MB_CANCELTRYCONTINUE */
        set[k++] = (mb_set_item){IDCANCEL, cancel};
        set[k++] = (mb_set_item){IDTRYAGAIN, try_};
        set[k++] = (mb_set_item){IDCONTINUE, cont};
        break;
    default: /* MB_OK */
        set[k++] = (mb_set_item){IDOK, ok};
        break;
    }
    if ((type & MB_HELP) && k < MB_MAX_BTNS)
        set[k++] = (mb_set_item){IDHELP, help};
    for (int i = 0; i < k; i++) {
        b[i].id = set[i].id;
        b[i].label = set[i].label;
        b[i].w = 0;
        memset(&b[i].r, 0, sizeof b[i].r);
    }
    return k;
}

static int mb_default_index(UINT type, int nbtn)
{
    int d = (int)((type & MB_DEFMASK) >> 8);
    if (d >= nbtn)
        d = nbtn - 1;
    return d < 0 ? 0 : d;
}

static int mb_icon_for(UINT type)
{
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: return 32513; /* IDI_ERROR */
    case MB_ICONQUESTION: return 32514; /* IDI_QUESTION */
    case MB_ICONEXCLAMATION: return 32515; /* IDI_WARNING */
    case MB_ICONASTERISK: return 32516; /* IDI_INFORMATION */
    default: return 0;
    }
}

/* id that Esc and the close button produce: Cancel if the set has one, else OK, else the default */
static int mb_cancel_id(const mb_state *s)
{
    for (int i = 0; i < s->nbtn; i++)
        if (s->btn[i].id == IDCANCEL)
            return IDCANCEL;
    for (int i = 0; i < s->nbtn; i++)
        if (s->btn[i].id == IDOK)
            return IDOK;
    return s->btn[s->defbtn].id;
}

/* ------------------------------------------------------------------ layout and paint */
static void mb_layout(mb_state *s, HDC dc, int screen_w)
{
    int max_text_w = screen_w * 2 / 3 - 80;
    if (max_text_w > 560)
        max_text_w = 560;
    if (max_text_w < 200)
        max_text_w = 200;
    int left = s->icon ? MB_MARGIN + 32 + MB_MARGIN : MB_MARGIN;
    RECT tr = {0, 0, max_text_w, 4000};
    __aw_user32_DrawTextW(dc, s->text ? s->text : u"", -1, &tr, MB_DT_WORDBREAK | MB_DT_CALCRECT);
    int text_w = tr.right, text_h = tr.bottom;
    if (text_h < 20)
        text_h = 20;
    s->text_r.left = left;
    s->text_r.top = MB_MARGIN;
    s->text_r.right = left + text_w;
    s->text_r.bottom = MB_MARGIN + text_h;

    int btns_w = 0;
    for (int i = 0; i < s->nbtn; i++) {
        SIZE sz = {0, 0};
        __aw_gdi32_GetTextExtentPoint32W(dc, s->btn[i].label, (int)aw_wcslen(s->btn[i].label), &sz);
        int bw = (int)sz.cx + 24;
        if (bw < MB_BTN_MIN_W)
            bw = MB_BTN_MIN_W;
        s->btn[i].w = bw;
        btns_w += bw;
    }
    btns_w += MB_BTN_GAP * (s->nbtn > 0 ? s->nbtn - 1 : 0);

    int icon_bottom = MB_MARGIN + (s->icon ? 32 : 0);
    int body_h = icon_bottom > s->text_r.bottom ? icon_bottom : s->text_r.bottom;
    s->client_w = left + text_w + MB_MARGIN;
    if (s->client_w < btns_w + 2 * MB_MARGIN)
        s->client_w = btns_w + 2 * MB_MARGIN;
    if (s->client_w < 280)
        s->client_w = 280;
    s->client_h = body_h + 20 + MB_BTN_H + MB_MARGIN;

    /* buttons right-aligned in the bottom row */
    int x = s->client_w - MB_MARGIN - btns_w;
    int y = s->client_h - MB_MARGIN - MB_BTN_H;
    for (int i = 0; i < s->nbtn; i++) {
        int bw = s->btn[i].w;
        s->btn[i].r.left = x;
        s->btn[i].r.top = y;
        s->btn[i].r.right = x + bw;
        s->btn[i].r.bottom = y + MB_BTN_H;
        x += bw + MB_BTN_GAP;
    }
}

static void mb_fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH br = __aw_gdi32_CreateSolidBrush(c);
    if (!br)
        return;
    __aw_user32_FillRect(dc, r, br);
    __aw_gdi32_DeleteObject(br);
}

static void mb_frame(HDC dc, const RECT *r, COLORREF c)
{
    RECT t = {r->left, r->top, r->right, r->top + 1};
    RECT b = {r->left, r->bottom - 1, r->right, r->bottom};
    RECT l = {r->left, r->top, r->left + 1, r->bottom};
    RECT rr = {r->right - 1, r->top, r->right, r->bottom};
    mb_fill(dc, &t, c);
    mb_fill(dc, &b, c);
    mb_fill(dc, &l, c);
    mb_fill(dc, &rr, c);
}

static void mb_paint(mb_state *s, HDC dc)
{
    RECT all = {0, 0, s->client_w, s->client_h};
    mb_fill(dc, &all, MB_RGB_WHITE);
    __aw_gdi32_SelectObject(dc, __aw_gdi32_GetStockObject(17 /* DEFAULT_GUI_FONT */));
    __aw_gdi32_SetBkMode(dc, 1 /* TRANSPARENT */);
    __aw_gdi32_SetTextColor(dc, MB_RGB_TEXT);
    if (s->icon) {
        HICON ic = __aw_user32_LoadIconW(NULL, (LPCWSTR)(uintptr_t)s->icon);
        if (ic)
            __aw_user32_DrawIconEx(dc, MB_MARGIN, MB_MARGIN, ic, 32, 32, 0, NULL, MB_DI_NORMAL);
    }
    RECT tr = s->text_r;
    __aw_user32_DrawTextW(dc, s->text ? s->text : u"", -1, &tr, MB_DT_WORDBREAK | MB_DT_LEFT);
    for (int i = 0; i < s->nbtn; i++) {
        mb_btn *b = &s->btn[i];
        int down = (i == s->pressed);
        mb_fill(dc, &b->r, down ? MB_RGB_BTN_DOWN : MB_RGB_BTN);
        mb_frame(dc, &b->r, i == s->defbtn ? MB_RGB_DEFAULT : MB_RGB_BTN_EDGE);
        if (i == s->focus) {
            RECT in = {b->r.left + 3, b->r.top + 3, b->r.right - 3, b->r.bottom - 3};
            mb_frame(dc, &in, MB_RGB_FOCUS);
        }
        RECT tb = b->r;
        __aw_user32_DrawTextW(dc, b->label, -1, &tb, MB_DT_CENTER | MB_DT_VCENTER | MB_DT_SINGLELINE);
    }
}

/* ------------------------------------------------------------------ state changes */
static void mb_end(mb_state *s, int id)
{
    s->result = id;
    s->done = 1;
}

static void mb_move_focus(mb_state *s, int dir)
{
    if (s->nbtn == 0)
        return;
    s->focus = (s->focus + dir + s->nbtn) % s->nbtn;
    __aw_user32_InvalidateRect(s->hwnd, NULL, FALSE);
}

static void mb_key(mb_state *s, UINT vk)
{
    switch (vk) {
    case 0x0D: /* VK_RETURN */
    case 0x20: /* VK_SPACE */
        mb_end(s, s->btn[s->focus].id);
        break;
    case 0x1B: /* VK_ESCAPE */
        mb_end(s, mb_cancel_id(s));
        break;
    case 0x25: /* VK_LEFT */
    case 0x26: /* VK_UP */
        mb_move_focus(s, -1);
        break;
    case 0x27: /* VK_RIGHT */
    case 0x28: /* VK_DOWN */
    case 0x09: /* VK_TAB */
        mb_move_focus(s, 1);
        break;
    default:
        break;
    }
}

static int mb_button_at(const mb_state *s, int x, int y)
{
    for (int i = 0; i < s->nbtn; i++) {
        const RECT *r = &s->btn[i].r;
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ window procedure */
static LRESULT AWCALL mb_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    mb_state *s = g_mb;
    if (!s || s->hwnd != h)
        return __aw_user32_DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        AW_PAINTSTRUCT ps;
        HDC dc = __aw_user32_BeginPaint(h, &ps);
        if (dc)
            mb_paint(s, dc);
        __aw_user32_EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        mb_key(s, (UINT)wp);
        return 0;
    case WM_LBUTTONDOWN:
        s->pressed = mb_button_at(s, (short)LOWORD(lp), (short)HIWORD(lp));
        __aw_user32_InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int i = mb_button_at(s, (short)LOWORD(lp), (short)HIWORD(lp));
        int was = s->pressed;
        s->pressed = -1;
        if (i >= 0 && i == was)
            mb_end(s, s->btn[i].id);
        else
            __aw_user32_InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        mb_end(s, mb_cancel_id(s));
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != 0)
            __aw_user32_SetFocus(h);
        return 0;
    default:
        return __aw_user32_DefWindowProcW(h, msg, wp, lp);
    }
}

static void mb_register(void)
{
    if (g_mb_class_done)
        return;
    g_mb_class_done = 1;
    AW_WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = mb_wndproc;
    wc.lpszClassName = MB_WND_CLASS;
    __aw_user32_RegisterClassExW(&wc);
}

/* ------------------------------------------------------------------ show */
static int mb_show(HWND owner, const WCHAR *text, const WCHAR *caption, UINT type)
{
    mb_state s;
    memset(&s, 0, sizeof s);
    s.owner = owner;
    s.text = text;
    s.type = type;
    s.pressed = -1;
    s.nbtn = mb_build_buttons(type, s.btn);
    s.defbtn = mb_default_index(type, s.nbtn);
    s.focus = s.defbtn;
    s.icon = mb_icon_for(type);
    aw_user_init();

    if (!aw_display) {
        /* no display: report on stderr and answer with the default button */
        char *t = aw_w2u(text ? text : u"", -1);
        char *c = aw_w2u(caption ? caption : u"", -1);
        fprintf(stderr, "%s: %s\n", c ? c : "", t ? t : "");
        free(t);
        free(c);
        return s.btn[s.defbtn].id;
    }

    int sw = 0, sh = 0;
    plat_screen_size(&sw, &sh);
    HDC dc = __aw_gdi32_CreateCompatibleDC(NULL);
    if (dc) {
        __aw_gdi32_SelectObject(dc, __aw_gdi32_GetStockObject(17 /* DEFAULT_GUI_FONT */));
        mb_layout(&s, dc, sw);
        __aw_gdi32_DeleteDC(dc);
    } else {
        s.client_w = 320;
        s.client_h = 120;
    }

    int x = (sw - s.client_w) / 2, y = (sh - s.client_h) / 3;
    if (owner) {
        RECT or_;
        if (__aw_user32_GetWindowRect(owner, &or_)) {
            x = or_.left + ((or_.right - or_.left) - s.client_w) / 2;
            y = or_.top + ((or_.bottom - or_.top) - s.client_h) / 3;
        }
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    mb_register();
    int owner_was_enabled = owner ? __aw_user32_IsWindowEnabled(owner) : 0;
    mb_state *saved = g_mb;
    g_mb = &s;
    s.hwnd = __aw_user32_CreateWindowExW(0, MB_WND_CLASS, caption ? caption : u"", MB_WS_CAPTION_DLG, x, y,
                                         s.client_w, s.client_h, owner, NULL, NULL, NULL);
    if (!s.hwnd) {
        g_mb = saved;
        return s.btn[s.defbtn].id;
    }
    if (owner)
        __aw_user32_EnableWindow(owner, FALSE);
    __aw_user32_ShowWindow(s.hwnd, 1 /* SW_SHOWNORMAL */);
    __aw_user32_SetForegroundWindow(s.hwnd);
    __aw_user32_SetFocus(s.hwnd);
    __aw_user32_InvalidateRect(s.hwnd, NULL, TRUE);

    aw_modal_loop(&s.done);

    if (owner && owner_was_enabled)
        __aw_user32_EnableWindow(owner, TRUE);
    __aw_user32_DestroyWindow(s.hwnd);
    if (owner && owner_was_enabled)
        __aw_user32_SetForegroundWindow(owner);
    g_mb = saved;
    return s.result;
}

API(user32, int, MessageBoxW, (HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType))
{
    return mb_show(hWnd, lpText, lpCaption, uType);
}

static int mb_show_ansi(HWND owner, LPCSTR text, LPCSTR caption, UINT type)
{
    WCHAR *t = aw_u2w(text ? text : "", -1);
    WCHAR *c = aw_u2w(caption ? caption : "", -1);
    int r = mb_show(owner, t, c, type);
    free(t);
    free(c);
    return r;
}

API(user32, int, MessageBoxA, (HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType))
{
    return mb_show_ansi(hWnd, lpText, lpCaption, uType);
}

API(user32, int, MessageBoxExW, (HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType, WORD wLanguageId))
{
    (void)wLanguageId;
    return mb_show(hWnd, lpText, lpCaption, uType);
}

API(user32, int, MessageBoxExA, (HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType, WORD wLanguageId))
{
    (void)wLanguageId;
    return mb_show_ansi(hWnd, lpText, lpCaption, uType);
}

API(user32, int, MessageBoxIndirectW, (const AW_MSGBOXPARAMSW *p))
{
    if (!p)
        return 0;
    return mb_show(p->hwndOwner, p->lpszText, p->lpszCaption, p->dwStyle);
}

API(user32, int, MessageBoxIndirectA, (const AW_MSGBOXPARAMSA *p))
{
    if (!p)
        return 0;
    return mb_show_ansi(p->hwndOwner, p->lpszText, p->lpszCaption, p->dwStyle);
}

API(user32, BOOL, MessageBeep, (UINT uType))
{
    (void)uType;
    plat_bell();
    return TRUE;
}
