/* web.h - local web UI (dashboard + JSON API) */
#ifndef WEB_H
#define WEB_H
int web_start(int port);   /* binds 127.0.0.1:port; 0 = disabled; returns 0 on success */
#endif
