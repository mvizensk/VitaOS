/* The Home tab: one row to jump back into whatever you were doing (the last
 * games, the film you are part-way through, the album you were playing) over
 * the focused item's art, full screen, drifting slowly. PS5's home row is the
 * model: big art, one row, the name under the focused tile only. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include "hometab.h"
#include "play.h"
#include "movies.h"
#include "music.h"
#include "sfx.h"
#include "downloads.h"
#include "weather.h"
#include "video.h"
#include <psp2/rtc.h>
#include <psp2/power.h>

enum { K_GAME, K_MOVIE, K_MUSIC };
typedef struct {
    int kind, index;
    const char *title, *kicker, *meta1, *meta2;
    vita2d_texture *tile, *art;
    unsigned int accent;
    int resume;                               /* art is RetroArch's quick-resume snapshot */
} Item;

#define MAX_ITEMS 10
#define TILE 124.0f
#define ROW_Y 318.0f
#define GAP 22.0f

static Item items[MAX_ITEMS];
static int nitems, sel, want_tab = -1;
static float pos, grow;
static int back_prev = -1;                  /* the item whose art we are fading away from */
static float back_mix = 1, drift;
static int last_sel = -1;
static int swiping;                                 /* a finger is dragging the row */

static void add_movie(void) {
    static char meta[48];
    const char *title;
    vita2d_texture *poster;
    unsigned int at;
    int m = movies_resume_item(&title, &poster, &at);
    if (!m || nitems >= MAX_ITEMS) return;
    if (at >= 3600000) snprintf(meta, sizeof(meta), "Resume at %u:%02u:%02u", at / 3600000, at / 60000 % 60, at / 1000 % 60);
    else snprintf(meta, sizeof(meta), "Resume at %u:%02u", at / 60000, at / 1000 % 60);
    items[nitems++] = (Item){K_MOVIE, m - 1, title, "CONTINUE WATCHING", meta, "Movies", poster, poster, C_ACCENT};
}

static void add_music(void) {
    const char *album, *artist;
    vita2d_texture *art;
    int now;
    if (nitems >= MAX_ITEMS || !music_last_item(&album, &artist, &art, &now)) return;
    items[nitems++] = (Item){K_MUSIC, 0, album, now ? "NOW PLAYING" : "CONTINUE LISTENING", artist, "Music", art, art, C_OK};
}

/* Quick Resume: RetroArch saves a state every two minutes (and on a clean
 * quit) with a snapshot beside it, savestates/<core>/<rom>.state.auto.png,
 * and loads it on the next launch. Home shows that snapshot: the exact moment
 * you left. Resolved once per visit; the files only change while a game runs. */
#define STATES "ux0:data/retroarch/savestates/"
/* Listed once, on a background thread: Home restarts after every game, so
 * the list is fresh each time it matters. */
#define MAX_SNAPS 256
static char snap_path[MAX_SNAPS][200];
static int nsnap_paths;
static volatile int snaps_ready;

static int snaps_scan(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUID d = sceIoDopen(STATES);
    if (d >= 0) {
        SceIoDirent core;
        while (nsnap_paths < MAX_SNAPS) {
            memset(&core, 0, sizeof(core));
            if (sceIoDread(d, &core) <= 0) break;
            if (!SCE_S_ISDIR(core.d_stat.st_mode)) continue;
            char dir[256];
            snprintf(dir, sizeof(dir), STATES "%s", core.d_name);
            SceUID d2 = sceIoDopen(dir);
            if (d2 < 0) continue;
            SceIoDirent e;
            while (nsnap_paths < MAX_SNAPS) {
                memset(&e, 0, sizeof(e));
                if (sceIoDread(d2, &e) <= 0) break;
                int n = strlen(e.d_name);
                /* a blank frame (a load screen, a GPU core) compresses to a couple of KB; the cover beats it */
                if (n > 15 && !strcmp(e.d_name + n - 15, ".state.auto.png") && e.d_stat.st_size >= 4096)
                    snprintf(snap_path[nsnap_paths++], sizeof(snap_path[0]), "%s/%s", dir, e.d_name);
            }
            sceIoDclose(d2);
        }
        sceIoDclose(d);
    }
    snaps_ready = 1;
    return sceKernelExitDeleteThread(0);
}

static vita2d_texture *snapshot(const char *rom) {
    static int started;
    if (!started) {
        started = 1;
        SceUID t = sceKernelCreateThread("home_snaps", snaps_scan, 0x10000110, 0x4000, 0, 0, NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
    if (!rom || !snaps_ready) return NULL;
    const char *base = strrchr(rom, '/');
    base = base ? base + 1 : rom;
    const char *dot = strrchr(base, '.');
    int stem = dot ? (int)(dot - base) : (int)strlen(base);
    for (int i = 0; i < nsnap_paths; ++i) {
        const char *f = strrchr(snap_path[i], '/') + 1;
        if (!strncmp(f, base, stem) && !strcmp(f + stem, ".state.auto.png")) return ui_image(snap_path[i]);
    }
    return NULL;
}

/* Widgets, top right, on frosted glass: the time, the battery with what it
 * has left, the weather, what is playing and what is downloading. */
static void widgets(void) {
    int x = 664, y = 82, w = 272, h = 104;
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int dow = sceRtcGetDayOfWeek(t.year, t.month, t.day);
    char line[96], small[64];
    float dl;
    int dls = downloads_progress(&dl);
    const char *np = music_now();
    char wx[64], wsub[64];
    int wkind = 0, have_wx = weather_line(wx, sizeof(wx), wsub, sizeof(wsub), &wkind);
    if (have_wx) h += 50;
    if (*np) h += 44;
    if (dls) h += 44;
    vita2d_draw_rectangle(x, y, w, h, RGBA8(18, 21, 30, 170));
    vita2d_draw_rectangle(x, y, w, 1, RGBA8(255, 255, 255, 28));
    snprintf(line, sizeof(line), "%d:%02d", t.hour % 12 ? t.hour % 12 : 12, t.minute);
    text(bold, x + 18, y + 50, C_TEXT, 40, line);
    text(font, x + 22 + text_w(bold, 40, line), y + 50, C_DIM, 16, t.hour < 12 ? "AM" : "PM");
    snprintf(small, sizeof(small), "%s, %s %d", days[dow % 7], months[(t.month + 11) % 12], t.day);
    text(font, x + 20, y + 76, C_DIM, 15, small);
    int pct = scePowerGetBatteryLifePercent(), mins = scePowerGetBatteryLifeTime();
    if (scePowerIsBatteryCharging()) snprintf(small, sizeof(small), "%d%%  charging", pct);
    else if (scePowerIsPowerOnline()) snprintf(small, sizeof(small), "%d%%  plugged in", pct);
    else if (mins > 0) snprintf(small, sizeof(small), "%d%%  %dh %02dm left", pct, mins / 60, mins % 60);
    else snprintf(small, sizeof(small), "%d%%", pct);
    draw_bar(x + 20, y + 88, 60, 5, pct / 100.0f, pct < 15 ? C_BAD : C_OK);
    text(font, x + 90, y + 95, pct < 15 ? C_BAD : C_DIM, 14, small);
    int yy = y + 104;
    if (have_wx) {                           /* a small glyph, then the words */
        vita2d_draw_rectangle(x + 16, yy, w - 32, 1, RGBA8(255, 255, 255, 20));
        unsigned int gc = wkind == 0 ? RGBA8(250, 204, 21, 255) : wkind == 2 ? RGBA8(96, 165, 250, 255) : RGBA8(203, 213, 225, 255);
        if (wkind == 0) vita2d_draw_fill_circle(x + 27, yy + 19, 6, gc);
        else {
            vita2d_draw_fill_circle(x + 24, yy + 20, 5, gc);
            vita2d_draw_fill_circle(x + 31, yy + 18, 6, gc);
            if (wkind >= 2) for (int d = 0; d < 3; ++d) vita2d_draw_rectangle(x + 21 + d * 5, yy + 27, 2, 4, gc);
        }
        text_fit(bold, x + 42, yy + 25, C_TEXT, 16, wx, w - 58);
        text_fit(font, x + 42, yy + 43, C_DIM, 13, wsub, w - 58);
        yy += 50;
    }
    if (*np) {
        vita2d_draw_rectangle(x + 16, yy, w - 32, 1, RGBA8(255, 255, 255, 20));
        text_fit(font, x + 20, yy + 26, C_TEXT, 15, np, w - 40);
        unsigned int d = video_duration_ms();
        draw_bar(x + 20, yy + 34, w - 40, 3, d ? (float)video_pos_ms() / d : 0, C_ACCENT);
        yy += 44;
    }
    if (dls) {
        vita2d_draw_rectangle(x + 16, yy, w - 32, 1, RGBA8(255, 255, 255, 20));
        snprintf(small, sizeof(small), "Downloading %d file%s  \xC2\xB7  %d%%", dls, dls == 1 ? "" : "s", (int)(dl * 100));
        text_fit(font, x + 20, yy + 26, C_TEXT, 15, small, w - 40);
        draw_bar(x + 20, yy + 34, w - 40, 3, dl, C_OK);
    }
}

/* Newest game first, then the film and the album, then the older games. */
static void gather(void) {
    STAGE("home: gather");
    nitems = 0;
    int ngames = play_recent_count();
    for (int i = 0; i < ngames && nitems < MAX_ITEMS; ++i) {
        if (i == 1) { add_movie(); add_music(); }
        PlayItem g;
        play_recent_item(i, &g);
        vita2d_texture *snap = snapshot(g.rom);
        items[nitems++] = (Item){K_GAME, i, g.title, snap ? "QUICK RESUME" : i == 0 ? "JUMP BACK IN" : "RECENTLY PLAYED",
                                 g.system, *g.year ? g.year : g.genre, g.cover, snap ? snap : g.art, g.accent, snap != NULL};
    }
    if (ngames < 2) { add_movie(); add_music(); }
}

/* Scaled to cover the whole screen, drifting a few pixels (Ken Burns). */
static void backdrop(vita2d_texture *t, int alpha) {
    if (!t || alpha <= 0) return;
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    float sc = (W / tw > H / th ? W / tw : H / th) * (1.06f + 0.02f * sinf(drift * 0.7f));
    float x = (W - tw * sc) / 2 + 14 * sinf(drift) - 12 * ui_tilt_x, y = (H - th * sc) / 2 + 8 * cosf(drift * 0.8f) + 8 * ui_tilt_y;
    vita2d_draw_texture_tint_scale(t, x, y, sc, sc, RGBA8(255, 255, 255, alpha));
}

void hometab_reset(void) { sel = 0; }

int hometab_wants_tab(void) { int t = want_tab; want_tab = -1; return t; }

const char *hometab_hint(void) {
    if (!nitems) return "L R tabs";
    switch (items[sel].kind) {
    case K_MOVIE: return "X resume   <- -> choose   L R tabs";
    case K_MUSIC: return "X play / pause   <- -> choose   L R tabs";
    default: return "X play   <- -> choose   L R tabs";
    }
}

void hometab_update(const Input *in) {
    gather();
    drift += 0.004f;
    if (!nitems) {
        text(bold, 48, 200, C_TEXT, 30, "Welcome back");
        text(font, 48, 236, C_DIM, 18, "Play a game, a film or an album and it shows up here.");
        return;
    }
    if (sel >= nitems) sel = nitems - 1;
    if (in->pressed & SCE_CTRL_LEFT) { if (sel > 0) sel--; else sfx_play(SFX_BUMP); }
    if (in->pressed & SCE_CTRL_RIGHT) { if (sel < nitems - 1) sel++; else sfx_play(SFX_BUMP); }
    if (in->tapped && in->tap_y > ROW_Y - 20 && in->tap_y < ROW_Y + TILE + 30) {
        int i = (int)((in->tap_x - 48 + (pos - (int)pos) * (TILE + GAP)) / (TILE + GAP)) + (int)pos;
        if (i >= 0 && i < nitems) {
            if (i == sel) goto act;
            sel = i;
        }
    }
    /* A swipe along the row scrolls it; letting go picks the tile that is now
     * where the focused one was (playtest 2026-09-25: it would not swipe). */
    if (in->touching && in->drag_dx && in->ty > ROW_Y - 20 && in->ty < ROW_Y + TILE + 30) swiping = 1;
    if (swiping) {
        pos -= in->drag_dx / (float)(TILE + GAP);
        if (pos < -0.5f) pos = -0.5f;
        if (pos > nitems - 0.5f) pos = nitems - 0.5f;
        if (!in->touching) {
            int t0 = sel > 2 ? sel - 2 : 0, ns = (int)(pos + 0.5f) + (sel - t0);
            sel = ns < 0 ? 0 : ns >= nitems ? nitems - 1 : ns;
            swiping = 0;
        }
    }
    if (in->pressed & SCE_CTRL_CROSS) goto act;
    goto draw;
act: {
        Item *it = &items[sel];
        if (it->kind == K_GAME) {
            if (play_recent_launch(it->index) < 0) ui_message("Could not start", "The game would not launch.");
        } else if (it->kind == K_MOVIE) {
            want_tab = HOMETAB_TO_MOVIES;
            movies_open(it->index);
        } else {
            music_resume();
        }
    }
draw:
    if (sel != last_sel) {
        back_prev = last_sel;   /* by index: texture caches may free old pointers */
        back_mix = 0;
        grow = 0;
        last_sel = sel;
    }
    back_mix = back_mix < 1 ? back_mix + 0.06f : 1;
    grow = grow < 1 ? grow + 0.08f : 1;
    /* Keep the focused tile near the left, like a console home row. */
    float target = sel > 2 ? sel - 2 : 0;
    if (!swiping) pos += (target - pos) * 0.2f;

    Item *it = &items[sel];
    ui_theme_from(it->tile);
    if (back_prev >= 0 && back_prev < nitems && back_mix < 1) backdrop(items[back_prev].art, (int)(150 * (1 - back_mix)));
    backdrop(it->art, (int)(150 * back_mix));
    for (int k = 0; k < 12; ++k)                                   /* darken toward the row and the left */
        vita2d_draw_rectangle(0, 64 + k * 40, W, 40, RGBA8(21, 24, 33, 40 + k * 16));
    for (int k = 0; k < 10; ++k)
        vita2d_draw_rectangle(k * 50, 64, 50, H - 104, RGBA8(21, 24, 33, 150 - k * 15));
    ui_ambient(0.6f);

    text(bold, 48, 118, it->kind == K_GAME ? (it->accent | 0xFF000000) : C_ACCENT, 14, it->kicker);
    text_fit(bold, 48, 162, C_TEXT, 36, it->title, 596);
    char meta[128];
    snprintf(meta, sizeof(meta), "%s%s%s", it->meta1, *it->meta1 && *it->meta2 ? "   \xC2\xB7   " : "", it->meta2);
    text_fit(font, 48, 194, C_DIM, 18, meta, 596);
    widgets();

    for (int i = 0; i < nitems; ++i) {
        float x = 48 + (i - pos) * (TILE + GAP);
        if (x < -TILE * 1.4f || x > W) continue;
        float lift = i == sel ? ease_back(grow) : 0, size = TILE * (1 + 0.22f * lift);
        float tx = x + (i > sel ? TILE * 0.22f : 0), ty = ROW_Y - 10 * lift - (size - TILE);   /* room for the big one */
        if (i == sel) draw_focus(tx, ty, size, size, lift > 1 ? 1 : lift);
        Item *t = &items[i];
        float rr = ui_corner(size, size);                      /* the same corners as the focus ring */
        if (t->tile) draw_round_cover(t->tile, tx, ty, size, rr, RGBA8(255, 255, 255, i == sel ? 255 : 200));
        else {
            draw_round_rect(tx, ty, size, size, rr, RGBA8(34, 40, 56, 255));
            text_fit(bold, (int)tx + 10, (int)(ty + size / 2), C_TEXT, 15, t->title, (int)size - 20);
        }
        if (t->resume) {                                        /* the snapshot, small, in the corner */
            float sw = size * 0.46f, tw2 = vita2d_texture_get_width(t->art), th2 = vita2d_texture_get_height(t->art);
            float sh = sw * th2 / tw2;
            draw_round_rect(tx + size - sw - 8, ty + size - sh - 8, sw + 4, sh + 4, 7, RGBA8(236, 239, 244, 230));
            draw_round_texture(t->art, tx + size - sw - 6, ty + size - sh - 6, sw, sh, 5, RGBA8(255, 255, 255, 255));
        }
        if (t->kind != K_GAME) {                               /* a small badge: film or music */
            draw_round_rect(tx + 8, ty + size - 30, 58, 22, 11, RGBA8(21, 24, 33, 210));   /* a pill, inside the corner */
            text(bold, (int)tx + 17, (int)(ty + size - 14), t->kind == K_MOVIE ? C_ACCENT : C_OK, 12,
                 t->kind == K_MOVIE ? "FILM" : "MUSIC");
        }
        if (i == sel) text_fit(bold, (int)tx, (int)(ROW_Y + TILE + 34), C_TEXT, 16, t->title, 300);
    }
}
