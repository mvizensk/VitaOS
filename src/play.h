#ifndef HOME_PLAY_H
#define HOME_PLAY_H

#include "ui.h"

void play_init(void);
void play_reload(void);               /* the library on the card changed */
void play_frame(const Input *in);     /* draws the tab's content */
int play_fullscreen(void);            /* the attract reel: no tab bar or footer */
const char *play_context(void);       /* e.g. "PlayStation   12 / 340" */
const char *play_hint(void);
void play_leave(void);
int play_wake(void);                  /* PS in the attract reel: back where you were */
void play_home(void);                 /* back to the shelves, as if you pressed O */

/* The Home tab's "jump back in" row: the recent games, newest first. */
typedef struct {
    const char *title, *system, *year, *genre;
    const char *rom;                      /* RetroArch games: the content path, else NULL */
    vita2d_texture *cover, *art;          /* box art; a screenshot when there is one */
    unsigned int accent;
} PlayItem;
int play_recent_count(void);
void play_recent_item(int i, PlayItem *out);
int play_recent_launch(int i);            /* like X on the shelf; < 0 on failure */
long play_recent_total_seconds(int i);    /* total play time, for the meta line */

#endif
