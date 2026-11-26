/* kernel32: anonymous pipes and named pipes.
 *
 * Anonymous pipes are pipe2() descriptors wrapped in H_FILE handles, so ReadFile, WriteFile and
 * CloseHandle work on them unchanged. A named pipe "\\.\pipe\NAME" is a UNIX-domain stream socket
 * at $TMPDIR-independent path /tmp/anywin-pipes-UID/name (case folded, separators mapped to '_').
 * One listening socket exists per name in the process; each CreateNamedPipe instance is an H_FILE
 * that ConnectNamedPipe fills with the next accepted client. Clients open the name with CreateFile,
 * which connects to the same socket. Other processes see the same names through the directory. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ERR_FILE_NOT_FOUND 2
#define ERR_INVALID_HANDLE 6
#define ERR_INVALID_FUNCTION 1
#define ERR_INVALID_PARAMETER 87
#define ERR_INSUFFICIENT_BUFFER 122
#define ERR_INVALID_NAME 123
#define ERR_SEM_TIMEOUT 121
#define ERR_PIPE_CONNECTED 535
#define PIPE_UNLIMITED_INSTANCES 255
#define NMPWAIT_USE_DEFAULT_WAIT 0
#define NMPWAIT_FOREVER 0xFFFFFFFFu
#define PREFIX "\\\\.\\pipe\\"
#define PREFIX_LEN 9

BOOL WINAPI __aw_kernel32_CloseHandle(HANDLE h);

static DWORD fail(DWORD e)
{
    aw_set_last_error(e);
    return 0;
}

static DWORD fail_errno(void)
{
    return fail(aw_errno_to_win(errno));
}

int aw_pipe_name(const char *win)
{
    return win && !strncasecmp(win, PREFIX, PREFIX_LEN) && win[PREFIX_LEN];
}

/* UNIX socket path for a pipe name; NULL when the name cannot be a socket path */
static char *pipe_path(const char *win)
{
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/anywin-pipes-%u", (unsigned)getuid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return NULL;
    char path[512];
    size_t n = 0;
    n = (size_t)snprintf(path, sizeof path, "%s/", dir);
    for (const char *p = win + PREFIX_LEN; *p && n + 1 < sizeof path; p++) {
        char c = *p;
        if (c == '\\' || c == '/')
            c = '_';
        path[n++] = (char)tolower((unsigned char)c);
        path[n] = 0;
    }
    if (strlen(path) >= 108) /* sun_path size */
        return NULL;
    return strdup(path);
}

static HANDLE new_file_handle(int fd, int inherit)
{
    aw_object *o = aw_object_new(H_FILE);
    aw_obj_set_fd(o, fd);
    *aw_obj_inherit(o) = inherit;
    HANDLE h = aw_handle_alloc(H_FILE, o);
    if (!h) {
        close(fd);
        free(o);
        fail(ERR_INVALID_HANDLE);
    }
    return h;
}

/* ---- anonymous pipes ----------------------------------------------------------- */
/* SECURITY_ATTRIBUTES, x64: nLength, lpSecurityDescriptor, bInheritHandle at +16 */
API(kernel32, BOOL, CreatePipe, (HANDLE * rd, HANDLE * wr, void *sa, DWORD size))
{
    (void)size;
    if (!rd || !wr)
        return fail(ERR_INVALID_PARAMETER), FALSE;
    int inherit = sa ? *(const int *)((const char *)sa + 16) != 0 : 0;
    int fds[2];
    if (pipe2(fds, inherit ? 0 : O_CLOEXEC) != 0)
        return fail_errno(), FALSE;
    /* new_file_handle closes its fd when it fails, so only the untouched end is closed here */
    HANDLE r = new_file_handle(fds[0], inherit);
    if (!r) {
        close(fds[1]);
        return FALSE;
    }
    HANDLE w = new_file_handle(fds[1], inherit);
    if (!w) {
        __aw_kernel32_CloseHandle(r);
        return FALSE;
    }
    *rd = r;
    *wr = w;
    return TRUE;
}

/* ---- named pipes ------------------------------------------------------------------- */
struct listener {
    char *path;
    int fd;
    struct listener *next;
};

static struct listener *listeners;
static pthread_mutex_t pipe_lock = PTHREAD_MUTEX_INITIALIZER;

/* the shared listening socket for a path, created on first use; NULL on failure */
static struct listener *listener_locked(const char *path)
{
    for (struct listener *l = listeners; l; l = l->next)
        if (!strcmp(l->path, path))
            return l;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    unlink(path); /* a socket left behind by an earlier run */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return NULL;
    }
    struct listener *l = calloc(1, sizeof *l);
    if (!l) {
        close(fd);
        return NULL;
    }
    l->path = strdup(path);
    l->fd = fd;
    l->next = listeners;
    listeners = l;
    return l;
}

/* An instance is an H_FILE with fd -1 until ConnectNamedPipe; its ext keeps the listener. */
API(kernel32, HANDLE, CreateNamedPipeA, (const char *name, DWORD open_mode, DWORD pipe_mode, DWORD max_inst,
                                         DWORD out_size, DWORD in_size, DWORD timeout, void *sa))
{
    (void)open_mode;
    (void)pipe_mode;
    (void)out_size;
    (void)in_size;
    (void)timeout;
    (void)sa;
    (void)max_inst;
    if (!aw_pipe_name(name)) {
        fail(ERR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    char *path = pipe_path(name);
    if (!path) {
        fail(ERR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    pthread_mutex_lock(&pipe_lock);
    struct listener *l = listener_locked(path);
    pthread_mutex_unlock(&pipe_lock);
    if (!l) {
        int e = errno;
        free(path);
        fail(aw_errno_to_win(e));
        return INVALID_HANDLE_VALUE;
    }
    free(path);
    aw_object *o = aw_object_new(H_FILE);
    *aw_obj_ext(o) = l;
    HANDLE h = aw_handle_alloc(H_FILE, o);
    if (!h) {
        free(o);
        fail(ERR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

static struct listener *instance_listener(HANDLE h)
{
    aw_object *o = aw_handle_object(h);
    if (!o || aw_obj_type(o) != H_FILE)
        return NULL;
    return *aw_obj_ext(o);
}

API(kernel32, BOOL, ConnectNamedPipe, (HANDLE h, void *ov))
{
    (void)ov;
    aw_object *o = aw_handle_object(h);
    struct listener *l = instance_listener(h);
    if (!o || !l)
        return fail(l ? ERR_INVALID_HANDLE : ERR_INVALID_FUNCTION), FALSE;
    if (aw_obj_fd(o) >= 0)
        return fail(ERR_PIPE_CONNECTED), FALSE;
    int cfd;
    do
        cfd = accept4(l->fd, NULL, NULL, SOCK_CLOEXEC);
    while (cfd < 0 && errno == EINTR);
    if (cfd < 0)
        return fail_errno(), FALSE;
    aw_obj_set_fd(o, cfd);
    return TRUE;
}

API(kernel32, BOOL, DisconnectNamedPipe, (HANDLE h))
{
    aw_object *o = aw_handle_object(h);
    if (!o || !instance_listener(h))
        return fail(ERR_INVALID_HANDLE), FALSE;
    if (aw_obj_fd(o) >= 0) {
        close(aw_obj_fd(o));
        aw_obj_set_fd(o, -1);
    }
    return TRUE;
}

API(kernel32, BOOL, WaitNamedPipeA, (const char *name, DWORD timeout))
{
    if (!aw_pipe_name(name))
        return fail(ERR_INVALID_NAME), FALSE;
    char *path = pipe_path(name);
    if (!path)
        return fail(ERR_INVALID_NAME), FALSE;
    DWORD ms = timeout == NMPWAIT_USE_DEFAULT_WAIT ? 50 : timeout;
    DWORD waited = 0;
    for (;;) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            free(path);
            return TRUE;
        }
        if (timeout != NMPWAIT_FOREVER && waited >= ms) {
            free(path);
            return fail(ERR_SEM_TIMEOUT), FALSE;
        }
        struct timespec ts = {0, 10 * 1000000L};
        nanosleep(&ts, NULL);
        waited += 10;
    }
}

/* client side of CreateFile on a pipe name; called by file.c's CreateFileA/W */
HANDLE aw_pipe_open(const char *win, DWORD access, DWORD disp)
{
    (void)access;
    (void)disp;
    char *path = pipe_path(win);
    if (!path)
        return fail(ERR_INVALID_NAME), INVALID_HANDLE_VALUE;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    free(path);
    if (fd < 0)
        return fail_errno(), INVALID_HANDLE_VALUE;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        /* no server listening, or only a stale socket file */
        close(fd);
        return fail(ERR_FILE_NOT_FOUND), INVALID_HANDLE_VALUE;
    }
    HANDLE h = new_file_handle(fd, 0);
    return h ? h : INVALID_HANDLE_VALUE;
}
