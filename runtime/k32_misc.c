/* kernel32: global and local memory, pointer encoding, debugger and CPU queries, time conversions,
 * directory and current-directory calls, FormatMessageA, Beep and the unwind entry points.
 *
 * GMEM_MOVEABLE and LMEM_MOVEABLE allocations get a small header that GlobalLock resolves to the
 * data block (lock counts are kept); every other allocation is a plain heap block whose handle is
 * its address, as on Windows. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ERR_INVALID_HANDLE 6
#define ERR_NOT_ENOUGH_MEMORY 8
#define ERR_INVALID_PARAMETER 87
#define GMEM_MOVEABLE 0x0002u
#define GMEM_ZEROINIT 0x0040u

void WINAPI __aw_kernel32_RtlUnwindEx(void *target_frame, void *target_ip, void *rec, void *retval, void *ctx,
                                      void *history);
DWORD WINAPI __aw_kernel32_FormatMessageW(DWORD flags, const void *src, DWORD id, DWORD lang, WCHAR *buf, DWORD size,
                                          void *args);
DWORD WINAPI __aw_kernel32_GetTempPathW(DWORD len, WCHAR *buf);
DWORD WINAPI __aw_kernel32_GetCurrentDirectoryW(DWORD len, WCHAR *buf);

static DWORD fail(DWORD e)
{
    aw_set_last_error(e);
    return 0;
}

static DWORD fail_errno(void)
{
    return fail(aw_errno_to_win(errno));
}

/* ---- global and local memory ------------------------------------------------ */
struct moveable {
    void *ptr;
    SIZE_T size;
    unsigned locks;
    struct moveable *next;
};

static struct moveable *moveables;
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;

static struct moveable *find_moveable(void *h)
{
    for (struct moveable *m = moveables; m; m = m->next)
        if (m == h)
            return m;
    return NULL;
}

static void *alloc_mem(DWORD flags, SIZE_T bytes)
{
    void *p = (flags & GMEM_ZEROINIT) ? calloc(1, bytes ? bytes : 1) : malloc(bytes ? bytes : 1);
    if (!p) {
        fail(ERR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!(flags & GMEM_MOVEABLE))
        return p;
    struct moveable *m = malloc(sizeof *m);
    if (!m) {
        free(p);
        fail(ERR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    m->ptr = p;
    m->size = bytes;
    m->locks = 0;
    pthread_mutex_lock(&mem_lock);
    m->next = moveables;
    moveables = m;
    pthread_mutex_unlock(&mem_lock);
    return m;
}

static void *free_mem(void *h)
{
    if (!h)
        return NULL;
    pthread_mutex_lock(&mem_lock);
    struct moveable *m = find_moveable(h);
    if (m) {
        for (struct moveable **pp = &moveables; *pp; pp = &(*pp)->next)
            if (*pp == m) {
                *pp = m->next;
                break;
            }
    }
    pthread_mutex_unlock(&mem_lock);
    if (m) {
        free(m->ptr);
        free(m);
    } else {
        free(h);
    }
    return NULL;
}

static void *lock_mem(void *h)
{
    if (!h)
        return fail(ERR_INVALID_HANDLE), NULL;
    pthread_mutex_lock(&mem_lock);
    struct moveable *m = find_moveable(h);
    void *p = m ? m->ptr : h;
    if (m)
        m->locks++;
    pthread_mutex_unlock(&mem_lock);
    return p;
}

static BOOL unlock_mem(void *h)
{
    if (!h)
        return fail(ERR_INVALID_HANDLE), FALSE;
    pthread_mutex_lock(&mem_lock);
    struct moveable *m = find_moveable(h);
    int still = 0;
    if (m && m->locks) {
        m->locks--;
        still = m->locks != 0;
    }
    pthread_mutex_unlock(&mem_lock);
    if (still)
        return TRUE;
    aw_set_last_error(0); /* unlocked: GetLastError reports NO_ERROR */
    return FALSE;
}

API(kernel32, HGLOBAL, GlobalAlloc, (DWORD flags, SIZE_T bytes)) { return alloc_mem(flags, bytes); }
API(kernel32, HGLOBAL, GlobalFree, (HGLOBAL h)) { return free_mem(h); }
API(kernel32, LPVOID, GlobalLock, (HGLOBAL h)) { return lock_mem(h); }
API(kernel32, BOOL, GlobalUnlock, (HGLOBAL h)) { return unlock_mem(h); }
/* Size of a global block: the requested size for GMEM_MOVEABLE handles, the usable heap size otherwise */
API(kernel32, SIZE_T, GlobalSize, (HGLOBAL h))
{
    if (!h)
        return fail(ERR_INVALID_HANDLE), 0;
    pthread_mutex_lock(&mem_lock);
    struct moveable *m = find_moveable(h);
    SIZE_T n = m ? m->size : 0;
    pthread_mutex_unlock(&mem_lock);
    return m ? n : malloc_usable_size(h);
}
API(kernel32, HLOCAL, LocalAlloc, (DWORD flags, SIZE_T bytes)) { return alloc_mem(flags, bytes); }
API(kernel32, HLOCAL, LocalFree, (HLOCAL h)) { return free_mem(h); }
API(kernel32, LPVOID, LocalLock, (HLOCAL h)) { return lock_mem(h); }
API(kernel32, BOOL, LocalUnlock, (HLOCAL h)) { return unlock_mem(h); }

API(kernel32, SIZE_T, HeapSize, (HANDLE heap, DWORD flags, const void *mem))
{
    (void)heap;
    (void)flags;
    if (!mem)
        return fail(ERR_INVALID_PARAMETER), (SIZE_T)-1;
    /* the C allocator does not keep the requested size; the usable size is at least that */
    return malloc_usable_size((void *)mem);
}

/* MEMORYSTATUS: all sizes are SIZE_T on x64 */
typedef struct {
    DWORD dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS_L;

API(kernel32, void, GlobalMemoryStatus, (MEMORYSTATUS_L * ms))
{
    if (!ms)
        return;
    SIZE_T page = (SIZE_T)sysconf(_SC_PAGESIZE);
    SIZE_T total = (SIZE_T)sysconf(_SC_PHYS_PAGES) * page;
    SIZE_T avail = (SIZE_T)sysconf(_SC_AVPHYS_PAGES) * page;
    ms->dwLength = sizeof *ms;
    ms->dwTotalPhys = total;
    ms->dwAvailPhys = avail;
    ms->dwMemoryLoad = total ? (DWORD)((total - avail) * 100 / total) : 0;
    ms->dwTotalPageFile = 0;
    ms->dwAvailPageFile = 0;
    ms->dwTotalVirtual = (SIZE_T)1 << 47;
    ms->dwAvailVirtual = ms->dwTotalVirtual;
}

/* ---- pointer encoding ------------------------------------------------------- */
/* Encoded pointers are the pointer xor a per-process cookie: enough to keep raw pointers out of
 * memory, which is what callers rely on (the Windows cookie is also process-private). */
static uintptr_t cookie_value;
static pthread_once_t cookie_once = PTHREAD_ONCE_INIT;

static void init_cookie(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    cookie_value = ((uintptr_t)getpid() << 32) ^ (uintptr_t)ts.tv_nsec ^ ((uintptr_t)ts.tv_sec * 0x9E3779B97F4A7C15ull);
}

static uintptr_t pointer_cookie(void)
{
    pthread_once(&cookie_once, init_cookie);
    return cookie_value;
}

API(kernel32, LPVOID, EncodePointer, (void *p)) { return (void *)((uintptr_t)p ^ pointer_cookie()); }
API(kernel32, LPVOID, DecodePointer, (void *p)) { return (void *)((uintptr_t)p ^ pointer_cookie()); }

/* ---- debugger and processor queries ------------------------------------------ */
API(kernel32, BOOL, IsDebuggerPresent, (void)) { return FALSE; }

API(kernel32, void, OutputDebugStringW, (const WCHAR *s))
{
    /* with no debugger attached Windows discards the text; the runtime shows it under AW_DEBUG */
    if (!aw_debug || !s)
        return;
    char *u = aw_w2u(s, -1);
    if (u)
        fprintf(stderr, "[debug] %s", u);
    free(u);
}

API(kernel32, BOOL, IsProcessorFeaturePresent, (DWORD feature))
{
    __builtin_cpu_init();
    switch (feature) {
    case 2: /* PF_COMPARE_EXCHANGE_DOUBLE */
    case 8: /* PF_RDTSC_INSTRUCTION_AVAILABLE */
    case 12: /* PF_NX_ENABLED */
    case 14: /* PF_COMPARE_EXCHANGE128 */
    case 15: /* PF_COMPARE64_EXCHANGE128 */
        return TRUE;
    case 3: return __builtin_cpu_supports("mmx");
    case 6: return __builtin_cpu_supports("sse");          /* PF_XMMI_INSTRUCTIONS_AVAILABLE */
    case 10: return __builtin_cpu_supports("sse2");         /* PF_XMMI64_INSTRUCTIONS_AVAILABLE */
    case 13: return __builtin_cpu_supports("sse3");         /* PF_SSE3_INSTRUCTIONS_AVAILABLE */
    case 36: return __builtin_cpu_supports("ssse3");        /* PF_SSSE3_INSTRUCTIONS_AVAILABLE */
    case 37: return __builtin_cpu_supports("sse4.1");       /* PF_SSE4_1_INSTRUCTIONS_AVAILABLE */
    case 38: return __builtin_cpu_supports("sse4.2");       /* PF_SSE4_2_INSTRUCTIONS_AVAILABLE */
    case 39: return __builtin_cpu_supports("avx");          /* PF_AVX_INSTRUCTIONS_AVAILABLE */
    case 40: return __builtin_cpu_supports("avx2");         /* PF_AVX2_INSTRUCTIONS_AVAILABLE */
    default: return FALSE;
    }
}

/* Sound hardware is not reached from the runtime: the call validates its arguments as Windows
 * does and reports success, so programs that beep for feedback keep running. */
API(kernel32, BOOL, Beep, (DWORD freq, DWORD dur))
{
    (void)dur;
    if (freq < 37 || freq > 32767)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    return TRUE;
}

/* ---- unwinding ----------------------------------------------------------------- */
/* Module lookup by program counter is not kept by the runtime: no module is reported. */
API(kernel32, void *, RtlPcToFileHeader, (void *pc, void **base))
{
    (void)pc;
    if (base)
        *base = NULL;
    return NULL;
}

API(kernel32, void, RtlUnwind, (void *target_frame, void *target_ip, void *rec, void *retval))
{
    __aw_kernel32_RtlUnwindEx(target_frame, target_ip, rec, retval, NULL, NULL);
}

/* ---- time ------------------------------------------------------------------------- */
static uint64_t ft_value(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void ft_store(FILETIME *f, uint64_t v)
{
    f->dwLowDateTime = (DWORD)v;
    f->dwHighDateTime = (DWORD)(v >> 32);
}

API(kernel32, void, GetSystemTimeAsFileTime, (FILETIME * ft)) { ft_store(ft, aw_now_filetime()); }

/* local time to UTC with the UTC offset in force at that instant */
API(kernel32, BOOL, LocalFileTimeToFileTime, (const FILETIME *local, FILETIME *utc))
{
    uint64_t v = ft_value(*local);
    time_t secs = (time_t)(v / 10000000ull - 11644473600ull);
    struct tm lt;
    if (!localtime_r(&secs, &lt))
        return fail(ERR_INVALID_PARAMETER), FALSE;
    int64_t delta = (int64_t)lt.tm_gmtoff * 10000000ll;
    ft_store(utc, (uint64_t)((int64_t)v - delta));
    return TRUE;
}

/* ---- directories ----------------------------------------------------------------- */
static BOOL set_cwd(const char *win)
{
    char *u = aw_unix_path(win);
    if (!u)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    int r = chdir(u);
    free(u);
    if (r != 0)
        return fail_errno(), FALSE;
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) {
        char *w = aw_win_path(cwd);
        if (w) {
            snprintf(aw_curdir_win, sizeof aw_curdir_win, "%s", w);
            free(w);
        }
    }
    return TRUE;
}

API(kernel32, BOOL, SetCurrentDirectoryA, (const char *dir)) { return set_cwd(dir); }

API(kernel32, BOOL, SetCurrentDirectoryW, (const WCHAR *dir))
{
    char *u = aw_w2u(dir, -1);
    BOOL r = set_cwd(u ? u : "");
    free(u);
    return r;
}

/* FormatMessageA: the wide formatter does the work; the text is converted to the ANSI code page */
API(kernel32, DWORD, FormatMessageA, (DWORD flags, const void *src, DWORD id, DWORD lang, char *buf, DWORD size,
                                      void *args))
{
    if (flags & 0x100u) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER: buf is a char** */
        WCHAR *w = NULL;
        DWORD n = __aw_kernel32_FormatMessageW(flags, src, id, lang, (WCHAR *)&w, 0, args);
        if (!n || !w)
            return 0;
        char *u = aw_w2u(w, -1);
        free(w);
        if (!u)
            return fail(ERR_NOT_ENOUGH_MEMORY), 0;
        *(char **)buf = u;
        return (DWORD)strlen(u);
    }
    if (!buf || !size)
        return fail(ERR_INVALID_PARAMETER), 0;
    WCHAR *w = malloc((size_t)size * sizeof(WCHAR));
    if (!w)
        return fail(ERR_NOT_ENOUGH_MEMORY), 0;
    DWORD n = __aw_kernel32_FormatMessageW(flags, src, id, lang, w, size, args);
    char *u = n ? aw_w2u(w, -1) : NULL;
    free(w);
    if (!u)
        return 0;
    size_t len = strlen(u);
    if (len > size - 1)
        len = size - 1;
    memcpy(buf, u, len);
    buf[len] = 0;
    free(u);
    return (DWORD)len;
}
