/* Movies: the films in ux0:video, a live muted preview of the one you are on,
 * and a full-screen player that remembers where you stopped. The files are
 * 960x544 H.264, which the hardware decoder plays directly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <math.h>

#include "movies.h"
#include "settings.h"
#include "search.h"
#include "video.h"

#define MAX_MOVIES 128
#define RESUME_FILE "ux0:data/arcadehub/user/movies.tsv"
#define PREVIEW_AT_MS (10 * 60 * 1000)     /* ten minutes in: past the logos */
#define POSTERS "ux0:data/arcadehub/posters/"   /* home/tools/movie_posters.py */

typedef struct { char path[256], title[96]; unsigned int resume_ms; vita2d_texture *poster; int tried; } Movie;

static Movie movies[MAX_MOVIES];
static int nmovies, sel, full, settle, controls;
static volatile int scanned;              /* set by movies_prewarm (a background thread) when done */
static float pos;                 /* the cover flow, easing toward sel */
static char previewing[256];

/* ---------- library and resume points ---------- */

static void add_dir(const char *dir, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    while (nmovies < MAX_MOVIES) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) { if (depth < 3) add_dir(path, depth + 1); continue; }
        const char *dot = strrchr(e.d_name, '.');
        if (!dot || (strcasecmp(dot, ".mp4") && strcasecmp(dot, ".m4v"))) continue;
        Movie *m = &movies[nmovies++];
        snprintf(m->path, sizeof(m->path), "%s", path);
        snprintf(m->title, sizeof(m->title), "%.*s", (int)(dot - e.d_name), e.d_name);
        m->resume_ms = 0;
    }
    sceIoDclose(d);
}

static int by_title(const void *a, const void *b) { return strcasecmp(((const Movie *)a)->title, ((const Movie *)b)->title); }

static void load_resume(void) {
    SceUID fd = sceIoOpen(RESUME_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    static char buf[8192];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        for (int i = 0; i < nmovies; ++i)
            if (!strcmp(movies[i].title, line)) movies[i].resume_ms = (unsigned int)strtoul(tab + 1, NULL, 10);
    }
}

static void save_resume(void) {
    static char buf[MAX_MOVIES * 128];
    int n = 0;
    for (int i = 0; i < nmovies; ++i)
        if (movies[i].resume_ms) n += snprintf(buf + n, sizeof(buf) - n, "%s\t%u\n", movies[i].title, movies[i].resume_ms);
    ui_save(RESUME_FILE, buf, n, 0);
}

static void scan(void) {
    STAGE("movies: scan");
    nmovies = 0;
    /* Where people keep films (Reddit, 2026-09-29: "recognizing none of my
     * movie files"): the system's video folder on every card mount, and the
     * obvious names. Folders that do not exist cost one failed open. */
    static const char *const roots[] = {"ux0:video", "uma0:video", "imc0:video", "xmc0:video", "grw0:video",
                                        "ux0:Videos", "ux0:Movies", "ux0:movies", "uma0:Movies", "uma0:movies",
                                        "ux0:data/video", "ux0:data/movies", "ux0:data/Movies"};
    for (unsigned int r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) add_dir(roots[r], 0);
    qsort(movies, nmovies, sizeof(Movie), by_title);
    load_resume();
    scanned = 1;
}

static void clock_str(unsigned int ms, char *out, int max) {
    unsigned int s = ms / 1000;
    if (s >= 3600) snprintf(out, max, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, max, "%u:%02u", s / 60, s % 60);
}

/* ---------- full screen ---------- */

/* A file the Vita cannot decode opens and ends at once, and the player used
 * to close without a word (Reddit and a DM, 2026-09-29: "videos aren't
 * playing even though they're mp4"). Ended inside 4 s without getting past
 * half a second: say why. */
static unsigned int open_frames, furthest_ms;

static void open_player(void) {
    Movie *m = &movies[sel];
    previewing[0] = 0;
    /* Back up a few seconds so you know where you are. */
    unsigned int from = m->resume_ms > 5000 ? m->resume_ms - 5000 : 0;
    if (video_play_opts(m->path, 1, 0, from, OWN_MOVIE) < 0) {
        ui_message("Cannot play", "The player would not open this file.");
        return;
    }
    full = 1;
    controls = 180;
    open_frames = 0;
    furthest_ms = 0;
}

static void close_player(int finished) {
    Movie *m = &movies[sel];
    unsigned int pos = video_pos_ms(), dur = video_duration_ms();
    /* Near the end counts as watched: start over next time. Backing out before
     * the file had even opened (no duration yet) keeps the old spot. */
    if (finished || (dur && pos + 3 * 60 * 1000 > dur)) m->resume_ms = 0;
    else if (dur && pos) m->resume_ms = pos;
    save_resume();
    video_stop_owned(OWN_MOVIE);
    full = 0;
    settle = 0;
}

static void player(const Input *in) {
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);       /* keep the screen on */
    if (video_owner() != OWN_MOVIE) {                         /* ended (or never opened) */
        int never = open_frames < 240 && furthest_ms < 500;
        close_player(video_duration_ms() > 0 && !never);
        if (never)
            ui_message("This film will not play",
                       "The Vita plays MP4 files with H.264 video (up to 960x544 is safest) and AAC audio. "
                       "Other formats, such as H.265/HEVC or 1080p, need converting first, for example with "
                       "HandBrake's H.264 presets.");
        return;
    }
    ++open_frames;
    unsigned int p = video_pos_ms();
    if (p > furthest_ms) furthest_ms = p;
    if (in->pressed || in->tapped) controls = 180;
    if (in->pressed & SCE_CTRL_CROSS) video_pause(!video_paused());
    unsigned int pos = video_pos_ms();
    if (in->pressed & SCE_CTRL_RIGHT) video_seek(pos + 10000);
    if (in->pressed & SCE_CTRL_LEFT) video_seek(pos > 10000 ? pos - 10000 : 1);
    if (in->pressed & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) video_seek(pos + 60000);
    if (in->pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) video_seek(pos > 60000 ? pos - 60000 : 1);
    if (in->pressed & SCE_CTRL_CIRCLE) { close_player(0); return; }

    /* Plex-style: drag up or down on the left half for brightness, on the
     * right half for volume; a level pill shows while it changes. */
    static int gesture, side, shown;                  /* gesture: 0 none, 1 undecided, 2 level */
    static float level, moved;
    if (in->touching) {
        if (!gesture) { gesture = 1; side = in->tx >= W / 2; moved = 0; level = side ? settings_volume() : settings_brightness(); }
        moved += in->drag_dy;
        if (gesture == 1 && (moved > 18 || moved < -18)) gesture = 2;
        if (gesture == 2 && in->drag_dy) {
            level -= in->drag_dy / 260.0f;
            level = level < 0 ? 0 : level > 1 ? 1 : level;
            if (side) settings_set_volume(level, 0); else settings_set_brightness(level, 0);
            shown = 90;
        }
    } else if (gesture) {
        if (gesture == 2) { if (side) settings_set_volume(level, 1); else settings_set_brightness(level, 1); }
        gesture = 0;
    }

    vita2d_draw_rectangle(0, 0, W, H, RGBA8(0, 0, 0, 255));
    vita2d_texture *f = video_frame();
    if (f) {
        float tw = vita2d_texture_get_width(f), th = vita2d_texture_get_height(f);
        float sc = W / tw < H / th ? W / tw : H / th;
        vita2d_draw_texture_scale(f, (W - tw * sc) / 2, (H - th * sc) / 2, sc, sc);
    }
    if (controls > 0 || video_paused()) {
        if (controls > 0) --controls;
        unsigned int dur = video_duration_ms();
        char a[16], b[16], line[64];
        clock_str(pos, a, sizeof(a));
        clock_str(dur, b, sizeof(b));
        snprintf(line, sizeof(line), "%s  /  %s", a, b);
        vita2d_draw_rectangle(0, H - 96, W, 96, RGBA8(0, 0, 0, 170));
        text(bold, 30, H - 62, C_TEXT, 20, movies[sel].title);
        text_right(font, W - 30, H - 62, C_DIM, 17, line);
        draw_bar(30, H - 48, W - 60, 5, dur ? (float)pos / dur : 0, C_ACCENT);
        draw_hints(30, H - 20, video_paused() ? "X play  <- -> 10 s  L R 1 min  O back" : "X pause  <- -> 10 s  L R 1 min  O back",
                   C_DIM, W - 200);
        if (video_paused()) text(bold, W - 30 - text_w(bold, 15, "PAUSED"), H - 14, C_ACCENT, 15, "PAUSED");
    }
    if (shown > 0) {                                  /* the level pill, on the side being dragged */
        shown--;
        float a = shown > 20 ? 1 : shown / 20.0f, px = side ? W - 70 : 40, py = 130, ph = 240;
        draw_round_rect(px, py, 30, ph, 15, RGBA8(0, 0, 0, (int)(150 * a)));
        draw_round_rect(px + 5, py + 5 + (ph - 10) * (1 - level), 20, (ph - 10) * level, 10, (C_ACCENT & 0x00FFFFFF) | ((unsigned int)(255 * a) << 24));
        const char *what = side ? "Volume" : "Brightness";
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)(level * 100 + 0.5f));
        int tx = side ? (int)px - 12 - text_w(bold, 16, what) : (int)px + 44;
        text(bold, tx, (int)(py + ph / 2), RGBA8(255, 255, 255, (int)(255 * a)), 16, what);
        text(font, tx, (int)(py + ph / 2 + 22), RGBA8(255, 255, 255, (int)(200 * a)), 15, pct);
    }
}

/* ---------- cover flow ---------- */

static vita2d_texture *poster_of(Movie *m) {
    char path[256];
    snprintf(path, sizeof(path), POSTERS "%s.png", m->title);
    return m->poster = ui_image(path);             /* decoded off the main thread */
}

static void posters_near(void) {
    for (int i = 0; i < nmovies; ++i)
        movies[i].poster = abs(i - sel) <= 5 ? poster_of(&movies[i]) : NULL;
}

#define PW 250.0f          /* the centre poster: as big as the page allows (playtest 2026-09-26) */
#define PH 375.0f
#define HORIZON 262.0f     /* posters are centred on this line */
#define STRIPS 24

/* One poster as a trapezoid: drawn in thin vertical strips, each its own
 * height, so a turned poster recedes toward its far edge like a real card.
 * h_left/h_right are the heights of its two edges; the reflection hangs
 * below the bottom edge, flipped and faded. */
static void draw_card(const Movie *m, float x, float w, float h_left, float h_right, unsigned int shade, int reflect) {
    (void)h_right;                                   /* posters face forward now: one height */
    float h = h_left, top = HORIZON - h / 2, bottom = HORIZON + h / 2;
    vita2d_texture *t = m->poster;
    if (!t) {
        vita2d_draw_rectangle(x, top, w, h, RGBA8(34, 40, 56, 255));
        int fs = (int)(h * 0.075f);
        text_fit(bold, (int)(x + w * 0.1f), (int)(HORIZON + fs / 3), C_TEXT, fs, m->title, (int)(w * 0.8f));
        return;
    }
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t), xs = w / tw, ys = h / th;
    draw_soft(ui_soft_shadow(), x + w / 2, bottom + 4, w * 1.15f, 26, RGBA8(0, 0, 0, 150));   /* a soft contact shadow */
    vita2d_draw_texture_tint_scale(t, x, top, xs, ys, shade);
    if (!reflect) return;
    /* The reflection: the bottom third, flipped, in bands that fade to nothing,
     * like a poster standing on a glossy floor. */
    const int bands = 20;
    float rh = th * 0.22f, bh = rh / bands;
    unsigned int lum = shade & 0xFF;
    for (int k = 0; k < bands; ++k) {
        float f = 1.0f - (float)k / bands;
        unsigned int a = (unsigned int)(38 * f * f * f);
        if (!a) break;
        float src = th - (k + 1) * bh;
        vita2d_draw_texture_tint_part_scale(t, x, bottom + 3 + (k + 1) * bh * ys, 0, src, tw, bh, xs, -ys,
                                            RGBA8(lum, lum, lum, a));
    }
}

/* Posters always face you (playtest, 2026-09-24): the ones to the sides are
 * smaller, dimmer and tucked behind their neighbours. */
static void cover_flow(void) {
    int order[MAX_MOVIES], n = 0;
    for (int i = 0; i < nmovies; ++i) if (fabsf(i - pos) < 5.5f) order[n++] = i;
    for (int a = 0; a < n; ++a)                                     /* far ones first */
        for (int b = a + 1; b < n; ++b)
            if (fabsf(order[b] - pos) > fabsf(order[a] - pos)) { int t = order[a]; order[a] = order[b]; order[b] = t; }
    for (int j = 0; j < n; ++j) {
        int i = order[j];
        float d = i - pos, ad = fabsf(d), near = ad < 1 ? ad : 1;
        float off = ad < 1 ? d * 255 : (d > 0 ? 1 : -1) * (255 + (ad - 1) * 150);
        float scale = 1.0f - 0.24f * near - (ad > 1 ? 0.07f * (ad - 1) : 0);
        if (scale < 0.45f) scale = 0.45f;
        float w = PW * scale, h = PH * scale, x = W / 2 + off - w / 2;
        int lum = (int)(255 - 95 * near - (ad > 1 ? 25 * (ad - 1) : 0));
        if (lum < 70) lum = 70;
        draw_card(&movies[i], x, w, h, h, RGBA8(lum, lum, lum, 255), 1);
    }
}

/* ---------- browsing ---------- */

int movies_fullscreen(void) { return full; }

/* For the Home tab: the film you are part-way through, if any. */
void movies_prewarm(void) { scan(); }

int movies_resume_item(const char **title, vita2d_texture **poster, unsigned int *at_ms) {
    if (!scanned) return 0;
    for (int i = 0; i < nmovies; ++i) {
        Movie *m = &movies[i];
        if (!m->resume_ms) continue;
        poster_of(m);
        *title = m->title;
        *poster = m->poster;
        *at_ms = m->resume_ms;
        return i + 1;
    }
    return 0;
}

int movies_find(const char *q, Hit *out, int max) {
    if (!scanned) return 0;
    int n = 0;
    for (int sc = 3; sc >= 1; --sc)
        for (int i = 0; i < nmovies && n < max; ++i)
            if (match_score(movies[i].title, q) == sc) {
                Movie *m = &movies[i];
                out[n] = (Hit){H_FILM, i, 0, sc, m->title, "Film", NULL, "", 0};
                snprintf(out[n++].path, sizeof(out[0].path), POSTERS "%s.png", m->title);
            }
    return n;
}

void movies_open_hit(const Hit *h) { movies_open(h->a); }

void movies_open(int i) {
    if (i < 0 || i >= nmovies) return;
    sel = i;
    pos = sel;
    open_player();
}

const char *movies_hint(void) {
    static char h[64];
    if (sel >= 0 && sel < nmovies && movies[sel].resume_ms) {
        char at[16];
        clock_str(movies[sel].resume_ms, at, sizeof(at));
        snprintf(h, sizeof(h), "X resume at %s   <- -> choose   L R tabs", at);
        return h;
    }
    return "X play   <- -> choose   L R tabs";
}

void movies_leave(void) {
    if (full) close_player(0);
    video_stop_owned(OWN_MOVIE);
    previewing[0] = 0;
}

void movies_update(const Input *in) {
    if (!scanned) { text(font, 40, 140, C_DIM, 18, "Loading your films\xE2\x80\xA6"); return; }
    if (full) { player(in); return; }
    if (!nmovies) {
        text(bold, 40, 140, C_TEXT, 22, "No movies yet");
        text(font, 40, 176, C_DIM, 18, "Put .mp4 files (H.264, up to 960x544) in ux0:video");
        return;
    }
    int was = sel;
    if (in->pressed & (SCE_CTRL_LEFT | SCE_CTRL_UP)) sel = sel > 0 ? sel - 1 : 0;
    if (in->pressed & (SCE_CTRL_RIGHT | SCE_CTRL_DOWN)) sel = sel < nmovies - 1 ? sel + 1 : sel;
    /* Swipe: the row follows the finger, then settles on the nearest poster
     * (with a little of the flick's speed carried through). */
    static float fling;
    if (in->touching && in->drag_dx) {
        pos -= in->drag_dx / 150.0f;
        fling = -in->drag_dx / 150.0f;
        if (pos < 0) pos = 0;
        if (pos > nmovies - 1) pos = nmovies - 1;
        sel = (int)(pos + 0.5f);
    }
    if (in->released && fling != 0) {
        int to = (int)(pos + fling * 6 + 0.5f);
        sel = to < 0 ? 0 : to >= nmovies ? nmovies - 1 : to;
        fling = 0;
    }
    if (in->tapped && in->tap_y > 70 && in->tap_y < 460) {
        int dx = in->tap_x - W / 2;
        if (dx > -PW / 2 && dx < PW / 2) { open_player(); return; }
        int step = dx < 0 ? -1 : 1, ad = dx < 0 ? -dx : dx;
        int k = ad < 255 + 80 ? 1 : 1 + (ad - 255 - 80) / 150 + 1;
        int idx = sel + step * k;
        sel = idx < 0 ? 0 : idx >= nmovies ? nmovies - 1 : idx;
    }
    if (in->pressed & SCE_CTRL_CROSS) { open_player(); return; }
    if (sel != was) settle = 0;
    if (!in->touching || !in->drag_dx) pos += (sel - pos) * 0.18f;
    if (fabsf(sel - pos) < 0.002f) pos = sel;
    posters_near();
    ui_theme_from(movies[sel].poster);

    /* A muted preview of the one you are on, once the selection settles,
     * playing dimmed behind the posters. Not while music has the player. */
    Movie *m = &movies[sel];
    if (++settle == 40 && video_owner() != OWN_MUSIC && strcmp(previewing, m->path)) {
        if (video_play_opts(m->path, 0, 1, m->resume_ms ? m->resume_ms : PREVIEW_AT_MS, OWN_MOVIE) >= 0)
            snprintf(previewing, sizeof(previewing), "%s", m->path);
    }
    if (settle < 40 && previewing[0] && strcmp(previewing, m->path)) {
        video_stop_owned(OWN_MOVIE);
        previewing[0] = 0;
    }
    vita2d_texture *f = previewing[0] && video_owner() == OWN_MOVIE ? video_frame() : NULL;
    if (f) {
        float tw = vita2d_texture_get_width(f), th = vita2d_texture_get_height(f);
        float sc = W / tw > H / th ? W / tw : H / th;               /* fill the screen */
        vita2d_draw_texture_tint_scale(f, (W - tw * sc) / 2, (H - th * sc) / 2, sc, sc, RGBA8(255, 255, 255, 80));
    }
    for (int k = 0; k < 8; ++k)                                     /* darker toward the floor */
        vita2d_draw_rectangle(0, 380 + k * 16, W, 16, RGBA8(21, 24, 33, 40 + k * 22));

    cover_flow();

    int tw = text_w(bold, 24, m->title);
    if (tw > W - 80) tw = W - 80;
    text_fit(bold, (W - tw) / 2, 486, C_TEXT, 22, m->title, W - 80);   /* what X does is in the footer */
    char count[24];
    snprintf(count, sizeof(count), "%d / %d", sel + 1, nmovies);
    text_right(font, W - 30, 92, C_FAINT, 15, count);
}
