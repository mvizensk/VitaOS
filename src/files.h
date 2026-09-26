/* Dual-pane file manager. Copy, move, delete and hash run on fileops' job
 * thread (the same code as the network bridge, so the same safety rules), and
 * several marked items queue up behind one another. */
#ifndef WB_FILES_H
#define WB_FILES_H

#include "ui.h"

void files_init(void);
void files_update(const Input *in);   /* handles input and draws */
int files_busy(void);                 /* a job or queued work is pending */
const char *files_hint(void);

#endif
