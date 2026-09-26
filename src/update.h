#ifndef HOME_UPDATE_H
#define HOME_UPDATE_H
void update_init(void);           /* after the network is up: at most one check a day */
const char *update_newer(void);   /* "1.2.0" when a newer release is out, else NULL */
void update_get(void);            /* download it to ux0:downloads (async, toasts) */
#endif
