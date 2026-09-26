#ifndef HOME_SFX_H
#define HOME_SFX_H

enum { SFX_MOVE, SFX_SELECT, SFX_BACK, SFX_TAB, SFX_BUMP, SFX_LAUNCH, SFX_BOOT, SFX_COUNT };

void sfx_init(void);
void sfx_play(int id);
int sfx_ok(void);               /* the audio port opened */
int sfx_level(void);            /* 0 (off) .. 10 */
void sfx_set_level(int level);  /* saved for next time */
void sfx_ambient(int want);     /* Home's quiet chords: call every frame with whether they fit */
int sfx_ambient_level(void);
void sfx_set_ambient_level(int level);

#endif
