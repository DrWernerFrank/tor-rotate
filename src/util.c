/* util.c - small helpers */
#include "mini.h"

config_t g_cfg;
static int g_quiet = 0;

void log_set_quiet(int q) { g_quiet = q; }

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
    va_list ap;
    printf("[%s] ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    ui_invalidate();            /* status block must redraw fresh below the log line */
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
