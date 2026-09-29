#ifndef HOME_STORE_H
#define HOME_STORE_H

#include "ui.h"

void store_init(void);             /* after the network is up */
int store_update(const Input *in); /* 0 when the user backs out to the download queue */
const char *store_hint(void);
void store_leave(void);

int store_fetch(const char *url, const char *dest);   /* HTTPS to a file; <0 on failure */
int store_match(const char *headline);   /* the app a news headline names, or -1 */
void store_show(int app);                /* open its page in the Store */
int store_uninstall(const char *tid, const char *name);   /* async; toasts the result, rescans Apps */
int store_updates_count(void);           /* installed apps behind the catalogue; for a Home badge */
int store_prepare_pkg(const char *vpk, const char *pkg, char tid[10]);   /* unzip + head.bin; <0 on failure */
float store_job_frac(void);              /* progress of the download running now, 0..1 */
int store_install_dir(const char *pkg);  /* promote a prepared folder; the promoter's result */

#endif
