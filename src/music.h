#ifndef HOME_MUSIC_H
#define HOME_MUSIC_H

#include "ui.h"

void music_update(const Input *in);   /* the tab */
void music_tick(const Input *in);     /* every frame, any tab: next track, no auto standby */
int music_playing(void);
const char *music_now(void);          /* "♪ Title · Artist", or "" */
const char *music_hint(void);
void music_prewarm(void);             /* background thread, at start-up */
/* Home tab: the last album (0 if none yet), and picking it up / pausing it. */
int music_last_item(const char **album, const char **artist, vita2d_texture **art, int *now_playing);
void music_resume(void);

#endif
