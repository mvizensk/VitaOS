#ifndef HOME_BATTERY_H
#define HOME_BATTERY_H

#include "ui.h"

void battery_tick(void);            /* every frame: measures time per charge while on battery */
void battery_open(void);            /* Settings > Battery health */
void battery_close(void);
int battery_active(void);
void battery_update(const Input *in);
const char *battery_hint(void);

#endif
