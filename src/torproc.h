/* torproc.h - tor lifecycle */
#ifndef TORPROC_H
#define TORPROC_H
#include "mini.h"

/* generate torrc in data_dir, spawn tor, wait for bootstrap via control port.
   returns 0 on success, fills ctrl connection in g_app. */
int torproc_launch(void);
void torproc_stop(void);

#endif
