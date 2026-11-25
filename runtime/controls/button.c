/* anywin runtime - Button control */
#include "ctl.h"

#define PS_SOLID 0

typedef struct {
    UINT check;        /* BST_* */
    UINT state;        /* BST_PUSHED / BST_FOCUS */
    int hot;
    int tracking;
    int pressed;
    HICON icon;
    HBITMAP bitmap;
    int image_type;    /* IMAGE_ICON / IMAGE_BITMAP */
    int dontclick;
    UINT dlgcode;
} btn_t;

#define IMAGE_BITMAP 0
#define IMAGE_ICON 1

static UINT btn_style(HWND hwnd)
{
    return (UINT)__aw_user32_GetWindowLongW(hwnd, GWL_STYLE);
}

static int btn_enabled(HWND hwnd)
{
    return __aw_user32_IsWindowEnabled(hwnd);
}

static void btn_notify(HWND hwnd, UINT code)
{
    btn_t *b = ctl_get(hwnd);
    int style = (int)btn_style(hwnd);
    if (style & BS_NOTIFY)
        ctl_command(hwnd, code);
    else if (code == BN_CLICKED)
        ctl_command(hwnd, code);
    (void)b;
}

static void btn_draw_check(HDC hdc, const RECT *box, int state, int radio, int hot, int enabled)
{
    COLORREF border = (hot || state) ? RGB(0x00, 0x78, 0xD7) : RGB(0xAD, 0xAD, 0xAD);
    COLORREF fill = enabled ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xF0, 0xF0, 0xF0);
    HBRUSH br;
    HPEN pen;
    HGDIOBJ oldb, oldp;
    RECT b = *box;

    if (radio) {
        pen = __aw_gdi32_CreatePen(PS_SOLID, 1, border);
        oldp = __aw_gdi32_SelectObject(hdc, pen);
        oldb = __aw_gdi32_SelectObject(hdc, __aw_gdi32_GetStockObject(NULL_BRUSH));
        __aw_gdi32_Ellipse(hdc, b.left, b.top, b.right, b.bottom);
        if (state == BST_CHECKED) {
            br = __aw_gdi32_CreateSolidBrush(enabled ? RGB(0x33, 0x33, 0x33) : RGB(0x90, 0x90, 0x90));
            oldp = __aw_gdi32_SelectObject(hdc, br);
            __aw_gdi32_Ellipse(hdc, b.left + 4, b.top + 4, b.right - 4, b.bottom - 4);
            __aw_gdi32_SelectObject(hdc, oldp);
            __aw_gdi32_DeleteObject(br);
        }
        __aw_gdi32_SelectObject(hdc, oldb);
        __aw_gdi32_SelectObject(hdc, oldp);
        __aw_gdi32_DeleteObject(pen);
        return;
    }

    ctl_fill(hdc, &b, (HBRUSH)__aw_gdi32_GetStockObject(WHITE_BRUSH));
    {
        HBRUSH fb = __aw_gdi32_CreateSolidBrush(fill);
        __aw_user32_FillRect(hdc, &b, fb);
        __aw_gdi32_DeleteObject(fb);
    }
    ctl_frame(hdc, &b, border);
    if (state == BST_CHECKED || state == BST_INDETERMINATE) {
        pen = __aw_gdi32_CreatePen(PS_SOLID, 2, enabled ? RGB(0x33, 0x33, 0x33) : RGB(0x90, 0x90, 0x90));
        oldp = __aw_gdi32_SelectObject(hdc, pen);
        if (state == BST_INDETERMINATE) {
            RECT r;
            br = __aw_gdi32_CreateSolidBrush(RGB(0x33, 0x33, 0x33));
            r.left = b.left + 3;
            r.top = b.top + 3;
            r.right = b.right - 3;
            r.bottom = b.bottom - 3;
            __aw_user32_FillRect(hdc, &r, br);
            __aw_gdi32_DeleteObject(br);
        } else {
            __aw_gdi32_MoveToEx(hdc, b.left + 3, b.top + 6, NULL);
            __aw_gdi32_LineTo(hdc, b.left + 6, b.top + 9);
            __aw_gdi32_LineTo(hdc, b.left + 10, b.top + 3);
        }
        __aw_gdi32_SelectObject(hdc, oldp);
        __aw_gdi32_DeleteObject(pen);
    }
}

static void btn_draw(btn_t *b, HWND hwnd, HDC hdc, const RECT *rc, HBRUSH bg)
{
    UINT style = btn_style(hwnd);
    UINT type = style & BS_TYPEMASK;
    RECT r = *rc;
    RECT tr;
    int enabled = btn_enabled(hwnd);
    int focused = (__aw_user32_GetFocus() == hwnd);
    COLORREF face = (COLORREF)aw_syscolor(COLOR_BTNFACE);
    COLORREF textc = enabled ? (COLORREF)aw_syscolor(COLOR_BTNTEXT) : (COLORREF)aw_syscolor(COLOR_GRAYTEXT);
    int pushed = (b->state & BST_PUSHED) != 0;
    int checked = (b->check == BST_CHECKED);
    WCHAR text[512];
    HFONT font = ctl_font(hwnd);

    __aw_user32_GetWindowTextW(hwnd, text, 512);

    if (!enabled)
        textc = (COLORREF)aw_syscolor(COLOR_GRAYTEXT);

    if (type == BS_OWNERDRAW) {
        DRAWITEMSTRUCT dis;
        memset(&dis, 0, sizeof(dis));
        dis.CtlType = ODT_BUTTON;
        dis.CtlID = (UINT)__aw_user32_GetDlgCtrlID(hwnd);
        dis.itemAction = ODA_DRAWENTIRE;
        dis.itemState = (b->check == BST_CHECKED ? ODS_CHECKED : 0) | (pushed ? ODS_SELECTED : 0) |
                        (focused ? ODS_FOCUS : 0) | (enabled ? 0 : ODS_DISABLED) | (b->hot ? ODS_HOTLIGHT : 0);
        dis.hwndItem = hwnd;
        dis.hDC = hdc;
        dis.rcItem = r;
        if (bg)
            ctl_fill(hdc, &r, bg);
        ctl_send_parent(hwnd, WM_DRAWITEM, dis.CtlID, (LPARAM)&dis);
        if (focused)
            ctl_draw_focus(hdc, &r);
        return;
    }

    if (type == BS_GROUPBOX) {
        int ty = r.top + ctl_font_height(hwnd) / 2;
        SIZE sz = ctl_measure(hwnd, font, text, -1);
        RECT fr = r;
        if (bg)
            ctl_fill(hdc, &r, bg);
        fr.top = ty;
        ctl_frame(hdc, &fr, RGB(0xAD, 0xAD, 0xAD));
        if (text[0]) {
            RECT tx;
            tx.left = r.left + 8;
            tx.top = r.top;
            tx.right = tx.left + sz.cx + 4;
            tx.bottom = r.top + ctl_font_height(hwnd);
            if (bg)
                ctl_fill(hdc, &tx, bg);
            tx.left += 2;
            ctl_text(hdc, font, text, tx, DT_LEFT | DT_TOP | DT_SINGLELINE, textc);
        }
        return;
    }

    if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE ||
        type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {
        int radio = (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON);
        RECT box;
        int bh = radio ? 12 : 13;
        int top = r.top + (r.bottom - r.top - bh) / 2;
        int text_left;
        if (style & BS_PUSHLIKE) {
            goto push_like;
        }
        if (bg)
            ctl_fill(hdc, &r, bg);
        box.left = r.left + 2;
        box.top = top;
        box.right = box.left + bh;
        box.bottom = box.top + bh;
        btn_draw_check(hdc, &box, b->check, radio, b->hot, enabled);
        tr = r;
        if (style & BS_LEFTTEXT) {
            box.left = r.right - bh - 2;
            box.right = box.left + bh;
            btn_draw_check(hdc, &box, b->check, radio, b->hot, enabled);
            tr.right = box.left - 4;
            tr.left += 2;
            ctl_text(hdc, font, text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | (style & BS_MULTILINE ? DT_WORDBREAK : 0), textc);
        } else {
            text_left = box.right + 4;
            tr.left = text_left;
            tr.right = r.right - 2;
            ctl_text(hdc, font, text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | (style & BS_MULTILINE ? DT_WORDBREAK : 0), textc);
        }
        if (focused)
            ctl_draw_focus(hdc, &box);
        return;
    }

push_like:
    /* push button / push-like check boxes */
    {
        COLORREF border = RGB(0xAD, 0xAD, 0xAD);
        COLORREF fill = face;
        RECT br = r;
        HBRUSH fb;
        if (type == BS_DEFPUSHBUTTON || type == BS_DEFSPLITBUTTON)
            border = RGB(0x00, 0x78, 0xD7);
        if (b->hot) {
            border = RGB(0x00, 0x78, 0xD7);
            fill = RGB(0xE5, 0xF1, 0xFB);
        }
        if (pushed || (checked && (style & BS_PUSHLIKE))) {
            border = RGB(0x00, 0x78, 0xD7);
            fill = RGB(0xCC, 0xE4, 0xF7);
        }
        if (style & BS_FLAT)
            border = b->hot ? RGB(0x00, 0x78, 0xD7) : face;
        if (!enabled)
            fill = face;
        if (bg)
            ctl_fill(hdc, &r, bg);
        fb = __aw_gdi32_CreateSolidBrush(fill);
        __aw_user32_FillRect(hdc, &br, fb);
        __aw_gdi32_DeleteObject(fb);
        ctl_frame(hdc, &br, border);
        if (type == BS_DEFPUSHBUTTON) {
            RECT in = br;
            in.left++;
            in.top++;
            in.right--;
            in.bottom--;
            ctl_frame(hdc, &in, RGB(0x00, 0x78, 0xD7));
        }
        if (style & BS_FLAT) {
            ctl_hline(hdc, br.left, br.right, br.top, RGB(0xD0, 0xD0, 0xD0));
        }
        tr = br;
        tr.left += 6;
        tr.right -= 6;
        if (pushed) {
            tr.left++;
            tr.top++;
        }
        if (b->image_type && (style & (BS_ICON | BS_BITMAP))) {
            int iw = 16, ih = 16;
            int x, y;
            if (b->image_type == IMAGE_ICON && b->icon) {
                aw_icon *ic = aw_icon_from_handle(b->icon);
                if (ic) {
                    iw = 16;
                    ih = 16;
                }
                x = tr.left + (tr.right - tr.left - iw) / 2;
                y = tr.top + (tr.bottom - tr.top - ih) / 2;
                __aw_user32_DrawIconEx(hdc, x, y, b->icon, iw, ih, 0, NULL, 0);
            } else if (b->bitmap) {
                aw_surface *s = gdi_bitmap_surface(b->bitmap);
                if (s) {
                    x = tr.left + (tr.right - tr.left - s->w) / 2;
                    y = tr.top + (tr.bottom - tr.top - s->h) / 2;
                    __aw_gdi32_BitBlt(hdc, x, y, s->w, s->h, NULL, 0, 0, 0);
                }
            }
        } else {
            UINT fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
            if (style & BS_LEFT)
                fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
            if (style & BS_RIGHT)
                fmt = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
            if (style & BS_MULTILINE)
                fmt = (fmt & ~DT_SINGLELINE) | DT_WORDBREAK;
            if (style & BS_TOP)
                fmt = (fmt & ~DT_VCENTER) | DT_TOP;
            if (style & BS_BOTTOM)
                fmt = (fmt & ~DT_VCENTER) | DT_BOTTOM;
            ctl_text(hdc, font, text, tr, fmt, textc);
        }
        if (focused)
            ctl_draw_focus(hdc, &br);
    }
}

static void btn_toggle(HWND hwnd, btn_t *b, int notify)
{
    UINT style = btn_style(hwnd);
    UINT type = style & BS_TYPEMASK;
    if (type == BS_AUTOCHECKBOX || type == BS_AUTO3STATE) {
        if (type == BS_AUTO3STATE) {
            b->check = (b->check + 1) % 3;
        } else {
            b->check = b->check == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED;
        }
    } else if (type == BS_AUTORADIOBUTTON) {
        b->check = BST_CHECKED;
        /* clear the rest of the group */
        HWND w = hwnd;
        HWND parent = __aw_user32_GetParent(hwnd);
        if (parent) {
            while (1) {
                HWND prev = __aw_user32_GetWindow(w, GW_HWNDPREV);
                if (!prev)
                    break;
                if (__aw_user32_GetWindowLongW(prev, GWL_STYLE) & WS_GROUP) {
                    break;
                }
                w = prev;
            }
            w = hwnd;
            while (1) {
                HWND nx;
                if (w != hwnd) {
                    UINT s2 = (UINT)__aw_user32_GetWindowLongW(w, GWL_STYLE);
                    UINT t2 = s2 & BS_TYPEMASK;
                    if (t2 == BS_RADIOBUTTON || t2 == BS_AUTORADIOBUTTON) {
                        __aw_user32_SendMessageW(w, BM_SETCHECK, BST_UNCHECKED, 0);
                    }
                }
                nx = __aw_user32_GetWindow(w, GW_HWNDNEXT);
                if (!nx)
                    break;
                if (__aw_user32_GetWindowLongW(nx, GWL_STYLE) & WS_GROUP)
                    break;
                w = nx;
            }
        }
    } else if (type == BS_3STATE) {
        /* application driven */
    } else if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
        /* nothing */
    }
    __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
    if (notify)
        btn_notify(hwnd, BN_CLICKED);
}

static void btn_press(HWND hwnd, btn_t *b, int down)
{
    if (down) {
        b->state |= BST_PUSHED;
        b->pressed = 1;
        __aw_user32_SetCapture(hwnd);
    } else {
        b->state &= ~(UINT)BST_PUSHED;
        b->pressed = 0;
        if (__aw_user32_GetCapture() == hwnd)
            __aw_user32_ReleaseCapture();
    }
    __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT btn_msg(HWND hwnd, btn_t *b, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        b->check = BST_UNCHECKED;
        return 0;
    case WM_PAINT: {
        HDC hdc;
        PAINTSTRUCT ps;
        RECT rc;
        HBRUSH bg;
        if (!ctl_begin(hwnd, WM_CTLCOLORBTN, &hdc, &ps, &rc, &bg))
            return 0;
        btn_draw(b, hwnd, hdc, &rc, bg);
        ctl_end(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT rc;
        __aw_user32_GetClientRect(hwnd, &rc);
        btn_draw(b, hwnd, (HDC)wp, &rc, NULL);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (!btn_enabled(hwnd))
            return 0;
        __aw_user32_SetFocus(hwnd);
        if (btn_style(hwnd) & BS_OWNERDRAW) {
            b->pressed = 1;
            __aw_user32_SetCapture(hwnd);
            __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        } else {
            btn_press(hwnd, b, 1);
        }
        return 0;
    case WM_LBUTTONUP:
        if (b->pressed) {
            RECT rc;
            POINT pt;
            pt.x = (int)(short)LOWORD(lp);
            pt.y = (int)(short)HIWORD(lp);
            __aw_user32_GetClientRect(hwnd, &rc);
            btn_press(hwnd, b, 0);
            b->pressed = 0;
            if (pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom) {
                if (!b->dontclick)
                    btn_toggle(hwnd, b, 1);
            }
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (btn_style(hwnd) & BS_NOTIFY)
            btn_notify(hwnd, BN_DBLCLK);
        else
            __aw_user32_SendMessageW(hwnd, WM_LBUTTONDOWN, wp, lp);
        return 0;
    case WM_MOUSEMOVE:
        if (!b->hot) {
            b->hot = 1;
            __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        }
        if (!b->tracking) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TM_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            b->tracking = __aw_user32_TrackMouseEvent(&tme) ? 1 : 0;
        }
        return 0;
    case WM_MOUSELEAVE:
        b->hot = 0;
        b->tracking = 0;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE) {
            if (!b->pressed)
                btn_press(hwnd, b, 1);
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wp == VK_SPACE) {
            if (b->pressed) {
                btn_press(hwnd, b, 0);
                if (!b->dontclick)
                    btn_toggle(hwnd, b, 1);
            }
            return 0;
        }
        break;
    case WM_GETDLGCODE: {
        UINT st = btn_style(hwnd);
        UINT t = st & BS_TYPEMASK;
        LRESULT r = DLGC_BUTTON;
        if (t == BS_DEFPUSHBUTTON)
            r |= DLGC_DEFPUSHBUTTON;
        else if (t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON)
            r |= DLGC_RADIOBUTTON;
        else if (t == BS_PUSHBUTTON)
            r |= DLGC_UNDEFPUSHBUTTON;
        r |= DLGC_WANTCHARS;
        return r;
    }
    case WM_SETFOCUS:
        b->state |= BST_FOCUS;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        if (btn_style(hwnd) & BS_NOTIFY)
            btn_notify(hwnd, BN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        b->state &= ~(UINT)BST_FOCUS;
        if (b->pressed)
            btn_press(hwnd, b, 0);
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        if (btn_style(hwnd) & BS_NOTIFY)
            btn_notify(hwnd, BN_KILLFOCUS);
        return 0;
    case WM_ENABLE:
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case BM_GETCHECK:
        return b->check;
    case BM_SETCHECK:
        b->check = (UINT)wp & 3;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case BM_GETSTATE: {
        LRESULT r = b->state;
        if (__aw_user32_GetFocus() == hwnd)
            r |= BST_FOCUS;
        return r;
    }
    case BM_SETSTATE:
        if (wp)
            b->state |= BST_PUSHED;
        else
            b->state &= ~(UINT)BST_PUSHED;
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case BM_SETSTYLE:
        __aw_user32_SetWindowLongW(hwnd, GWL_STYLE, (LONG)wp);
        if (lp)
            __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case BM_CLICK:
        btn_press(hwnd, b, 1);
        btn_press(hwnd, b, 0);
        if (!b->dontclick)
            btn_toggle(hwnd, b, 1);
        return 0;
    case BM_GETIMAGE:
        if (wp == IMAGE_BITMAP)
            return (LRESULT)(intptr_t)b->bitmap;
        return (LRESULT)(intptr_t)b->icon;
    case BM_SETIMAGE: {
        LRESULT old;
        if (wp == IMAGE_BITMAP) {
            old = (LRESULT)(intptr_t)b->bitmap;
            b->bitmap = (HBITMAP)lp;
            b->image_type = IMAGE_BITMAP;
        } else {
            old = (LRESULT)(intptr_t)b->icon;
            b->icon = (HICON)lp;
            b->image_type = IMAGE_ICON;
        }
        __aw_user32_InvalidateRect(hwnd, NULL, FALSE);
        return old;
    }
    case BM_SETDONTCLICK:
        b->dontclick = (int)wp;
        return 0;
    case WM_SETTEXT:
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        break;
    case WM_SIZE:
        __aw_user32_InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    default:
        break;
    }
    return -1;
}

LRESULT WINAPI aw_btn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    btn_t *b = ctl_get(hwnd);
    LRESULT res;

    if (ctl_common_msg(hwnd, msg, wp, lp, &res))
        return res;

    if (msg == WM_CREATE) {
        b = ctl_alloc(hwnd, sizeof(btn_t));
        if (!b)
            return -1;
        b->check = BST_UNCHECKED;
        b->dlgcode = 0;
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        ctl_free(hwnd);
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!b)
        return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
    {
        LRESULT r = btn_msg(hwnd, b, msg, wp, lp);
        if (r != -1)
            return r;
    }
    return __aw_user32_DefWindowProcW(hwnd, msg, wp, lp);
}
