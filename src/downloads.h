/* Downloads: HTTP(S) straight to the card with libcurl + OpenSSL, which speak
 * modern TLS that the Vita's own 2018 SSL library cannot (GitHub's CDN fails
 * the handshake there even with the certificate check out of the way).
 *
 * The list comes from ux0:data/workbench/downloads.txt, one "URL[<TAB>dest]"
 * per line, so an agent can queue work with `vita.py put` and the player can
 * add URLs with the keyboard. Files land in ux0:downloads/ unless a dest is
 * given, and every finished file shows its SHA-256. */
#ifndef WB_DOWNLOADS_H
#define WB_DOWNLOADS_H

#include "ui.h"

int downloads_init(void);             /* <0: network or TLS stack unavailable */
void downloads_update(const Input *in);
int downloads_busy(void);
int downloads_progress(float *frac);   /* files left while the worker runs, else 0 */
const char *downloads_hint(void);
void downloads_show_store(void);      /* back to the Store page (a search hit opened there) */
void downloads_term(void);

#endif
