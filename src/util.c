/* util.c - small helpers */
#include "mini.h"

config_t g_cfg;
static int g_quiet = 0;

#define LOG_RING 64
#define LOG_LINE 256
static char g_logring[LOG_RING][LOG_LINE];
static int  g_logn = 0;      /* lines stored, capped at LOG_RING */
static int  g_logpos = 0;    /* next write slot */
static mutex_t g_logmx;

void log_set_quiet(int q) { g_quiet = q; }

void log_init(void) { mutex_init(&g_logmx); }

void ui_invalidate(void);   /* ui.c */

void log_line(const char *fmt, ...)
{
    if (g_quiet) return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);

    mutex_lock(&g_logmx);
    snprintf(g_logring[g_logpos], LOG_LINE, "[%s] %s", ts, msg);
    g_logpos = (g_logpos + 1) % LOG_RING;
    if (g_logn < LOG_RING) g_logn++;
    mutex_unlock(&g_logmx);

    ui_invalidate();            /* status block must redraw fresh below the log line */
}

int log_count(void)
{
    mutex_lock(&g_logmx);
    int n = g_logn;
    mutex_unlock(&g_logmx);
    return n;
}

int log_get(int idx, char *buf, int buflen)
{
    mutex_lock(&g_logmx);
    int ok = idx >= 0 && idx < g_logn;
    if (ok) {
        int first = (g_logpos - g_logn + LOG_RING) % LOG_RING;
        str_copy(buf, (size_t)buflen, g_logring[(first + idx) % LOG_RING]);
    }
    mutex_unlock(&g_logmx);
    return ok ? 0 : -1;
}

long now_sec(void) { return (long)time(NULL); }

void str_copy(char *dst, size_t n, const char *src)
{
    if (!dst || !n) return;
    if (!src) src = "";
    size_t i = 0;
    while (src[i] && i + 1 < n) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int str_has(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

int str_icontains(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    for (; *hay; hay++) {
        const char *h = hay, *nd = needle;
        while (*h && *nd && tolower((unsigned char)*h) == tolower((unsigned char)*nd)) { h++; nd++; }
        if (!*nd) return 1;
    }
    return 0;
}

void msleep(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

void ensure_dir(const char *path)
{
    if (!path || !*path) return;
    char tmp[512];
    str_copy(tmp, sizeof tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = c;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

void make_abs(char *buf, size_t n, const char *path)
{
    if (!path || !*path) { if (n) buf[0] = 0; return; }
#ifdef _WIN32
    DWORD r = GetFullPathNameA(path, (DWORD)n, buf, NULL);
    if (r == 0 || r >= n) str_copy(buf, n, path);
#else
    if (path[0] == '/') { str_copy(buf, n, path); return; }
    char cwd[512];
    if (getcwd(cwd, sizeof cwd)) {
        snprintf(buf, n, "%s/%s", cwd, path);
    } else {
        str_copy(buf, n, path);
    }
#endif
}
