#ifndef HOME_CRASH_H
#define HOME_CRASH_H

#include "ui.h"

void crash_init(void);                                   /* reads the last run's stage, then looks for its crash file */
void crash_note_stage(const char *stage, unsigned int frames);   /* the watchdog, every tick */
int crash_active(void);                                  /* the report card is up */
void crash_update(const Input *in);
const char *crash_hint(void);

#endif
