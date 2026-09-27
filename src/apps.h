#ifndef HOME_APPS_H
#define HOME_APPS_H

#include "ui.h"

void apps_update(const Input *in);
const char *apps_hint(void);
int apps_count(void);
void apps_prewarm(void);               /* starts a background scan of ux0:app */
const char *apps_title_for(const char *tid);

const char *app_art(const char *tid, const char *file, char *out, int max);   /* ur0:appmeta first */

#endif
