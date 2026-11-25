/* sock.c - tiny TCP abstraction (win32 + posix) */
#include "sock.h"

#ifdef _WIN32
typedef int socklen_t;
#endif

int sock_init(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN);
    return 0;
#endif
}

int sock_connect(const char *host, int port, int timeout_ms)
{
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    sock_t s = socket(res->ai_family, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) { freeaddrinfo(res); return -1; }

    /* non-blocking connect with timeout */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        fd_set wset;
        struct timeval tv;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select((int)s + 1, NULL, &wset, NULL, &tv) <= 0) {
            closesocket(s);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
        if (err != 0) { closesocket(s); return -1; }
    }
#ifdef _WIN32
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fl);
#endif
    return (int)s;
}

int sock_listen(const char *bind_addr, int port)
{
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = bind_addr && *bind_addr
        ? inet_addr(bind_addr) : htonl(INADDR_LOOPBACK);
    if (sa.sin_addr.s_addr == INADDR_NONE && bind_addr && *bind_addr)
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) { closesocket(s); return -1; }
    if (listen(s, 16) != 0) { closesocket(s); return -1; }
    return (int)s;
}

int sock_accept(int lsock)
{
    struct sockaddr_in sa;
#ifdef _WIN32
    int slen = (int)sizeof sa;
#else
    socklen_t slen = sizeof sa;
#endif
    return (int)accept((sock_t)lsock, (struct sockaddr *)&sa, &slen);
}

int sock_send_all(sock_t s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

int sock_recv(sock_t s, void *buf, int len)
{
    return (int)recv(s, buf, len, 0);
}

int sock_recv_exact(sock_t s, void *buf, int len)
{
    char *p = (char *)buf;
    int got = 0;
    while (got < len) {
        int n = recv(s, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return got;
}

int sock_readline(sock_t s, char *buf, int maxlen)
{
    int i = 0;
    while (i < maxlen - 1) {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = 0;
    return i;
}

void sock_close(sock_t s)
{
    if (s != SOCK_INVALID) {
#ifdef _WIN32
        shutdown(s, SD_BOTH);
#else
        shutdown(s, SHUT_RDWR);
#endif
        closesocket(s);
    }
}

int sock_pump(sock_t a, sock_t b)
{
    char buf[16384];
    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(a, &rs);
        FD_SET(b, &rs);
        int mx = (a > b ? a : b) + 1;
        if (select(mx, &rs, NULL, NULL, NULL) <= 0) continue;
        if (FD_ISSET(a, &rs)) {
            int n = recv(a, buf, sizeof buf, 0);
            if (n <= 0) break;
            if (sock_send_all(b, buf, n) < 0) break;
        }
        if (FD_ISSET(b, &rs)) {
            int n = recv(b, buf, sizeof buf, 0);
            if (n <= 0) break;
            if (sock_send_all(a, buf, n) < 0) break;
        }
    }
    return 0;
}
