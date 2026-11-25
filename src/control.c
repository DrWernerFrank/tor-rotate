/* control.c - minimal tor control protocol client */
#include "control.h"
#include "sock.h"

static int read_reply(ctrl_t *c, char *reply, int reply_max)
{
    char line[1024];
    int ok = 0, reply_len = 0;

    for (;;) {
        if (sock_readline(c->s, line, sizeof line) < 0) return -1;
        if (strlen(line) < 3) continue;
        int code = atoi(line);
        char sp = line[3];

        if (sp == '+') {                       /* multi-line value */
            const char *key = line + 4;
            if (reply && reply_len < reply_max)
                reply_len += snprintf(reply + reply_len, (size_t)(reply_max - reply_len),
                                      "%s\n", key);
            for (;;) {
                if (sock_readline(c->s, line, sizeof line) < 0) return -1;
                if (strcmp(line, ".") == 0) break;
                if (reply && reply_len < reply_max) {
                    int n = snprintf(reply + reply_len, (size_t)(reply_max - reply_len),
                                     "%s\n", line);
                    if (n > 0) reply_len += n;
                }
            }
        } else {
            const char *rest = line + 4;
            /* skip appending the bare "OK" final status line */
            if (reply && *rest && strcmp(rest, "OK") != 0 && reply_len < reply_max) {
                int n = snprintf(reply + reply_len, (size_t)(reply_max - reply_len),
                                 "%s\n", rest);
                if (n > 0) reply_len += n;
            }
        }

        if (sp == ' ') {                       /* final line of this reply */
            ok = (code >= 200 && code < 300);
            break;
        }
        /* 650- events: keep reading; they end with 650 <sp> */
    }
    if (reply && reply_len < reply_max) reply[reply_len] = 0;
    else if (reply && reply_max > 0) reply[reply_max - 1] = 0;
    return ok ? 0 : -1;
}

int ctrl_connect(ctrl_t *c, int port)
{
    memset(c, 0, sizeof *c);
    c->s = SOCK_INVALID;
    char host[16] = "127.0.0.1";
    int s = sock_connect(host, port, 5000);
    if (s < 0) return -1;
    c->s = (sock_t)s;
    return 0;
}

static int read_file_bytes(const char *path, unsigned char *out, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(out, 1, (size_t)max, f);
    fclose(f);
    return n;
}

int ctrl_auth(ctrl_t *c, const char *datadir)
{
    unsigned char cookie[64];
    char path[768];
    int n = -1;

    if (datadir && *datadir) {
        snprintf(path, sizeof path, "%s/control_auth_cookie", datadir);
        n = read_file_bytes(path, cookie, sizeof cookie);
    }
    if (n > 0) {
        char hex[129];
        for (int i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", cookie[i]);
        hex[n * 2] = 0;
        char cmd[192];
        snprintf(cmd, sizeof cmd, "AUTHENTICATE %s", hex);
        if (ctrl_cmd(c, cmd, NULL, 0) == 0) { c->authenticated = 1; return 0; }
    }
    /* fallback: null authentication */
    if (ctrl_cmd(c, "AUTHENTICATE", NULL, 0) == 0) { c->authenticated = 1; return 0; }
    return -1;
}

void ctrl_close(ctrl_t *c)
{
    if (c->authenticated) ctrl_cmd(c, "QUIT", NULL, 0);
    sock_close(c->s);
    c->s = SOCK_INVALID;
    c->authenticated = 0;
}

int ctrl_cmd(ctrl_t *c, const char *cmd, char *reply, int reply_max)
{
    if (c->s == SOCK_INVALID) return -1;
    char full[2048];
    snprintf(full, sizeof full, "%s\r\n", cmd);
    if (sock_send_all(c->s, full, (int)strlen(full)) < 0) return -1;
    return read_reply(c, reply, reply_max);
}

int ctrl_newnym(ctrl_t *c)
{
    return ctrl_cmd(c, "SIGNAL NEWNYM", NULL, 0);
}

int ctrl_get_bootstrap(ctrl_t *c, char *buf, int max)
{
    return ctrl_get_info(c, "status/bootstrap-phase", buf, max);
}

int ctrl_get_info(ctrl_t *c, const char *key, char *val, int vmax)
{
    char reply[16384];
    reply[0] = 0;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "GETINFO %s", key);
    if (ctrl_cmd(c, cmd, reply, sizeof reply) != 0) return -1;
    /* first line after the key */
    char *p = strstr(reply, key);
    if (!p) return -1;
    p += strlen(key);
    if (*p == '=') p++;
    while (*p == '\n' || *p == ' ') p++;
    if (*p == '"') p++;
    str_copy(val, (size_t)vmax, p);
    /* trim trailing quote/newlines */
    for (char *e = val + strlen(val) - 1; e >= val && (*e == '"' || *e == '\n'); e--) *e = 0;
    return 0;
}

/* parse one circuit-status blob into exit list (newest last).
   Prefers PURPOSE=GENERAL circuits; falls back to any BUILT. */
int ctrl_get_exits(ctrl_t *c, exit_info_t *out, int max)
{
    char blob[32768];
    if (ctrl_get_info(c, "circuit-status", blob, sizeof blob) != 0) return 0;

    int count = 0;
    char blob2[32768];
    str_copy(blob2, sizeof blob2, blob);          /* strtok_r destroys the string */
    for (int pass = 0; pass < 2 && count == 0; pass++) {
        const char *filter = (pass == 0) ? "PURPOSE=GENERAL" : NULL;
        char *src = (pass == 0) ? blob : blob2;
        char *save = NULL;
        for (char *line = strtok_r(src, "\n", &save); line && count < max;
             line = strtok_r(NULL, "\n", &save)) {
            if (!str_has(line, " BUILT ") && !str_has(line, "BUILT ")) continue;
            if (filter && !str_has(line, filter)) continue;
            /* find node list: starts at first '$' */
            char *nodes = strchr(line, '$');
            if (!nodes) continue;
            char nodelist[1024];
            int i = 0;
            while (nodes[i] && nodes[i] != ' ' && i < (int)sizeof nodelist - 1) {
                nodelist[i] = nodes[i]; i++;
            }
            nodelist[i] = 0;
            /* last element = exit */
            char *last = strrchr(nodelist, ',');
            last = last ? last + 1 : nodelist;
            exit_info_t *e = &out[count++];
            memset(e, 0, sizeof *e);
            char *fp = last + 1;                       /* skip '$' */
            char *sep = strchr(last, '~');
            if (!sep) sep = strchr(last, '=');
            if (sep) {
                size_t fl = (size_t)(sep - fp);
                if (fl >= sizeof e->fp) fl = sizeof e->fp - 1;
                memcpy(e->fp, fp, fl);
                str_copy(e->nick, sizeof e->nick, sep + 1);
            } else {
                str_copy(e->fp, sizeof e->fp, fp);
            }
        }
    }
    return count;
}

/* resolve an exit's IP from the consensus: GETINFO ns/id/<fp> */
int ctrl_resolve_ip(ctrl_t *c, const char *fp, char *ip, int ipmax)
{
    char reply[4096];
    reply[0] = 0;
    char cmd[128];
    snprintf(cmd, sizeof cmd, "GETINFO ns/id/%s", fp);
    if (ctrl_cmd(c, cmd, reply, sizeof reply) != 0) return -1;

    /* the "r" line: r nick hash b64 date time IP port 0 */
    char *r = strstr(reply, "\nr ");
    if (r) r += 3;
    else if (strncmp(reply, "r ", 2) == 0) r = reply + 2;
    else return -1;

    /* fields: nick hash base64desc date time IP port */
    char nick[64], hash[128], b64[128], date[32], t[32], ipbuf[64];
    if (sscanf(r, "%63s %127s %127s %31s %31s %63s", nick, hash, b64, date, t, ipbuf) != 6)
        return -1;
    str_copy(ip, (size_t)ipmax, ipbuf);
    return 0;
}

int ctrl_set_exit_policy(ctrl_t *c, const char *include, const char *exclude)
{
    int ok = 1;
    char cmd[512];

    if (include && *include) {
        snprintf(cmd, sizeof cmd, "SETCONF ExitNodes=\"%s\"", include);
        if (ctrl_cmd(c, cmd, NULL, 0) != 0) ok = 0;
    } else {
        if (ctrl_cmd(c, "SETCONF ExitNodes", NULL, 0) != 0) ok = 0; /* reset to default */
    }
    if (exclude && *exclude) {
        snprintf(cmd, sizeof cmd, "SETCONF ExcludeNodes=\"%s\" StrictNodes=1", exclude);
        if (ctrl_cmd(c, cmd, NULL, 0) != 0) ok = 0;
    } else {
        if (ctrl_cmd(c, "SETCONF ExcludeNodes", NULL, 0) != 0) ok = 0;
    }
    /* force new circuits under the new policy */
    ctrl_newnym(c);
    return ok ? 0 : -1;
}
