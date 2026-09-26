/* Music's library, found on the card (VitaOS: no Mac-side importer needed).
 *
 * Walks ux0:data/music for .m4a / .mp4 audio (what the Vita's player plays),
 * reads each file's MP4 tags (title, artist, album, track, duration, cover)
 * and writes library.tsv in the importer's format:
 *   path  title  artist  album  track  duration_ms  cover
 * paths relative to ux0:data/music/, sorted by artist, album, track. Covers
 * are saved once under .covers/. A library.tsv from the Mac importer
 * (import_music.py) has no "#vitaos" first line and is left alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include "musiclib.h"

#define LIB "ux0:data/music/"
#define MARK "#vitaos"
#define MAX_TRACKS 2048

typedef struct { char path[256], title[128], artist[96], album[96], cover[64]; int track; unsigned int dur; } T;
static T *tracks;
static int n;

static unsigned int be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Reads [off, off+len) of the file into a malloc'd buffer (capped). */
static unsigned char *read_at(SceUID fd, unsigned int off, unsigned int len) {
    if (len > 4 * 1024 * 1024) return NULL;
    unsigned char *b = malloc(len);
    if (!b) return NULL;
    sceIoLseek(fd, off, SCE_SEEK_SET);
    if (sceIoRead(fd, b, len) != (int)len) { free(b); return NULL; }
    return b;
}

static void cover_name(const char *path, char *out, int max) {
    unsigned int h = 2166136261u;
    for (const char *q = path; *q; ++q) h = (h ^ (unsigned char)*q) * 16777619u;
    snprintf(out, max, ".covers/%08x.jpg", h);
}

/* ilst's text atoms: [size][name][size 'data' type locale][text] */
static void text_of(const unsigned char *atom, unsigned int size, char *out, int max) {
    if (size < 24 || memcmp(atom + 12, "data", 4)) return;
    int len = (int)size - 24;
    if (len >= max) len = max - 1;
    memcpy(out, atom + 24, len);
    out[len] = 0;
    for (char *c = out; *c; ++c) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
}

static void walk_ilst(const unsigned char *p, unsigned int len, T *t, const char *rel) {
    for (unsigned int o = 0; o + 8 <= len;) {
        unsigned int sz = be32(p + o);
        if (sz < 8 || o + sz > len) break;
        const unsigned char *a = p + o;
        if (!memcmp(a + 4, "\xA9nam", 4)) text_of(a, sz, t->title, sizeof(t->title));
        else if (!memcmp(a + 4, "\xA9" "ART", 4)) text_of(a, sz, t->artist, sizeof(t->artist));
        else if (!memcmp(a + 4, "aART", 4) && !t->artist[0]) text_of(a, sz, t->artist, sizeof(t->artist));
        else if (!memcmp(a + 4, "\xA9" "alb", 4)) text_of(a, sz, t->album, sizeof(t->album));
        else if (!memcmp(a + 4, "trkn", 4) && sz >= 30) t->track = a[27] | a[26] << 8;
        else if (!memcmp(a + 4, "covr", 4) && sz > 24 && !memcmp(a + 12, "data", 4)) {
            char cn[64], full[128];
            cover_name(rel, cn, sizeof(cn));
            snprintf(full, sizeof(full), LIB "%s", cn);
            SceIoStat st;
            if (sceIoGetstat(full, &st) < 0) {
                SceUID fd = sceIoOpen(full, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                if (fd >= 0) { sceIoWrite(fd, a + 24, sz - 24); sceIoClose(fd); }
            }
            snprintf(t->cover, sizeof(t->cover), "%s", cn);
        }
        o += sz;
    }
}

/* Finds a box by path ("moov", "udta", ...) inside [p, p+len). */
static const unsigned char *find_box(const unsigned char *p, unsigned int len, const char *type, unsigned int *out_len) {
    for (unsigned int o = 0; o + 8 <= len;) {
        unsigned int sz = be32(p + o);
        if (sz < 8 || o + sz > len) return NULL;
        if (!memcmp(p + o + 4, type, 4)) { *out_len = sz - 8; return p + o + 8; }
        o += sz;
    }
    return NULL;
}

static int read_tags(const char *full, const char *rel, T *t) {
    SceUID fd = sceIoOpen(full, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    unsigned int size = (unsigned int)sceIoLseek(fd, 0, SCE_SEEK_END), off = 0;
    int ok = -1;
    while (off + 8 <= size) {                         /* top-level boxes: find moov */
        unsigned char h[8];
        sceIoLseek(fd, off, SCE_SEEK_SET);
        if (sceIoRead(fd, h, 8) != 8) break;
        unsigned int sz = be32(h);
        if (sz < 8) break;
        if (!memcmp(h + 4, "moov", 4)) {
            unsigned char *moov = read_at(fd, off + 8, sz - 8);
            if (moov) {
                unsigned int ml, ul, mtl, il;
                const unsigned char *mvhd = find_box(moov, sz - 8, "mvhd", &ml);
                if (mvhd && ml >= 20) {
                    unsigned int scale = mvhd[0] == 1 ? be32(mvhd + 20) : be32(mvhd + 12);
                    unsigned int dur = mvhd[0] == 1 ? be32(mvhd + 28) : be32(mvhd + 16);
                    if (scale) t->dur = (unsigned int)((unsigned long long)dur * 1000 / scale);
                }
                const unsigned char *udta = find_box(moov, sz - 8, "udta", &ul);
                const unsigned char *meta = udta ? find_box(udta, ul, "meta", &mtl) : NULL;
                const unsigned char *ilst = meta && mtl > 4 ? find_box(meta + 4, mtl - 4, "ilst", &il) : NULL;
                if (ilst) walk_ilst(ilst, il, t, rel);
                free(moov);
                ok = 0;
            }
            break;
        }
        off += sz;
    }
    sceIoClose(fd);
    return ok;
}

static void walk(const char *dir, const char *rel, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    while (n < MAX_TRACKS) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        char full[400], r[300];
        snprintf(full, sizeof(full), "%s/%s", dir, e.d_name);
        snprintf(r, sizeof(r), "%s%s%s", rel, *rel ? "/" : "", e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) { if (depth < 4) walk(full, r, depth + 1); continue; }
        const char *dot = strrchr(e.d_name, '.');
        if (!dot || (strcasecmp(dot, ".m4a") && strcasecmp(dot, ".mp4") && strcasecmp(dot, ".aac"))) continue;
        T *t = &tracks[n];
        memset(t, 0, sizeof(*t));
        snprintf(t->path, sizeof(t->path), "%s", r);
        if (read_tags(full, r, t) < 0) continue;
        if (!t->title[0]) { snprintf(t->title, sizeof(t->title), "%.*s", (int)(dot - e.d_name), e.d_name); }
        if (!t->artist[0]) snprintf(t->artist, sizeof(t->artist), "Unknown artist");
        if (!t->album[0]) snprintf(t->album, sizeof(t->album), "Singles");
        ++n;
    }
    sceIoDclose(d);
}

static int by_artist(const void *a, const void *b) {
    const T *x = a, *y = b;
    int c = strcasecmp(x->artist, y->artist);
    if (!c) c = strcasecmp(x->album, y->album);
    if (!c) c = x->track - y->track;
    if (!c) c = strcasecmp(x->title, y->title);
    return c;
}

int musiclib_scan(void) {
    char head[16] = {0};
    SceUID fd = sceIoOpen(LIB "library.tsv", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        sceIoRead(fd, head, sizeof(head) - 1);
        sceIoClose(fd);
        if (strncmp(head, MARK, strlen(MARK))) return 0;   /* the Mac importer's: leave it */
    }
    tracks = calloc(MAX_TRACKS, sizeof(T));
    if (!tracks) return -1;
    n = 0;
    sceIoMkdir("ux0:data/music", 0777);
    sceIoMkdir(LIB ".covers", 0777);
    walk("ux0:data/music", "", 0);
    qsort(tracks, n, sizeof(T), by_artist);
    fd = sceIoOpen(LIB "library.tsv.new", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, MARK "\n", strlen(MARK) + 1);
        static char line[800];
        for (int i = 0; i < n; ++i) {
            T *t = &tracks[i];
            int len = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%d\t%u\t%s\n", t->path, t->title, t->artist, t->album,
                               t->track, t->dur, t->cover);
            sceIoWrite(fd, line, len);
        }
        sceIoClose(fd);
        sceIoRemove(LIB "library.tsv");
        sceIoRename(LIB "library.tsv.new", LIB "library.tsv");
    }
    int found = n;
    free(tracks);
    tracks = NULL;
    return found;
}
