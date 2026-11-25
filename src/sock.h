/* sock.h - tiny TCP abstraction */
#ifndef SOCK_H
#define SOCK_H
#include "mini.h"

int  sock_init(void);                          /* WSAStartup on win32 */
int  sock_connect(const char *host, int port, int timeout_ms);
int  sock_listen(const char *bind_addr, int port);
int  sock_accept(int lsock);                   /* blocking accept */
int  sock_send_all(sock_t s, const void *buf, int len);
int  sock_recv(sock_t s, void *buf, int len);  /* single read */
int  sock_recv_exact(sock_t s, void *buf, int len); /* loop until len or fail */
int  sock_readline(sock_t s, char *buf, int maxlen); /* 0..n-1 chars, no \n */
void sock_close(sock_t s);
int  sock_pump(sock_t a, sock_t b);            /* bidirectional until EOF */

#endif
