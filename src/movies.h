#ifndef HOME_MOVIES_H
#define HOME_MOVIES_H

#include "ui.h"

void movies_update(const Input *in);
int movies_fullscreen(void);          /* the player: no tab bar, L/R seek instead of switching tabs */
const char *movies_hint(void);
void movies_leave(void);
void movies_prewarm(void);            /* background thread, at start-up */
/* Home tab: the part-watched film (returns its index + 1, or 0), and opening one. */
int movies_resume_item(const char **title, vita2d_texture **poster, unsigned int *at_ms);
void movies_open(int i);

#endif
