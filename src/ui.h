/* ui.h */
#ifndef UI_H
#define UI_H
void ui_init(void);
void ui_status(void);
int  ui_poll_key(void);   /* 0 continue, -1 quit */
void ui_invalidate(void); /* next ui_status redraws fresh (after log lines) */
#endif
