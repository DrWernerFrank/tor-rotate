/* proxy.h - local socks5 listener that chains into tor */
#ifndef PROXY_H
#define PROXY_H
#include "mini.h"

int  proxy_start(int listen_port);   /* spawns listener thread, returns 0 ok */
int  proxy_probe(void);              /* open+close one tor connection to force
                                        a fresh circuit build (used after NEWNYM) */

#endif
