#ifndef HOME_CAMERA_H
#define HOME_CAMERA_H

#include "ui.h"

void camera_open(int roll);        /* 0: the viewfinder, 1: the photo roll */
void camera_leave(void);           /* stops the camera; any tab switch or PS calls it */
int camera_active(void);
int camera_fullscreen(void);       /* viewfinder or a photo: no tab bar, L R are its own */
void camera_update(const Input *in);
const char *camera_hint(void);
void camera_draw_icon(int kind, float x, float y, float size);   /* 0 Camera, 1 Photos */

#endif
