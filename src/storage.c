/* Settings > Storage: where the card went. Sizes are walked in a background
 * thread (the card has ~10,000 files), then shown as one stacked bar by
 * category, the biggest folders, and a cleanup that only touches things that
 * are safe to lose: crash dumps, RetroArch's logs and temp, half downloads. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/devctl.h>
#include <psp2/kernel/threadmgr.h>

#include "storage.h"
#include "apps.h"

typedef struct { const char *name, *path; unsigned int color; unsigned long long size; } Cat;
static Cat cats[] = {
    {"Games and apps", "ux0:app", RGBA8(88, 166, 255, 255), 0},
    {"ROMs", "ux0:data/RetroFlow", RGBA8(167, 139, 250, 255), 0},
    {"Films", "ux0:video", RGBA8(250, 204, 21, 255), 0},
    {"Music", "ux0:data/music", RGBA8(74, 222, 128, 255), 0},
    {"Home's art and clips", "ux0:data/arcadehub", RGBA8(248, 113, 113, 255), 0},
    {"RetroArch", "ux0:data/retroarch", RGBA8(251, 146, 60, 255), 0},
    {"Downloads", "ux0:downloads", RGBA8(45, 212, 191, 255), 0},
};
#define NCATS ((int)(sizeof(cats) / sizeof(cats[0])))

#define TOP 10
static struct { char name[64]; unsigned long long size; } top[TOP];
static int ntop;
static unsigned long long junk_size, dev_free, dev_total, other;
static int junk_files;
static volatile int state;       /* 0 idle, 1 walking, 2 done */
static volatile int walked;      /* files seen, for the progress line */
static int active, cleaning;

static int is_junk(const char *dir, const char *name) {
    int n = strlen(name);
    if (!strcmp(dir, "ux0:data") && n > 8 && !strcasecmp(name + n - 8, ".psp2dmp")) return 1;
    if (!strncmp(dir, "ux0:data/retroarch/logs", 23) || !strncmp(dir, "ux0:data/retroarch/temp", 23)) return 1;
    if (n > 5 && !strcasecmp(name + n - 5, ".part") && !strncmp(dir, "ux0:downloads", 13)) return 1;
    return 0;
}

/* Size of a tree; junk found on the way is added up (and deleted when asked). */
static unsigned long long walk(const char *dir, int depth, int remove_junk) {
    if (depth > 12) return 0;
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    unsigned long long total = 0;
    SceIoDirent e;
    char path[512];
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) { total += walk(path, depth + 1, remove_junk); continue; }
        total += e.d_stat.st_size;
        walked++;
        if (is_junk(dir, e.d_name)) {
            if (remove_junk) sceIoRemove(path);
            else { junk_size += e.d_stat.st_size; junk_files++; }
        }
    }
    sceIoDclose(d);
    return total;
}

static void note_top(const char *name, unsigned long long size) {
    int at = ntop < TOP ? ntop : TOP - 1;
    if (ntop == TOP && size <= top[TOP - 1].size) return;
    while (at > 0 && top[at - 1].size < size) { if (at < TOP) top[at] = top[at - 1]; at--; }
    snprintf(top[at].name, sizeof(top[0].name), "%s", name);
    top[at].size = size;
    if (ntop < TOP) ntop++;
}

static const char *app_title(const char *tid, char *out, int max) {
    /* Home keeps no title table here; the folder name is the title ID. */
    snprintf(out, max, "%s", tid);
    return out;
}

static int walker(SceSize args, void *argp) {
    (void)args; (void)argp;
    junk_size = 0; junk_files = 0; ntop = 0; walked = 0;
    for (int c = 0; c < NCATS; ++c) cats[c].size = 0;
    /* ux0:app and ux0:data one level down, so the biggest folders fall out too. */
    const char *roots[2] = {"ux0:app", "ux0:data"};
    for (int r = 0; r < 2; ++r) {
        SceUID d = sceIoDopen(roots[r]);
        if (d < 0) continue;
        SceIoDirent e;
        char path[256], label[80], t[16];
        for (;;) {
            memset(&e, 0, sizeof(e));
            if (sceIoDread(d, &e) <= 0) break;
            snprintf(path, sizeof(path), "%s/%s", roots[r], e.d_name);
            unsigned long long sz;
            if (SCE_S_ISDIR(e.d_stat.st_mode)) sz = walk(path, 1, 0);
            else {
                sz = e.d_stat.st_size;
                if (is_junk(roots[r], e.d_name)) { junk_size += sz; junk_files++; }
            }
            if (r == 0) { cats[0].size += sz; snprintf(label, sizeof(label), "App %s", app_title(e.d_name, t, sizeof(t))); }
            else {
                int hit = 0;
                for (int c = 1; c < NCATS; ++c)
                    if (!strcmp(cats[c].path, path)) { cats[c].size += sz; hit = 1; }
                if (!hit) other += sz;
                snprintf(label, sizeof(label), "%s", path);
            }
            note_top(label, sz);
        }
        sceIoDclose(d);
    }
    cats[2].size = walk("ux0:video", 1, 0);
    cats[6].size = walk("ux0:downloads", 1, 0);
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) >= 0) { dev_free = info.free_size; dev_total = info.max_size; }
    state = 2;
    return sceKernelExitDeleteThread(0);
}

static void start_walk(void) {
    if (state == 1) return;
    other = 0;
    state = 1;
    SceUID t = sceKernelCreateThread("storage_walk", walker, 0xB0, 0x8000, 0, 0, NULL);   /* low: the UI stays smooth */
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) state = 0;
}

void storage_open(void) { active = 1; if (state != 1) start_walk(); }
int storage_active(void) { return active; }
void storage_close(void) { active = 0; }
const char *storage_hint(void) { return "X clean up    [] measure again    O back"; }

void storage_update(const Input *in) {
    if (in->pressed & SCE_CTRL_CIRCLE) { active = 0; return; }
    if (in->pressed & SCE_CTRL_SQUARE && state != 1) start_walk();
    if (in->pressed & SCE_CTRL_CROSS && state == 2 && junk_files) {
        char msg[200], sz[32];
        human_size(junk_size, sz, sizeof(sz));
        snprintf(msg, sizeof(msg), "Delete %d files (%s): crash dumps, RetroArch logs and temp files, and half-finished downloads. Nothing you made or saved.", junk_files, sz);
        if (ui_confirm("Clean up", msg)) {
            cleaning = 1;
            SceUID d = sceIoDopen("ux0:data");               /* crash dumps sit at the top */
            if (d >= 0) {
                SceIoDirent e;
                char path[300];
                for (;;) {
                    memset(&e, 0, sizeof(e));
                    if (sceIoDread(d, &e) <= 0) break;
                    if (!SCE_S_ISDIR(e.d_stat.st_mode) && is_junk("ux0:data", e.d_name)) {
                        snprintf(path, sizeof(path), "ux0:data/%s", e.d_name);
                        sceIoRemove(path);
                    }
                }
                sceIoDclose(d);
            }
            walk("ux0:data/retroarch/logs", 1, 1);
            walk("ux0:data/retroarch/temp", 1, 1);
            walk("ux0:downloads", 1, 1);
            cleaning = 0;
            ui_toast("Cleaned up", C_OK);
            start_walk();
        }
    }

    char line[128], a[32], b[32];
    text(bold, 40, 110, C_TEXT, 24, "Storage");
    if (state == 1) {
        snprintf(line, sizeof(line), "Measuring\xE2\x80\xA6 %d files so far", walked);
        text(font, 40, 142, C_DIM, 16, line);
        draw_bar(40, 160, W - 80, 6, (ui_pulse() * 0.6f) + 0.2f, C_ACCENT);
        return;
    }
    if (state != 2) return;
    human_size(dev_total - dev_free, a, sizeof(a));
    human_size(dev_total, b, sizeof(b));
    snprintf(line, sizeof(line), "%s used of %s on the memory card", a, b);
    text(font, 40, 142, C_DIM, 16, line);

    /* one bar, a segment per category, free space last */
    int x = 40, bw = W - 80;
    vita2d_draw_rectangle(40, 156, bw, 18, RGBA8(40, 45, 60, 255));
    for (int c = 0; c < NCATS; ++c) {
        int w = dev_total ? (int)((double)cats[c].size / dev_total * bw) : 0;
        vita2d_draw_rectangle(x, 156, w, 18, cats[c].color);
        x += w;
    }
    for (int c = 0; c < NCATS; ++c) {                                   /* legend */
        int lx = 40 + (c % 4) * 222, ly = 204 + (c / 4) * 26;
        vita2d_draw_rectangle(lx, ly - 10, 10, 10, cats[c].color);
        human_size(cats[c].size, a, sizeof(a));
        snprintf(line, sizeof(line), "%s  %s", cats[c].name, a);
        text_fit(font, lx + 16, ly, C_TEXT, 14, line, 200);
    }

    text(bold, 40, 270, C_TEXT, 17, "Biggest folders");
    for (int i = 0; i < ntop && i < 7; ++i) {
        int y = 296 + i * 26;
        human_size(top[i].size, a, sizeof(a));
        const char *name = top[i].name, *t = !strncmp(name, "App ", 4) ? apps_title_for(name + 4) : NULL;
        char shown[96];
        if (t) snprintf(shown, sizeof(shown), "%s  (%s)", t, name + 4);
        text_fit(font, 40, y, C_DIM, 15, t ? shown : name, 380);
        text_right(font, 520, y, C_TEXT, 15, a);
        draw_bar(40, y + 6, 480, 3, top[0].size ? (float)top[i].size / top[0].size : 0, C_ACCENT);
    }

    int cx = 580, cy = 256;
    vita2d_draw_rectangle(cx, cy, 340, 180, C_PANEL);
    text(bold, cx + 18, cy + 32, C_TEXT, 18, "Clean up");
    human_size(junk_size, a, sizeof(a));
    if (junk_files) {
        snprintf(line, sizeof(line), "%d files, %s", junk_files, a);
        text(bold, cx + 18, cy + 62, C_OK, 20, line);
        text(font, cx + 18, cy + 92, C_DIM, 14, "Crash dumps, RetroArch logs and temp,");
        text(font, cx + 18, cy + 112, C_DIM, 14, "half-finished downloads.");
        draw_focus(cx + 18, cy + 128, 150, 36, 1);
        draw_action_button(cx + 18, cy + 128, 150, 36, "X Clean up", 1, C_ACCENT);
    } else {
        text(font, cx + 18, cy + 64, C_DIM, 15, "Nothing to clean up.");
    }
    (void)cleaning;
}
