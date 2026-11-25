/* main.c - tor_rotate: IP rotation on top of tor, runs layered over vpns/xray */
#include "mini.h"
#include "state.h"
#include "sock.h"
#include "control.h"
#include "torproc.h"
#include "proxy.h"
#include "worker.h"
#include "ui.h"
#include "web.h"

static void usage(void)
{
    printf(
    "tor_rotate " ROT_VERSION " - tor exit-node IP rotator (socks5 front-end)\n\n"
    "usage: tor_rotate [options]\n"
    "  --tor PATH        path to tor executable (default: .\\tor\\tor.exe / 'tor')\n"
    "  --datadir DIR     tor data dir (default: .\\data)\n"
    "  --socks N         tor socks port        (default 9050)\n"
    "  --control N       tor control port      (default 9051)\n"
    "  --listen N        our socks5 port       (default 1080)\n"
    "  --web N           web ui port on 127.0.0.1 (default 8080)\n"
    "  --no-web          disable the web ui\n"
    "  --upstream H:P    tor dials out through this socks5 (e.g. xray 127.0.0.1:10808)\n"
    "  --include CCS     only use these exits, e.g. {de},{nl},{se}\n"
    "  --exclude CCS     never use these exits, e.g. {us},{gb}\n"
    "  --mode MODE       manual|per|timer|off (default manual)\n"
    "  --timer SEC       rotation interval for timer mode (default 300)\n"
    "  --batch N         requests per rotation in per mode (default 1)\n"
    "  --quiet           suppress log lines\n"
    "  -h                this help\n\n"
    "chain: app -> 127.0.0.1:LISTEN -> tor socks -> [xray/VPN] -> exit\n"
    "point any socks5 client at 127.0.0.1:LISTEN.\n");
}

static void defaults(void)
{
#ifdef _WIN32
    str_copy(g_cfg.tor_path, sizeof g_cfg.tor_path, ".\\tor\\tor.exe");
#else
    str_copy(g_cfg.tor_path, sizeof g_cfg.tor_path, "/usr/bin/tor");
#endif
    str_copy(g_cfg.data_dir, sizeof g_cfg.data_dir, "data");
    g_cfg.socks_port = 9050;
    g_cfg.control_port = 9051;
    g_cfg.listen_port = 1080;
    g_cfg.web_port = 8080;
    g_cfg.mode = ROT_MANUAL;
    g_cfg.timer_sec = 300;
    g_cfg.rot_every_n = 1;
}

static void load_bridges_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "FATAL: cannot open bridges file %s\n", path); exit(1); }
    char line[512];
    while (fgets(line, sizeof line, f) && g_cfg.n_bridges < 16) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (*s == ' ') s++;
        if (!*s || *s == '#') continue;
        str_copy(g_cfg.bridge_lines[g_cfg.n_bridges], 256, s);
        g_cfg.n_bridges++;
    }
    fclose(f);
}

/* parse "mode" words */
static rot_mode_t parse_mode(const char *s)
{
    if (!strcmp(s, "per")) return ROT_PER_REQ;
    if (!strcmp(s, "timer")) return ROT_TIMER;
    if (!strcmp(s, "off")) return ROT_OFF;
    return ROT_MANUAL;
}

int main(int argc, char **argv)
{
    defaults();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : "";
        if      (!strcmp(a, "--tor"))      str_copy(g_cfg.tor_path, sizeof g_cfg.tor_path, next), i++;
        else if (!strcmp(a, "--datadir"))  str_copy(g_cfg.data_dir, sizeof g_cfg.data_dir, next), i++;
        else if (!strcmp(a, "--socks"))    g_cfg.socks_port = atoi(next), i++;
        else if (!strcmp(a, "--control"))  g_cfg.control_port = atoi(next), i++;
        else if (!strcmp(a, "--listen"))   g_cfg.listen_port = atoi(next), i++;
        else if (!strcmp(a, "--web"))      g_cfg.web_port = atoi(next), i++;
        else if (!strcmp(a, "--no-web"))   g_cfg.web_port = 0;
        else if (!strcmp(a, "--upstream")) str_copy(g_cfg.upstream_proxy, sizeof g_cfg.upstream_proxy, next), i++;
        else if (!strcmp(a, "--include"))  str_copy(g_cfg.exit_include, sizeof g_cfg.exit_include, next), i++;
        else if (!strcmp(a, "--exclude"))  str_copy(g_cfg.exit_exclude, sizeof g_cfg.exit_exclude, next), i++;
        else if (!strcmp(a, "--bridge")) {
            if (g_cfg.n_bridges < 16) {
                str_copy(g_cfg.bridge_lines[g_cfg.n_bridges], 256, next);
                g_cfg.n_bridges++;
            }
            i++;
        }
        else if (!strcmp(a, "--bridges-file")) str_copy(g_cfg.bridges_file, sizeof g_cfg.bridges_file, next), i++;
        else if (!strcmp(a, "--ptdir"))    str_copy(g_cfg.ptdir, sizeof g_cfg.ptdir, next), i++;
        else if (!strcmp(a, "--geoip"))    str_copy(g_cfg.geoip_path, sizeof g_cfg.geoip_path, next), i++;
        else if (!strcmp(a, "--mode"))     g_cfg.mode = parse_mode(next), i++;
        else if (!strcmp(a, "--timer"))    g_cfg.timer_sec = atoi(next), i++;
        else if (!strcmp(a, "--batch"))    g_cfg.rot_every_n = atoi(next), i++;
        else if (!strcmp(a, "--quiet"))    log_set_quiet(1);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else { printf("unknown option %s\n", a); usage(); return 1; }
    }
    if (g_cfg.rot_every_n < 1) g_cfg.rot_every_n = 1;
    if (g_cfg.timer_sec < 10) g_cfg.timer_sec = 10;

    /* default PT dir = tor executable's directory */
    if (!g_cfg.ptdir[0]) {
        str_copy(g_cfg.ptdir, sizeof g_cfg.ptdir, g_cfg.tor_path);
        char *slash = strrchr(g_cfg.ptdir, '/');
        char *bslash = strrchr(g_cfg.ptdir, '\\');
        char *cut = slash > bslash ? slash : bslash;
        if (cut) *cut = 0;
        else str_copy(g_cfg.ptdir, sizeof g_cfg.ptdir, ".");
    }
    /* absolute paths everywhere: tor and CreateProcess mis-resolve relative ones */
    make_abs(g_cfg.tor_path, sizeof g_cfg.tor_path, g_cfg.tor_path);
    make_abs(g_cfg.data_dir, sizeof g_cfg.data_dir, g_cfg.data_dir);
    make_abs(g_cfg.ptdir, sizeof g_cfg.ptdir, g_cfg.ptdir);
    if (g_cfg.geoip_path[0]) make_abs(g_cfg.geoip_path, sizeof g_cfg.geoip_path, g_cfg.geoip_path);
    if (g_cfg.bridges_file[0]) load_bridges_file(g_cfg.bridges_file);

    state_init();
    g_app.mode = g_cfg.mode;
    g_app.timer_sec = g_cfg.timer_sec;
    g_app.rot_every_n = g_cfg.rot_every_n;

    printf("tor_rotate " ROT_VERSION "  -  [h] for help\n");
    if (sock_init() != 0) { log_line("FATAL: socket init failed"); return 1; }

    web_start(g_cfg.web_port);
    if (torproc_launch() != 0) { torproc_stop(); return 1; }

    worker_start();
    if (proxy_start(g_cfg.listen_port) != 0) { torproc_stop(); return 1; }

    log_line("ready. chain your client through socks5://127.0.0.1:%d", g_cfg.listen_port);
    ui_init();
    ui_status();

#ifndef _WIN32
    /* raw terminal for single-key input */
    struct termios t;
    tcgetattr(0, &t);
    struct termios raw = t;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);
#endif

    long last_status = 0;
    while (g_app.running) {
        if (ui_poll_key() != 0) break;
        if (now_sec() - last_status >= 2) {
            ui_status();
            last_status = now_sec();
        }
        msleep(100);
    }

    log_line("shutting down...");
#ifndef _WIN32
    tcsetattr(0, TCSANOW, &t);
#endif
    g_app.running = 0;
    msleep(150);
    ctrl_close(&g_app.ctrl);
    torproc_stop();
    log_line("bye");
    return 0;
}
