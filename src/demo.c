/* The tour: which tab, which buttons, how long to linger, frame by frame at
 * 60 fps. Taps on the tab bar switch tabs, so the tour never depends on
 * where Home was left. */
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "demo.h"

#define REQ "ux0:data/arcadehub/user/demo.req"
enum { WAIT, PRESS, TAP };
typedef struct { int kind, a, b; } Step;         /* WAIT frames | PRESS button | TAP x y */

#define TAB(x) {TAP, x, 34}
static const Step tour[] = {
    TAB(64), {WAIT, 110},                                            /* Home, with the news */
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 55}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 55},
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 70}, {PRESS, SCE_CTRL_CROSS}, {WAIT, 150},  /* a news post */
    {PRESS, SCE_CTRL_CIRCLE}, {WAIT, 30}, {PRESS, SCE_CTRL_LEFT}, {WAIT, 20},
    {PRESS, SCE_CTRL_LEFT}, {WAIT, 20}, {PRESS, SCE_CTRL_LEFT}, {WAIT, 40},
    TAB(139), {WAIT, 90},                                            /* Play: the consoles */
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 50}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 50},
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 50}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 70},
    {PRESS, SCE_CTRL_CROSS}, {WAIT, 90},                             /* into a console's games */
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 40}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 40},
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 90}, {PRESS, SCE_CTRL_CIRCLE}, {WAIT, 30},
    TAB(221), {WAIT, 90},                                            /* Movies */
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 60}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 60}, {PRESS, SCE_CTRL_LEFT}, {WAIT, 50},
    TAB(312), {WAIT, 90},                                            /* Music */
    {PRESS, SCE_CTRL_DOWN}, {WAIT, 50}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 40}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 40},
    {PRESS, SCE_CTRL_DOWN}, {WAIT, 70}, {PRESS, SCE_CTRL_UP}, {WAIT, 20}, {PRESS, SCE_CTRL_UP}, {WAIT, 30},
    TAB(543), {WAIT, 100},                                           /* the Store */
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 50}, {PRESS, SCE_CTRL_DOWN}, {WAIT, 50},
    {PRESS, SCE_CTRL_RIGHT}, {WAIT, 35}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 35}, {PRESS, SCE_CTRL_RIGHT}, {WAIT, 45},
    {PRESS, SCE_CTRL_DOWN}, {WAIT, 50}, {PRESS, SCE_CTRL_DOWN}, {WAIT, 50},
    {PRESS, SCE_CTRL_CROSS}, {WAIT, 160},                            /* an app page, with screenshots */
    {PRESS, SCE_CTRL_CIRCLE}, {WAIT, 30}, {PRESS, SCE_CTRL_UP}, {WAIT, 20}, {PRESS, SCE_CTRL_UP}, {WAIT, 20},
    {PRESS, SCE_CTRL_UP}, {WAIT, 30},
    TAB(64), {WAIT, 90},
};
#define NSTEPS ((int)(sizeof(tour) / sizeof(tour[0])))

/* The first recording ran through the tour in about 16 s: far too quick to
 * follow. Every wait is stretched by this. */
#define SLOW 3
static int step = -1, wait_left, frame;

int demo_running(void) { return step >= 0; }

void demo_poll(void) {
    if (step >= 0 || ++frame % 60) return;
    SceIoStat st;
    if (sceIoGetstat(REQ, &st) < 0) return;
    sceIoRemove(REQ);
    step = 0; wait_left = 0;
}

int demo_input(Input *in) {
    if (step < 0) return 0;
    memset(in, 0, sizeof(*in));
    if (wait_left > 0) { --wait_left; return 1; }
    if (step >= NSTEPS) { step = -1; return 0; }
    const Step *s = &tour[step++];
    if (s->kind == WAIT) wait_left = s->a * SLOW;
    else if (s->kind == PRESS) { in->pressed = in->held = (unsigned int)s->a; }
    else { in->tapped = 1; in->tap_x = s->a; in->tap_y = s->b; }
    return 1;
}
