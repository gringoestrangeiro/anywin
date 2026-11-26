/* Winsock asynchronous notification probe: WSAAsyncSelect messages (FD_ACCEPT, FD_CONNECT,
 * FD_WRITE, FD_READ, FD_CLOSE with error codes, cancel), the one-shot re-arm rules, the
 * WSAEWOULDBLOCK paths, WSAEventSelect / WSAEnumNetworkEvents / WSAWaitForMultipleEvents and
 * MsgWaitForMultipleObjects on a socket event, a cross-thread wake-up of the message loop and
 * idle CPU use.  Loopback TCP only.  Prints "ok ..." / "FAIL ..." lines and a final verdict. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define WM_SOCK (WM_USER + 100)
#define NOTE_MAX 1024
#define TIMER_WAIT 77

typedef struct {
    SOCKET s;
    long ev;
    int err;
} note_t;

static note_t notes[NOTE_MAX];
static int nnotes;
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

static void note_add(SOCKET s, long ev, int err)
{
    if (nnotes < NOTE_MAX) {
        notes[nnotes].s = s;
        notes[nnotes].ev = ev;
        notes[nnotes].err = err;
        nnotes++;
    }
}

/* Pumps the message loop until a WM_SOCK note for (s, ev) arrives (consumed) or ms elapse. */
static int take(SOCKET s, long ev, int *err, int ms)
{
    for (int i = 0; i < nnotes; i++)
        if (notes[i].s == s && notes[i].ev == ev) {
            if (err) *err = notes[i].err;
            memmove(&notes[i], &notes[i + 1], (size_t)(nnotes - i - 1) * sizeof notes[0]);
            nnotes--;
            return 1;
        }
    SetTimer(hwnd, TIMER_WAIT, (UINT)ms, NULL);
    for (;;) {
        MSG m;
        if (!GetMessageW(&m, NULL, 0, 0))
            break;
        if (m.message == WM_SOCK) {
            note_add((SOCKET)m.wParam, (long)LOWORD(m.lParam), (int)HIWORD(m.lParam));
            if ((SOCKET)m.wParam == s && (long)LOWORD(m.lParam) == ev) {
                KillTimer(hwnd, TIMER_WAIT);
                return take(s, ev, err, 0);
            }
        } else if (m.message == WM_TIMER && m.wParam == TIMER_WAIT) {
            break;
        } else {
            DispatchMessageW(&m);
        }
    }
    KillTimer(hwnd, TIMER_WAIT);
    return 0;
}

/* Pumps for ms, keeping whatever arrives; used to let the stream settle. */
static void settle(int ms)
{
    take((SOCKET)0, 0, NULL, ms);
    nnotes = 0;
}

static SOCKET tcp(void) { return socket(AF_INET, SOCK_STREAM, 0); }

static struct sockaddr_in loop(unsigned short port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

static unsigned short listen_port(SOCKET *out)
{
    SOCKET l = tcp();
    struct sockaddr_in a = loop(0);
    int n = sizeof a;
    bind(l, (struct sockaddr *)&a, sizeof a);
    getsockname(l, (struct sockaddr *)&a, &n);
    listen(l, 8);
    *out = l;
    return ntohs(a.sin_port);
}

/* Blocking loopback pair: *cli connected, *srv accepted. */
static void pair(SOCKET *cli, SOCKET *srv)
{
    SOCKET l;
    unsigned short port = listen_port(&l);
    *cli = tcp();
    struct sockaddr_in a = loop(port);
    connect(*cli, (struct sockaddr *)&a, sizeof a);
    *srv = accept(l, NULL, NULL);
    closesocket(l);
}

static DWORD WINAPI delayed_send(LPVOID arg)
{
    Sleep(300);
    send((SOCKET)(uintptr_t)arg, "late", 4, 0);
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

int main(void)
{
    WSADATA wd;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = L"wsa_probe";
    RegisterClassW(&wc);
    check(WSAStartup(0x0202, &wd) == 0, "WSAStartup");
    hwnd = CreateWindowExW(0, L"wsa_probe", L"wsa", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    check(hwnd != NULL, "create message window");

    /* 1. accept, connect (non-blocking), write-after-connect */
    SOCKET l;
    unsigned short port = listen_port(&l);
    check(WSAAsyncSelect(l, hwnd, WM_SOCK, FD_ACCEPT) == 0, "WSAAsyncSelect listener");
    SOCKET cli = tcp();
    check(WSAAsyncSelect(cli, hwnd, WM_SOCK, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE) == 0,
          "WSAAsyncSelect client");
    struct sockaddr_in a = loop(port);
    int r = connect(cli, (struct sockaddr *)&a, sizeof a);
    check(r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "non-blocking connect -> WSAEWOULDBLOCK");
    int err = -1;
    check(take(l, FD_ACCEPT, &err, 2000), "FD_ACCEPT on listener");
    check(take(cli, FD_CONNECT, &err, 2000) && err == 0, "FD_CONNECT without error");
    check(take(cli, FD_WRITE, &err, 2000), "FD_WRITE after connect");
    SOCKET srv = accept(l, NULL, NULL);
    check(srv != INVALID_SOCKET, "accept returns a socket");
    check(WSAAsyncSelect(srv, hwnd, WM_SOCK, FD_READ | FD_WRITE | FD_CLOSE) == 0, "WSAAsyncSelect accepted");
    check(take(srv, FD_WRITE, &err, 2000), "FD_WRITE at registration of connected socket");
    settle(100);

    /* 2. FD_READ when data arrives, recv clears it, data kept (partial read re-posts) */
    check(send(srv, "hello", 5, 0) == 5, "server send hello");
    check(take(cli, FD_READ, &err, 2000) && err == 0, "FD_READ on client");
    char buf[64];
    int n = recv(cli, buf, sizeof buf, 0);
    check(n == 5 && memcmp(buf, "hello", 5) == 0, "client recv hello");
    check(send(cli, "ab", 2, 0) == 2 && send(cli, "cd", 2, 0) == 2, "client send ab cd");
    check(take(srv, FD_READ, &err, 2000), "FD_READ on server");
    n = recv(srv, buf, 1, 0);
    check(n == 1, "partial recv of 1 byte");
    check(take(srv, FD_READ, &err, 2000), "FD_READ re-posted while data remains");
    n = recv(srv, buf, sizeof buf, 0);
    check(n == 3 && memcmp(buf, "bcd", 3) == 0, "rest of the data arrives intact");
    settle(100);

    /* 3. WSAEWOULDBLOCK on send, FD_WRITE once space frees up */
    static char big[65536];
    memset(big, 'x', sizeof big);
    long sent = 0, got = 0;
    for (;;) {
        int s = send(cli, big, sizeof big, 0);
        if (s == SOCKET_ERROR) {
            check(WSAGetLastError() == WSAEWOULDBLOCK, "send fills the buffer -> WSAEWOULDBLOCK");
            break;
        }
        sent += s;
        if (sent > 64L * 1024 * 1024) {
            check(0, "send never blocked");
            break;
        }
    }
    settle(50);
    while (got < sent) {
        n = recv(srv, buf, sizeof buf, 0);
        if (n > 0) {
            got += n;
        } else if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            if (!take(srv, FD_READ, &err, 2000))
                break;
        } else {
            break;
        }
    }
    check(got == sent, "all blocked data delivered");
    check(take(cli, FD_WRITE, &err, 2000), "FD_WRITE after the receiver drained");
    settle(50);

    /* 4. FD_CLOSE on graceful close, then no further read events */
    closesocket(cli);
    check(take(srv, FD_CLOSE, &err, 2000) && err == 0, "FD_CLOSE on peer close, no error");
    closesocket(srv);

    /* 5. connection refused -> FD_CONNECT carries WSAECONNREFUSED */
    SOCKET dead;
    unsigned short deadport = listen_port(&dead);
    closesocket(dead);
    SOCKET c2 = tcp();
    WSAAsyncSelect(c2, hwnd, WM_SOCK, FD_CONNECT | FD_CLOSE);
    struct sockaddr_in da = loop(deadport);
    connect(c2, (struct sockaddr *)&da, sizeof da);
    check(take(c2, FD_CONNECT, &err, 2000) && err == WSAECONNREFUSED, "FD_CONNECT error WSAECONNREFUSED");
    closesocket(c2);

    /* 6. cancel with lEvent 0: no more messages, socket back to blocking */
    SOCKET c3, s3;
    pair(&c3, &s3);
    WSAAsyncSelect(s3, hwnd, WM_SOCK, FD_READ);
    WSAAsyncSelect(s3, hwnd, WM_SOCK, 0);
    send(c3, "zz", 2, 0);
    check(!take(s3, FD_READ, &err, 300), "cancelled registration posts nothing");
    u_long nb = 0;
    check(ioctlsocket(s3, FIONBIO, &nb) == 0, "ioctlsocket FIONBIO back to blocking");
    closesocket(c3);
    closesocket(s3);
    closesocket(l);

    /* 7. event objects: WSAEventSelect, WSAEnumNetworkEvents, WSAWaitForMultipleEvents */
    SOCKET e1, e2;
    pair(&e1, &e2);
    WSAEVENT ev = WSACreateEvent();
    check(WSAEventSelect(e2, ev, FD_READ | FD_CLOSE) == 0, "WSAEventSelect");
    check(WSAWaitForMultipleEvents(1, &ev, FALSE, 0, FALSE) == WSA_WAIT_TIMEOUT, "event not signalled while idle");
    send(e1, "e", 1, 0);
    check(WSAWaitForMultipleEvents(1, &ev, FALSE, 2000, FALSE) == WSA_WAIT_EVENT_0, "WSAWaitForMultipleEvents sees FD_READ");
    WSANETWORKEVENTS ne;
    check(WSAEnumNetworkEvents(e2, ev, &ne) == 0 && (ne.lNetworkEvents & FD_READ) && ne.iErrorCode[0] == 0,
          "WSAEnumNetworkEvents reports FD_READ");
    check(WSAWaitForMultipleEvents(1, &ev, FALSE, 0, FALSE) == WSA_WAIT_TIMEOUT, "event reset by enumeration");
    check(recv(e2, buf, 1, 0) == 1, "recv the event byte");
    send(e1, "f", 1, 0);
    check(MsgWaitForMultipleObjects(1, &ev, FALSE, 2000, QS_ALLINPUT) == WAIT_OBJECT_0,
          "MsgWaitForMultipleObjects wakes on a socket event");
    recv(e2, buf, 1, 0);
    WSAEnumNetworkEvents(e2, ev, &ne);
    closesocket(e1);
    check(WSAWaitForMultipleEvents(1, &ev, FALSE, 2000, FALSE) == WSA_WAIT_EVENT_0, "FD_CLOSE through event");
    WSAEnumNetworkEvents(e2, ev, &ne);
    check((ne.lNetworkEvents & FD_CLOSE) != 0, "WSAEnumNetworkEvents reports FD_CLOSE");
    closesocket(e2);
    WSACloseEvent(ev);

    /* 8. a send from another thread wakes the blocked message loop */
    SOCKET w1, w2;
    pair(&w1, &w2);
    WSAAsyncSelect(w2, hwnd, WM_SOCK, FD_READ);
    HANDLE th = CreateThread(NULL, 0, delayed_send, (LPVOID)(uintptr_t)w1, 0, NULL);
    check(take(w2, FD_READ, &err, 3000), "cross-thread send wakes the message loop");
    CloseHandle(th);
    recv(w2, buf, sizeof buf, 0);
    closesocket(w1);
    closesocket(w2);

    /* 9. idle: registered sockets with nothing to say cost no CPU */
    SOCKET i1, i2;
    pair(&i1, &i2);
    WSAAsyncSelect(i2, hwnd, WM_SOCK, FD_READ | FD_WRITE | FD_CLOSE);
    settle(100);
    FILETIME c0, e0, x0, k0, u0, c1, e1t, x1, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    settle(1500);
    GetProcessTimes(GetCurrentProcess(), &c1, &e1t, &k1, &u1);
    ULONGLONG used = ((ULONGLONG)u1.dwHighDateTime << 32 | u1.dwLowDateTime) +
                     ((ULONGLONG)k1.dwHighDateTime << 32 | k1.dwLowDateTime) -
                     ((ULONGLONG)u0.dwHighDateTime << 32 | u0.dwLowDateTime) -
                     ((ULONGLONG)k0.dwHighDateTime << 32 | k0.dwLowDateTime);
    printf("idle cpu over 1.5 s: %.1f ms\n", (double)used / 10000.0);
    check(used < 150ULL * 10000ULL, "idle CPU below 150 ms over 1.5 s");
    closesocket(i1);
    closesocket(i2);

    WSACleanup();
    printf("%s\n", fails ? "wsa_async: FAILED" : "wsa_async: ok");
    return fails ? 1 : 0;
}
