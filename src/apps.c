/* Apps: every installed app as an icon grid, read from ux0:app/<TITLEID>
 * (param.sfo for the name and version, sce_sys/icon0.png for the icon).
 * Icons load lazily and a small cache keeps memory flat however many apps
 * are installed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <psp2/kernel/threadmgr.h>
#include <math.h>
#include "sfx.h"
#include "apps.h"
#include "search.h"
#include "camera.h"
#include "memo.h"
#include "store.h"

#define MAX_APPS 400
#define COLS 6
#define CELL_W 152
#define CELL_H 150
#define GRID_X 24
#define GRID_Y 78
#define ICON 96
#define ICON_CACHE 36

typedef struct { char tid[10], title[64], version[8]; } App;

static App apps[MAX_APPS];
static int napps, sel, moving = -1;
static volatile int loaded, scanning;
static float top;

/* Just enough SFO: the TITLE and APP_VER string entries. */
static void read_sfo(const char *path, App *a) {
    static unsigned char buf[16 * 1024];
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    int n = sceIoRead(fd, buf, sizeof(buf));
    sceIoClose(fd);
    if (n < 20 || memcmp(buf, "\0PSF", 4)) return;
    unsigned int keys = buf[8] | buf[9] << 8 | buf[10] << 16 | (unsigned)buf[11] << 24;
    unsigned int data = buf[12] | buf[13] << 8 | buf[14] << 16 | (unsigned)buf[15] << 24;
    unsigned int count = buf[16] | buf[17] << 8 | buf[18] << 16 | (unsigned)buf[19] << 24;
    for (unsigned int i = 0; i < count && 20 + i * 16 + 16 <= (unsigned)n; ++i) {
        unsigned char *e = buf + 20 + i * 16;
        unsigned int key = e[0] | e[1] << 8, len = e[4] | e[5] << 8 | e[6] << 16;
        unsigned int off = e[12] | e[13] << 8 | e[14] << 16 | (unsigned)e[15] << 24;
        if (keys + key >= (unsigned)n || data + off + len > (unsigned)n) continue;
        const char *name = (const char *)buf + keys + key;
        const char *val = (const char *)buf + data + off;
        if (!strcmp(name, "TITLE")) snprintf(a->title, sizeof(a->title), "%.*s", (int)len, val);
        else if (!strcmp(name, "APP_VER")) snprintf(a->version, sizeof(a->version), "%.*s", (int)len, val);
    }
}

static int by_title(const void *x, const void *y) {
    return strcasecmp(((const App *)x)->title, ((const App *)y)->title);
}

/* The grid's order, one title ID a line. Apps it does not list yet (new
 * installs) go first; the rest follow in the saved order. Hold X to move one. */
#define ORDER "ux0:data/arcadehub/user/apps-order.txt"

static void save_order(void) {
    static char buf[MAX_APPS * 11];
    int n = 0;
    for (int i = 0; i < napps; ++i) n += snprintf(buf + n, sizeof(buf) - n, "%s\n", apps[i].tid);
    ui_save(ORDER, buf, n, 0);
}

static void apply_order(void) {                       /* scan thread */
    static char buf[MAX_APPS * 11 + 1];
    SceUID fd = sceIoOpen(ORDER, SCE_O_RDONLY, 0);
    int n = fd >= 0 ? sceIoRead(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) sceIoClose(fd);
    if (n <= 0) { save_order(); return; }             /* first run: A to Z is the order */
    buf[n] = 0;
    static App sorted[MAX_APPS];
    static char used[MAX_APPS];
    memset(used, 0, sizeof(used));
    int m = 0, fresh = 0;
    for (int i = 0; i < napps; ++i)                   /* new ones first (still A to Z among themselves) */
        if (!strstr(buf, apps[i].tid)) { sorted[m++] = apps[i]; used[i] = 1; fresh = 1; }
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        for (int i = 0; i < napps; ++i)
            if (!used[i] && !strcmp(apps[i].tid, line)) { sorted[m++] = apps[i]; used[i] = 1; break; }
    memcpy(apps, sorted, m * sizeof(App));
    napps = m;
    if (fresh) save_order();
}

static void scan(void) {
    STAGE("apps: scan");
    napps = 0;
    SceUID d = sceIoDopen("ux0:app");
    if (d < 0) return;
    SceIoDirent e;
    while (napps < MAX_APPS) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (!SCE_S_ISDIR(e.d_stat.st_mode) || strlen(e.d_name) != 9) continue;
        App *a = &apps[napps];
        memset(a, 0, sizeof(*a));
        snprintf(a->tid, sizeof(a->tid), "%s", e.d_name);
        char path[64];
        snprintf(path, sizeof(path), "ux0:app/%s/sce_sys/param.sfo", a->tid);
        read_sfo(path, a);
        if (!a->title[0]) continue;                 /* a leftover folder, not an app */
        /* Home is this app; showing it here would only offer to close itself. */
        if (!strcmp(a->tid, "MVZA00010")) continue;
        ++napps;
    }
    sceIoDclose(d);
    qsort(apps, napps, sizeof(App), by_title);
    apply_order();
    loaded = 1;
}

/* ---------- icon cache ---------- */

static struct { char tid[10]; vita2d_texture *t; unsigned int used; } cache[ICON_CACHE];
static unsigned int tick;

/* An app's picture (icon0.png, pic0.png). Retail games keep theirs encrypted
 * in ux0:app, so read the decrypted copy the system keeps for its bubbles in
 * ur0:appmeta; fall back to ux0:app for the few homebrew apps without one
 * (Reddit, 2026-09-27: retail icons were blank). Decided once per app. */
const char *app_art(const char *tid, const char *file, char *out, int max) {
    static struct { char key[24]; int meta; } memo[512];
    static int nmemo;
    char key[24];
    snprintf(key, sizeof(key), "%.9s/%s", tid, file);
    int meta = -1;
    for (int i = 0; i < nmemo; ++i) if (!strcmp(memo[i].key, key)) { meta = memo[i].meta; break; }
    if (meta < 0) {
        char p[96];
        SceIoStat st;
        snprintf(p, sizeof(p), "ur0:appmeta/%s/%s", tid, file);
        meta = sceIoGetstat(p, &st) >= 0;
        if (nmemo < 512) { snprintf(memo[nmemo].key, sizeof(memo[nmemo].key), "%s", key); memo[nmemo++].meta = meta; }
    }
    if (meta) snprintf(out, max, "ur0:appmeta/%s/%s", tid, file);
    else snprintf(out, max, "ux0:app/%s/sce_sys/%s", tid, file);
    return out;
}

static vita2d_texture *icon(const char *tid) {
    char path[96];
    return ui_image(app_art(tid, "icon0.png", path, sizeof(path)));
}

/* ---------- launching ---------- */

static void launch(const App *a) {
    char msg[160];
    snprintf(msg, sizeof(msg), "Open %s? Home closes while it runs and comes back when you quit it.", a->title);
    if (!ui_confirm(a->title, msg)) return;
    /* The agent bridge taps OK on the shell's "will close" dialog when it
     * sees this file, the same handshake Play uses. */
    SceUID fd = sceIoOpen("ux0:data/arcadehub/confirm.req", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
    char uri[48];
    snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", a->tid);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
}

static void info(const App *a) {
    char msg[200];
    snprintf(msg, sizeof(msg), "%s\nTitle ID %s    version %s\nux0:app/%s", a->title, a->tid,
             a->version[0] ? a->version : "?", a->tid);
    ui_message("About this app", msg);
}

static int scan_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    scan();
    scanning = 0;
    return sceKernelExitDeleteThread(0);
}

/* Reading ux0:app takes a while: always on a background thread. */
void apps_prewarm(void) {
    if (scanning) return;
    scanning = 1;
    SceUID t = sceKernelCreateThread("apps_scan", scan_thread, 0x10000100, 0x8000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) scanning = 0;
}

int apps_find(const char *q, Hit *out, int max) {
    if (!loaded) return 0;
    int n = 0;
    for (int sc = 3; sc >= 1; --sc)
        for (int i = 0; i < napps && n < max; ++i)
            if (match_score(apps[i].title, q) == sc) {
                out[n] = (Hit){H_APP, i, 0, sc, apps[i].title, "App", NULL, "", 0};
                app_art(apps[i].tid, "icon0.png", out[n].path, sizeof(out[0].path)); n++;
            }
    return n;
}

/* The name of an installed title, or NULL (Settings > Storage uses it). */
const char *apps_title_for(const char *tid) {
    if (!loaded) return NULL;
    for (int i = 0; i < napps; ++i) if (!strcmp(apps[i].tid, tid)) return apps[i].title;
    return NULL;
}

/* Camera, Photos and Voice Memos sit in front of the installed apps, like a phone's. */
#define NB 3
static const char *const BUILTIN[NB] = {"Camera", "Photos", "Voice Memos"};

void apps_open_hit(const Hit *h) { if (h->a >= 0 && h->a < napps) { sel = h->a + NB; launch(&apps[h->a]); } }

static void open_cell(int i) {
    if (i == 2) memo_open();
    else if (i < NB) camera_open(i);
    else launch(&apps[i - NB]);
}

const char *apps_hint(void) {
    if (camera_active()) return camera_hint();
    if (memo_active()) return memo_hint();
    if (moving >= 0) return "<- -> UP DOWN  move  X put it here  O put it here";
    return "X open  hold X move  /\\ options  [] refresh  L R tabs";
}

static void options(int i) {
    App *a = &apps[i];
    static const char *const items[] = {"About this app", "Delete this app"};
    int k = ui_menu(a->title, items, 2);
    if (k == 0) info(a);
    if (k == 1) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Delete %s? The app and its data are removed from the Vita. This can't be undone.", a->title);
        if (ui_confirm("Delete app", msg) && store_uninstall(a->tid, a->title) == 0) ui_toast("Deleting\xE2\x80\xA6", C_ACCENT);
    }
}

void apps_update(const Input *in) {
    if (camera_active()) { camera_update(in); return; }
    if (memo_active()) { memo_update(in); return; }
    int count = NB + (loaded ? napps : 0);
    static int hold, held_long;
    if (moving >= 0) {                                /* moving an app: the arrows carry it */
        int to = moving;
        if (in->pressed & SCE_CTRL_LEFT) to = moving - 1;
        if (in->pressed & SCE_CTRL_RIGHT) to = moving + 1;
        if (in->pressed & SCE_CTRL_UP) to = moving - COLS;
        if (in->pressed & SCE_CTRL_DOWN) to = moving + COLS;
        if (to < NB) to = NB;                         /* the built-ins stay in front */
        if (to > count - 1) to = count - 1;
        while (to != moving) {                        /* slide it there, one swap at a time */
            int step = to > moving ? 1 : -1;
            App t = apps[moving - NB];
            apps[moving - NB] = apps[moving + step - NB];
            apps[moving + step - NB] = t;
            moving += step;
        }
        sel = moving;
        if ((in->pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) || in->tapped) { moving = -1; save_order(); ui_toast("Moved", C_ACCENT); }
    } else {
        if (in->pressed & SCE_CTRL_SQUARE && !scanning) { ui_toast("Refreshing apps", C_ACCENT); apps_prewarm(); }
        if (in->pressed & SCE_CTRL_LEFT) sel = sel > 0 ? sel - 1 : 0;
        if (in->pressed & SCE_CTRL_RIGHT) sel = sel < count - 1 ? sel + 1 : sel;
        if (in->pressed & SCE_CTRL_UP) sel = sel >= COLS ? sel - COLS : sel;
        if (in->pressed & SCE_CTRL_DOWN) sel = sel + COLS < count ? sel + COLS : count - 1;
        /* X opens on release; held ~0.6 s it picks the app up to move instead. */
        if (in->pressed & SCE_CTRL_CROSS) { hold = 1; held_long = 0; }
        if (hold && (in->held & SCE_CTRL_CROSS)) {
            if (++hold == 36 && sel >= NB) { moving = sel; held_long = 1; sfx_play(SFX_SELECT); }
        } else if (hold) {
            hold = 0;
            if (!held_long) { open_cell(sel); if (camera_active() || memo_active()) return; }
        }
        if (in->pressed & SCE_CTRL_TRIANGLE && sel >= NB) options(sel - NB);
        /* the same by touch: a long press on an app picks it up */
        static int touch_frames;
        touch_frames = in->touching && !in->drag_dx && !in->drag_dy ? touch_frames + 1 : 0;
        if (touch_frames == 40 && in->ty > GRID_Y && in->ty < H - 40) {
            int c = (in->tx - GRID_X) / CELL_W, r = (int)(top + (in->ty - GRID_Y) / (float)CELL_H), idx = r * COLS + c;
            if (c >= 0 && c < COLS && idx >= NB && idx < count) { sel = moving = idx; sfx_play(SFX_SELECT); }
        }
        if (in->tapped && moving < 0 && touch_frames < 40 && in->tap_y > GRID_Y && in->tap_y < H - 40) {
            int c = (in->tap_x - GRID_X) / CELL_W, r = (int)(top + (in->tap_y - GRID_Y) / (float)CELL_H);
            int idx = r * COLS + c;
            if (c >= 0 && c < COLS && idx >= 0 && idx < count) {
                if (idx == sel) { open_cell(idx); if (camera_active() || memo_active()) return; }
                else sel = idx;
            }
        }
    }
    static GridScroll gs;
    grid_scroll(&gs, &sel, COLS, count, 2, CELL_H, moving >= 0 ? &(Input){0} : in);
    top = gs.top;
    if (!loaded && !scanning) apps_prewarm();
    static int lifted = -1;
    static float grow;
    if (lifted != sel) { lifted = sel; grow = 0; }
    grow = grow < 1 ? grow + 0.09f : 1;
    for (int i = 0; i < count; ++i) {
        float y = GRID_Y + (i / COLS - top) * CELL_H;
        if (y < GRID_Y - CELL_H || y > H - 40) continue;
        int x = GRID_X + (i % COLS) * CELL_W;
        /* The focused icon grows a little (springy), rises, and glows. Icons
         * that will not decode (interlaced PNGs) get their initials. */
        float lift = i == sel ? ease_back(grow) : 0, size = ICON * (1 + 0.12f * lift);
        float ix = x + (CELL_W - size) / 2, iy = y + 12 - 6 * lift - (size - ICON) / 2;
        if (i == moving) {                            /* picked up: bigger, and it wobbles */
            size = ICON * 1.18f;
            ix = x + (CELL_W - size) / 2 + 2.5f * sinf(ui_frames() * 0.5f);
            iy = y + 4 - (size - ICON) / 2;
        }
        if (i == sel) draw_focus_r(ix, iy, size, size, lift > 1 ? 1 : lift, size * 0.22f);
        const char *title = i < NB ? BUILTIN[i] : apps[i - NB].title;
        if (i == 2) memo_draw_icon(ix, iy, size);
        else if (i < NB) camera_draw_icon(i, ix, iy, size);
        else {
            char path[96];
            draw_app_icon(app_art(apps[i - NB].tid, "icon0.png", path, sizeof(path)), ix, iy, size, title, 0);
        }
        int tw = text_w(font, 15, title);
        if (tw <= CELL_W - 16) text(font, x + (CELL_W - tw) / 2, y + ICON + 36, i == sel ? C_TEXT : C_DIM, 15, title);
        else text_fit(font, x + 8, y + ICON + 36, i == sel ? C_TEXT : C_DIM, 15, title, CELL_W - 16);
    }
    if (!loaded) text(font, GRID_X + 2 * CELL_W + 20, GRID_Y + 70, C_DIM, 16, "Loading your apps\xE2\x80\xA6");
    /* The tab bar and footer are drawn over this, so rows scrolled under
     * them are hidden without clipping. */
}

int apps_count(void) { return napps; }
