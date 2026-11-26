/* anywin runtime - shell32 launching, file operations and known folders.
 *
 * ShellExecute opens URLs and documents with xdg-open and runs executables directly; the child is
 * detached (double fork) and its exec failure is reported back through a close-on-exec pipe.
 * SHFileOperationW deletes into the freedesktop trash (gio trash, else a plain move into
 * ~/.local/share/Trash/files) and renames or moves. Known folders map to the environment folders
 * the runtime already defines (APPDATA, LOCALAPPDATA, USERPROFILE) and to the C: drive tree. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SE_ERR_FNF 2
#define SE_ERR_ACCESSDENIED 5
#define SE_ERR_NOASSOC 31
#define SE_SUCCESS 42
#define KF_FLAG_CREATE 0x00008000u
#define FO_MOVE 1
#define FO_COPY 2
#define FO_DELETE 3
#define FO_RENAME 4
#define E_FAIL ((HRESULT)0x80004005)

/* fork, exec and report: 0 when the program was started, else the errno from the exec attempt */
static int spawn_detached(char *const argv[], const char *cwd)
{
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0)
        return errno;
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(pfd[0]);
        close(pfd[1]);
        return e;
    }
    if (pid == 0) {
        close(pfd[0]);
        pid_t gc = fork();
        if (gc == 0) {
            setsid();
            if (cwd && chdir(cwd) != 0) {
                int e = errno;
                (void)!write(pfd[1], &e, sizeof e);
                _exit(127);
            }
            execvp(argv[0], argv);
            int e = errno;
            (void)!write(pfd[1], &e, sizeof e);
            _exit(127);
        }
        _exit(0);
    }
    close(pfd[1]);
    waitpid(pid, NULL, 0);
    int err = 0;
    ssize_t r;
    do
        r = read(pfd[0], &err, sizeof err);
    while (r < 0 && errno == EINTR);
    close(pfd[0]);
    return r == (ssize_t)sizeof err ? err : 0;
}

static int shell_errno(int e)
{
    if (e == ENOENT || e == ENOTDIR)
        return SE_ERR_FNF;
    if (e == EACCES || e == EPERM)
        return SE_ERR_ACCESSDENIED;
    return SE_ERR_NOASSOC;
}

/* Runs a verb on a file, URL or directory. Returns SE_SUCCESS or an SE_ERR_* code. */
static int run_shell(const char *file, const char *params, const char *dir)
{
    if (!file || !*file)
        return SE_ERR_FNF;
    char *cwd = dir && *dir ? aw_unix_path(dir) : NULL;
    int url = strstr(file, "://") != NULL || !strncasecmp(file, "mailto:", 7);
    char *target = url ? strdup(file) : aw_unix_path(file);
    int result = SE_SUCCESS;
    struct stat st;
    if (!url && stat(target, &st) != 0) {
        result = SE_ERR_FNF;
    } else {
        size_t l = strlen(file);
        int is_exe = !url && !(S_ISDIR(st.st_mode)) && l > 4 && !strcasecmp(file + l - 4, ".exe");
        if (is_exe || (!url && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR) && !strchr(file, '.'))) {
            /* executable: its arguments are split on blanks */
            char *pp = params ? strdup(params) : strdup("");
            int argc = 1;
            char **argv = calloc(64, sizeof *argv);
            argv[0] = target;
            for (char *tok = strtok(pp, " \t"); tok && argc < 62; tok = strtok(NULL, " \t"))
                argv[argc++] = tok;
            argv[argc] = NULL;
            int e = spawn_detached(argv, cwd);
            result = e ? shell_errno(e) : SE_SUCCESS;
            free(argv);
            free(pp);
        } else {
            char *argv[] = {"xdg-open", target, NULL};
            int e = spawn_detached(argv, NULL);
            result = e ? shell_errno(e) : SE_SUCCESS;
        }
    }
    free(target);
    free(cwd);
    return result;
}

API(shell32, HINSTANCE, ShellExecuteW, (HWND hwnd, const WCHAR *verb, const WCHAR *file, const WCHAR *params,
                                        const WCHAR *dir, INT show))
{
    (void)hwnd;
    (void)verb;
    (void)show;
    char *f = aw_w2u(file, -1), *p = aw_w2u(params, -1), *d = aw_w2u(dir, -1);
    int r = run_shell(f, p, d);
    free(f);
    free(p);
    free(d);
    return (HINSTANCE)(intptr_t)r;
}

/* SHELLEXECUTEINFOW, x64 layout */
typedef struct {
    DWORD cbSize, fMask;
    HWND hwnd;
    const WCHAR *lpVerb, *lpFile, *lpParameters, *lpDirectory;
    INT nShow;
    HINSTANCE hInstApp;
    void *lpIDList;
    const WCHAR *lpClass;
    HKEY hkeyClass;
    DWORD dwHotKey;
    HANDLE hIcon;
    HANDLE hProcess;
} SHELLEXECUTEINFOW_L;

API(shell32, BOOL, ShellExecuteExW, (SHELLEXECUTEINFOW_L * info))
{
    if (!info || info->cbSize < sizeof *info) {
        aw_set_last_error(87);
        return FALSE;
    }
    char *f = aw_w2u(info->lpFile, -1), *p = aw_w2u(info->lpParameters, -1), *d = aw_w2u(info->lpDirectory, -1);
    int r = run_shell(f, p, d);
    free(f);
    free(p);
    free(d);
    info->hProcess = NULL;
    if (r == SE_SUCCESS)
        return TRUE;
    aw_set_last_error(r == SE_ERR_FNF ? 2 : r == SE_ERR_ACCESSDENIED ? 5 : 31);
    return FALSE;
}

/* ---- SHFileOperationW ----------------------------------------------------- */
typedef struct {
    HWND hwnd;
    UINT wFunc;
    const WCHAR *pFrom, *pTo;
    WORD fFlags;
    BOOL fAnyOperationsAborted;
    void *hNameMappings;
    const WCHAR *lpszProgressTitle;
} SHFILEOPSTRUCTW_L;

static int run_wait(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int mkdir_p(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    return mkdir(buf, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* moves a path into the trash: gio when it is there, else a plain move into Trash/files */
static int trash_path(const char *u)
{
    char *gio[] = {"gio", "trash", "--", (char *)u, NULL};
    if (run_wait(gio) == 0)
        return 0;
    const char *home = getenv("HOME");
    if (!home || !*home || strlen(home) > 3000)
        return -1;
    char dir[4096], dst[4096];
    snprintf(dir, sizeof dir, "%s/.local/share/Trash/files", home);
    mkdir_p(dir);
    const char *base = strrchr(u, '/');
    base = base ? base + 1 : u;
    snprintf(dst, sizeof dst, "%s/%s", dir, base);
    for (int i = 1; access(dst, F_OK) == 0 && i < 1000; i++)
        snprintf(dst, sizeof dst, "%s/%s.%d", dir, base, i);
    return rename(u, dst);
}

API(shell32, int, SHFileOperationW, (SHFILEOPSTRUCTW_L * op))
{
    if (!op)
        return 0x10000 | 0x7C; /* DE_INVALIDFILES */
    op->fAnyOperationsAborted = FALSE;
    char *from = NULL, *to = NULL;
    /* pFrom: NUL-separated list, ended by an empty entry */
    int rc = 0;
    if (op->wFunc == FO_DELETE) {
        for (const WCHAR *p = op->pFrom; p && *p;) {
            size_t n = aw_wcslen(p);
            char *u = aw_w2u(p, -1);
            char *unix_path = aw_unix_path(u);
            struct stat st;
            if (lstat(unix_path, &st) != 0)
                rc = 2;
            else if (trash_path(unix_path) != 0)
                rc = 5;
            free(unix_path);
            free(u);
            p += n + 1;
        }
    } else if (op->wFunc == FO_MOVE || op->wFunc == FO_RENAME) {
        from = aw_w2u(op->pFrom, -1);
        to = aw_w2u(op->pTo, -1);
        char *uf = aw_unix_path(from ? from : ""), *ut = aw_unix_path(to ? to : "");
        struct stat st;
        if (stat(ut, &st) == 0 && S_ISDIR(st.st_mode) && op->wFunc == FO_MOVE) {
            const char *base = strrchr(uf, '/');
            char dst[4096];
            snprintf(dst, sizeof dst, "%s/%s", ut, base ? base + 1 : uf);
            rc = rename(uf, dst) == 0 ? 0 : 5;
        } else {
            rc = rename(uf, ut) == 0 ? 0 : 5;
        }
        free(uf);
        free(ut);
    } else {
        rc = 0x78; /* ERROR_CALL_NOT_IMPLEMENTED: copy is not supported */
    }
    free(from);
    free(to);
    return rc;
}

/* ---- known folders ------------------------------------------------------- */
typedef struct {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} GUID_L;

static const struct {
    GUID_L id;
    const char *name;   /* folder under the base */
    const char *base;   /* "APPDATA", "LOCALAPPDATA", "USERPROFILE", "C:" or NULL */
    const char *sub;    /* path under the base, may be NULL */
    int always_create;  /* created even without KF_FLAG_CREATE */
} folders[] = {
    {{0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}}, "RoamingAppData", "APPDATA", NULL, 0},
    {{0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}}, "LocalAppData", "LOCALAPPDATA", NULL, 0},
    {{0x5E6C858F, 0x0E22, 0x4760, {0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73}}, "Profile", "USERPROFILE", NULL, 0},
    {{0xB4BFCC3A, 0xDB2C, 0x424C, {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}}, "Desktop", "USERPROFILE", "Desktop", 0},
    {{0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}}, "Documents", "USERPROFILE", "Documents", 0},
    {{0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}}, "Downloads", "USERPROFILE", "Downloads", 0},
    {{0xA77F5D77, 0x2E2B, 0x44C3, {0xA6, 0xA2, 0xAB, 0xA6, 0x01, 0x05, 0x4A, 0x51}}, "Programs", "APPDATA",
     "Microsoft\\Windows\\Start Menu\\Programs", 1},
    {{0x625B53C3, 0xAB48, 0x4EC1, {0xBA, 0x1F, 0xA1, 0xEF, 0x41, 0x46, 0xFC, 0x19}}, "StartMenu", "APPDATA",
     "Microsoft\\Windows\\Start Menu", 1},
    {{0xB97D20BB, 0xF46A, 0x4C97, {0xBA, 0x10, 0x5E, 0x36, 0x08, 0x43, 0x08, 0x54}}, "Startup", "APPDATA",
     "Microsoft\\Windows\\Start Menu\\Programs\\Startup", 1},
    {{0xF38BF404, 0x1D43, 0x42F2, {0x93, 0x05, 0x67, 0xDE, 0x0B, 0x28, 0xFC, 0x23}}, "Windows", "C:", "Windows", 0},
    {{0x1AC14E77, 0x02E7, 0x4E5D, {0xB7, 0x44, 0x2E, 0xB1, 0xAE, 0x51, 0x98, 0xB7}}, "System", "C:", "Windows\\System32", 0},
    {{0x905E63B6, 0xC1BF, 0x494E, {0xB2, 0x9C, 0x65, 0xB7, 0x32, 0xD3, 0xD2, 0x1A}}, "ProgramFiles", "C:",
     "Program Files", 0},
    {{0x7C5A40EF, 0xA0FB, 0x4BFC, {0x87, 0x4A, 0xC0, 0xF2, 0xE0, 0xB9, 0xFA, 0x8E}}, "ProgramFilesX86", "C:",
     "Program Files (x86)", 0},
    {{0x62AB5D82, 0xFDC1, 0x4DC3, {0xA9, 0xDD, 0x07, 0x0D, 0x1D, 0x49, 0x5D, 0x97}}, "ProgramData", "C:",
     "ProgramData", 0},
};

/* Windows path of a known folder into buf; 0 when it is not known */
static int folder_path(const GUID_L *id, char *buf, size_t cap, int *always)
{
    for (size_t i = 0; i < sizeof folders / sizeof *folders; i++) {
        if (memcmp(id, &folders[i].id, sizeof *id))
            continue;
        const char *base = NULL;
        if (!strcmp(folders[i].base, "C:"))
            base = "C:";
        else
            base = getenv(folders[i].base);
        if (!base || !*base)
            return 0;
        size_t bl = strlen(base);
        if (folders[i].sub)
            snprintf(buf, cap, "%s%s%s", base, bl && base[bl - 1] == '\\' ? "" : "\\", folders[i].sub);
        else
            snprintf(buf, cap, "%s", base);
        *always = folders[i].always_create;
        return 1;
    }
    return 0;
}

API(shell32, HRESULT, SHGetKnownFolderPath, (const GUID_L *id, DWORD flags, HANDLE token, WCHAR **out))
{
    (void)token;
    if (!out)
        return (HRESULT)0x80070057; /* E_INVALIDARG */
    *out = NULL;
    char win[4096];
    int always = 0;
    if (!folder_path(id, win, sizeof win, &always))
        return E_FAIL;
    if ((flags & KF_FLAG_CREATE) || always) {
        char *u = aw_unix_path(win);
        mkdir_p(u);
        free(u);
    }
    WCHAR *w = aw_u2w(win, -1);
    *out = w; /* CoTaskMemFree releases it */
    return 0;
}
