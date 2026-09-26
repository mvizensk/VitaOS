#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <curl/curl.h>

#include "downloads.h"
#include "store.h"
#include "shared/fileops.h"
#include "shared/sha256.h"

#define QUEUE "ux0:data/workbench/downloads.txt"
#define DEFAULT_DIR "ux0:downloads"
#define MAX_ITEMS 64
#define WRITE_BUF (512 * 1024)      /* batch curl's small chunks into big card writes */

enum { D_QUEUED, D_RUNNING, D_DONE, D_FAILED, D_CANCELLED };

typedef struct {
    char url[1024], dest[FO_PATH_MAX];
    volatile int state;
    volatile unsigned long long done, total;
    volatile float speed;           /* bytes/s, smoothed */
    char sha[65], error[128];
} Item;

static Item items[MAX_ITEMS];
static int nitems, sel, ready;
static volatile int cancel_now, worker_busy;
static SceUID worker = -1, wake_sema = -1;
static volatile int run_all;
static float top;

/* ---------- queue file ---------- */

static const char *basename_of(const char *url) {
    const char *end = url + strcspn(url, "?#");
    const char *s = end;
    while (s > url && s[-1] != '/') --s;
    return s < end ? s : "download.bin";
}

static void default_dest(const char *url, char *out, int max) {
    const char *b = basename_of(url);
    int n = (int)strcspn(b, "?#");
    snprintf(out, max, "%s/%.*s", DEFAULT_DIR, n, b);
}

static void add_item(const char *url, const char *dest) {
    if (nitems >= MAX_ITEMS) return;
    for (int i = 0; i < nitems; ++i)
        if (!strcmp(items[i].url, url) && items[i].state != D_DONE) return;   /* already listed */
    Item *it = &items[nitems];
    memset(it, 0, sizeof(*it));
    snprintf(it->url, sizeof(it->url), "%s", url);
    if (dest && dest[0]) snprintf(it->dest, sizeof(it->dest), "%s", dest);
    else default_dest(url, it->dest, sizeof(it->dest));
    it->state = D_QUEUED;
    ++nitems;
}

static void load_queue(void) {
    SceUID fd = sceIoOpen(QUEUE, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    static char buf[32 * 1024];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ') ++line;
        if (strncmp(line, "http://", 7) && strncmp(line, "https://", 8)) continue;
        char *tab = strchr(line, '\t');
        if (tab) *tab++ = 0;
        add_item(line, tab);
    }
}

/* Appends so the list survives a restart; agents write the same file. */
static void save_to_queue(const char *url) {
    fo_mkdirs("ux0:data/workbench");
    SceUID fd = sceIoOpen(QUEUE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, url, strlen(url));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

/* ---------- the transfer ---------- */

typedef struct {
    Item *it;
    SceUID fd;
    unsigned char *buf;
    unsigned int fill;
    Sha256 h;
    int write_error;
    SceUInt64 t0, last_t;
    unsigned long long last_done;
} Ctx;

static int flush(Ctx *c) {
    if (!c->fill) return 0;
    int w = sceIoWrite(c->fd, c->buf, c->fill);
    if (w != (int)c->fill) { c->write_error = w < 0 ? w : -1; return -1; }
    c->fill = 0;
    return 0;
}

static size_t on_data(char *ptr, size_t size, size_t n, void *arg) {
    Ctx *c = arg;
    size_t len = size * n, off = 0;
    sha256_update(&c->h, ptr, (unsigned int)len);
    while (off < len) {
        size_t room = WRITE_BUF - c->fill, take = len - off < room ? len - off : room;
        memcpy(c->buf + c->fill, ptr + off, take);
        c->fill += take;
        off += take;
        if (c->fill == WRITE_BUF && flush(c) < 0) return 0;   /* 0 tells curl to stop */
    }
    c->it->done += len;
    return len;
}

static int on_progress(void *arg, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ut, curl_off_t un) {
    (void)dlnow; (void)ut; (void)un;
    Ctx *c = arg;
    if (dltotal > 0) c->it->total = (unsigned long long)dltotal;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - c->last_t > 500000) {
        float inst = (c->it->done - c->last_done) * 1e6f / (float)(now - c->last_t);
        c->it->speed = c->it->speed ? c->it->speed * 0.6f + inst * 0.4f : inst;
        c->last_t = now;
        c->last_done = c->it->done;
    }
    return cancel_now;                /* non-zero aborts the transfer */
}

static void run_one(Item *it) {
    static unsigned char *buf;
    if (!buf) buf = malloc(WRITE_BUF);
    char part[FO_PATH_MAX + 8];
    snprintf(part, sizeof(part), "%s.part", it->dest);
    char dir[FO_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", it->dest);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; fo_mkdirs(dir); }
    if (fo_can_write(it->dest) < 0) {
        snprintf(it->error, sizeof(it->error), "destination is protected");
        it->state = D_FAILED;
        return;
    }
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.it = it;
    c.buf = buf;
    c.fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (c.fd < 0 || !buf) {
        snprintf(it->error, sizeof(it->error), "cannot write %s (0x%08X)", part, c.fd);
        it->state = D_FAILED;
        return;
    }
    sha256_init(&c.h);
    c.t0 = c.last_t = sceKernelGetProcessTimeWide();
    it->done = it->total = 0;
    it->speed = 0;

    CURL *curl = curl_easy_init();
    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, it->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "app0:assets/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Workbench/1.0 (PS Vita)");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &c);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc == CURLE_OK) flush(&c);
    sceIoClose(c.fd);
    if (rc == CURLE_OK && !c.write_error) {
        unsigned char d[32];
        sha256_final(&c.h, d);
        sha256_hex(d, it->sha);
        sceIoRemove(it->dest);
        sceIoRename(part, it->dest);
        it->state = D_DONE;
        const char *name = strrchr(it->dest, '/');
        char msg[96];
        snprintf(msg, sizeof(msg), "Downloaded %.60s", name ? name + 1 : it->dest);
        ui_toast(msg, C_OK);
        return;
    }
    sceIoRemove(part);                       /* never leave half a download */
    if (cancel_now) {
        it->state = D_CANCELLED;
        snprintf(it->error, sizeof(it->error), "cancelled");
    } else {
        it->state = D_FAILED;
        ui_toast("A download failed: see Downloads", C_BAD);
        if (c.write_error) snprintf(it->error, sizeof(it->error), "card write failed (0x%08X)", c.write_error);
        else if (status >= 400) snprintf(it->error, sizeof(it->error), "server said %ld", status);
        else snprintf(it->error, sizeof(it->error), "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    }
}

static int worker_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(wake_sema, 1, NULL);
        if (!ready) break;
        for (;;) {
            Item *next = NULL;
            for (int i = 0; i < nitems && !next; ++i)
                if (items[i].state == D_RUNNING) next = &items[i];
            if (!next) break;
            cancel_now = 0;
            worker_busy = 1;
            run_one(next);
            worker_busy = 0;
            if (!run_all) break;
            /* "Start all": pick up the next queued item. */
            for (int i = 0; i < nitems; ++i)
                if (items[i].state == D_QUEUED) { items[i].state = D_RUNNING; break; }
        }
        run_all = 0;
    }
    return sceKernelExitDeleteThread(0);
}

static void start(int i, int all) {
    if (worker_busy || i < 0 || i >= nitems) return;
    if (items[i].state == D_RUNNING) return;
    items[i].state = D_RUNNING;
    items[i].error[0] = 0;
    run_all = all;
    sceKernelSignalSema(wake_sema, 1);
}

/* ---------- screen ---------- */

int downloads_init(void) {
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) return -1;
    wake_sema = sceKernelCreateSema("wb_dl_wake", 0, 0, 1, NULL);
    worker = sceKernelCreateThread("wb_download", worker_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (wake_sema < 0 || worker < 0) return -1;
    ready = 1;
    sceKernelStartThread(worker, 0, NULL);
    store_init();
    load_queue();
    return 0;
}

void downloads_term(void) {
    if (!ready) return;
    cancel_now = 1;
    ready = 0;
    sceKernelSignalSema(wake_sema, 1);
    SceUInt timeout = 3000000;
    sceKernelWaitThreadEnd(worker, NULL, &timeout);
    curl_global_cleanup();
}

int downloads_busy(void) { return worker_busy; }

/* For the Home widget: files still to fetch and how far the current one is. */
int downloads_progress(float *frac) {
    int left = 0;
    *frac = 0;
    for (int i = 0; i < nitems; ++i) {
        if (items[i].state == D_QUEUED || items[i].state == D_RUNNING) left++;
        if (items[i].state == D_RUNNING && items[i].total) *frac = (float)items[i].done / items[i].total;
    }
    return worker_busy ? left : 0;
}

static int in_store = 1;                 /* the store is the front page; O goes to the queue */

const char *downloads_hint(void) {
    if (in_store) return store_hint();
    return "X start   START start all   /\\ add URL   [] reload   O store (or cancel)   L R tabs";
}

void downloads_update(const Input *in) {
    STAGE("downloads: update");
    if (in_store && ready) {
        if (store_update(in)) return;
        in_store = 0;
        return;
    }
    if (!ready) {
        text(font, 40, 130, C_BAD, 20, "Network or TLS failed to start; downloads are unavailable.");
        return;
    }
    if (in->pressed & SCE_CTRL_UP && nitems) sel = (sel + nitems - 1) % nitems;
    if (in->pressed & SCE_CTRL_DOWN && nitems) sel = (sel + 1) % nitems;
    if (in->pressed & SCE_CTRL_CROSS) start(sel, 0);
    if (in->pressed & SCE_CTRL_START) {
        for (int i = 0; i < nitems; ++i)
            if (items[i].state == D_QUEUED) { start(i, 1); break; }
    }
    if (in->pressed & SCE_CTRL_CIRCLE && worker_busy) cancel_now = 1;
    else if (in->pressed & SCE_CTRL_CIRCLE) { in_store = 1; return; }
    if (in->pressed & SCE_CTRL_SQUARE && !worker_busy) {
        /* Reload: drop finished and failed rows, pick up newly queued lines. */
        int j = 0;
        for (int i = 0; i < nitems; ++i)
            if (items[i].state == D_QUEUED) items[j++] = items[i];
        nitems = j;
        sel = 0;
        load_queue();
    }
    if (in->pressed & SCE_CTRL_TRIANGLE) {
        char url[1024] = "https://";
        if (ui_ask_text("Download URL", url, sizeof(url)) &&
            (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) && strlen(url) > 9) {
            add_item(url, NULL);
            save_to_queue(url);
            sel = nitems - 1;
        }
    }
    if (in->tapped && in->tap_y > 90 && in->tap_y < 460) {
        int idx = (int)(top + (in->tap_y - 90) / 74.0f);
        if (idx >= 0 && idx < nitems) {
            if (idx == sel) start(idx, 0);
            sel = idx;
        }
    }
    if (sel >= nitems) sel = nitems ? nitems - 1 : 0;

    if (!nitems) {
        text(bold, 40, 140, C_TEXT, 22, "Nothing queued");
        text(font, 40, 176, C_DIM, 18, "Press TRIANGLE to type a URL, or add lines to");
        text(font, 40, 202, C_ACCENT, 18, QUEUE);
        text(font, 40, 228, C_DIM, 18, "(one URL per line, optional TAB and a destination path).");
        text(font, 40, 270, C_DIM, 18, "Files land in ux0:downloads/. HTTPS is verified against the bundled CA list.");
        return;
    }
    float target = sel > top + 4 ? sel - 4 : sel < top ? sel : top;
    top += (target - top) * 0.3f;
    for (int i = 0; i < nitems; ++i) {
        float y = 90 + (i - top) * 74;
        if (y < 70 || y > 440) continue;
        Item *it = &items[i];
        vita2d_draw_rectangle(12, y, W - 24, 68, i == sel ? C_SEL_DIM : C_PANEL);
        if (i == sel) vita2d_draw_rectangle(12, y, 4, 68, C_ACCENT);
        const char *name = strrchr(it->dest, '/') ? strrchr(it->dest, '/') + 1 : it->dest;
        text_fit(bold, 30, y + 26, C_TEXT, 19, name, 520);
        text_fit(font, 30, y + 52, C_FAINT, 15, it->url, 560);
        char right[96], a[32], b[32], sp[32];
        unsigned int color = C_DIM;
        switch (it->state) {
        case D_QUEUED: snprintf(right, sizeof(right), "queued"); break;
        case D_RUNNING:
            human_size(it->done, a, sizeof(a));
            human_size(it->total, b, sizeof(b));
            human_size((unsigned long long)it->speed, sp, sizeof(sp));
            if (it->total) snprintf(right, sizeof(right), "%s of %s   %s/s", a, b, sp);
            else snprintf(right, sizeof(right), "%s   %s/s", a, sp);
            draw_bar(600, y + 44, 330, 8, it->total ? (float)it->done / it->total : 0, C_ACCENT);
            color = C_TEXT;
            break;
        case D_DONE:
            snprintf(right, sizeof(right), "done  sha256 %.12s", it->sha);
            color = C_OK;
            break;
        default:
            snprintf(right, sizeof(right), "%s", it->error);
            color = C_BAD;
        }
        text_fit(font, 600, y + 28, color, 16, right, 330);
    }
}
