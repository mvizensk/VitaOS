#ifndef HOME_SEARCH_H
#define HOME_SEARCH_H

#include "ui.h"

/* Global search: SELECT anywhere. Every module answers with hits. */
enum { H_GAME, H_APP, H_FILM, H_ALBUM, H_SETTING };
typedef struct {
    int kind, a, b;             /* module-specific: e.g. system and index for a game */
    int score;                  /* 3 title starts with it, 2 a word does, 1 contains */
    const char *title, *sub;
    vita2d_texture *art;        /* may be NULL: then 'path' is loaded lazily, one per frame */
    char path[192];
    int owned;                  /* search loaded 'art' itself and frees it */
} Hit;

int match_score(const char *title, const char *q);

/* The overlay: open it, then while it is active the main loop hands it the
 * frame. It returns the tab to switch to after opening a result, or -1. */
void search_open(void);
void search_prepare(void);
void search_close(void);       /* main calls this between frames */
int search_active(void);
int search_update(const Input *in);
enum { SEARCH_TO_MOVIES = 100, SEARCH_TO_MUSIC, SEARCH_TO_SETTINGS, SEARCH_TO_PLAY };

/* Answered by the modules. */
int play_find(const char *q, Hit *out, int max);
int apps_find(const char *q, Hit *out, int max);
int movies_find(const char *q, Hit *out, int max);
int music_find(const char *q, Hit *out, int max);
void play_open_hit(const Hit *h);    /* launches the game */
void apps_open_hit(const Hit *h);
void movies_open_hit(const Hit *h);
void music_open_hit(const Hit *h);
void play_show_all(const char *q);   /* the Search shelf in Play, filled with q */

#endif
