/* anywin resources (S-RES): FindResource / LoadResource / LockResource / SizeofResource.
 *
 * The image's .rsrc directory is walked by aw_find_resource (kernel32.c).  An HRSRC is a small
 * record holding the data address and size; records are interned per data address so that
 * repeated FindResource calls for the same resource return the same handle and the table stays
 * bounded by the number of distinct resources in the image. */
#include "user32_int.h"
#include <pthread.h>

#define AW_ERROR_RESOURCE_NAME_NOT_FOUND 1814
#define AW_ERROR_INVALID_PARAMETER 87

typedef struct aw_hrsrc {
    const void *data;
    uint32_t size;
    struct aw_hrsrc *next;
} aw_hrsrc;

static aw_hrsrc *rsrc_list;
static pthread_mutex_t rsrc_lock = PTHREAD_MUTEX_INITIALIZER;

static aw_hrsrc *intern_rsrc(const void *data, uint32_t size)
{
    pthread_mutex_lock(&rsrc_lock);
    aw_hrsrc *r = rsrc_list;
    while (r && r->data != data)
        r = r->next;
    if (!r) {
        r = calloc(1, sizeof *r);
        if (r) {
            r->data = data;
            r->size = size;
            r->next = rsrc_list;
            rsrc_list = r;
        }
    }
    pthread_mutex_unlock(&rsrc_lock);
    return r;
}

/* type and name are MAKEINTRESOURCE values or UTF-16 strings; the wide variant is passed through */
static HRSRC find_common(HMODULE mod, const void *name, const void *type)
{
    uint32_t size = 0;
    void *data = aw_find_resource(mod, type, name, &size);
    if (!data) {
        aw_set_last_error(AW_ERROR_RESOURCE_NAME_NOT_FOUND);
        return NULL;
    }
    return (HRSRC)intern_rsrc(data, size);
}

/* ANSI resource names (UTF-8 strings) are converted to UTF-16 for the directory walk */
static const void *ansi_res_arg(const char *s, WCHAR **owned)
{
    *owned = NULL;
    if (IS_INTRESOURCE(s))
        return s;
    WCHAR *w = aw_u2w(s, -1);
    *owned = w;
    return w;
}

API(kernel32, HRSRC, FindResourceA, (HMODULE mod, LPCSTR name, LPCSTR type))
{
    if (!name || !type) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    WCHAR *wn, *wt;
    const void *n = ansi_res_arg(name, &wn), *t = ansi_res_arg(type, &wt);
    HRSRC r = find_common(mod, n, t);
    free(wn);
    free(wt);
    return r;
}

API(kernel32, HRSRC, FindResourceW, (HMODULE mod, LPCWSTR name, LPCWSTR type))
{
    if (!name || !type) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return find_common(mod, name, type);
}

API(kernel32, HRSRC, FindResourceExA, (HMODULE mod, LPCSTR type, LPCSTR name, WORD lang))
{
    (void)lang; /* the first language entry is used, as aw_find_resource does */
    return __aw_kernel32_FindResourceA(mod, name, type);
}

API(kernel32, HRSRC, FindResourceExW, (HMODULE mod, LPCWSTR type, LPCWSTR name, WORD lang))
{
    (void)lang;
    return __aw_kernel32_FindResourceW(mod, name, type);
}

API(kernel32, HGLOBAL, LoadResource, (HMODULE mod, HRSRC r))
{
    (void)mod;
    aw_hrsrc *h = (aw_hrsrc *)r;
    if (!h) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return (HGLOBAL)(void *)h->data;
}

API(kernel32, LPVOID, LockResource, (HGLOBAL h)) { return h; }

API(kernel32, DWORD, SizeofResource, (HMODULE mod, HRSRC r))
{
    (void)mod;
    aw_hrsrc *h = (aw_hrsrc *)r;
    if (!h) {
        aw_set_last_error(AW_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return h->size;
}

API(kernel32, BOOL, FreeResource, (HGLOBAL h))
{
    (void)h; /* resources live in the image: nothing to release */
    return FALSE;
}
