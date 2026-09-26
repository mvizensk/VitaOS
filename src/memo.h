#ifndef HOME_MEMO_H
#define HOME_MEMO_H

#include "ui.h"

void memo_open(void);
int memo_active(void);
void memo_leave(void);             /* stops recording or playback; tab switches and PS call it */
void memo_update(const Input *in);
const char *memo_hint(void);
void memo_draw_icon(float x, float y, float size);

#endif
