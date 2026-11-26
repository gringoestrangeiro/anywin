/* ws2_32: Winsock 2 on top of Linux sockets.  Windows structure layouts are
 * preserved exactly; SOCKET values are the Linux file descriptors. */
#include "aw.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include "ws2_int.h"

typedef unsigned int u_int;

static __thread int wsa_error;

static void set_wsa(int e)
{
    wsa_error = e;
    aw_set_last_error((DWORD)e);
}

void ws_set_error(int e) { set_wsa(e); }

/* errno -> WSAE* */
static int errno_to_wsa(int e)
{
    switch (e) {
    case 0: return 0;
    case EINTR: return WSAEINTR;
    case EBADF: return WSAEBADF;
    case EACCES: case EPERM: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return WSAEMFILE;
    case EAGAIN: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK; /* a non-blocking connect in progress reports WSAEWOULDBLOCK */
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: case EPIPE: return WSAECONNRESET;
    case ENOBUFS: case ENOMEM: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ESHUTDOWN: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case ELOOP: return WSAELOOP;
    case ENAMETOOLONG: return WSAENAMETOOLONG;
    case EHOSTDOWN: return WSAEHOSTDOWN;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    case ENOTEMPTY: return WSAENOTEMPTY;
    default: return WSAEINVAL;
    }
}

int ws_errno_wsa(int e) { return errno_to_wsa(e); }

static int af_to_linux(int af) { return af == WS2_AF_INET6 ? AF_INET6 : af; }
static int af_to_win(int af) { return af == AF_INET6 ? WS2_AF_INET6 : af; }

/* Copy a Windows sockaddr, patching the family for Linux. */
static int sa_to_linux(const void *src, int len, struct sockaddr_storage *dst)
{
    if (!src || len <= 0 || len > (int)sizeof *dst)
        return -1;
    memcpy(dst, src, (size_t)len);
    if (dst->ss_family == WS2_AF_INET6)
        dst->ss_family = AF_INET6;
    return len;
}

static void sa_to_win(void *sa, int *len)
{
    struct sockaddr *s = sa;
    if (s && (s->sa_family == AF_INET6))
        s->sa_family = WS2_AF_INET6;
    (void)len;
}

/* ---- startup ------------------------------------------------------------ */
static int wsa_refcount;

API(ws2_32, int, WSAStartup, (WORD version, WSADATA *data))
{
    int major = version & 0xff, minor = version >> 8;
    if (!data)
        return WSAEFAULT;
    if (!major)
        return WSAVERNOTSUPPORTED;
    memset(data, 0, sizeof *data);
    if (major > 2 || (major == 2 && minor > 2))
        data->wVersion = 0x0202;
    else if (major == 1 && minor > 1)
        data->wVersion = 0x0101;
    else
        data->wVersion = version;
    data->wHighVersion = 0x0202;
    snprintf(data->szDescription, sizeof data->szDescription, "anywin Winsock 2.2");
    data->iMaxSockets = major == 1 ? 32767 : 0;
    data->iMaxUdpDg = major == 1 ? 65467 : 1472;
    wsa_refcount++;
    set_wsa(0);
    return 0;
}

API(ws2_32, int, WSACleanup, (void))
{
    if (!wsa_refcount) {
        set_wsa(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }
    wsa_refcount--;
    return 0;
}

API(ws2_32, int, WSAGetLastError, (void)) { return wsa_error; }
API(ws2_32, void, WSASetLastError, (int e))
{
    wsa_error = e;
    aw_set_last_error((DWORD)e);
}

/* ---- sockets ------------------------------------------------------------ */
API(ws2_32, SOCKET, socket, (int af, int type, int protocol))
{
    int fd = socket(af_to_linux(af), type, protocol);
    if (fd < 0) {
        set_wsa(errno_to_wsa(errno));
        return INVALID_SOCKET;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return (SOCKET)fd;
}

API(ws2_32, int, closesocket, (SOCKET s))
{
    ws_async_forget((int)s);
    if (close((int)s) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    return 0;
}

API(ws2_32, int, bind, (SOCKET s, const void *name, int namelen))
{
    struct sockaddr_storage ss;
    if (sa_to_linux(name, namelen, &ss) < 0) {
        set_wsa(WSAEFAULT);
        return SOCKET_ERROR;
    }
    if (bind((int)s, (struct sockaddr *)&ss, (socklen_t)namelen) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    return 0;
}

API(ws2_32, int, listen, (SOCKET s, int backlog))
{
    if (backlog == 0x7fffffff)
        backlog = 128;
    if (listen((int)s, backlog) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    ws_async_listening((int)s);
    return 0;
}

API(ws2_32, SOCKET, accept, (SOCKET s, void *addr, int *addrlen))
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int fd = accept((int)s, addr ? (struct sockaddr *)&ss : NULL, addr ? &sl : NULL);
    if (fd < 0) {
        set_wsa(errno_to_wsa(errno));
        return INVALID_SOCKET;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* as on Windows, the accepted socket starts non-blocking when the listener is */
    if (fcntl((int)s, F_GETFL) & O_NONBLOCK)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    ws_async_accepted((int)s);
    if (addr && addrlen) {
        int n = (int)sl < *addrlen ? (int)sl : *addrlen;
        sa_to_win(&ss, &n);
        memcpy(addr, &ss, (size_t)n);
        *addrlen = (int)sl;
    }
    return (SOCKET)fd;
}

API(ws2_32, int, connect, (SOCKET s, const void *name, int namelen))
{
    struct sockaddr_storage ss;
    if (sa_to_linux(name, namelen, &ss) < 0) {
        set_wsa(WSAEFAULT);
        return SOCKET_ERROR;
    }
    if (connect((int)s, (struct sockaddr *)&ss, (socklen_t)namelen) < 0) {
        int e = errno;
        if (e == EINPROGRESS)
            ws_async_connecting((int)s);
        set_wsa(errno_to_wsa(e));
        return SOCKET_ERROR;
    }
    return 0;
}

API(ws2_32, int, send, (SOCKET s, const char *buf, int len, int flags))
{
    ssize_t r = send((int)s, buf, (size_t)len, flags);
    if (r < 0) {
        if (errno == EAGAIN)
            ws_async_write_blocked((int)s);
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    return (int)r;
}

API(ws2_32, int, recv, (SOCKET s, char *buf, int len, int flags))
{
    ssize_t r = recv((int)s, buf, (size_t)len, flags);
    ws_async_read_done((int)s);
    if (r < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    return (int)r;
}

API(ws2_32, int, sendto, (SOCKET s, const char *buf, int len, int flags, const void *to, int tolen))
{
    struct sockaddr_storage ss;
    int sl = 0;
    if (to) {
        sl = sa_to_linux(to, tolen, &ss);
        if (sl < 0) {
            set_wsa(WSAEFAULT);
            return SOCKET_ERROR;
        }
    }
    ssize_t r = sendto((int)s, buf, (size_t)len, flags, to ? (struct sockaddr *)&ss : NULL, (socklen_t)sl);
    if (r < 0) {
        if (errno == EAGAIN)
            ws_async_write_blocked((int)s);
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    return (int)r;
}

API(ws2_32, int, recvfrom, (SOCKET s, char *buf, int len, int flags, void *from, int *fromlen))
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    ssize_t r = recvfrom((int)s, buf, (size_t)len, flags, from ? (struct sockaddr *)&ss : NULL, from ? &sl : NULL);
    ws_async_read_done((int)s);
    if (r < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    if (from && fromlen) {
        int n = (int)sl < *fromlen ? (int)sl : *fromlen;
        sa_to_win(&ss, &n);
        memcpy(from, &ss, (size_t)n);
        *fromlen = (int)sl;
    }
    return (int)r;
}

API(ws2_32, int, shutdown, (SOCKET s, int how))
{
    if (shutdown((int)s, how) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    return 0;
}

/* ---- select ------------------------------------------------------------- */
#define WS2_MAX_SELECT 192

API(ws2_32, int, select, (int nfds, win_fd_set *rd, win_fd_set *wr, win_fd_set *ex, win_timeval *timeout))
{
    struct pollfd pfds[WS2_MAX_SELECT];
    int rdm[WS2_MAX_SELECT], wrm[WS2_MAX_SELECT], exm[WS2_MAX_SELECT];
    int np = 0;
    (void)nfds;
    for (int i = 0; rd && i < (int)rd->fd_count && np < WS2_MAX_SELECT; i++) {
        pfds[np].fd = (int)rd->fd_array[i];
        pfds[np].events = POLLIN;
        pfds[np].revents = 0;
        rdm[np] = (int)rd->fd_array[i];
        wrm[np] = -1;
        exm[np] = -1;
        np++;
    }
    for (int i = 0; wr && i < (int)wr->fd_count && np < WS2_MAX_SELECT; i++) {
        pfds[np].fd = (int)wr->fd_array[i];
        pfds[np].events = POLLOUT;
        pfds[np].revents = 0;
        rdm[np] = -1;
        wrm[np] = (int)wr->fd_array[i];
        exm[np] = -1;
        np++;
    }
    for (int i = 0; ex && i < (int)ex->fd_count && np < WS2_MAX_SELECT; i++) {
        pfds[np].fd = (int)ex->fd_array[i];
        pfds[np].events = POLLPRI;
        pfds[np].revents = 0;
        rdm[np] = -1;
        wrm[np] = -1;
        exm[np] = (int)ex->fd_array[i];
        np++;
    }
    int ms = -1;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0)
            ms = -1;
        else
            ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    }
    if (np == 0) {
        if (ms > 0)
            poll(NULL, 0, ms);
        return 0;
    }
    int r = poll(pfds, (nfds_t)np, ms);
    if (r < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    u_int nrd = 0, nwr = 0, nex = 0;
    int count = 0;
    for (int i = 0; i < np; i++) {
        short ev = pfds[i].revents;
        if (!ev)
            continue;
        if (rdm[i] >= 0 && (ev & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            if (rd) rd->fd_array[nrd++] = (SOCKET)rdm[i];
            count++;
        }
        if (wrm[i] >= 0 && (ev & (POLLOUT | POLLHUP | POLLERR | POLLNVAL))) {
            if (wr) wr->fd_array[nwr++] = (SOCKET)wrm[i];
            count++;
        }
        if (exm[i] >= 0 && (ev & (POLLPRI | POLLERR | POLLNVAL))) {
            if (ex) ex->fd_array[nex++] = (SOCKET)exm[i];
            count++;
        }
    }
    if (rd) rd->fd_count = nrd;
    if (wr) wr->fd_count = nwr;
    if (ex) ex->fd_count = nex;
    return count;
}

API(ws2_32, int, __WSAFDIsSet, (SOCKET s, const win_fd_set *set))
{
    if (!set)
        return 0;
    for (u_int i = 0; i < set->fd_count; i++)
        if (set->fd_array[i] == s)
            return 1;
    return 0;
}

/* ---- ioctl -------------------------------------------------------------- */
API(ws2_32, int, ioctlsocket, (SOCKET s, int32_t cmd, uint32_t *argp))
{
    if (!argp) {
        set_wsa(WSAEFAULT);
        return SOCKET_ERROR;
    }
    if ((uint32_t)cmd == 0x8004667eu) { /* FIONBIO */
        int fl = fcntl((int)s, F_GETFL);
        if (fl < 0) {
            set_wsa(errno_to_wsa(errno));
            return SOCKET_ERROR;
        }
        fl = *argp ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        if (fcntl((int)s, F_SETFL, fl) < 0) {
            set_wsa(errno_to_wsa(errno));
            return SOCKET_ERROR;
        }
        return 0;
    }
    if ((uint32_t)cmd == 0x4004667fu) { /* FIONREAD */
        int n = 0;
        if (ioctl((int)s, FIONREAD, &n) < 0) {
            set_wsa(errno_to_wsa(errno));
            return SOCKET_ERROR;
        }
        *argp = (uint32_t)n;
        return 0;
    }
    set_wsa(WSAEINVAL);
    return SOCKET_ERROR;
}

/* ---- setsockopt / getsockopt ------------------------------------------- */
static int opt_to_linux(int level, int opt)
{
    if (level == WS2_SOL_SOCKET) {
        switch (opt) {
        case WS2_SO_REUSEADDR: return SO_REUSEADDR;
        case WS2_SO_KEEPALIVE: return SO_KEEPALIVE;
        case WS2_SO_DONTROUTE: return SO_DONTROUTE;
        case WS2_SO_BROADCAST: return SO_BROADCAST;
        case WS2_SO_LINGER: return SO_LINGER;
        case WS2_SO_SNDBUF: return SO_SNDBUF;
        case WS2_SO_RCVBUF: return SO_RCVBUF;
        case WS2_SO_SNDTIMEO: return SO_SNDTIMEO;
        case WS2_SO_RCVTIMEO: return SO_RCVTIMEO;
        case WS2_SO_ERROR: return SO_ERROR;
        case WS2_SO_TYPE: return SO_TYPE;
        default: return -1;
        }
    }
    if (level == 0 /* IPPROTO_IP */) {
        switch (opt) {
        case 2: return IP_HDRINCL;
        case 3: return IP_TOS;
        case 4: return IP_TTL;
        case 9: return IP_MULTICAST_IF;
        case 10: return IP_MULTICAST_TTL;
        case 11: return IP_MULTICAST_LOOP;
        case 12: return IP_ADD_MEMBERSHIP;
        case 13: return IP_DROP_MEMBERSHIP;
        default: break;
        }
    }
    return opt;
}

typedef struct { unsigned short l_onoff, l_linger; } win_linger;

API(ws2_32, int, setsockopt, (SOCKET s, int level, int optname, const char *optval, int optlen))
{
    int lvl = level == WS2_SOL_SOCKET ? SOL_SOCKET : level;
    int opt = opt_to_linux(level, optname);
    if (opt < 0 || !optval) {
        set_wsa(WSAENOPROTOOPT);
        return SOCKET_ERROR;
    }
    if (level == WS2_SOL_SOCKET && (optname == WS2_SO_SNDTIMEO || optname == WS2_SO_RCVTIMEO)) {
        DWORD ms = 0;
        if (optlen >= 4)
            memcpy(&ms, optval, 4);
        struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000) };
        if (setsockopt((int)s, SOL_SOCKET, opt, &tv, sizeof tv) < 0) {
            set_wsa(errno_to_wsa(errno));
            return SOCKET_ERROR;
        }
        return 0;
    }
    if (level == WS2_SOL_SOCKET && (optname == WS2_SO_LINGER || (uint32_t)optname == (uint32_t)WS2_SO_DONTLINGER)) {
        struct linger lv = {0, 0};
        if (optlen >= (int)sizeof(win_linger)) {
            win_linger wl;
            memcpy(&wl, optval, sizeof wl);
            if ((uint32_t)optname == (uint32_t)WS2_SO_DONTLINGER) {
                lv.l_onoff = 0;
            } else {
                lv.l_onoff = wl.l_onoff;
                lv.l_linger = wl.l_linger;
            }
        }
        if (setsockopt((int)s, SOL_SOCKET, SO_LINGER, &lv, sizeof lv) < 0) {
            set_wsa(errno_to_wsa(errno));
            return SOCKET_ERROR;
        }
        return 0;
    }
    if (setsockopt((int)s, lvl, opt, optval, (socklen_t)optlen) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    return 0;
}

API(ws2_32, int, getsockopt, (SOCKET s, int level, int optname, char *optval, int *optlen))
{
    int lvl = level == WS2_SOL_SOCKET ? SOL_SOCKET : level;
    int opt = opt_to_linux(level, optname);
    if (opt < 0 || !optval || !optlen) {
        set_wsa(WSAENOPROTOOPT);
        return SOCKET_ERROR;
    }
    if (level == WS2_SOL_SOCKET && (optname == WS2_SO_SNDTIMEO || optname == WS2_SO_RCVTIMEO)) {
        struct timeval tv;
        socklen_t sl = sizeof tv;
        if (getsockopt((int)s, SOL_SOCKET, opt, &tv, &sl) < 0) {
            set_wsa(errno_to_wsa(errno));
            return SOCKET_ERROR;
        }
        if (*optlen < 4) {
            set_wsa(WSAEFAULT);
            return SOCKET_ERROR;
        }
        DWORD ms = (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
        memcpy(optval, &ms, 4);
        *optlen = 4;
        return 0;
    }
    if (level == WS2_SOL_SOCKET && optname == WS2_SO_LINGER) {
        struct linger lv;
        socklen_t sl = sizeof lv;
        if (getsockopt((int)s, SOL_SOCKET, SO_LINGER, &lv, &sl) < 0) {
            set_wsa(errno_to_wsa(errno));
            return SOCKET_ERROR;
        }
        if (*optlen < (int)sizeof(win_linger)) {
            set_wsa(WSAEFAULT);
            return SOCKET_ERROR;
        }
        win_linger wl = { (unsigned short)lv.l_onoff, (unsigned short)lv.l_linger };
        memcpy(optval, &wl, sizeof wl);
        *optlen = sizeof wl;
        return 0;
    }
    socklen_t sl = (socklen_t)*optlen;
    if (getsockopt((int)s, lvl, opt, optval, &sl) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    *optlen = (int)sl;
    return 0;
}

API(ws2_32, int, getsockname, (SOCKET s, void *name, int *namelen))
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (!name || !namelen || getsockname((int)s, (struct sockaddr *)&ss, &sl) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    int n = (int)sl < *namelen ? (int)sl : *namelen;
    sa_to_win(&ss, &n);
    memcpy(name, &ss, (size_t)n);
    *namelen = (int)sl;
    return 0;
}

API(ws2_32, int, getpeername, (SOCKET s, void *name, int *namelen))
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (!name || !namelen || getpeername((int)s, (struct sockaddr *)&ss, &sl) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    int n = (int)sl < *namelen ? (int)sl : *namelen;
    sa_to_win(&ss, &n);
    memcpy(name, &ss, (size_t)n);
    *namelen = (int)sl;
    return 0;
}

API(ws2_32, int, gethostname, (char *name, int namelen))
{
    char buf[256];
    if (!name || namelen <= 0) {
        set_wsa(WSAEFAULT);
        return SOCKET_ERROR;
    }
    if (gethostname(buf, sizeof buf) < 0) {
        set_wsa(errno_to_wsa(errno));
        return SOCKET_ERROR;
    }
    buf[sizeof buf - 1] = 0;
    if ((int)strlen(buf) + 1 > namelen) {
        set_wsa(WSAEFAULT);
        return SOCKET_ERROR;
    }
    strcpy(name, buf);
    return 0;
}

/* ---- hostent / servent / protoent (per-thread Windows layouts) ---------- */
typedef struct {
    char *h_name;
    char **h_aliases;
    short h_addrtype;
    short h_length;
    char **h_addr_list;
} win_hostent;

/* x64 SDK layout (psdk _ip_types.h): s_proto precedes s_port */
typedef struct {
    char *s_name;
    char **s_aliases;
    char *s_proto;
    short s_port;
} win_servent;

typedef struct {
    char *p_name;
    char **p_aliases;
    short p_proto;
} win_protoent;

typedef struct {
    win_hostent he;
    char name[256];
    char *aliases[2];
    char *addrs[17];
    unsigned char abuf[16][16];
    win_servent se;
    char sname[128], sproto[64];
    char *saliases[2];
    win_protoent pe;
    char pname[128];
    char *paliases[2];
} hostent_storage;

static __thread hostent_storage hs;

static void reset_hostent(void)
{
    memset(&hs.he, 0, sizeof hs.he);
    hs.aliases[0] = NULL;
    hs.he.h_aliases = hs.aliases;
    for (int i = 0; i < 17; i++)
        hs.addrs[i] = NULL;
    hs.he.h_addr_list = hs.addrs;
    hs.he.h_addrtype = WS2_AF_INET;
    hs.he.h_length = 4;
    hs.he.h_name = hs.name;
}

/* Name resolution, shared by the synchronous calls (results in hs) and the asynchronous ones
 * (ws2_async.c, results marshalled into the program's buffer by ws_lookup_*).  Each returns 0
 * or the WSA error code. */
typedef struct {
    char name[256];
    int naddr;
    unsigned char addr[16][4];
} host_res;

typedef struct {
    char name[128], proto[64];
    int port; /* network byte order, as servent.s_port */
} serv_res;

typedef struct {
    char name[128];
    int proto;
} proto_res;

static int resolve_host(const char *name, host_res *out)
{
    struct addrinfo hints, *res = NULL;
    if (!name)
        return WSAHOST_NOT_FOUND;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    int r = getaddrinfo(name, NULL, &hints, &res);
    if (r)
        return r == EAI_AGAIN ? WSATRY_AGAIN : WSAHOST_NOT_FOUND;
    snprintf(out->name, sizeof out->name, "%s", res->ai_canonname ? res->ai_canonname : name);
    out->naddr = 0;
    for (struct addrinfo *ai = res; ai && out->naddr < 16; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET || !ai->ai_addr)
            continue;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ai->ai_addr;
        memcpy(out->addr[out->naddr++], &sin->sin_addr, 4);
    }
    freeaddrinfo(res);
    return out->naddr ? 0 : WSAHOST_NOT_FOUND;
}

static int resolve_addr(const char *addr, int len, int type, host_res *out)
{
    struct sockaddr_in sin;
    char host[256];
    if (!addr || len < 4 || type != WS2_AF_INET)
        return WSAHOST_NOT_FOUND;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    memcpy(&sin.sin_addr, addr, 4);
    int r = getnameinfo((struct sockaddr *)&sin, sizeof sin, host, sizeof host, NULL, 0, NI_NAMEREQD);
    if (r && inet_ntop(AF_INET, &sin.sin_addr, host, sizeof host) == NULL)
        return WSAHOST_NOT_FOUND;
    snprintf(out->name, sizeof out->name, "%s", host);
    out->naddr = 1;
    memcpy(out->addr[0], addr, 4);
    return 0;
}

static int resolve_serv(const char *name, const char *proto, serv_res *out)
{
    struct servent se, *res = NULL;
    char buf[1024];
    if (!name || getservbyname_r(name, proto, &se, buf, sizeof buf, &res) || !res)
        return WSANO_DATA;
    snprintf(out->name, sizeof out->name, "%s", se.s_name ? se.s_name : name);
    snprintf(out->proto, sizeof out->proto, "%s", se.s_proto ? se.s_proto : (proto ? proto : ""));
    out->port = se.s_port;
    return 0;
}

static int resolve_proto(const char *name, proto_res *out)
{
    struct protoent pe, *res = NULL;
    char buf[512];
    if (!name || getprotobyname_r(name, &pe, buf, sizeof buf, &res) || !res)
        return WSANO_DATA;
    snprintf(out->name, sizeof out->name, "%s", pe.p_name ? pe.p_name : name);
    out->proto = pe.p_proto;
    return 0;
}

API(ws2_32, void *, gethostbyname, (const char *name))
{
    host_res r;
    int e = resolve_host(name, &r);
    if (e) {
        set_wsa(e);
        return NULL;
    }
    reset_hostent();
    snprintf(hs.name, sizeof hs.name, "%s", r.name);
    for (int i = 0; i < r.naddr; i++) {
        memcpy(hs.abuf[i], r.addr[i], 4);
        hs.addrs[i] = (char *)hs.abuf[i];
    }
    hs.addrs[r.naddr] = NULL;
    return &hs.he;
}

API(ws2_32, void *, gethostbyaddr, (const char *addr, int len, int type))
{
    host_res r;
    int e = resolve_addr(addr, len, type, &r);
    if (e) {
        set_wsa(e);
        return NULL;
    }
    reset_hostent();
    snprintf(hs.name, sizeof hs.name, "%s", r.name);
    memcpy(hs.abuf[0], r.addr[0], 4);
    hs.addrs[0] = (char *)hs.abuf[0];
    hs.addrs[1] = NULL;
    return &hs.he;
}

API(ws2_32, void *, getservbyname, (const char *name, const char *proto))
{
    serv_res r;
    int e = resolve_serv(name, proto, &r);
    if (e) {
        set_wsa(e);
        return NULL;
    }
    memset(&hs.se, 0, sizeof hs.se);
    snprintf(hs.sname, sizeof hs.sname, "%s", r.name);
    snprintf(hs.sproto, sizeof hs.sproto, "%s", r.proto);
    hs.saliases[0] = NULL;
    hs.se.s_name = hs.sname;
    hs.se.s_aliases = hs.saliases;
    hs.se.s_port = (short)r.port;
    hs.se.s_proto = hs.sproto;
    return &hs.se;
}

API(ws2_32, void *, getservbyport, (int port, const char *proto))
{
    struct servent se, *res = NULL;
    char buf[1024];
    if (getservbyport_r(port, proto, &se, buf, sizeof buf, &res) || !res) {
        set_wsa(WSANO_DATA);
        return NULL;
    }
    memset(&hs.se, 0, sizeof hs.se);
    snprintf(hs.sname, sizeof hs.sname, "%s", se.s_name ? se.s_name : "");
    snprintf(hs.sproto, sizeof hs.sproto, "%s", se.s_proto ? se.s_proto : "");
    hs.saliases[0] = NULL;
    hs.se.s_name = hs.sname;
    hs.se.s_aliases = hs.saliases;
    hs.se.s_port = se.s_port;
    hs.se.s_proto = hs.sproto;
    return &hs.se;
}

API(ws2_32, void *, getprotobyname, (const char *name))
{
    proto_res r;
    int e = resolve_proto(name, &r);
    if (e) {
        set_wsa(e);
        return NULL;
    }
    memset(&hs.pe, 0, sizeof hs.pe);
    snprintf(hs.pname, sizeof hs.pname, "%s", r.name);
    hs.paliases[0] = NULL;
    hs.pe.p_name = hs.pname;
    hs.pe.p_aliases = hs.paliases;
    hs.pe.p_proto = (short)r.proto;
    return &hs.pe;
}

API(ws2_32, void *, getprotobynumber, (int num))
{
    struct protoent pe, *res = NULL;
    char buf[512];
    if (getprotobynumber_r(num, &pe, buf, sizeof buf, &res) || !res) {
        set_wsa(WSANO_DATA);
        return NULL;
    }
    memset(&hs.pe, 0, sizeof hs.pe);
    snprintf(hs.pname, sizeof hs.pname, "%s", pe.p_name ? pe.p_name : "");
    hs.paliases[0] = NULL;
    hs.pe.p_name = hs.pname;
    hs.pe.p_aliases = hs.paliases;
    hs.pe.p_proto = pe.p_proto;
    return &hs.pe;
}

/* ---- lookups for the asynchronous calls (ws2_async.c) ------------------- */
/* The Win32 structure goes at the start of the program's buffer, followed by the pointer
 * arrays and strings it refers to (absolute addresses inside that buffer), as the synchronous
 * results lay them out.  Each function sets *need to the size required; it writes the
 * structure only when buflen can hold it and returns WSAENOBUFS otherwise. */
static size_t wl_align8(size_t x) { return (x + 7) & ~(size_t)7; }

static int marshal_host(const host_res *r, char *buf, DWORD buflen, DWORD *need)
{
    size_t n = (size_t)r->naddr, len = strlen(r->name) + 1;
    size_t off_al = sizeof(win_hostent), off_addr = off_al + sizeof(char *);
    size_t off_name = off_addr + (n + 1) * sizeof(char *);
    size_t off_data = wl_align8(off_name + len);
    size_t total = off_data + 4 * n;
    *need = (DWORD)total;
    if (total > buflen)
        return WSAENOBUFS;
    char *aliases[1] = { NULL };
    char *addrs[17];
    for (size_t i = 0; i < n; i++) {
        memcpy(buf + off_data + 4 * i, r->addr[i], 4);
        addrs[i] = buf + off_data + 4 * i;
    }
    addrs[n] = NULL;
    memcpy(buf + off_name, r->name, len);
    memcpy(buf + off_al, aliases, sizeof aliases);
    memcpy(buf + off_addr, addrs, (n + 1) * sizeof(char *));
    win_hostent he;
    he.h_name = buf + off_name;
    he.h_aliases = (char **)(buf + off_al);
    he.h_addrtype = WS2_AF_INET;
    he.h_length = 4;
    he.h_addr_list = (char **)(buf + off_addr);
    memcpy(buf, &he, sizeof he);
    return 0;
}

static int marshal_serv(const serv_res *r, char *buf, DWORD buflen, DWORD *need)
{
    size_t off_al = sizeof(win_servent), off_name = off_al + sizeof(char *);
    size_t off_proto = off_name + strlen(r->name) + 1;
    size_t total = off_proto + strlen(r->proto) + 1;
    *need = (DWORD)total;
    if (total > buflen)
        return WSAENOBUFS;
    char *aliases[1] = { NULL };
    memcpy(buf + off_al, aliases, sizeof aliases);
    memcpy(buf + off_name, r->name, strlen(r->name) + 1);
    memcpy(buf + off_proto, r->proto, strlen(r->proto) + 1);
    win_servent se;
    se.s_name = buf + off_name;
    se.s_aliases = (char **)(buf + off_al);
    se.s_port = (short)r->port;
    se.s_proto = buf + off_proto;
    memcpy(buf, &se, sizeof se);
    return 0;
}

static int marshal_proto(const proto_res *r, char *buf, DWORD buflen, DWORD *need)
{
    size_t off_al = sizeof(win_protoent), off_name = off_al + sizeof(char *);
    size_t total = off_name + strlen(r->name) + 1;
    *need = (DWORD)total;
    if (total > buflen)
        return WSAENOBUFS;
    char *aliases[1] = { NULL };
    memcpy(buf + off_al, aliases, sizeof aliases);
    memcpy(buf + off_name, r->name, strlen(r->name) + 1);
    win_protoent pe;
    pe.p_name = buf + off_name;
    pe.p_aliases = (char **)(buf + off_al);
    pe.p_proto = (short)r->proto;
    memcpy(buf, &pe, sizeof pe);
    return 0;
}

int ws_lookup_host(const char *name, char *buf, DWORD buflen, DWORD *need)
{
    host_res r;
    *need = 0;
    int e = resolve_host(name, &r);
    return e ? e : marshal_host(&r, buf, buflen, need);
}

int ws_lookup_addr(const char *addr, int len, int type, char *buf, DWORD buflen, DWORD *need)
{
    host_res r;
    *need = 0;
    int e = resolve_addr(addr, len, type, &r);
    return e ? e : marshal_host(&r, buf, buflen, need);
}

int ws_lookup_serv(const char *name, const char *proto, char *buf, DWORD buflen, DWORD *need)
{
    serv_res r;
    *need = 0;
    int e = resolve_serv(name, proto, &r);
    return e ? e : marshal_serv(&r, buf, buflen, need);
}

int ws_lookup_proto(const char *name, char *buf, DWORD buflen, DWORD *need)
{
    proto_res r;
    *need = 0;
    int e = resolve_proto(name, &r);
    return e ? e : marshal_proto(&r, buf, buflen, need);
}

/* ---- getaddrinfo -------------------------------------------------------- */
typedef struct aw_addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t ai_addrlen;
    void *ai_canonname;
    struct sockaddr *ai_addr;
    struct aw_addrinfo *ai_next;
} aw_addrinfo;

static int ai_flags_to_linux(int f)
{
    int r = 0;
    if (f & 0x0001) r |= AI_PASSIVE;
    if (f & 0x0002) r |= AI_CANONNAME;
    if (f & 0x0004) r |= AI_NUMERICHOST;
    if (f & 0x0008) r |= AI_NUMERICSERV;
    if (f & 0x0100) r |= AI_ALL;
    if (f & 0x0400) r |= AI_ADDRCONFIG;
    if (f & 0x0800) r |= AI_V4MAPPED;
    return r;
}

static int eai_to_wsa(int e)
{
    switch (e) {
    case 0: return 0;
    case EAI_AGAIN: return WSATRY_AGAIN;
    case EAI_BADFLAGS: return WSAEINVAL;
    case EAI_FAIL: return WSANO_RECOVERY;
    case EAI_FAMILY: return WSAEAFNOSUPPORT;
    case EAI_MEMORY: return WSAENOBUFS;
    case EAI_NONAME: return WSAHOST_NOT_FOUND;
    case EAI_SERVICE: return WSAEOPNOTSUPP;
    case EAI_SOCKTYPE: return WSAESOCKTNOSUPPORT;
    case EAI_SYSTEM: return errno_to_wsa(errno);
    default: return WSAHOST_NOT_FOUND;
    }
}

static aw_addrinfo *convert_addrinfo(struct addrinfo *res, int wide)
{
    aw_addrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        aw_addrinfo *n = calloc(1, sizeof *n);
        if (!n)
            break;
        n->ai_flags = ai->ai_flags;
        n->ai_family = af_to_win(ai->ai_family);
        n->ai_socktype = ai->ai_socktype;
        n->ai_protocol = ai->ai_protocol;
        n->ai_addrlen = ai->ai_addrlen;
        if (ai->ai_addr && ai->ai_addrlen) {
            n->ai_addr = malloc(ai->ai_addrlen);
            if (n->ai_addr) {
                memcpy(n->ai_addr, ai->ai_addr, ai->ai_addrlen);
                if (n->ai_addr->sa_family == AF_INET6)
                    n->ai_addr->sa_family = WS2_AF_INET6;
            }
        }
        if (ai->ai_canonname) {
            if (wide) {
                n->ai_canonname = aw_u2w(ai->ai_canonname, -1);
            } else {
                n->ai_canonname = strdup(ai->ai_canonname);
            }
        }
        *tail = n;
        tail = &n->ai_next;
    }
    return head;
}

static int do_getaddrinfo(const char *node, const char *service, const aw_addrinfo *hints, aw_addrinfo **res, int wide)
{
    struct addrinfo h, *r = NULL;
    memset(&h, 0, sizeof h);
    if (hints) {
        h.ai_flags = ai_flags_to_linux(hints->ai_flags);
        h.ai_family = af_to_linux(hints->ai_family);
        h.ai_socktype = hints->ai_socktype;
        h.ai_protocol = hints->ai_protocol;
    }
    if (!res) {
        set_wsa(WSAEFAULT);
        return WSAEINVAL;
    }
    int rc = getaddrinfo(node, service, hints ? &h : NULL, &r);
    if (rc) {
        *res = NULL;
        int e = eai_to_wsa(rc);
        set_wsa(e);
        return e;
    }
    *res = convert_addrinfo(r, wide);
    freeaddrinfo(r);
    if (!*res) {
        set_wsa(WSAENOBUFS);
        return WSAENOBUFS;
    }
    return 0;
}

API(ws2_32, int, getaddrinfo, (const char *node, const char *service, const aw_addrinfo *hints, aw_addrinfo **res))
{
    return do_getaddrinfo(node, service, hints, res, 0);
}

API(ws2_32, int, GetAddrInfoW, (const WCHAR *node, const WCHAR *service, const aw_addrinfo *hints, aw_addrinfo **res))
{
    char *n = node ? aw_w2u(node, -1) : NULL;
    char *s = service ? aw_w2u(service, -1) : NULL;
    int r = do_getaddrinfo(n, s, hints, res, 1);
    free(n);
    free(s);
    return r;
}

static void free_ai_list(aw_addrinfo *ai)
{
    while (ai) {
        aw_addrinfo *nx = ai->ai_next;
        free(ai->ai_addr);
        free(ai->ai_canonname);
        free(ai);
        ai = nx;
    }
}

API(ws2_32, void, freeaddrinfo, (aw_addrinfo *ai)) { free_ai_list(ai); }
API(ws2_32, void, FreeAddrInfoW, (aw_addrinfo * ai)) { free_ai_list(ai); }

/* ---- getnameinfo -------------------------------------------------------- */
API(ws2_32, int, getnameinfo,
    (const void *sa, int salen, char *host, DWORD hostlen, char *serv, DWORD servlen, int flags))
{
    struct sockaddr_storage ss;
    if (sa_to_linux(sa, salen, &ss) < 0) {
        set_wsa(WSAEFAULT);
        return WSAEINVAL;
    }
    int r = getnameinfo((struct sockaddr *)&ss, (socklen_t)salen, host, (socklen_t)hostlen, serv,
                        (socklen_t)servlen, flags);
    if (r) {
        int e = eai_to_wsa(r);
        set_wsa(e);
        return e;
    }
    return 0;
}

API(ws2_32, int, GetNameInfoW,
    (const void *sa, int salen, WCHAR *host, DWORD hostlen, WCHAR *serv, DWORD servlen, int flags))
{
    char hbuf[1024], sbuf[256];
    struct sockaddr_storage ss;
    if (sa_to_linux(sa, salen, &ss) < 0) {
        set_wsa(WSAEFAULT);
        return WSAEINVAL;
    }
    int r = getnameinfo((struct sockaddr *)&ss, (socklen_t)salen, host ? hbuf : NULL,
                        (socklen_t)sizeof hbuf, serv ? sbuf : NULL, (socklen_t)sizeof sbuf, flags);
    if (r) {
        int e = eai_to_wsa(r);
        set_wsa(e);
        return e;
    }
    if (host)
        aw_u2w_buf(hbuf, -1, host, (int)hostlen);
    if (serv)
        aw_u2w_buf(sbuf, -1, serv, (int)servlen);
    return 0;
}

/* ---- address conversion -------------------------------------------------- */
API(ws2_32, unsigned int, inet_addr, (const char *cp))
{
    struct in_addr a;
    if (!cp || inet_pton(AF_INET, cp, &a) != 1)
        return 0xffffffffu; /* INADDR_NONE */
    return a.s_addr;
}

API(ws2_32, char *, inet_ntoa, (struct in_addr in))
{
    static __thread char buf[24];
    if (!inet_ntop(AF_INET, &in, buf, sizeof buf))
        return NULL;
    return buf;
}

API(ws2_32, int, inet_pton, (int af, const char *src, void *dst))
{
    int r = inet_pton(af_to_linux(af), src, dst);
    if (r < 0) {
        set_wsa(errno_to_wsa(errno));
        return -1;
    }
    return r;
}

API(ws2_32, const char *, inet_ntop, (int af, const void *src, char *dst, size_t size))
{
    const char *r = inet_ntop(af_to_linux(af), src, dst, size);
    if (!r)
        set_wsa(errno_to_wsa(errno));
    return r;
}

API(ws2_32, int, InetPtonA, (int af, const char *src, void *dst))
{
    return __aw_ws2_32_inet_pton(af, src, dst);
}

API(ws2_32, const char *, InetNtopA, (int af, const void *src, char *dst, size_t size))
{
    return __aw_ws2_32_inet_ntop(af, src, dst, size);
}

API(ws2_32, int, InetPtonW, (int af, const WCHAR *src, void *dst))
{
    char *s = aw_w2u(src, -1);
    int r = inet_pton(af_to_linux(af), s, dst);
    free(s);
    if (r < 0) {
        set_wsa(errno_to_wsa(errno));
        return -1;
    }
    return r;
}

API(ws2_32, const WCHAR *, InetNtopW, (int af, const void *src, WCHAR *dst, size_t size))
{
    char buf[128];
    if (!inet_ntop(af_to_linux(af), src, buf, sizeof buf)) {
        set_wsa(errno_to_wsa(errno));
        return NULL;
    }
    if (aw_u2w_buf(buf, -1, dst, (int)size) + 1 > (int)size)
        return NULL;
    return dst;
}

API(ws2_32, unsigned short, htons, (unsigned short v)) { return (unsigned short)__builtin_bswap16(v); }
API(ws2_32, unsigned short, ntohs, (unsigned short v)) { return (unsigned short)__builtin_bswap16(v); }
API(ws2_32, unsigned int, htonl, (unsigned int v)) { return __builtin_bswap32(v); }
API(ws2_32, unsigned int, ntohl, (unsigned int v)) { return __builtin_bswap32(v); }
API(ws2_32, unsigned long long, htonll, (unsigned long long v)) { return __builtin_bswap64(v); }
API(ws2_32, unsigned long long, ntohll, (unsigned long long v)) { return __builtin_bswap64(v); }
