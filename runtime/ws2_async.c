/* ws2_async: Winsock asynchronous socket notification.
 *
 * WSAAsyncSelect posts a window message per FD_* event; WSAEventSelect signals an event object
 * that WSAEnumNetworkEvents reports from.  Both are driven by aw_net_pump(), which polls the
 * registered sockets.  The GUI wait (x11.c plat_poll, via plat_add_fd_source) and the
 * MsgWaitForMultipleObjects loop call it, so a socket event wakes the message loop.
 *
 * A socket is watched only while it has an interest that is not already armed, so an idle
 * socket costs nothing (no busy loop on a socket that is readable but not wanted).  Events
 * follow Winsock's one-shot rules:
 *   FD_READ    once until the program calls recv again (also re-posted when data remains)
 *   FD_WRITE   after connect completes, at registration, and after a send returned WSAEWOULDBLOCK
 *   FD_ACCEPT  once per pending connection until accept is called
 *   FD_CONNECT once, with the connect error (if any) in the high word of lParam
 *   FD_CLOSE   once, when the peer closed (no data left) or the connection failed
 * The socket is switched to non-blocking mode while it is registered, as on Windows, and back
 * to blocking mode when the registration is cancelled with lEvent 0. */
#include "aw_api.h"
#include "gui.h"
#include "ws2_int.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WS_FD_READ 0x01
#define WS_FD_WRITE 0x02
#define WS_FD_OOB 0x04
#define WS_FD_ACCEPT 0x08
#define WS_FD_CONNECT 0x10
#define WS_FD_CLOSE 0x20
#define WS_FD_ADDRESS_LIST_CHANGE 0x200
#define WS_FD_MAX_EVENTS 10
#define WS_WAIT_EVENT_0 0
#define WS_WAIT_TIMEOUT 258
#define WS_WAIT_FAILED 0xffffffffu
#define WS_INFINITE 0xffffffffu
#define WS_IOC_KEEPALIVE_VALS 0x98000004u
#define WS_IOC_UDP_CONNRESET 0x9800000cu
#define WS_AF_INET 2
#define WS_AF_INET6 23

typedef struct {
    long lNetworkEvents;
    int iErrorCode[WS_FD_MAX_EVENTS];
} ws_network_events;

AWP_USER(BOOL, PostMessageW, (HWND, UINT, WPARAM, LPARAM))
AWP_KERNEL(BOOL, SetEvent, (HANDLE))
AWP_KERNEL(BOOL, ResetEvent, (HANDLE))
AWP_KERNEL(BOOL, CloseHandle, (HANDLE))
AWP_KERNEL(HANDLE, CreateEventA, (void *, BOOL, BOOL, const char *))
AWP_KERNEL(DWORD, WaitForMultipleObjects, (DWORD, const HANDLE *, BOOL, DWORD))

typedef struct ws_rec {
    struct ws_rec *next;
    int fd;
    HWND hwnd;      /* WSAAsyncSelect target (async mode) */
    UINT msg;
    HANDLE ev;      /* WSAEventSelect event (event mode) */
    long mask;      /* FD_* interest, 0 = none */
    unsigned pend;  /* event mode: FD_* bits not yet reported by WSAEnumNetworkEvents */
    int err[WS_FD_MAX_EVENTS];
    unsigned char connecting, listening, connected, dgram;
    unsigned char read_armed, oob_armed, accept_armed, write_want, eof;
} ws_rec;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ws_rec *g_recs;
static int g_source_added;

/* ---- records (g_lock held) ------------------------------------------------ */
static ws_rec *rec_find(int fd)
{
    for (ws_rec *r = g_recs; r; r = r->next)
        if (r->fd == fd)
            return r;
    return NULL;
}

static ws_rec *rec_get(int fd)
{
    ws_rec *r = rec_find(fd);
    if (!r) {
        r = calloc(1, sizeof *r);
        if (!r)
            return NULL;
        r->fd = fd;
        r->next = g_recs;
        g_recs = r;
    }
    return r;
}

static void rec_unlink(int fd)
{
    for (ws_rec **pp = &g_recs; *pp; pp = &(*pp)->next)
        if ((*pp)->fd == fd) {
            ws_rec *r = *pp;
            *pp = r->next;
            free(r);
            return;
        }
}

/* Registration bookkeeping: the one-shot flags restart, as a new WSAAsyncSelect does. */
static void rec_arm(ws_rec *r, long mask)
{
    r->mask = mask;
    r->pend = 0;
    memset(r->err, 0, sizeof r->err);
    r->read_armed = r->oob_armed = r->accept_armed = r->eof = 0;
    r->write_want = r->connected && !r->listening && !r->connecting;
}

/* Reports one FD_* event: a window message (async mode) or a bit plus the event object (event mode). */
static void notify(ws_rec *r, long fe, int err)
{
    if (!(r->mask & fe))
        return;
    if (r->ev) {
        r->pend |= (unsigned)fe;
        r->err[__builtin_ctzl((unsigned long)fe)] = err;
        __aw_kernel32_SetEvent(r->ev);
    } else if (r->hwnd) {
        __aw_user32_PostMessageW(r->hwnd, r->msg, (WPARAM)(uintptr_t)r->fd,
                                 (LPARAM)(((uint32_t)(uint16_t)err << 16) | (uint32_t)fe));
    }
}

static int so_error(int fd)
{
    int e = 0;
    socklen_t l = sizeof e;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) < 0)
        e = errno;
    return ws_errno_wsa(e);
}

/* ---- state machine (g_lock held) -------------------------------------------- */
static void complete_connect(ws_rec *r)
{
    int err = so_error(r->fd);
    r->connecting = 0;
    notify(r, WS_FD_CONNECT, err);
    if (err) {
        r->eof = 1;
        return;
    }
    r->connected = 1;
    r->write_want = 0;
    notify(r, WS_FD_WRITE, 0);
}

/* POLLIN/POLLHUP/POLLERR on a connected socket: data, end of stream or error. */
static void probe_read(ws_rec *r, int hup)
{
    char c;
    ssize_t n = recv(r->fd, &c, 1, MSG_PEEK);
    int e = errno;
    if (r->dgram) {
        if (n >= 0) {
            r->read_armed = 1;
            notify(r, WS_FD_READ, 0);
        } else if (e != EAGAIN && e != EINTR) {
            r->read_armed = 1;
            notify(r, WS_FD_READ, ws_errno_wsa(e));
        }
        return;
    }
    if (n > 0) {
        r->read_armed = 1;
        notify(r, WS_FD_READ, 0);
        return;
    }
    if (n < 0 && (e == EAGAIN || e == EINTR) && !hup)
        return;
    r->eof = 1; /* stop watching: a closed socket keeps reporting POLLHUP */
    notify(r, WS_FD_CLOSE, n == 0 || e == EAGAIN ? 0 : ws_errno_wsa(e));
}

static int fill_cb(struct pollfd *pf, int max)
{
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (ws_rec *r = g_recs; r && n < max; r = r->next) {
        short ev = 0;
        if (!r->mask || r->eof)
            continue;
        if (r->connecting)
            ev = POLLOUT;
        else if (r->listening) {
            if ((r->mask & WS_FD_ACCEPT) && !r->accept_armed)
                ev = POLLIN;
        } else if (r->connected) {
            if ((r->mask & (WS_FD_READ | WS_FD_CLOSE)) && !r->read_armed)
                ev |= POLLIN;
            if ((r->mask & WS_FD_OOB) && !r->oob_armed)
                ev |= POLLPRI;
            if ((r->mask & WS_FD_WRITE) && r->write_want)
                ev |= POLLOUT;
        }
        if (!ev)
            continue;
        pf[n].fd = r->fd;
        pf[n].events = ev;
        pf[n].revents = 0;
        n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

static void ready_cb(struct pollfd *pf, int n)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < n; i++) {
        short re = pf[i].revents;
        if (!re)
            continue;
        ws_rec *r = rec_find(pf[i].fd);
        if (!r || !r->mask || r->eof)
            continue;
        if (r->connecting) {
            if (re & (POLLOUT | POLLERR | POLLHUP))
                complete_connect(r);
            continue;
        }
        if (r->listening) {
            if (re & (POLLERR | POLLHUP)) {
                r->eof = 1;
            } else if (re & POLLIN) {
                r->accept_armed = 1;
                notify(r, WS_FD_ACCEPT, 0);
            }
            continue;
        }
        if (!r->connected)
            continue;
        if ((re & POLLOUT) && r->write_want && (r->mask & WS_FD_WRITE)) {
            r->write_want = 0;
            notify(r, WS_FD_WRITE, 0);
        }
        if ((re & POLLPRI) && !r->oob_armed && (r->mask & WS_FD_OOB)) {
            r->oob_armed = 1;
            notify(r, WS_FD_OOB, 0);
        }
        if ((re & (POLLIN | POLLERR | POLLHUP)) && !r->read_armed && !r->eof)
            probe_read(r, (re & (POLLERR | POLLHUP)) != 0);
        /* armed for reading with the socket hung up: stop asking for writability too, or
         * POLLHUP would wake the loop forever */
        if ((re & (POLLERR | POLLHUP)) && r->read_armed)
            r->write_want = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

static void ensure_source(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_source_added) {
        g_source_added = 1;
        plat_add_fd_source(fill_cb, ready_cb);
    }
    pthread_mutex_unlock(&g_lock);
}

void aw_net_pump(int timeout_ms)
{
    struct pollfd pf[256];
    int n = fill_cb(pf, 256);
    if (n == 0) {
        if (timeout_ms > 0)
            poll(NULL, 0, timeout_ms);
        return;
    }
    if (poll(pf, (nfds_t)n, timeout_ms) > 0)
        ready_cb(pf, n);
}

/* ---- hooks called by ws2_32.c ------------------------------------------------ */
void ws_async_forget(int fd)
{
    pthread_mutex_lock(&g_lock);
    rec_unlink(fd);
    pthread_mutex_unlock(&g_lock);
}

void ws_async_listening(int fd)
{
    pthread_mutex_lock(&g_lock);
    ws_rec *r = rec_get(fd);
    if (r) {
        r->listening = 1;
        r->connected = 1;
        r->write_want = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void ws_async_accepted(int listener)
{
    pthread_mutex_lock(&g_lock);
    ws_rec *r = rec_find(listener);
    if (r)
        r->accept_armed = 0;
    pthread_mutex_unlock(&g_lock);
}

void ws_async_connecting(int fd)
{
    pthread_mutex_lock(&g_lock);
    ws_rec *r = rec_get(fd);
    if (r) {
        r->connecting = 1;
        r->connected = 0;
        r->write_want = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void ws_async_read_done(int fd)
{
    pthread_mutex_lock(&g_lock);
    ws_rec *r = rec_find(fd);
    if (r)
        r->read_armed = r->oob_armed = 0;
    pthread_mutex_unlock(&g_lock);
}

void ws_async_write_blocked(int fd)
{
    pthread_mutex_lock(&g_lock);
    ws_rec *r = rec_find(fd);
    if (r && r->connected && !r->listening)
        r->write_want = 1;
    pthread_mutex_unlock(&g_lock);
}

/* ---- registration ------------------------------------------------------------ */
/* Sets the registration of fd: window message (h != NULL) or event object (ev != NULL).
 * mask 0 cancels it and restores blocking mode. */
static int register_socket(SOCKET s, HWND h, UINT msg, HANDLE ev, long mask)
{
    int fd = (int)s;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) {
        ws_set_error(WSAENOTSOCK);
        return SOCKET_ERROR;
    }
    ensure_source();
    pthread_mutex_lock(&g_lock);
    ws_rec *r = rec_get(fd);
    if (!r) {
        pthread_mutex_unlock(&g_lock);
        ws_set_error(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    if (!mask) {
        r->mask = 0;
        r->hwnd = NULL;
        r->ev = NULL;
        r->pend = 0;
        if (!r->connecting)
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        if (!r->connecting && !r->listening)
            rec_unlink(fd);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    int st = 0;
    socklen_t tl = sizeof st;
    r->dgram = getsockopt(fd, SOL_SOCKET, SO_TYPE, &st, &tl) == 0 && st == SOCK_DGRAM;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0 || r->dgram)
        r->connected = 1;
    r->hwnd = ev ? NULL : h;
    r->msg = msg;
    r->ev = ev;
    rec_arm(r, mask);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    pthread_mutex_unlock(&g_lock);
    aw_wake(); /* another thread may be blocked in the wait; let it rebuild its poll set */
    return 0;
}

API(ws2_32, int, WSAAsyncSelect, (SOCKET s, HWND hwnd, UINT msg, long lEvent))
{
    if (lEvent && !hwnd) {
        ws_set_error(WSAEINVAL);
        return SOCKET_ERROR;
    }
    return register_socket(s, hwnd, msg, NULL, lEvent);
}

API(ws2_32, int, WSAEventSelect, (SOCKET s, HANDLE ev, long lNetworkEvents))
{
    if (lNetworkEvents && !ev) {
        ws_set_error(WSAEINVAL);
        return SOCKET_ERROR;
    }
    return register_socket(s, NULL, 0, ev, lNetworkEvents);
}

API(ws2_32, int, WSAEnumNetworkEvents, (SOCKET s, HANDLE ev, ws_network_events *out))
{
    if (!out) {
        ws_set_error(WSAEFAULT);
        return SOCKET_ERROR;
    }
    pthread_mutex_lock(&g_lock);
    ws_rec *r = rec_find((int)s);
    if (!r) {
        pthread_mutex_unlock(&g_lock);
        ws_set_error(WSAENOTSOCK);
        return SOCKET_ERROR;
    }
    memset(out, 0, sizeof *out);
    out->lNetworkEvents = (long)r->pend;
    memcpy(out->iErrorCode, r->err, sizeof out->iErrorCode);
    r->pend = 0;
    memset(r->err, 0, sizeof r->err);
    if (ev)
        __aw_kernel32_ResetEvent(ev);
    else if (r->ev)
        __aw_kernel32_ResetEvent(r->ev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ---- event objects ------------------------------------------------------------- */
API(ws2_32, HANDLE, WSACreateEvent, (void))
{
    return __aw_kernel32_CreateEventA(NULL, TRUE, FALSE, NULL);
}

API(ws2_32, BOOL, WSACloseEvent, (HANDLE ev)) { return __aw_kernel32_CloseHandle(ev); }
API(ws2_32, BOOL, WSASetEvent, (HANDLE ev)) { return __aw_kernel32_SetEvent(ev); }
API(ws2_32, BOOL, WSAResetEvent, (HANDLE ev)) { return __aw_kernel32_ResetEvent(ev); }

API(ws2_32, DWORD, WSAWaitForMultipleEvents, (DWORD n, const HANDLE *evs, BOOL all, DWORD ms, BOOL alertable))
{
    (void)alertable;
    if (n == 0 || n > 64 || !evs) {
        ws_set_error(WSAEINVAL);
        return WS_WAIT_FAILED;
    }
    uint32_t start = aw_tick_ms();
    for (;;) {
        aw_net_pump(0);
        DWORD r = __aw_kernel32_WaitForMultipleObjects(n, evs, all, 0);
        if (r != WS_WAIT_TIMEOUT)
            return r == 0xffffffffu ? WS_WAIT_FAILED : r;
        uint32_t slice = 10;
        if (ms != WS_INFINITE) {
            uint32_t el = aw_tick_ms() - start;
            if (el >= ms)
                return WS_WAIT_TIMEOUT;
            if (ms - el < slice)
                slice = ms - el;
        }
        aw_net_pump((int)slice);
    }
}

/* ---- WSAIoctl / WSAAddressToStringA -------------------------------------------- */
API(ws2_32, int, WSAIoctl,
    (SOCKET s, DWORD code, void *in, DWORD inlen, void *out, DWORD outlen, DWORD *ret, void *ov, void *cr))
{
    (void)out;
    (void)outlen;
    (void)ov;
    (void)cr;
    if (ret)
        *ret = 0;
    if (code == WS_IOC_UDP_CONNRESET)
        return 0;
    if (code == WS_IOC_KEEPALIVE_VALS) { /* struct tcp_keepalive { u_long onoff, keepalivetime, interval; } */
        uint32_t v[3];
        if (!in || inlen < sizeof v) {
            ws_set_error(WSAEFAULT);
            return SOCKET_ERROR;
        }
        memcpy(v, in, sizeof v);
        int on = v[0] != 0;
        if (setsockopt((int)s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) < 0) {
            ws_set_error(ws_errno_wsa(errno));
            return SOCKET_ERROR;
        }
        int idle = (int)(v[1] / 1000), intvl = (int)(v[2] / 1000);
        if (idle > 0) setsockopt((int)s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        if (intvl > 0) setsockopt((int)s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        return 0;
    }
    ws_set_error(WSAEOPNOTSUPP);
    return SOCKET_ERROR;
}

API(ws2_32, int, WSAAddressToStringA, (void *sa, DWORD salen, void *info, char *buf, DWORD *buflen))
{
    (void)info;
    const unsigned char *p = sa;
    char host[INET6_ADDRSTRLEN], out[INET6_ADDRSTRLEN + 16];
    uint16_t fam, port;
    if (!p || salen < 4 || !buf || !buflen) {
        ws_set_error(WSAEFAULT);
        return SOCKET_ERROR;
    }
    memcpy(&fam, p, 2);
    memcpy(&port, p + 2, 2);
    port = ntohs(port);
    if (fam == WS_AF_INET && salen >= 8) {
        inet_ntop(AF_INET, p + 4, host, sizeof host);
        snprintf(out, sizeof out, "%s:%u", host, (unsigned)port);
    } else if ((fam == WS_AF_INET6 || fam == AF_INET6) && salen >= 24) {
        inet_ntop(AF_INET6, p + 8, host, sizeof host);
        snprintf(out, sizeof out, "[%s]:%u", host, (unsigned)port);
    } else {
        ws_set_error(WSAEINVAL);
        return SOCKET_ERROR;
    }
    DWORD need = (DWORD)strlen(out) + 1;
    if (*buflen < need) {
        *buflen = need;
        ws_set_error(WSAEFAULT);
        return SOCKET_ERROR;
    }
    memcpy(buf, out, need);
    *buflen = need;
    return 0;
}

/* ---- asynchronous name resolution ---------------------------------------------- */
/* WSAAsyncGetHostByName and friends: the lookup runs on a detached worker thread that writes
 * the Win32 structure into the program's buffer and then posts WSAMAKEASYNCREPLY(size, error)
 * to the window, with the task handle as wParam.  Tasks live on g_tasks until the worker is
 * done with them.  WSACancelAsyncRequest only flags a pending task; the worker, which owns the
 * task, drops the message and frees it.  A task marked done (message being posted) cannot be
 * cancelled any more, which is what WSAEALREADY reports. */
enum { TK_HOST, TK_ADDR, TK_SERV, TK_PROTO };

typedef struct ws_task {
    struct ws_task *next;
    uintptr_t id;       /* the HANDLE given to the program, never 0 */
    HWND hwnd;
    UINT msg;
    int kind;
    char *name;         /* host/service/protocol name, or the raw address bytes */
    int nlen;           /* length of the address bytes (TK_ADDR) */
    int atype;          /* address family (TK_ADDR) */
    char *proto;        /* protocol name (TK_SERV), or NULL */
    char *buf;          /* program's buffer */
    DWORD buflen;
    int cancelled, done;
} ws_task;

static pthread_mutex_t g_task_lock = PTHREAD_MUTEX_INITIALIZER;
static ws_task *g_tasks;
static uintptr_t g_task_seq;

/* g_task_lock held */
static ws_task *task_find_locked(uintptr_t id)
{
    for (ws_task *t = g_tasks; t; t = t->next)
        if (t->id == id)
            return t;
    return NULL;
}

static void task_unlink_locked(ws_task *t)
{
    for (ws_task **pp = &g_tasks; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            return;
        }
}

static void task_free(ws_task *t)
{
    if (!t)
        return;
    free(t->name);
    free(t->proto);
    free(t);
}

static void *task_main(void *arg)
{
    ws_task *t = arg;
    DWORD need = 0;
    int err;
    switch (t->kind) {
    case TK_HOST:
        err = ws_lookup_host(t->name, t->buf, t->buflen, &need);
        break;
    case TK_ADDR:
        err = ws_lookup_addr(t->name, t->nlen, t->atype, t->buf, t->buflen, &need);
        break;
    case TK_SERV:
        err = ws_lookup_serv(t->name, t->proto, t->buf, t->buflen, &need);
        break;
    default:
        err = ws_lookup_proto(t->name, t->buf, t->buflen, &need);
        break;
    }
    /* WSAMAKEASYNCREPLY(buflen, error): the size is reported on success and on WSAENOBUFS */
    uint32_t size = (err == 0 || err == WSAENOBUFS) ? (need & 0xffffu) : 0;
    LPARAM lp = (LPARAM)(((uint32_t)size << 16) | ((uint32_t)err & 0xffffu));

    pthread_mutex_lock(&g_task_lock);
    if (t->cancelled) {
        task_unlink_locked(t);
        pthread_mutex_unlock(&g_task_lock);
        task_free(t);
        return NULL;
    }
    t->done = 1;
    pthread_mutex_unlock(&g_task_lock);

    __aw_user32_PostMessageW(t->hwnd, t->msg, (WPARAM)t->id, lp);

    pthread_mutex_lock(&g_task_lock);
    task_unlink_locked(t);
    pthread_mutex_unlock(&g_task_lock);
    task_free(t);
    return NULL;
}

static HANDLE task_start(int kind, HWND hwnd, UINT msg, const char *name, int nlen, int atype,
                         const char *proto, char *buf, int buflen)
{
    if (!hwnd) {
        ws_set_error(WSAEINVAL);
        return NULL;
    }
    if (!name || !buf || buflen < 0 || nlen < 0 || (kind == TK_ADDR && nlen > 256)) {
        ws_set_error(WSAEFAULT);
        return NULL;
    }
    ws_task *t = calloc(1, sizeof *t);
    if (!t) {
        ws_set_error(WSAENOBUFS);
        return NULL;
    }
    size_t n = kind == TK_ADDR ? (size_t)nlen : strlen(name);
    t->name = malloc(n + 1);
    if (!t->name) {
        free(t);
        ws_set_error(WSAENOBUFS);
        return NULL;
    }
    memcpy(t->name, name, n);
    t->name[n] = 0;
    if (kind == TK_SERV && proto) {
        t->proto = strdup(proto);
        if (!t->proto) {
            task_free(t);
            ws_set_error(WSAENOBUFS);
            return NULL;
        }
    }
    t->kind = kind;
    t->hwnd = hwnd;
    t->msg = msg;
    t->nlen = nlen;
    t->atype = atype;
    t->buf = buf;
    t->buflen = (DWORD)buflen;

    pthread_mutex_lock(&g_task_lock);
    if (!++g_task_seq)
        ++g_task_seq; /* 0 is never a handle */
    t->id = g_task_seq;
    t->next = g_tasks;
    g_tasks = t;
    pthread_mutex_unlock(&g_task_lock);

    pthread_t th;
    if (pthread_create(&th, NULL, task_main, t)) {
        pthread_mutex_lock(&g_task_lock);
        task_unlink_locked(t);
        pthread_mutex_unlock(&g_task_lock);
        task_free(t);
        ws_set_error(WSAENOBUFS);
        return NULL;
    }
    pthread_detach(th);
    return (HANDLE)(uintptr_t)t->id;
}

API(ws2_32, HANDLE, WSAAsyncGetHostByName, (HWND hWnd, UINT wMsg, const char *name, char *buf, int buflen))
{
    return task_start(TK_HOST, hWnd, wMsg, name, 0, 0, NULL, buf, buflen);
}

API(ws2_32, HANDLE, WSAAsyncGetHostByAddr,
    (HWND hWnd, UINT wMsg, const char *addr, int len, int type, char *buf, int buflen))
{
    if (!addr) {
        ws_set_error(WSAEFAULT);
        return NULL;
    }
    return task_start(TK_ADDR, hWnd, wMsg, addr, len, type, NULL, buf, buflen);
}

API(ws2_32, HANDLE, WSAAsyncGetServByName,
    (HWND hWnd, UINT wMsg, const char *name, const char *proto, char *buf, int buflen))
{
    return task_start(TK_SERV, hWnd, wMsg, name, 0, 0, proto, buf, buflen);
}

API(ws2_32, HANDLE, WSAAsyncGetProtoByName, (HWND hWnd, UINT wMsg, const char *name, char *buf, int buflen))
{
    return task_start(TK_PROTO, hWnd, wMsg, name, 0, 0, NULL, buf, buflen);
}

API(ws2_32, int, WSACancelAsyncRequest, (HANDLE hAsyncTaskHandle))
{
    int err = 0;
    pthread_mutex_lock(&g_task_lock);
    ws_task *t = task_find_locked((uintptr_t)hAsyncTaskHandle);
    if (!t)
        err = WSAEINVAL;
    else if (t->done)
        err = WSAEALREADY;
    else
        t->cancelled = 1;
    pthread_mutex_unlock(&g_task_lock);
    if (err) {
        ws_set_error(err);
        return SOCKET_ERROR;
    }
    return 0;
}
