#ifndef HOME_HOMETAB_H
#define HOME_HOMETAB_H

#include "ui.h"

void hometab_update(const Input *in);
const char *hometab_hint(void);
void hometab_reset(void);   /* PS: focus back on the first tile */
enum { HOMETAB_TO_MOVIES = 1 };
int hometab_wants_tab(void);   /* HOMETAB_TO_MOVIES after opening a film, else -1 */

#endif
