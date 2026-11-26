/* anywin clipboard (S-CLIP): the Win32 clipboard API over the X11 CLIPBOARD selection.
 *
 * Data set by this process is kept as HGLOBAL objects (owned) and also published to X as UTF-8
 * (plat_clipboard_set).  Text requested while another client owns the selection is fetched
 * with plat_clipboard_get, converted to the requested format and cached until the next
 * OpenClipboard.  Text is UTF-8 in ANSI form (this runtime's code page is 65001).
 *
 * Global memory comes from the kernel32 GlobalAlloc family when it is linked in; the weak
 * references fall back to malloc-backed handles otherwise. */
#include "user32_int.h"
#include "aw_api.h"
#include <strings.h>

extern HGLOBAL WINAPI __aw_kernel32_GlobalAlloc(UINT flags, SIZE_T n) __attribute__((weak));
extern HGLOBAL WINAPI __aw_kernel32_GlobalFree(HGLOBAL h) __attribute__((weak));
extern LPVOID WINAPI __aw_kernel32_GlobalLock(HGLOBAL h) __attribute__((weak));
extern BOOL WINAPI __aw_kernel32_GlobalUnlock(HGLOBAL h) __attribute__((weak));

#define AW_CF_TEXT 1
#define AW_CF_UNICODETEXT 13
#define AW_CF_OEMTEXT 7
#define AW_CF_FIRST_REGISTERED 0xC000
#define AW_CF_LAST_REGISTERED 0xFFFF
#define AW_ERROR_CLIPBOARD_NOT_OPEN 1418
#define AW_ERROR_INVALID_PARAMETER 87

typedef struct {
    UINT fmt;
    HGLOBAL h;
    int owned; /* set by SetClipboardData (freed on EmptyClipboard); 0 = cached from X */
} clip_item;

static struct {
    int is_open;
    HWND open_for;
    HWND owner;
    clip_item *items;
    int n, cap;
} clip;

static struct {
    char *name;
    UINT id;
} *reg_fmts;
static int reg_n, reg_cap;

static HGLOBAL g_alloc(size_t n)
{
    if (__aw_kernel32_GlobalAlloc)
        return __aw_kernel32_GlobalAlloc(0x0002 /* GMEM_MOVEABLE */ | 0x0040 /* GMEM_ZEROINIT: no stale tail */, n);
    return calloc(1, n ? n : 1);
}

static void *g_lock(HGLOBAL h)
{
    return __aw_kernel32_GlobalLock ? __aw_kernel32_GlobalLock(h) : h;
}

static void g_unlock(HGLOBAL h)
{
    if (__aw_kernel32_GlobalUnlock)
        __aw_kernel32_GlobalUnlock(h);
}

static void g_free(HGLOBAL h)
{
    if (!h)
        return;
    if (__aw_kernel32_GlobalFree)
        __aw_kernel32_GlobalFree(h);
    else
        free(h);
}

static int is_text_fmt(UINT f) { return f == AW_CF_TEXT || f == AW_CF_UNICODETEXT || f == AW_CF_OEMTEXT; }

static clip_item *item_find(UINT fmt)
{
    for (int i = 0; i < clip.n; i++)
        if (clip.items[i].fmt == fmt)
            return &clip.items[i];
    return NULL;
}

/* frees the items: all of them, or only the cached ones fetched from X (cached_only) */
static void items_clear(int cached_only)
{
    for (int i = clip.n - 1; i >= 0; i--)
        if (!cached_only || !clip.items[i].owned) {
            if (clip.items[i].h)
                g_free(clip.items[i].h);
            clip.items[i] = clip.items[--clip.n];
        }
}

static void item_put(UINT fmt, HGLOBAL h, int owned)
{
    clip_item *it = item_find(fmt);
    if (it) {
        if (it->h && it->h != h)
            g_free(it->h);
        it->h = h;
        it->owned = owned;
        return;
    }
    if (clip.n == clip.cap) {
        int nc = clip.cap ? clip.cap * 2 : 4;
        clip_item *p = realloc(clip.items, sizeof *p * (size_t)nc);
        if (!p)
            aw_fatal("clipboard: out of memory");
        clip.items = p;
        clip.cap = nc;
    }
    clip.items[clip.n++] = (clip_item){fmt, h, owned};
}

/* UTF-8 text of the clipboard: an owned text item first, then the X selection (malloc'd or NULL) */
static char *text_utf8(void)
{
    clip_item *u = item_find(AW_CF_UNICODETEXT);
    if (u && u->h) {
        const WCHAR *w = g_lock(u->h);
        char *s = w ? aw_w2u(w, -1) : NULL;
        g_unlock(u->h);
        if (s)
            return s;
    }
    clip_item *a = item_find(AW_CF_TEXT);
    if (!a) a = item_find(AW_CF_OEMTEXT);
    if (a && a->h) {
        const char *c = g_lock(a->h);
        char *s = c ? strdup(c) : NULL;
        g_unlock(a->h);
        if (s)
            return s;
    }
    return plat_clipboard_get();
}

/* ---------------------------------------------------------------------- */

API(user32, BOOL, OpenClipboard, (HWND h))
{
    if (clip.is_open) {
        if (clip.open_for == h)
            return TRUE;
        aw_set_last_error(5 /* ERROR_ACCESS_DENIED */);
        return FALSE;
    }
    items_clear(1); /* cached remote data is stale once the clipboard is reopened */
    clip.is_open = 1;
    clip.open_for = h;
    return TRUE;
}

API(user32, BOOL, CloseClipboard, (void))
{
    if (!clip.is_open) {
        aw_set_last_error(AW_ERROR_CLIPBOARD_NOT_OPEN);
        return FALSE;
    }
    clip.is_open = 0;
    clip.open_for = NULL;
    return TRUE;
}

API(user32, BOOL, EmptyClipboard, (void))
{
    if (!clip.is_open) {
        aw_set_last_error(AW_ERROR_CLIPBOARD_NOT_OPEN);
        return FALSE;
    }
    items_clear(0);
    clip.owner = clip.open_for;
    plat_clipboard_set(NULL); /* give up the X selection */
    return TRUE;
}

API(user32, HANDLE, SetClipboardData, (UINT fmt, HANDLE h))
{
    if (!clip.is_open) {
        aw_set_last_error(AW_ERROR_CLIPBOARD_NOT_OPEN);
        return NULL;
    }
    if (!fmt || !h) {
        /* delayed rendering (NULL) needs WM_RENDERFORMAT, which is not supported */
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    item_put(fmt, (HGLOBAL)h, 1);
    if (is_text_fmt(fmt)) {
        char *utf8 = text_utf8();
        plat_clipboard_set(utf8);
        free(utf8);
    }
    return h;
}

API(user32, HANDLE, GetClipboardData, (UINT fmt))
{
    if (!clip.is_open) {
        aw_set_last_error(AW_ERROR_CLIPBOARD_NOT_OPEN);
        return NULL;
    }
    clip_item *it = item_find(fmt);
    if (it && it->h)
        return it->h;
    if (!is_text_fmt(fmt)) {
        aw_set_last_error(0);
        return NULL;
    }
    char *utf8 = text_utf8();
    if (!utf8) {
        aw_set_last_error(0);
        return NULL;
    }
    HGLOBAL out = NULL;
    if (fmt == AW_CF_UNICODETEXT) {
        WCHAR *w = aw_u2w(utf8, -1);
        if (w) {
            size_t bytes = (aw_wcslen(w) + 1) * sizeof(WCHAR);
            out = g_alloc(bytes);
            void *p = out ? g_lock(out) : NULL;
            if (p)
                memcpy(p, w, bytes);
            g_unlock(out);
        }
        free(w);
    } else {
        size_t n = strlen(utf8);
        out = g_alloc(n + 1);
        void *p = out ? g_lock(out) : NULL;
        if (p)
            memcpy(p, utf8, n + 1);
        g_unlock(out);
    }
    free(utf8);
    if (out)
        item_put(fmt, out, 0);
    return out;
}

API(user32, BOOL, IsClipboardFormatAvailable, (UINT fmt))
{
    if (item_find(fmt))
        return TRUE;
    if (!is_text_fmt(fmt))
        return FALSE;
    char *utf8 = text_utf8();
    free(utf8);
    return utf8 != NULL;
}

API(user32, HWND, GetClipboardOwner, (void)) { return clip.owner; }

/* Formats on the clipboard in Win32 order: the ones set here first (in SetClipboardData order), then
 * the text formats the system synthesizes from remote text (CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT). */
static UINT *formats_list(int *count)
{
    char *utf8 = text_utf8();
    int extra = utf8 ? 3 : 0;
    free(utf8);
    UINT *out = malloc(sizeof *out * (size_t)(clip.n + extra + 1));
    if (!out)
        return NULL;
    int n = 0;
    for (int i = 0; i < clip.n; i++)
        out[n++] = clip.items[i].fmt;
    if (extra) {
        static const UINT synth[3] = {AW_CF_UNICODETEXT, AW_CF_TEXT, AW_CF_OEMTEXT};
        for (int k = 0; k < 3; k++) {
            int have = 0;
            for (int i = 0; i < n; i++)
                if (out[i] == synth[k])
                    have = 1;
            if (!have)
                out[n++] = synth[k];
        }
    }
    *count = n;
    return out;
}

API(user32, int, CountClipboardFormats, (void))
{
    if (!clip.is_open) {
        aw_set_last_error(AW_ERROR_CLIPBOARD_NOT_OPEN);
        return 0;
    }
    int n = 0;
    UINT *f = formats_list(&n);
    free(f);
    return n;
}

API(user32, UINT, EnumClipboardFormats, (UINT fmt))
{
    if (!clip.is_open) {
        aw_set_last_error(AW_ERROR_CLIPBOARD_NOT_OPEN);
        return 0;
    }
    int n = 0;
    UINT *f = formats_list(&n);
    UINT next = 0;
    if (f) {
        int pos = -1;
        if (fmt) {
            for (int i = 0; i < n; i++)
                if (f[i] == fmt) {
                    pos = i;
                    break;
                }
            if (pos < 0) {
                aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
                free(f);
                return 0;
            }
        }
        if (pos + 1 < n)
            next = f[pos + 1];
        free(f);
    }
    if (!next)
        aw_set_last_error(0);
    return next;
}

static UINT register_fmt(const char *name, const WCHAR *wname)
{
    char *n = NULL;
    if (name) {
        n = strdup(name);
    } else if (wname) {
        n = aw_w2u(wname, -1);
    }
    if (!n) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (int i = 0; i < reg_n; i++)
        if (!strcasecmp(reg_fmts[i].name, n)) {
            free(n);
            return reg_fmts[i].id;
        }
    if (AW_CF_FIRST_REGISTERED + reg_n > AW_CF_LAST_REGISTERED) {
        free(n);
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (reg_n == reg_cap) {
        int nc = reg_cap ? reg_cap * 2 : 8;
        void *p = realloc(reg_fmts, sizeof *reg_fmts * (size_t)nc);
        if (!p)
            aw_fatal("clipboard: out of memory");
        reg_fmts = p;
        reg_cap = nc;
    }
    reg_fmts[reg_n].name = n;
    reg_fmts[reg_n].id = AW_CF_FIRST_REGISTERED + (UINT)reg_n;
    return reg_fmts[reg_n++].id;
}

API(user32, UINT, RegisterClipboardFormatA, (LPCSTR name)) { return register_fmt(name, NULL); }
API(user32, UINT, RegisterClipboardFormatW, (LPCWSTR name)) { return register_fmt(NULL, name); }
