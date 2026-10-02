#ifndef HOME_DROP_H
#define HOME_DROP_H

#include "ui.h"

void drop_open(const char *dir, const char *app_title);   /* serve the upload page into dir until closed */
int drop_active(void);
void drop_update(const Input *in);
const char *drop_hint(void);

#endif
