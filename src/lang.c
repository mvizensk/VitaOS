/* Translations and text size (2026-10-01: much of the Vita scene does not
 * read English). Every string VitaOS draws passes through tr() at the text
 * calls in ui.c; a table for the chosen language maps the English text to
 * its translation, exact match only, so names, titles and numbers pass
 * through untouched. Tables: app0:assets/lang/<code>.tsv, "English\tOther"
 * per line. Hints keep their button prefix ("X select"): the words after it
 * are looked up on their own, as are labels after arrow glyphs.
 *
 * The language follows the system's unless Settings picks one
 * (user/lang.txt). Large text adds a point or two to every size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/io/fcntl.h>
#include "lang.h"

#define LANG_CFG "ux0:data/arcadehub/user/lang.txt"
#define TEXT_CFG "ux0:data/arcadehub/user/textsize.txt"
#define SLOTS 2048                                   /* power of two, well above the table size */

static const char *const codes[] = {"auto", "en", "es", "pt"};   /* French and German next */
static const char *const names[] = {"Automatic", "English", "Espa\xC3\xB1ol", "Portugu\xC3\xAAs"};
#define NCODES (int)(sizeof(codes) / sizeof(codes[0]))

static int choice, active_lang = 1, large;
static char *blob;
static struct { const char *en, *tr; } table[SLOTS];

static unsigned int hash(const char *s, int n) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < n && s[i]; ++i) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static const char *find(const char *s, int n) {
    if (!blob) return NULL;
    for (unsigned int i = hash(s, n) & (SLOTS - 1), k = 0; table[i].en && k < SLOTS; i = (i + 1) & (SLOTS - 1), ++k)
        if (!strncmp(table[i].en, s, n) && table[i].en[n] == 0) return table[i].tr;
    return NULL;
}

static int system_lang(void) {
    int l = 1;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &l) < 0) return 1;
    switch (l) {
    case SCE_SYSTEM_PARAM_LANG_SPANISH: return 2;
    case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
    case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: return 3;
    default: return 1;
    }
}

static void load_table(void) {
    free(blob);
    blob = NULL;
    memset(table, 0, sizeof(table));
    if (active_lang <= 1) return;
    char path[64];
    snprintf(path, sizeof(path), "app0:assets/lang/%s.tsv", codes[active_lang]);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    blob = size > 0 ? malloc(size + 1) : NULL;
    int n = blob ? sceIoRead(fd, blob, size) : 0;
    sceIoClose(fd);
    if (!blob) return;
    blob[n > 0 ? n : 0] = 0;
    for (char *line = blob; *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *tab = strchr(line, '\t');
        if (tab && line[0] != '#') {
            *tab = 0;
            char *t = tab + 1;
            int tl = (int)strlen(t);
            if (tl && t[tl - 1] == '\r') t[tl - 1] = 0;
            /* "\n" in the file is a line break in the text */
            for (char *src = t, *dst = t; ; ++src) {
                if (src[0] == '\\' && src[1] == 'n') { *dst++ = '\n'; ++src; continue; }
                if (!(*dst++ = *src)) break;
            }
            for (char *src = line, *dst = line; ; ++src) {
                if (src[0] == '\\' && src[1] == 'n') { *dst++ = '\n'; ++src; continue; }
                if (!(*dst++ = *src)) break;
            }
            unsigned int i = hash(line, 1 << 30) & (SLOTS - 1);
            for (int k = 0; table[i].en && k < SLOTS; ++k) i = (i + 1) & (SLOTS - 1);
            table[i].en = line;
            table[i].tr = t;
        }
        if (!end) break;
        line = end + 1;
    }
}

static int read_int_file(const char *path, char *buf, int max) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, buf, max - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    return n > 0;
}

void lang_init(void) {
    char b[16];
    choice = 0;
    if (read_int_file(LANG_CFG, b, sizeof(b)))
        for (int i = 0; i < NCODES; ++i) if (!strncmp(b, codes[i], strlen(codes[i])) && strlen(codes[i]) >= 2) choice = i;
    active_lang = choice ? choice : system_lang();
    large = read_int_file(TEXT_CFG, b, sizeof(b)) && b[0] == 'l';
    load_table();
}

int lang_count(void) { return NCODES; }
const char *lang_name(int i) { return i >= 0 && i < NCODES ? names[i] : ""; }
int lang_choice(void) { return choice; }
const char *lang_active_name(void) { return names[active_lang]; }

static void save(const char *path, const char *text) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, text, strlen(text)); sceIoClose(fd); }
}

void lang_set(int i) {
    if (i < 0 || i >= NCODES) return;
    choice = i;
    save(LANG_CFG, codes[i]);
    active_lang = choice ? choice : system_lang();
    load_table();
}

int text_large(void) { return large; }
void text_set_large(int on) { large = on; save(TEXT_CFG, on ? "large" : "normal"); }
int text_bump(int size) { return large ? (size <= 14 ? size + 1 : size + 2) : size; }

/* The translation of s, or s itself. Results that need building (a label
 * after an arrow, words after a button name) go to a small ring of buffers,
 * enough for one frame's worth of nested calls. */
const char *tr(const char *s) {
    if (!blob || !s || !*s) return s;
    int n = (int)strlen(s);
    const char *t = find(s, n);
    if (t) return t;
    int skip = 0;                                      /* leading arrows, spaces and glyphs */
    while (skip < n && ((unsigned char)s[skip] >= 0x80 || s[skip] == ' ')) ++skip;
    if (!skip || skip >= n) return s;
    t = find(s + skip, n - skip);
    if (!t) return s;
    static char ring[8][256];
    static int r;
    char *out = ring[r++ & 7];
    snprintf(out, 256, "%.*s%s", skip, s, t);
    return out;
}
