/* worker.c - background thread: timer rotation + current-exit tracking */
#include "worker.h"
#include "state.h"
#include "proxy.h"
#include "sock.h"

static void refresh_exit(void)
{
    if (!g_app.tor_ready || g_app.ctrl.s == SOCK_INVALID) return;
    /* newest built circuit -> exit nick/fp */
    exit_info_t exits[8];
    int n = ctrl_get_exits(&g_app.ctrl, exits, 8);
    if (n > 0)
        g_app.cur_exit = exits[n - 1];

    /* resolve exit ip from consensus */
    if (g_app.cur_exit.fp[0]) {
        char ip[64];
        if (ctrl_resolve_ip(&g_app.ctrl, g_app.cur_exit.fp, ip, sizeof ip) == 0 && ip[0]) {
            str_copy(g_app.cur_exit.ip, sizeof g_app.cur_exit.ip, ip);
            /* country via geoip (only when GeoIPFile is configured) */
            if (g_cfg.geoip_path[0]) {
                char key[128], cc[16];
                snprintf(key, sizeof key, "ip-to-country/%s", ip);
                cc[0] = 0;
                if (ctrl_get_info(&g_app.ctrl, key, cc, sizeof cc) == 0) {
                    for (char *p = cc; *p; p++)
                        if (*p == '"') { *p = 0; break; }
                    if (strlen(cc) == 2 || strcmp(cc, "???") == 0)
                        str_copy(g_app.cur_exit.cc, sizeof g_app.cur_exit.cc, cc);
                }
            }
        }
    }
}

static void timer_rotate(void)
{
    if (g_app.paused || g_app.mode != ROT_TIMER) return;
    if (now_sec() - g_app.last_rotate_ts < g_app.timer_sec) return;
    if (g_app.ctrl.s != SOCK_INVALID && ctrl_newnym(&g_app.ctrl) == 0) {
        g_app.rotations++;
        g_app.last_rotate_ts = now_sec();
        proxy_probe();               /* push tor to build the new circuit */
        log_line("timer rotation #%ld done", g_app.rotations);
    }
}

static THREAD_RET worker_thread(void *arg)
{
    (void)arg;
    while (g_app.running) {
        timer_rotate();
        refresh_exit();
        msleep(1000);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void worker_start(void)
{
#ifdef _WIN32
    uintptr_t th = _beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    if (th) CloseHandle((HANDLE)th);
#else
    pthread_t th;
    pthread_create(&th, NULL, worker_thread, NULL);
    pthread_detach(th);
#endif
}
