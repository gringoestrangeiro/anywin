/* Winsock 2 regression probe: real TCP/UDP traffic over loopback plus the
 * socket management calls used by ordinary programs. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

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

static struct sockaddr_in loopback(unsigned short port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

int main(void)
{
    WSADATA wd;
    check(WSACleanup() == SOCKET_ERROR && WSAGetLastError() == WSANOTINITIALISED,
          "WSACleanup before startup");
    check(WSAStartup(MAKEWORD(1, 1), &wd) == 0 && wd.wVersion == MAKEWORD(1, 1), "WSAStartup 1.1");
    check(WSACleanup() == 0, "WSACleanup 1.1");
    check(WSAStartup(MAKEWORD(2, 2), &wd) == 0 && wd.wVersion == MAKEWORD(2, 2), "WSAStartup 2.2");

    /* getaddrinfo on a numeric loopback address. */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    check(getaddrinfo("127.0.0.1", "7", &hints, &res) == 0 && res && res->ai_family == AF_INET &&
              res->ai_addrlen >= sizeof(struct sockaddr_in),
          "getaddrinfo");
    if (res)
        freeaddrinfo(res);

    /* TCP listener on an ephemeral loopback port. */
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(srv != INVALID_SOCKET, "socket TCP");
    BOOL yes = TRUE;
    check(setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes) == 0, "setsockopt SO_REUSEADDR");

    struct sockaddr_in addr = loopback(0);
    check(bind(srv, (const struct sockaddr *)&addr, sizeof addr) == 0, "bind");
    int alen = sizeof addr;
    check(getsockname(srv, (struct sockaddr *)&addr, &alen) == 0 && ntohs(addr.sin_port) != 0, "getsockname");
    check(listen(srv, 4) == 0, "listen");

    SOCKET cli = socket(AF_INET, SOCK_STREAM, 0);
    check(cli != INVALID_SOCKET, "socket client");
    check(connect(cli, (const struct sockaddr *)&addr, sizeof addr) == 0, "connect");

    struct sockaddr_in peer;
    int plen = sizeof peer;
    SOCKET acc = accept(srv, (struct sockaddr *)&peer, &plen);
    check(acc != INVALID_SOCKET && peer.sin_family == AF_INET && ntohs(peer.sin_port) != 0, "accept");

    int type = 0, tlen = sizeof type;
    check(getsockopt(cli, SOL_SOCKET, SO_TYPE, (char *)&type, &tlen) == 0 && type == SOCK_STREAM, "getsockopt SO_TYPE");

    const char ping[] = "ping";
    char buf[32];
    check(send(cli, ping, 4, 0) == 4, "send");

    fd_set rf;
    struct timeval tv;
    FD_ZERO(&rf);
    FD_SET(acc, &rf);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    int n = select(0, &rf, NULL, NULL, &tv);
    check(n == 1 && FD_ISSET(acc, &rf), "select readable");

    int got = recv(acc, buf, 4, 0);
    check(got == 4 && memcmp(buf, ping, 4) == 0, "recv");
    check(send(acc, "pong", 4, 0) == 4, "send reply");
    got = recv(cli, buf, 4, 0);
    check(got == 4 && memcmp(buf, "pong", 4) == 0, "recv reply");

    /* Non-blocking mode must report WSAEWOULDBLOCK instead of blocking. */
    u_long nb = 1;
    check(ioctlsocket(cli, FIONBIO, &nb) == 0, "ioctlsocket FIONBIO on");
    got = recv(cli, buf, sizeof buf, 0);
    check(got == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "non-blocking recv");
    nb = 0;
    check(ioctlsocket(cli, FIONBIO, &nb) == 0, "ioctlsocket FIONBIO off");

    check(shutdown(cli, SD_SEND) == 0, "shutdown send");
    check(recv(acc, buf, sizeof buf, 0) == 0, "recv EOF");

    /* UDP sendto/recvfrom on a second loopback port. */
    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    check(us != INVALID_SOCKET, "socket UDP");
    struct sockaddr_in ua = loopback(0);
    check(bind(us, (const struct sockaddr *)&ua, sizeof ua) == 0, "bind UDP");
    alen = sizeof ua;
    check(getsockname(us, (struct sockaddr *)&ua, &alen) == 0 && ntohs(ua.sin_port) != 0, "getsockname UDP");

    SOCKET uc = socket(AF_INET, SOCK_DGRAM, 0);
    check(uc != INVALID_SOCKET, "socket UDP client");
    struct sockaddr_in ca = loopback(0);
    check(bind(uc, (const struct sockaddr *)&ca, sizeof ca) == 0, "bind UDP client");
    alen = sizeof ca;
    check(getsockname(uc, (struct sockaddr *)&ca, &alen) == 0, "getsockname UDP client");

    check(sendto(uc, "dgram", 5, 0, (const struct sockaddr *)&ua, sizeof ua) == 5, "sendto");
    FD_ZERO(&rf);
    FD_SET(us, &rf);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    check(select(0, &rf, NULL, NULL, &tv) == 1 && FD_ISSET(us, &rf), "select UDP readable");
    struct sockaddr_in from;
    int flen = sizeof from;
    got = recvfrom(us, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
    check(got == 5 && memcmp(buf, "dgram", 5) == 0 && ntohs(from.sin_port) == ntohs(ca.sin_port), "recvfrom");

    check(closesocket(uc) == 0, "closesocket UDP client");
    check(closesocket(us) == 0, "closesocket UDP server");
    check(closesocket(acc) == 0, "closesocket accepted");
    check(closesocket(cli) == 0, "closesocket client");
    check(closesocket(srv) == 0, "closesocket server");
    check(WSACleanup() == 0, "WSACleanup");

    if (fails) {
        printf("winsock: %d failure(s)\n", fails);
        return 1;
    }
    printf("winsock: ok\n");
    return 0;
}
