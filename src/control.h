/* control.h - tor control-port client */
#ifndef CONTROL_H
#define CONTROL_H
#include "mini.h"

typedef struct {
    sock_t s;
    char cookie_hex[128];
    int  authenticated;
} ctrl_t;

typedef struct {
    char fp[64];       /* exit fingerprint (no $) */
    char nick[64];     /* exit nickname */
    char ip[48];       /* exit ip (best effort) */
    char cc[4];        /* country code (best effort, may be empty) */
} exit_info_t;

int  ctrl_connect(ctrl_t *c, int port);
int  ctrl_auth(ctrl_t *c, const char *datadir);  /* cookie or NULL auth */
void ctrl_close(ctrl_t *c);

/* raw command; returns 0 on 250-type success, fills reply (may be NULL) */
int  ctrl_cmd(ctrl_t *c, const char *cmd, char *reply, int reply_max);

/* convenience wrappers */
int  ctrl_newnym(ctrl_t *c);                       /* SIGNAL NEWNYM */
int  ctrl_get_bootstrap(ctrl_t *c, char *buf, int max);
int  ctrl_get_exits(ctrl_t *c, exit_info_t *out, int max); /* built circuits */
int  ctrl_set_exit_policy(ctrl_t *c, const char *include, const char *exclude);
int  ctrl_get_info(ctrl_t *c, const char *key, char *val, int vmax);
int  ctrl_resolve_ip(ctrl_t *c, const char *fp, char *ip, int ipmax);

#endif
