/* state.h - shared runtime state */
#ifndef STATE_H
#define STATE_H
#include "mini.h"
#include "control.h"

typedef struct {
    /* rotation */
    volatile rot_mode_t mode;
    volatile int timer_sec;
    volatile int rot_every_n;
    volatile int paused;

    /* stats */
    volatile long requests;        /* total proxied connections */
    volatile long rotations;       /* NEWNYMs sent */
    long last_rotate_ts;

    /* current exit view */
    exit_info_t cur_exit;

    /* control */
    ctrl_t ctrl;
    volatile int tor_ready;        /* bootstrap 100% */
    volatile int running;

    THREAD_HANDLE timer_thread;
} app_state_t;

extern app_state_t g_app;

void state_init(void);
void state_request_hook(void);      /* called per request; may signal newnym */
void state_set_mode(rot_mode_t m);
const char *state_mode_str(void);

#endif
