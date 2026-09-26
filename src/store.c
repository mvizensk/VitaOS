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
#define MAX_APPS 1400

typedef struct {
    char *name, *icon, *version, *author, *type, *description, *date, *titleid, *long_description,
         *size, *url, *data, *requirements, *release_page, *downloads;
    volatile int icon_state;                  /* 0 not asked, 1 queued, 2 on the card, 3 failed, 4 loaded */
} App;

static App apps[MAX_APPS];
static int napps, view[MAX_APPS], nview;
static char *blob;
static int cat;                      /* 0 all, then the VitaDB types below */
static const char *const cat_names[] = {"All", "Games", "Ports", "Utilities", "Emulators"};
static const char *const cat_types[] = {NULL, "1", "2", "4", "5"};
static int sel, detail, chips;
static float top;
static int grow_sel = -1;
static float grow;

/* Worker: the catalogue, icons, one install at a time. */
static SceUID wake = -1;
static volatile int want_catalog, installing = -1, job_stage, catalog_state;   /* catalog: 0 none 1 loading 2 ok 3 failed */
static volatile float job_frac;
static char job_msg[128];
static volatile int icon_queue[16], iq_head, iq_tail;

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

static int by_date(const void *a, const void *b) {
    return strcmp(apps[*(const int *)b].date, apps[*(const int *)a].date);    /* newest first */
}

/* Most downloaded first: the hottest software on top (playtest 2026-09-25). */
static int by_popular(const void *a, const void *b) {
    long x = atol(apps[*(const int *)a].downloads), y = atol(apps[*(const int *)b].downloads);
    return x < y ? 1 : x > y ? -1 : by_date(a, b);
}
static int sort_new;                  /* 0 popular (default), 1 newest */

static void filter(void) {
    nview = 0;
    for (int i = 0; i < napps; ++i)
        if (!cat_types[cat] || !strcmp(apps[i].type, cat_types[cat])) view[nview++] = i;
    qsort(view, nview, sizeof(int), sort_new ? by_date : by_popular);
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
                DEF(long_description); DEF(size); DEF(data); DEF(requirements); DEF(release_page); DEF(downloads);
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

static int sfo_title_id(const char *dir, char *tid) {
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
        if (!strcmp(k, "TITLE_ID")) { snprintf(tid, 10, "%s", (const char *)sfo + data + rd32(e + 12)); return 0; }
    }
    return -2;
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
            if (installing >= 0) break;
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


void store_leave(void) { if (installing < 0) { detail = 0; job_stage = 0; } chips = 0; }

const char *store_hint(void) {
    if (detail) return installing >= 0 ? "Installing\xE2\x80\xA6" : "X install    O back";
    if (chips) return "<- -> category    X back to the apps    L R tabs";
    return sort_new ? "X details  UP category  /\\ sort: newest  [] refresh  O queue  L R tabs"
                    : "X details  UP category  /\\ sort: popular  [] refresh  O queue  L R tabs";
}

int store_update(const Input *in) {
    STAGE("store: update");
    unsigned int p = in->pressed;
    if (spare_ready && installing < 0 && !detail) { iq_tail = iq_head; swap_in(); }

    if (detail) {
        App *a = &apps[view[sel]];
        if (in->tapped && installing < 0) {
            if (in->tap_x >= 190 && in->tap_x < 340 && in->tap_y >= 176 && in->tap_y < 214) p |= SCE_CTRL_CROSS;
        }
        if (p & SCE_CTRL_CIRCLE && installing < 0) { detail = 0; job_stage = 0; }
        if (p & SCE_CTRL_CROSS && installing < 0 && job_stage != 4) {
            int is_vpk = strlen(a->url) > 4 && !strcasecmp(a->url + strlen(a->url) - 4, ".vpk");
            if (denied(a)) ui_message("Not installing this one", "It wedged the shell on this Vita before (2026-09-19).");
            else if (!is_vpk) ui_message("Not a VPK", "This one downloads as an archive; install it by hand.");
            else {
                char msg[300];
                snprintf(msg, sizeof(msg), "Install %s %s (%s KB)?%s", a->name, a->version, a->size[0] ? a->size : "?",
                         a->data[0] ? " It also needs data files that are not installed automatically." : "");
                if (ui_confirm("Install", msg)) { installing = view[sel]; job_stage = 1; job_msg[0] = 0; kick(); }
            }
        }
        /* the panel */
        vita2d_draw_rectangle(0, 65, W, H - 105, RGBA8(21, 24, 33, 235));
        icon_of(a);
        if (a->icon_state == 2) {
            char path[256];
            snprintf(path, sizeof(path), DIR "/icons/%s", a->icon);
            draw_app_icon(path, 40, 90, 128, a->name, 0);
        } else draw_app_icon("", 40, 90, 128, a->name, 0);
        text_fit(bold, 190, 124, C_TEXT, 26, a->name, W - 230);
        char meta[200];
        snprintf(meta, sizeof(meta), "%s   \xC2\xB7   %s   \xC2\xB7   %s   \xC2\xB7   %.1f MB", a->version, a->author, a->date,
                 atoi(a->size) / 1048576.0f);
        text_fit(font, 190, 152, C_DIM, 15, meta, W - 230);
        int by = 176;
        if (installing >= 0 || job_stage) {
            text(font, 190, by + 20, job_stage == 9 ? C_BAD : job_stage == 4 ? C_OK : C_TEXT, 16, job_msg);
            if (job_stage >= 1 && job_stage <= 3) draw_bar(190, by + 30, 400, 5, job_stage == 3 ? ui_pulse() : job_frac, C_ACCENT);
        } else {
            draw_focus(190, by, 150, 38, 1);
            draw_action_button(190, by, 150, 38, denied(a) ? "Blocked" : "X Install", 1, denied(a) ? C_BAD : C_ACCENT);
        }
        int y = 250;
        if (a->requirements[0]) {
            text(bold, 40, y, C_MARK, 15, "Needs");
            draw_wrapped_text(a->requirements, 110, y, W - 150, 14, 3, C_TEXT);
            y += 62;
        }
        if (a->data[0]) { text(font, 40, y, C_MARK, 14, "Also needs data files (not installed automatically)."); y += 26; }
        draw_wrapped_text(a->long_description[0] ? a->long_description : a->description, 40, y + 6, W - 80, 15, 8, C_DIM);
        return 1;
    }

    if (p & SCE_CTRL_SQUARE) { want_catalog = 1; kick(); ui_toast("Refreshing the store", C_ACCENT); }
    if (catalog_state == 3 && (p & SCE_CTRL_SQUARE)) catalog_state = 1;
    if (p & SCE_CTRL_TRIANGLE) { sort_new = !sort_new; filter(); sel = 0; ui_toast(sort_new ? "Newest first" : "Most popular first", C_ACCENT); }
    if (p & SCE_CTRL_CIRCLE) return 0;                              /* back to the queue */
    if (!nview) {
        text(font, 40, 150, C_DIM, 18, catalog_state == 3 ? "The store could not be reached. [] tries again." : "Loading the store\xE2\x80\xA6");
        return 1;
    }
    /* The category chips are a row above the grid: UP from the top row reaches
     * them, left/right switch category at once, DOWN (or X) goes back. */
    if (chips) {
        if (p & SCE_CTRL_LEFT && cat > 0) { cat--; filter(); }
        if (p & SCE_CTRL_RIGHT && cat < 4) { cat++; filter(); }
        if (p & (SCE_CTRL_DOWN | SCE_CTRL_CROSS)) chips = 0;
    } else {
        if (p & SCE_CTRL_LEFT) sel = sel > 0 ? sel - 1 : 0;
        if (p & SCE_CTRL_RIGHT) sel = sel < nview - 1 ? sel + 1 : sel;
        if (p & SCE_CTRL_UP) { if (sel >= 5) sel -= 5; else chips = 1; }
        if (p & SCE_CTRL_DOWN) sel = sel + 5 < nview ? sel + 5 : nview - 1;
        if (p & SCE_CTRL_CROSS) { detail = 1; job_stage = 0; }
    }
    if (in->tapped) {                                               /* touch: chips, then tiles */
        int tx = in->tap_x, ty = in->tap_y, chx = 40;
        for (int c = 0; c < 5; ++c) {
            int w = text_w(font, 15, cat_names[c]) + 28;
            if (tx >= chx && tx < chx + w && ty >= 74 && ty < 110) { cat = c; filter(); chips = 0; }
            chx += w + 8;
        }
        if (ty > 116 && ty < H - 40) {
            int col = (tx - 40) / 180, k = (int)(top + (ty - 120) / 178.0f) * 5 + col;
            if (col >= 0 && col < 5 && k >= 0 && k < nview) { chips = 0; if (k == sel) { detail = 1; job_stage = 0; } else sel = k; }
        }
    }



    if (grow_sel != sel) { grow_sel = sel; grow = 0; }
    grow = grow < 1 ? grow + 0.09f : 1;
    static GridScroll gs;
    if (!chips) grid_scroll(&gs, &sel, 5, nview, 2, 178, in);
    top = gs.top;
    ui_theme_from(sel < nview ? icon_of(&apps[view[sel]]) : NULL);
    for (int k = 0; k < nview; ++k) {
        float y = 120 + (k / 5 - top) * 178;
        if (y < 110 - 178 || y > H - 40) continue;
        int x = 40 + (k % 5) * 180;
        App *a = &apps[view[k]];
        float lift = k == sel ? ease_back(grow) : 0, size = 104 * (1 + 0.1f * lift);
        float ix = x + (156 - size) / 2, iy = y + 8 - 5 * lift - (size - 104) / 2;
        if (k == sel && !chips) draw_focus_r(ix, iy, size, size, lift > 1 ? 1 : lift, size * 0.22f);
        icon_of(a);                                   /* makes sure it is on the card */
        if (a->icon_state == 2) {
            char path[256];
            snprintf(path, sizeof(path), DIR "/icons/%s", a->icon);
            draw_app_icon(path, ix, iy, size, a->name, 0);
        } else if (a->icon_state == 3 || !a->icon[0]) draw_app_icon("", ix, iy, size, a->name, 0);
        else { draw_round_rect(ix, iy, size, size, size * 0.22f, RGBA8(36, 41, 56, 255)); draw_shimmer(ix + size * 0.1f, iy, size * 0.8f, size); }
        text_fit(k == sel ? bold : font, x + 4, (int)y + 134, k == sel ? C_TEXT : C_DIM, 15, a->name, 150);
        text_fit(font, x + 4, (int)y + 152, C_FAINT, 13, a->author, 150);
    }
    /* The chips last, on a band of background, so rows scrolled up pass under them. */
    draw_gradient(0, 65, W, 52, C_BG, C_BG, C_BG, (C_BG & 0x00FFFFFF) | 0xE0000000);
    int cx = 40;
    for (int c = 0; c < 5; ++c) {
        int w = text_w(font, 15, cat_names[c]) + 30;
        if (chips && c == cat) draw_focus(cx, 78, w, 30, 1);
        draw_round_rect(cx, 78, w, 30, 15, c == cat ? RGBA8(245, 245, 250, 255) : RGBA8(255, 255, 255, 26));
        text(font, cx + 15, 99, c == cat ? RGBA8(15, 15, 20, 255) : C_TEXT, 15, cat_names[c]);
        cx += w + 10;
    }
    char count[32];
    snprintf(count, sizeof(count), "%d apps", nview);
    text_right(font, W - 40, 99, C_FAINT, 14, count);
    return 1;
}
