/* Winsock internals shared by ws2_32.c (synchronous calls) and ws2_async.c (async notification). */
#ifndef WS2_INT_H
#define WS2_INT_H
#include "aw.h"
#include <stdint.h>

typedef uintptr_t SOCKET;

#define INVALID_SOCKET ((SOCKET)(~(uintptr_t)0))
#define SOCKET_ERROR (-1)

/* ---- Windows constants -------------------------------------------------- */
#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN 128
#define WSA_FD_SETSIZE 64

typedef struct {
    WORD wVersion, wHighVersion;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA;

typedef struct {
    u_int fd_count;
    SOCKET fd_array[WSA_FD_SETSIZE];
} win_fd_set;

typedef struct {
    int32_t tv_sec, tv_usec;
} win_timeval;

#define WS2_AF_INET 2
#define WS2_AF_INET6 23
#define WS2_SOL_SOCKET 0xffff
#define WS2_SO_REUSEADDR 4
#define WS2_SO_KEEPALIVE 8
#define WS2_SO_DONTROUTE 0x10
#define WS2_SO_BROADCAST 0x20
#define WS2_SO_LINGER 0x80
#define WS2_SO_SNDBUF 0x1001
#define WS2_SO_RCVBUF 0x1002
#define WS2_SO_SNDTIMEO 0x1005
#define WS2_SO_RCVTIMEO 0x1006
#define WS2_SO_ERROR 0x1007
#define WS2_SO_TYPE 0x1008
#define WS2_SO_DONTLINGER 0xffffff7f

/* Windows WSAE* codes */
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEACCES 10013
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEDESTADDRREQ 10039
#define WSAEMSGSIZE 10040
#define WSAEPROTOTYPE 10041
#define WSAENOPROTOOPT 10042
#define WSAEPROTONOSUPPORT 10043
#define WSAESOCKTNOSUPPORT 10044
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN 10050
#define WSAENETUNREACH 10051
#define WSAENETRESET 10052
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAESHUTDOWN 10058
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAELOOP 10062
#define WSAENAMETOOLONG 10063
#define WSAEHOSTDOWN 10064
#define WSAEHOSTUNREACH 10065
#define WSAENOTEMPTY 10066
#define WSASYSNOTREADY 10091
#define WSAVERNOTSUPPORTED 10092
#define WSANOTINITIALISED 10093
#define WSAHOST_NOT_FOUND 11001
#define WSATRY_AGAIN 11002
#define WSANO_RECOVERY 11003
#define WSANO_DATA 11004

/* ws2_32.c: error bookkeeping shared with ws2_async.c */
void ws_set_error(int e);
int ws_errno_wsa(int e);

/* ws2_async.c: per-socket notification state, kept in step with the synchronous calls */
void ws_async_forget(int fd);
void ws_async_listening(int fd);
void ws_async_accepted(int listener);
void ws_async_connecting(int fd);
void ws_async_read_done(int fd);
void ws_async_write_blocked(int fd);

/* ws2_32.c: blocking lookups in the Win32 layout for the asynchronous calls (ws2_async.c).
 * Each sets *need to the buffer size required and writes into buf only when buflen fits;
 * returns 0, WSAENOBUFS (buffer too small) or the WSA error of the lookup. */
int ws_lookup_host(const char *name, char *buf, DWORD buflen, DWORD *need);
int ws_lookup_addr(const char *addr, int len, int type, char *buf, DWORD buflen, DWORD *need);
int ws_lookup_serv(const char *name, const char *proto, char *buf, DWORD buflen, DWORD *need);
int ws_lookup_proto(const char *name, char *buf, DWORD buflen, DWORD *need);

/* Services every registered socket (WSAAsyncSelect / WSAEventSelect); timeout_ms 0 = no wait. */
void aw_net_pump(int timeout_ms);

#endif
