/* web.c - local web UI: embedded dashboard page + small JSON API, 127.0.0.1 only */
#include <ctype.h>
#include "web.h"
#include "state.h"
#include "control.h"
#include "proxy.h"
#include "sock.h"

static const char INDEX_HTML[] =
"<!doctype html>\n"
"<html lang='en'>\n"
"<head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>tor_rotate</title>\n"
"<style>\n"
"body{font-family:system-ui,'Segoe UI',sans-serif;background:#111418;color:#e6e8eb;margin:0;padding:24px}\n"
"h1{font-size:20px;margin:0 0 16px;display:flex;align-items:center;gap:10px}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;margin-bottom:16px}\n"
".card{background:#1b1f25;border:1px solid #2a3038;border-radius:10px;padding:16px}\n"
".card h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:#8b95a3;margin:0 0 12px}\n"
".kv{display:flex;justify-content:space-between;gap:12px;padding:4px 0;font-size:14px}\n"
".kv span:first-child{color:#8b95a3}\n"
".mono{font-family:Consolas,monospace}\n"
"button{background:#2b6cb0;color:#fff;border:0;border-radius:6px;padding:8px 14px;font-size:14px;cursor:pointer;margin:4px 6px 4px 0}\n"
"button:hover{background:#3182ce}\n"
"button.alt{background:#2a3038}\n"
"button.alt:hover{background:#3a424c}\n"
"button.warn{background:#b7791f}\n"
"button.warn:hover{background:#d69e2e}\n"
"select{background:#111418;color:#e6e8eb;border:1px solid #2a3038;border-radius:6px;padding:6px;font-size:14px}\n"
".row{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:10px;font-size:14px}\n"
".row span:first-child{color:#8b95a3}\n"
"table{width:100%;border-collapse:collapse;font-size:13px}\n"
"td,th{text-align:left;padding:6px;border-bottom:1px solid #2a3038;font-family:Consolas,monospace}\n"
"th{color:#8b95a3;font-family:system-ui,sans-serif;font-weight:normal}\n"
"#log{background:#0b0d10;border-radius:8px;padding:10px;height:280px;overflow:auto;font-family:Consolas,monospace;font-size:12px;white-space:pre-wrap}\n"
".badge{display:inline-block;padding:2px 10px;border-radius:10px;background:#2a3038;font-size:12px;font-weight:normal}\n"
".badge.on{background:#276749}\n"
".badge.paused{background:#975a16}\n"
".badge.off{background:#742a2a}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<h1>tor_rotate <span id='ver' class='badge'></span> <span id='state' class='badge'>connecting</span></h1>\n"
"<div class='grid'>\n"
" <div class='card'>\n"
"  <h2>Exit</h2>\n"
"  <div class='kv'><span>nickname</span><span id='nick' class='mono'>-</span></div>\n"
"  <div class='kv'><span>fingerprint</span><span id='fp' class='mono'>-</span></div>\n"
"  <div class='kv'><span>ip</span><span id='ip' class='mono'>-</span></div>\n"
"  <div class='kv'><span>country</span><span id='cc' class='mono'>-</span></div>\n"
"  <div style='margin-top:12px'><button id='btn-rotate'>Rotate now</button><button id='btn-list' class='alt'>Show circuits</button></div>\n"
" </div>\n"
" <div class='card'>\n"
"  <h2>Rotation</h2>\n"
"  <div class='row'><span>mode</span><select id='mode'><option value='manual'>manual</option><option value='per'>per-request</option><option value='timer'>timer</option><option value='off'>off</option></select></div>\n"
"  <div class='row'><span>timer interval</span><select id='timer'></select></div>\n"
"  <div class='row'><span>per-request batch</span><select id='batch'></select></div>\n"
"  <div style='margin-top:8px'><button id='btn-pause' class='warn'>Pause</button></div>\n"
" </div>\n"
" <div class='card'>\n"
"  <h2>Stats</h2>\n"
"  <div class='kv'><span>requests</span><span id='req' class='mono'>0</span></div>\n"
"  <div class='kv'><span>rotations</span><span id='rot' class='mono'>0</span></div>\n"
"  <div class='kv'><span>socks5 listener</span><span id='socks' class='mono'>-</span></div>\n"
"  <div class='kv'><span>tor</span><span id='tor'>-</span></div>\n"
" </div>\n"
"</div>\n"
"<div class='card' style='margin-bottom:16px'>\n"
" <h2>Circuits</h2>\n"
" <table><thead><tr><th>#</th><th>nickname</th><th>fingerprint</th></tr></thead><tbody id='circ'><tr><td colspan='3'>press Show circuits</td></tr></tbody></table>\n"
"</div>\n"
"<div class='card'>\n"
" <h2>Log</h2>\n"
" <div id='log'></div>\n"
"</div>\n"
"<script>\n"
"var $ = function(id) { return document.getElementById(id); };\n"
"var TIMERS = [60, 180, 300, 600, 1800, 3600];\n"
"var BATCHES = [1, 5, 10, 25, 50];\n"
"function fill(sel, vals, suffix) {\n"
"  sel.textContent = '';\n"
"  vals.forEach(function(v) { addOpt(sel, v, suffix); });\n"
"}\n"
"function addOpt(sel, v, suffix) {\n"
"  var o = document.createElement('option');\n"
"  o.value = v; o.textContent = v + suffix; sel.appendChild(o);\n"
"}\n"
"fill($('timer'), TIMERS, ' s');\n"
"fill($('batch'), BATCHES, '');\n"
"function api(path, method) {\n"
"  return fetch(path, { method: method || 'GET', headers: { 'X-Tor-Rotate': '1' } }).then(function(r) { return r.json(); });\n"
"}\n"
"function render(s) {\n"
"  if (s.error) { alert(s.error); return; }\n"
"  $('ver').textContent = 'v' + s.version;\n"
"  var st = $('state');\n"
"  st.textContent = s.paused ? 'paused' : s.mode;\n"
"  st.className = 'badge ' + (s.paused ? 'paused' : (s.mode === 'off' ? 'off' : 'on'));\n"
"  $('nick').textContent = s.exit.nick || '(none)';\n"
"  $('fp').textContent = s.exit.fp ? s.exit.fp.substring(0, 16) + '...' : '-';\n"
"  $('ip').textContent = s.exit.ip || '-';\n"
"  $('cc').textContent = s.exit.cc || '-';\n"
"  $('req').textContent = s.requests;\n"
"  $('rot').textContent = s.rotations;\n"
"  $('socks').textContent = '127.0.0.1:' + s.socks_port;\n"
"  $('tor').textContent = s.tor_ready ? 'bootstrapped' : 'starting...';\n"
"  $('btn-pause').textContent = s.paused ? 'Resume' : 'Pause';\n"
"  if (document.activeElement !== $('mode')) $('mode').value = s.mode;\n"
"  if (document.activeElement !== $('timer')) {\n"
"    if (!$('timer').querySelector('option[value=\"' + s.timer_sec + '\"]')) addOpt($('timer'), s.timer_sec, ' s');\n"
"    $('timer').value = s.timer_sec;\n"
"  }\n"
"  if (document.activeElement !== $('batch')) {\n"
"    if (!$('batch').querySelector('option[value=\"' + s.batch + '\"]')) addOpt($('batch'), s.batch, '');\n"
"    $('batch').value = s.batch;\n"
"  }\n"
"  var log = $('log');\n"
"  var atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 20;\n"
"  log.textContent = s.logs.join(String.fromCharCode(10));\n"
"  if (atBottom) log.scrollTop = log.scrollHeight;\n"
"}\n"
"function act(path) {\n"
"  return api(path, 'POST').then(function(s) { render(s); return s; }).catch(function(e) { alert('request failed: ' + e); });\n"
"}\n"
"function renderCirc(s) {\n"
"  var tb = $('circ');\n"
"  tb.textContent = '';\n"
"  if (!s.circuits || !s.circuits.length) {\n"
"    var r0 = tb.insertRow(); var c0 = r0.insertCell(); c0.colSpan = 3; c0.textContent = 'no built circuits';\n"
"    return;\n"
"  }\n"
"  s.circuits.forEach(function(x, i) {\n"
"    var r = tb.insertRow();\n"
"    [i + 1, x.nick, x.fp].forEach(function(v) { r.insertCell().textContent = String(v); });\n"
"  });\n"
"}\n"
"function loadCirc() { return api('/api/circuits').then(renderCirc).catch(function(e) { alert('request failed: ' + e); }); }\n"
"$('btn-rotate').onclick = function() { act('/api/rotate').then(loadCirc); };\n"
"$('btn-pause').onclick = function() { act('/api/pause'); };\n"
"$('btn-list').onclick = loadCirc;\n"
"$('mode').onchange = function() { act('/api/mode?set=' + this.value); };\n"
"$('timer').onchange = function() { act('/api/timer?sec=' + this.value); };\n"
"$('batch').onchange = function() { act('/api/batch?n=' + this.value); };\n"
"function poll() {\n"
"  api('/api/status').then(render).catch(function() {\n"
"    var st = $('state'); st.textContent = 'offline'; st.className = 'badge';\n"
"  });\n"
"}\n"
"poll();\n"
"setInterval(poll, 1500);\n"
"</script>\n"
"</body>\n"
"</html>\n";

typedef struct {
    char *buf;
    int len;
    int cap;
} sbuf_t;

static sbuf_t sb_new(int cap)
{
    sbuf_t b;
    b.buf = (char *)malloc((size_t)cap);
    b.buf[0] = 0;
    b.len = 0;
    b.cap = cap;
    return b;
}

static void sb_put(sbuf_t *b, const char *s)
{
    while (*s && b->len < b->cap - 1) b->buf[b->len++] = *s++;
    b->buf[b->len] = 0;
}

static void sb_fmt(sbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int room = b->cap - b->len;
    if (room > 1) {
        int n = vsnprintf(b->buf + b->len, (size_t)room, fmt, ap);
        if (n > 0) b->len += (n < room ? n : room - 1);
    }
    va_end(ap);
}

static void sb_jstr(sbuf_t *b, const char *s)
{
    sb_put(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char esc[3] = { '\\', (char)c, 0 };
            sb_put(b, esc);
        } else if (c < 0x20) {
            sb_fmt(b, "\\u%04x", c);
        } else {
            char one[2] = { (char)c, 0 };
            sb_put(b, one);
        }
    }
    sb_put(b, "\"");
}

static const char *mode_key(void)
{
    switch (g_app.mode) {
        case ROT_OFF:     return "off";
        case ROT_PER_REQ: return "per";
        case ROT_TIMER:   return "timer";
        default:          return "manual";
    }
}

static void build_status(sbuf_t *b)
{
    sb_put(b, "{\"version\":");
    sb_jstr(b, ROT_VERSION);
    sb_put(b, ",\"mode\":");
    sb_jstr(b, mode_key());
    sb_fmt(b, ",\"paused\":%s", g_app.paused ? "true" : "false");
    sb_fmt(b, ",\"timer_sec\":%d,\"batch\":%d,\"requests\":%ld,\"rotations\":%ld,\"socks_port\":%d,\"tor_ready\":%s",
           g_app.timer_sec, g_app.rot_every_n, g_app.requests, g_app.rotations,
           g_cfg.listen_port, g_app.tor_ready ? "true" : "false");
    sb_put(b, ",\"exit\":{\"nick\":");
    sb_jstr(b, g_app.cur_exit.nick);
    sb_put(b, ",\"fp\":");
    sb_jstr(b, g_app.cur_exit.fp);
    sb_put(b, ",\"ip\":");
    sb_jstr(b, g_app.cur_exit.ip);
    sb_put(b, ",\"cc\":");
    sb_jstr(b, g_app.cur_exit.cc);
    sb_put(b, "}");

    sb_put(b, ",\"logs\":[");
    int n = log_count();
    int start = n > 40 ? n - 40 : 0;
    for (int i = start; i < n; i++) {
        char line[256];
        if (log_get(i, line, sizeof line) != 0) continue;
        if (i > start) sb_put(b, ",");
        sb_jstr(b, line);
    }
    sb_put(b, "]}");
}

static void send_resp(sock_t c, int code, const char *text, const char *ctype,
                      const char *body, int blen)
{
    char hdr[320];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     code, text, ctype, blen);
    sock_send_all(c, hdr, n);
    if (blen > 0) sock_send_all(c, body, blen);
}

static void send_json(sock_t c, int code, const char *text, sbuf_t *b)
{
    send_resp(c, code, text, "application/json", b->buf, b->len);
}

static void send_error(sock_t c, int code, const char *text, const char *msg)
{
    sbuf_t b = sb_new(512);
    sb_put(&b, "{\"error\":");
    sb_jstr(&b, msg);
    sb_put(&b, "}");
    send_json(c, code, text, &b);
    free(b.buf);
}

static void reply_status(sock_t c, int rc, const char *err)
{
    if (rc != 0) { send_error(c, 400, "Bad Request", err); return; }
    sbuf_t b = sb_new(32768);
    build_status(&b);
    send_json(c, 200, "OK", &b);
    free(b.buf);
}

static int qparam(const char *q, const char *key, char *out, int max)
{
    size_t kl = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1;
            int i = 0;
            while (v[i] && v[i] != '&' && i < max - 1) { out[i] = v[i]; i++; }
            out[i] = 0;
            return 0;
        }
        const char *amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return -1;
}

static int web_rotate(void)
{
    if (!g_app.tor_ready || g_app.ctrl.s == SOCK_INVALID) return -1;
    if (ctrl_newnym(&g_app.ctrl) != 0) return -1;
    g_app.rotations++;
    g_app.last_rotate_ts = now_sec();
    proxy_probe();
    log_line("manual rotation #%ld (web)", g_app.rotations);
    return 0;
}

static int host_is_local(const char *v)
{
    char h[64];
    int i = 0;
    while (v[i] && v[i] != ':' && v[i] != '\r' && i < (int)sizeof h - 1) { h[i] = v[i]; i++; }
    h[i] = 0;
    return !strcmp(h, "127.0.0.1") || !strcmp(h, "localhost");
}

static const char *hdr_value(const char *line, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++)
        if (!line[i] || tolower((unsigned char)line[i]) != tolower((unsigned char)name[i]))
            return NULL;
    if (line[n] != ':') return NULL;
    const char *v = line + n + 1;
    while (*v == ' ') v++;
    return v;
}

static void handle_request(sock_t c, const char *method, const char *target,
                           int local_host, int has_xreq)
{
    char path[128] = "";
    char query[256] = "";
    const char *qm = strchr(target, '?');
    size_t pl = qm ? (size_t)(qm - target) : strlen(target);
    if (pl >= sizeof path) pl = sizeof path - 1;
    memcpy(path, target, pl);
    path[pl] = 0;
    if (qm) str_copy(query, sizeof query, qm + 1);

    int get = !strcmp(method, "GET");
    int post = !strcmp(method, "POST");
    if (!local_host) { send_error(c, 403, "Forbidden", "host must be localhost"); return; }
    if (!get && !post) { send_error(c, 405, "Method Not Allowed", "use GET or POST"); return; }
    if (post && !has_xreq) { send_error(c, 403, "Forbidden", "missing X-Tor-Rotate header"); return; }

    if (get && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
        send_resp(c, 200, "OK", "text/html; charset=utf-8", INDEX_HTML, (int)sizeof INDEX_HTML - 1);
        return;
    }
    if (get && !strcmp(path, "/api/status")) {
        reply_status(c, 0, NULL);
        return;
    }
    if (get && !strcmp(path, "/api/circuits")) {
        exit_info_t ex[8];
        int n = ctrl_get_exits(&g_app.ctrl, ex, 8);
        sbuf_t b = sb_new(4096);
        sb_put(&b, "{\"circuits\":[");
        for (int i = 0; i < n; i++) {
            if (i) sb_put(&b, ",");
            sb_put(&b, "{\"nick\":");
            sb_jstr(&b, ex[i].nick);
            sb_put(&b, ",\"fp\":");
            sb_jstr(&b, ex[i].fp);
            sb_put(&b, "}");
        }
        sb_put(&b, "]}");
        send_json(c, 200, "OK", &b);
        free(b.buf);
        return;
    }
    if (post && !strcmp(path, "/api/rotate")) {
        reply_status(c, web_rotate() == 0 ? 0 : -1, "rotation failed: tor not ready");
        return;
    }
    if (post && !strcmp(path, "/api/pause")) {
        g_app.paused = !g_app.paused;
        log_line("rotation %s (web)", g_app.paused ? "paused" : "resumed");
        reply_status(c, 0, NULL);
        return;
    }
    if (post && !strcmp(path, "/api/mode")) {
        char v[16];
        if (qparam(query, "set", v, sizeof v) != 0) { reply_status(c, -1, "missing set"); return; }
        rot_mode_t m;
        if (!strcmp(v, "manual"))      m = ROT_MANUAL;
        else if (!strcmp(v, "per"))    m = ROT_PER_REQ;
        else if (!strcmp(v, "timer"))  m = ROT_TIMER;
        else if (!strcmp(v, "off"))    m = ROT_OFF;
        else { reply_status(c, -1, "mode must be manual|per|timer|off"); return; }
        state_set_mode(m);
        log_line("mode -> %s (web)", state_mode_str());
        reply_status(c, 0, NULL);
        return;
    }
    if (post && !strcmp(path, "/api/timer")) {
        char v[16];
        int sec = qparam(query, "sec", v, sizeof v) == 0 ? atoi(v) : 0;
        if (sec < 10 || sec > 86400) { reply_status(c, -1, "sec must be 10..86400"); return; }
        g_app.timer_sec = sec;
        log_line("timer interval -> %ds (web)", sec);
        reply_status(c, 0, NULL);
        return;
    }
    if (post && !strcmp(path, "/api/batch")) {
        char v[16];
        int n = qparam(query, "n", v, sizeof v) == 0 ? atoi(v) : 0;
        if (n < 1 || n > 1000) { reply_status(c, -1, "n must be 1..1000"); return; }
        g_app.rot_every_n = n;
        log_line("per-request batch -> %d (web)", n);
        reply_status(c, 0, NULL);
        return;
    }
    send_error(c, 404, "Not Found", "no such endpoint");
}

static THREAD_RET conn_thread(void *arg)
{
    sock_t c = (sock_t)(intptr_t)arg;
#ifdef _WIN32
    DWORD tmo = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
#else
    struct timeval tv = { 5, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif

    char line[1024];
    char method[8], target[512];
    if (sock_readline(c, line, sizeof line) <= 0 ||
        sscanf(line, "%7s %511s", method, target) != 2) {
        goto done;
    }

    int local_host = 0, has_xreq = 0;
    for (;;) {
        char hl[512];
        int n = sock_readline(c, hl, sizeof hl);
        if (n < 0) goto done;
        if (n == 0) break;
        const char *v = hdr_value(hl, "Host");
        if (v) local_host = host_is_local(v);
        else if (hdr_value(hl, "X-Tor-Rotate")) has_xreq = 1;
    }

    handle_request(c, method, target, local_host, has_xreq);

done:
    sock_close(c);
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
        uintptr_t th = _beginthreadex(NULL, 0, conn_thread, (void *)(intptr_t)c, 0, NULL);
        if (th) CloseHandle((HANDLE)th);
#else
        pthread_t th;
        pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)c);
        pthread_detach(th);
#endif
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int web_start(int port)
{
    if (port <= 0) return 0;
    int l = sock_listen("127.0.0.1", port);
    if (l < 0) {
        log_line("web ui: cannot listen on 127.0.0.1:%d", port);
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
    log_line("web ui on http://127.0.0.1:%d", port);
    return 0;
}
