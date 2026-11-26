/* kernel32: child processes and process handles.
 *
 * CreateProcessA forks and execs the program directly (no Windows loader). The child's standard
 * handles come from STARTUPINFO when STARTF_USESTDHANDLES is set, else are inherited. An exec
 * failure comes back through a close-on-exec pipe, so CreateProcess reports it like Windows does.
 *
 * A process object (H_PROCESS) holds a refcounted record with the pid. The process handle and the
 * primary-thread handle share one record; whichever handle polls reaps the child (under the record
 * lock) and both report the same exit code. OpenProcess on a pid that is not our child polls with
 * kill(0) and reports 0 when the process has gone. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ERR_FILE_NOT_FOUND 2
#define ERR_ACCESS_DENIED 5
#define ERR_INVALID_HANDLE 6
#define ERR_INVALID_PARAMETER 87
#define CREATE_UNICODE_ENVIRONMENT 0x00000400u
#define DETACHED_PROCESS 0x00000008u
#define STARTF_USESTDHANDLES 0x00000100u
#define STILL_ACTIVE 259

BOOL WINAPI __aw_kernel32_CloseHandle(HANDLE h);
HANDLE aw_current_thread_handle(void);

struct child {
    pid_t pid;
    int refs;
    int reaped;
    int is_child;
    DWORD code;
};

static pthread_mutex_t child_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec started;

__attribute__((constructor)) static void note_start(void) { clock_gettime(CLOCK_REALTIME, &started); }

static DWORD fail(DWORD e)
{
    aw_set_last_error(e);
    return 0;
}

static DWORD map_errno(int e)
{
    if (e == ENOENT || e == ENOTDIR)
        return ERR_FILE_NOT_FOUND;
    if (e == EACCES || e == EPERM)
        return ERR_ACCESS_DENIED;
    return aw_errno_to_win(e);
}

/* ---- records and polling ------------------------------------------------------ */
static void reap_locked(struct child *c)
{
    if (c->reaped)
        return;
    int st = 0;
    pid_t r = waitpid(c->pid, &st, WNOHANG);
    if (r == c->pid) {
        c->reaped = 1;
        c->code = WIFEXITED(st) ? (DWORD)WEXITSTATUS(st) : WIFSIGNALED(st) ? (DWORD)(128 + WTERMSIG(st)) : 1;
    } else if (r < 0 && errno == ECHILD) {
        /* not our child: it is finished once the pid no longer exists */
        if (kill(c->pid, 0) != 0 && errno == ESRCH) {
            c->reaped = 1;
            c->code = 0;
        }
    }
}

/* sync.c calls this for H_PROCESS objects that are not finished; nonzero when finished */
int aw_process_poll(aw_object *o)
{
    void **ext = aw_obj_ext(o);
    struct child *c = *ext;
    if (aw_obj_finished(o))
        return 1;
    if (!c)
        return 0;
    pthread_mutex_lock(&child_lock);
    reap_locked(c);
    if (c->reaped)
        aw_obj_set_finished(o, c->code);
    pthread_mutex_unlock(&child_lock);
    return aw_obj_finished(o);
}

/* CloseHandle of an H_PROCESS object: drops the record reference */
void aw_process_close(aw_object *o)
{
    void **ext = aw_obj_ext(o);
    struct child *c = *ext;
    *ext = NULL;
    if (!c)
        return;
    pthread_mutex_lock(&child_lock);
    int last = --c->refs == 0;
    pthread_mutex_unlock(&child_lock);
    if (last)
        free(c);
}

static HANDLE make_handle(struct child *c, int shared)
{
    aw_object *o = aw_object_new(H_PROCESS);
    *aw_obj_ext(o) = c;
    if (shared) {
        pthread_mutex_lock(&child_lock);
        c->refs++;
        pthread_mutex_unlock(&child_lock);
    }
    HANDLE h = aw_handle_alloc(H_PROCESS, o);
    if (!h) {
        *aw_obj_ext(o) = NULL;
        free(o);
        return NULL;
    }
    return h;
}

static struct child *new_record(pid_t pid, int is_child)
{
    struct child *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->pid = pid;
    c->refs = 1;
    c->is_child = is_child;
    return c;
}

/* ---- command line and environment --------------------------------------------- */
/* Splits a Windows command line into argv using the MSVCRT rules (quotes, 2n backslashes). */
static char **split_command(const char *cmd, int *argc_out)
{
    size_t cap = 16, argc = 0, len = strlen(cmd);
    char **argv = calloc(cap + 1, sizeof *argv);
    char *buf = malloc(len + 1);
    if (!argv || !buf)
        return NULL;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (argc + 1 >= cap) {
            cap *= 2;
            argv = realloc(argv, (cap + 1) * sizeof *argv);
        }
        char *out = buf + (p - cmd);
        char *start = out;
        int inq = 0;
        while (*p && (inq || (*p != ' ' && *p != '\t'))) {
            if (*p == '\\') {
                int n = 0;
                while (*p == '\\')
                    n++, p++;
                if (*p == '"') {
                    for (int i = 0; i < n / 2; i++)
                        *out++ = '\\';
                    if (n % 2) {
                        *out++ = '"';
                        p++;
                    } else {
                        inq = !inq;
                        p++;
                    }
                } else {
                    for (int i = 0; i < n; i++)
                        *out++ = '\\';
                }
            } else if (*p == '"') {
                inq = !inq;
                p++;
            } else {
                *out++ = *p++;
            }
        }
        *out = 0;
        argv[argc++] = strdup(start);
    }
    free(buf);
    argv[argc] = NULL;
    *argc_out = (int)argc;
    return argv;
}

static char **env_from_block(const void *block, int wide)
{
    size_t n = 0, cap = 64;
    char **envp = calloc(cap, sizeof *envp);
    if (wide) {
        const WCHAR *w = block;
        while (*w) {
            size_t l = aw_wcslen(w);
            if (n + 1 >= cap)
                envp = realloc(envp, (cap *= 2) * sizeof *envp);
            envp[n++] = aw_w2u(w, -1);
            w += l + 1;
        }
    } else {
        const char *c = block;
        while (*c) {
            size_t l = strlen(c);
            if (n + 1 >= cap)
                envp = realloc(envp, (cap *= 2) * sizeof *envp);
            envp[n++] = strdup(c);
            c += l + 1;
        }
    }
    envp[n] = NULL;
    return envp;
}

static void free_strings(char **v)
{
    if (!v)
        return;
    for (char **p = v; *p; p++)
        free(*p);
    free(v);
}

/* ---- CreateProcess ------------------------------------------------------------------ */
typedef struct {
    DWORD cb;
    LPSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFO_A;

typedef struct {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION_L;

/* Forks, redirects the standard handles, and execs. Returns 0 or a Windows error code. */
static DWORD spawn(const char *app, char *cmdline, const char *cwd, const void *env, int env_wide, DWORD flags,
                   const STARTUPINFO_A *si, pid_t *pid_out)
{
    int argc = 0;
    char **argv = cmdline ? split_command(cmdline, &argc) : NULL;
    if (!app && (!argv || !argv[0])) {
        free_strings(argv);
        return ERR_INVALID_PARAMETER;
    }
    char *prog = app ? aw_unix_path(app) : strdup(argv[0]);
    char *dir = cwd ? aw_unix_path(cwd) : NULL;
    char **envp = env ? env_from_block(env, env_wide) : NULL;
    if (!argv || !argv[0]) {
        /* no command line: argv[0] is the program itself */
        free(argv);
        argv = calloc(2, sizeof *argv);
        argv[0] = strdup(prog);
    }
    int inh[3] = {-1, -1, -1};
    if (si && (si->dwFlags & STARTF_USESTDHANDLES)) {
        HANDLE hs[3] = {si->hStdInput, si->hStdOutput, si->hStdError};
        for (int i = 0; i < 3; i++)
            inh[i] = aw_handle_fd(hs[i]);
    }
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) {
        DWORD e = map_errno(errno);
        free_strings(argv);
        free_strings(envp);
        free(prog);
        free(dir);
        return e;
    }
    fflush(NULL); /* buffered output must not be written twice by the child */
    pid_t pid = fork();
    if (pid < 0) {
        DWORD e = map_errno(errno);
        close(pfd[0]);
        close(pfd[1]);
        free_strings(argv);
        free_strings(envp);
        free(prog);
        free(dir);
        return e;
    }
    if (pid == 0) {
        close(pfd[0]);
        if (flags & DETACHED_PROCESS)
            setsid();
        if (inh[0] >= 0 || inh[1] >= 0 || inh[2] >= 0) {
            /* duplicate every source first: a source may be one of the target fds */
            int src[3];
            for (int i = 0; i < 3; i++)
                src[i] = inh[i] >= 0 ? fcntl(inh[i], F_DUPFD, 10) : -1;
            for (int i = 0; i < 3; i++)
                if (src[i] >= 0)
                    dup2(src[i], i);
            for (int i = 0; i < 3; i++)
                if (src[i] >= 0)
                    close(src[i]);
        }
        if (dir && chdir(dir) != 0) {
            int e = errno;
            (void)!write(pfd[1], &e, sizeof e);
            _exit(127);
        }
        if (envp)
            execve(prog, argv, envp);
        else
            execv(prog, argv);
        int e = errno;
        (void)!write(pfd[1], &e, sizeof e);
        _exit(127);
    }
    close(pfd[1]);
    int err = 0;
    ssize_t r;
    do
        r = read(pfd[0], &err, sizeof err);
    while (r < 0 && errno == EINTR);
    close(pfd[0]);
    free_strings(argv);
    free_strings(envp);
    free(prog);
    free(dir);
    if (r == (ssize_t)sizeof err) {
        int st;
        waitpid(pid, &st, 0); /* the child exited after reporting the failure */
        return map_errno(err);
    }
    *pid_out = pid;
    return 0;
}

API(kernel32, BOOL, CreateProcessA, (const char *app, char *cmdline, void *pattr, void *tattr, BOOL inherit,
                                     DWORD flags, void *env, const char *cwd, STARTUPINFO_A *si,
                                     PROCESS_INFORMATION_L *pi))
{
    (void)pattr;
    (void)tattr;
    (void)inherit;
    if (!pi || !si || si->cb < sizeof(STARTUPINFO_A) - 16)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    if (!app && !cmdline)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    pid_t pid = 0;
    DWORD e = spawn(app, cmdline, cwd, env, (flags & CREATE_UNICODE_ENVIRONMENT) ? 1 : 0, flags, si, &pid);
    if (e)
        return fail(e), FALSE;
    struct child *c = new_record(pid, 1);
    HANDLE hp = c ? make_handle(c, 0) : NULL;
    HANDLE ht = hp ? make_handle(c, 1) : NULL;
    if (!ht) {
        if (hp)
            __aw_kernel32_CloseHandle(hp);
        else if (c)
            free(c);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return fail(8), FALSE;
    }
    pi->hProcess = hp;
    pi->hThread = ht;
    pi->dwProcessId = (DWORD)pid;
    pi->dwThreadId = (DWORD)pid; /* the runtime has no separate thread id for the primary thread */
    aw_set_last_error(0);
    return TRUE;
}

API(kernel32, BOOL, GetExitCodeProcess, (HANDLE h, DWORD *code))
{
    if (h == (HANDLE)-1) { /* the current process */
        if (code)
            *code = STILL_ACTIVE;
        return TRUE;
    }
    aw_object *o = aw_handle_get(h, H_PROCESS);
    if (!o)
        return fail(ERR_INVALID_HANDLE), FALSE;
    aw_process_poll(o);
    if (code)
        *code = aw_obj_finished(o) ? aw_obj_exit_code(o) : STILL_ACTIVE;
    return TRUE;
}

API(kernel32, HANDLE, OpenProcess, (DWORD access, BOOL inherit, DWORD pid))
{
    (void)access;
    (void)inherit;
    if (pid == 0)
        return fail(ERR_INVALID_PARAMETER), NULL;
    if (kill((pid_t)pid, 0) != 0 && errno == ESRCH)
        return fail(ERR_INVALID_PARAMETER), NULL;
    struct child *c = new_record((pid_t)pid, 0);
    if (!c)
        return fail(8), NULL;
    HANDLE h = make_handle(c, 0);
    if (!h) {
        free(c);
        return fail(8), NULL;
    }
    return h;
}

/* ---- times ---------------------------------------------------------------------------- */
static void put_ft(FILETIME *ft, uint64_t v)
{
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static uint64_t tv_100ns(struct timeval tv) { return (uint64_t)tv.tv_sec * 10000000ull + (uint64_t)tv.tv_usec * 10u; }

static void fill_times(int who, FILETIME *creation, FILETIME *exit, FILETIME *kernel, FILETIME *user, int finished)
{
    struct rusage ru;
    if (getrusage(who, &ru) != 0)
        memset(&ru, 0, sizeof ru);
    if (creation) {
        uint64_t start = ((uint64_t)started.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)started.tv_nsec / 100u;
        put_ft(creation, start);
    }
    if (exit)
        put_ft(exit, finished ? aw_now_filetime() : 0);
    if (kernel)
        put_ft(kernel, tv_100ns(ru.ru_stime));
    if (user)
        put_ft(user, tv_100ns(ru.ru_utime));
}

API(kernel32, BOOL, GetProcessTimes, (HANDLE h, FILETIME *creation, FILETIME *exit, FILETIME *kernel, FILETIME *user))
{
    if (h == (HANDLE)-1) {
        fill_times(RUSAGE_SELF, creation, exit, kernel, user, 0);
        return TRUE;
    }
    aw_object *o = aw_handle_get(h, H_PROCESS);
    if (!o)
        return fail(ERR_INVALID_HANDLE), FALSE;
    aw_process_poll(o);
    struct child *c = *aw_obj_ext(o);
    /* the children's usage is cumulative for all children: it is the best figure available */
    fill_times(c && c->is_child ? RUSAGE_CHILDREN : RUSAGE_SELF, creation, exit, kernel, user,
               aw_obj_finished(o));
    return TRUE;
}

API(kernel32, BOOL, GetThreadTimes, (HANDLE h, FILETIME *creation, FILETIME *exit, FILETIME *kernel, FILETIME *user))
{
    /* the calling thread's own usage; other threads are reported with the process total */
    int who = (h == (HANDLE)-2 || h == aw_current_thread_handle()) ? RUSAGE_THREAD : RUSAGE_SELF;
    if (!h)
        return fail(ERR_INVALID_HANDLE), FALSE;
    fill_times(who, creation, exit, kernel, user, 0);
    return TRUE;
}
