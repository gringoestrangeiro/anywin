/* user32 core - atoms, window classes, class data, registered window messages (S5).
 * Class and atom names are stored as UTF-16 internally; A entry points convert at the boundary. */
#include "user32_int.h"

#define GCL_MENUNAME (-8)
#define GCL_HBRBACKGROUND (-10)
#define GCL_HCURSOR (-12)
#define GCL_HICON (-14)
#define GCL_HMODULE (-16)
#define GCL_CBWNDEXTRA (-18)
#define GCL_CBCLSEXTRA (-20)
#define GCL_WNDPROC (-24)
#define GCL_STYLE (-26)
#define GCL_HICONSM (-34)
#define ERR_CLASS_DOES_NOT_EXIST 1412 /* ERROR_CLASS_DOES_NOT_EXIST */
#define ERR_CLASS_ALREADY_EXISTS 1410
#define ERR_CLASS_HAS_WINDOWS 1412

static aw_atom_entry g_atoms[AW_MAX_ATOMS];
static aw_wndclass g_classes[AW_MAX_CLASSES];

static aw_atom_entry *atom_slot(ATOM a)
{
    if (a < AW_ATOM_BASE || a >= AW_ATOM_BASE + AW_MAX_ATOMS)
        return NULL;
    return &g_atoms[a - AW_ATOM_BASE];
}

int aw_wcmp_ci(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (WCHAR)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (WCHAR)(y + 32);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
        a++;
        b++;
    }
}

ATOM aw_atom_add(const WCHAR *name, int global)
{
    if (!name || !name[0] || aw_wcslen(name) > 255)
        return 0;
    pthread_mutex_lock(&aw_wnd_lock);
    ATOM r = 0;
    int free_i = -1;
    for (int i = 0; i < AW_MAX_ATOMS; i++) {
        if (g_atoms[i].used) {
            if (!aw_wcmp_ci(g_atoms[i].name, name)) {
                g_atoms[i].global |= global;
                r = (ATOM)(AW_ATOM_BASE + i);
                break;
            }
        } else if (free_i < 0) {
            free_i = i;
        }
    }
    if (!r && free_i >= 0) {
        size_t n = aw_wcslen(name);
        WCHAR *cp = malloc((n + 1) * sizeof(WCHAR));
        if (cp) {
            memcpy(cp, name, (n + 1) * sizeof(WCHAR));
            g_atoms[free_i].name = cp;
            g_atoms[free_i].used = 1;
            g_atoms[free_i].global = global;
            r = (ATOM)(AW_ATOM_BASE + free_i);
        }
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    return r;
}

ATOM aw_atom_find(const WCHAR *name)
{
    if (!name)
        return 0;
    pthread_mutex_lock(&aw_wnd_lock);
    ATOM r = 0;
    for (int i = 0; i < AW_MAX_ATOMS; i++)
        if (g_atoms[i].used && !aw_wcmp_ci(g_atoms[i].name, name)) {
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

static int atom_delete(ATOM a)
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

aw_wndclass *aw_class_find(const WCHAR *name, ATOM atom)
{
    for (int i = 0; i < AW_MAX_CLASSES; i++) {
        aw_wndclass *c = &g_classes[i];
        if (!c->used)
            continue;
        if (atom && c->atom == atom)
            return c;
        if (name && !aw_wcmp_ci(c->name, name))
            return c;
    }
    return NULL;
}

static void class_free(aw_wndclass *c)
{
    free(c->menu_name);
    free(c->cls_extra);
    memset(c, 0, sizeof *c);
}

ATOM aw_class_add(const aw_wndclass *tmpl, const WCHAR *name)
{
    if (!name || !name[0] || aw_wcslen(name) >= 128 || !tmpl->proc)
        return 0;
    pthread_mutex_lock(&aw_wnd_lock);
    if (aw_class_find(name, 0)) {
        pthread_mutex_unlock(&aw_wnd_lock);
        aw_set_last_error(ERR_CLASS_ALREADY_EXISTS);
        return 0;
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    ATOM atom = aw_atom_add(name, 0);
    if (!atom)
        return 0;
    pthread_mutex_lock(&aw_wnd_lock);
    aw_wndclass *c = NULL;
    for (int i = 0; i < AW_MAX_CLASSES; i++)
        if (!g_classes[i].used) {
            c = &g_classes[i];
            break;
        }
    if (c) {
        memset(c, 0, sizeof *c);
        c->used = 1;
        c->atom = atom;
        c->style = tmpl->style;
        c->proc = tmpl->proc;
        c->unicode = tmpl->unicode;
        c->cb_cls = tmpl->cb_cls > 0 ? tmpl->cb_cls : 0;
        c->cb_wnd = tmpl->cb_wnd > 0 ? tmpl->cb_wnd : 0;
        c->hinst = tmpl->hinst;
        c->icon = tmpl->icon;
        c->icon_sm = tmpl->icon_sm;
        c->cursor = tmpl->cursor;
        c->brush = tmpl->brush;
        c->menu_name = tmpl->menu_name ? aw_u2w(aw_w2u(tmpl->menu_name, -1), -1) : NULL;
        c->menu_id = tmpl->menu_id;
        c->builtin = tmpl->builtin;
        if (c->cb_cls)
            c->cls_extra = calloc(1, (size_t)c->cb_cls);
        size_t n = aw_wcslen(name);
        memcpy(c->name, name, (n + 1) * sizeof(WCHAR));
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    return c ? atom : 0;
}

/* ------------------------------------------------------------------ registration */
/* A name or atom parameter -> UTF-16 name (malloc'd) or NULL with *atom set for atoms. */
static WCHAR *name_param(const void *p, int unicode, ATOM *atom)
{
    *atom = 0;
    if (!p)
        return NULL;
    if (IS_INTRESOURCE(p)) {
        *atom = (ATOM)(uintptr_t)p;
        return NULL;
    }
    if (unicode)
        return aw_u2w(aw_w2u((const WCHAR *)p, -1), -1);
    return aw_u2w((const char *)p, -1);
}

static ATOM register_common(const WNDCLASSEXW *x, const WNDCLASSEXA *a, const WNDCLASSA *ca,
                            const WNDCLASSW *cw, int unicode)
{
    aw_user_init();
    aw_wndclass t;
    memset(&t, 0, sizeof t);
    const void *nm, *menu;
    if (unicode) {
        if (x) {
            t.style = x->style; t.proc = x->lpfnWndProc; t.cb_cls = x->cbClsExtra; t.cb_wnd = x->cbWndExtra;
            t.hinst = x->hInstance; t.icon = x->hIcon; t.icon_sm = x->hIconSm; t.cursor = x->hCursor;
            t.brush = x->hbrBackground; menu = x->lpszMenuName; nm = x->lpszClassName;
        } else {
            t.style = cw->style; t.proc = cw->lpfnWndProc; t.cb_cls = cw->cbClsExtra; t.cb_wnd = cw->cbWndExtra;
            t.hinst = cw->hInstance; t.icon = cw->hIcon; t.icon_sm = cw->hIcon; t.cursor = cw->hCursor;
            t.brush = cw->hbrBackground; menu = cw->lpszMenuName; nm = cw->lpszClassName;
        }
    } else {
        if (a) {
            t.style = a->style; t.proc = a->lpfnWndProc; t.cb_cls = a->cbClsExtra; t.cb_wnd = a->cbWndExtra;
            t.hinst = a->hInstance; t.icon = a->hIcon; t.icon_sm = a->hIconSm; t.cursor = a->hCursor;
            t.brush = a->hbrBackground; menu = a->lpszMenuName; nm = a->lpszClassName;
        } else {
            t.style = ca->style; t.proc = ca->lpfnWndProc; t.cb_cls = ca->cbClsExtra; t.cb_wnd = ca->cbWndExtra;
            t.hinst = ca->hInstance; t.icon = ca->hIcon; t.icon_sm = ca->hIcon; t.cursor = ca->hCursor;
            t.brush = ca->hbrBackground; menu = ca->lpszMenuName; nm = ca->lpszClassName;
        }
    }
    t.unicode = unicode;
    if (!t.proc) {
        aw_set_last_error(87);
        return 0;
    }
    ATOM atom0;
    WCHAR *name = name_param(nm, unicode, &atom0);
    if (!name) {
        aw_set_last_error(87);
        return 0;
    }
    if (menu) {
        if (IS_INTRESOURCE(menu)) {
            t.menu_id = (uintptr_t)menu;
        } else {
            t.menu_name = name_param(menu, unicode, &atom0);
        }
    }
    ATOM r = aw_class_add(&t, name);
    if (!r && aw_get_last_error() == 0)
        aw_set_last_error(ERR_CLASS_ALREADY_EXISTS);
    free(t.menu_name);
    free(name);
    return r;
}

API(user32, ATOM, RegisterClassA, (const WNDCLASSA *wc))
{
    if (!wc) return 0;
    return register_common(NULL, NULL, wc, NULL, 0);
}
API(user32, ATOM, RegisterClassW, (const WNDCLASSW *wc))
{
    if (!wc) return 0;
    return register_common(NULL, NULL, NULL, wc, 1);
}
API(user32, ATOM, RegisterClassExA, (const WNDCLASSEXA *wc))
{
    if (!wc) return 0;
    return register_common(NULL, wc, NULL, NULL, 0);
}
API(user32, ATOM, RegisterClassExW, (const WNDCLASSEXW *wc))
{
    if (!wc) return 0;
    return register_common(wc, NULL, NULL, NULL, 1);
}

static BOOL unregister_common(const void *name, HINSTANCE hi, int unicode)
{
    (void)hi;
    aw_user_init();
    ATOM atom;
    WCHAR *nm = name_param(name, unicode, &atom);
    if (!nm && !atom) return FALSE;
    pthread_mutex_lock(&aw_wnd_lock);
    aw_wndclass *c = aw_class_find(nm, atom);
    BOOL ok = FALSE;
    if (c) {
        int in_use = 0;
        for (int i = 0; i < AW_MAX_WINDOWS && !in_use; i++) {
            aw_win *w = aw_win_slot(i);
            if (w->used && w->cls == c)
                in_use = 1;
        }
        if (in_use) {
            aw_set_last_error(ERR_CLASS_HAS_WINDOWS);
        } else {
            class_free(c);
            ok = TRUE;
        }
    } else {
        aw_set_last_error(ERR_CLASS_DOES_NOT_EXIST);
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    free(nm);
    return ok;
}

API(user32, BOOL, UnregisterClassA, (LPCSTR name, HINSTANCE hi)) { return unregister_common(name, hi, 0); }
API(user32, BOOL, UnregisterClassW, (LPCWSTR name, HINSTANCE hi)) { return unregister_common(name, hi, 1); }

/* ------------------------------------------------------------------ class info */
static BOOL class_info(const void *name, void *out, int ex, int unicode)
{
    aw_user_init();
    ATOM atom;
    WCHAR *nm = name_param(name, unicode, &atom);
    pthread_mutex_lock(&aw_wnd_lock);
    aw_wndclass *c = aw_class_find(nm, atom);
    if (!c) {
        pthread_mutex_unlock(&aw_wnd_lock);
        free(nm);
        aw_set_last_error(ERR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }
    if (ex) {
        WNDCLASSEXW *o = out; /* same prefix layout for A and W apart from string pointers */
        memset(o, 0, sizeof *o);
        o->cbSize = sizeof(WNDCLASSEXW);
        o->style = c->style; o->lpfnWndProc = c->proc; o->cbClsExtra = c->cb_cls; o->cbWndExtra = c->cb_wnd;
        o->hInstance = c->hinst; o->hIcon = c->icon; o->hCursor = c->cursor; o->hbrBackground = c->brush;
        o->hIconSm = c->icon_sm;
    } else {
        WNDCLASSW *o = out;
        memset(o, 0, sizeof *o);
        o->style = c->style; o->lpfnWndProc = c->proc; o->cbClsExtra = c->cb_cls; o->cbWndExtra = c->cb_wnd;
        o->hInstance = c->hinst; o->hIcon = c->icon; o->hCursor = c->cursor; o->hbrBackground = c->brush;
    }
    pthread_mutex_unlock(&aw_wnd_lock);
    free(nm);
    return TRUE;
}

/* The WNDCLASSA and WNDCLASSW layouts differ only by string types, so the non-string fields are
 * filled through the same code; string members are left NULL. */
API(user32, BOOL, GetClassInfoA, (HINSTANCE hi, LPCSTR name, WNDCLASSA *out))
{
    (void)hi;
    return class_info(name, out, 0, 0);
}
API(user32, BOOL, GetClassInfoW, (HINSTANCE hi, LPCWSTR name, WNDCLASSW *out))
{
    (void)hi;
    return class_info(name, out, 0, 1);
}
API(user32, BOOL, GetClassInfoExA, (HINSTANCE hi, LPCSTR name, WNDCLASSEXA *out))
{
    (void)hi;
    return class_info(name, out, 1, 0);
}
API(user32, BOOL, GetClassInfoExW, (HINSTANCE hi, LPCWSTR name, WNDCLASSEXW *out))
{
    (void)hi;
    return class_info(name, out, 1, 1);
}

static int class_name_out(const aw_wndclass *c, void *buf, int max, int unicode)
{
    if (!buf || max <= 0) return 0;
    if (unicode) {
        int n = aw_wcslen(c->name);
        if (n > max - 1) n = max - 1;
        memcpy(buf, c->name, (size_t)n * sizeof(WCHAR));
        ((WCHAR *)buf)[n] = 0;
        return n;
    }
    int n = aw_w2u_buf(c->name, -1, buf, max);
    return n < 0 ? 0 : n;
}

static int get_class_name(HWND h, void *buf, int max, int unicode)
{
    aw_win *w = aw_win_from_hwnd(h);
    if (!w || !w->cls) return 0;
    return class_name_out(w->cls, buf, max, unicode);
}

API(user32, int, GetClassNameA, (HWND h, LPSTR buf, int max)) { return get_class_name(h, buf, max, 0); }
API(user32, int, GetClassNameW, (HWND h, LPWSTR buf, int max)) { return get_class_name(h, buf, max, 1); }
API(user32, int, RealGetWindowClassA, (HWND h, LPSTR buf, UINT max)) { return get_class_name(h, buf, (int)max, 0); }
API(user32, int, RealGetWindowClassW, (HWND h, LPWSTR buf, UINT max)) { return get_class_name(h, buf, (int)max, 1); }

/* ------------------------------------------------------------------ class data */
static aw_wndclass *class_of_hwnd(HWND h)
{
    aw_win *w = aw_win_from_hwnd(h);
    return w ? w->cls : NULL;
}

static LONG_PTR get_class_long(HWND h, int off)
{
    aw_wndclass *c = class_of_hwnd(h);
    if (!c) return 0;
    switch (off) {
    case GCL_MENUNAME: return (LONG_PTR)c->menu_name;
    case GCL_HBRBACKGROUND: return (LONG_PTR)c->brush;
    case GCL_HCURSOR: return (LONG_PTR)c->cursor;
    case GCL_HICON: return (LONG_PTR)c->icon;
    case GCL_HICONSM: return (LONG_PTR)c->icon_sm;
    case GCL_HMODULE: return (LONG_PTR)c->hinst;
    case GCL_CBWNDEXTRA: return c->cb_wnd;
    case GCL_CBCLSEXTRA: return c->cb_cls;
    case GCL_WNDPROC: return (LONG_PTR)c->proc;
    case GCL_STYLE: return c->style;
    default: break;
    }
    if (off >= 0 && c->cls_extra && off + 8 <= c->cb_cls) {
        LONG_PTR v = 0;
        memcpy(&v, c->cls_extra + off, 8);
        return v;
    }
    return 0;
}

static LONG_PTR set_class_long(HWND h, int off, LONG_PTR val, int size)
{
    aw_wndclass *c = class_of_hwnd(h);
    if (!c) return 0;
    LONG_PTR old = get_class_long(h, off);
    switch (off) {
    case GCL_HBRBACKGROUND: c->brush = (HBRUSH)val; return old;
    case GCL_HCURSOR: c->cursor = (HCURSOR)val; return old;
    case GCL_HICON: c->icon = (HICON)val; return old;
    case GCL_HICONSM: c->icon_sm = (HICON)val; return old;
    case GCL_WNDPROC: c->proc = (WNDPROC)val; return old;
    case GCL_STYLE: c->style = (UINT)val; return old;
    default: break;
    }
    if (off >= 0 && c->cls_extra && off + size <= c->cb_cls) {
        memcpy(c->cls_extra + off, &val, (size_t)size);
        return old;
    }
    return 0;
}

API(user32, DWORD, GetClassLongA, (HWND h, int off)) { return (DWORD)get_class_long(h, off); }
API(user32, DWORD, GetClassLongW, (HWND h, int off)) { return (DWORD)get_class_long(h, off); }
API(user32, ULONG_PTR, GetClassLongPtrA, (HWND h, int off)) { return (ULONG_PTR)get_class_long(h, off); }
API(user32, ULONG_PTR, GetClassLongPtrW, (HWND h, int off)) { return (ULONG_PTR)get_class_long(h, off); }
API(user32, DWORD, SetClassLongA, (HWND h, int off, LONG v)) { return (DWORD)set_class_long(h, off, (LONG_PTR)v, 4); }
API(user32, DWORD, SetClassLongW, (HWND h, int off, LONG v)) { return (DWORD)set_class_long(h, off, (LONG_PTR)v, 4); }
API(user32, ULONG_PTR, SetClassLongPtrA, (HWND h, int off, LONG_PTR v))
{
    return (ULONG_PTR)set_class_long(h, off, v, 8);
}
API(user32, ULONG_PTR, SetClassLongPtrW, (HWND h, int off, LONG_PTR v))
{
    return (ULONG_PTR)set_class_long(h, off, v, 8);
}
API(user32, WORD, GetClassWord, (HWND h, int off)) { return (WORD)get_class_long(h, off); }
API(user32, WORD, SetClassWord, (HWND h, int off, WORD v)) { return (WORD)set_class_long(h, off, v, 2); }

/* ------------------------------------------------------------------ registered messages */
static UINT register_msg(const WCHAR *name)
{
    ATOM a = aw_atom_add(name, 0);
    return a;
}
API(user32, UINT, RegisterWindowMessageA, (LPCSTR s))
{
    WCHAR *w = aw_u2w(s, -1);
    if (!w) return 0;
    UINT r = register_msg(w);
    free(w);
    return r;
}
API(user32, UINT, RegisterWindowMessageW, (LPCWSTR s)) { return register_msg(s); }

/* atom deletion for the ATOM table used by user32 (kernel32 names are exported by S1) */
int aw_atom_delete(ATOM a) { return atom_delete(a); }
