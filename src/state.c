/* state.c */
#include "state.h"

app_state_t g_app;

void state_init(void)
{
    memset(&g_app, 0, sizeof g_app);
    g_app.ctrl.s = SOCK_INVALID;
    g_app.mode = ROT_MANUAL;
    g_app.rot_every_n = 1;
    g_app.timer_sec = 300;
    g_app.running = 1;
    log_init();
    ctrl_lock_init();
}

static int do_newnym(void)
{
    if (!g_app.tor_ready || g_app.ctrl.s == SOCK_INVALID) return -1;
    if (ctrl_newnym(&g_app.ctrl) != 0) return -1;
    g_app.rotations++;
    g_app.last_rotate_ts = now_sec();
    return 0;
}

void state_request_hook(void)
{
    if (g_app.paused || g_app.mode != ROT_PER_REQ) return;
    /* rotate when the batch counter wraps */
    if (g_app.rot_every_n <= 1 ||
        (g_app.requests % g_app.rot_every_n) == 0)
        do_newnym();
}

void state_set_mode(rot_mode_t m) { g_app.mode = m; }

const char *state_mode_str(void)
{
    if (g_app.paused) return "paused";
    switch (g_app.mode) {
        case ROT_OFF:     return "off";
        case ROT_PER_REQ: return "per-request";
        case ROT_TIMER:   return "timer";
        default:          return "manual";
    }
}
