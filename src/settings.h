#ifndef HOME_SETTINGS_H
#define HOME_SETTINGS_H

#include "ui.h"

void settings_update(const Input *in);
const char *settings_hint(void);
/* Called by the "System home" button: Home lifts its PS lock for a while. */
void settings_on_release_ps(void (*fn)(void));
void settings_on_library(void (*rescan)(void), void (*art)(void));   /* Settings > VitaOS */

void settings_leave(void);   /* PS: back to the top of the page */
float settings_brightness(void);                 /* 0..1 */
float settings_volume(void);
void settings_set_brightness(float f, int save); /* save: write the registry too (on release) */
void settings_set_volume(float f, int save);

#endif
