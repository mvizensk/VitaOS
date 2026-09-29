/* The homebrew store, in Downloads. The catalogue is VitaHomebrewDB (the
 * community mirror of VitaDB, which closed on 2026-07-31): one JSON file on
 * GitHub Pages, every entry a direct VPK link. Installing happens here on the
 * Vita, the way VitaShell does it: download, unzip (miniz's inflater), write
 * a homebrew head.bin, and hand the folder to the system's promoter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/promoterutil.h>

#include "store.h"
#include "apps.h"
#include "sfx.h"
#include "store_headbin.h"
#include "../third_party/miniz/miniz.h"

#define DIR "ux0:data/arcadehub/store"
#define CATALOG_URL "https://drdecki.github.io/VitaHomebrewDB/apps.json"
#define ICON_URL "https://drdecki.github.io/VitaHomebrewDB/icons/"
#define BASE_URL "https://drdecki.github.io/VitaHomebrewDB/"
#define MAX_APPS 1400

typedef struct {
    char *name, *icon, *version, *author, *type, *description, *date, *titleid, *long_description,
         *size, *url, *data, *requirements, *release_page, *downloads, *screenshots;
    volatile int icon_state;                  /* 0 not asked, 1 queued, 2 on the card, 3 failed, 4 loaded */
    volatile int shots_state;                 /* 0 not asked, 1 fetching, 2 on the card */
    int inst;                                 /* 0 not checked, 1 not installed, 2 installed */
} App;

static App apps[MAX_APPS];
static int napps, view[MAX_APPS], nview;
static char *blob;
/* The front page is "For you" (rows, like the Play Store), then Top charts,
 * then one grid per VitaDB type. */
enum { C_FORYOU, C_TOP, C_UPDATES, NCATS = 7 };
static int cat;
static const char *const cat_names[] = {"For you", "Top charts", "Updates", "Games", "Ports", "Utilities", "Emulators"};
static const char *const cat_types[] = {NULL, NULL, NULL, "1", "2", "4", "5"};
static int sel, detail, chips, cur;                  /* cur: the app on the detail page (index into apps) */
static float top;
static int grow_sel = -1;
static float grow;

/* Worker: the catalogue, icons, one install at a time. */
static SceUID wake = -1;
static volatile int want_catalog, installing = -1, job_stage, catalog_state;   /* catalog: 0 none 1 loading 2 ok 3 failed */
static volatile float job_frac;
static char job_msg[128];
static volatile int icon_queue[16], iq_head, iq_tail, shot_want = -1;

/* Titles that have caused trouble on this Vita: shown, never installed. */
static int denied(const App *a) {
    for (const char *h = a->name; *h; ++h) if (!strncasecmp(h, "caffeine", 8)) return 1;   /* wedged SceShell, 2026-09-19 */
    return 0;
}

/* ---------- JSON: an array of objects whose values are all strings ---------- */

static char *jstring(char **p) {                     /* at the opening quote; unescapes in place */
    char *s = ++*p, *o = s;
    while (**p && **p != '"') {
        if (**p == '\\') {
            ++*p;
            switch (**p) {
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = ' '; break;
            case 'r': break;
            case 'u': {
                unsigned int c = (unsigned int)strtoul((char[5]){(*p)[1], (*p)[2], (*p)[3], (*p)[4], 0}, NULL, 16);
                *p += 4;
                if (c < 0x80) *o++ = (char)c;
                else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
                else { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
                break;
            }
            default: *o++ = **p;
            }
            ++*p;
        } else *o++ = *(*p)++;
    }
    if (**p) ++*p;
    *o = 0;
    return s;
}

/* Checking an app reads the card (a stat, and its param.sfo when installed):
 * a screenful of new cards froze Home for 300 ms (2026-09-26). A few per
 * frame; the rest show plain until their turn, a frame or two later. */
static int inst_budget = 3;

static int by_date(const void *a, const void *b) {
    return strcmp(apps[*(const int *)b].date, apps[*(const int *)a].date);    /* newest first */
}

/* Most downloaded first: the hottest software on top (playtest 2026-09-25). */
static int by_popular(const void *a, const void *b) {
    long x = atol(apps[*(const int *)a].downloads), y = atol(apps[*(const int *)b].downloads);
    return x < y ? 1 : x > y ? -1 : by_date(a, b);
}
static int sort_new;                  /* 0 popular (default), 1 newest */
static int has_update(App *a);        /* defined below, by the install/SFO code; filter() wants it for Updates */

/* For you: a featured banner, then rows of the most popular in each kind,
 * then what is new. Built from the catalogue whenever it changes. */
#define NROWS 7
#define ROW_MAX 14
static const char *const row_titles[NROWS] = {"", "Popular right now", "Emulators", "Ports of PC and console games",
                                               "Handy utilities", "Homebrew games", "New and updated"};
static const char *const row_types[NROWS] = {NULL, NULL, "5", "2", "4", "1", NULL};
static int rows[NROWS][ROW_MAX], nrow[NROWS], rcol[NROWS], rrow;
static float rscroll[NROWS], vscroll;

static void build_rows(void) {
    static int all[MAX_APPS];
    for (int i = 0; i < napps; ++i) all[i] = i;
    qsort(all, napps, sizeof(int), by_popular);
    for (int r = 0; r < NROWS; ++r) {
        nrow[r] = 0;
        if (r == NROWS - 1) continue;
        for (int k = 0; k < napps && nrow[r] < (r == 0 ? 6 : ROW_MAX); ++k) {
            App *a = &apps[all[k]];
            if (row_types[r] && strcmp(a->type, row_types[r])) continue;
            if (r == 0 && !a->screenshots[0]) continue;          /* the banner wants a picture */
            rows[r][nrow[r]++] = all[k];
        }
    }
    int *nw = rows[NROWS - 1];
    for (int i = 0; i < napps; ++i) all[i] = i;
    qsort(all, napps, sizeof(int), by_date);
    for (int k = 0; k < napps && nrow[NROWS - 1] < ROW_MAX; ++k) nw[nrow[NROWS - 1]++] = all[k];
    for (int r = 0; r < NROWS; ++r) { rcol[r] = 0; rscroll[r] = 0; }
    rrow = 0; vscroll = 0;
}

static void filter(void) {
    nview = 0;
    for (int i = 0; i < napps; ++i) {
        if (cat == C_UPDATES) { inst_budget = 1; if (has_update(&apps[i])) view[nview++] = i; continue; }   /* opening Updates checks them all */
        if (!cat_types[cat] || !strcmp(apps[i].type, cat_types[cat])) view[nview++] = i;
    }
    qsort(view, nview, sizeof(int), sort_new && cat >= 2 ? by_date : by_popular);
    sel = 0; top = 0;
}

/* Parsed on the worker into the spare list, then swapped in by the main
 * thread between frames (parsing 1.6 MB on the main thread froze the UI). */
static App spare[MAX_APPS];
static int nspare;
static char *spare_blob;
static volatile int spare_ready;

static int parse(char *p, App *out) {
    int n = 0;
    App cur;
    memset(&cur, 0, sizeof(cur));
    while (*p && n < MAX_APPS) {
        if (*p == '{') { memset(&cur, 0, sizeof(cur)); ++p; continue; }
        if (*p == '}') {
            if (cur.name && cur.url) {
#define DEF(f) if (!cur.f) cur.f = ""
                DEF(icon); DEF(version); DEF(author); DEF(type); DEF(description); DEF(date); DEF(titleid);
                DEF(long_description); DEF(size); DEF(data); DEF(requirements); DEF(release_page); DEF(downloads); DEF(screenshots);
                out[n++] = cur;
            }
            ++p;
            continue;
        }
        if (*p != '"') { ++p; continue; }
        char *key = jstring(&p);
        while (*p && *p != ':') ++p;
        if (*p) ++p;
        while (*p == ' ') ++p;
        if (*p != '"') continue;
        char *val = jstring(&p);
#define KEY(f) else if (!strcmp(key, #f)) cur.f = val
        if (0) {}
        KEY(name); KEY(icon); KEY(version); KEY(author); KEY(type); KEY(description); KEY(date); KEY(titleid);
        KEY(long_description); KEY(size); KEY(url); KEY(data); KEY(requirements); KEY(release_page); KEY(downloads);
        KEY(screenshots);
    }
    return n;
}

static int load_catalog(void) {                       /* worker thread */
    if (spare_ready) return 0;                         /* the last one is not swapped in yet */
    SceUID fd = sceIoOpen(DIR "/apps.json", SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    char *b = size > 0 ? malloc(size + 1) : NULL;
    if (!b) { sceIoClose(fd); return -1; }
    int n = sceIoRead(fd, b, size);
    sceIoClose(fd);
    b[n > 0 ? n : 0] = 0;
    spare_blob = b;
    nspare = parse(b, spare);
    if (!nspare) { free(b); spare_blob = NULL; return -1; }
    spare_ready = 1;
    return 0;
}

static void swap_in(void) {                           /* main thread, never mid-install */
    if (!spare_ready || installing >= 0) return;
    free(blob);
    blob = spare_blob;
    memcpy(apps, spare, nspare * sizeof(App));
    napps = nspare;
    spare_ready = 0;
    filter();
    build_rows();
}

/* ---------- network ---------- */

typedef struct { SceUID fd; long long done, total; int write_error; } Sink;

static size_t on_data(char *ptr, size_t size, size_t n, void *arg) {
    Sink *s = arg;
    int w = sceIoWrite(s->fd, ptr, size * n);
    if (w != (int)(size * n)) { s->write_error = w < 0 ? w : -1; return 0; }
    s->done += w;
    if (s->total) job_frac = (float)s->done / s->total;
    return size * n;
}

static int on_progress(void *arg, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ul, curl_off_t un) {
    (void)ul; (void)un; (void)dlnow;
    Sink *s = arg;
    if (dltotal > 0) s->total = dltotal;
    return 0;
}

static int fetch(const char *url, const char *dest) {
    char part[300];
    snprintf(part, sizeof(part), "%s.part", dest);
    Sink s = {sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666), 0, 0, 0};
    if (s.fd < 0) return s.fd;
    CURL *c = curl_easy_init();
    if (!c) { sceIoClose(s.fd); return -1; }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Home/1.0 (PS Vita)");
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &s);
    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    sceIoClose(s.fd);
    if (rc != CURLE_OK || s.write_error) {
        sceIoRemove(part);
        if (http >= 400) return -(int)http;          /* -404: the file is gone from the server */
        return s.write_error ? -2 : -(int)rc - 1000;
    }
    sceIoRemove(dest);
    return sceIoRename(part, dest);
}

int store_fetch(const char *url, const char *dest) { return fetch(url, dest); }

/* ---------- unzip (the same inflater and checks as the agent installer) ---------- */

static unsigned char inbuf[64 * 1024], dict[TINFL_LZ_DICT_SIZE], cdir[64 * 1024];
static tinfl_decompressor inflator;
static unsigned int rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned int rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24; }

static void mkdirs(const char *file) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", file);
    for (char *q = strchr(tmp + 5, '/'); q; q = strchr(q + 1, '/')) { *q = 0; sceIoMkdir(tmp, 0777); *q = '/'; }
}

static int write_all(SceUID fd, const void *d, unsigned int len) {
    const unsigned char *p = d;
    while (len) { int n = sceIoWrite(fd, p, len); if (n <= 0) return -1; p += n; len -= n; }
    return 0;
}

static int extract_entry(SceUID zip, unsigned int off, unsigned int method, unsigned int csize, unsigned int usize, const char *out_path) {
    SceUID out = sceIoOpen(out_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) return out;
    sceIoLseek(zip, off, SCE_SEEK_SET);
    int rc = 0;
    unsigned int left = csize;
    if (method == 0) {
        while (left && rc >= 0) {
            int n = sceIoRead(zip, inbuf, left > sizeof(inbuf) ? sizeof(inbuf) : left);
            if (n <= 0) { rc = -3; break; }
            rc = write_all(out, inbuf, n);
            left -= n;
        }
    } else if (method == 8) {
        tinfl_init(&inflator);
        size_t in_avail = 0, in_pos = 0, dict_pos = 0;
        unsigned int written = 0;
        for (;;) {
            if (!in_avail && left) {
                int n = sceIoRead(zip, inbuf, left > sizeof(inbuf) ? sizeof(inbuf) : left);
                if (n <= 0) { rc = -3; break; }
                in_avail = n; in_pos = 0; left -= n;
            }
            size_t ib = in_avail, ob = TINFL_LZ_DICT_SIZE - dict_pos;
            tinfl_status st = tinfl_decompress(&inflator, inbuf + in_pos, &ib, dict, dict + dict_pos, &ob,
                                               left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            in_pos += ib; in_avail -= ib;
            if (ob) {
                if (write_all(out, dict + dict_pos, ob) < 0) { rc = -8; break; }
                written += ob;
                dict_pos = (dict_pos + ob) & (TINFL_LZ_DICT_SIZE - 1);
            }
            if (st == TINFL_STATUS_DONE) break;
            if (st < 0) { rc = -4; break; }
            if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !left && !in_avail) { rc = -5; break; }
        }
        if (rc >= 0 && written != usize) rc = -6;
    } else rc = -7;
    sceIoClose(out);
    return rc;
}

static int extract_zip(const char *zip_path, const char *dest) {
    SceUID zip = sceIoOpen(zip_path, SCE_O_RDONLY, 0);
    if (zip < 0) return zip;
    SceOff size = sceIoLseek(zip, 0, SCE_SEEK_END);
    unsigned int tail = size > (SceOff)sizeof(cdir) ? sizeof(cdir) : (unsigned int)size;
    sceIoLseek(zip, size - tail, SCE_SEEK_SET);
    if (sceIoRead(zip, cdir, tail) != (int)tail) { sceIoClose(zip); return -10; }
    int eocd = -1;
    for (int i = (int)tail - 22; i >= 0; --i) if (rd32(cdir + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { sceIoClose(zip); return -11; }
    unsigned int count = rd16(cdir + eocd + 10), pos = rd32(cdir + eocd + 16);
    static char out_path[512];
    static unsigned char hdr[46];
    int rc = 0;
    for (unsigned int done = 0; done < count && rc >= 0; ++done) {
        sceIoLseek(zip, pos, SCE_SEEK_SET);
        if (sceIoRead(zip, hdr, 46) != 46 || rd32(hdr) != 0x02014b50) { rc = -12; break; }
        unsigned int method = rd16(hdr + 10), csize = rd32(hdr + 20), usize = rd32(hdr + 24);
        unsigned int nlen = rd16(hdr + 28), xlen = rd16(hdr + 30), clen = rd16(hdr + 32), local = rd32(hdr + 42);
        char name[400];
        if (nlen >= sizeof(name) || sceIoRead(zip, name, nlen) != (int)nlen) { rc = -13; break; }
        name[nlen] = 0;
        pos += 46 + nlen + xlen + clen;
        int unsafe = name[0] == '/' || strchr(name, ':') != NULL;          /* no escaping the folder */
        for (const char *c = name; *c && !unsafe;) {
            const char *e = c;
            while (*e && *e != '/' && *e != '\\') ++e;
            if (e - c == 2 && c[0] == '.' && c[1] == '.') unsafe = 1;
            c = *e ? e + 1 : e;
        }
        if (unsafe) continue;
        snprintf(out_path, sizeof(out_path), "%s/%s", dest, name);
        mkdirs(out_path);
        if (nlen && name[nlen - 1] == '/') continue;
        unsigned char lh[30];
        sceIoLseek(zip, local, SCE_SEEK_SET);
        if (sceIoRead(zip, lh, 30) != 30 || rd32(lh) != 0x04034b50) { rc = -15; break; }
        rc = extract_entry(zip, local + 30 + rd16(lh + 26) + rd16(lh + 28), method, csize, usize, out_path);
        job_frac = (float)done / count;
    }
    sceIoClose(zip);
    return rc;
}

/* ---------- head.bin and the promoter ---------- */

/* One string value out of sce_sys/param.sfo, by key. TITLE_ID for the
 * promoter, APP_VER (Sony's "NN.NN") for the update check below. */
static int sfo_value(const char *dir, const char *key, char *out, int outmax) {
    char path[300];
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
    static unsigned char sfo[16 * 1024];
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd;
    int n = sceIoRead(fd, sfo, sizeof(sfo));
    sceIoClose(fd);
    if (n < 20 || memcmp(sfo, "\0PSF", 4)) return -1;
    unsigned int keys = rd32(sfo + 8), data = rd32(sfo + 12), count = rd32(sfo + 16);
    for (unsigned int i = 0; i < count && 20 + i * 16 + 16 <= (unsigned int)n; ++i) {
        const unsigned char *e = sfo + 20 + i * 16;
        const char *k = (const char *)sfo + keys + rd16(e);
        if (!strcmp(k, key)) { snprintf(out, outmax, "%s", (const char *)sfo + data + rd32(e + 12)); return 0; }
    }
    return -2;
}

static int sfo_title_id(const char *dir, char *tid) { return sfo_value(dir, "TITLE_ID", tid, 10); }
static int sfo_app_ver(const char *dir, char *ver) { return sfo_value(dir, "APP_VER", ver, 16); }

/* Version compare: the catalogue writes "v.2.9.1" or "1.3"; an installed
 * app's APP_VER is Sony's own "NN.NN" (a literal decimal number, so "1.3"
 * and "01.30" are the same release). Two-part versions compare as real
 * numbers; three-part ones compare component by component. Mixed shapes,
 * or anything that will not parse, are not confident, so no update is
 * ever claimed on a guess. */
static int parse_version(const char *v, int *dots, double *num, int part[3]) {
    if (!v || !*v) return 0;
    if (*v == 'v' || *v == 'V') { ++v; if (*v == '.') ++v; }
    if (!*v) return 0;
    int d = 0;
    for (const char *c = v; *c; ++c) {
        if (*c == '.') ++d;
        else if (*c < '0' || *c > '9') return 0;         /* anything but digits and dots: unparseable */
    }
    if (d > 2) return 0;                                 /* four-part and beyond: not confident */
    *dots = d;
    if (d <= 1) { *num = atof(v); return 1; }
    return sscanf(v, "%d.%d.%d", &part[0], &part[1], &part[2]) == 3;
}

/* 1 when 'installed' is behind 'latest'; 0 otherwise, including "can't tell". */
static int version_older(const char *installed, const char *latest) {
    int di, dl, pi[3], pl[3];
    double ni, nl;
    if (!parse_version(installed, &di, &ni, pi) || !parse_version(latest, &dl, &nl, pl)) return 0;
    if ((di <= 1) != (dl <= 1)) return 0;                /* different shapes: not confident */
    if (di <= 1) return nl > ni + 1e-9;
    for (int k = 0; k < 3; ++k) if (pl[k] != pi[k]) return pl[k] > pi[k];
    return 0;
}

static void package_hash(const unsigned char *data, unsigned int len, unsigned char out[16]) {
    unsigned char d[20], m[64] = {0};
    SHA1(data, len, d);
    memcpy(m, d + 4, 8); memcpy(m + 8, d + 4, 8); memcpy(m + 16, d + 12, 4); m[20] = d[16];
    memcpy(m + 21, d + 1, 3); memcpy(m + 24, m + 16, 8);
    unsigned char h[20];
    SHA1(m, 64, h);
    memcpy(out, h, 16);
}

static unsigned int be32(const unsigned char *p) { return (unsigned int)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* VitaShell's recipe: the content id comes from TITLE_ID, then three hashes. */
static int write_head_bin(const char *dir, const char *tid) {
    char path[300];
    SceIoStat st;
    snprintf(path, sizeof(path), "%s/sce_sys/package/head.bin", dir);
    if (sceIoGetstat(path, &st) >= 0) return 0;
    unsigned char h[sizeof(HEAD_BIN)];
    memcpy(h, HEAD_BIN, sizeof(h));
    char content[48] = {0};
    snprintf(content, sizeof(content), "EP9000-%s_00-0000000000000000", tid);
    memcpy(h + 0x30, content, 48);
    package_hash(h, be32(h + 0xD0), h + be32(h + 0xD0));
    package_hash(h + be32(h + 8), be32(h + 0x10) - 64, h + be32(h + 0xD4));
    package_hash(h, be32(h + 0xE8), h + be32(h + 0xE8));
    mkdirs(path);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return fd;
    int rc = write_all(fd, h, sizeof(h));
    sceIoClose(fd);
    return rc;
}

static int promoter_ready;

static int promoter_up(void) {
    if (!promoter_ready) {
        unsigned int paf_args[] = {0x180000, -1, -1, 1, -1, -1};
        int entry = -1;
        SceSysmoduleOpt opt = {sizeof(opt), &entry, {-1, -1}};
        int rc = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(paf_args), paf_args, &opt);
        if (rc < 0 && rc != (int)0x805A1002) return rc;               /* already loaded is fine */
        if ((rc = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL)) < 0) return rc;
        if ((rc = scePromoterUtilityInit()) < 0) return rc;
        promoter_ready = 1;
    }
    return 0;
}

static int promote(const char *dir) {
    int rc = promoter_up();
    return rc < 0 ? rc : scePromoterUtilityPromotePkgWithRif(dir, 1);
}

/* Uninstalling: the promoter again, on its own thread (it takes seconds). */
static char un_tid[10], un_name[64];
static volatile int un_busy;

static int uninstall_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int rc = promoter_up();
    if (rc >= 0) rc = scePromoterUtilityDeletePkg(un_tid);
    char msg[120];
    if (rc < 0) snprintf(msg, sizeof(msg), "Could not delete %.50s (0x%08X)", un_name, rc);
    else snprintf(msg, sizeof(msg), "Deleted %.60s", un_name);
    ui_toast(msg, rc < 0 ? C_BAD : C_OK);
    apps_prewarm();                                   /* the Apps grid rescans */
    un_busy = 0;
    return sceKernelExitDeleteThread(0);
}

int store_uninstall(const char *tid, const char *name) {
    if (un_busy || strlen(tid) != 9 || !strncmp(tid, "MVZA", 4) || !strncmp(tid, "NPXS", 4)) return -1;
    snprintf(un_tid, sizeof(un_tid), "%s", tid);
    snprintf(un_name, sizeof(un_name), "%s", name);
    un_busy = 1;
    SceUID t = sceKernelCreateThread("uninstall", uninstall_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) { un_busy = 0; return -1; }
    return 0;
}

static void remove_tree(const char *dir) {
    SceUID d = sceIoDopen(dir);
    if (d >= 0) {
        SceIoDirent e;
        char p[512];
        for (;;) {
            memset(&e, 0, sizeof(e));
            if (sceIoDread(d, &e) <= 0) break;
            snprintf(p, sizeof(p), "%s/%s", dir, e.d_name);
            if (SCE_S_ISDIR(e.d_stat.st_mode)) remove_tree(p); else sceIoRemove(p);
        }
        sceIoDclose(d);
    }
    sceIoRmdir(dir);
}

static void install(App *a) {
    char vpk[256], pkg[256], tid[10] = {0};
    snprintf(vpk, sizeof(vpk), DIR "/dl.vpk");
    snprintf(pkg, sizeof(pkg), DIR "/pkg");
    remove_tree(pkg);
    job_stage = 1; job_frac = 0;
    snprintf(job_msg, sizeof(job_msg), "Downloading");
    int rc = fetch(a->url, vpk);
    if (rc == -404 || rc == -410) {
        snprintf(job_msg, sizeof(job_msg), "The author took this download down (%d). Nothing to install.", -rc);
        job_stage = 9; return;
    }
    if (rc <= -400 && rc > -600) { snprintf(job_msg, sizeof(job_msg), "The server refused the download (%d).", -rc); job_stage = 9; return; }
    if (rc == -2) { snprintf(job_msg, sizeof(job_msg), "The memory card could not be written."); job_stage = 9; return; }
    if (rc < 0) { snprintf(job_msg, sizeof(job_msg), "Could not reach the server. Check Wi-Fi and try again."); job_stage = 9; return; }
    job_stage = 2; job_frac = 0;
    snprintf(job_msg, sizeof(job_msg), "Unpacking");
    rc = extract_zip(vpk, pkg);
    sceIoRemove(vpk);
    if (rc < 0) { snprintf(job_msg, sizeof(job_msg), "Could not unpack (%d)", rc); job_stage = 9; remove_tree(pkg); return; }
    if (sfo_title_id(pkg, tid) < 0 || strlen(tid) != 9 || !strncmp(tid, "MVZA", 4) || !strncmp(tid, "NPXS", 4)) {
        snprintf(job_msg, sizeof(job_msg), "Refused: title ID %s", tid[0] ? tid : "missing");
        job_stage = 9; remove_tree(pkg); return;
    }
    job_stage = 3;
    snprintf(job_msg, sizeof(job_msg), "Installing %s", tid);
    rc = write_head_bin(pkg, tid);
    if (rc >= 0) rc = promote(pkg);
    if (rc < 0) { snprintf(job_msg, sizeof(job_msg), "Install failed (0x%08X)", rc); job_stage = 9; remove_tree(pkg); return; }
    /* The promoter works in the background; wait until it lets go of the folder. */
    for (int i = 0; i < 600; ++i) {
        int state = 0;
        if (scePromoterUtilityGetState(&state) < 0 || !state) break;
        sceKernelDelayThread(100 * 1000);
    }
    int result = 0;
    scePromoterUtilityGetResult(&result);
    remove_tree(pkg);
    if (result < 0) { snprintf(job_msg, sizeof(job_msg), "Install failed (0x%08X)", result); job_stage = 9; return; }
    snprintf(job_msg, sizeof(job_msg), "Installed: it is first in Apps");
    job_stage = 4;
    apps_prewarm();                                   /* new apps go to the top of the grid */
    char toast[96];
    snprintf(toast, sizeof(toast), "Installed %.60s", a->name);
    ui_toast(toast, C_OK);
}

float store_job_frac(void) { return job_frac; }   /* the last download's progress, 0..1 */

/* For VitaOS's own update (update.c): unpack a VPK and write its head.bin,
 * ready for the promoter; the title ID comes back in tid. */
int store_prepare_pkg(const char *vpk, const char *pkg, char tid[10]) {
    remove_tree(pkg);
    int rc = extract_zip(vpk, pkg);
    if (rc < 0) { remove_tree(pkg); return rc; }
    memset(tid, 0, 10);
    if (sfo_title_id(pkg, tid) < 0 || strlen(tid) != 9) { remove_tree(pkg); return -3; }
    rc = write_head_bin(pkg, tid);
    if (rc < 0) remove_tree(pkg);
    return rc;
}

/* Install a prepared folder (used for the updater app, never VitaOS itself). */
int store_install_dir(const char *pkg) {
    int rc = promote(pkg);
    if (rc < 0) return rc;
    for (int i = 0; i < 600; ++i) {
        int state = 0;
        if (scePromoterUtilityGetState(&state) < 0 || !state) break;
        sceKernelDelayThread(100 * 1000);
    }
    int result = 0;
    scePromoterUtilityGetResult(&result);
    remove_tree(pkg);
    return result;
}

static int worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    sceIoMkdir(DIR, 0777);
    sceIoMkdir(DIR "/icons", 0777);
    if (load_catalog() >= 0) catalog_state = 2;       /* the saved list first: the store opens at once */
    want_catalog = 1;                                  /* then a fresh one */
    for (;;) {
        if (installing >= 0) { install(&apps[installing]); installing = -1; }
        if (want_catalog) {
            want_catalog = 0;
            if (catalog_state != 2) catalog_state = 1;
            if (fetch(CATALOG_URL, DIR "/apps.json") >= 0 && load_catalog() >= 0) catalog_state = 2;
            else if (catalog_state != 2) catalog_state = 3;
        }
        if (shot_want >= 0 && shot_want < napps) {     /* the detail page's screenshots, up to three */
            App *a = &apps[shot_want];
            shot_want = -1;
            sceIoMkdir(DIR "/shots", 0777);
            char list[512];
            snprintf(list, sizeof(list), "%s", a->screenshots);
            int k = 0;
            for (char *s = strtok(list, ";"); s && k < 3; s = strtok(NULL, ";"), ++k) {
                const char *base = strrchr(s, '/') ? strrchr(s, '/') + 1 : s;
                char url[300], dest[256];
                SceIoStat st;
                snprintf(dest, sizeof(dest), DIR "/shots/%s", base);
                if (sceIoGetstat(dest, &st) < 0) {
                    snprintf(url, sizeof(url), BASE_URL "%s", s);
                    fetch(url, dest);
                }
            }
            a->shots_state = 2;
        }
        while (iq_tail != iq_head) {                   /* icons: on the card already, or fetched */
            int i = icon_queue[iq_tail % 16];
            if (i >= 0 && i < napps) {
                char url[256], dest[256];
                SceIoStat st;
                snprintf(dest, sizeof(dest), DIR "/icons/%s", apps[i].icon);
                if (sceIoGetstat(dest, &st) >= 0) apps[i].icon_state = 2;
                else {
                    snprintf(url, sizeof(url), ICON_URL "%s", apps[i].icon);
                    apps[i].icon_state = fetch(url, dest) < 0 ? 3 : 2;
                }
            }
            iq_tail++;
            if (installing >= 0 || shot_want >= 0) break;  /* an install, or the open page's pictures, go first */
        }
        sceKernelWaitSema(wake, 1, NULL);
    }
    return 0;
}

static void kick(void) { if (wake >= 0) sceKernelSignalSema(wake, 1); }

void store_init(void) {
    wake = sceKernelCreateSema("store", 0, 0, 1, NULL);
    SceUID t = sceKernelCreateThread("store", worker, 0x10000100, 0x10000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* ---------- screen ---------- */

static vita2d_texture *icon_of(App *a) {
    if (!a->icon[0] || a->icon_state == 3) return NULL;
    if (a->icon_state == 0 && iq_head - iq_tail < 16) {   /* the worker checks the card, or downloads it */
        icon_queue[iq_head % 16] = (int)(a - apps);
        iq_head++;
        a->icon_state = 1;
        kick();
    }
    if (a->icon_state != 2) return NULL;
    char path[256];
    snprintf(path, sizeof(path), DIR "/icons/%s", a->icon);
    return ui_image(path);
}


/* ---------- the store's pages ---------- */

static void short_count(const char *n, char *out, int max) {
    long v = atol(n);
    if (v >= 1000000) snprintf(out, max, "%.1fM", v / 1e6);
    else if (v >= 10000) snprintf(out, max, "%ldK", (v + 500) / 1000);
    else if (v >= 1000) snprintf(out, max, "%.1fK", v / 1e3);
    else snprintf(out, max, "%ld", v);
}

static const char *type_name(const App *a) {
    switch (atoi(a->type)) {
    case 1: return "Game";
    case 2: return "Port";
    case 4: return "Utility";
    case 5: return "Emulator";
    }
    return "App";
}

/* inst: 0 not checked, 1 not installed, 2 installed and current, 3 installed
 * but behind the catalogue. The APP_VER read only happens for apps that are
 * actually on the Vita, and only once (cached here like the install check). */
static int is_installed(App *a) {
    if (!a->inst && inst_budget > 0) {
        --inst_budget;
        char dir[48], p[64], ver[16];
        SceIoStat st;
        snprintf(dir, sizeof(dir), "ux0:app/%s", a->titleid);
        snprintf(p, sizeof(p), "%s/eboot.bin", dir);
        if (!a->titleid[0] || sceIoGetstat(p, &st) < 0) a->inst = 1;
        else a->inst = sfo_app_ver(dir, ver) >= 0 && version_older(ver, a->version) ? 3 : 2;
    }
    return a->inst >= 2;
}

static int has_update(App *a) { is_installed(a); return a->inst == 3; }

/* For a badge on the Store tab: how many installed apps are behind the
 * catalogue. Cheap once the catalogue and inst cache are warm (an int
 * compare per app); the first pass over an unchecked app still costs an
 * sceIoGetstat and, if installed, an SFO read. */
int store_updates_count(void) {
    int n = 0;
    for (int i = 0; i < napps; ++i) if (has_update(&apps[i])) ++n;
    return n;
}

static void draw_icon(App *a, float x, float y, float s) {
    icon_of(a);                                       /* makes sure it is on the card */
    if (a->icon_state == 2) {
        char path[256];
        snprintf(path, sizeof(path), DIR "/icons/%s", a->icon);
        draw_app_icon(path, x, y, s, a->name, 0);
    } else if (a->icon_state == 3 || !a->icon[0]) draw_app_icon("", x, y, s, a->name, 0);
    else { draw_round_rect(x, y, s, s, s * 0.22f, RGBA8(36, 41, 56, 255)); draw_shimmer(x + s * 0.1f, y, s * 0.8f, s); }
}

/* The k-th screenshot, once the worker has it on the card (asks for it if not). */
static vita2d_texture *shot_of(App *a, int k) {
    if (!a->screenshots[0]) return NULL;
    if (a->shots_state == 0 && shot_want < 0) { a->shots_state = 1; shot_want = (int)(a - apps); kick(); }
    if (a->shots_state != 2) return NULL;
    const char *s = a->screenshots;
    for (int i = 0; i < k && s; ++i) { s = strchr(s, ';'); if (s) ++s; }
    if (!s || !*s) return NULL;
    const char *end = strchr(s, ';');
    char one[200], path[256];
    snprintf(one, sizeof(one), "%.*s", end ? (int)(end - s) : (int)strlen(s), s);
    const char *base = strrchr(one, '/') ? strrchr(one, '/') + 1 : one;
    snprintf(path, sizeof(path), DIR "/shots/%s", base);
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) return NULL;
    return ui_image(path);
}

static void open_detail(int app) { cur = app; detail = 1; job_stage = 0; }

void store_leave(void) { if (installing < 0) { detail = 0; job_stage = 0; } chips = 0; }

const char *store_hint(void) {
    if (detail) return installing >= 0 ? "Installing\xE2\x80\xA6" : has_update(&apps[cur]) ? "X update    O back" : "X install    O back";
    if (chips) return "\xE2\x86\x90 \xE2\x86\x92  section    X back to the apps    L R tabs";
    if (cat == C_FORYOU) return "X details    \xE2\x86\x91 \xE2\x86\x93 rows    [] refresh    O downloads    L R tabs";
    if (cat == C_TOP) return "X details    [] refresh    O downloads    L R tabs";
    return sort_new ? "X details    /\\ sort: newest    [] refresh    O downloads    L R tabs"
                    : "X details    /\\ sort: popular    [] refresh    O downloads    L R tabs";
}

/* The detail page, laid out like a phone store's: who made it, the numbers,
 * one big Install button, what it is, and pictures of it. */
static int detail_page(const Input *in, unsigned int p) {
    App *a = &apps[cur];
    int installed = is_installed(a);
    int upd = has_update(a);
    const char *verb = upd ? "Update" : installed ? "Reinstall" : "Install";
    if (in->tapped && installing < 0 && in->tap_x >= 40 && in->tap_x < 460 && in->tap_y >= 250 && in->tap_y < 294) p |= SCE_CTRL_CROSS;
    if (p & SCE_CTRL_CIRCLE && installing < 0) { detail = 0; job_stage = 0; return 1; }
    if (p & SCE_CTRL_CROSS && installing < 0 && job_stage != 4) {
        int is_vpk = strlen(a->url) > 4 && !strcasecmp(a->url + strlen(a->url) - 4, ".vpk");
        if (denied(a)) ui_message("Not installing this one", "It wedged the shell on this Vita before (2026-09-19).");
        else if (!is_vpk) ui_message("Not a VPK", "This one downloads as an archive; install it by hand.");
        else {
            char msg[300];
            snprintf(msg, sizeof(msg), "%s %s %s (%.1f MB)?%s", verb, a->name, a->version,
                     atoi(a->size) / 1048576.0f, a->data[0] ? " It also needs data files that are not installed automatically." : "");
            if (ui_confirm(verb, msg)) { installing = cur; job_stage = 1; job_msg[0] = 0; kick(); }
        }
    }
    if (job_stage == 4) a->inst = 2;
    vita2d_draw_rectangle(0, 65, W, H - 105, RGBA8(21, 24, 33, 240));
    ui_theme_from(icon_of(a));

    /* left: name, maker, numbers, Install, about */
    draw_icon(a, 40, 84, 96);
    text_fit(bold, 152, 114, C_TEXT, 24, a->name, 310);
    text_fit(bold, 152, 139, C_ACCENT, 15, a->author, 310);
    char meta[96];
    snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %s", type_name(a), a->version);
    text_fit(font, 152, 160, C_DIM, 13, meta, 310);
    char dl[16], mb[16];
    short_count(a->downloads, dl, sizeof(dl));
    snprintf(mb, sizeof(mb), "%.1f MB", atoi(a->size) / 1048576.0f);
    const char *big[3] = {dl, mb, type_name(a)}, *small[3] = {"downloads", "size", "category"};
    for (int k = 0; k < 3; ++k) {
        int cx = 40 + 70 + k * 140;
        if (k) vita2d_draw_rectangle(40 + k * 140, 204, 1, 30, RGBA8(255, 255, 255, 30));
        int tw = text_w(bold, 16, big[k]);
        text(bold, cx - tw / 2, 218, C_TEXT, 16, big[k]);
        tw = text_w(font, 12, small[k]);
        text(font, cx - tw / 2, 236, C_FAINT, 12, small[k]);
    }
    int by = 250;
    if (installing >= 0 || (job_stage && job_stage != 4)) {
        text(font, 40, by + 20, job_stage == 9 ? C_BAD : C_TEXT, 16, job_msg);
        if (job_stage >= 1 && job_stage <= 3) draw_bar(40, by + 32, 420, 5, job_stage == 3 ? ui_pulse() : job_frac, C_ACCENT);
    } else if (denied(a)) {
        draw_action_button(40, by, 420, 44, "Blocked on this Vita", 1, C_BAD);
    } else if (upd) {
        char label[48];
        snprintf(label, sizeof(label), "X Update to %s", a->version);
        draw_focus_r(40, by, 420, 44, 1, 22);
        draw_action_button(40, by, 420, 44, label, 1, C_ACCENT);
    } else if (installed || job_stage == 4) {
        draw_round_rect(40, by, 420, 44, 22, RGBA8(255, 255, 255, 22));
        draw_hints_centered(250, by + 22, job_stage == 4 ? "Installed: it is first in Apps" : "Installed    X reinstall", C_TEXT);
    } else {
        draw_focus_r(40, by, 420, 44, 1, 22);
        draw_action_button(40, by, 420, 44, "X Install", 1, RGBA8(52, 168, 83, 255));
    }
    int y = 324;
    text(bold, 40, y, C_TEXT, 17, "About this app");
    y += 22;
    draw_wrapped_text(a->long_description[0] ? a->long_description : a->description, 40, y, 420, 14, 4, C_DIM);
    y += 4 * 18 + 6;
    /* What else it needs, one line each (the catalogue writes them as "- x\n- y") */
    if (a->requirements[0] || a->data[0]) {
        text(bold, 500, 344, C_MARK, 14, "Also needs");
        int ny = 366, lines = 0;
        const char *s = a->requirements;
        while (*s && lines < 5) {
            const char *e = strchr(s, '\n');
            int n = e ? (int)(e - s) : (int)strlen(s);
            char line[160];
            snprintf(line, sizeof(line), "%.*s", n < 159 ? n : 159, s);
            if (line[0]) { text_fit(font, 500, ny, C_TEXT, 13, line, 420); ny += 19; lines++; }
            s = e ? e + 1 : s + n;
        }
        if (a->data[0] && lines < 6) text_fit(font, 500, ny, C_DIM, 13, "Data files: not installed automatically", 420);
    }

    /* right: the screenshots */
    int needs = a->requirements[0] || a->data[0];
    vita2d_texture *s0 = shot_of(a, 0);
    if (s0) {
        draw_round_texture(s0, 500, 84, 420, 238, 14, 0xFFFFFFFF);
        for (int k = 1; k < 3 && !needs; ++k) {        /* the extra pictures, when nothing else needs the space */
            vita2d_texture *s = shot_of(a, k);
            if (s) draw_round_texture(s, 500 + (k - 1) * 215, 334, 205, 116, 10, 0xFFFFFFFF);
        }
    } else if (a->screenshots[0]) {
        draw_round_rect(500, 84, 420, 238, 14, RGBA8(255, 255, 255, 10));
        text(font, 710 - text_w(font, 14, "Loading screenshots") / 2, 208, C_FAINT, 14, "Loading screenshots");
    } else {
        draw_round_rect(500, 84, 420, 238, 14, RGBA8(255, 255, 255, 10));
        draw_icon(a, 500 + 210 - 48, 84 + 119 - 48, 96);
        text_right(font, 910, 310, C_FAINT, 12, "No screenshots yet");
    }
    return 1;
}

/* One small app card: icon, name, kind and downloads. */
static void card(App *a, float x, float y, int on) {
    if (on) draw_focus_r(x, y, 100, 100, 1, 22);
    draw_icon(a, x, y, 100);
    text_fit(on ? bold : font, (int)x, (int)y + 120, on ? C_TEXT : C_DIM, 14, a->name, 122);
    char sub[48], dl[16];
    short_count(a->downloads, dl, sizeof(dl));
    int upd = has_update(a);
    snprintf(sub, sizeof(sub), upd ? "Update" : is_installed(a) ? "Installed" : "%s  \xC2\xB7  %s \xE2\x86\x93", type_name(a), dl);
    text_fit(font, (int)x, (int)y + 138, upd ? C_ACCENT : is_installed(a) ? C_OK : C_FAINT, 12, sub, 122);
}

#define BANNER_H 176
#define ROW_H 196
static float row_y(int r) { return r == 0 ? 0 : BANNER_H + 14 + (r - 1) * ROW_H; }

static void for_you(const Input *in, unsigned int p) {
    if (!chips) {
        if (p & SCE_CTRL_DOWN) { int r = rrow + 1; while (r < NROWS && !nrow[r]) r++; if (r < NROWS) rrow = r; }
        if (p & SCE_CTRL_UP) { if (rrow == 0) chips = 1; else { int r = rrow - 1; while (r > 0 && !nrow[r]) r--; rrow = r; } }
        if (p & SCE_CTRL_LEFT && rcol[rrow] > 0) rcol[rrow]--;
        if (p & SCE_CTRL_RIGHT && rcol[rrow] < nrow[rrow] - 1) rcol[rrow]++;
        if (p & SCE_CTRL_CROSS && nrow[rrow]) open_detail(rows[rrow][rcol[rrow]]);
    }
    /* vertical: keep the focused row whole; a finger drags the page */
    static float vt;
    float area = H - 40 - 120, total = row_y(NROWS - 1) + ROW_H, vmax = total > area ? total - area : 0;
    float ry = row_y(rrow);
    if (p & (SCE_CTRL_UP | SCE_CTRL_DOWN)) { if (ry < vt) vt = ry; if (ry + ROW_H > vt + area) vt = ry + ROW_H - area; }
    static int drag_row = -1;
    if (in->touching && drag_row < 0 && (in->drag_dx || in->drag_dy)) {
        drag_row = abs(in->drag_dx) > abs(in->drag_dy) ? 1 : 0;   /* 1: sideways, along a row */
    }
    if (!in->touching) drag_row = -1;
    int touch_r = -1;
    for (int r = 0; r < NROWS; ++r) { float y = 120 + row_y(r) - vscroll; if (in->ty >= y && in->ty < y + ROW_H) touch_r = r; }
    if (in->touching && drag_row == 0) vt -= in->drag_dy;
    if (in->touching && drag_row == 1 && touch_r > 0) {
        rscroll[touch_r] -= in->drag_dx / 140.0f;
        if (rscroll[touch_r] < 0) rscroll[touch_r] = 0;
        if (rscroll[touch_r] > nrow[touch_r] - 5) rscroll[touch_r] = nrow[touch_r] > 5 ? nrow[touch_r] - 5 : 0;
    }
    if (vt < 0) vt = 0;
    if (vt > vmax) vt = vmax;
    vscroll += (vt - vscroll) * (in->touching ? 1.0f : 0.25f);

    /* the featured banner */
    if (nrow[0]) {
        App *f = &apps[rows[0][rcol[0]]];
        float y = 120 + row_y(0) - vscroll;
        int on = rrow == 0 && !chips;
        if (in->tapped && in->tap_y >= y && in->tap_y < y + BANNER_H && in->tap_y > 116) open_detail(rows[0][rcol[0]]);
        if (on) draw_focus_r(40, y, 880, BANNER_H - 10, 1, 20);
        draw_round_gradient(40, y, 880, BANNER_H - 10, 20, RGBA8(40, 52, 96, 255), RGBA8(26, 30, 46, 255));
        vita2d_texture *s = shot_of(f, 0);
        if (s) draw_round_texture(s, 620, y + 12, 284, 142, 12, 0xFFFFFFFF);
        draw_icon(f, 64, y + 26, 84);
        text(bold, 170, (int)y + 40, C_ACCENT, 12, "FEATURED");
        text_fit(bold, 170, (int)y + 70, C_TEXT, 26, f->name, 420);
        draw_wrapped_text(f->description, 170, (int)y + 98, 420, 14, 2, C_DIM);
        char dl[16], line[64];
        short_count(f->downloads, dl, sizeof(dl));
        snprintf(line, sizeof(line), "%s  \xC2\xB7  %s downloads", type_name(f), dl);
        text(font, 170, (int)y + 146, C_FAINT, 13, line);
        for (int k = 0; k < nrow[0]; ++k)                             /* which of the featured */
            vita2d_draw_fill_circle(64 + k * 14, y + BANNER_H - 26, k == rcol[0] ? 4 : 3,
                                    k == rcol[0] ? C_TEXT : RGBA8(255, 255, 255, 60));
    }
    /* the rows */
    for (int r = 1; r < NROWS; ++r) {
        if (!nrow[r]) continue;
        float y = 120 + row_y(r) - vscroll;
        if (y > H - 40 || y + ROW_H < 110) continue;
        text(bold, 40, (int)y + 24, C_TEXT, 19, row_titles[r]);
        float target = rcol[r] > 4 ? rcol[r] - 4 : 0;
        if (!(in->touching && touch_r == r)) rscroll[r] += (target - rscroll[r]) * 0.25f;
        for (int k = 0; k < nrow[r]; ++k) {
            float x = 40 + (k - rscroll[r]) * 140;
            if (x < -120 || x > W) continue;
            App *a = &apps[rows[r][k]];
            if (in->tapped && in->tap_x >= x && in->tap_x < x + 124 && in->tap_y >= y + 38 && in->tap_y < y + 186 && in->tap_y > 116)
                open_detail(rows[r][k]);
            card(a, x, y + 40, rrow == r && rcol[r] == k && !chips);
        }
    }
}

static void top_charts(const Input *in, unsigned int p) {
    if (!chips) {
        if (p & SCE_CTRL_LEFT && sel % 2) sel--;
        if (p & SCE_CTRL_RIGHT && !(sel % 2) && sel + 1 < nview) sel++;
        if (p & SCE_CTRL_UP) { if (sel >= 2) sel -= 2; else chips = 1; }
        if (p & SCE_CTRL_DOWN && sel + 2 < nview) sel += 2;
        if (p & SCE_CTRL_CROSS && nview) open_detail(view[sel]);
    }
    static GridScroll gs;
    if (!chips) grid_scroll(&gs, &sel, 2, nview, 5, 74, in);
    if (in->tapped && in->tap_y > 116) {
        int col = in->tap_x < 480 ? 0 : 1, k = (int)(gs.top + (in->tap_y - 120) / 74.0f) * 2 + col;
        if (k >= 0 && k < nview) { sel = k; open_detail(view[k]); }
    }
    for (int k = 0; k < nview; ++k) {
        float y = 120 + (k / 2 - gs.top) * 74;
        if (y < 110 - 74 || y > H - 40) continue;
        int x = 40 + (k % 2) * 450, on = k == sel && !chips;
        App *a = &apps[view[k]];
        if (on) draw_round_rect(x - 10, y - 4, 440, 70, 14, RGBA8(255, 255, 255, 18));
        char rank[8];
        snprintf(rank, sizeof(rank), "%d", k + 1);
        text(bold, x, (int)y + 38, on ? C_TEXT : C_DIM, 17, rank);
        draw_icon(a, x + 40, y + 4, 54);
        text_fit(on ? bold : font, x + 108, (int)y + 26, C_TEXT, 16, a->name, 230);
        char sub[64], mb[16];
        snprintf(mb, sizeof(mb), "%.1f MB", atoi(a->size) / 1048576.0f);
        snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", type_name(a), mb);
        text_fit(font, x + 108, (int)y + 46, C_FAINT, 13, sub, 230);
        if (has_update(a)) text_right(font, x + 420, (int)y + 36, C_ACCENT, 13, "Update");
        else if (is_installed(a)) text_right(font, x + 420, (int)y + 36, C_OK, 13, "Installed");
        else {
            char dl[16];
            short_count(a->downloads, dl, sizeof(dl));
            text_right(bold, x + 420, (int)y + 30, C_TEXT, 15, dl);
            text_right(font, x + 420, (int)y + 47, C_FAINT, 11, "downloads");
        }
    }
}

static void category_grid(const Input *in, unsigned int p) {
    if (!chips) {
        if (p & SCE_CTRL_LEFT) sel = sel > 0 ? sel - 1 : 0;
        if (p & SCE_CTRL_RIGHT) sel = sel < nview - 1 ? sel + 1 : sel;
        if (p & SCE_CTRL_UP) { if (sel >= 6) sel -= 6; else chips = 1; }
        if (p & SCE_CTRL_DOWN) sel = sel + 6 < nview ? sel + 6 : nview - 1;
        if (p & SCE_CTRL_CROSS && nview) open_detail(view[sel]);
    }
    static GridScroll gs;
    if (!chips) grid_scroll(&gs, &sel, 6, nview, 2, 180, in);
    if (in->tapped && in->tap_y > 116) {
        int col = (in->tap_x - 40) / 148, k = (int)(gs.top + (in->tap_y - 124) / 180.0f) * 6 + col;
        if (col >= 0 && col < 6 && k >= 0 && k < nview) { sel = k; open_detail(view[k]); }
    }
    if (grow_sel != sel) { grow_sel = sel; grow = 0; }
    grow = grow < 1 ? grow + 0.09f : 1;
    for (int k = 0; k < nview; ++k) {
        float y = 124 + (k / 6 - gs.top) * 180;
        if (y < 110 - 180 || y > H - 40) continue;
        card(&apps[view[k]], 40 + (k % 6) * 148, y, k == sel && !chips);
    }
}

int store_update(const Input *in) {
    STAGE("store: update");
    inst_budget = 3;
    unsigned int p = in->pressed;
    if (spare_ready && installing < 0 && !detail) { iq_tail = iq_head; swap_in(); }
    if (detail) return detail_page(in, p);

    if (p & SCE_CTRL_SQUARE) { want_catalog = 1; kick(); ui_toast("Refreshing the store", C_ACCENT); }
    if (catalog_state == 3 && (p & SCE_CTRL_SQUARE)) catalog_state = 1;
    if (p & SCE_CTRL_TRIANGLE && cat >= 2) { sort_new = !sort_new; filter(); ui_toast(sort_new ? "Newest first" : "Most popular first", C_ACCENT); }
    if (p & SCE_CTRL_CIRCLE) return 0;                              /* to the downloads list */
    if (!napps) {
        text(font, 40, 150, C_DIM, 18, catalog_state == 3 ? "The store could not be reached. [] tries again." : "Loading the store\xE2\x80\xA6");
        return 1;
    }
    if (!nrow[1]) build_rows();
    /* the section chips: UP from the top reaches them, left/right switch, DOWN (or X) goes back */
    if (chips) {
        if (p & SCE_CTRL_LEFT && cat > 0) { cat--; filter(); }
        if (p & SCE_CTRL_RIGHT && cat < NCATS - 1) { cat++; filter(); }
        if (p & (SCE_CTRL_DOWN | SCE_CTRL_CROSS)) chips = 0;
        p = 0;
    }
    Input in2 = *in;
    int chx = 40;
    for (int c = 0; c < NCATS; ++c) {                              /* chip taps first */
        int w = text_w(font, 15, cat_names[c]) + 30;
        if (in->tapped && in->tap_x >= chx && in->tap_x < chx + w && in->tap_y >= 70 && in->tap_y < 114) { cat = c; filter(); chips = 0; in2.tapped = 0; }
        chx += w + 10;
    }
    if (in2.tapped && in2.tap_y < 116) in2.tapped = 0;
    if (cat == C_FORYOU) {
        ui_theme_default();
        for_you(&in2, p);
    } else if (cat == C_UPDATES && !nview) {
        text(font, 40, 150, C_DIM, 18, "Everything is up to date");
    } else {
        if (cat == C_TOP || cat == C_UPDATES) top_charts(&in2, p); else category_grid(&in2, p);
        ui_theme_from(sel < nview ? icon_of(&apps[view[sel]]) : NULL);
    }
    /* the chips last, on a band of background, so rows scrolled up pass under them */
    draw_gradient(0, 65, W, 52, C_BG, C_BG, C_BG, (C_BG & 0x00FFFFFF) | 0xE0000000);
    int cx = 40;
    for (int c = 0; c < NCATS; ++c) {
        int w = text_w(font, 15, cat_names[c]) + 30;
        if (chips && c == cat) draw_focus(cx, 78, w, 30, 1);
        draw_round_rect(cx, 78, w, 30, 15, c == cat ? RGBA8(245, 245, 250, 255) : RGBA8(255, 255, 255, 26));
        text(font, cx + 15, 99, c == cat ? RGBA8(15, 15, 20, 255) : C_TEXT, 15, cat_names[c]);
        cx += w + 10;
    }
    char count[32];
    snprintf(count, sizeof(count), "%d apps", napps);
    text_right(font, W - 40, 99, C_FAINT, 14, count);
    return 1;
}

/* ---------- from elsewhere: news that names a store app ---------- */

static void lower_simple(const char *in, char *out, int max) {
    int o = 0;
    for (; *in && o < max - 1; ++in) {
        char c = *in >= 'A' && *in <= 'Z' ? *in + 32 : *in;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
        else if (o && out[o - 1] != ' ') out[o++] = ' ';
    }
    while (o && out[o - 1] == ' ') --o;
    out[o] = 0;
}

/* The store app a headline is about: the longest app name (without "Vita")
 * that appears in it as whole words. -1 when none does. */
static int store_match_uncached(const char *headline);

/* Home asks every frame, for every news tile: remember the answers (the scan
 * over ~1,100 names cost Home ~10 ms a frame, 60 fps down to 39). */
int store_match(const char *headline) {
    static struct { unsigned int hash; int napps, result; } memo[8];
    static int next;
    unsigned int h = 2166136261u;
    for (const char *c = headline; *c; ++c) h = (h ^ (unsigned char)*c) * 16777619u;
    for (int i = 0; i < 8; ++i) if (memo[i].hash == h && memo[i].napps == napps) return memo[i].result;
    int r = store_match_uncached(headline);
    memo[next].hash = h; memo[next].napps = napps; memo[next].result = r;
    next = (next + 1) % 8;
    return r;
}

static int store_match_uncached(const char *headline) {
    char h[256];
    lower_simple(headline, h + 1, sizeof(h) - 2);
    h[0] = ' ';
    strcat(h, " ");
    int best = -1, best_len = 0;
    for (int i = 0; i < napps; ++i) {
        char n[128], k[132];
        lower_simple(apps[i].name, n, sizeof(n));
        char *v = strstr(n, " vita");
        if (v && !v[5]) *v = 0;                         /* "Hollow Knight Vita" -> "hollow knight" */
        if (!strncmp(n, "vita ", 5)) memmove(n, n + 5, strlen(n + 5) + 1);
        int len = strlen(n);
        if (len < 5 || len <= best_len) continue;
        snprintf(k, sizeof(k), " %s ", n);
        if (strstr(h, k)) { best = i; best_len = len; }
    }
    return best;
}

void store_show(int app) {
    if (app < 0 || app >= napps || installing >= 0) return;
    open_detail(app);
}
