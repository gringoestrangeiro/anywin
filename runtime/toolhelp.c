/* anywin runtime - Toolhelp32 module snapshot (kernel32).
 * The only module reported is the program itself: the relinked image is one ELF and the other
 * modules of a Windows process have no counterpart here. Process snapshots are not provided. */
#include "aw.h"
#include "k32.h"
#include <unistd.h>

#define TH32CS_SNAPMODULE 0x00000008u
#define ERR_INVALID_HANDLE 6
#define ERR_NO_MORE_FILES 18
#define ERR_INSUFFICIENT_BUFFER 122

/* MODULEENTRY32W, x64 layout (1080 bytes) */
typedef struct {
    DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
    BYTE *modBaseAddr;
    DWORD modBaseSize;
    HMODULE hModule;
    WCHAR szModule[256];
    WCHAR szExePath[260];
} MODULEENTRY32W_L;

API(kernel32, HANDLE, CreateToolhelp32Snapshot, (DWORD flags, DWORD pid))
{
    (void)pid;
    if (!(flags & TH32CS_SNAPMODULE)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    aw_object *o = aw_object_new(H_FILE);
    HANDLE h = aw_handle_alloc(H_FILE, o);
    if (!h) {
        free(o);
        return INVALID_HANDLE_VALUE;
    }
    aw_set_last_error(0);
    return h;
}

static void fill_main_module(MODULEENTRY32W_L *m)
{
    m->th32ModuleID = 1;
    m->th32ProcessID = (DWORD)getpid();
    m->modBaseAddr = (BYTE *)__aw_image_base;
    m->modBaseSize = 0; /* the size of the image is not known to the runtime */
    m->hModule = (HMODULE)__aw_image_base;
    WCHAR *exe = aw_u2w(aw_exe_win, -1);
    WCHAR *base = exe;
    for (WCHAR *p = exe; *p; p++)
        if (*p == '\\')
            base = p + 1;
    size_t n = aw_wcslen(base);
    if (n > 255)
        n = 255;
    memcpy(m->szModule, base, n * sizeof(WCHAR));
    m->szModule[n] = 0;
    size_t en = aw_wcslen(exe);
    if (en > 259)
        en = 259;
    memcpy(m->szExePath, exe, en * sizeof(WCHAR));
    m->szExePath[en] = 0;
    free(exe);
}

API(kernel32, BOOL, Module32FirstW, (HANDLE snap, MODULEENTRY32W_L *m))
{
    if (!aw_handle_object(snap)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return FALSE;
    }
    if (!m || m->dwSize < sizeof *m) {
        aw_set_last_error(ERR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    DWORD size = m->dwSize;
    memset(m, 0, sizeof *m);
    m->dwSize = size;
    fill_main_module(m);
    aw_set_last_error(0);
    return TRUE;
}

API(kernel32, BOOL, Module32NextW, (HANDLE snap, MODULEENTRY32W_L *m))
{
    (void)m;
    if (!aw_handle_object(snap)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return FALSE;
    }
    aw_set_last_error(ERR_NO_MORE_FILES);
    return FALSE;
}
