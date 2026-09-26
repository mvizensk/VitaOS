#ifndef HOME_STORE_H
#define HOME_STORE_H

#include "ui.h"

void store_init(void);             /* after the network is up */
int store_update(const Input *in); /* 0 when the user backs out to the download queue */
const char *store_hint(void);
void store_leave(void);

int store_fetch(const char *url, const char *dest);   /* HTTPS to a file; <0 on failure */
int store_uninstall(const char *tid, const char *name);   /* async; toasts the result, rescans Apps */

#endif
