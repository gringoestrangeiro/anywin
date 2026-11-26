/* Winsock asynchronous name resolution probe: WSAAsyncGetHostByName, WSAAsyncGetHostByAddr,
 * WSAAsyncGetServByName, WSAAsyncGetProtoByName and WSACancelAsyncRequest.  The replies are
 * window messages (wParam = task handle, lParam = WSAMAKEASYNCREPLY(size, error)) received by
 * a hidden window's message loop.  Prints "ok ..." / "FAIL ..." lines and a final verdict. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define WM_ASYNC_REPLY (WM_USER + 200)
#define TIMER_PUMP 91
#define NREPLY 32
#define E_NOBUFS 10055
#define E_HOST_NOT_FOUND 11001
#define E_TRY_AGAIN 11002
#define E_NO_RECOVERY 11003
#define E_NO_DATA 11004

typedef struct {
    WPARAM w;
    LPARAM l;
} reply_t;

static reply_t replies[NREPLY];
static int nreplies;
static HWND hwnd;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        printf("ok %s\n", what);
    } else {
        printf("FAIL %s (WSA %d)\n", what, WSAGetLastError());
        fails++;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

/* Pumps the message loop for about ms; WM_ASYNC_REPLY messages are kept for take(). */
static void pump(int ms)
{
    SetTimer(hwnd, TIMER_PUMP, (UINT)ms, NULL);
    for (;;) {
        MSG m;
        if (!GetMessageW(&m, NULL, 0, 0))
            break;
        if (m.message == WM_TIMER && m.wParam == TIMER_PUMP)
            break;
        if (m.message == WM_ASYNC_REPLY) {
            if (nreplies < NREPLY) {
                replies[nreplies].w = m.wParam;
                replies[nreplies].l = m.lParam;
                nreplies++;
            }
        } else {
            DispatchMessageW(&m);
        }
    }
    KillTimer(hwnd, TIMER_PUMP);
}

/* Waits up to ms for the reply of task h; on success stores its lParam and consumes it. */
static int take(HANDLE h, LPARAM *lp, int ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        for (int i = 0; i < nreplies; i++)
            if (replies[i].w == (WPARAM)h) {
                *lp = replies[i].l;
                memmove(&replies[i], &replies[i + 1], (size_t)(nreplies - i - 1) * sizeof replies[0]);
                nreplies--;
                return 1;
            }
        if (GetTickCount() - start >= (DWORD)ms)
            return 0;
        pump(20);
    }
}

static int in_buf(const void *p, const char *buf, size_t len)
{
    return (const char *)p >= buf && (const char *)p < buf + len;
}

static char hbuf[2048], hbuf2[2048], sbuf[512], pbuf[256], abuf[2048], cbuf[2048];

int main(void)
{
    WSADATA wd;
    WNDCLASSW wc;
    LPARAM lp = 0;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = L"wsa_async_probe";
    RegisterClassW(&wc);
    check(WSAStartup(0x0202, &wd) == 0, "WSAStartup");
    hwnd = CreateWindowExW(0, L"wsa_async_probe", L"wsa_async", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    check(hwnd != NULL, "create message window");

    /* 1. localhost: HOSTENT marshalled into the buffer, pointers inside it */
    HANDLE h1 = WSAAsyncGetHostByName(hwnd, WM_ASYNC_REPLY, "localhost", hbuf, sizeof hbuf);
    check(h1 != NULL, "WSAAsyncGetHostByName(localhost) returns a handle");
    if (take(h1, &lp, 5000)) {
        size_t used = HIWORD(lp);
        struct hostent *he = (struct hostent *)hbuf;
        check(LOWORD(lp) == 0, "localhost reply: no error");
        check(used >= sizeof(struct hostent) && used <= sizeof hbuf, "localhost reply: buffer length in HIWORD");
        check(in_buf(he->h_name, hbuf, used) && strncmp(he->h_name, "localhost", 9) == 0,
              "localhost: h_name points into buffer");
        check(he->h_addrtype == AF_INET && he->h_length == 4, "localhost: AF_INET, length 4");
        check(he->h_addr_list[0] && in_buf(he->h_addr_list[0], hbuf, used) &&
                  memcmp(he->h_addr_list[0], "\x7f\0\0\x01", 4) == 0,
              "localhost: first address is 127.0.0.1");
        check(he->h_aliases && in_buf(he->h_aliases, hbuf, used), "localhost: h_aliases inside buffer");
    } else {
        check(0, "localhost reply arrives");
    }

    /* 2. a name that does not exist: error in LOWORD, nothing in HIWORD */
    HANDLE h2 = WSAAsyncGetHostByName(hwnd, WM_ASYNC_REPLY, "no-such-host.invalid", hbuf2, sizeof hbuf2);
    check(h2 != NULL, "WSAAsyncGetHostByName(unknown name) returns a handle");
    if (take(h2, &lp, 5000)) {
        int e = LOWORD(lp);
        check(e == E_HOST_NOT_FOUND || e == E_TRY_AGAIN || e == E_NO_RECOVERY,
              "unknown host: WSA host error in LOWORD");
        check(HIWORD(lp) == 0, "unknown host: no size in HIWORD");
    } else {
        check(0, "unknown host reply arrives");
    }

    /* 3. buffer too small: WSAENOBUFS with the required size in HIWORD, then retry with that size */
    char small[8];
    HANDLE h3 = WSAAsyncGetHostByName(hwnd, WM_ASYNC_REPLY, "localhost", small, sizeof small);
    check(h3 != NULL, "WSAAsyncGetHostByName with 8-byte buffer returns a handle");
    if (take(h3, &lp, 5000)) {
        check(LOWORD(lp) == E_NOBUFS, "small buffer: WSAENOBUFS");
        check(HIWORD(lp) >= sizeof(struct hostent), "small buffer: required size in HIWORD");
        DWORD need = HIWORD(lp);
        HANDLE h4 = WSAAsyncGetHostByName(hwnd, WM_ASYNC_REPLY, "localhost", hbuf2, (int)need);
        check(h4 != NULL && take(h4, &lp, 5000) && LOWORD(lp) == 0 && HIWORD(lp) == need,
              "exact required size succeeds");
    } else {
        check(0, "small buffer reply arrives");
    }

    /* 4. service http/tcp */
    HANDLE h5 = WSAAsyncGetServByName(hwnd, WM_ASYNC_REPLY, "http", "tcp", sbuf, sizeof sbuf);
    check(h5 != NULL, "WSAAsyncGetServByName(http) returns a handle");
    if (take(h5, &lp, 5000)) {
        struct servent *se = (struct servent *)sbuf;
        check(LOWORD(lp) == 0, "http reply: no error");
        check((unsigned short)se->s_port == htons(80), "http: port 80 (network order)");
        check(se->s_name && strcmp(se->s_name, "http") == 0, "http: s_name");
        check(se->s_proto && strcmp(se->s_proto, "tcp") == 0, "http: s_proto tcp");
    } else {
        check(0, "http reply arrives");
    }
    HANDLE h6 = WSAAsyncGetServByName(hwnd, WM_ASYNC_REPLY, "no-such-service", "tcp", sbuf, sizeof sbuf);
    check(h6 != NULL && take(h6, &lp, 5000) && LOWORD(lp) == E_NO_DATA,
          "unknown service: WSANO_DATA");

    /* 5. protocol tcp */
    HANDLE h7 = WSAAsyncGetProtoByName(hwnd, WM_ASYNC_REPLY, "tcp", pbuf, sizeof pbuf);
    check(h7 != NULL, "WSAAsyncGetProtoByName(tcp) returns a handle");
    if (take(h7, &lp, 5000)) {
        struct protoent *pe = (struct protoent *)pbuf;
        check(LOWORD(lp) == 0, "tcp reply: no error");
        check(pe->p_proto == 6 && pe->p_name && strcmp(pe->p_name, "tcp") == 0, "tcp: p_proto 6, p_name");
    } else {
        check(0, "tcp reply arrives");
    }
    HANDLE h8 = WSAAsyncGetProtoByName(hwnd, WM_ASYNC_REPLY, "no-such-proto", pbuf, sizeof pbuf);
    check(h8 != NULL && take(h8, &lp, 5000) && LOWORD(lp) == E_NO_DATA, "unknown protocol: WSANO_DATA");

    /* 6. reverse lookup of 127.0.0.1 */
    const char addr[4] = { 127, 0, 0, 1 };
    HANDLE h9 = WSAAsyncGetHostByAddr(hwnd, WM_ASYNC_REPLY, addr, 4, AF_INET, abuf, sizeof abuf);
    check(h9 != NULL, "WSAAsyncGetHostByAddr returns a handle");
    if (take(h9, &lp, 5000)) {
        struct hostent *he = (struct hostent *)abuf;
        check(LOWORD(lp) == 0 && he->h_addr_list[0] && memcmp(he->h_addr_list[0], addr, 4) == 0,
              "reverse lookup of 127.0.0.1 reports the address");
    } else {
        check(0, "reverse lookup reply arrives");
    }

    /* 7. argument validation is synchronous and posts nothing */
    WSASetLastError(0);
    check(WSAAsyncGetHostByName(hwnd, WM_ASYNC_REPLY, NULL, hbuf, sizeof hbuf) == NULL &&
              WSAGetLastError() == WSAEFAULT,
          "NULL name: NULL handle and WSAEFAULT");
    check(WSAAsyncGetHostByName(NULL, WM_ASYNC_REPLY, "localhost", hbuf, sizeof hbuf) == NULL &&
              WSAGetLastError() == WSAEINVAL,
          "NULL window: NULL handle and WSAEINVAL");

    /* 8. cancellation */
    check(WSACancelAsyncRequest((HANDLE)(uintptr_t)0x1234567) == SOCKET_ERROR &&
              WSAGetLastError() == WSAEINVAL,
          "WSACancelAsyncRequest(unknown handle): SOCKET_ERROR, WSAEINVAL");
    HANDLE hc = WSAAsyncGetHostByName(hwnd, WM_ASYNC_REPLY, "localhost", cbuf, sizeof cbuf);
    check(hc != NULL, "request to cancel returns a handle");
    int rc = WSACancelAsyncRequest(hc);
    if (rc == 0) {
        check(!take(hc, &lp, 500), "cancelled request: no message after WSACancelAsyncRequest");
        printf("ok cancel: request was still pending\n");
    } else if (WSAGetLastError() == WSAEALREADY) {
        /* the lookup finished first: its reply must then be delivered */
        check(take(hc, &lp, 5000), "completed request: reply delivered, cancel reports WSAEALREADY");
    } else {
        check(0, "WSACancelAsyncRequest on a pending request returns 0");
    }

    /* 9. several requests in flight: each reply carries its own handle */
    HANDLE ha = WSAAsyncGetHostByName(hwnd, WM_ASYNC_REPLY, "localhost", hbuf, sizeof hbuf);
    HANDLE hb = WSAAsyncGetServByName(hwnd, WM_ASYNC_REPLY, "https", "tcp", sbuf, sizeof sbuf);
    HANDLE hd = WSAAsyncGetProtoByName(hwnd, WM_ASYNC_REPLY, "udp", pbuf, sizeof pbuf);
    LPARAM la = 0, lb = 0, ld = 0;
    check(ha && hb && hd, "three requests in flight");
    int got_b = take(hb, &lb, 5000);
    check(got_b, "https reply arrives with its handle");
    check(got_b && LOWORD(lb) == 0, "https reply: no error");
    check(got_b && (unsigned short)((struct servent *)sbuf)->s_port == htons(443), "https: port 443");
    check(take(hd, &ld, 5000) && LOWORD(ld) == 0 && ((struct protoent *)pbuf)->p_proto == 17,
          "udp reply matches its handle");
    check(take(ha, &la, 5000) && LOWORD(la) == 0, "localhost reply matches its handle");

    if (fails)
        printf("wsa_async_probe: FAILED (%d checks)\n", fails);
    else
        printf("wsa_async_probe: ok\n");
    WSACleanup();
    return fails ? 1 : 0;
}
