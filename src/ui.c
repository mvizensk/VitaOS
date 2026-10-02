#include <math.h>
#include <psp2/motion.h>
#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <stdlib.h>
#include <psp2/io/stat.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/rtc.h>
#include <psp2/power.h>
#include <psp2/net/netctl.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "ui.h"
#include "lang.h"
#include "video.h"

UiFont *font, *bold;

static int in_frame;

static unsigned int frames;
volatile const char *ui_where = "";
volatile int ui_agent_active;
static void free_evicted(void);
unsigned int ui_accent = C_ACCENT_DEFAULT;
static float acc[3] = {88, 166, 255}, acc_to[3] = {88, 166, 255};

void ui_theme_color(unsigned int c) {
    acc_to[0] = c & 0xFF; acc_to[1] = (c >> 8) & 0xFF; acc_to[2] = (c >> 16) & 0xFF;
}

/* Settings > Theme: pin the accent to one of these instead of the art. */
static const unsigned int THEME_ACCENTS[6] = {
    C_ACCENT_DEFAULT, RGBA8(168, 120, 255, 255), RGBA8(255, 120, 190, 255),
    RGBA8(255, 150, 70, 255), RGBA8(110, 220, 110, 255), RGBA8(70, 210, 200, 255),
};
static const char *const THEME_ACCENT_NAMES[6] = {"Blue", "Purple", "Pink", "Orange", "Green", "Teal"};
static int theme_accent_idx = -1;    /* -1 = match the art */

void ui_theme_set_accent(int idx) { theme_accent_idx = idx < -1 || idx > 5 ? -1 : idx; }
int ui_theme_accent_index(void) { return theme_accent_idx; }
const char *ui_theme_accent_name(int idx) { return idx >= 0 && idx < 6 ? THEME_ACCENT_NAMES[idx] : "Match the art"; }

void ui_theme_default(void) { ui_theme_color(theme_accent_idx >= 0 ? THEME_ACCENTS[theme_accent_idx] : C_ACCENT_DEFAULT); }

/* Settings > Theme: the background behind tabs with no backdrop art. */
static ThemeBg theme_bg = THEME_BG_AURORA;
static char theme_wallpaper[64] = "";

void ui_theme_set_bg(ThemeBg bg, const char *wallpaper) {
    theme_bg = bg;
    if (bg == THEME_BG_WALLPAPER && wallpaper) snprintf(theme_wallpaper, sizeof(theme_wallpaper), "%s", wallpaper);
}
ThemeBg ui_theme_bg(void) { return theme_bg; }
const char *ui_theme_bg_name(ThemeBg bg) {
    switch (bg) { case THEME_BG_PLAIN: return "Plain"; case THEME_BG_MIDNIGHT: return "Midnight";
                  case THEME_BG_WALLPAPER: return "Wallpaper"; default: return "Aurora"; }
}
const char *ui_theme_wallpaper(void) { return theme_wallpaper; }

#define THEME_CFG "ux0:data/arcadehub/user/theme.cfg"
void ui_theme_load(void) {
    char b[128] = {0};
    SceUID fd = sceIoOpen(THEME_CFG, SCE_O_RDONLY, 0);
    if (fd < 0) return;                                   /* no file yet: match the art, Aurora */
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    int a = -1, bgv = 0;
    char w[64] = "-";
    if (sscanf(b, "%d %d %63s", &a, &bgv, w) < 2) return;
    ui_theme_set_accent(a);
    theme_bg = bgv < 0 || bgv > THEME_BG_WALLPAPER ? THEME_BG_AURORA : (ThemeBg)bgv;
    if (theme_bg == THEME_BG_WALLPAPER && strcmp(w, "-")) snprintf(theme_wallpaper, sizeof(theme_wallpaper), "%s", w);
}

/* The most vivid colour that covers a good part of the art: hue buckets
 * weighted by saturation and brightness, then lifted so it reads on the dark
 * UI. 16x16 samples; the answer is cached per texture. */
static unsigned int art_accent(vita2d_texture *t) {
    static struct { vita2d_texture *t; unsigned int c; } memo[24];
    static int next;
    for (int i = 0; i < 24; ++i) if (memo[i].t == t) return memo[i].c;
    unsigned int result = C_ACCENT_DEFAULT;
    if (vita2d_texture_get_format(t) == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR) {
        const unsigned char *px = vita2d_texture_get_datap(t);
        unsigned int w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t), stride = vita2d_texture_get_stride(t);
        float weight[12] = {0}, sum[12][3] = {{0}};
        for (int gy = 0; gy < 16; ++gy)
            for (int gx = 0; gx < 16; ++gx) {
                const unsigned char *p = px + (h * (2 * gy + 1) / 32) * stride + (w * (2 * gx + 1) / 32) * 4;
                float r = p[0], g = p[1], b = p[2];
                float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                if (mx < 40) continue;                                   /* too dark to say */
                float sat = (mx - mn) / mx, val = mx / 255;
                if (sat < 0.25f) continue;                              /* greys and whites */
                float hue = mx == r ? (g - b) / (mx - mn) : mx == g ? 2 + (b - r) / (mx - mn) : 4 + (r - g) / (mx - mn);
                int bucket = ((int)((hue < 0 ? hue + 6 : hue) * 2)) % 12;
                float wgt = sat * sat * val;
                weight[bucket] += wgt;
                sum[bucket][0] += r * wgt; sum[bucket][1] += g * wgt; sum[bucket][2] += b * wgt;
            }
        int best = -1;
        for (int k = 0; k < 12; ++k) if (weight[k] > 1.2f && (best < 0 || weight[k] > weight[best])) best = k;
        if (best >= 0) {
            float r = sum[best][0] / weight[best], g = sum[best][1] / weight[best], b = sum[best][2] / weight[best];
            float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float lift = 235 / (mx > 1 ? mx : 1);                       /* bright enough for the dark UI */
            r *= lift; g *= lift; b *= lift;
            float grey = (r + g + b) / 3;                               /* and not neon */
            r = grey + (r - grey) * 0.8f; g = grey + (g - grey) * 0.8f; b = grey + (b - grey) * 0.8f;
            result = RGBA8((int)(r > 255 ? 255 : r), (int)(g > 255 ? 255 : g), (int)(b > 255 ? 255 : b), 255);
        }
    }
    memo[next].t = t;
    memo[next].c = result;
    next = (next + 1) % 24;
    return result;
}

void ui_theme_from(vita2d_texture *art) {
    if (theme_accent_idx >= 0) { ui_theme_color(THEME_ACCENTS[theme_accent_idx]); return; }   /* pinned: ignore the art */
    if (art) ui_theme_color(art_accent(art)); else ui_theme_default();
}

/* Frames are drawn into an offscreen target and then shown, so a dialog can
 * blur whatever was on screen behind it (PS5-style). blur_rt is a small
 * copy; scaled back up with linear filtering it is a soft blur for free. */
static vita2d_texture *scenes[2], *scene, *blur_rt, *blur_rt2;
static int cur, last_done = -1;              /* dialogs blur the last *finished* frame */

/* Tilt from the accelerometer, relative to how the console has been held
 * lately (a slow baseline), so any holding angle is "level" and only a
 * change of angle moves things. */
float ui_tilt_x, ui_tilt_y;
static void motion_tick(void) {
    static float bx, by, sx, sy;
    static int primed;
    SceMotionState m;
    if (sceMotionGetState(&m) < 0) return;
    float ax = m.acceleration.x, ay = m.acceleration.y;
    if (!primed) { bx = ax; by = ay; primed = 1; }
    bx += (ax - bx) * 0.012f;                        /* ~1.5 s to settle on a new grip */
    by += (ay - by) * 0.012f;
    float tx = (ax - bx) * 4, ty = (ay - by) * 4;
    tx = tx < -1 ? -1 : tx > 1 ? 1 : tx;
    ty = ty < -1 ? -1 : ty > 1 ? 1 : ty;
    sx += (tx - sx) * 0.15f;
    sy += (ty - sy) * 0.15f;
    ui_tilt_x = sx;
    ui_tilt_y = sy;
}

void ui_begin_frame(void) {
    motion_tick();
    if (!scenes[0]) {
        scenes[0] = vita2d_create_empty_texture_rendertarget(W, H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        scenes[1] = vita2d_create_empty_texture_rendertarget(W, H, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        if (!scenes[1]) scenes[1] = scenes[0];
        blur_rt = vita2d_create_empty_texture_rendertarget(240, 136, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        blur_rt2 = vita2d_create_empty_texture_rendertarget(96, 54, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    }
    scene = scenes[cur];
    /* Reset vita2d's vertex pool once, here, at the top of a frame. Presenting
     * below must not use vita2d_start_drawing(), which resets it again while
     * the GPU may still be reading this frame's vertices. */
    free_evicted();
    video_collect();
    if (scene) { vita2d_pool_reset(); vita2d_start_drawing_advanced(scene, 0); }
    else vita2d_start_drawing();
    vita2d_clear_screen();
    in_frame = 1;
    ++frames;
    for (int k = 0; k < 3; ++k) acc[k] += (acc_to[k] - acc[k]) * 0.08f;   /* ~0.5 s to settle */
    ui_accent = RGBA8((int)acc[0], (int)acc[1], (int)acc[2], 255);
}

/* ---------- toasts ---------- */

/* Small notices that slide in under the clock and leave by themselves.
 * Any thread may post one (the download worker does), so posting only copies
 * into a ring; the main loop draws. */
#define TOASTS 4
static struct { char text[96]; unsigned int color; int frames; } toasts[TOASTS];
static volatile unsigned int toast_head;
static SceUID toast_lock = -1;

void ui_toast(const char *text, unsigned int color) {
    if (toast_lock < 0) toast_lock = sceKernelCreateSema("toast", 0, 1, 1, NULL);
    sceKernelWaitSema(toast_lock, 1, NULL);
    unsigned int i = toast_head++ % TOASTS;
    snprintf(toasts[i].text, sizeof(toasts[i].text), "%s", tr(text));
    toasts[i].color = color;
    toasts[i].frames = 240;                      /* 4 s */
    sceKernelSignalSema(toast_lock, 1);
}

void ui_draw_toasts(void) {
    int y = 76;
    for (int k = 0; k < TOASTS; ++k) {
        unsigned int i = (toast_head - 1 - k) % TOASTS;   /* newest on top */
        if (toasts[i].frames <= 0) continue;
        int f = toasts[i].frames--, age = 240 - f;
        float in = age < 14 ? age / 14.0f : 1, out = f < 18 ? f / 18.0f : 1, t = in < out ? in : out;
        float ease = 1 - (1 - t) * (1 - t);
        int w = text_w(font, 16, toasts[i].text) + 52, x = (int)(W - 16 - w * ease);
        vita2d_draw_rectangle(x + 3, y + 4, w, 40, RGBA8(0, 0, 0, (int)(70 * t)));
        vita2d_draw_rectangle(x, y, w, 40, RGBA8(30, 34, 46, (int)(242 * t)));
        vita2d_draw_rectangle(x, y, 4, 40, (toasts[i].color & 0x00FFFFFF) | ((unsigned int)(255 * t) << 24));
        vita2d_draw_rectangle(x + 18, y + 16, 8, 8, (toasts[i].color & 0x00FFFFFF) | ((unsigned int)(255 * t) << 24));
        text(font, x + 36, y + 26, (C_TEXT & 0x00FFFFFF) | ((unsigned int)(255 * t) << 24), 16, toasts[i].text);
        y += 48;
    }
}

/* Word-wrapped text, up to max_lines; newlines start a new line. */
void draw_wrapped_text(const char *s, int x, int y, int width, int size, int max_lines, unsigned int color) {
    s = tr(s);
    char line[512] = {0};
    int lines = 0, len = 0;
    const char *p = s;
    while (p && *p && lines < max_lines) {
        const char *word = p;
        while (*p && *p != ' ' && *p != '\n') ++p;
        int wl = p - word;
        char trial[512];
        snprintf(trial, sizeof(trial), "%s%s%.*s", line, len ? " " : "", wl, word);
        if (len && text_w(font, size, trial) > width) {
            text(font, x, y + lines * (size + 8), color, size, line);
            lines++;
            snprintf(line, sizeof(line), "%.*s", wl, word);
        } else snprintf(line, sizeof(line), "%s", trial);
        len = strlen(line);
        if (*p == '\n') {
            if (lines < max_lines) text(font, x, y + lines * (size + 8), color, size, line);
            lines++; line[0] = 0; len = 0;
        }
        if (*p) ++p;
    }
    if (len && lines < max_lines) text(font, x, y + lines * (size + 8), color, size, line);
}

/* ---------- images, loaded off the main thread ----------
 * Reading the memory card here can block for a second or more, and every tab
 * used to decode its art on the main thread: each first visit froze the UI
 * (measured 0.4-2 s, 20-44 s in the store). Now ui_image() only looks up a
 * cache; a worker thread does the reading and decoding, and the picture shows
 * up a frame or two later. Evicted textures are freed at the start of the next
 * frame, after the GPU has finished with them. */
#define IMGS 160
typedef struct { char path[200]; vita2d_texture *tex; volatile int state; unsigned int used; int tries; SceUInt64 failed_at; } Img;   /* 0 free 1 queued 2 loading 3 ready 4 failed */
static Img imgs[IMGS];
static unsigned int img_tick;
static SceUID img_sema = -1, img_lock = -1;
static vita2d_texture *to_free[IMGS];
static int nfree;

static void round_corners(vita2d_texture *t, float frac) {
    if (vita2d_texture_get_format(t) != SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR) return;
    unsigned int w = vita2d_texture_get_width(t), h = vita2d_texture_get_height(t);
    unsigned int stride = vita2d_texture_get_stride(t) / 4, *px = vita2d_texture_get_datap(t);
    float r = (w < h ? w : h) * frac;
    for (unsigned int y = 0; y < h; ++y) {
        float cy = y + 0.5f < r ? r : y + 0.5f > h - r ? h - r : -1;
        if (cy < 0) continue;
        for (unsigned int x = 0; x < w; ++x) {
            float cx = x + 0.5f < r ? r : x + 0.5f > w - r ? w - r : -1;
            if (cx < 0) continue;
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            float cover = r + 0.5f - d;                          /* 1 inside, 0 outside, soft edge */
            if (cover >= 1) continue;
            unsigned int p = px[y * stride + x], a = p >> 24;
            a = cover <= 0 ? 0 : (unsigned int)(a * cover);
            px[y * stride + x] = (p & 0x00FFFFFFu) | (a << 24);
        }
    }
}

static int img_worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    char path[200];
    for (;;) {
        sceKernelWaitSema(img_sema, 1, NULL);
        for (;;) {
            int pick = -1;
            sceKernelWaitSema(img_lock, 1, NULL);
            for (int i = 0; i < IMGS; ++i)                       /* the most recently wanted first */
                if (imgs[i].state == 1 && (pick < 0 || imgs[i].used > imgs[pick].used)) pick = i;
            if (pick >= 0) { imgs[pick].state = 2; memcpy(path, imgs[pick].path, sizeof(path)); }
            sceKernelSignalSema(img_lock, 1);
            if (pick < 0) break;
            /* "<file>#round": the corners are cut round (antialiased) into the
             * picture itself, so it sits on any background like an app icon. */
            char *round = strstr(path, "#round");
            if (round) *round = 0;
            const char *dot = strrchr(path, '.');
            SceIoStat st;
            vita2d_texture *t = NULL;
            if (sceIoGetstat(path, &st) >= 0 && st.st_size > 0)
                t = dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) ? vita2d_load_JPEG_file(path)
                                                                                  : vita2d_load_PNG_file(path);
            if (t && round) round_corners(t, 0.22f);
            imgs[pick].tex = t;
            if (!t) { imgs[pick].tries++; imgs[pick].failed_at = sceKernelGetProcessTimeWide(); }
            imgs[pick].state = t ? 3 : 4;
        }
    }
    return 0;
}

vita2d_texture *ui_image(const char *path) {
    if (!path || !*path) return NULL;
    if (img_sema < 0) {
        img_sema = sceKernelCreateSema("img_wake", 0, 0, 1, NULL);
        img_lock = sceKernelCreateSema("img_lock", 0, 1, 1, NULL);
        SceUID t = sceKernelCreateThread("img_loader", img_worker, 0x10000110, 0x10000, 0, 0, NULL);   /* below the UI */
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
    int victim = -1;
    for (int i = 0; i < IMGS; ++i) {
        if (imgs[i].state && !strcmp(imgs[i].path, path)) {
            imgs[i].used = ++img_tick;
            /* A failure can be passing (memory or the card busy at startup): try
             * twice more, 2 s apart. Red Vita's Apps icons stayed blank until
             * scrolled out of the cache and back (2026-10-01). */
            if (imgs[i].state == 4 && imgs[i].tries < 3 && sceKernelGetProcessTimeWide() - imgs[i].failed_at > 2000000) {
                imgs[i].state = 1;
                sceKernelSignalSema(img_sema, 1);
            }
            return imgs[i].state == 3 ? imgs[i].tex : NULL;
        }
        if (!imgs[i].state) victim = i;
    }
    sceKernelWaitSema(img_lock, 1, NULL);
    if (victim < 0)                                          /* full: the least recently used finished one */
        for (int i = 0; i < IMGS; ++i)
            if ((imgs[i].state == 3 || imgs[i].state == 4 || imgs[i].state == 1) && (victim < 0 || imgs[i].used < imgs[victim].used)) victim = i;
    if (victim >= 0) {
        if (imgs[victim].state == 3 && imgs[victim].tex && nfree < IMGS) to_free[nfree++] = imgs[victim].tex;
        imgs[victim].tex = NULL;
        snprintf(imgs[victim].path, sizeof(imgs[victim].path), "%s", path);
        imgs[victim].used = ++img_tick;
        imgs[victim].tries = 0;
        imgs[victim].state = 1;
    }
    sceKernelSignalSema(img_lock, 1);
    sceKernelSignalSema(img_sema, 1);
    return NULL;
}

/* 1 while a picture is queued or being decoded (show a shimmer), 0 once it
 * is ready or known to be missing. */
int ui_image_pending(const char *path) {
    for (int i = 0; i < IMGS; ++i)
        if ((imgs[i].state == 1 || imgs[i].state == 2) && !strcmp(imgs[i].path, path)) return 1;
    return 0;
}

/* The outline of a rounded rectangle, clockwise: 8 steps per corner. */
#define RSEG 8
static int round_poly(float x, float y, float w, float h, float r, float *px, float *py) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    static const float cs[RSEG + 1] = {1, 0.9808f, 0.9239f, 0.8315f, 0.7071f, 0.5556f, 0.3827f, 0.1951f, 0};
    const float cx[4] = {x + w - r, x + w - r, x + r, x + r}, cy[4] = {y + r, y + h - r, y + h - r, y + r};
    int n = 0;
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k <= RSEG; ++k) {
            float a = cs[RSEG - k], b = cs[k];            /* (sin, cos) of k * 90 / RSEG degrees */
            float dx, dy;
            switch (c) {
            case 0: dx = a; dy = -b; break;               /* top right: from the top edge down to the right edge */
            case 1: dx = b; dy = a; break;
            case 2: dx = -a; dy = b; break;
            default: dx = -b; dy = -a; break;
            }
            px[n] = cx[c] + dx * r;
            py[n] = cy[c] + dy * r;
            ++n;
        }
    return n;
}

/* Rounded shapes as single polygons: no overlaps, so translucent colours
 * stay even (the old version stacked four circles on three rectangles). */
void draw_round_rect(float x, float y, float w, float h, float r, unsigned int c) {
    float px[4 * (RSEG + 1)], py[4 * (RSEG + 1)];
    int n = round_poly(x, y, w, h, r, px, py);
    vita2d_color_vertex *v = vita2d_pool_memalign((n + 2) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    v[0] = (vita2d_color_vertex){x + w / 2, y + h / 2, 0.5f, c};
    for (int k = 0; k < n; ++k) v[k + 1] = (vita2d_color_vertex){px[k], py[k], 0.5f, c};
    v[n + 1] = v[1];
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 2);
}

/* A rounded outline of the given thickness (inside the box). */
void draw_round_ring(float x, float y, float w, float h, float r, float t, unsigned int c) {
    float ox[4 * (RSEG + 1)], oy[4 * (RSEG + 1)], ix[4 * (RSEG + 1)], iy[4 * (RSEG + 1)];
    int n = round_poly(x, y, w, h, r, ox, oy);
    round_poly(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, ix, iy);
    vita2d_color_vertex *v = vita2d_pool_memalign((2 * n + 2) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    for (int k = 0; k <= n; ++k) {
        int m = k % n;
        v[2 * k] = (vita2d_color_vertex){ox[m], oy[m], 0.5f, c};
        v[2 * k + 1] = (vita2d_color_vertex){ix[m], iy[m], 0.5f, c};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * n + 2);
}

/* The corner radius for a tile of this size: what the focus ring uses, so
 * whatever sits inside the ring has the same corners (square covers under a
 * rounded ring looked wrong on Home, 2026-09-25). */
float ui_corner(float w, float h) {
    float r = (w < h ? w : h) * 0.16f;
    return r < 6 ? 6 : r > 22 ? 22 : r;
}

/* A picture with rounded corners, cut at draw time (works for any format).
 * The _uv form shows only the part (u0,v0)-(u1,v1), 0..1, e.g. a cover cropped square. */
void draw_round_texture_uv(vita2d_texture *t, float x, float y, float w, float h, float r,
                           float u0, float v0, float u1, float v1, unsigned int tint) {
    float px[4 * (RSEG + 1)], py[4 * (RSEG + 1)];
    int n = round_poly(x, y, w, h, r, px, py);
    vita2d_texture_vertex *v = vita2d_pool_memalign((n + 2) * sizeof(vita2d_texture_vertex), sizeof(vita2d_texture_vertex));
    if (!v) return;
    float du = u1 - u0, dv = v1 - v0;
    v[0] = (vita2d_texture_vertex){x + w / 2, y + h / 2, 0.5f, u0 + du / 2, v0 + dv / 2};
    for (int k = 0; k < n; ++k)
        v[k + 1] = (vita2d_texture_vertex){px[k], py[k], 0.5f, u0 + du * (px[k] - x) / w, v0 + dv * (py[k] - y) / h};
    v[n + 1] = v[1];
    vita2d_draw_array_textured(t, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 2, tint);
}

void draw_round_texture(vita2d_texture *t, float x, float y, float w, float h, float r, unsigned int tint) {
    draw_round_texture_uv(t, x, y, w, h, r, 0, 0, 1, 1, tint);
}

/* Fills the square, cropping the long side, with rounded corners. */
void draw_round_cover(vita2d_texture *t, float x, float y, float s, float r, unsigned int tint) {
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    float cu = tw > th ? th / tw : 1, cv = th > tw ? tw / th : 1;
    draw_round_texture_uv(t, x, y, s, s, r, (1 - cu) / 2, (1 - cv) / 2, (1 + cu) / 2, (1 + cv) / 2, tint);
}

/* A rounded rectangle shaded left to right between two colours. */
void draw_round_gradient(float x, float y, float w, float h, float r, unsigned int left, unsigned int right) {
    float px[4 * (RSEG + 1)], py[4 * (RSEG + 1)];
    int n = round_poly(x, y, w, h, r, px, py);
    vita2d_color_vertex *v = vita2d_pool_memalign((n + 2) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    #define MIX(f) (((unsigned int)(((left >> 24) & 255) * (1 - (f)) + ((right >> 24) & 255) * (f)) << 24) | \
                    ((unsigned int)(((left >> 16) & 255) * (1 - (f)) + ((right >> 16) & 255) * (f)) << 16) | \
                    ((unsigned int)(((left >> 8) & 255) * (1 - (f)) + ((right >> 8) & 255) * (f)) << 8) | \
                    (unsigned int)((left & 255) * (1 - (f)) + (right & 255) * (f)))
    v[0] = (vita2d_color_vertex){x + w / 2, y + h / 2, 0.5f, MIX(0.5f)};
    for (int k = 0; k < n; ++k) { float f = (px[k] - x) / w; v[k + 1] = (vita2d_color_vertex){px[k], py[k], 0.5f, MIX(f)}; }
    #undef MIX
    v[n + 1] = v[1];
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 2);
}

/* An app icon the Android way: a rounded tile with a soft shadow; a shimmer
 * while it loads; the name's initials on a tinted tile if there is none. */
void draw_app_icon(const char *path, float x, float y, float size, const char *name, int dim) {
    const char *key = path;
    vita2d_texture *t = *path ? ui_image(key) : NULL;
    draw_soft(ui_soft_shadow(), x + size / 2, y + size + 2, size * 0.9f, size * 0.2f, RGBA8(0, 0, 0, 120));
    if (t) {
        draw_round_texture(t, x, y, size, size, size * 0.22f, RGBA8(255, 255, 255, dim ? 200 : 255));
        return;
    }
    if (*path && ui_image_pending(key)) {
        draw_round_rect(x, y, size, size, size * 0.22f, RGBA8(36, 41, 56, 255));
        draw_shimmer(x + size * 0.1f, y, size * 0.8f, size);
        return;
    }
    unsigned int hsh = 0;                                          /* a colour of its own */
    for (const char *q = name; *q; ++q) hsh = hsh * 31 + (unsigned char)*q;
    static const unsigned int tints[] = {0x3B82F6, 0x8B5CF6, 0x10B981, 0xF59E0B, 0xEF4444, 0x06B6D4, 0xEC4899};
    unsigned int c = tints[hsh % 7];
    draw_round_rect(x, y, size, size, size * 0.22f, RGBA8((c >> 16) & 255, (c >> 8) & 255, c & 255, 255));
    char ini[3] = {name[0], 0, 0};
    for (const char *q = name + 1; *q; ++q) if (q[-1] == ' ' && *q != ' ') { ini[1] = *q; break; }
    int fs = (int)(size * 0.36f), iw = text_w(bold, fs, ini);
    text(bold, (int)(x + (size - iw) / 2), (int)(y + size / 2 + fs * 0.36f), RGBA8(255, 255, 255, 240), fs, ini);
}

/* A rectangle shaded between four corner colours (top-left, top-right,
 * bottom-left, bottom-right). */
void draw_gradient(float x, float y, float w, float h, unsigned int tl, unsigned int tr, unsigned int bl, unsigned int br) {
    vita2d_color_vertex *v = vita2d_pool_memalign(4 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    v[0] = (vita2d_color_vertex){x, y, 0.5f, tl};
    v[1] = (vita2d_color_vertex){x + w, y, 0.5f, tr};
    v[2] = (vita2d_color_vertex){x, y + h, 0.5f, bl};
    v[3] = (vita2d_color_vertex){x + w, y + h, 0.5f, br};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

/* Cover art for music that has none: two colours of its own (from the name),
 * a soft light in one corner, the initials, and the name small underneath. */
void draw_made_cover(const char *name, const char *sub, float x, float y, float s, int dim) {
    unsigned int hsh = 2166136261u;
    for (const char *q = name; *q; ++q) hsh = (hsh ^ (unsigned char)*q) * 16777619u;
    static const unsigned int pal[][2] = {{0x1E3A8A, 0x7C3AED}, {0x0F766E, 0x22D3EE}, {0x9D174D, 0xF97316},
                                          {0x312E81, 0xEC4899}, {0x14532D, 0x84CC16}, {0x7C2D12, 0xFACC15},
                                          {0x1F2937, 0x60A5FA}, {0x581C87, 0xF472B6}};
    unsigned int a = pal[hsh % 8][0], b = pal[hsh % 8][1], al = dim ? 200 : 255;
#define RGB_(c, al) RGBA8(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255, al)
    draw_round_gradient(x, y, s, s, ui_corner(s, s), RGB_(b, al), RGB_(a, al));
    draw_soft(ui_glow(), x + s * 0.34f, y + s * 0.3f, s * 0.62f, s * 0.56f, RGBA8(255, 255, 255, 34));   /* kept inside the tile */
    char ini[3] = {name[0] == '<' ? '?' : name[0], 0, 0};
    for (const char *q = name + 1; *q; ++q) if (q[-1] == ' ' && *q != ' ') { ini[1] = *q; break; }
    int fs = (int)(s * 0.34f);
    text(bold, (int)(x + s * 0.1f), (int)(y + s * 0.5f), RGBA8(255, 255, 255, 235), fs, ini);
    int ts = (int)(s * 0.1f);
    if (ts >= 9) {
        text_fit(bold, (int)(x + s * 0.1f), (int)(y + s * 0.78f), RGBA8(255, 255, 255, 220), ts, name, (int)(s * 0.8f));
        if (sub && *sub) text_fit(font, (int)(x + s * 0.1f), (int)(y + s * 0.78f + ts * 1.3f), RGBA8(255, 255, 255, 160), ts, sub, (int)(s * 0.8f));
    }
#undef RGB_
}

/* The loading placeholder: a tile with a soft highlight sweeping across. */
void draw_shimmer(float x, float y, float w, float h) {
    float r = ui_corner(w, h);
    draw_round_rect(x, y, w, h, r, RGBA8(36, 41, 56, 255));
    x += r; w -= 2 * r;                                            /* the sweep stays clear of the corners */
    if (w <= 0) return;
    float t = (frames % 90) / 90.0f, band = w * 0.6f, cx = x - band + (w + 2 * band) * t;
    const int steps = 12;
    for (int k = 0; k < steps; ++k) {
        float f = (float)k / (steps - 1), sx = cx - band / 2 + band * f, sw = band / steps + 1;
        float a = 1 - (2 * f - 1) * (2 * f - 1);                   /* strongest in the middle */
        float x0 = sx < x ? x : sx, x1 = sx + sw > x + w ? x + w : sx + sw;
        if (x1 > x0) vita2d_draw_rectangle(x0, y, x1 - x0, h, RGBA8(255, 255, 255, (int)(22 * a)));
    }
}

/* Forget a path (the file changed, or a failed load should be tried again). */
void ui_image_forget(const char *path) {
    for (int i = 0; i < IMGS; ++i)
        if ((imgs[i].state == 3 || imgs[i].state == 4) && !strcmp(imgs[i].path, path)) {
            if (imgs[i].tex && nfree < IMGS) to_free[nfree++] = imgs[i].tex;
            imgs[i].tex = NULL;
            imgs[i].state = 0;
        }
}

static void free_evicted(void) {
    if (!nfree) return;
    vita2d_wait_rendering_done();
    for (int i = 0; i < nfree; ++i) vita2d_free_texture(to_free[i]);
    nfree = 0;
}

/* ---------- file writes, off the main thread ----------
 * A write on the main thread waited 53 s once while other threads were busy
 * with the card (2026-09-24). Saves are queued here; the newest content for a
 * path wins, appends are kept in order. */
#define SAVES 24
static struct { char path[160]; char *data; int len, append; } saves[SAVES];
static int nsaves;
static SceUID save_lock = -1, save_sema = -1;

static int save_worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(save_sema, 1, NULL);
        for (;;) {
            sceKernelWaitSema(save_lock, 1, NULL);
            if (!nsaves) { sceKernelSignalSema(save_lock, 1); break; }
            char path[160];
            memcpy(path, saves[0].path, sizeof(path));
            char *data = saves[0].data;
            int len = saves[0].len, append = saves[0].append;
            memmove(&saves[0], &saves[1], (--nsaves) * sizeof(saves[0]));
            sceKernelSignalSema(save_lock, 1);
            SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | (append ? SCE_O_APPEND : SCE_O_TRUNC), 0666);
            if (fd >= 0) { if (len) sceIoWrite(fd, data, len); sceIoClose(fd); }
            free(data);
        }
    }
    return 0;
}

void ui_save(const char *path, const void *data, int len, int append) {
    if (save_sema < 0) {
        save_lock = sceKernelCreateSema("save_lock", 0, 1, 1, NULL);
        save_sema = sceKernelCreateSema("save_wake", 0, 0, 1, NULL);
        SceUID t = sceKernelCreateThread("saver", save_worker, 0x10000100, 0x4000, 0, 0, NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
    char *copy = malloc(len > 0 ? len : 1);
    if (!copy) return;
    memcpy(copy, data, len);
    sceKernelWaitSema(save_lock, 1, NULL);
    int at = -1;
    if (!append)
        for (int i = 0; i < nsaves; ++i)
            if (!saves[i].append && !strcmp(saves[i].path, path)) { free(saves[i].data); at = i; break; }
    if (at < 0 && nsaves < SAVES) at = nsaves++;
    if (at >= 0) {
        snprintf(saves[at].path, sizeof(saves[at].path), "%s", path);
        saves[at].data = copy; saves[at].len = len; saves[at].append = append;
    } else free(copy);
    sceKernelSignalSema(save_lock, 1);
    sceKernelSignalSema(save_sema, 1);
}

/* Soft shapes made once in memory: an elliptical shadow and a round glow.
 * Drawn scaled and tinted, they replace hard-edged rectangles. */
static vita2d_texture *make_falloff(int w, int h, float power) {
    vita2d_texture *t = vita2d_create_empty_texture(w, h);
    if (!t) return NULL;
    unsigned int *px = vita2d_texture_get_datap(t), stride = vita2d_texture_get_stride(t) / 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float dx = (x + 0.5f) / w * 2 - 1, dy = (y + 0.5f) / h * 2 - 1, d = dx * dx + dy * dy;
            float a = d >= 1 ? 0 : 1 - d;
            float v = a;
            for (int k = 1; k < (int)power; ++k) v *= a;
            px[y * stride + x] = 0x00FFFFFFu | ((unsigned int)(v * 255) << 24);   /* white; tint to colour */
        }
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t;
}

vita2d_texture *ui_soft_shadow(void) { static vita2d_texture *t; if (!t) t = make_falloff(128, 32, 2); return t; }
vita2d_texture *ui_glow(void) { static vita2d_texture *t; if (!t) t = make_falloff(128, 128, 3); return t; }

void draw_soft(vita2d_texture *t, float cx, float cy, float w, float h, unsigned int color) {
    if (!t) return;
    vita2d_draw_texture_tint_scale(t, cx - w / 2, cy - h / 2, w / vita2d_texture_get_width(t), h / vita2d_texture_get_height(t), color);
}

/* PS5-style ambience behind the content: three big glows in the theme colour
 * drifting slowly, two faint ribbons of light rolling along the lower half,
 * and a few motes rising. All low alpha and slow; strength scales it (Home
 * lays it over art more gently than a plain tab). */
static unsigned int with_alpha(unsigned int c, float a) {
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (c & 0x00FFFFFFu) | ((unsigned int)a << 24);
}

static void ribbon(float t, float base, float amp, float k, float speed, float thick, unsigned int c, float alpha) {
    enum { N = 48 };
    vita2d_color_vertex *v = vita2d_pool_memalign(2 * (N + 1) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    for (int i = 0; i <= N; ++i) {
        float u = (float)i / N, x = u * W;
        float y = base + amp * sinf(u * k + t * speed) + amp * 0.4f * sinf(u * k * 2.3f - t * speed * 0.7f);
        float h = thick * (0.55f + 0.45f * sinf(u * 3.1f + t * speed * 0.5f));
        float edge = u < 0.15f ? u / 0.15f : u > 0.85f ? (1 - u) / 0.15f : 1;   /* fade in and out at the sides */
        unsigned int top = with_alpha(c, alpha * edge), bot = with_alpha(c, 0);
        v[2 * i] = (vita2d_color_vertex){x, y - h, 0.5f, bot};
        v[2 * i + 1] = (vita2d_color_vertex){x, y, 0.5f, top};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * (N + 1));
    for (int i = 0; i <= N; ++i) {                  /* and a softer fall below the bright edge */
        float y = v[2 * i + 1].y;
        v[2 * i].y = y;
        v[2 * i].color = v[2 * i + 1].color;
        v[2 * i + 1].y = y + thick * 1.6f;
        v[2 * i + 1].color = with_alpha(c, 0);
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * (N + 1));
}

/* The wallpaper, scaled to cover the screen (crops, never letterboxes). */
static void draw_wallpaper_cover(vita2d_texture *t) {
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    float s = (float)W / tw > (float)H / th ? (float)W / tw : (float)H / th;
    float dw = tw * s, dh = th * s;
    vita2d_draw_texture_tint_scale(t, (W - dw) / 2, (H - dh) / 2, s, s, RGBA8(255, 255, 255, 255));
}

void ui_ambient(float strength) {
    if (theme_bg == THEME_BG_PLAIN) return;                         /* calm: no glow, no ribbons, no motes */
    if (theme_bg == THEME_BG_MIDNIGHT) {
        vita2d_draw_rectangle(0, 0, W, H, RGBA8(0, 0, 0, 70));       /* darker, before the (dimmer) glow */
        strength *= 0.35f;
    }
    /* strength is 1.0 only where the caller has no backdrop art of its own
     * (Home and Play pass less, to lay the glow over their art instead): the
     * wallpaper stands in for the glow there, never under their own art. */
    if (theme_bg == THEME_BG_WALLPAPER && theme_wallpaper[0] && strength > 0.9f) {
        char path[100];
        snprintf(path, sizeof(path), "ux0:data/arcadehub/wallpapers/%s", theme_wallpaper);
        vita2d_texture *w = ui_image(path);
        if (w) {
            draw_wallpaper_cover(w);
            vita2d_draw_rectangle(0, 0, W, H, RGBA8(0, 0, 0, 110));  /* scrim: text stays readable */
            return;
        }
    }
    float t = frames / 60.0f;
    unsigned int acc = ui_accent, cool = RGBA8(90, 120, 255, 255), warm = RGBA8(200, 90, 255, 255);
    vita2d_texture *g = ui_glow();
    static const float B[3][6] = {            /* x, y, size, speed, phase, which colour */
        {0.20f, 0.30f, 620, 0.050f, 0.0f, 0}, {0.82f, 0.62f, 560, 0.037f, 2.1f, 1}, {0.55f, 1.02f, 700, 0.029f, 4.0f, 2}};
    for (int i = 0; i < 3; ++i) {
        float cx = W * (B[i][0] + 0.09f * sinf(t * B[i][3] * 6.28f + B[i][4])) - 26 * ui_tilt_x;
        float cy = H * (B[i][1] + 0.07f * cosf(t * B[i][3] * 5.0f + B[i][4])) + 18 * ui_tilt_y;
        float sz = B[i][2] * (0.92f + 0.08f * sinf(t * 0.21f + i));
        unsigned int c = B[i][5] == 0 ? acc : B[i][5] == 1 ? cool : warm;
        draw_soft(g, cx, cy, sz, sz * 0.72f, with_alpha(c, (i == 0 ? 46 : 30) * strength));
    }
    ribbon(t, H * 0.70f + 10 * ui_tilt_y, 26, 5.2f, 0.22f, 34, acc, 34 * strength);
    ribbon(t + 40, H * 0.78f + 14 * ui_tilt_y, 20, 3.7f, -0.16f, 26, cool, 24 * strength);
    enum { MOTES = 22 };
    for (int i = 0; i < MOTES; ++i) {                /* rise slowly, wrap, twinkle */
        unsigned int h = (unsigned int)i * 2654435761u;
        float sp = 6 + (h >> 8 & 15), x0 = (h >> 12 & 1023) / 1023.0f * W;
        float y = H + 20 - fmodf(t * sp + (h >> 3 & 511), H + 40);
        float x = x0 + 14 * sinf(t * 0.4f + i) - (6 + (h >> 20 & 3) * 4) * ui_tilt_x;   /* bigger motes are nearer */
        float tw = 0.5f + 0.5f * sinf(t * (0.8f + (h & 7) * 0.15f) + i * 1.7f);
        float r = 1.0f + (h >> 20 & 3) * 0.5f;
        vita2d_draw_fill_circle(x, y, r, with_alpha(RGBA8(220, 230, 255, 255), (40 + 90 * tw) * strength));
    }
}

/* Grid scrolling shared by Apps, Music and the store: the rows follow the
 * finger, keep going a little after a flick, and then the selection is pulled
 * into view; buttons move the selection and the rows ease after it. The old
 * versions eased toward the selection every frame, which fought the finger. */
void grid_scroll(GridScroll *g, int *sel, int cols, int count, int rows_visible, float cell_h, const Input *in) {
    int rows = (count + cols - 1) / cols;
    float max_top = rows > rows_visible ? rows - rows_visible : 0;
    if (in->touching && in->drag_dy) {
        g->top -= in->drag_dy / cell_h;
        g->vel = -in->drag_dy / cell_h;
        g->touch = 1;
    } else if (!in->touching && g->touch) {
        g->top += g->vel;
        g->vel *= 0.92f;
        if (g->vel < 0.004f && g->vel > -0.004f) g->touch = 0;
    }
    if (g->top < 0) { g->top = 0; g->vel = 0; }
    if (g->top > max_top) { g->top = max_top; g->vel = 0; }
    int row = *sel / cols;
    if (g->touch) {                                   /* the selection follows the view */
        int first = (int)(g->top + 0.5f), last = first + rows_visible - 1;
        if (row < first) *sel += (first - row) * cols;
        if (row > last) *sel -= (row - last) * cols;
        if (*sel >= count) *sel = count - 1;
        if (*sel < 0) *sel = 0;
    } else {                                          /* the view follows the selection */
        float target = row < g->top ? row : row > g->top + rows_visible - 1 ? row - (rows_visible - 1) : g->top;
        if (target > max_top) target = max_top;
        g->top += (target - g->top) * 0.3f;
    }
}

float ui_pulse(void) { return 0.5f + 0.5f * sinf(frames * 0.055f); }
unsigned int ui_frames(void) { return frames; }

/* The focus look shared by every grid: a soft accent glow that breathes
 * slowly, a crisp 2 px ring, and a shadow that makes the tile sit above the
 * page. 'lift' (0..1) is how far the focus animation has got. */
void draw_focus(float x, float y, float w, float h, float lift) { draw_focus_r(x, y, w, h, lift, ui_corner(w, h)); }

/* The ring for a tile with its own corner radius r (app icons are rounder). */
void draw_focus_r(float x, float y, float w, float h, float lift, float r) {
    float glow = lift * (0.75f + 0.25f * ui_pulse());
    draw_round_rect(x + 3, y + 8 * lift + 2, w, h, r, RGBA8(0, 0, 0, (int)(90 * lift)));      /* shadow */
    for (int k = 5; k >= 1; --k) {
        float pad = k * 2.6f;
        draw_round_rect(x - pad, y - pad, w + 2 * pad, h + 2 * pad, r + pad,
                        (C_ACCENT & 0x00FFFFFF) | ((unsigned int)(glow * (8 + (5 - k) * 4)) << 24));
    }
    draw_round_ring(x - 4, y - 4, w + 8, h + 8, r + 4, 2.5f, (C_ACCENT & 0x00FFFFFF) | ((unsigned int)(255 * lift) << 24));
}

/* Springy 0 -> 1 with a little overshoot, for tiles growing into focus. */
float ease_back(float t) {
    if (t >= 1) return 1;
    float c = 1.9f, u = t - 1;
    return 1 + (c + 1) * u * u * u + c * u * u;
}

void ui_end_frame(void) {
    if (!in_frame) return;
    vita2d_end_drawing();
    if (scene) {                                  /* show the finished frame */
        vita2d_start_drawing_advanced(NULL, 0);
        vita2d_draw_texture(scene, 0, 0);
        vita2d_end_drawing();
    }
    vita2d_swap_buffers();
    in_frame = 0;
    last_done = cur;
    cur ^= 1;
}

/* Shrink the last frame twice (960 -> 240 -> 96): each step averages, so
 * the small copy scaled back up is a soft, even blur. */
static void capture_backdrop(void) {
    if (last_done < 0 || !scenes[last_done] || !blur_rt || !blur_rt2) return;
    vita2d_texture *src = scenes[last_done];
    vita2d_texture_set_filters(src, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    /* vita2d maps its 960x544 space onto whatever target is bound, so each
     * pass draws at full-screen size and the target's size does the shrinking. */
    vita2d_start_drawing_advanced(blur_rt, 0);
    vita2d_draw_texture(src, 0, 0);
    vita2d_end_drawing();
    vita2d_texture_set_filters(blur_rt, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_start_drawing_advanced(blur_rt2, 0);
    vita2d_draw_texture_scale(blur_rt, 0, 0, (float)W / 240, (float)H / 136);
    vita2d_end_drawing();
    vita2d_wait_rendering_done();
}

void ui_blur_capture(void) { capture_backdrop(); }
void ui_blur_draw(void) {
    if (!blur_rt2) { vita2d_draw_rectangle(0, 0, W, H, C_BG); return; }
    vita2d_texture_set_filters(blur_rt2, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_draw_texture_scale(blur_rt2, 0, 0, (float)W / 96, (float)H / 54);
}

static void draw_backdrop(void) {
    if (!blur_rt2) { vita2d_draw_rectangle(0, 0, W, H, C_BG); return; }
    vita2d_texture_set_filters(blur_rt2, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_draw_texture_scale(blur_rt2, 0, 0, (float)W / 96, (float)H / 54);
    vita2d_draw_rectangle(0, 0, W, H, RGBA8(8, 10, 16, 150));       /* dimmed */
}

/* Dialogs bracket themselves with these. */
/* A dialog opened mid-frame: close the half-drawn frame without showing it,
 * and blur the last complete one for the dialog's backdrop. */
static int suspend_frame(void) {
    int was = in_frame;
    if (in_frame) { vita2d_end_drawing(); in_frame = 0; }
    capture_backdrop();
    return was;
}

static void resume_frame(int was) {
    if (was) ui_begin_frame();
}

void ui_init(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceMotionStartSampling();
    lang_init();                                     /* before anything is drawn */
    font = uifont_load("app0:assets/Inter-Regular.ttf");
    bold = uifont_load("app0:assets/Inter-Bold.ttf");
    uifont_set_fallback(uifont_load("app0:assets/NotoSansJP-Subset.otf"));   /* kana and common kanji */
    ui_theme_load();
}

#define REPEAT_DELAY_US 350000
#define REPEAT_EVERY_US 70000
#define DIRS (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)

void ui_read_input(Input *in) {
    static unsigned int last;
    static SceUInt64 held_since, last_repeat;
    static int was_touching, start_x, start_y, last_x, last_y, moved;
    /* Every sample the driver buffered since the last frame, not just the
     * newest: a quick press that began and ended while a frame was stuck
     * (a preview opening, a folder scan) used to vanish. */
    static SceCtrlData pads[64];
    static SceUInt64 last_ts;
    int n = sceCtrlPeekBufferPositive(0, pads, 64);
    /* A press counts when a button is down in any new sample but was up at the
     * end of the last frame. (Not sample-to-sample edges: the agent bridge's
     * injected presses show only in the newest sample, which made every frame
     * look like a fresh press.) */
    unsigned int b = last, seen = 0;
    int fresh = 0;
    for (int i = 0; i < n; ++i) {
        if (pads[i].timeStamp <= last_ts) continue;
        last_ts = pads[i].timeStamp;
        unsigned int s = pads[i].buttons;
        /* The left stick doubles as the D-pad for list movement. */
        if (pads[i].ly < 40) s |= SCE_CTRL_UP;
        if (pads[i].ly > 215) s |= SCE_CTRL_DOWN;
        if (pads[i].lx < 40) s |= SCE_CTRL_LEFT;
        if (pads[i].lx > 215) s |= SCE_CTRL_RIGHT;
        seen |= s;
        b = s;
        fresh = 1;
    }
    /* The PS button, for the app that holds the PS lock: the "2" read may
     * carry it where the plain one does not (bridge note is the fallback). */
    {
        SceCtrlData p2;
        if (sceCtrlPeekBufferPositive2(0, &p2, 1) > 0 && (p2.buttons & 0x10000)) {
            if (!(last & 0x10000)) fresh = 1;
            seen |= 0x10000;
            b |= 0x10000;
        }
    }
    unsigned int edges = fresh ? seen & ~last : 0;

    SceUInt64 now = sceKernelGetProcessTimeWide();
    in->held = b;
    in->pressed = edges;
    if ((b & DIRS) && (b & DIRS) == (last & DIRS)) {
        if (now - held_since > REPEAT_DELAY_US && now - last_repeat > REPEAT_EVERY_US) {
            in->pressed |= b & DIRS;
            last_repeat = now;
        }
    } else {
        held_since = last_repeat = now;
    }
    last = b;

    SceTouchData t;
    memset(&t, 0, sizeof(t));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
    in->tapped = 0;
    in->released = 0;
    in->drag_dx = in->drag_dy = 0;
    if (t.reportNum > 0) {
        int x = t.report[0].x / 2, y = t.report[0].y / 2;   /* panel units are 2x screen */
        if (!was_touching) { start_x = x; start_y = y; last_x = x; last_y = y; moved = 0; }
        if ((x - start_x) * (x - start_x) + (y - start_y) * (y - start_y) > 20 * 20) moved = 1;
        in->drag_dy = moved ? y - last_y : 0;
        in->drag_dx = moved ? x - last_x : 0;
        last_x = x;
        last_y = y;
        in->touching = 1; in->tx = x; in->ty = y;
        was_touching = 1;
    } else {
        /* The release frame carries no coordinates, so a tap uses where the
         * finger was last seen (the Arcade Hub lesson, 2026-09-21). */
        if (was_touching && !moved) { in->tapped = 1; in->tap_x = in->tx; in->tap_y = in->ty; }
        if (was_touching) in->released = 1;
        in->touching = 0;
        was_touching = 0;
    }

}

/* Every drawn string is translated here, and sized for Settings > Text size (lang.c). */
int text_w(UiFont *f, int size, const char *s) { return uifont_width(f, text_bump(size), tr(s)); }

void text(UiFont *f, int x, int y, unsigned int color, int size, const char *s) {
    uifont_draw(f, x, y, color, text_bump(size), tr(s));
}

void text_fit(UiFont *f, int x, int y, unsigned int color, int size, const char *s, int max_w) {
    s = tr(s);
    if (text_w(f, size, s) <= max_w) { text(f, x, y, color, size, s); return; }
    char buf[300];
    int n = (int)strlen(s);
    if (n > 280) n = 280;
    while (n > 0) {
        snprintf(buf, sizeof(buf), "%.*s...", n, s);
        if (text_w(f, size, buf) <= max_w) break;
        --n;
    }
    text(f, x, y, color, size, buf);
}

void text_right(UiFont *f, int right, int y, unsigned int color, int size, const char *s) {
    text(f, right - text_w(f, size, s), y, color, size, s);
}

void human_size(unsigned long long n, char *out, int max) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)n;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    if (i == 0) snprintf(out, max, "%llu B", n);
    else snprintf(out, max, v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

#define TAB_X0 36
#define TAB_GAP 26
#define TAB_SIZE 20

/* ---------- header status icons: Wi-Fi and Bluetooth, left of the clock ----
 * A short tap on either goes to our own Settings tab (handled in main.c
 * alongside the tab-bar taps); a long press jumps straight into the matching
 * system Settings page (main.c owns that timing too). thick_line is defined
 * further down, where the footer's button glyphs use it; forward-declared
 * here so the icons can share it instead of duplicating a line-drawer. */
static void thick_line(float x0, float y0, float x1, float y1, unsigned int c);

#define ICON_HIT_W 36    /* touch hit box: generous, the glyphs themselves are small */
#define ICON_HIT_H 40
#define ICON_GAP   30    /* Wi-Fi to Bluetooth, center to center */
#define ICON_PAD   18    /* Bluetooth to the clock text */

/* A ring segment `t` px thick, from angle a0 to a1 (radians, 0 = up,
 * clockwise) around (cx, cy). A fixed 8-segment fan is smooth enough at
 * icon size, so unlike draw_round_ring there's no need to size the buffer
 * to a variable segment count. */
static void arc_ring(float cx, float cy, float r, float t, float a0, float a1, unsigned int c) {
    enum { N = 8 };
    vita2d_color_vertex *v = vita2d_pool_memalign(2 * (N + 1) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    for (int k = 0; k <= N; ++k) {
        float a = a0 + (a1 - a0) * k / N, s = sinf(a), co = cosf(a);
        v[2 * k]     = (vita2d_color_vertex){cx + s * r,       cy - co * r,       0.5f, c};
        v[2 * k + 1] = (vita2d_color_vertex){cx + s * (r - t), cy - co * (r - t), 0.5f, c};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * (N + 1));
}

/* Three arcs fanning up from a dot: the familiar Wi-Fi glyph. Dim and
 * slashed through with no connection, full dim-white when connected. */
static void icon_wifi(float cx, float cy, int connected) {
    unsigned int c = connected ? C_DIM : C_FAINT;
    vita2d_draw_fill_circle(cx, cy + 6, 2, c);
    arc_ring(cx, cy + 6, 6,  2, -0.9f, 0.9f, c);
    arc_ring(cx, cy + 6, 10, 2, -0.9f, 0.9f, c);
    arc_ring(cx, cy + 6, 14, 2, -0.9f, 0.9f, c);
    if (!connected) thick_line(cx - 10, cy - 7, cx + 10, cy + 13, c);   /* struck through: no signal */
}

/* The Bluetooth bind-rune (Hagall + Berkanan): a spine plus two triangular
 * wings. Always dim-white; no pairing state is read for this icon. */
/* The Bluetooth mark as it is drawn everywhere: one stroke from lower left,
 * across to the right point, up the spine, round the top point and back out
 * to upper left, so the two diagonals cross the spine (asked for 2026-09-27:
 * "use the real Bluetooth icon"). */
static void icon_bt(float cx, float cy, unsigned int c) {
    float h = 8, w = 5;
    float px[6] = {cx - w, cx + w, cx, cx, cx + w, cx - w};
    float py[6] = {cy - h / 2, cy + h / 2, cy + h, cy - h, cy - h / 2, cy + h / 2};
    for (int i = 0; i < 5; ++i) thick_line(px[i], py[i], px[i + 1], py[i + 1], c);
}

/* sceNetCtlInetGetState is a round trip to the net stack; once a second is
 * plenty for a status icon and keeps it off the per-frame budget. */
static int wifi_connected(void) {
    static int connected, checked;
    static unsigned int last_frame;
    unsigned int f = ui_frames();
    if (!checked || f - last_frame >= 60) {
        checked = 1; last_frame = f;
        int st = 0;
        connected = sceNetCtlInetGetState(&st) >= 0 && st == SCE_NETCTL_STATE_CONNECTED;
    }
    return connected;
}

/* The clock/battery string and the x where it starts. Shared by draw_header
 * and header_icon_centers so a touch this frame (checked in main.c before
 * draw_header runs) lines up with what actually gets painted. */
static void header_clock_text(char *out, int max, int *right_x) {
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    int h = t.hour % 12 ? t.hour % 12 : 12;
    snprintf(out, max, "%d:%02d %s    %d%%%s", h, t.minute, t.hour < 12 ? "AM" : "PM",
             scePowerGetBatteryLifePercent(), scePowerIsBatteryCharging() ? " +" : "");
    if (right_x) *right_x = W - 30 - text_w(font, 18, out);
}

/* Centers for the two icons, in screen pixels. */
static void header_icon_centers(int *wifi_cx, int *bt_cx, int *cy) {
    char buf[64]; int right_x;
    header_clock_text(buf, sizeof(buf), &right_x);
    *bt_cx = right_x - ICON_PAD - ICON_HIT_W / 2;
    *wifi_cx = *bt_cx - ICON_GAP;
    *cy = 30;
}

/* Tab names that fit before the status icons in every language (2026-10-02:
 * Russian ran into the Wi-Fi icon and the clock): tighter gaps first, then a
 * smaller size. Shared by drawing and by the tap test. */
static void tab_fit(const char *const tabs[], int ntabs, int *gap, int *size) {
    int wifi_cx, bt_cx, cy;
    header_icon_centers(&wifi_cx, &bt_cx, &cy);
    int avail = wifi_cx - 28 - TAB_X0;
    *gap = TAB_GAP;
    for (*size = TAB_SIZE; *size >= 14; --*size) {
        int sum = 0;
        for (int i = 0; i < ntabs; ++i) sum += text_w(bold, *size, tabs[i]);
        if (ntabs < 2) return;
        int g = (avail - sum) / (ntabs - 1);
        if (g >= TAB_GAP) return;
        if (g >= 12) { *gap = g; return; }
    }
    *size = 14;
    *gap = 12;
}

void draw_header(const char *const tabs[], int ntabs, int active, const char *context) {
    vita2d_draw_rectangle(0, 0, W, 64, C_PANEL);
    vita2d_draw_rectangle(0, 64, W, 1, C_LINE);
    int x = TAB_X0, ax = 0, aw = 0, gap, size;
    tab_fit(tabs, ntabs, &gap, &size);
    for (int i = 0; i < ntabs; ++i) {
        int w = text_w(bold, size, tabs[i]);
        text(i == active ? bold : font, x, 41, i == active ? C_TEXT : C_DIM, size, tabs[i]);
        if (i == active) { ax = x; aw = w; }
        x += w + gap;
    }
    /* The underline glides to the new tab instead of jumping (critically
     * damped: fast, no overshoot), stretching a little while it travels. */
    static float ux = -1, uw;
    if (ux < 0) { ux = ax; uw = aw; }
    ux += (ax - ux) * 0.28f;
    uw += (aw - uw) * 0.28f;
    float travel = ax - ux < 0 ? ux - ax : ax - ux;
    float stretch = travel > 2 ? travel * 0.15f : 0;
    /* Each tab its own underline colour (2026-10-01: Movies was yellow from
     * its poster theme, every other tab the plain accent). By name, so a
     * hidden tab does not shift the others' colours. The colour glides too. */
    static const struct { const char *name; unsigned int rgb; } tint[] = {
        {"Home", 0x60A5FA}, {"Play", 0x34D399}, {"Movies", 0xFBBF24}, {"Music", 0xF472B6},
        {"Apps", 0xA78BFA}, {"Files", 0x22D3EE}, {"Store", 0xFB923C}, {"Settings", 0x94A3B8}};
    unsigned int want = 0x60A5FA;
    for (unsigned int k = 0; k < sizeof(tint) / sizeof(tint[0]); ++k)
        if (active >= 0 && active < ntabs && !strcmp(tabs[active], tint[k].name)) want = tint[k].rgb;
    static float cr = -1, cg, cb;
    float tr = (want >> 16) & 255, tg = (want >> 8) & 255, tb = want & 255;
    if (cr < 0) { cr = tr; cg = tg; cb = tb; }
    cr += (tr - cr) * 0.2f; cg += (tg - cg) * 0.2f; cb += (tb - cb) * 0.2f;
    vita2d_draw_rectangle(ux - stretch / 2, 58, uw + stretch, 3, RGBA8((int)cr, (int)cg, (int)cb, 255));
    char right[64]; int right_x;
    header_clock_text(right, sizeof(right), &right_x);
    text_right(font, W - 30, 41, C_DIM, 18, right);
    int wifi_cx, bt_cx, icon_cy;
    header_icon_centers(&wifi_cx, &bt_cx, &icon_cy);
    icon_wifi(wifi_cx, icon_cy, wifi_connected());
    icon_bt(bt_cx, icon_cy, C_DIM);
    if (context && *context) {
        int cw = text_w(font, 16, context), rx = wifi_cx - ICON_HIT_W / 2 - 10, room = rx - x;
        if (room >= 90) {
            if (cw > room) cw = room;
            text_fit(font, rx - cw, 40, C_FAINT, 16, context, cw);
        }
    }
}

int header_tab_at(int x, int y, const char *const tabs[], int ntabs) {
    if (y > 64) return -1;
    int left = TAB_X0, gap, size;
    tab_fit(tabs, ntabs, &gap, &size);
    for (int i = 0; i < ntabs; ++i) {
        int w = text_w(bold, size, tabs[i]);
        if (x >= left - gap / 2 && x <= left + w + gap / 2) return i;
        left += w + gap;
    }
    return -1;
}

/* Which header status icon (0 = Wi-Fi, 1 = Bluetooth) a point landed in, or
 * -1. The hit box is generous (36x40): the glyphs themselves are small. */
int header_icon_at(int x, int y) {
    int wifi_cx, bt_cx, cy;
    header_icon_centers(&wifi_cx, &bt_cx, &cy);
    if (x >= wifi_cx - ICON_HIT_W / 2 && x <= wifi_cx + ICON_HIT_W / 2 &&
        y >= cy - ICON_HIT_H / 2 && y <= cy + ICON_HIT_H / 2) return 0;
    if (x >= bt_cx - ICON_HIT_W / 2 && x <= bt_cx + ICON_HIT_W / 2 &&
        y >= cy - ICON_HIT_H / 2 && y <= cy + ICON_HIT_H / 2) return 1;
    return -1;
}

/* ---------- the footer: hints with the console's own buttons ----------
 * Hints stay plain strings ("X play   O back   L R tabs"); items are split
 * on runs of spaces, and a leading button name is drawn as the button. */
enum { B_NONE, B_CROSS, B_CIRCLE, B_TRIANGLE, B_SQUARE, B_DPAD_H, B_DPAD_V, B_DPAD, B_START, B_SELECT, B_LR, B_L, B_R, B_PS };
static const struct { const char *name; int b; } BUTTON_NAMES[] = {
    {"\xE2\x86\x90 \xE2\x86\x92", B_DPAD_H}, {"<- ->", B_DPAD_H}, {"\xE2\x86\x91\xE2\x86\x93", B_DPAD_V},
    {"UP DOWN", B_DPAD_V}, {"UP", B_DPAD_V}, {"DOWN", B_DPAD_V}, {"L R", B_LR}, {"START", B_START},
    {"SELECT", B_SELECT}, {"\xC3\x97", B_CROSS}, {"\xE2\x97\x8B", B_CIRCLE}, {"\xE2\x96\xB3", B_TRIANGLE},
    {"\xE2\x96\xA1", B_SQUARE}, {"/\\", B_TRIANGLE}, {"[]", B_SQUARE}, {"PS", B_PS}, {"X", B_CROSS},
    {"O", B_CIRCLE}, {"L", B_L}, {"R", B_R},
};

static void thick_line(float x0, float y0, float x1, float y1, unsigned int c) {
    for (float o = -0.9f; o <= 0.9f; o += 0.6f) {
        vita2d_draw_line(x0 + o, y0, x1 + o, y1, c);
        vita2d_draw_line(x0, y0 + o, x1, y1 + o, c);
    }
}

/* A small filled arrowhead with its tip at (tx, ty), pointing (dx, dy). */
static void arrow(float tx, float ty, float dx, float dy, unsigned int color) {
    vita2d_color_vertex *v = vita2d_pool_memalign(3 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    float bx = tx - dx * 4.5f, by = ty - dy * 4.5f, px = -dy * 4.2f, py = dx * 4.2f;
    v[0] = (vita2d_color_vertex){tx, ty, 0.5f, color};
    v[1] = (vita2d_color_vertex){bx + px, by + py, 0.5f, color};
    v[2] = (vita2d_color_vertex){bx - px, by - py, 0.5f, color};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
}

static int pill(int x, int cy, const char *label) {
    int tw = text_w(bold, 11, label), w = tw + 16;
    unsigned int bg = RGBA8(58, 64, 82, 255);
    vita2d_draw_fill_circle(x + 9, cy, 9, bg);
    vita2d_draw_fill_circle(x + w - 9, cy, 9, bg);
    vita2d_draw_rectangle(x + 9, cy - 9, w - 18, 18, bg);
    text(bold, x + 8, cy + 4, C_TEXT, 11, label);
    return w;
}

/* Draws the button at x (vertically centred on cy); returns its width. */
static int draw_button(int b, int x, int cy) {
    unsigned int face = RGBA8(52, 58, 74, 255);
    float c = x + 11;
    switch (b) {
    case B_CROSS:
        vita2d_draw_fill_circle(c, cy, 11, face);
        thick_line(c - 5, cy - 5, c + 5, cy + 5, RGBA8(124, 178, 250, 255));
        thick_line(c - 5, cy + 5, c + 5, cy - 5, RGBA8(124, 178, 250, 255));
        return 22;
    case B_CIRCLE:
        vita2d_draw_fill_circle(c, cy, 11, face);
        vita2d_draw_fill_circle(c, cy, 6.5f, RGBA8(255, 104, 104, 255));
        vita2d_draw_fill_circle(c, cy, 4.3f, face);
        return 22;
    case B_TRIANGLE: {
        vita2d_draw_fill_circle(c, cy, 11, face);
        unsigned int g = RGBA8(64, 226, 160, 255);
        thick_line(c, cy - 6, c - 6, cy + 4.5f, g);
        thick_line(c - 6, cy + 4.5f, c + 6, cy + 4.5f, g);
        thick_line(c + 6, cy + 4.5f, c, cy - 6, g);
        return 22;
    }
    case B_SQUARE: {
        vita2d_draw_fill_circle(c, cy, 11, face);
        unsigned int pk = RGBA8(255, 112, 230, 255);
        vita2d_draw_rectangle(c - 5, cy - 5, 10, 2, pk);
        vita2d_draw_rectangle(c - 5, cy + 3, 10, 2, pk);
        vita2d_draw_rectangle(c - 5, cy - 5, 2, 10, pk);
        vita2d_draw_rectangle(c + 3, cy - 5, 2, 10, pk);
        return 22;
    }
    case B_DPAD_H: case B_DPAD_V: case B_DPAD: {
        /* A round button like the others, with four arrow tips; the ones this
         * hint is about are lit. */
        vita2d_draw_fill_circle(c, cy, 11, face);
        unsigned int on = RGBA8(236, 239, 244, 255), off = RGBA8(96, 104, 126, 255);
        int h = b != B_DPAD_V, v = b != B_DPAD_H;
        arrow(c, cy - 8.5f, 0, -1, v ? on : off);
        arrow(c, cy + 8.5f, 0, 1, v ? on : off);
        arrow(c - 8.5f, cy, -1, 0, h ? on : off);
        arrow(c + 8.5f, cy, 1, 0, h ? on : off);
        return 22;
    }
    case B_LR: { int w = pill(x, cy, "L"); return w + 4 + pill(x + w + 4, cy, "R"); }
    case B_L: return pill(x, cy, "L");
    case B_R: return pill(x, cy, "R");
    case B_START: return pill(x, cy, "START");
    case B_SELECT: return pill(x, cy, "SELECT");
    case B_PS: return pill(x, cy, "PS");
    }
    return 0;
}

void draw_footer(const char *hint) { draw_footer_r(hint, 0); }

/* The footer with `reserve` pixels kept free at its right end (left of the
 * agent pill); returns where that space starts. */
int draw_footer_r(const char *hint, int reserve) {
    vita2d_draw_rectangle(0, H - 40, W, 40, C_PANEL);
    vita2d_draw_rectangle(0, H - 40, W, 1, C_LINE);
    int right_edge = W - 20;
    if (ui_agent_active) {                          /* a steady status while an agent drives the Vita */
        const char *msg = "Agent working";
        int tw = text_w(bold, 13, msg), pw = tw + 32, px = W - 16 - pw;
        draw_round_rect(px, H - 32, pw, 24, 12, RGBA8(251, 191, 36, 40));
        vita2d_draw_fill_circle(px + 13, H - 20, 3.5f + 1.5f * ui_pulse(), RGBA8(251, 191, 36, 255));
        text(bold, px + 23, H - 15, RGBA8(251, 191, 36, 255), 13, msg);
        right_edge = px - 12;
    }
    right_edge -= reserve;
    draw_hints(26, H - 20, hint, C_DIM, right_edge - 40);
    return right_edge;
}

/* A hint string drawn with the console's buttons, from x, centred on cy.
 * Returns the width drawn. Used by the footer, dialogs and full-screen views. */
static int button_width(int b) {
    switch (b) {
    case B_NONE: return 0;
    case B_LR: return text_w(bold, 11, "L") + 16 + 4 + text_w(bold, 11, "R") + 16;
    case B_L: return text_w(bold, 11, "L") + 16;
    case B_R: return text_w(bold, 11, "R") + 16;
    case B_START: return text_w(bold, 11, "START") + 16;
    case B_SELECT: return text_w(bold, 11, "SELECT") + 16;
    case B_PS: return text_w(bold, 11, "PS") + 16;
    default: return 22;
    }
}

static int hints_measuring;

/* How wide draw_hints would draw this hint. */
int hints_width(const char *hint) {
    hints_measuring = 1;
    int w = draw_hints(0, 0, hint, 0, 100000);
    hints_measuring = 0;
    return w;
}

int draw_hints(int x0, int cy, const char *hint, unsigned int color, int max_x) {
    int x = x0;
    const char *p = hint;
    while (p && *p && x < max_x) {
        while (*p == ' ') ++p;
        const char *end = p;                          /* an item runs to the next double space */
        while (*end && !(end[0] == ' ' && end[1] == ' ')) ++end;
        char item[128];
        snprintf(item, sizeof(item), "%.*s", (int)(end - p) < 127 ? (int)(end - p) : 127, p);
        p = end;
        const char *label = item;
        int b = B_NONE;
        for (unsigned int k = 0; k < sizeof(BUTTON_NAMES) / sizeof(BUTTON_NAMES[0]); ++k) {
            int n = strlen(BUTTON_NAMES[k].name);
            if (!strncmp(item, BUTTON_NAMES[k].name, n) && (item[n] == ' ' || item[n] == 0)) {
                b = BUTTON_NAMES[k].b;
                label = item + n + (item[n] == ' ');
                break;
            }
        }
        if (b != B_NONE) x += (hints_measuring ? button_width(b) : draw_button(b, x, cy)) + 7;
        if (*label) {
            if (!hints_measuring) text(font, x, cy + 6, color, 16, label);
            x += text_w(font, 16, label);
        }
        x += 22;
    }
    return x - x0 - 22;
}

/* A solid action button ("X Play"): accent-filled when focused. */
void draw_action_button(float x, float y, float w, float h, const char *hint, int on, unsigned int fill) {
    draw_round_rect(x, y, w, h, h / 2, on ? fill : RGBA8(255, 255, 255, 22));
    int tw = hints_width(hint);
    draw_hints((int)(x + (w - tw) / 2), (int)(y + h / 2), hint, on ? RGBA8(10, 12, 18, 255) : C_TEXT, (int)(x + w));
}

/* hint centred on cx */
void draw_hints_centered(int cx, int cy, const char *hint, unsigned int color) {
    draw_hints(cx - hints_width(hint) / 2, cy, hint, color, W);
}

/* A rounded button with a console glyph ("X Yes"): tapped when the finger
 * lands inside. Returns 1 when tapped. */
static int glyph_button(int x, int y, int w, const char *hint, int primary, const Input *in) {
    draw_round_rect(x, y, w, 44, 22, primary ? RGBA8(255, 255, 255, 30) : RGBA8(255, 255, 255, 14));
    if (primary) draw_round_ring(x, y, w, 44, 22, 1.5f, (C_ACCENT & 0x00FFFFFF) | 0xB0000000);
    int tw = hints_width(hint);
    draw_hints(x + (w - tw) / 2, y + 22, hint, primary ? C_TEXT : C_DIM, x + w);
    return in && in->tapped && in->tap_x >= x && in->tap_x < x + w && in->tap_y >= y && in->tap_y < y + 44;
}

void draw_bar(int x, int y, int w, int h, float frac, unsigned int color) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    vita2d_draw_rectangle(x, y, w, h, C_LINE);
    vita2d_draw_rectangle(x, y, w * frac, h, color);
}

/* ---------- modal dialogs ---------- */

/* A dialog frame: dim what is behind, draw a card. The caller's last frame is
 * gone after the first swap, so dialogs draw on a plain dimmed background. */
static void card(int w, int h, const char *title, int *x, int *y) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    draw_backdrop();
    *x = (W - w) / 2;
    *y = (H - h) / 2;
    draw_soft(ui_soft_shadow(), *x + w / 2, *y + h + 6, w * 1.05f, 60, RGBA8(0, 0, 0, 150));   /* the card casts a shadow */
    draw_round_rect(*x, *y, w, h, 22, C_PANEL);
    draw_round_ring(*x, *y, w, h, 22, 1, RGBA8(255, 255, 255, 22));
    text(bold, *x + 28, *y + 46, C_TEXT, 24, title);
}

static void end_frame(void) {
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* Word-wraps body inside the card. */
static void body_text(int x, int y, int w, const char *body) {
    char line[200];
    int len = 0, ly = y;
    const char *p = body;
    while (*p) {
        const char *word = p;
        while (*p && *p != ' ' && *p != '\n') ++p;
        int wl = (int)(p - word);
        char trial[200];
        snprintf(trial, sizeof(trial), "%.*s%s%.*s", len, line, len ? " " : "", wl, word);
        if (len && text_w(font, 19, trial) > w) {
            line[len] = 0;
            text(font, x, ly, C_DIM, 19, line);
            ly += 28;
            len = snprintf(line, sizeof(line), "%.*s", wl, word);
        } else {
            len = snprintf(line, sizeof(line), "%s", trial);
        }
        if (*p == '\n') {
            line[len] = 0;
            text(font, x, ly, C_DIM, 19, line);
            ly += 28;
            len = 0;
        }
        if (*p) ++p;
    }
    if (len) { line[len] = 0; text(font, x, ly, C_DIM, 19, line); }
}

static int confirm_impl(const char *title, const char *body) {
    Input in;
    memset(&in, 0, sizeof(in));
    for (int f = 0;; ++f) {
        ui_read_input(&in);
        if (f > 2 && (in.pressed & SCE_CTRL_CROSS)) return 1;
        if (f > 2 && (in.pressed & SCE_CTRL_CIRCLE)) return 0;
        int x, y;
        card(600, 270, title, &x, &y);
        body_text(x + 28, y + 90, 544, body);
        int yes = glyph_button(x + 600 - 28 - 150, y + 200, 150, "X Yes", 1, &in);
        int no = glyph_button(x + 600 - 28 - 150 - 12 - 170, y + 200, 170, "O Cancel", 0, &in);
        end_frame();
        if (f > 2 && yes) return 1;
        if (f > 2 && no) return 0;
    }
}

static void message_impl(const char *title, const char *body) {
    Input in;
    memset(&in, 0, sizeof(in));
    for (int f = 0;; ++f) {
        ui_read_input(&in);
        if (f > 2 && (in.pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))) return;
        if (f > 2 && in.tapped) return;
        int x, y;
        card(640, 300, title, &x, &y);
        body_text(x + 28, y + 90, 584, body);
        glyph_button(x + 640 - 28 - 130, y + 232, 130, "X OK", 1, NULL);
        end_frame();
    }
}

static int menu_impl(const char *title, const char *const items[], int n) {
    Input in;
    memset(&in, 0, sizeof(in));
    int sel = 0, h = 140 + n * 44;
    for (int f = 0;; ++f) {
        ui_read_input(&in);
        if (in.pressed & SCE_CTRL_UP) sel = (sel + n - 1) % n;
        if (in.pressed & SCE_CTRL_DOWN) sel = (sel + 1) % n;
        if (f > 2 && (in.pressed & SCE_CTRL_CROSS)) return sel;
        if (f > 2 && (in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE))) return -1;
        int x, y;
        card(420, h, title, &x, &y);
        for (int i = 0; i < n; ++i) {
            int ry = y + 70 + i * 44;
            if (i == sel) draw_round_rect(x + 12, ry, 396, 40, 14, C_SEL);
            text(font, x + 28, ry + 28, C_TEXT, 20, items[i]);
            if (in.tapped && in.tap_x > x && in.tap_x < x + 420 && in.tap_y > ry && in.tap_y < ry + 40) {
                end_frame();
                return i;
            }
        }
        draw_hints(x + 28, y + h - 28, "X select  O back", C_DIM, x + 400);
        if (in.tapped && (in.tap_x < x || in.tap_x > x + 420 || in.tap_y < y || in.tap_y > y + h)) {
            end_frame();
            return -1;
        }
        end_frame();
    }
}

static void to_utf16(const char *in, SceWChar16 *out, int max) {
    int i = 0;
    for (; in[i] && i < max - 1; ++i) out[i] = (unsigned char)in[i];
    out[i] = 0;
}

static void from_utf16(const SceWChar16 *in, char *out, int max) {
    int i = 0;
    for (; in[i] && i < max - 1; ++i) out[i] = in[i] < 128 ? (char)in[i] : '_';
    out[i] = 0;
}

static int ask_impl(const char *title, char *text_io, int max) {
    static SceWChar16 title16[80], initial16[520], buffer16[520];
    to_utf16(title, title16, 80);
    to_utf16(text_io, initial16, 520);
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_TRUE;
    p.type = SCE_IME_TYPE_URL;                     /* no auto-capitals, has / and . keys */
    p.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = title16;
    p.maxTextLength = max - 1 < 511 ? max - 1 : 511;
    p.initialText = initial16;
    p.inputTextBuffer = buffer16;
    if (sceImeDialogInit(&p) < 0) return 0;
    for (int frames = 0;; ++frames) {
        SceCommonDialogStatus st = sceImeDialogGetStatus();
        if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) break;
        if ((st == SCE_COMMON_DIALOG_STATUS_NONE && frames > 120) || frames > 60 * 300) {
            sceImeDialogTerm();
            return 0;
        }
        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_backdrop();
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
    }
    SceImeDialogResult r;
    memset(&r, 0, sizeof(r));
    sceImeDialogGetResult(&r);
    sceImeDialogTerm();
    if (r.button != SCE_IME_DIALOG_BUTTON_ENTER) return 0;
    from_utf16(buffer16, text_io, max);
    return 1;
}

int ui_confirm(const char *title, const char *body) {
    int was = suspend_frame();
    int r = confirm_impl(title, body);
    resume_frame(was);
    return r;
}

void ui_message(const char *title, const char *body) {
    int was = suspend_frame();
    message_impl(title, body);
    resume_frame(was);
}

int ui_menu(const char *title, const char *const items[], int n) {
    int was = suspend_frame();
    int r = menu_impl(title, items, n);
    resume_frame(was);
    return r;
}

int ui_ask_text(const char *title, char *text_io, int max) {
    int was = suspend_frame();
    int r = ask_impl(title, text_io, max);
    resume_frame(was);
    return r;
}
