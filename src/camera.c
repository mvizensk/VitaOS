/* Camera and Photos, the two built-in tiles at the front of Apps.
 *
 * Camera: a full-screen viewfinder from either camera (640x360, the sensor's
 * widest mode, streamed straight into a texture). X takes a picture, which a
 * background thread saves as a JPEG under ux0:picture/CAMERA/ with a small
 * thumbnail beside Home's data. The camera runs only while the viewfinder is
 * open, and closes itself after two idle minutes to spare the hardware.
 *
 * Photos: every picture under ux0:picture/, newest first, as a grid of
 * thumbnails. Thumbnails are made once, on a background thread, so the grid
 * never decodes a full picture. Two chips at the top split the roll: Camera
 * (this app's own shots and anything else loose under ux0:picture) and
 * Screenshots, which the Vita's PS+START capture and the pngshot plugin both
 * file one folder per game under ux0:picture/SCREENSHOT/<Game>/ — that folder
 * name becomes the album. Everything is one flat scan; which chip a picture
 * shows under is just whether it carries an album name. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <jpeglib.h>
#include <psp2/ctrl.h>
#include <psp2/camera.h>
#include <psp2/rtc.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include "camera.h"
#include "sfx.h"

#define CAM_W 640
#define CAM_H 360
#define SHOTS_DIR "ux0:picture/CAMERA"
#define THUMBS "ux0:data/arcadehub/photo-thumbs"
#define THUMB_W 256
#define MAX_PICS 600
#define MAX_ALBUMS 64
#define COLS 5
#define CELL_W 184
#define CELL_H 118
#define CELL_H_ALB 230               /* a photo grid cell plus two lines of caption */
#define GRID_X 20
#define GRID_Y 118
#define IDLE_FRAMES (60 * 120)

enum { M_OFF, M_FINDER, M_HOME, M_ALBUM, M_VIEW };
static int mode = M_OFF;

enum { SEC_CAMERA, SEC_SHOTS, NSEC };
static const char *SECT[NSEC] = { "Camera", "Screenshots" };
static int section, chips;          /* the chip bar, the way Music picks a section */
static int sel_album;               /* selection in the albums grid (separate from a photo grid's sel) */
static char album_game[48];         /* which album M_ALBUM is showing */
static int view_from_album;         /* M_VIEW's way back: to the album grid, or the camera roll */

/* ---------- JPEG out (libjpeg-turbo, straight from ABGR memory) ---------- */

static int write_jpeg(const char *path, const unsigned char *px, int w, int h, int stride, int quality) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = w;
    c.image_height = h;
    c.input_components = 4;
    c.in_color_space = JCS_EXT_RGBX;          /* ABGR words are R,G,B,A bytes in memory */
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(px + c.next_scanline * stride);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    fclose(f);
    return 0;
}

/* A box-filtered copy THUMB_W wide, kept to the picture's shape. */
static unsigned char *shrink(const unsigned char *px, int w, int h, int stride, int bpp, int *tw, int *th) {
    int ow = THUMB_W, oh = h * THUMB_W / w;
    if (w < ow) { ow = w; oh = h; }
    if (oh < 1) oh = 1;
    unsigned char *o = malloc(ow * oh * 4);
    if (!o) return NULL;
    for (int y = 0; y < oh; ++y) {
        int y0 = y * h / oh, y1 = (y + 1) * h / oh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ow; ++x) {
            int x0 = x * w / ow, x1 = (x + 1) * w / ow;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned int s[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 4)          /* a few samples is plenty */
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 4) {
                    const unsigned char *p = px + yy * stride + xx * bpp;
                    s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; ++n;
                }
            unsigned char *q = o + (y * ow + x) * 4;
            q[0] = s[0] / n; q[1] = s[1] / n; q[2] = s[2] / n; q[3] = 255;
        }
    }
    *tw = ow; *th = oh;
    return o;
}

static void thumb_path(const char *pic, char *out, int max) {
    unsigned int h = 2166136261u;
    for (const char *q = pic; *q; ++q) h = (h ^ (unsigned char)*q) * 16777619u;
    snprintf(out, max, THUMBS "/%08x.jpg", h);
}

/* ---------- the roll ---------- */

typedef struct { char path[160]; SceDateTime when; unsigned int size; int thumbed; char game[48]; } Pic;
static Pic pics[MAX_PICS];
static int npics, sel;
static volatile int scanned, scanning;
static SceUID roll_lock = -1;

/* One tile per game folder found under SCREENSHOT, newest shot as the cover.
 * Rebuilt from pics[] on demand (a few hundred strcmps, cheap) rather than
 * kept in step with every scan/delete/new-shot edit to pics[]. */
typedef struct { char game[48]; int cover; int count; } Album;
static Album albums[MAX_ALBUMS];
static int nalbums;

static void build_albums(void) {                  /* call while holding roll_lock */
    nalbums = 0;
    for (int i = 0; i < npics; ++i) {
        if (!pics[i].game[0]) continue;
        int a = -1;
        for (int k = 0; k < nalbums; ++k) if (!strcmp(albums[k].game, pics[i].game)) { a = k; break; }
        if (a < 0) {
            if (nalbums >= MAX_ALBUMS) continue;   /* rare: more distinct games than we track; drop the overflow */
            a = nalbums++;
            snprintf(albums[a].game, sizeof(albums[a].game), "%s", pics[i].game);
            albums[a].cover = i;
            albums[a].count = 0;
        }
        ++albums[a].count;
    }
}

/* The camera roll (no album) or one album's shots, in pics[]'s own newest-
 * first order. idxs must hold MAX_PICS ints. Call while holding roll_lock. */
static int build_cur(int *idxs) {
    int n = 0;
    for (int i = 0; i < npics; ++i) {
        if (section == SEC_CAMERA ? !pics[i].game[0] : (pics[i].game[0] && !strcmp(pics[i].game, album_game)))
            idxs[n++] = i;
    }
    return n;
}

static int newer(const void *a, const void *b) {
    const SceDateTime *x = &((const Pic *)a)->when, *y = &((const Pic *)b)->when;
    SceUInt64 tx, ty;
    sceRtcGetTick(x, (SceRtcTick *)&tx);
    sceRtcGetTick(y, (SceRtcTick *)&ty);
    return tx < ty ? 1 : tx > ty ? -1 : 0;
}

static int is_picture(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") || !strcasecmp(dot, ".bmp"));
}

/* game is NULL outside SCREENSHOT, "" for the SCREENSHOT folder itself (its
 * children are per-game folders, not pictures), or the game folder's name
 * once we're inside one — that's what every picture below it gets tagged
 * with. Handles ux0:picture/SCREENSHOT/<Game>/ and pngshot's own <Game>/ or
 * <TITLEID>/ folders the same way: whatever the folder is named is the album. */
static void walk(const char *dir, int depth, const char *game, Pic *out, int *n) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    while (*n < MAX_PICS) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        char p[160];
        snprintf(p, sizeof(p), "%s/%s", dir, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) {
            if (depth >= 2) continue;
            const char *sub = game;
            if (depth == 0 && !strcasecmp(e.d_name, "SCREENSHOT")) sub = "";
            else if (game && !game[0]) sub = e.d_name;      /* a game folder, one level under SCREENSHOT */
            walk(p, depth + 1, sub, out, n);
            continue;
        }
        if (!is_picture(e.d_name) || e.d_stat.st_size < 1024) continue;
        Pic *q = &out[(*n)++];
        snprintf(q->path, sizeof(q->path), "%s", p);
        q->when = e.d_stat.st_mtime;
        q->size = (unsigned int)e.d_stat.st_size;
        q->game[0] = 0;
        if (game && game[0]) snprintf(q->game, sizeof(q->game), "%s", game);
        char tp[80];
        SceIoStat st;
        thumb_path(p, tp, sizeof(tp));
        q->thumbed = sceIoGetstat(tp, &st) >= 0;
    }
    sceIoDclose(d);
}

static vita2d_texture *load_any(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    if (!strcasecmp(dot, ".png")) return vita2d_load_PNG_file(path);
    if (!strcasecmp(dot, ".bmp")) return vita2d_load_BMP_file(path);
    return vita2d_load_JPEG_file(path);
}

/* Lists the pictures, shows them at once, then makes the missing thumbnails
 * newest first. Textures made here are never drawn, so freeing them on this
 * thread is safe (the GPU never saw them). */
static int roll_scan(SceSize args, void *argp) {
    (void)args; (void)argp;
    static Pic found[MAX_PICS];
    int n = 0;
    walk("ux0:picture", 0, NULL, found, &n);
    qsort(found, n, sizeof(Pic), newer);
    sceKernelWaitSema(roll_lock, 1, NULL);
    memcpy(pics, found, n * sizeof(Pic));
    npics = n;
    if (sel >= npics) sel = npics ? npics - 1 : 0;
    sceKernelSignalSema(roll_lock, 1);
    scanned = 1;
    sceIoMkdir(THUMBS, 0777);
    for (int i = 0; i < n; ++i) {
        if (found[i].thumbed) continue;
        vita2d_texture *t = load_any(found[i].path);
        int made = -1;                                     /* -1: no thumbnail possible, drawn as a plain tile */
        if (t) {
            int w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t), tw, th;
            /* vita2d decodes JPEGs to 3 bytes a pixel (U8U8U8_BGR), PNGs and BMPs to 4 */
            int bpp = vita2d_texture_get_format(t) == SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR ? 3 : 4;
            unsigned char *s = shrink(vita2d_texture_get_datap(t), w, h, vita2d_texture_get_stride(t), bpp, &tw, &th);
            vita2d_free_texture(t);
            if (s) {
                char tp[80];
                thumb_path(found[i].path, tp, sizeof(tp));
                if (write_jpeg(tp, s, tw, th, tw * 4, 85) == 0) made = 1;
                free(s);
            }
        }
        sceKernelWaitSema(roll_lock, 1, NULL);            /* the grid may have moved on: find it by path */
        for (int k = 0; k < npics; ++k)
            if (!strcmp(pics[k].path, found[i].path)) { pics[k].thumbed = made; break; }
        sceKernelSignalSema(roll_lock, 1);
    }
    scanning = 0;
    return sceKernelExitDeleteThread(0);
}

static void rescan(void) {
    if (roll_lock < 0) roll_lock = sceKernelCreateSema("roll_lock", 0, 1, 1, NULL);
    if (scanning) return;
    scanning = 1;
    SceUID t = sceKernelCreateThread("roll_scan", roll_scan, 0x10000110, 0x8000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) scanning = 0;
}

/* ---------- the camera ---------- */

static vita2d_texture *finder;
static int dev = SCE_CAMERA_DEVICE_BACK, cam_on, effect, idle;
static float flash, fly;                      /* shutter flash; the new shot flying to the corner */
static char last_thumb[80];

static const struct { int mode; const char *name; } EFFECTS[] = {
    {SCE_CAMERA_EFFECT_NORMAL, "Normal"}, {SCE_CAMERA_EFFECT_BLACKWHITE, "Mono"},
    {SCE_CAMERA_EFFECT_SEPIA, "Sepia"}, {SCE_CAMERA_EFFECT_BLUE, "Cool"},
    {SCE_CAMERA_EFFECT_RED, "Warm"}, {SCE_CAMERA_EFFECT_NEGATIVE, "Negative"},
};
#define NEFFECTS (int)(sizeof(EFFECTS) / sizeof(EFFECTS[0]))

/* Opening and starting the sensor takes a second or more (a 3 s frame in the
 * 2026-09-25 smoke run), so it runs on its own thread; the viewfinder shows
 * "Starting" until cam_on. Stopping waits for a start still in flight. */
static volatile int cam_starting, cam_failed;
static SceUID cam_thread = -1;

static int cam_start_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceCameraInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.format = SCE_CAMERA_FORMAT_ABGR;
    info.resolution = SCE_CAMERA_RESOLUTION_640_360;
    info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    info.width = CAM_W;
    info.height = CAM_H;
    info.range = 1;
    info.sizeIBase = vita2d_texture_get_stride(finder) * CAM_H;
    info.pIBase = vita2d_texture_get_datap(finder);
    info.pitch = vita2d_texture_get_stride(finder) / 4 - CAM_W;
    int r = sceCameraOpen(dev, &info);
    if (r >= 0 && (r = sceCameraStart(dev)) < 0) sceCameraClose(dev);
    if (r >= 0) {
        sceCameraSetReverse(dev, dev == SCE_CAMERA_DEVICE_FRONT ? SCE_CAMERA_REVERSE_MIRROR : SCE_CAMERA_REVERSE_OFF);
        sceCameraSetEffect(dev, EFFECTS[effect].mode);
        cam_on = 1;
    } else {
        char m[64];
        snprintf(m, sizeof(m), "Camera would not open (0x%08X)", r);
        ui_toast(m, C_BAD);
        cam_failed = 1;
    }
    cam_starting = 0;
    return sceKernelExitDeleteThread(0);
}

static void cam_join(void) {
    if (!cam_starting) return;
    SceUInt timeout = 5 * 1000 * 1000;
    sceKernelWaitThreadEnd(cam_thread, NULL, &timeout);
}

static void cam_stop(void) {
    cam_join();
    if (!cam_on) return;
    sceCameraStop(dev);
    sceCameraClose(dev);
    cam_on = 0;
}

static int cam_start(void) {
    if (cam_on || cam_starting) return 0;
    if (!finder) finder = vita2d_create_empty_texture(CAM_W, CAM_H);
    if (!finder) return -1;
    memset(vita2d_texture_get_datap(finder), 0, vita2d_texture_get_stride(finder) * CAM_H);
    cam_failed = 0;
    cam_starting = 1;
    cam_thread = sceKernelCreateThread("cam_start", cam_start_main, 0x10000100, 0x4000, 0, 0, NULL);
    if (cam_thread < 0 || sceKernelStartThread(cam_thread, 0, NULL) < 0) { cam_starting = 0; return -1; }
    return 0;
}

/* Saving: the main thread copies the frame, this thread encodes and writes. */
static unsigned char *shot;
static char shot_path[96];
static volatile int shot_busy;

static int shot_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    sceIoMkdir("ux0:picture", 0777);
    sceIoMkdir(SHOTS_DIR, 0777);
    sceIoMkdir(THUMBS, 0777);
    int ok = write_jpeg(shot_path, shot, CAM_W, CAM_H, CAM_W * 4, 92) == 0;
    int tw, th;
    unsigned char *s = shrink(shot, CAM_W, CAM_H, CAM_W * 4, 4, &tw, &th);
    if (s) {
        char tp[80];
        thumb_path(shot_path, tp, sizeof(tp));
        write_jpeg(tp, s, tw, th, tw * 4, 85);
        free(s);
        if (ok) snprintf(last_thumb, sizeof(last_thumb), "%s", tp);
    }
    free(shot);
    shot = NULL;
    if (!ok) ui_toast("Could not save the photo", C_BAD);
    if (ok && roll_lock >= 0) {                  /* straight onto the front of the roll */
        sceKernelWaitSema(roll_lock, 1, NULL);
        if (npics == MAX_PICS) --npics;
        memmove(&pics[1], &pics[0], npics * sizeof(Pic));
        memset(&pics[0], 0, sizeof(Pic));
        snprintf(pics[0].path, sizeof(pics[0].path), "%s", shot_path);
        sceRtcGetCurrentClockLocalTime(&pics[0].when);
        pics[0].thumbed = 1;
        ++npics;
        sceKernelSignalSema(roll_lock, 1);
    }
    shot_busy = 0;
    return sceKernelExitDeleteThread(0);
}

static void take_picture(void) {
    if (!cam_on || shot_busy) return;
    shot = malloc(CAM_W * CAM_H * 4);
    if (!shot) return;
    const unsigned char *src = vita2d_texture_get_datap(finder);
    int stride = vita2d_texture_get_stride(finder);
    for (int y = 0; y < CAM_H; ++y) memcpy(shot + y * CAM_W * 4, src + y * stride, CAM_W * 4);
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(shot_path, sizeof(shot_path), SHOTS_DIR "/HOME_%04d%02d%02d_%02d%02d%02d.jpg", t.year, t.month, t.day,
             t.hour, t.minute, t.second);
    shot_busy = 1;
    SceUID th = sceKernelCreateThread("shot_save", shot_thread, 0x10000110, 0x10000, 0, 0, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) { free(shot); shot = NULL; shot_busy = 0; return; }
    flash = 1;
    fly = 1;
    sfx_play(SFX_LAUNCH);
}

/* ---------- icons for the Apps grid ---------- */

static vita2d_texture *make_icon(int kind) {
    enum { S = 128 };
    vita2d_texture *t = vita2d_create_empty_texture(S, S);
    if (!t) return NULL;
    unsigned int *px = vita2d_texture_get_datap(t), st = vita2d_texture_get_stride(t) / 4;
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            float fx = x + 0.5f - S / 2.0f, fy = y + 0.5f - S / 2.0f, d = sqrtf(fx * fx + fy * fy);
            float r, g, b;
            if (kind == 0) {                                 /* Camera: a lens on graphite */
                float v = 1 - y / (float)S;
                r = 44 + 30 * v; g = 48 + 32 * v; b = 58 + 38 * v;
                float ring = d < 40 ? 1 : d < 42 ? 42 - d : 0;
                if (ring > 0) { r = r * (1 - ring) + 200 * ring; g = g * (1 - ring) + 206 * ring; b = b * (1 - ring) + 216 * ring; }
                if (d < 33) { r = 18; g = 22; b = 34; }
                if (d < 24) { float k = 1 - d / 24; r = 20 + 30 * k; g = 50 + 80 * k; b = 110 + 120 * k; }
                float hx = fx + 8, hy = fy + 8, hd = sqrtf(hx * hx + hy * hy);
                if (hd < 6) { float k = 1 - hd / 6; r += (255 - r) * k; g += (255 - g) * k; b += (255 - b) * k; }
                float fl = sqrtf((x - 100.0f) * (x - 100.0f) + (y - 26.0f) * (y - 26.0f));
                if (fl < 7) { r = 250; g = 204; b = 21; }
            } else {                                        /* Photos: a four-colour pinwheel on white */
                r = g = b = 250;
                if (d < 44 && d > 3) {
                    int q = (fx >= 0) + 2 * (fy >= 0);
                    static const unsigned char C[4][3] = {{234, 67, 53}, {251, 188, 5}, {66, 133, 244}, {52, 168, 83}};
                    float petal = (q == 0 || q == 3) ? (fy < 0 ? -fy : fy) : (fx < 0 ? -fx : fx);   /* a leaf per quarter */
                    if (petal < d * 0.72f) { r = C[q][0]; g = C[q][1]; b = C[q][2]; }
                }
            }
            px[y * st + x] = RGBA8((int)r, (int)g, (int)b, 255);
        }
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t;
}

void camera_draw_icon(int kind, float x, float y, float size) {
    static vita2d_texture *icons[2];
    if (!icons[kind]) icons[kind] = make_icon(kind);
    draw_soft(ui_soft_shadow(), x + size / 2, y + size + 2, size * 0.9f, size * 0.2f, RGBA8(0, 0, 0, 120));
    if (icons[kind]) draw_round_texture(icons[kind], x, y, size, size, size * 0.22f, RGBA8(255, 255, 255, 255));
}

/* ---------- open, close ---------- */

void camera_open(int roll) {
    rescan();
    if (roll) { mode = M_HOME; return; }
    mode = M_FINDER;
    idle = 0;
    if (cam_start() < 0) mode = M_HOME;
}

void camera_leave(void) {
    cam_stop();
    mode = M_OFF;
}

int camera_active(void) { return mode != M_OFF; }
int camera_fullscreen(void) { return mode == M_FINDER || mode == M_VIEW; }

/* Back from the camera always lands on the roll, not wherever Screenshots was left. */
static void to_roll(void) { mode = M_HOME; section = SEC_CAMERA; sel = 0; }

const char *camera_hint(void) {
    if (mode == M_ALBUM) return "X view  [] delete  O back  L R tabs";
    if (mode == M_HOME) return chips ? "<- -> section  X open  L R tabs" : "X view  [] delete  START camera  O back  L R tabs";
    return "";
}

/* ---------- drawing ---------- */

static void pill_text(float x, float y, const char *s, unsigned int c) {
    int w = text_w(bold, 14, s) + 24;
    draw_round_rect(x, y, w, 28, 14, RGBA8(0, 0, 0, 140));
    text(bold, (int)x + 12, (int)y + 19, c, 14, s);
}

static void finder_frame(const Input *in) {
    if (in->pressed || in->touching) idle = 0;
    if (++idle > IDLE_FRAMES) { cam_stop(); to_roll(); ui_toast("Camera closed to save power", C_ACCENT); return; }
    if (in->pressed & SCE_CTRL_CIRCLE) { cam_stop(); mode = M_OFF; return; }
    if (in->pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1 | SCE_CTRL_START)) { cam_stop(); to_roll(); return; }
    if (in->pressed & SCE_CTRL_TRIANGLE) {
        cam_stop();
        dev = dev == SCE_CAMERA_DEVICE_BACK ? SCE_CAMERA_DEVICE_FRONT : SCE_CAMERA_DEVICE_BACK;
        if (cam_start() < 0) { to_roll(); return; }
    }
    if (in->pressed & SCE_CTRL_SQUARE) {
        effect = (effect + 1) % NEFFECTS;
        sceCameraSetEffect(dev, EFFECTS[effect].mode);
    }
    int flip_tap = in->tapped && in->tap_x > W - 110 && in->tap_x < W - 18 && in->tap_y > 60 && in->tap_y < 150;
    if (flip_tap) {
        cam_stop();
        dev = dev == SCE_CAMERA_DEVICE_BACK ? SCE_CAMERA_DEVICE_FRONT : SCE_CAMERA_DEVICE_BACK;
        if (cam_start() < 0) { to_roll(); return; }
    }
    int shutter_tap = in->tapped && in->tap_x > W - 130 && in->tap_y > H / 2 - 60 && in->tap_y < H / 2 + 60;
    int thumb_tap = in->tapped && in->tap_x < 130 && in->tap_y > H - 125 && in->tap_y < H - 44;
    /* R is the shutter, where a camera's is (playtest 2026-09-25); X and a tap work too. */
    if ((in->pressed & (SCE_CTRL_CROSS | SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) || shutter_tap) take_picture();
    if (thumb_tap) { cam_stop(); to_roll(); return; }

    if (cam_failed) { cam_failed = 0; to_roll(); return; }
    if (cam_on) {
        SceCameraRead rd;
        memset(&rd, 0, sizeof(rd));
        rd.size = sizeof(rd);
        rd.mode = 1;                              /* don't wait for the next frame; the UI keeps 60 */
        sceCameraRead(dev, &rd);
    }

    vita2d_draw_rectangle(0, 0, W, H, RGBA8(0, 0, 0, 255));
    float sc = W / (float)CAM_W;                  /* 1.5: fills the width, 2 px short of the height */
    vita2d_draw_texture_scale(finder, 0, (H - CAM_H * sc) / 2, sc, sc);
    if (!cam_on) text(font, W / 2 - 60, H / 2 + 6, C_DIM, 18, "Starting camera\xE2\x80\xA6");

    /* chrome: top bar, shutter on the right like a phone held sideways */
    draw_gradient(0, 0, W, 70, RGBA8(0, 0, 0, 150), RGBA8(0, 0, 0, 150), RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 0));
    pill_text(20, 18, dev == SCE_CAMERA_DEVICE_BACK ? "Rear camera" : "Front camera", C_TEXT);
    pill_text(170, 18, EFFECTS[effect].name, effect ? C_ACCENT : C_DIM);
    float cx = W - 64, cy = H / 2;
    float press = (in->held & SCE_CTRL_CROSS) ? 0.9f : 1.0f;
    draw_round_ring(cx - 36, cy - 36, 72, 72, 36, 4, RGBA8(255, 255, 255, 230));
    draw_round_rect(cx - 28 * press, cy - 28 * press, 56 * press, 56 * press, 28 * press, RGBA8(255, 255, 255, 240));
    /* flip: a round button above the shutter, two arrows chasing round a lens */
    float fx = W - 64, fy = 104;
    vita2d_draw_fill_circle(fx, fy, 28, RGBA8(0, 0, 0, 120));
    draw_round_ring(fx - 28, fy - 28, 56, 56, 28, 2, RGBA8(255, 255, 255, 200));
    for (int k = 0; k < 2; ++k) {
        float a0 = k * 3.14159f + 0.35f;
        for (float a = a0; a < a0 + 2.3f; a += 0.12f)
            vita2d_draw_fill_circle(fx + 13 * cosf(a), fy + 13 * sinf(a), 1.8f, RGBA8(255, 255, 255, 235));
        float ae = a0 + 2.3f, tx = fx + 13 * cosf(ae), ty = fy + 13 * sinf(ae);
        vita2d_draw_fill_circle(tx, ty, 3.4f, RGBA8(255, 255, 255, 235));
    }
    vita2d_draw_fill_circle(fx, fy, 4, RGBA8(255, 255, 255, 235));

    /* the last shot, bottom left; a new one flies in from full frame */
    vita2d_texture *lt = last_thumb[0] ? ui_image(last_thumb) : NULL;
    if (!lt) {
        for (int i = 0; i < npics; ++i) {                 /* newest camera shot, not a screenshot from any game */
            if (pics[i].game[0] || pics[i].thumbed <= 0) continue;
            static char tp[80];
            thumb_path(pics[i].path, tp, sizeof(tp));
            lt = ui_image(tp);
            break;
        }
    }
    if (lt) {
        float k = fly > 0 ? fly * fly : 0;
        float w = 96 + (W - 96) * k, h = w * 9 / 16, x = 20 * (1 - k), y = (H - 44 - 54 - 12) * (1 - k) + (H - h) / 2 * k;
        draw_round_texture(lt, x, y, w, h, 8, RGBA8(255, 255, 255, 255));
        draw_round_ring(x, y, w, h, 8, 2, RGBA8(255, 255, 255, 200));
    }
    if (fly > 0) fly -= 0.07f;
    if (flash > 0) { vita2d_draw_rectangle(0, 0, W, H, RGBA8(255, 255, 255, (int)(flash * 230))); flash -= 0.12f; }
    draw_gradient(0, H - 44, W, 44, RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 170), RGBA8(0, 0, 0, 170));
    draw_hints(140, H - 20, "R shutter  /\\ flip  [] effect  L photos  O close", C_TEXT, W - 20);
}

/* Chips at GRID_Y's usual spot, the way Music's section bar works: drawn
 * last so rows scrolled up pass under it, tapped/steered before the grid
 * below gets the input. */
static void photos_chips(void) {
    draw_gradient(0, 65, W, 50, C_BG, C_BG, C_BG, (C_BG & 0x00FFFFFF) | 0xE0000000);
    int cx = GRID_X;
    for (int k = 0; k < NSEC; ++k) {
        int w = text_w(font, 15, SECT[k]) + 30;
        if (chips && k == section) draw_focus(cx, 78, w, 30, 1);
        draw_round_rect(cx, 78, w, 30, 15, k == section ? RGBA8(245, 245, 250, 255) : RGBA8(255, 255, 255, 26));
        text(font, cx + 15, 99, k == section ? RGBA8(15, 15, 20, 255) : C_TEXT, 15, SECT[k]);
        cx += w + 10;
    }
}

/* A plain title bar for a page under a chip (an open album), Circle-back
 * instead of a chip row. */
static void section_title(const char *title, int n) {
    draw_gradient(0, 65, W, GRID_Y - 69, C_BG, C_BG, (C_BG & 0x00FFFFFF) | 0xE0000000, (C_BG & 0x00FFFFFF) | 0xE0000000);
    text(bold, GRID_X + 4, GRID_Y - 16, C_TEXT, 20, title);
    if (n) {
        char count[24];
        snprintf(count, sizeof(count), "%d", n);
        text_right(font, W - 24, GRID_Y - 16, C_FAINT, 15, count);
    }
}

/* Asks, then deletes the picture and its thumbnail. 1 if it went. */
static int delete_pic(const char *path) {
    char msg[200];
    const char *name = strrchr(path, '/');
    snprintf(msg, sizeof(msg), "Delete %s? This can't be undone.", name ? name + 1 : path);
    if (!ui_confirm("Delete photo", msg) || sceIoRemove(path) < 0) return 0;
    char tp[80];
    thumb_path(path, tp, sizeof(tp));
    sceIoRemove(tp);
    ui_image_forget(path);
    ui_image_forget(tp);
    sceKernelWaitSema(roll_lock, 1, NULL);
    for (int k = 0; k < npics; ++k)
        if (!strcmp(pics[k].path, path)) { memmove(&pics[k], &pics[k + 1], (npics - k - 1) * sizeof(Pic)); --npics; break; }
    sceKernelSignalSema(roll_lock, 1);
    ui_toast("Photo deleted", C_ACCENT);
    return 1;
}

/* The camera roll (SEC_CAMERA, mode M_HOME) or one open album's shots (mode
 * M_ALBUM): both are just a filtered, newest-first slice of pics[], so one
 * grid draws either. Returns the slice's count, for the caller's header. */
static int pic_grid_frame(const Input *in) {
    if (in->pressed & SCE_CTRL_SQUARE) {                  /* delete from the grid too */
        sceKernelWaitSema(roll_lock, 1, NULL);
        static int didxs[MAX_PICS];
        int dn = build_cur(didxs);
        char path[160];
        int has = sel < dn;
        if (has) snprintf(path, sizeof(path), "%s", pics[didxs[sel]].path);
        sceKernelSignalSema(roll_lock, 1);
        if (has) delete_pic(path);
    }
    sceKernelWaitSema(roll_lock, 1, NULL);
    static int idxs[MAX_PICS];
    int n = build_cur(idxs);
    if (sel >= n) sel = n ? n - 1 : 0;
    if (n) {
        if (in->pressed & SCE_CTRL_LEFT) sel = sel > 0 ? sel - 1 : 0;
        if (in->pressed & SCE_CTRL_RIGHT) sel = sel < n - 1 ? sel + 1 : sel;
        if (in->pressed & SCE_CTRL_UP) { if (sel >= COLS) sel -= COLS; else if (mode != M_ALBUM) chips = 1; }
        if (in->pressed & SCE_CTRL_DOWN) sel = sel + COLS < n ? sel + COLS : n - 1;
        if (in->pressed & SCE_CTRL_CROSS) { view_from_album = mode == M_ALBUM; mode = M_VIEW; }
    }
    static GridScroll gs[2];                              /* [0] the camera roll, [1] inside an album */
    GridScroll *g = &gs[mode == M_ALBUM];
    grid_scroll(g, &sel, COLS, n, 2, CELL_H, in);
    if (in->tapped && in->tap_y > GRID_Y && in->tap_y < H - 40) {
        int c = (in->tap_x - GRID_X) / CELL_W, r = (int)(g->top + (in->tap_y - GRID_Y) / (float)CELL_H), idx = r * COLS + c;
        if (c >= 0 && c < COLS && idx >= 0 && idx < n) { if (idx == sel) { view_from_album = mode == M_ALBUM; mode = M_VIEW; } else sel = idx; }
    }
    if (!n) {
        sceKernelSignalSema(roll_lock, 1);
        if (mode == M_ALBUM) text(font, 40, 170, C_DIM, 18, "No screenshots in this album yet.");
        else text(font, 40, 170, C_DIM, 18, scanned ? "No photos yet. Press START to open the camera." : "Looking for photos\xE2\x80\xA6");
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        float y = GRID_Y + (i / COLS - g->top) * CELL_H;
        if (y < GRID_Y - CELL_H || y > H - 40) continue;
        float x = GRID_X + (i % COLS) * CELL_W, w = CELL_W - 12, h = CELL_H - 12;
        if (i == sel && !chips) draw_focus(x, y, w, h, 1);
        Pic *pc = &pics[idxs[i]];
        char tp[80];
        thumb_path(pc->path, tp, sizeof(tp));
        vita2d_texture *t = pc->thumbed > 0 ? ui_image(tp) : NULL;
        if (t) {
            draw_round_texture(t, x, y, w, h, ui_corner(w, h), RGBA8(255, 255, 255, i == sel ? 255 : 225));
        } else if (pc->thumbed < 0) {
            draw_round_rect(x, y, w, h, 8, RGBA8(36, 41, 56, 255));
            text_fit(font, (int)x + 10, (int)(y + h / 2 + 5), C_FAINT, 13, strrchr(pc->path, '/') + 1, (int)w - 20);
        } else {
            draw_round_rect(x, y, w, h, 8, RGBA8(36, 41, 56, 255));
            draw_shimmer(x, y, w, h);
        }
    }
    sceKernelSignalSema(roll_lock, 1);
    return n;
}

/* Screenshots' own grid, one tile per game (SEC_SHOTS, mode M_HOME). */
static void albums_grid_frame(const Input *in) {
    sceKernelWaitSema(roll_lock, 1, NULL);
    build_albums();
    int n = nalbums;
    if (sel_album >= n) sel_album = n ? n - 1 : 0;
    if (n) {
        if (in->pressed & SCE_CTRL_LEFT) sel_album = sel_album > 0 ? sel_album - 1 : 0;
        if (in->pressed & SCE_CTRL_RIGHT) sel_album = sel_album < n - 1 ? sel_album + 1 : sel_album;
        if (in->pressed & SCE_CTRL_UP) { if (sel_album >= COLS) sel_album -= COLS; else chips = 1; }
        if (in->pressed & SCE_CTRL_DOWN) sel_album = sel_album + COLS < n ? sel_album + COLS : n - 1;
    }
    int open_ix = (n && (in->pressed & SCE_CTRL_CROSS)) ? sel_album : -1;
    static GridScroll gs;
    grid_scroll(&gs, &sel_album, COLS, n, 2, CELL_H_ALB, in);
    if (in->tapped && in->tap_y > GRID_Y && in->tap_y < H - 40) {
        int c = (in->tap_x - GRID_X) / CELL_W, r = (int)(gs.top + (in->tap_y - GRID_Y) / (float)CELL_H_ALB), idx = r * COLS + c;
        if (c >= 0 && c < COLS && idx >= 0 && idx < n) { if (idx == sel_album) open_ix = idx; else sel_album = idx; }
    }
    char opened[48];
    if (open_ix >= 0) snprintf(opened, sizeof(opened), "%s", albums[open_ix].game);
    if (!n) {
        sceKernelSignalSema(roll_lock, 1);
        text(font, 40, 170, C_DIM, 18, scanned ? "No screenshots yet. PS + START on any game saves one." : "Looking for photos\xE2\x80\xA6");
        return;
    }
    for (int i = 0; i < n; ++i) {
        float y = GRID_Y + (i / COLS - gs.top) * CELL_H_ALB;
        if (y < GRID_Y - CELL_H_ALB || y > H - 40) continue;
        float x = GRID_X + (i % COLS) * CELL_W, w = CELL_W - 12;
        if (i == sel_album && !chips) draw_focus(x, y, w, w, 1);
        char tp[80];
        thumb_path(pics[albums[i].cover].path, tp, sizeof(tp));
        vita2d_texture *t = pics[albums[i].cover].thumbed > 0 ? ui_image(tp) : NULL;
        if (t) draw_round_texture(t, x, y, w, w, ui_corner(w, w), RGBA8(255, 255, 255, i == sel_album ? 255 : 225));
        else { draw_round_rect(x, y, w, w, 8, RGBA8(36, 41, 56, 255)); draw_shimmer(x, y, w, w); }
        text_fit(i == sel_album ? bold : font, (int)x, (int)(y + w + 18), i == sel_album ? C_TEXT : C_DIM, 14, albums[i].game, (int)w);
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%d shot%s", albums[i].count, albums[i].count == 1 ? "" : "s");
        text(font, (int)x, (int)(y + w + 34), C_FAINT, 12, cnt);
    }
    sceKernelSignalSema(roll_lock, 1);
    if (open_ix >= 0) { snprintf(album_game, sizeof(album_game), "%s", opened); mode = M_ALBUM; sel = 0; }
}

/* The dispatcher for mode M_HOME (chips + whichever section's grid) and
 * M_ALBUM (one album, Circle back to its grid). */
static void photos_frame(const Input *in) {
    if (in->pressed & SCE_CTRL_START) { camera_open(0); return; }
    unsigned int p = in->pressed;
    if (mode == M_ALBUM) {
        if (p & SCE_CTRL_CIRCLE) { mode = M_HOME; return; }
    } else if (chips) {
        if (p & SCE_CTRL_LEFT && section > 0) { --section; sel = 0; }
        if (p & SCE_CTRL_RIGHT && section < NSEC - 1) { ++section; sel = 0; }
        if (p & (SCE_CTRL_DOWN | SCE_CTRL_CROSS)) chips = 0;
        p = 0;
    } else if (p & SCE_CTRL_CIRCLE) { mode = M_OFF; return; }

    if (mode == M_HOME) {
        int cx = GRID_X;
        for (int k = 0; k < NSEC; ++k) {                  /* chip taps, before the grid below sees them */
            int w = text_w(font, 15, SECT[k]) + 30;
            if (in->tapped && in->tap_x >= cx && in->tap_x < cx + w && in->tap_y >= 74 && in->tap_y < 112) { section = k; sel = 0; p = 0; }
            cx += w + 10;
        }
    }
    Input in2 = *in;
    in2.pressed = p;
    if (mode == M_HOME && in2.tapped && in2.tap_y < 112) in2.tapped = 0;

    if (mode == M_ALBUM) {
        int n = pic_grid_frame(&in2);
        section_title(album_game, n);
        return;
    }
    if (section == SEC_CAMERA) pic_grid_frame(&in2);
    else albums_grid_frame(&in2);
    photos_chips();                                       /* last, over rows scrolled up under it */
}

static void view_frame(const Input *in) {
    sceKernelWaitSema(roll_lock, 1, NULL);
    static int idxs[MAX_PICS];
    int n = build_cur(idxs);
    if (!n) { sceKernelSignalSema(roll_lock, 1); mode = view_from_album ? M_ALBUM : M_HOME; return; }
    if (sel >= n) sel = n - 1;
    Pic p = pics[idxs[sel]];
    sceKernelSignalSema(roll_lock, 1);
    static float swipe;
    if (in->touching) swipe += in->drag_dx;
    if (in->released) {
        if (swipe < -80 && sel < n - 1) ++sel;
        else if (swipe > 80 && sel > 0) --sel;
        swipe = 0;
    }
    if (in->pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) sel = sel < n - 1 ? sel + 1 : sel;
    if (in->pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) sel = sel > 0 ? sel - 1 : 0;
    if (in->pressed & SCE_CTRL_CIRCLE) { mode = view_from_album ? M_ALBUM : M_HOME; return; }
    if ((in->pressed & SCE_CTRL_SQUARE) && delete_pic(p.path)) { mode = view_from_album ? M_ALBUM : M_HOME; return; }
    vita2d_draw_rectangle(0, 0, W, H, RGBA8(0, 0, 0, 255));
    vita2d_texture *t = ui_image(p.path);
    if (!t) {                                     /* the thumbnail, blown up, while the full one loads */
        char tp[80];
        thumb_path(p.path, tp, sizeof(tp));
        t = p.thumbed > 0 ? ui_image(tp) : NULL;
    }
    if (t) {
        float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
        float sc = W / tw < H / th ? W / tw : H / th;
        vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        vita2d_draw_texture_scale(t, (W - tw * sc) / 2 + swipe, (H - th * sc) / 2, sc, sc);
    } else draw_shimmer(W / 2 - 160, H / 2 - 90, 320, 180);
    const char *name = strrchr(p.path, '/');
    char line[200];
    snprintf(line, sizeof(line), "%s     %d/%d     %04d-%02d-%02d %02d:%02d", name ? name + 1 : p.path, sel + 1, n,
             p.when.year, p.when.month, p.when.day, p.when.hour, p.when.minute);
    draw_gradient(0, H - 46, W, 46, RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 170), RGBA8(0, 0, 0, 170));
    text_fit(font, 20, H - 16, C_DIM, 14, line, W - 330);
    {
        const char *h = "<- -> browse  [] delete  O back";
        draw_hints(W - 20 - hints_width(h), H - 20, h, C_DIM, W);
    }
}

void camera_update(const Input *in) {
    if (roll_lock < 0) roll_lock = sceKernelCreateSema("roll_lock", 0, 1, 1, NULL);
    switch (mode) {
    case M_FINDER: finder_frame(in); break;
    case M_HOME: case M_ALBUM: photos_frame(in); break;
    case M_VIEW: view_frame(in); break;
    }
}
