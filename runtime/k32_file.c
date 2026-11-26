/* kernel32: file handle queries and updates, ANSI wrappers of the wide file APIs, console input,
 * overlapped result lookup and the std-handle table. Handles are H_FILE objects over fds (see file.c);
 * pipes and sockets are H_FILE objects too, so they answer the same calls. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR_FILE_NOT_FOUND 2
#define ERR_ACCESS_DENIED 5
#define ERR_INVALID_HANDLE 6
#define ERR_INVALID_PARAMETER 87
#define FILE_TYPE_DISK 1
#define FILE_TYPE_CHAR 2
#define FILE_TYPE_PIPE 3
#define HANDLE_FLAG_INHERIT 0x1

/* kernel32.c and file.c export these; declared here for the wrappers below */
HANDLE WINAPI __aw_kernel32_FindFirstFileExW(const WCHAR *name, DWORD level, void *data, DWORD op, void *filter,
                                             DWORD flags);
BOOL WINAPI __aw_kernel32_FindNextFileW(HANDLE h, void *data);
DWORD WINAPI __aw_kernel32_GetTempPathW(DWORD len, WCHAR *buf);
DWORD WINAPI __aw_kernel32_GetCurrentDirectoryW(DWORD len, WCHAR *buf);

/* WIN32_FIND_DATAA and WIN32_FIND_DATAW share one layout apart from the string element type */
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
} FIND_DATA_WL;

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} FIND_DATA_AL;

static DWORD fail_err(DWORD e)
{
    aw_set_last_error(e);
    return 0;
}

static DWORD fail_errno(void)
{
    return fail_err(aw_errno_to_win(errno));
}

static int file_fd(HANDLE h)
{
    int fd = aw_handle_fd(h);
    if (fd < 0)
        aw_set_last_error(ERR_INVALID_HANDLE);
    return fd;
}

API(kernel32, BOOL, GetFileSizeEx, (HANDLE h, LONGLONG *size))
{
    int fd = file_fd(h);
    struct stat st;
    if (fd < 0)
        return FALSE;
    if (fstat(fd, &st) != 0) {
        fail_errno();
        return FALSE;
    }
    if (size)
        *size = S_ISREG(st.st_mode) ? (LONGLONG)st.st_size : 0;
    return TRUE;
}

API(kernel32, BOOL, SetFilePointerEx, (HANDLE h, LONGLONG dist, LONGLONG *newpos, DWORD method))
{
    int fd = file_fd(h);
    if (fd < 0)
        return FALSE;
    int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : -1;
    if (whence < 0) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return FALSE;
    }
    off_t r = lseek(fd, (off_t)dist, whence);
    if (r < 0) {
        fail_errno();
        return FALSE;
    }
    if (newpos)
        *newpos = (LONGLONG)r;
    return TRUE;
}

API(kernel32, BOOL, SetEndOfFile, (HANDLE h))
{
    int fd = file_fd(h);
    if (fd < 0)
        return FALSE;
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(fd, pos) != 0) {
        fail_errno();
        return FALSE;
    }
    return TRUE;
}

API(kernel32, DWORD, GetFileType, (HANDLE h))
{
    int fd = file_fd(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
        return 0; /* FILE_TYPE_UNKNOWN */
    if (isatty(fd) || S_ISCHR(st.st_mode))
        return FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return FILE_TYPE_PIPE;
    return FILE_TYPE_DISK;
}

API(kernel32, BOOL, FlushFileBuffers, (HANDLE h))
{
    int fd = file_fd(h);
    if (fd < 0)
        return FALSE;
    /* pipes, sockets and terminals have nothing to flush to the disk */
    if (fsync(fd) != 0 && errno != EINVAL && errno != EROFS)
        return fail_errno(), FALSE;
    return TRUE;
}

API(kernel32, BOOL, SetHandleInformation, (HANDLE h, DWORD mask, DWORD flags))
{
    aw_object *o = aw_handle_object(h);
    if (!o)
        return fail_err(ERR_INVALID_HANDLE), FALSE;
    if (mask & HANDLE_FLAG_INHERIT)
        *aw_obj_inherit(o) = (flags & HANDLE_FLAG_INHERIT) ? 1 : 0;
    return TRUE;
}

/* OVERLAPPED: Internal (NTSTATUS) at +0, InternalHigh (bytes transferred) at +8. Reads and writes
 * complete before they return, so the result is always final. */
API(kernel32, BOOL, GetOverlappedResult, (HANDLE h, void *ov, DWORD *transferred, BOOL wait))
{
    (void)wait;
    if (!ov || !aw_handle_object(h))
        return fail_err(ERR_INVALID_HANDLE), FALSE;
    uint64_t status = *(uint64_t *)((char *)ov + 0);
    if (status) {
        if (transferred)
            *transferred = 0;
        return fail_err(aw_errno_to_win(EIO)), FALSE;
    }
    if (transferred)
        *transferred = (DWORD)*(uint64_t *)((char *)ov + 8);
    return TRUE;
}

/* ---- std handles ----------------------------------------------------------- */
void aw_set_std_handle(int which, HANDLE h);

API(kernel32, BOOL, SetStdHandle, (DWORD which, HANDLE h))
{
    int idx = which == (DWORD)-10 ? 0 : which == (DWORD)-11 ? 1 : which == (DWORD)-12 ? 2 : -1;
    if (idx < 0)
        return fail_err(ERR_INVALID_PARAMETER), FALSE;
    aw_set_std_handle(idx, h);
    return TRUE;
}

/* ---- console input ----------------------------------------------------------- */
API(kernel32, BOOL, ReadConsoleW, (HANDLE h, WCHAR *buf, DWORD n, DWORD *nread, void *ctl))
{
    (void)ctl;
    if (nread)
        *nread = 0;
    int fd = file_fd(h);
    if (fd < 0)
        return FALSE;
    /* line input: bytes up to and including a newline, decoded from UTF-8 */
    size_t cap = (size_t)n * 4 + 1, got = 0;
    char *tmp = malloc(cap);
    if (!tmp)
        return fail_err(8), FALSE;
    while (got < cap - 1) {
        ssize_t r = read(fd, tmp + got, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            free(tmp);
            return fail_errno(), FALSE;
        }
        if (r == 0)
            break;
        if (tmp[got++] == '\n')
            break;
    }
    int wn = aw_u2w_buf(tmp, (int)got, buf, (int)n);
    free(tmp);
    if (nread)
        *nread = (DWORD)(wn < 0 ? 0 : wn);
    return TRUE;
}

/* ---- deletion ----------------------------------------------------------------- */
API(kernel32, BOOL, DeleteFileA, (const char *name))
{
    if (!name)
        return fail_err(ERR_INVALID_PARAMETER), FALSE;
    char *u = aw_unix_path(name);
    struct stat st;
    if (!u)
        return fail_err(ERR_INVALID_PARAMETER), FALSE;
    if (stat(u, &st) == 0 && S_ISDIR(st.st_mode)) {
        free(u);
        return fail_err(ERR_ACCESS_DENIED), FALSE;
    }
    int r = unlink(u);
    int e = errno;
    free(u);
    if (r != 0)
        return fail_err(aw_errno_to_win(e)), FALSE;
    return TRUE;
}

/* ---- find, ANSI forms over the wide implementation ------------------------ */
static void find_to_a(const FIND_DATA_WL *w, FIND_DATA_AL *a)
{
    memcpy(a, w, offsetof(FIND_DATA_WL, cFileName));
    char *n = aw_w2u(w->cFileName, -1);
    snprintf(a->cFileName, sizeof a->cFileName, "%s", n ? n : "");
    free(n);
    char *alt = aw_w2u(w->cAlternateFileName, -1);
    snprintf(a->cAlternateFileName, sizeof a->cAlternateFileName, "%s", alt ? alt : "");
    free(alt);
}

API(kernel32, HANDLE, FindFirstFileW, (const WCHAR *name, FIND_DATA_WL *data))
{
    return __aw_kernel32_FindFirstFileExW(name, 0, data, 0, NULL, 0);
}

API(kernel32, HANDLE, FindFirstFileA, (const char *name, FIND_DATA_AL *data))
{
    if (!name)
        return fail_err(ERR_INVALID_PARAMETER), INVALID_HANDLE_VALUE;
    WCHAR *w = aw_u2w(name, -1);
    FIND_DATA_WL wd;
    HANDLE h = __aw_kernel32_FindFirstFileExW(w, 0, &wd, 0, NULL, 0);
    free(w);
    if (h != INVALID_HANDLE_VALUE && data)
        find_to_a(&wd, data);
    return h;
}

API(kernel32, BOOL, FindNextFileA, (HANDLE h, FIND_DATA_AL *data))
{
    FIND_DATA_WL wd;
    if (!__aw_kernel32_FindNextFileW(h, &wd))
        return FALSE;
    if (data)
        find_to_a(&wd, data);
    return TRUE;
}

/* ---- Windows directory-relative names --------------------------------------- */
/* Returns the length needed for `s` (with the terminating NUL) or copies it when it fits. */
static DWORD copy_ansi(const char *s, DWORD len, char *buf)
{
    DWORD need = (DWORD)strlen(s) + 1;
    if (len < need)
        return need;
    memcpy(buf, s, need);
    return need - 1;
}

API(kernel32, UINT, GetSystemDirectoryA, (char *buf, UINT len))
{
    return copy_ansi("C:\\Windows\\System32", len, buf);
}

API(kernel32, UINT, GetWindowsDirectoryA, (char *buf, UINT len))
{
    return copy_ansi("C:\\Windows", len, buf);
}

API(kernel32, DWORD, GetCurrentDirectoryA, (DWORD len, char *buf))
{
    WCHAR wbuf[4096];
    DWORD n = __aw_kernel32_GetCurrentDirectoryW(4096, wbuf);
    if (!n)
        return 0;
    char *u = aw_w2u(wbuf, -1);
    DWORD r = copy_ansi(u ? u : "", len, buf);
    free(u);
    return r;
}

API(kernel32, DWORD, GetTempPathA, (DWORD len, char *buf))
{
    WCHAR wbuf[4096];
    DWORD n = __aw_kernel32_GetTempPathW(4096, wbuf);
    if (!n)
        return 0;
    char *u = aw_w2u(wbuf, -1);
    DWORD r = copy_ansi(u ? u : "", len, buf);
    free(u);
    return r;
}
