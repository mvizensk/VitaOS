#ifndef HUB_VIDEO_H
#define HUB_VIDEO_H
#include <vita2d.h>
int video_init(void);                 /* loads reAvPlayer once; <0 if unavailable */
int video_play(const char *path, int with_sound);  /* looping preview; sound in attract mode */
void video_stop(void);
void video_collect(void);             /* main thread, between frames: frees what a closed player left */
vita2d_texture *video_frame(void);    /* latest decoded frame, or NULL */

/* One player serves previews, movies and music, so every playback has an
 * owner: a preview must never cut off your music, and stopping is only done
 * by whoever started it. */
enum { OWN_NONE, OWN_PREVIEW, OWN_MOVIE, OWN_MUSIC };
int video_play_opts(const char *path, int with_sound, int loop, unsigned int start_ms, int owner);
void video_stop_owned(int owner);     /* stops only if `owner` is playing */
int video_owner(void);                /* OWN_NONE when nothing plays */
int video_finished(void);             /* a non-looping item reached its end */
void video_pause(int paused);
int video_paused(void);
void video_seek(unsigned int ms);
unsigned int video_pos_ms(void);
unsigned int video_duration_ms(void);
#endif
