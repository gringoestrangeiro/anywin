/* anywin runtime - kernel32 extras: process heap, console probes, performance counter,
 * local time, FormatMessage, exception-handler plumbing that Rust std calls at startup,
 * and NtWriteFile. Everything here is a thin mapping onto libc or a fixed answer. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

#define HEAP_ZERO_MEMORY 0x00000008u
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100u
#define NTSTATUS_INVALID_HANDLE 0xC0000008u

/* ---- process heap: backed by the C allocator --------------------------- */
API(kernel32, HANDLE, GetProcessHeap, (void)) { return aw_process_heap(); }

API(kernel32, LPVOID, HeapAlloc, (HANDLE heap, DWORD flags, SIZE_T size))
{
    (void)heap;
    void *p = malloc(size ? size : 1);
    if (!p) {
        aw_set_last_error(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    if (flags & HEAP_ZERO_MEMORY)
        memset(p, 0, size);
    return p;
}

API(kernel32, BOOL, HeapFree, (HANDLE heap, DWORD flags, LPVOID mem))
{
    (void)heap;
    (void)flags;
    free(mem);
    return TRUE;
}

API(kernel32, LPVOID, HeapReAlloc, (HANDLE heap, DWORD flags, LPVOID mem, SIZE_T size))
{
    (void)heap;
    if (!mem)
        return __aw_kernel32_HeapAlloc(heap, flags, size);
    size_t old = malloc_usable_size(mem);
    void *p = realloc(mem, size ? size : 1);
    if (!p) {
        aw_set_last_error(8);
        return NULL;
    }
    if ((flags & HEAP_ZERO_MEMORY) && size > old)
        memset((char *)p + old, 0, size - old);
    return p;
}

/* ---- console probes ----------------------------------------------------- */
/* Handles are H_FILE objects on fd 0..2 or files. Only a real terminal reports a console. */
API(kernel32, BOOL, GetConsoleMode, (HANDLE h, DWORD *mode))
{
    int fd = aw_handle_fd(h);
    if (fd < 0 || !isatty(fd)) {
        aw_set_last_error(6); /* ERROR_INVALID_HANDLE: not a console */
        return FALSE;
    }
    if (mode)
        *mode = 0x0003; /* ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT */
    return TRUE;
}

API(kernel32, BOOL, SetConsoleMode, (HANDLE h, DWORD mode))
{
    (void)mode;
    return aw_handle_fd(h) >= 0 ? TRUE : (aw_set_last_error(6), FALSE);
}

API(kernel32, UINT, GetConsoleOutputCP, (void)) { return 437; }
API(kernel32, UINT, GetConsoleCP, (void)) { return 437; }

API(kernel32, BOOL, WriteConsoleW, (HANDLE h, const WCHAR *buf, DWORD n, DWORD *written, void *reserved))
{
    (void)reserved;
    if (written)
        *written = 0;
    int fd = aw_handle_fd(h);
    if (fd < 0) {
        aw_set_last_error(6);
        return FALSE;
    }
    char *u = aw_w2u(buf, (int)n);
    size_t len = u ? strlen(u) : 0;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, u + done, len - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        done += (size_t)w;
    }
    free(u);
    if (written)
        *written = n;
    return done == len ? TRUE : (aw_set_last_error(aw_errno_to_win(errno)), FALSE);
}

/* ntdll!NtWriteFile(handle, event, apc, apc_ctx, IO_STATUS_BLOCK*, buf, len, offset, key) */
API(ntdll, LONG, NtWriteFile, (HANDLE h, HANDLE event, void *apc, void *apc_ctx, uint64_t *iosb, const void *buf,
                               ULONG len, int64_t *offset, ULONG *key))
{
    (void)event;
    (void)apc;
    (void)apc_ctx;
    (void)offset;
    (void)key;
    int fd = aw_handle_fd(h);
    LONG status = 0;
    DWORD done = 0;
    if (fd < 0)
        status = (LONG)NTSTATUS_INVALID_HANDLE;
    else {
        const uint8_t *p = buf;
        while (done < len) {
            ssize_t w = write(fd, p + done, len - done);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                status = (LONG)0xC0000185u; /* STATUS_IO_DEVICE_ERROR */
                break;
            }
            done += (DWORD)w;
        }
    }
    if (iosb) {
        iosb[0] = (uint64_t)(uint32_t)status; /* Status */
        iosb[1] = done;                       /* Information */
    }
    return status;
}

/* NTSTATUS -> Win32 error. Only the statuses this runtime produces are named. */
API(ntdll, DWORD, RtlNtStatusToDosError, (LONG status))
{
    switch ((uint32_t)status) {
    case 0: return 0;
    case NTSTATUS_INVALID_HANDLE: return 6;
    case 0xC000000Du: return 87;  /* STATUS_INVALID_PARAMETER */
    case 0xC0000034u: return 2;   /* STATUS_OBJECT_NAME_NOT_FOUND */
    case 0xC000003Au: return 3;   /* STATUS_OBJECT_PATH_NOT_FOUND */
    case 0xC0000022u: return 5;   /* STATUS_ACCESS_DENIED */
    default: return 31;           /* ERROR_GEN_FAILURE */
    }
}

/* ---- high resolution counter and local time ----------------------------- */
API(kernel32, BOOL, QueryPerformanceFrequency, (int64_t *freq))
{
    if (freq)
        *freq = 1000000000;
    return TRUE;
}

API(kernel32, BOOL, QueryPerformanceCounter, (int64_t *count))
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (count)
        *count = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    return TRUE;
}

API(kernel32, void, GetLocalTime, (SYSTEMTIME *st))
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}

/* ---- fibers, stack guarantee, vectored handlers -------------------------- */
API(kernel32, BOOL, IsThreadAFiber, (void)) { return FALSE; }

/* No guard page is reserved for the stack, so the requested guarantee is trivially met. */
API(kernel32, BOOL, SetThreadStackGuarantee, (ULONG *size))
{
    (void)size;
    return TRUE;
}

/* Registered handlers are never invoked (faults reach the SEH layer), but the handle must be
 * non-NULL because callers treat NULL as failure. */
API(kernel32, void *, AddVectoredExceptionHandler, (ULONG first, void *handler))
{
    (void)first;
    (void)handler;
    static char marker[16];
    return marker;
}

API(kernel32, ULONG, RemoveVectoredExceptionHandler, (void *handle))
{
    (void)handle;
    return 1;
}

API(kernel32, void, RtlCaptureContext, (void *ctx))
{
    if (ctx)
        memset(ctx, 0, 0x4d0); /* sizeof(CONTEXT) on x64 */
}

API(kernel32, void, RtlUnwindEx, (void *target_frame, void *target_ip, void *rec, void *retval, void *ctx,
                                  void *history))
{
    (void)target_frame;
    (void)target_ip;
    (void)rec;
    (void)retval;
    (void)ctx;
    (void)history;
    aw_fatal("RtlUnwindEx: unwinding to a Windows handler is not supported");
}

/* ---- FormatMessageW ----------------------------------------------------- */
static const struct { DWORD id; const char *text; } messages[] = {
    {0, "The operation completed successfully."},
    {2, "The system cannot find the file specified."},
    {3, "The system cannot find the path specified."},
    {4, "The system cannot open the file."},
    {5, "Access is denied."},
    {6, "The handle is invalid."},
    {8, "Not enough memory resources are available to process this command."},
    {18, "There are no more files."},
    {32, "The process cannot access the file because it is being used by another process."},
    {87, "The parameter is incorrect."},
    {112, "There is not enough space on the disk."},
    {122, "The data area passed to a system call is too small."},
    {183, "Cannot create a file when that file already exists."},
    {206, "The filename or extension is too long."},
    {1060, "The specified service does not exist as an installed service."},
    {1812, "The specified image file did not contain a resource section."},
};

/* Only FORMAT_MESSAGE_FROM_SYSTEM text is known; other sources fail. */
API(kernel32, DWORD, FormatMessageW, (DWORD flags, const void *source, DWORD msgid, DWORD lang, WCHAR *buf, DWORD size,
                                      void *args))
{
    (void)source;
    (void)lang;
    (void)args;
    const char *text = NULL;
    char unknown[64];
    for (size_t i = 0; i < sizeof messages / sizeof *messages; i++)
        if (messages[i].id == msgid) {
            text = messages[i].text;
            break;
        }
    if (!text) {
        snprintf(unknown, sizeof unknown, "The system returned error 0x%08x.", (unsigned)msgid);
        text = unknown;
    }
    char *full = NULL;
    if (asprintf(&full, "%s\r\n", text) < 0)
        return 0;
    WCHAR *w = aw_u2w(full, -1);
    free(full);
    size_t n = aw_wcslen(w);
    DWORD ret = 0;
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        WCHAR *out = malloc((n + 1) * sizeof(WCHAR));
        memcpy(out, w, (n + 1) * sizeof(WCHAR));
        *(WCHAR **)buf = out;
        ret = (DWORD)n;
    } else if (buf && size) {
        DWORD cpy = (DWORD)n < size - 1 ? (DWORD)n : size - 1;
        memcpy(buf, w, cpy * sizeof(WCHAR));
        buf[cpy] = 0;
        ret = cpy;
    }
    free(w);
    if (!ret)
        aw_set_last_error(122);
    return ret;
}

/* ---- msvcrt startup helper ---------------------------------------------- */
API(msvcrt, void, _fpreset, (void)) { }
