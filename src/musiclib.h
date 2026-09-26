#ifndef HOME_MUSICLIB_H
#define HOME_MUSICLIB_H

/* Scans ux0:data/music and writes library.tsv (unless the Mac importer's is
 * there: returns 0). Returns the number of tracks. Slow: call from a thread. */
int musiclib_scan(void);

#endif
