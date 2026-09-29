#pragma once
/* A game's description from Wikipedia, cached per title (gamedesc.c). */
int gamedesc_cached(const char *title, char *out, int max);   /* 1 text, 0 looked and none, -1 not looked yet */
void gamedesc_request(const char *title);                     /* starts a lookup on its own thread */
const char *gamedesc_result(const char *title);               /* the finished lookup for this title, or NULL */
