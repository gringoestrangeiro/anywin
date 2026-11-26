/* anywin runtime - files, directories, paths and file mappings (kernel32).
 *
 * Windows paths reach libc through aw_unix_path()/aw_unix_pathW() (drive roots, separators and
 * case-insensitive matching). A file handle is an H_FILE object holding the fd and, for
 * FILE_FLAG_DELETE_ON_CLOSE or FileDispositionInfo, the path to unlink on close. A find handle
 * is an H_FIND object whose ext points to a find_state. */
#include "aw.h"
#include "k32.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ERR_FILE_NOT_FOUND 2
#define ERR_PATH_NOT_FOUND 3
#define ERR_ACCESS_DENIED 5
#define ERR_INVALID_HANDLE 6
#define ERR_INVALID_PARAMETER 87
#define ERR_NOT_SUPPORTED 50
#define ERR_NO_MORE_FILES 18
#define ERR_ALREADY_EXISTS 183
#define ERR_BAD_LENGTH 24
#define ERR_CALL_NOT_IMPLEMENTED 120

#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_MAP_COPY 0x1
#define FILE_MAP_WRITE 0x2
#define MOVEFILE_REPLACE_EXISTING 0x1

#define FT_EPOCH_SECS 11644473600ull

BOOL WINAPI __aw_kernel32_CloseHandle(HANDLE h);

/* WIN32_FIND_DATAW, x64 layout */
typedef struct find_data_w {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
} FIND_DATA_W;

/* ---- time and attribute conversion ------------------------------------- */
static uint64_t ts_to_ft64(struct timespec ts)
{
    return ((uint64_t)ts.tv_sec + FT_EPOCH_SECS) * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
}

static void put_ft(FILETIME *ft, struct timespec ts)
{
    uint64_t t = ts_to_ft64(ts);
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static struct timespec ft_to_ts(uint64_t t)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(t / 10000000ull) - (time_t)FT_EPOCH_SECS;
    ts.tv_nsec = (long)((t % 10000000ull) * 100u);
    return ts;
}

static DWORD attrs_of(const struct stat *st, const char *name)
{
    DWORD a = 0;
    if (S_ISDIR(st->st_mode))
        a |= FILE_ATTRIBUTE_DIRECTORY;
    if (S_ISLNK(st->st_mode))
        a |= FILE_ATTRIBUTE_REPARSE_POINT;
    if (!(st->st_mode & S_IWUSR))
        a |= FILE_ATTRIBUTE_READONLY;
    if (name && name[0] == '.' && strcmp(name, ".") && strcmp(name, ".."))
        a |= FILE_ATTRIBUTE_HIDDEN;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

static DWORD fail(DWORD err)
{
    aw_set_last_error(err);
    return 0;
}

static DWORD fail_errno(void)
{
    aw_set_last_error(aw_errno_to_win(errno));
    return 0;
}

/* ---- Windows path helpers ----------------------------------------------- */
/* Turns a (possibly relative) Windows path into an absolute one with normalized components. */
static void win_full_path(const char *in, char *out, size_t cap)
{
    char gc[4096], joined[8192], cwd[4096];
    if (!getcwd(gc, sizeof gc))
        gc[0] = 0;
    char *wc = aw_win_path(gc[0] ? gc : "/");
    snprintf(cwd, sizeof cwd, "%s", wc);
    free(wc);
    int absdrive = strlen(in) >= 3 && isalpha((unsigned char)in[0]) && in[1] == ':' && (in[2] == '\\' || in[2] == '/');
    if (absdrive)
        snprintf(joined, sizeof joined, "%s", in);
    else if (in[0] == '\\' || in[0] == '/')
        snprintf(joined, sizeof joined, "%.2s%s", cwd, in);
    else if (isalpha((unsigned char)in[0]) && in[1] == ':')
        snprintf(joined, sizeof joined, "%c:\\%s", toupper((unsigned char)in[0]), in + 2);
    else
        snprintf(joined, sizeof joined, "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '\\' ? "" : "\\", in);

    /* normalize: drive root first, then components with . and .. resolved */
    char comps[8192] = "";
    size_t cl = 0;
    char dr = (char)toupper((unsigned char)joined[0]);
    const char *p = joined + 2;
    while (*p) {
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        const char *e = p;
        while (*e && *e != '\\' && *e != '/')
            e++;
        size_t len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') {
            /* current directory: nothing to add */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (cl > 0 && comps[cl - 1] != '\\')
                cl--;
            if (cl > 0)
                cl--;
        } else if (cl + 1 + len < sizeof comps) {
            comps[cl++] = '\\';
            memcpy(comps + cl, p, len);
            cl += len;
        }
        p = e;
    }
    comps[cl] = 0;
    snprintf(out, cap, "%c:%s", dr, cl ? comps : "\\");
}

/* Windows path -> unix directory, last component (pattern) split off. */
static void split_pattern(const char *u, char *dirwin, size_t cap, const char **pattern)
{
    const char *s = strrchr(u, '\\');
    const char *s2 = strrchr(u, '/');
    if (!s || (s2 && s2 > s))
        s = s2;
    if (!s) {
        snprintf(dirwin, cap, "%s", ".");
        *pattern = u;
        return;
    }
    size_t n = (size_t)(s - u);
    if (n == 0)
        snprintf(dirwin, cap, "\\");
    else if (n == 2 && u[1] == ':')
        snprintf(dirwin, cap, "%.2s\\", u);
    else
        snprintf(dirwin, cap, "%.*s", (int)n, u);
    *pattern = s + 1;
}

/* ---- CreateFile / ReadFile ----------------------------------------------- */
static HANDLE open_file(const char *wpath, DWORD access, DWORD disp, DWORD flags)
{
    char *u = aw_unix_path(wpath);
    struct stat st;
    int existed = stat(u, &st) == 0;
    if (existed && S_ISDIR(st.st_mode) && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
        free(u);
        aw_set_last_error(ERR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    int rd = (access & 0x80000000u) || !(access & 0x40000000u);
    int wr = access & 0x40000000u;
    int of = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case 1: of |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: of |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                           /* OPEN_EXISTING */
    case 4: of |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: of |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    default:
        free(u);
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (existed && S_ISDIR(st.st_mode))
        of = O_RDONLY | O_DIRECTORY;
    int fd = open(u, of | O_CLOEXEC, 0666);
    if (fd < 0) {
        int e = errno;
        free(u);
        aw_set_last_error(aw_errno_to_win(e));
        return INVALID_HANDLE_VALUE;
    }
    /* CREATE_ALWAYS and OPEN_ALWAYS report ERROR_ALREADY_EXISTS on success when the file was there */
    aw_set_last_error((disp == 2 || disp == 4) && existed ? ERR_ALREADY_EXISTS : 0);
    aw_object *o = aw_object_new(H_FILE);
    aw_obj_set_fd(o, fd);
    *aw_obj_path(o) = u; /* freed by the handle close path */
    *aw_obj_delete_on_close(o) = (flags & FILE_FLAG_DELETE_ON_CLOSE) ? 1 : 0;
    HANDLE h = aw_handle_alloc(H_FILE, o);
    if (!h) {
        close(fd);
        free(u);
        free(o);
        aw_set_last_error(ERR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

API(kernel32, HANDLE, CreateFileW, (const WCHAR *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags,
                                    HANDLE tmpl))
{
    (void)share;
    (void)sa;
    (void)tmpl;
    char *w = aw_w2u(name, -1);
    if (!w) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (aw_pipe_name(w)) { /* \\.\pipe\NAME connects to a named pipe (k32_pipe.c) */
        HANDLE ph = aw_pipe_open(w, access, disp);
        free(w);
        return ph;
    }
    HANDLE h = open_file(w, access, disp, flags);
    free(w);
    return h;
}

API(kernel32, HANDLE, CreateFileA, (const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags,
                                    HANDLE tmpl))
{
    (void)share;
    (void)sa;
    (void)tmpl;
    if (!name) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (aw_pipe_name(name))
        return aw_pipe_open(name, access, disp);
    return open_file(name, access, disp, flags);
}

API(kernel32, BOOL, ReadFile, (HANDLE h, void *buf, DWORD n, DWORD *nread, void *ov))
{
    if (nread)
        *nread = 0;
    int fd = aw_handle_fd(h);
    if (fd < 0)
        return fail(ERR_INVALID_HANDLE), FALSE;
    ssize_t r;
    if (ov) {
        /* OVERLAPPED: Offset and OffsetHigh are at +16 and +20; the call is synchronous */
        uint64_t off = *(uint32_t *)((char *)ov + 16) | ((uint64_t)*(uint32_t *)((char *)ov + 20) << 32);
        do
            r = pread(fd, buf, n, (off_t)off);
        while (r < 0 && errno == EINTR);
    } else {
        do
            r = read(fd, buf, n);
        while (r < 0 && errno == EINTR);
    }
    if (r < 0) {
        fail_errno();
        return FALSE;
    }
    if (ov) { /* OVERLAPPED result: Internal = status 0, InternalHigh = bytes transferred */
        *(uint64_t *)((char *)ov + 0) = 0;
        *(uint64_t *)((char *)ov + 8) = (uint64_t)r;
    }
    if (nread)
        *nread = (DWORD)r;
    return TRUE;
}

/* ---- find -------------------------------------------------------------------- */
struct find_state {
    DIR *dir;
    char *pattern; /* UTF-8, Windows wildcards (* and ?) */
    char *dirunix;
};

void aw_find_release(void *p)
{
    struct find_state *st = p;
    if (!st)
        return;
    if (st->dir)
        closedir(st->dir);
    free(st->pattern);
    free(st->dirunix);
    free(st);
}

static void fill_find_data(FIND_DATA_W *d, const char *dirunix, const char *name)
{
    memset(d, 0, sizeof *d);
    struct stat st;
    char *full = NULL;
    if (asprintf(&full, "%s/%s", dirunix, name) >= 0 && lstat(full, &st) == 0) {
        d->dwFileAttributes = attrs_of(&st, name);
        put_ft(&d->ftCreationTime, st.st_ctim);
        put_ft(&d->ftLastAccessTime, st.st_atim);
        put_ft(&d->ftLastWriteTime, st.st_mtim);
        uint64_t size = (uint64_t)st.st_size;
        d->nFileSizeHigh = (DWORD)(size >> 32);
        d->nFileSizeLow = (DWORD)size;
    } else {
        d->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    free(full);
    WCHAR *w = aw_u2w(name, -1);
    size_t wl = aw_wcslen(w);
    if (wl > 259)
        wl = 259;
    memcpy(d->cFileName, w, wl * sizeof(WCHAR));
    d->cFileName[wl] = 0;
    free(w);
}

/* advances to the next entry matching the pattern; 0 when there are none left */
static int next_match(struct find_state *st, FIND_DATA_W *d)
{
    struct dirent *de;
    while ((de = readdir(st->dir))) {
        if (fnmatch(st->pattern, de->d_name, FNM_CASEFOLD | FNM_NOESCAPE) == 0) {
            fill_find_data(d, st->dirunix, de->d_name);
            return 1;
        }
    }
    return 0;
}

API(kernel32, HANDLE, FindFirstFileExW, (const WCHAR *name, DWORD level, FIND_DATA_W *data, DWORD searchop,
                                         void *filter, DWORD flags))
{
    (void)level;
    (void)searchop;
    (void)filter;
    (void)flags;
    char *u = aw_w2u(name, -1);
    if (!u || !*u) {
        free(u);
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    char dirwin[4096];
    const char *pattern;
    split_pattern(u, dirwin, sizeof dirwin, &pattern);
    struct find_state *st = calloc(1, sizeof *st);
    st->pattern = strdup(strcmp(pattern, "*.*") ? pattern : "*");
    st->dirunix = aw_unix_path(dirwin);
    free(u);
    st->dir = opendir(st->dirunix);
    if (!st->dir) {
        int e = errno;
        aw_find_release(st);
        aw_set_last_error(aw_errno_to_win(e));
        return INVALID_HANDLE_VALUE;
    }
    if (!next_match(st, data)) {
        aw_find_release(st);
        aw_set_last_error(ERR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    aw_object *o = aw_object_new(H_FIND);
    *aw_obj_ext(o) = st;
    HANDLE h = aw_handle_alloc(H_FIND, o);
    if (!h) {
        aw_find_release(st);
        free(o);
        aw_set_last_error(8);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

API(kernel32, BOOL, FindNextFileW, (HANDLE h, FIND_DATA_W *data))
{
    aw_object *o = aw_handle_get(h, H_FIND);
    struct find_state *st = o ? *aw_obj_ext(o) : NULL;
    if (!st) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return FALSE;
    }
    if (!next_match(st, data)) {
        aw_set_last_error(ERR_NO_MORE_FILES);
        return FALSE;
    }
    return TRUE;
}

API(kernel32, BOOL, FindClose, (HANDLE h))
{
    return __aw_kernel32_CloseHandle(h);
}

/* ---- directories and paths ------------------------------------------------ */
API(kernel32, DWORD, GetCurrentDirectoryW, (DWORD len, WCHAR *buf))
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd))
        return fail_errno();
    char *win = aw_win_path(cwd);
    WCHAR *w = aw_u2w(win, -1);
    free(win);
    DWORD n = (DWORD)aw_wcslen(w);
    if (len <= n || !buf) {
        free(w);
        return n + 1;
    }
    memcpy(buf, w, (n + 1) * sizeof(WCHAR));
    free(w);
    return n;
}

API(kernel32, DWORD, GetFullPathNameW, (const WCHAR *name, DWORD len, WCHAR *buf, WCHAR **filepart))
{
    char *u = aw_w2u(name, -1);
    if (!u)
        return fail(ERR_INVALID_PARAMETER);
    char full[8192];
    win_full_path(u, full, sizeof full);
    free(u);
    WCHAR *w = aw_u2w(full, -1);
    DWORD n = (DWORD)aw_wcslen(w);
    if (len <= n || !buf) {
        free(w);
        return n + 1;
    }
    memcpy(buf, w, (n + 1) * sizeof(WCHAR));
    if (filepart) {
        *filepart = NULL;
        for (DWORD i = 0; i < n; i++)
            if (buf[i] == '\\')
                *filepart = buf + i + 1;
    }
    free(w);
    return n;
}

static int file_exists(const char *wpath)
{
    char *u = aw_unix_path(wpath);
    struct stat st;
    int ok = stat(u, &st) == 0 && S_ISREG(st.st_mode);
    free(u);
    return ok;
}

/* tries one candidate; on success stores its full Windows path in *found */
static int try_candidate(const char *name, const char *ext, int add_ext, char **found)
{
    char full[8192], cand[8192];
    snprintf(cand, sizeof cand, "%s%s", name, add_ext ? ext : "");
    win_full_path(cand, full, sizeof full);
    if (!file_exists(full))
        return 0;
    *found = strdup(full);
    return 1;
}

API(kernel32, DWORD, SearchPathW, (const WCHAR *path, const WCHAR *file, const WCHAR *ext, DWORD len, WCHAR *buf,
                                   WCHAR **filepart))
{
    char *f = aw_w2u(file, -1);
    char *e = aw_w2u(ext, -1);
    char *dirs = aw_w2u(path, -1);
    if (!f || !*f) {
        free(f);
        free(e);
        free(dirs);
        return fail(ERR_INVALID_PARAMETER);
    }
    const char *ex = e && *e ? e : "";
    const char *dot = strrchr(f, '.');
    int has_ext = dot && !strchr(dot, '\\') && !strchr(dot, '/');
    char *found = NULL;
    if (strchr(f, '\\') || strchr(f, '/') || (isalpha((unsigned char)f[0]) && f[1] == ':')) {
        if (!try_candidate(f, ex, 0, &found) && !has_ext)
            try_candidate(f, ex, 1, &found);
    } else {
        /* explicit directories (';' separated, Windows form); without them: the working directory, then PATH */
        int explicit_dirs = dirs && *dirs;
        char *list = strdup(explicit_dirs ? dirs : ".");
        for (char *tok = strtok(list, ";"); tok && !found; tok = strtok(NULL, ";")) {
            char dirw[4096], name[8192];
            if (tok[0] == '/') {
                char *wp = aw_win_path(tok);
                snprintf(dirw, sizeof dirw, "%s", wp);
                free(wp);
            } else
                snprintf(dirw, sizeof dirw, "%s", tok);
            snprintf(name, sizeof name, "%s\\%s", dirw, f);
            if (!try_candidate(name, ex, 0, &found) && !has_ext)
                try_candidate(name, ex, 1, &found);
        }
        if (!explicit_dirs && !found) {
            /* PATH uses ':' separators and Linux directories */
            const char *pe = getenv("PATH");
            char *pl = strdup(pe ? pe : "");
            for (char *tok = strtok(pl, ":"); tok && !found; tok = strtok(NULL, ":")) {
                char *wp = aw_win_path(tok);
                char name[8192];
                snprintf(name, sizeof name, "%s\\%s", wp, f);
                free(wp);
                if (!try_candidate(name, ex, 0, &found) && !has_ext)
                    try_candidate(name, ex, 1, &found);
            }
            free(pl);
        }
        free(list);
    }
    free(f);
    free(e);
    free(dirs);
    if (!found)
        return fail(ERR_FILE_NOT_FOUND);
    WCHAR *w = aw_u2w(found, -1);
    DWORD n = (DWORD)aw_wcslen(w);
    DWORD ret = n;
    if (len <= n || !buf) {
        ret = n + 1;
    } else {
        memcpy(buf, w, (n + 1) * sizeof(WCHAR));
        if (filepart) {
            *filepart = NULL;
            for (DWORD i = 0; i < n; i++)
                if (buf[i] == '\\')
                    *filepart = buf + i + 1;
        }
    }
    free(w);
    free(found);
    return ret;
}

/* %TEMP% as a Windows directory with a trailing backslash; created when missing. */
API(kernel32, DWORD, GetTempPathW, (DWORD len, WCHAR *buf))
{
    const char *t = getenv("TEMP");
    if (!t || !*t)
        t = "Z:\\tmp";
    char dir[4096];
    size_t n = strlen(t);
    snprintf(dir, sizeof dir, "%s%s", t, n && t[n - 1] == '\\' ? "" : "\\");
    char *u = aw_unix_path(dir);
    mkdir(u, 0700);
    free(u);
    WCHAR *w = aw_u2w(dir, -1);
    DWORD wl = (DWORD)aw_wcslen(w);
    if (len <= wl || !buf) {
        free(w);
        return wl + 1;
    }
    memcpy(buf, w, (wl + 1) * sizeof(WCHAR));
    free(w);
    return wl;
}

static int copy_file_unix(const char *from, const char *to)
{
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t r;
    int ok = 0;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < r;) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                ok = -1;
                goto done;
            }
            off += w;
        }
    }
    ok = r < 0 ? -1 : 0;
done:
    close(in);
    close(out);
    return ok;
}

API(kernel32, BOOL, MoveFileExW, (const WCHAR *from, const WCHAR *to, DWORD flags))
{
    char *uf = aw_unix_pathW(from), *ut = aw_unix_pathW(to);
    BOOL ok = FALSE;
    struct stat st;
    if (!uf || !ut) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
    } else if (!(flags & MOVEFILE_REPLACE_EXISTING) && lstat(ut, &st) == 0) {
        aw_set_last_error(ERR_ALREADY_EXISTS);
    } else if (rename(uf, ut) == 0) {
        ok = TRUE;
    } else if (errno == EXDEV && lstat(uf, &st) == 0 && !S_ISDIR(st.st_mode) && copy_file_unix(uf, ut) == 0 &&
               unlink(uf) == 0) {
        ok = TRUE; /* different file systems: copy, then remove the source */
    } else {
        fail_errno();
    }
    free(uf);
    free(ut);
    return ok;
}

/* ---- handle information --------------------------------------------------- */
struct by_handle_info {
    DWORD attributes;
    FILETIME ctime, atime, mtime;
    DWORD volume;
    DWORD size_hi, size_lo;
    DWORD links;
    DWORD index_hi, index_lo;
};

API(kernel32, BOOL, GetFileInformationByHandle, (HANDLE h, struct by_handle_info *info))
{
    int fd = aw_handle_fd(h);
    struct stat st;
    if (fd < 0)
        return fail(ERR_INVALID_HANDLE), FALSE;
    if (fstat(fd, &st) != 0)
        return fail_errno(), FALSE;
    memset(info, 0, sizeof *info);
    info->attributes = attrs_of(&st, NULL);
    put_ft(&info->ctime, st.st_ctim);
    put_ft(&info->atime, st.st_atim);
    put_ft(&info->mtime, st.st_mtim);
    info->volume = (DWORD)st.st_dev;
    info->size_hi = (DWORD)((uint64_t)st.st_size >> 32);
    info->size_lo = (DWORD)st.st_size;
    info->links = (DWORD)st.st_nlink;
    info->index_hi = (DWORD)((uint64_t)st.st_ino >> 32);
    info->index_lo = (DWORD)st.st_ino;
    return TRUE;
}

/* FILE_INFO_BY_HANDLE_CLASS: 0 FileBasicInfo, 1 FileStandardInfo */
API(kernel32, BOOL, GetFileInformationByHandleEx, (HANDLE h, DWORD cls, void *buf, DWORD size))
{
    int fd = aw_handle_fd(h);
    struct stat st;
    if (fd < 0)
        return fail(ERR_INVALID_HANDLE), FALSE;
    if (fstat(fd, &st) != 0)
        return fail_errno(), FALSE;
    if (cls == 0) {
        if (size < 40)
            return fail(ERR_BAD_LENGTH), FALSE;
        uint64_t *q = buf;
        q[0] = ts_to_ft64(st.st_ctim);
        q[1] = ts_to_ft64(st.st_atim);
        q[2] = ts_to_ft64(st.st_mtim);
        q[3] = ts_to_ft64(st.st_ctim);
        *(DWORD *)((char *)buf + 32) = attrs_of(&st, NULL);
        return TRUE;
    }
    if (cls == 1) {
        if (size < 24)
            return fail(ERR_BAD_LENGTH), FALSE;
        uint64_t *q = buf;
        q[0] = (uint64_t)((st.st_blocks * 512 + 4095) / 4096 * 4096); /* AllocationSize */
        q[1] = (uint64_t)st.st_size;                                  /* EndOfFile */
        *(DWORD *)((char *)buf + 16) = (DWORD)st.st_nlink;
        *((char *)buf + 20) = 0;                                      /* DeletePending */
        *((char *)buf + 21) = S_ISDIR(st.st_mode) ? 1 : 0;            /* Directory */
        return TRUE;
    }
    return fail(ERR_INVALID_PARAMETER), FALSE;
}

API(kernel32, BOOL, SetFileInformationByHandle, (HANDLE h, DWORD cls, void *buf, DWORD size))
{
    (void)size;
    aw_object *o = aw_handle_object(h);
    if (!o || aw_obj_fd(o) < 0)
        return fail(ERR_INVALID_HANDLE), FALSE;
    if (cls == 0)                 /* FileBasicInfo: timestamps and attributes are not changed here */
        return TRUE;
    if (cls == 4) {               /* FileDispositionInfo: DeleteFile flag, applied on close */
        *aw_obj_delete_on_close(o) = *(BYTE *)buf ? 1 : 0;
        return TRUE;
    }
    return fail(ERR_CALL_NOT_IMPLEMENTED), FALSE;
}

API(kernel32, BOOL, SetFileTime, (HANDLE h, const FILETIME *created, const FILETIME *access, const FILETIME *write))
{
    (void)created;
    int fd = aw_handle_fd(h);
    if (fd < 0)
        return fail(ERR_INVALID_HANDLE), FALSE;
    struct timespec ts[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
    if (access)
        ts[0] = ft_to_ts(((uint64_t)access->dwHighDateTime << 32) | access->dwLowDateTime);
    if (write)
        ts[1] = ft_to_ts(((uint64_t)write->dwHighDateTime << 32) | write->dwLowDateTime);
    if (futimens(fd, ts) != 0)
        return fail_errno(), FALSE;
    return TRUE;
}

/* ---- file mappings -------------------------------------------------------- */
/* Live views: base address and length, so UnmapViewOfFile can release them. */
#define MAX_VIEWS 256
static struct { void *addr; size_t len; } views[MAX_VIEWS];
static pthread_mutex_t views_lock = PTHREAD_MUTEX_INITIALIZER;

API(kernel32, HANDLE, CreateFileMappingA, (HANDLE file, void *sa, DWORD protect, DWORD hi, DWORD lo, const char *name))
{
    (void)sa;
    (void)protect;
    (void)name;
    uint64_t size = ((uint64_t)hi << 32) | lo;
    int fd;
    if (file == INVALID_HANDLE_VALUE) {
        fd = memfd_create("anywin-mapping", MFD_CLOEXEC);
        if (fd < 0)
            return fail_errno(), NULL;
        if (!size || ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            return fail(ERR_INVALID_PARAMETER), NULL;
        }
    } else {
        int src = aw_handle_fd(file);
        if (src < 0)
            return fail(ERR_INVALID_HANDLE), NULL;
        fd = fcntl(src, F_DUPFD_CLOEXEC, 0);
        if (fd < 0)
            return fail_errno(), NULL;
        if (!size) {
            struct stat st;
            fstat(fd, &st);
            size = (uint64_t)st.st_size;
        }
    }
    aw_object *o = aw_object_new(H_MAPPING);
    aw_obj_set_fd(o, fd);
    *aw_obj_ext(o) = (void *)(uintptr_t)size; /* size kept in the pointer slot, nothing to free */
    HANDLE h = aw_handle_alloc(H_MAPPING, o);
    if (!h) {
        close(fd);
        free(o);
        return fail(8), NULL;
    }
    aw_set_last_error(0);
    return h;
}

API(kernel32, void *, MapViewOfFile, (HANDLE h, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T bytes))
{
    aw_object *o = aw_handle_get(h, H_MAPPING);
    if (!o)
        return fail(ERR_INVALID_HANDLE), NULL;
    uint64_t total = (uint64_t)(uintptr_t)*aw_obj_ext(o);
    uint64_t off = ((uint64_t)off_hi << 32) | off_lo;
    if (off > total)
        return fail(ERR_INVALID_PARAMETER), NULL;
    size_t len = bytes ? bytes : (size_t)(total - off);
    if (!len)
        return fail(ERR_INVALID_PARAMETER), NULL;
    /* mmap needs a page-aligned offset: map from the page below and return the adjusted pointer */
    uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t base = off - off % pg;
    size_t delta = (size_t)(off - base);
    int prot = PROT_READ | ((access & FILE_MAP_WRITE) ? PROT_WRITE : 0);
    int flags = (access & FILE_MAP_COPY) ? MAP_PRIVATE : MAP_SHARED;
    void *p = mmap(NULL, len + delta, prot, flags, aw_obj_fd(o), (off_t)base);
    if (p == MAP_FAILED)
        return fail_errno(), NULL;
    pthread_mutex_lock(&views_lock);
    for (int i = 0; i < MAX_VIEWS; i++)
        if (!views[i].addr) {
            views[i].addr = p;
            views[i].len = len + delta;
            break;
        }
    pthread_mutex_unlock(&views_lock);
    aw_set_last_error(0);
    return (char *)p + delta;
}

API(kernel32, BOOL, UnmapViewOfFile, (void *addr))
{
    if (!addr)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
    void *base = (void *)((uintptr_t)addr - ((uintptr_t)addr % pg));
    size_t len = 0;
    pthread_mutex_lock(&views_lock);
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i].addr == base) {
            len = views[i].len;
            views[i].addr = NULL;
            break;
        }
    pthread_mutex_unlock(&views_lock);
    if (!len)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    if (munmap(base, len) != 0)
        return fail_errno(), FALSE;
    return TRUE;
}
