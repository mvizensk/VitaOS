#ifndef HOME_LANG_H
#define HOME_LANG_H

void lang_init(void);              /* at start: the saved choice, else the system's language */
const char *tr(const char *s);     /* the translation of s, or s itself */
int lang_count(void);              /* choices: 0 automatic, then each language */
const char *lang_name(int i);
int lang_choice(void);
const char *lang_active_name(void);
void lang_set(int i);

int text_large(void);              /* Settings > Display & sound > Text size */
void text_set_large(int on);
int text_bump(int size);           /* the size to draw at */

#endif
