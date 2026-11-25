/* ui.c - minimalistic console UI (single keypress commands) */
#include "state.h"
#include "proxy.h"
#include "sock.h"

#ifdef _WIN32
static void ui_vt_enable(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#else
static void ui_vt_enable(void) {}
#endif

static const char *HELP =
    "keys: [r] rotate now  [m] mode  [t] timer interval  [n] per-req batch\n"
    "      [e] list circuits  [p] pause  [h] help  [q] quit\n";

void ui_init(void) { ui_vt_enable(); }

#define UI_LINES 6   /* lines printed by ui_status */
static int redraw_valid = 0;

void ui_invalidate(void) { redraw_valid = 0; }

void ui_status(void)
{
    char fp[20];
    if (strlen(g_app.cur_exit.fp) > 16) {
        memcpy(fp, g_app.cur_exit.fp, 16);
        fp[16] = 0;
    } else
        str_copy(fp, sizeof fp, g_app.cur_exit.fp);

    /* redraw in place: move cursor back up over the previous block */
    if (redraw_valid) printf("\033[%dA\r\033[J", UI_LINES);
    redraw_valid = 1;

    printf("--------------------------------------------------------------\n");
    printf(" mode: %-12s interval: %ds  batch: %d  %s\n",
           state_mode_str(), g_app.timer_sec, g_app.rot_every_n,
           g_app.paused ? "[PAUSED]" : "");
    printf(" exit: %-16s %s  ip: %-15s cc: %s\n",
           g_app.cur_exit.nick[0] ? g_app.cur_exit.nick : "(none)",
           fp, g_app.cur_exit.ip[0] ? g_app.cur_exit.ip : "-",
           g_app.cur_exit.cc[0] ? g_app.cur_exit.cc : "-");
    printf(" requests: %ld   rotations: %ld   socks: 127.0.0.1:%d\n",
           g_app.requests, g_app.rotations, g_cfg.listen_port);
    printf("--------------------------------------------------------------\n");
    fflush(stdout);
}

static void cycle_mode(void)
{
    switch (g_app.mode) {
        case ROT_MANUAL:  state_set_mode(ROT_PER_REQ); break;
        case ROT_PER_REQ: state_set_mode(ROT_TIMER);   break;
        case ROT_TIMER:   state_set_mode(ROT_OFF);     break;
        default:          state_set_mode(ROT_MANUAL);  break;
    }
    log_line("mode -> %s", state_mode_str());
}

static void cycle_timer(void)
{
    int vals[] = { 60, 180, 300, 600, 1800, 3600 };
    int n = (int)(sizeof vals / sizeof vals[0]);
    for (int i = 0; i < n; i++) {
        if (g_app.timer_sec == vals[i]) {
            g_app.timer_sec = vals[(i + 1) % n];
            break;
        }
        if (i == n - 1) g_app.timer_sec = 60;
    }
    log_line("timer interval -> %ds", g_app.timer_sec);
}

static void cycle_batch(void)
{
    int vals[] = { 1, 5, 10, 25, 50 };
    int n = (int)(sizeof vals / sizeof vals[0]);
    for (int i = 0; i < n; i++) {
        if (g_app.rot_every_n == vals[i]) {
            g_app.rot_every_n = vals[(i + 1) % n];
            break;
        }
        if (i == n - 1) g_app.rot_every_n = 1;
    }
    log_line("per-request batch -> %d", g_app.rot_every_n);
}

static void list_circuits(void)
{
    exit_info_t exits[8];
    int n = ctrl_get_exits(&g_app.ctrl, exits, 8);
    if (n <= 0) { log_line("no built circuits"); return; }
    for (int i = 0; i < n; i++)
        printf("  %d. %s  %.16s...\n", i + 1, exits[i].nick, exits[i].fp);
}

/* returns 0 to continue, -1 to quit */
int ui_poll_key(void)
{
#ifdef _WIN32
    if (!_kbhit()) return 0;
    int k = _getch();
#else
    /* non-blocking stdin read (terminal set raw by main) */
    char c;
    int flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, flags | O_NONBLOCK);
    int n = (int)read(0, &c, 1);
    fcntl(0, F_SETFL, flags);
    if (n <= 0) return 0;
    int k = c;
#endif
    switch (k) {
        case 'r': {
            if (g_app.ctrl.s != SOCK_INVALID && ctrl_newnym(&g_app.ctrl) == 0) {
                g_app.rotations++;
                g_app.last_rotate_ts = now_sec();
                proxy_probe();
                log_line("manual rotation #%ld", g_app.rotations);
            }
            break;
        }
        case 'm': cycle_mode(); break;
        case 't': cycle_timer(); break;
        case 'n': cycle_batch(); break;
        case 'e': list_circuits(); break;
        case 'p': g_app.paused = !g_app.paused;
                  log_line("rotation %s", g_app.paused ? "paused" : "resumed"); break;
        case 'h': fputs(HELP, stdout); break;
        case 'q': case 'Q': case 3: return -1;
    }
    return 0;
}
