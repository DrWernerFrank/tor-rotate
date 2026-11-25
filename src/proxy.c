/* proxy.c - minimal SOCKS5 (CONNECT only, no auth) -> tor SOCKS chain */
#include "proxy.h"
#include "state.h"
#include "sock.h"

/* --- tor socks connect: send request bytes for a target, return socket --- */
static int tor_connect_raw(const char *host_or_ip, int port)
{
    int s = sock_connect("127.0.0.1", g_cfg.socks_port, 8000);
    if (s < 0) return -1;

    /* greeting: no auth */
    unsigned char greet[3] = { 0x05, 0x01, 0x00 };
    if (sock_send_all((sock_t)s, greet, 3) < 0) { sock_close((sock_t)s); return -1; }
    unsigned char r[2];
    if (sock_recv_exact((sock_t)s, r, 2) != 2 || r[1] != 0x00) { sock_close((sock_t)s); return -1; }

    /* request: CONNECT, ATYP=domain (tor resolves; also covers IPs as domains) */
    char req[600];
    int hlen = (int)strlen(host_or_ip);
    if (hlen > 255) { sock_close((sock_t)s); return -1; }
    int i = 0;
    req[i++] = 0x05; req[i++] = 0x01; req[i++] = 0x00;
    req[i++] = 0x03;
    req[i++] = (char)hlen;
    memcpy(req + i, host_or_ip, (size_t)hlen); i += hlen;
    req[i++] = (char)((port >> 8) & 0xff);
    req[i++] = (char)(port & 0xff);

    if (sock_send_all((sock_t)s, req, i) < 0) { sock_close((sock_t)s); return -1; }

    /* reply: 4 bytes hdr then addr */
    unsigned char rep[4];
    if (sock_recv_exact((sock_t)s, rep, 4) != 4) { sock_close((sock_t)s); return -1; }
    if (rep[1] != 0x00) { sock_close((sock_t)s); return -2; }  /* tor refused */
    int extra = 0;
    switch (rep[3]) {
        case 0x01: extra = 4 + 2; break;               /* addr + port */
        case 0x03: { unsigned char l; sock_recv_exact((sock_t)s, &l, 1); extra = l + 2; break; }
        case 0x04: extra = 16 + 2; break;
        default:   extra = 6;
    }
    if (extra > 0) {
        unsigned char skip[20];
        if (sock_recv_exact((sock_t)s, skip, extra) != extra) { sock_close((sock_t)s); return -1; }
    }
    return s;
}

int proxy_probe(void)
{
    /* force tor to build a circuit now by opening one connection and closing */
    int s = tor_connect_raw("1.1.1.1", 80);
    if (s >= 0) sock_close((sock_t)s);
    return s >= 0 ? 0 : -1;
}

static THREAD_RET client_thread(void *arg)
{
    int csock = (int)(intptr_t)arg;
    int tsock = -1;
    char target[280] = "?";
    int tport = 0;

    g_app.requests++;
    state_request_hook();           /* may send NEWNYM before this request */

    /* greeting: VER NMETHODS METHODS — consume all methods offered */
    unsigned char b[2];
    if (sock_recv_exact((sock_t)csock, b, 2) != 2 || b[0] != 0x05) goto done;
    if (b[1] > 0) {
        unsigned char methods[255];
        if (sock_recv_exact((sock_t)csock, methods, b[1]) != b[1]) goto done;
    }
    unsigned char reply_greet[2] = { 0x05, 0x00 };
    if (sock_send_all((sock_t)csock, reply_greet, 2) < 0) goto done;

    /* request */
    unsigned char hdr[4];
    if (sock_recv_exact((sock_t)csock, hdr, 4) != 4) goto done;
    if (hdr[1] != 0x01) {           /* CONNECT only */
        unsigned char no[10] = { 0x05, 0x07, 0x00, 0x01, 0,0,0,0, 0,0 };
        sock_send_all((sock_t)csock, no, 10);
        goto done;
    }
    if (hdr[3] == 0x01) {
        unsigned char a[4];
        if (sock_recv_exact((sock_t)csock, a, 4) != 4) goto done;
        unsigned char p[2];
        if (sock_recv_exact((sock_t)csock, p, 2) != 2) goto done;
        tport = (p[0] << 8) | p[1];
        snprintf(target, sizeof target, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    } else if (hdr[3] == 0x03) {
        unsigned char l;
        if (sock_recv_exact((sock_t)csock, &l, 1) != 1 || l == 0) goto done;
        if (sock_recv_exact((sock_t)csock, target, l) != l) goto done;
        target[l] = 0;
        unsigned char p[2];
        if (sock_recv_exact((sock_t)csock, p, 2) != 2) goto done;
        tport = (p[0] << 8) | p[1];
    } else if (hdr[3] == 0x04) {
        unsigned char raw[18];
        if (sock_recv_exact((sock_t)csock, raw, 18) != 18) goto done;
        struct in6_addr a6;
        memcpy(&a6, raw, 16);
        char ip6[64];
        if (!inet_ntop(AF_INET6, &a6, ip6, sizeof ip6)) goto done;
        tport = (raw[16] << 8) | raw[17];
        str_copy(target, sizeof target, ip6);
    } else {
        goto done;
    }

    tsock = tor_connect_raw(target, tport);

    /* answer client */
    unsigned char ok[10]  = { 0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0 };
    unsigned char bad[10] = { 0x05, 0x05, 0x00, 0x01, 0,0,0,0, 0,0 };
    sock_send_all((sock_t)csock, tsock >= 0 ? ok : bad, 10);
    if (tsock < 0) goto done;

    sock_pump((sock_t)csock, (sock_t)tsock);

done:
    if (tsock >= 0) sock_close((sock_t)tsock);
    sock_close((sock_t)csock);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static THREAD_RET listener_thread(void *arg)
{
    int lsock = (int)(intptr_t)arg;
    while (g_app.running) {
        int c = sock_accept(lsock);
        if (c < 0) { msleep(50); continue; }
#ifdef _WIN32
        uintptr_t th = _beginthreadex(NULL, 0, client_thread, (void *)(intptr_t)c, 0, NULL);
        if (th) CloseHandle((HANDLE)th);
#else
        pthread_t th;
        pthread_create(&th, NULL, client_thread, (void *)(intptr_t)c);
        pthread_detach(th);
#endif
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int proxy_start(int listen_port)
{
    int l = sock_listen("127.0.0.1", listen_port);
    if (l < 0) {
        fprintf(stderr, "FATAL: cannot listen on 127.0.0.1:%d\n", listen_port);
        return -1;
    }
#ifdef _WIN32
    uintptr_t th = _beginthreadex(NULL, 0, listener_thread, (void *)(intptr_t)l, 0, NULL);
    if (th) CloseHandle((HANDLE)th);
#else
    pthread_t th;
    pthread_create(&th, NULL, listener_thread, (void *)(intptr_t)l);
    pthread_detach(th);
#endif
    log_line("socks5 listener on 127.0.0.1:%d", listen_port);
    return 0;
}
