#ifndef HOME_STORAGE_H
#define HOME_STORAGE_H

#include "ui.h"

void storage_open(void);          /* Settings > Storage: starts measuring in the background */
int storage_active(void);
void storage_close(void);
void storage_update(const Input *in);
const char *storage_hint(void);

#endif
