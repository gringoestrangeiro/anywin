/* user32 core: init, classes/atoms, window objects, creation/destruction,
 * hierarchy, window data, text and properties. */
#include "user32_int.h"
#include <stddef.h>
#include <stdint.h>

#define HWND_BASE 0x10000u
#define HWND_INDEX_MASK 0xFFFu
#define ERR_CLASS_NOT_FOUND 1407
#define ERR_CLASS_ALREADY_EXISTS 1410
#define ERR_CLASS_HAS_WINDOWS 1411
#define ERR_INVALID_INDEX 1413
#define ERR_INVALID_WINDOW 1400
#define ERR_NOT_ENOUGH_MEMORY 8

pthread_mutex_t aw_wnd_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static aw_win g_windows[AW_MAX_WINDOWS];
static aw_wndclass g_classes[AW_MAX_CLASSES];
static aw_atom_entry g_atoms[AW_MAX_ATOMS];
static int g_atom_next = AW_ATOM_BASE;
static HWND g_first_top, g_last_top;
static HWND g_desktop; /* top-level parent sentinel; set by user init (S5) */
static int g_cascade;
static int g_init_done, g_init_running, g_display;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
extern HMODULE WINAPI __aw_kernel32_GetModuleHandleW(LPCWSTR);
extern HMODULE WINAPI __aw_kernel32_GetModuleHandleA(LPCSTR);

/* ------------------------------------------------------------------ helpers */
static WCHAR wlower(WCHAR c)
{
    if (c >= 'A' && c <= 'Z')
        return (WCHAR)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return (WCHAR)(c + 32);
    return c;
}

static int wcmp_ci(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR ca = wlower(*a), cb = wlower(*b);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (!ca)
            return 0;
    }
}

static int wstr_len(const WCHAR *s, int max)
{
    int n = 0;
    if (s)
        while (s[n] && n < max)
            n++;
    return n;
}

static void wcopy(WCHAR *d, const WCHAR *s, int n)
{
    int i = 0;
    for (; i < n - 1 && s && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

static aw_win *win_from_h(HWND h, int lock)
{
    uintptr_t v = (uintptr_t)h;
    if (lock)
        pthread_mutex_lock(&aw_wnd_lock);
    aw_win *w = NULL;
    if (v >= HWND_BASE && v <= (HWND_BASE | 0x1FFF)) {
        unsigned idx = (unsigned)((v - HWND_BASE) >> 1) & HWND_INDEX_MASK;
        aw_win *t = &g_windows[idx];
        if (t->used && t->hwnd == h)
            w = t;
    }
    if (lock)
        pthread_mutex_unlock(&aw_wnd_lock);
    return w;
}

aw_win *aw_win_from_hwnd(HWND h) { return win_from_h(h, 1); }
int aw_hwnd_valid(HWND h) { return win_from_h(h, 0) != NULL; }

aw_win *aw_win_from_xwin(uint32_t xwin)
{
    if (!xwin)
        return NULL;
    for (int i = 0; i < AW_MAX_WINDOWS; i++)
        if (g_windows[i].used && g_windows[i].xwin == xwin)
            return &g_windows[i];
    return NULL;
}

aw_win *aw_win_alloc(void)
{
    pthread_mutex_lock(&aw_wnd_lock);
    for (int i = 0; i < AW_MAX_WINDOWS; i++) {
        aw_win *w = &g_windows[i];
        if (!w->used) {
            memset(w, 0, sizeof *w);
            w->used = 1;
            w->gen = (uint16_t)(w->gen + 1);
            w->hwnd = (HWND)(uintptr_t)(HWND_BASE | ((uint32_t)i << 1) | (w->gen & 1));
            w->enabled = 1;
            w->redraw = 1;
            w->place.length = sizeof(WINDOWPLACEMENT);
            w->place.showCmd = SW_SHOWNORMAL;
            w->hsi.cbSize = w->vsi.cbSize = sizeof(SCROLLINFO);
            w->hsi.nMax = 100;
            w->vsi.nMax = 100;
            w->tid = aw_current_tid();
            rgn_init(&w->update);
            rgn_init(&w->wnd_rgn);
            pthread_mutex_unlock(&aw_wnd_lock);
            return w;
        }
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    return NULL;
}

static void win_cleanup(aw_win *w)
{
    struct aw_prop *p = w->props;
    while (p) {
        struct aw_prop *n = p->next;
        free(p->name);
        free(p);
        p = n;
    }
    w->props = NULL;
    free(w->text);
    w->text = NULL;
    free(w->extra);
    w->extra = NULL;
    rgn_free(&w->update);
    rgn_free(&w->wnd_rgn);
    if (w->surf) {
        surface_free(w->surf);
        w->surf = NULL;
    }
    if (w->xwin) {
        plat_window_destroy(w->xwin);
        w->xwin = 0;
    }
    if (w->owndc) {
        gdi_delete_window_dc(w->owndc);
        w->owndc = NULL;
    }
    w->classdc = NULL;
    w->destroyed = 1;
    w->used = 0;
}

/* ------------------------------------------------------------------ atoms */
static aw_atom_entry *atom_slot(ATOM a)
{
    if (a < AW_ATOM_BASE || a >= AW_ATOM_BASE + AW_MAX_ATOMS)
        return NULL;
    return &g_atoms[a - AW_ATOM_BASE];
}

ATOM aw_atom_add(const WCHAR *name, int global)
{
    if (!name || !name[0] || aw_wcslen(name) > 255)
        return 0;
    pthread_mutex_lock(&aw_wnd_lock);
    for (int i = 0; i < AW_MAX_ATOMS; i++)
        if (g_atoms[i].used && !wcmp_ci(g_atoms[i].name, name)) {
            ATOM a = (ATOM)(AW_ATOM_BASE + i);
            g_atoms[i].global |= global;
            pthread_mutex_unlock(&aw_wnd_lock);
            return a;
        }
    for (int i = 0; i < AW_MAX_ATOMS; i++)
        if (!g_atoms[i].used) {
            g_atoms[i].name = aw_u2w(aw_w2u(name, -1), -1);
            if (!g_atoms[i].name)
                break;
            g_atoms[i].used = 1;
            g_atoms[i].global = global;
            ATOM a = (ATOM)(AW_ATOM_BASE + i);
            pthread_mutex_unlock(&aw_wnd_lock);
            return a;
        }
    pthread_mutex_unlock(&aw_wnd_lock);
    return 0;
}

ATOM aw_atom_find(const WCHAR *name)
{
    if (!name)
        return 0;
    pthread_mutex_lock(&aw_wnd_lock);
    ATOM r = 0;
    for (int i = 0; i < AW_MAX_ATOMS; i++)
        if (g_atoms[i].used && !wcmp_ci(g_atoms[i].name, name)) {
            r = (ATOM)(AW_ATOM_BASE + i);
            break;
        }
    pthread_mutex_unlock(&aw_wnd_lock);
    return r;
}

const WCHAR *aw_atom_name(ATOM a)
{
    aw_atom_entry *e = atom_slot(a);
    return e && e->used ? e->name : NULL;
}

int aw_atom_delete(ATOM a)
{
    pthread_mutex_lock(&aw_wnd_lock);
    aw_atom_entry *e = atom_slot(a);
    int ok = 0;
    if (e && e->used && !e->global) {
        free(e->name);
        memset(e, 0, sizeof *e);
        ok = 1;
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    return ok;
}

static ATOM atom_add_u(const char *s, int global)
{
    WCHAR *w = aw_u2w(s, -1);
    if (!w)
        return 0;
    ATOM a = aw_atom_add(w, global);
    free(w);
    return a;
}

/* ------------------------------------------------------------------ classes */
aw_wndclass *aw_class_find(const WCHAR *name, ATOM atom)
{
    for (int i = 0; i < AW_MAX_CLASSES; i++) {
        aw_wndclass *c = &g_classes[i];
        if (!c->used)
            continue;
        if (atom && c->atom == atom)
            return c;
        if (name && !wcmp_ci(c->name, name))
            return c;
    }
    return NULL;
}

static aw_wndclass *class_from_hwnd(HWND h)
{
    aw_win *w = aw_win_from_hwnd(h);
    return w ? w->cls : NULL;
}

static void class_free(aw_wndclass *c)
{
    free(c->menu_name);
    free(c->cls_extra);
    memset(c, 0, sizeof *c);
}

ATOM aw_register_class(const WNDCLASSA *ca, const WNDCLASSW *cw, const WNDCLASSEXA *cxa, const WNDCLASSEXW *cxw,
                       int unicode)
{
    UINT style;
    WNDPROC proc;
    int cbcls, cbwnd;
    HINSTANCE hi;
    HICON ic, ics;
    HCURSOR cur;
    HBRUSH br;
    const void *menuname;
    const WCHAR *name;

    if (unicode) {
        const WNDCLASSW *s = cw ? cw : (const WNDCLASSW *)cxw;
        if (!s && cxw)
            s = (const WNDCLASSW *)cxw;
        style = s->style;
        proc = s->lpfnWndProc;
        cbcls = s->cbClsExtra;
        cbwnd = s->cbWndExtra;
        hi = s->hInstance;
        ic = s->hIcon;
        ics = cxw ? cxw->hIconSm : s->hIcon;
        cur = s->hCursor;
        br = s->hbrBackground;
        menuname = s->lpszMenuName;
        name = s->lpszClassName;
    } else {
        const WNDCLASSA *s = ca ? ca : (const WNDCLASSA *)cxa;
        style = s->style;
        proc = s->lpfnWndProc;
        cbcls = s->cbClsExtra;
        cbwnd = s->cbWndExtra;
        hi = s->hInstance;
        ic = s->hIcon;
        ics = cxa ? cxa->hIconSm : s->hIcon;
        cur = s->hCursor;
        br = s->hbrBackground;
        menuname = s->lpszMenuName;
        name = aw_u2w((const char *)s->lpszClassName, -1);
    }
    if (!name || !name[0] || aw_wcslen(name) >= 128 || !proc) {
        if (!unicode)
            free((void *)name);
        return 0;
    }
    WCHAR *nm = aw_u2w(aw_w2u(name, -1), -1);
    if (!unicode)
        free((void *)name);
    if (!nm)
        return 0;

    pthread_mutex_lock(&aw_wnd_lock);
    if (aw_class_find(nm, 0)) {
        pthread_mutex_unlock(&aw_wnd_lock);
        free(nm);
        aw_set_last_error(ERR_CLASS_ALREADY_EXISTS);
        return 0;
    }
    ATOM atom = aw_atom_add(nm, 0);
    aw_wndclass *c = NULL;
    for (int i = 0; i < AW_MAX_CLASSES; i++)
        if (!g_classes[i].used) {
            c = &g_classes[i];
            break;
        }
    if (!c || !atom) {
        pthread_mutex_unlock(&aw_wnd_lock);
        free(nm);
        return 0;
    }
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->atom = atom;
    c->style = style;
    c->proc = proc;
    c->unicode = unicode;
    c->cb_cls = cbcls > 0 ? cbcls : 0;
    c->cb_wnd = cbwnd > 0 ? cbwnd : 0;
    c->hinst = hi;
    c->icon = ic;
    c->icon_sm = ics;
    c->cursor = cur;
    c->brush = br;
    if (c->cb_cls)
        c->cls_extra = calloc(1, (size_t)c->cb_cls);
    wcopy(c->name, nm, 128);
    if (menuname) {
        if (IS_INTRESOURCE(menuname))
            c->menu_name = (WCHAR *)(uintptr_t)(uintptr_t)menuname;
        else
            c->menu_name = aw_u2w(unicode ? (const char *)menuname : (const char *)menuname, -1);
        if (c->menu_name && !IS_INTRESOURCE(menuname) && unicode) {
            free(c->menu_name);
            c->menu_name = aw_u2w((const char *)menuname, -1);
        }
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    free(nm);
    return atom;
}

API(user32, ATOM, RegisterClassA, (const WNDCLASSA *wc))
{
    return aw_register_class(wc, NULL, NULL, NULL, 0);
}
API(user32, ATOM, RegisterClassW, (const WNDCLASSW *wc))
{
    return aw_register_class(NULL, wc, NULL, NULL, 1);
}
API(user32, ATOM, RegisterClassExA, (const WNDCLASSEXA *wc))
{
    if (!wc || wc->cbSize != sizeof(WNDCLASSEXA))
        return 0;
    return aw_register_class(NULL, NULL, wc, NULL, 0);
}
API(user32, ATOM, RegisterClassExW, (const WNDCLASSEXW *wc))
{
    if (!wc || wc->cbSize != sizeof(WNDCLASSEXW))
        return 0;
    return aw_register_class(NULL, NULL, NULL, wc, 1);
}

static int unregister_class_common(const void *name, HINSTANCE hi, int unicode)
{
    WCHAR *nm;
    ATOM atom = 0;
    if (IS_INTRESOURCE(name)) {
        atom = (ATOM)(uintptr_t)name;
        nm = NULL;
    } else if (unicode) {
        nm = aw_u2w((const char *)name, -1);
    } else {
        nm = aw_u2w((const char *)name, -1);
    }
    if (!nm && !atom)
        return FALSE;
    pthread_mutex_lock(&aw_wnd_lock);
    aw_wndclass *c = aw_class_find(nm, atom);
    int ok = FALSE;
    if (c && (!hi || !c->hinst || c->hinst == hi)) {
        if (c->refcount > 0) {
            aw_set_last_error(ERR_CLASS_HAS_WINDOWS);
        } else {
            class_free(c);
            ok = TRUE;
        }
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    free(nm);
    return ok;
}

API(user32, BOOL, UnregisterClassA, (LPCSTR name, HINSTANCE hi))
{
    return unregister_class_common(name, hi, 0);
}
API(user32, BOOL, UnregisterClassW, (LPCWSTR name, HINSTANCE hi))
{
    return unregister_class_common(name, hi, 1);
}

static void fill_class(WORD *out, const aw_wndclass *c, int ex, int unicode)
{
    if (unicode) {
        WNDCLASSW *s = (WNDCLASSW *)out;
        if (ex) {
            WNDCLASSEXW *e = (WNDCLASSEXW *)out;
            memset(e, 0, sizeof *e);
            e->cbSize = sizeof *e;
            e->style = c->style;
            e->lpfnWndProc = c->proc;
            e->cbClsExtra = c->cb_cls;
            e->cbWndExtra = c->cb_wnd;
            e->hInstance = c->hinst;
            e->hIcon = c->icon;
            e->hCursor = c->cursor;
            e->hbrBackground = c->brush;
            e->lpszMenuName = (LPCWSTR)c->menu_name;
            e->lpszClassName = c->name;
            e->hIconSm = c->icon_sm;
            return;
        }
        memset(s, 0, sizeof *s);
        s->style = c->style;
        s->lpfnWndProc = c->proc;
        s->cbClsExtra = c->cb_cls;
        s->cbWndExtra = c->cb_wnd;
        s->hInstance = c->hinst;
        s->hIcon = c->icon;
        s->hCursor = c->cursor;
        s->hbrBackground = c->brush;
        s->lpszMenuName = (LPCWSTR)c->menu_name;
        s->lpszClassName = c->name;
    } else {
        WNDCLASSA *s = (WNDCLASSA *)out;
        if (ex) {
            WNDCLASSEXA *e = (WNDCLASSEXA *)out;
            memset(e, 0, sizeof *e);
            e->cbSize = sizeof *e;
            e->style = c->style;
            e->lpfnWndProc = c->proc;
            e->cbClsExtra = c->cb_cls;
            e->cbWndExtra = c->cb_wnd;
            e->hInstance = c->hinst;
            e->hIcon = c->icon;
            e->hCursor = c->cursor;
            e->hbrBackground = c->brush;
            e->lpszMenuName = (LPCSTR)c->menu_name;
            e->lpszClassName = (LPCSTR)c->name;
            e->hIconSm = c->icon_sm;
            return;
        }
        memset(s, 0, sizeof *s);
        s->style = c->style;
        s->lpfnWndProc = c->proc;
        s->cbClsExtra = c->cb_cls;
        s->cbWndExtra = c->cb_wnd;
        s->hInstance = c->hinst;
        s->hIcon = c->icon;
        s->hCursor = c->cursor;
        s->hbrBackground = c->brush;
        s->lpszMenuName = (LPCSTR)c->menu_name;
        s->lpszClassName = (LPCSTR)c->name;
    }
}

static BOOL get_class_info(const void *name, HINSTANCE hi, void *out, int ex, int unicode)
{
    WCHAR *nm = NULL;
    ATOM atom = 0;
    if (IS_INTRESOURCE(name))
        atom = (ATOM)(uintptr_t)name;
    else
        nm = aw_u2w((const char *)name, -1);
    pthread_mutex_lock(&aw_wnd_lock);
    aw_wndclass *c = aw_class_find(nm, atom);
    BOOL ok = FALSE;
    if (c && (!hi || !c->hinst || c->hinst == hi)) {
        fill_class(out, c, ex, unicode);
        ok = TRUE;
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    free(nm);
    return ok;
}

API(user32, BOOL, GetClassInfoA, (HINSTANCE hi, LPCSTR name, WNDCLASSA *out))
{
    if (!out)
        return FALSE;
    return get_class_info(name, hi, out, 0, 0);
}
API(user32, BOOL, GetClassInfoW, (HINSTANCE hi, LPCWSTR name, WNDCLASSW *out))
{
    if (!out)
        return FALSE;
    return get_class_info(name, hi, out, 0, 1);
}
API(user32, BOOL, GetClassInfoExA, (HINSTANCE hi, LPCSTR name, WNDCLASSEXA *out))
{
    if (!out)
        return FALSE;
    return get_class_info(name, hi, out, 1, 0);
}
API(user32, BOOL, GetClassInfoExW, (HINSTANCE hi, LPCWSTR name, WNDCLASSEXW *out))
{
    if (!out)
        return FALSE;
    return get_class_info(name, hi, out, 1, 1);
}

static int get_class_name(HWND h, void *buf, int max, int unicode)
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w || !w->cls || !buf || max <= 0)
        return 0;
    int n = wstr_len(w->cls->name, max - 1);
    if (unicode) {
        WCHAR *d = buf;
        for (int i = 0; i < n; i++)
            d[i] = w->cls->name[i];
        d[n] = 0;
        return n;
    }
    char *d = buf;
    int k = aw_w2u_buf(w->cls->name, n, d, max - 1);
    if (k > max - 1)
        k = max - 1;
    d[k] = 0;
    return k;
}

API(user32, int, GetClassNameA, (HWND h, LPSTR buf, int max))
{
    return get_class_name(h, buf, max, 0);
}
API(user32, int, GetClassNameW, (HWND h, LPWSTR buf, int max))
{
    return get_class_name(h, buf, max, 1);
}
API(user32, int, RealGetWindowClassA, (HWND h, LPSTR buf, UINT max))
{
    return get_class_name(h, buf, (int)max, 0);
}
API(user32, int, RealGetWindowClassW, (HWND h, LPWSTR buf, UINT max))
{
    return get_class_name(h, buf, (int)max, 1);
}

static LONG_PTR get_class_long(HWND h, int off, int unicode)
{
    (void)unicode;
    aw_win *w = aw_win_from_hwnd(h);
    if (!w || !w->cls)
        return 0;
    aw_wndclass *c = w->cls;
    switch (off) {
    case -32: return c->atom;
    case -26: return c->style;
    case -24: return (LONG_PTR)c->proc;
    case -22: return c->cb_wnd;
    case -20: return c->cb_cls;
    case -16: return (LONG_PTR)c->hinst;
    case -14: return (LONG_PTR)c->icon;
    case -12: return (LONG_PTR)c->cursor;
    case -10: return (LONG_PTR)c->brush;
    case -8: return (LONG_PTR)c->menu_name;
    case -34: return (LONG_PTR)c->icon_sm;
    default:
        if (off >= 0 && off + 8 <= c->cb_cls && c->cls_extra)
            return *(LONG_PTR *)(c->cls_extra + off);
        return 0;
    }
}

static LONG_PTR set_class_long(HWND h, int off, LONG_PTR val, int unicode)
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w || !w->cls)
        return 0;
    aw_wndclass *c = w->cls;
    switch (off) {
    case -26: {
        LONG_PTR old = c->style;
        c->style = (UINT)val;
        return old;
    }
    case -24: {
        LONG_PTR old = (LONG_PTR)c->proc;
        c->proc = (WNDPROC)val;
        c->unicode = unicode;
        for (int i = 0; i < AW_MAX_WINDOWS; i++)
            if (g_windows[i].used && g_windows[i].cls == c && g_windows[i].proc == (WNDPROC)old) {
                g_windows[i].proc = (WNDPROC)val;
                g_windows[i].unicode = unicode;
            }
        return old;
    }
    case -16: {
        LONG_PTR old = (LONG_PTR)c->hinst;
        c->hinst = (HINSTANCE)val;
        return old;
    }
    case -14: {
        LONG_PTR old = (LONG_PTR)c->icon;
        c->icon = (HICON)val;
        return old;
    }
    case -12: {
        LONG_PTR old = (LONG_PTR)c->cursor;
        c->cursor = (HCURSOR)val;
        return old;
    }
    case -10: {
        LONG_PTR old = (LONG_PTR)c->brush;
        c->brush = (HBRUSH)val;
        return old;
    }
    case -34: {
        LONG_PTR old = (LONG_PTR)c->icon_sm;
        c->icon_sm = (HICON)val;
        return old;
    }
    case -8: {
        LONG_PTR old = (LONG_PTR)c->menu_name;
        free(c->menu_name);
        c->menu_name = (WCHAR *)val;
        return old;
    }
    default:
        if (off >= 0 && off + 8 <= c->cb_cls && c->cls_extra) {
            LONG_PTR old = *(LONG_PTR *)(c->cls_extra + off);
            *(LONG_PTR *)(c->cls_extra + off) = val;
            return old;
        }
        return 0;
    }
}

API(user32, DWORD, GetClassLongA, (HWND h, int off)) { return (DWORD)get_class_long(h, off, 0); }
API(user32, DWORD, GetClassLongW, (HWND h, int off)) { return (DWORD)get_class_long(h, off, 1); }
API(user32, ULONG_PTR, GetClassLongPtrA, (HWND h, int off)) { return (ULONG_PTR)get_class_long(h, off, 0); }
API(user32, ULONG_PTR, GetClassLongPtrW, (HWND h, int off)) { return (ULONG_PTR)get_class_long(h, off, 1); }
API(user32, DWORD, SetClassLongA, (HWND h, int off, LONG v)) { return (DWORD)set_class_long(h, off, v, 0); }
API(user32, DWORD, SetClassLongW, (HWND h, int off, LONG v)) { return (DWORD)set_class_long(h, off, v, 1); }
API(user32, ULONG_PTR, SetClassLongPtrA, (HWND h, int off, LONG_PTR v)) { return (ULONG_PTR)set_class_long(h, off, v, 0); }
API(user32, ULONG_PTR, SetClassLongPtrW, (HWND h, int off, LONG_PTR v)) { return (ULONG_PTR)set_class_long(h, off, v, 1); }
API(user32, WORD, GetClassWord, (HWND h, int off)) { return (WORD)get_class_long(h, off, 1); }
API(user32, WORD, SetClassWord, (HWND h, int off, WORD v)) { return (WORD)set_class_long(h, off, v, 1); }

/* ------------------------------------------------------------------ init */
void aw_user_init(void)
{
    if (g_init_done)
        return;
    pthread_mutex_lock(&g_init_lock);
    if (g_init_done || g_init_running) {
        pthread_mutex_unlock(&g_init_lock);
        return;
    }
    g_init_running = 1;
    g_display = plat_init();
    aw_win *d = aw_win_alloc();
    if (d) {
        d->is_desktop = 1;
        d->visible = 1;
        d->style = WS_POPUP;
        d->proc = NULL;
        d->cls = NULL;
        d->parent = NULL;
        int sw = 1024, sh = 768;
        if (g_display)
            plat_screen_size(&sw, &sh);
        d->rect.left = d->rect.top = 0;
        d->rect.right = sw;
        d->rect.bottom = sh;
        d->client = d->rect;
    }
    aw_controls_register();
    aw_dialogs_register();
    aw_menus_register();
    aw_gui_atexit = aw_atexit_gui;
    g_init_done = 1;
    g_init_running = 0;
    pthread_mutex_unlock(&g_init_lock);
}

void aw_atexit_gui(void)
{
    for (int i = 0; i < AW_MAX_WINDOWS; i++) {
        aw_win *w = &g_windows[i];
        if (!w->used || w->is_desktop)
            continue;
        if (w->xwin) {
            plat_window_destroy(w->xwin);
            w->xwin = 0;
        }
        if (w->surf) {
            surface_free(w->surf);
            w->surf = NULL;
        }
    }
    plat_flush();
}

/* ------------------------------------------------------------------ window table lists */
aw_win *aw_top_ancestor(aw_win *w)
{
    while (w && w->parent)
        w = aw_win_from_hwnd(w->parent);
    return w;
}

void aw_insert_top(aw_win *w)
{
    w->parent = NULL;
    w->prev_sib = g_last_top;
    w->next_sib = NULL;
    if (g_last_top) {
        aw_win *l = aw_win_from_hwnd(g_last_top);
        l->next_sib = w->hwnd;
    } else
        g_first_top = w->hwnd;
    g_last_top = w->hwnd;
}

void aw_remove_top(aw_win *w)
{
    if (w->prev_sib) {
        aw_win *p = aw_win_from_hwnd(w->prev_sib);
        if (p)
            p->next_sib = w->next_sib;
    } else if (g_first_top == w->hwnd)
        g_first_top = w->next_sib;
    if (w->next_sib) {
        aw_win *n = aw_win_from_hwnd(w->next_sib);
        if (n)
            n->prev_sib = w->prev_sib;
    } else if (g_last_top == w->hwnd)
        g_last_top = w->prev_sib;
    w->next_sib = w->prev_sib = NULL;
}

void aw_insert_child(aw_win *parent, aw_win *w, HWND after)
{
    w->parent = parent->hwnd;
    aw_win *a = after && after != HWND_TOP ? aw_win_from_hwnd(after) : NULL;
    if (after == HWND_TOP || !a) {
        w->prev_sib = NULL;
        w->next_sib = parent->first_child;
        if (parent->first_child) {
            aw_win *f = aw_win_from_hwnd(parent->first_child);
            if (f)
                f->prev_sib = w->hwnd;
        } else
            parent->last_child = w->hwnd;
        parent->first_child = w->hwnd;
    } else if (after == HWND_BOTTOM) {
        w->next_sib = NULL;
        w->prev_sib = parent->last_child;
        if (parent->last_child) {
            aw_win *l = aw_win_from_hwnd(parent->last_child);
            if (l)
                l->next_sib = w->hwnd;
        } else
            parent->first_child = w->hwnd;
        parent->last_child = w->hwnd;
    } else {
        w->prev_sib = a->hwnd;
        w->next_sib = a->next_sib;
        if (a->next_sib) {
            aw_win *n = aw_win_from_hwnd(a->next_sib);
            if (n)
                n->prev_sib = w->hwnd;
        } else
            parent->last_child = w->hwnd;
        a->next_sib = w->hwnd;
    }
}

void aw_remove_child(aw_win *w)
{
    aw_win *p = aw_win_from_hwnd(w->parent);
    if (!p) {
        w->parent = NULL;
        w->next_sib = w->prev_sib = NULL;
        return;
    }
    if (w->prev_sib) {
        aw_win *q = aw_win_from_hwnd(w->prev_sib);
        if (q)
            q->next_sib = w->next_sib;
    } else if (p->first_child == w->hwnd)
        p->first_child = w->next_sib;
    if (w->next_sib) {
        aw_win *q = aw_win_from_hwnd(w->next_sib);
        if (q)
            q->prev_sib = w->prev_sib;
    } else if (p->last_child == w->hwnd)
        p->last_child = w->prev_sib;
    w->parent = NULL;
    w->next_sib = w->prev_sib = NULL;
}

void aw_zorder_child(aw_win *w, HWND after)
{
    if (!w->parent || w->parent == g_desktop)
        return;
    aw_win *p = aw_win_from_hwnd(w->parent);
    if (!p)
        return;
    HWND nxt = w->next_sib, prv = w->prev_sib;
    (void)nxt;
    (void)prv;
    aw_remove_child(w);
    aw_insert_child(p, w, after);
}

int aw_is_descendant(aw_win *anc, aw_win *w)
{
    for (aw_win *p = w; p; p = p->parent ? aw_win_from_hwnd(p->parent) : NULL)
        if (p == anc)
            return 1;
    return 0;
}

int aw_window_visible_chain(aw_win *w)
{
    for (; w; w = w->parent ? aw_win_from_hwnd(w->parent) : NULL)
        if (!w->visible)
            return 0;
    return 1;
}

int aw_window_enabled_chain(aw_win *w)
{
    for (; w; w = w->parent ? aw_win_from_hwnd(w->parent) : NULL)
        if (!w->enabled)
            return 0;
    return 1;
}

