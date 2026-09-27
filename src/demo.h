#ifndef HOME_DEMO_H
#define HOME_DEMO_H
#include "ui.h"
/* A scripted tour of VitaOS for screen recordings (promo video, posts):
 * start it by creating ux0:data/arcadehub/user/demo.req. While it runs it
 * replaces the pad and touch input with its own. */
void demo_poll(void);            /* once a frame: starts the tour when asked */
int demo_input(Input *in);       /* 1 while the tour drives; fills in */
int demo_running(void);          /* the recording is on: no agent pill or toasts */
#endif
