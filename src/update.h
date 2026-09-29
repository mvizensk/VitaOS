#ifndef HOME_UPDATE_H
#define HOME_UPDATE_H
void update_init(void);           /* after the network is up: at most one check a day */
const char *update_newer(void);   /* "1.2.0" when a newer release is out, else NULL */
void update_get(void);            /* download, get it ready and hand over to VitaOS Updater (async) */
void update_tick(void);           /* each frame: launches the updater once the update is ready */
int update_stage(const char **msg, float *frac);   /* 0 idle, 1 downloading, 2 preparing, 3-4 handing over, 9 failed */
#endif
