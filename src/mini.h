/* mini.h - common types & config for tor_rotate */
#ifndef MINI_H
#define MINI_H

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <conio.h>
#  include <direct.h>
#  include <process.h>
#ifdef _MSC_VER
#  pragma comment(lib, "ws2_32.lib")
#endif
typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define THREAD_RET unsigned __stdcall
#  define THREAD_HANDLE HANDLE
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <pthread.h>
#  include <termios.h>
#  include <fcntl.h>
typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define THREAD_RET void *
#  define THREAD_HANDLE pthread_t
#  define closesocket close
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#define ROT_VERSION "0.1"

/* rotation policy */
typedef enum { ROT_OFF = 0, ROT_PER_REQ, ROT_TIMER, ROT_MANUAL } rot_mode_t;

typedef struct {
    char tor_path[512];       /* tor executable */
    char data_dir[512];       /* tor DataDirectory */
    char bin_dir[512];        /* dir holding geoip etc. */
    int  socks_port;          /* tor SOCKS (default 9050) */
    int  control_port;        /* tor ControlPort (default 9051) */
    int  listen_port;         /* our SOCKS5 listener (default 1080) */
    char upstream_proxy[256]; /* optional socks5 host:port tor dials through */
    char exit_include[256];   /* ExitNodes e.g. "{de},{nl}" (may be empty) */
    char exit_exclude[256];   /* ExcludeNodes (may be empty) */
    char ptdir[512];          /* dir with lyrebird.exe (default: tor dir) */
    char geoip_path[512];     /* optional GeoIPFile for cc display */
    char bridges_file[512];   /* file with bridge lines, one per line */
    char bridge_lines[16][256];
    int  n_bridges;
    rot_mode_t mode;
    int  timer_sec;           /* for ROT_TIMER */
    int  rot_every_n;         /* for ROT_PER_REQ batch size */
} config_t;

extern config_t g_cfg;

/* util.c */
void log_line(const char *fmt, ...);
void log_set_quiet(int q);
long now_sec(void);
void str_copy(char *dst, size_t n, const char *src);
int  str_has(const char *hay, const char *needle);
int  str_icontains(const char *hay, const char *needle);
void msleep(int ms);
void ensure_dir(const char *path);

/* resolve path to absolute (in place buffer) */
void make_abs(char *buf, size_t n, const char *path);

#endif
