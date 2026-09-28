/* Music, laid out like YouTube Music: Home (listen again, your mixes), Liked
 * songs, Albums, Artists and Songs across the top; album, artist and playlist
 * pages with Play and Shuffle; a mini player and a full Now Playing screen
 * with like, shuffle, repeat and what plays next. Likes and recently played
 * are kept in ux0:data/arcadehub/user/. The library is what
 * home/tools/import_music.py copies to ux0:data/music (AAC, covers, library.tsv). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include "music.h"
#include "search.h"
#include "sfx.h"
#include "video.h"
#include "musiclib.h"

#define LIB "ux0:data/music/"      /* not ux0:music: apps get EPERM there */
#define USERDIR "ux0:data/arcadehub/user/"
#define LAST USERDIR "music-last.txt"
#define LIKED USERDIR "music-liked.tsv"
#define RECENT USERDIR "music-recent.tsv"
#define MAX_TRACKS 4096
#define MAX_ALBUMS 512
#define MAX_ARTISTS 512
#define MAX_RECENT 24

typedef struct { char *path, *title, *artist, *album, *cover; int track, album_ix; unsigned int dur; } Track;
typedef struct { int first, count; } Album;
typedef struct { const char *name; int first, count; } Artist;   /* tracks are sorted by artist */

static Track tracks[MAX_TRACKS];
static Album albums[MAX_ALBUMS];
static Artist artists[MAX_ARTISTS];
static int ntracks, nalbums, nartists, load_rc;
static int by_title[MAX_TRACKS];            /* Songs, A to Z */
static volatile int loaded;                 /* set by music_prewarm (a background thread) when done */
static char *blob;

static unsigned char liked[MAX_TRACKS];
static int liked_list[MAX_TRACKS], nliked;  /* newest like first, as YouTube Music lists them */
static int recent[MAX_RECENT], nrecent;     /* tracks, newest first */

/* The queue: what plays, in order; qpos is the current one. */
static int queue[MAX_TRACKS], nqueue, qpos = -1;
static int playing = -1, state, shuffle, repeat;   /* state 1 = playing; repeat 0 off, 1 all, 2 one */
static int last_played = -1;
int music_playing(void);

/* ---------- screens ---------- */
enum { S_HOME, S_LIKED, S_ALBUMS, S_ARTISTS, S_SONGS, NSECT };
static const char *const SECT[NSECT] = {"Home", "Liked songs", "Albums", "Artists", "Songs"};
enum { P_NONE, P_ALBUM, P_ARTIST };          /* a page opened from a section */
static int section, chips, page, page_ix, now_open;
static int sel, sel_page, home_row, home_col[3];
static float scroll_page, home_scroll[3];
static int list_buf[MAX_TRACKS];

/* ---------- library ---------- */

static int title_cmp(const void *a, const void *b) {
    return strcasecmp(tracks[*(const int *)a].title, tracks[*(const int *)b].title);
}

static int int_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void read_small(const char *path, char *out, int max) {
    out[0] = 0;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    int r = sceIoRead(fd, out, max - 1);
    sceIoClose(fd);
    out[r > 0 ? r : 0] = 0;
}

static int track_by_path(const char *p) {
    for (int i = 0; i < ntracks; ++i) if (!strcmp(tracks[i].path, p)) return i;
    return -1;
}

static void load(void) {
    free(blob);
    blob = NULL;
    ntracks = nalbums = nartists = 0;
    memset(liked, 0, sizeof(liked));                  /* indices change when the library does */
    nliked = nrecent = 0;
    last_played = -1;
    SceUID fd = sceIoOpen(LIB "library.tsv", SCE_O_RDONLY, 0);
    load_rc = fd;
    if (fd < 0) { loaded = 1; return; }
    int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size <= 0 || !(blob = malloc(size + 1))) { sceIoClose(fd); loaded = 1; return; }
    int n = sceIoRead(fd, blob, size);
    sceIoClose(fd);
    blob[n > 0 ? n : 0] = 0;
    char *line = blob;
    while (line && *line && ntracks < MAX_TRACKS) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *f[7] = {0};
        int k = 0;
        for (char *p = line; k < 7 && p; ++k) { f[k] = p; p = strchr(p, '\t'); if (p) *p++ = 0; }
        if (k >= 6) {
            Track *t = &tracks[ntracks];
            t->path = f[0]; t->title = f[1]; t->artist = f[2]; t->album = f[3];
            t->track = atoi(f[4]); t->dur = (unsigned int)strtoul(f[5], NULL, 10); t->cover = k > 6 ? f[6] : "";
            /* library.tsv is sorted by artist, album, track: albums and artists are runs */
            if (!nalbums || strcmp(tracks[albums[nalbums - 1].first].album, t->album) ||
                strcmp(tracks[albums[nalbums - 1].first].artist, t->artist)) {
                if (nalbums == MAX_ALBUMS) break;
                albums[nalbums].first = ntracks;
                albums[nalbums++].count = 0;
            }
            albums[nalbums - 1].count++;
            t->album_ix = nalbums - 1;
            if (!nartists || strcasecmp(artists[nartists - 1].name, t->artist)) {
                if (nartists < MAX_ARTISTS) { artists[nartists].name = t->artist; artists[nartists].first = ntracks; artists[nartists++].count = 0; }
            }
            if (nartists) artists[nartists - 1].count++;
            ++ntracks;
        }
        line = next;
    }
    for (int i = 0; i < ntracks; ++i) by_title[i] = i;
    qsort(by_title, ntracks, sizeof(int), title_cmp);

    static char buf[64 * 1024];
    read_small(LAST, buf, 400);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    if (buf[0]) last_played = track_by_path(buf);
    read_small(LIKED, buf, sizeof(buf));
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
        int i = track_by_path(l);
        if (i >= 0 && !liked[i]) { liked[i] = 1; liked_list[nliked++] = i; }
    }
    read_small(RECENT, buf, sizeof(buf));
    for (char *l = strtok(buf, "\n"); l && nrecent < MAX_RECENT; l = strtok(NULL, "\n")) {
        int i = track_by_path(l);
        if (i >= 0) recent[nrecent++] = i;
    }
    loaded = 1;
}

void music_prewarm(void) { musiclib_scan(); load(); }   /* VitaOS: find the music on the card first */

static void save_list(const char *path, const int *list, int n) {
    static char buf[128 * 1024];
    int len = 0;
    for (int i = 0; i < n && len < (int)sizeof(buf) - 300; ++i)
        len += snprintf(buf + len, sizeof(buf) - len, "%s\n", tracks[list[i]].path);
    ui_save(path, buf, len, 0);
}

static void toggle_like(int i) {
    if (i < 0 || i >= ntracks) return;
    if (liked[i]) {
        liked[i] = 0;
        for (int k = 0; k < nliked; ++k)
            if (liked_list[k] == i) { memmove(&liked_list[k], &liked_list[k + 1], (--nliked - k) * sizeof(int)); break; }
        ui_toast("Removed from Liked songs", C_DIM);
    } else {
        liked[i] = 1;
        memmove(&liked_list[1], &liked_list[0], nliked++ * sizeof(int));
        liked_list[0] = i;
        ui_toast("Added to Liked songs", C_ACCENT);
    }
    sfx_play(SFX_SELECT);
    save_list(LIKED, liked_list, nliked);
}

/* ---------- art ---------- */

static vita2d_texture *cover(const Track *t) {
    if (!t->cover || !*t->cover) return NULL;
    char path[256];
    snprintf(path, sizeof(path), LIB "%s", t->cover);
    return ui_image(path);
}

static int is_single(const Track *t) { return !strcmp(t->album, "Singles") || t->album[0] == '<'; }

static void draw_cover(const Track *t, float x, float y, float s, int on) {
    vita2d_texture *c = cover(t);
    if (c) { draw_round_cover(c, x, y, s, ui_corner(s, s), RGBA8(255, 255, 255, on ? 255 : 200)); return; }
    char path[256];
    if (t->cover && *t->cover) {
        snprintf(path, sizeof(path), LIB "%s", t->cover);
        if (ui_image_pending(path)) { draw_shimmer(x, y, s, s); return; }
    }
    draw_made_cover(is_single(t) ? t->artist : t->album, is_single(t) ? "Singles" : t->artist, x, y, s, !on);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, unsigned int c) {
    vita2d_color_vertex *v = vita2d_pool_memalign(3 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    v[0] = (vita2d_color_vertex){x0, y0, 0.5f, c};
    v[1] = (vita2d_color_vertex){x1, y1, 0.5f, c};
    v[2] = (vita2d_color_vertex){x2, y2, 0.5f, c};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
}

/* A heart: two circles and a point; hollow when not liked. */
/* A heart from the classic curve (x = 16 sin^3 t, y = 13 cos t - 5 cos 2t -
 * 2 cos 3t - cos 4t), 72 points: the two-circles-and-a-triangle version showed
 * seams and jagged edges (2026-09-25). Filled, or an outline ring that works
 * over any background, each with a faint wider pass as an anti-aliased edge. */
#define HEART_N 72
static void heart_pts(float cx, float cy, float s, float *px, float *py) {
    for (int i = 0; i < HEART_N; ++i) {
        float t = i * 6.2831853f / HEART_N, st = sinf(t);
        float x = 16 * st * st * st, y = 13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t);
        px[i] = cx + x * s / 34.0f;
        py[i] = cy - (y + 2.5f) * s / 34.0f;               /* centred on the shape, not the curve's origin */
    }
}

static void heart_fill(float cx, float cy, float s, unsigned int c) {
    float px[HEART_N], py[HEART_N];
    heart_pts(cx, cy, s, px, py);
    vita2d_color_vertex *v = vita2d_pool_memalign((HEART_N + 2) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    v[0] = (vita2d_color_vertex){cx, cy, 0.5f, c};
    for (int i = 0; i < HEART_N; ++i) v[i + 1] = (vita2d_color_vertex){px[i], py[i], 0.5f, c};
    v[HEART_N + 1] = v[1];
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, HEART_N + 2);
}

static void heart_ring(float cx, float cy, float s, float inner, unsigned int c) {
    float ox[HEART_N], oy[HEART_N], ix[HEART_N], iy[HEART_N];
    heart_pts(cx, cy, s, ox, oy);
    heart_pts(cx, cy + s * 0.03f, s * inner, ix, iy);
    vita2d_color_vertex *v = vita2d_pool_memalign((2 * HEART_N + 2) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    for (int i = 0; i <= HEART_N; ++i) {
        int k = i % HEART_N;
        v[2 * i] = (vita2d_color_vertex){ox[k], oy[k], 0.5f, c};
        v[2 * i + 1] = (vita2d_color_vertex){ix[k], iy[k], 0.5f, c};
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 2 * HEART_N + 2);
}

static void heart(float cx, float cy, float s, int filled, unsigned int c) {
    unsigned int soft = (c & 0x00FFFFFF) | ((((c >> 24) & 255) / 4) << 24);
    if (filled) {
        heart_fill(cx, cy, s * 1.05f, soft);               /* the soft edge */
        heart_fill(cx, cy, s, c);
    } else {
        heart_ring(cx, cy, s * 1.04f, 0.72f, soft);
        heart_ring(cx, cy, s, 0.74f, c);
    }
}

enum { I_PLAY, I_PAUSE, I_NEXT, I_PREV, I_SHUFFLE, I_REPEAT };
/* A line w pixels wide, as two triangles, with square-ish caps. */
static void thick(float x0, float y0, float x1, float y1, float w, unsigned int c) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * w / 2, ny = dx / len * w / 2, ex = dx / len * w * 0.35f, ey = dy / len * w * 0.35f;
    x0 -= ex; y0 -= ey; x1 += ex; y1 += ey;                /* overlap the joints */
    tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

static void icon(int kind, float cx, float cy, float s, unsigned int c) {
    float h = s / 2;
    switch (kind) {
    case I_PLAY: tri(cx - h * 0.6f, cy - h, cx - h * 0.6f, cy + h, cx + h, cy, c); break;
    case I_PAUSE: vita2d_draw_rectangle(cx - h * 0.8f, cy - h, h * 0.55f, s, c); vita2d_draw_rectangle(cx + h * 0.25f, cy - h, h * 0.55f, s, c); break;
    case I_NEXT: tri(cx - h, cy - h * 0.8f, cx - h, cy + h * 0.8f, cx + h * 0.5f, cy, c); vita2d_draw_rectangle(cx + h * 0.55f, cy - h * 0.8f, h * 0.3f, s * 0.8f, c); break;
    case I_PREV: tri(cx + h, cy - h * 0.8f, cx + h, cy + h * 0.8f, cx - h * 0.5f, cy, c); vita2d_draw_rectangle(cx - h * 0.85f, cy - h * 0.8f, h * 0.3f, s * 0.8f, c); break;
    case I_SHUFFLE: {                                 /* two crossing arrows, solid strokes (1 px lines looked jagged) */
        float w = s * 0.13f, top = cy - h * 0.5f, bot = cy + h * 0.5f;
        float xl = cx - h, xs = cx - h * 0.42f, xd = cx + h * 0.28f, xa = cx + h * 0.5f;
        for (int k = 0; k < 2; ++k) {
            float y0 = k ? bot : top, y1 = k ? top : bot;
            thick(xl, y0, xs, y0, w, c);                  /* in from the left */
            thick(xs, y0, xd, y1, w, c);                  /* across */
            thick(xd, y1, xa, y1, w, c);                  /* out to the arrow */
            tri(xa - w * 0.2f, y1 - h * 0.34f, xa - w * 0.2f, y1 + h * 0.34f, cx + h, y1, c);
        }
        break;
    }
    case I_REPEAT:                                    /* a loop with two arrows */
        vita2d_draw_rectangle(cx - h, cy - h * 0.6f, s * 0.8f, 2, c);
        vita2d_draw_rectangle(cx - h * 0.6f, cy + h * 0.6f - 2, s * 0.8f, 2, c);
        vita2d_draw_rectangle(cx - h, cy - h * 0.6f, 2, h * 0.9f, c);
        vita2d_draw_rectangle(cx + h - 2, cy - h * 0.3f, 2, h * 0.9f, c);
        tri(cx + h * 0.4f, cy - h * 0.95f, cx + h * 0.4f, cy - h * 0.25f, cx + h * 0.85f, cy - h * 0.6f, c);
        tri(cx - h * 0.4f, cy + h * 0.95f, cx - h * 0.4f, cy + h * 0.25f, cx - h * 0.85f, cy + h * 0.6f, c);
        break;
    }
}

/* Three bars that dance while a row's song is playing. */
static void eq_bars(float x, float y, unsigned int c) {
    static unsigned int f;
    ++f;
    for (int k = 0; k < 3; ++k) {
        float h = video_paused() ? 4 : 5 + 7 * (0.5f + 0.5f * sinf(f * 0.16f + k * 2.1f));
        vita2d_draw_rectangle(x + k * 5, y - h, 3, h, c);
    }
}

/* ---------- playback ---------- */

static void note_recent(int i) {
    for (int k = 0; k < nrecent; ++k)
        if (recent[k] == i) { memmove(&recent[k], &recent[k + 1], (--nrecent - k) * sizeof(int)); break; }
    if (nrecent == MAX_RECENT) nrecent--;
    memmove(&recent[1], &recent[0], nrecent++ * sizeof(int));
    recent[0] = i;
    save_list(RECENT, recent, nrecent);
}

static void start(int i) {
    if (i < 0 || i >= ntracks) { state = 0; return; }
    char path[320];
    snprintf(path, sizeof(path), LIB "%s", tracks[i].path);
    if (video_play_opts(path, 1, 0, 0, OWN_MUSIC) < 0) { state = 0; ui_toast("This song would not play", C_BAD); return; }
    playing = i;
    state = 1;
    note_recent(i);
    if (last_played != i) { last_played = i; ui_save(LAST, tracks[i].path, strlen(tracks[i].path), 0); }
}

/* Play a list from one of its songs; shuffled keeps that song first. */
static void play_list(const int *list, int n, int from, int shuf) {
    if (n <= 0) return;
    memmove(queue, list, n * sizeof(int));
    nqueue = n;
    shuffle = shuf;
    int first = from >= 0 && from < n ? list[from] : -1;
    if (shuf) {
        for (int k = n - 1; k > 0; --k) { int j = rand() % (k + 1), t = queue[k]; queue[k] = queue[j]; queue[j] = t; }
        if (first >= 0) for (int k = 0; k < n; ++k) if (queue[k] == first) { queue[k] = queue[0]; queue[0] = first; break; }
        from = 0;
    }
    qpos = from < 0 ? 0 : from;
    start(queue[qpos]);
}

static void advance(int dir, int user) {
    if (qpos < 0 || !nqueue) return;
    if (repeat == 2 && !user) { start(queue[qpos]); return; }
    int to = qpos + dir;
    if (to >= nqueue) { if (repeat == 1 || user) to = 0; else { state = 0; return; } }
    if (to < 0) to = 0;
    qpos = to;
    start(queue[qpos]);
}

static void toggle_shuffle(void) {
    shuffle = !shuffle;
    if (qpos >= 0 && nqueue) {
        int cur = queue[qpos];
        if (shuffle) {
            for (int k = nqueue - 1; k > 0; --k) { int j = rand() % (k + 1), t = queue[k]; queue[k] = queue[j]; queue[j] = t; }
            for (int k = 0; k < nqueue; ++k) if (queue[k] == cur) { queue[k] = queue[0]; queue[0] = cur; break; }
            qpos = 0;
        } else {                                      /* back to library order, from the current song */
            qsort(queue, nqueue, sizeof(int), int_cmp);
            for (int k = 0; k < nqueue; ++k) if (queue[k] == cur) { qpos = k; break; }
        }
    }
    ui_toast(shuffle ? "Shuffle on" : "Shuffle off", C_ACCENT);
}

int music_playing(void) { return state == 1 && video_owner() == OWN_MUSIC; }

const char *music_now(void) {
    static char line[128];
    if (!music_playing()) return "";
    snprintf(line, sizeof(line), "\xE2\x99\xAA  %s  \xC2\xB7  %s%s", tracks[playing].title, tracks[playing].artist,
             video_paused() ? "  (paused)" : "");
    return line;
}

/* Every frame, whatever the tab: the next song when one ends, and no auto
 * standby while music plays (it would stop it). */
void music_tick(const Input *in) {
    (void)in;
    if (state != 1) return;
    int ended = video_owner() == OWN_MUSIC || video_owner() == OWN_NONE ? video_finished() : 0;
    if (ended) {
        advance(1, 0);
        if (state == 1) {
            char msg[96];
            snprintf(msg, sizeof(msg), "\xE2\x99\xAA  %.40s  \xC2\xB7  %.30s", tracks[playing].title, tracks[playing].artist);
            ui_toast(msg, C_OK);
        }
        return;
    }
    if (video_owner() != OWN_MUSIC) { state = 0; return; }   /* a film took the player */
    if (!video_paused()) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

/* ---------- search and the Home tab ---------- */

int music_find(const char *q, Hit *out, int max) {
    if (!loaded) return 0;
    int n = 0;
    for (int sc = 3; sc >= 1; --sc)
        for (int i = 0; i < nalbums && n < max; ++i) {
            const Track *t = &tracks[albums[i].first];
            int s1 = match_score(t->album, q), s2 = match_score(t->artist, q);
            if ((s1 > s2 ? s1 : s2) != sc) continue;
            out[n] = (Hit){H_ALBUM, i, 0, sc, is_single(t) ? t->title : t->album, t->artist, NULL, "", 0};
            if (t->cover && *t->cover) snprintf(out[n].path, sizeof(out[0].path), LIB "%s", t->cover);
            n++;
        }
    return n;
}

void music_open_hit(const Hit *h) {
    if (h->a < 0 || h->a >= nalbums) return;
    section = S_ALBUMS; sel = h->a; page = P_ALBUM; page_ix = h->a; sel_page = -1; scroll_page = 0; now_open = 0; chips = 0;
}

int music_last_item(const char **album, const char **artist, vita2d_texture **art, int *now_playing) {
    if (!loaded || last_played < 0 || last_played >= ntracks) return 0;
    const Track *t = &tracks[last_played];
    *album = is_single(t) ? t->title : t->album;
    *artist = t->artist;
    *art = cover(t);
    *now_playing = music_playing();
    return 1;
}

void music_resume(void) {
    if (last_played < 0) return;
    if (music_playing()) { video_pause(!video_paused()); return; }
    const Album *a = &albums[tracks[last_played].album_ix];
    int n = 0;
    for (int k = 0; k < a->count; ++k) list_buf[n++] = a->first + k;
    play_list(list_buf, n, last_played - a->first, 0);
}

/* ---------- drawing pieces ---------- */

#define TOP_Y 118                               /* below the section chips */
static int list_bottom(void) { return H - 40 - (playing >= 0 ? 64 : 0); }

static void fmt_time(unsigned int ms, char *out, int max) { snprintf(out, max, "%u:%02u", ms / 60000, ms / 1000 % 60); }

/* One song row: cover, title, artist and album, heart, length. */
static void song_row(int ti, float y, int focused, int number) {
    const Track *t = &tracks[ti];
    if (focused) { draw_round_rect(24, y - 2, W - 48, 52, 10, RGBA8(255, 255, 255, 18)); draw_round_rect(24, y - 2, 4, 52, 2, C_ACCENT); }
    int x = 40;
    if (number > 0) {
        char n[8];
        snprintf(n, sizeof(n), "%d", number);
        if (ti == playing && music_playing()) eq_bars(x + 2, y + 32, C_ACCENT);
        else text(font, x, y + 31, C_FAINT, 15, n);
        x += 34;
    }
    draw_cover(t, x, y + 2, 44, 1);
    if (ti == playing && music_playing() && number <= 0) { vita2d_draw_rectangle(x, y + 2, 44, 44, RGBA8(0, 0, 0, 130)); eq_bars(x + 14, y + 32, C_TEXT); }
    unsigned int tc = ti == playing ? C_ACCENT : C_TEXT;
    text_fit(focused ? bold : font, x + 58, y + 21, tc, 16, t->title, 520);
    char sub[160];
    snprintf(sub, sizeof(sub), "%s%s%s", t->artist, is_single(t) ? "" : "  \xC2\xB7  ", is_single(t) ? "" : t->album);
    text_fit(font, x + 58, y + 41, C_DIM, 13, sub, 520);
    if (liked[ti]) heart(W - 118, y + 23, 16, 1, RGBA8(255, 90, 110, 255));
    else if (focused) heart(W - 118, y + 23, 16, 0, C_FAINT);
    char d[12];
    fmt_time(t->dur, d, sizeof(d));
    text_right(font, W - 44, y + 29, C_FAINT, 14, d);
}

/* A playlist/album/artist page header: art, title, subtitle, Play and Shuffle. */
static void page_header(const char *title, const char *sub, const Track *art_track, int is_liked, int focus_btn) {
    float ax = 40, ay = TOP_Y - scroll_page * 52 + 4;
    if (ay < -200) return;
    if (is_liked) {
        draw_gradient(ax, ay, 150, 150, RGBA8(236, 72, 153, 255), RGBA8(124, 58, 237, 255), RGBA8(79, 70, 229, 255), RGBA8(37, 99, 235, 255));
        heart(ax + 75, ay + 72, 60, 1, RGBA8(255, 255, 255, 240));
    } else if (art_track) draw_cover(art_track, ax, ay, 150, 1);
    text_fit(bold, 214, ay + 44, C_TEXT, 30, title, W - 260);
    text_fit(font, 214, ay + 74, C_DIM, 16, sub, W - 260);
    const char *labels[2] = {"Play", "Shuffle"};
    for (int b = 0; b < 2; ++b) {
        float bx = 214 + b * 150, by = ay + 100;
        int on = focus_btn == b;
        if (on) draw_focus(bx, by, 134, 40, 1);
        draw_round_rect(bx, by, 134, 40, 20, b == 0 ? RGBA8(245, 245, 250, 255) : RGBA8(255, 255, 255, on ? 40 : 22));
        unsigned int c = b == 0 ? RGBA8(15, 15, 20, 255) : C_TEXT;
        icon(b == 0 ? I_PLAY : I_SHUFFLE, bx + 30, by + 20, 14, c);
        text(bold, (int)bx + 50, (int)by + 26, c, 16, labels[b]);
    }
}

/* A list page (Liked songs, an album, an artist, Songs): the header's two
 * buttons are row -1; songs follow. Returns the song picked with X, or -2 for
 * Play, -3 for Shuffle, -1 for nothing. */
/* Curating: [] on a song asks, then removes the file, writes it to
 * music-removed.tsv (import_music.py leaves those out next time), rewrites
 * library.tsv without it and reloads. Likes, recents and what is playing are
 * kept by path. */
#define REMOVED USERDIR "music-removed.tsv"
static void delete_track(int i) {
    if (i < 0 || i >= ntracks) return;
    char msg[240], path[512], gone[400], now_path[400] = "";
    snprintf(msg, sizeof(msg), "Delete \"%.60s\" by %.40s from the Vita? This can't be undone.", tracks[i].title, tracks[i].artist);
    if (!ui_confirm("Delete song", msg)) return;
    snprintf(gone, sizeof(gone), "%s", tracks[i].path);
    if (playing >= 0) snprintf(now_path, sizeof(now_path), "%s", tracks[playing].path);
    if (playing == i) { video_stop(); state = 0; playing = -1; nqueue = 0; qpos = -1; now_path[0] = 0; }
    snprintf(path, sizeof(path), LIB "%s", gone);
    sceIoRemove(path);
    char line[420];
    int ln = snprintf(line, sizeof(line), "%s\n", gone);
    ui_save(REMOVED, line, ln, 1);
    /* library.tsv again, without it (a deliberate action: a short write here is fine) */
    SceUID fd = sceIoOpen(LIB "library.tsv.new", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { ui_toast("Could not update the library", C_BAD); return; }
    static char row[2048];
    for (int k = 0; k < ntracks; ++k) {
        if (k == i) continue;
        const Track *t = &tracks[k];
        int n = snprintf(row, sizeof(row), "%s\t%s\t%s\t%s\t%d\t%u\t%s\n", t->path, t->title, t->artist, t->album, t->track, t->dur,
                         t->cover ? t->cover : "");
        sceIoWrite(fd, row, n);
    }
    sceIoClose(fd);
    sceIoRemove(LIB "library.tsv");
    sceIoRename(LIB "library.tsv.new", LIB "library.tsv");
    int keep_queue = nqueue;
    load();
    if (now_path[0]) playing = track_by_path(now_path);
    if (keep_queue) { nqueue = 0; qpos = -1; }          /* the queue held old indices */
    if (sel_page >= 0) sel_page--;
    ui_toast("Song deleted", C_ACCENT);
}

static int list_page(const Input *in, const int *list, int n, const char *title, const char *sub,
                     const Track *art, int is_liked, int numbered) {
    unsigned int p = in->pressed;
    static int btn;
    if (sel_page < -1) sel_page = -1;
    if (sel_page >= n) sel_page = n - 1;
    if (p & SCE_CTRL_UP) sel_page = sel_page > -1 ? sel_page - 1 : -1;
    if (p & SCE_CTRL_DOWN && sel_page < n - 1) sel_page++;
    if (sel_page == -1 && (p & SCE_CTRL_LEFT)) btn = 0;
    if (sel_page == -1 && (p & SCE_CTRL_RIGHT)) btn = 1;
    if (sel_page >= 0 && (p & SCE_CTRL_TRIANGLE)) toggle_like(list[sel_page]);
    if (sel_page >= 0 && (p & SCE_CTRL_SQUARE)) { delete_track(list[sel_page]); return -1; }
    if (in->touching && in->drag_dy) scroll_page -= in->drag_dy / 52.0f;
    int result = -1;
    if (p & SCE_CTRL_CROSS) result = sel_page == -1 ? (btn ? -3 : -2) : sel_page;
    if (in->tapped) {
        float hy = TOP_Y - scroll_page * 52 + 104;
        if (in->tap_y >= hy && in->tap_y < hy + 40 && in->tap_x >= 214 && in->tap_x < 514) result = in->tap_x < 348 ? -2 : -3;
        else if (in->tap_y < list_bottom()) {
            int k = (int)floorf((in->tap_y - (TOP_Y + 180 - scroll_page * 52)) / 52.0f);
            if (k >= 0 && k < n) { if (k == sel_page) result = k; else sel_page = k; }
        }
    }
    /* keep the focused row on screen (not while a finger is scrolling) */
    float rows_vis = (list_bottom() - TOP_Y) / 52.0f;
    if (!in->touching) {
        float lo = sel_page < 0 ? 0 : sel_page + 4.6f - rows_vis, hi = sel_page < 0 ? 0 : sel_page + 3.4f;
        if (scroll_page < lo) scroll_page += (lo - scroll_page) * 0.3f;
        if (scroll_page > hi) scroll_page += (hi - scroll_page) * 0.3f;
    }
    float max_scroll = n + 3.6f - rows_vis;
    if (scroll_page > max_scroll) scroll_page = max_scroll;
    if (scroll_page < 0) scroll_page = 0;

    page_header(title, sub, art, is_liked, sel_page == -1 ? btn : -1);
    for (int k = 0; k < n; ++k) {
        float y = TOP_Y + 180 + (k - scroll_page) * 52;
        if (y < TOP_Y - 60 || y > list_bottom() - 20) continue;
        song_row(list[k], y, k == sel_page, numbered ? k + 1 : 0);
    }
    return result;
}

/* ---------- Now Playing ---------- */

static void now_playing(const Input *in) {
    unsigned int p = in->pressed;
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) { now_open = 0; return; }
    if (p & SCE_CTRL_CROSS) { if (music_playing()) video_pause(!video_paused()); else if (playing >= 0) start(playing); }
    if (p & SCE_CTRL_RIGHT) advance(1, 1);
    if (p & SCE_CTRL_LEFT) {
        if (video_pos_ms() > 3000 || qpos <= 0) video_seek(1); else advance(-1, 1);
    }
    if (p & SCE_CTRL_UP) video_seek(video_pos_ms() + 10000);
    if (p & SCE_CTRL_DOWN) { unsigned int ps = video_pos_ms(); video_seek(ps > 10000 ? ps - 10000 : 1); }
    if (p & SCE_CTRL_TRIANGLE) toggle_like(playing);
    if (p & SCE_CTRL_SQUARE) toggle_shuffle();
    if (in->tapped) {
        int tx = in->tap_x, ty = in->tap_y;
        if (ty > 360 && ty < 420) {
            if (tx > 330 && tx < 380) toggle_shuffle();
            else if (tx > 390 && tx < 440) advance(-1, 1);
            else if (tx > 450 && tx < 510) { if (music_playing()) video_pause(!video_paused()); }
            else if (tx > 520 && tx < 570) advance(1, 1);
            else if (tx > 580 && tx < 630) { repeat = (repeat + 1) % 3; ui_toast(repeat == 0 ? "Repeat off" : repeat == 1 ? "Repeat all" : "Repeat one", C_ACCENT); }
        }
        if (tx > 850 && ty > 120 && ty < 180) toggle_like(playing);
    }
    const Track *t = &tracks[playing];

    /* the backdrop: the cover, huge and dim, under a dark wash */
    vita2d_texture *c = cover(t);
    if (c) {
        float s = W * 1.2f / vita2d_texture_get_width(c);
        vita2d_draw_texture_tint_scale(c, -W * 0.1f, -H * 0.4f, s, s, RGBA8(255, 255, 255, 60));
    }
    draw_gradient(0, 65, W, H - 105, RGBA8(10, 12, 18, 150), RGBA8(10, 12, 18, 150), RGBA8(10, 12, 18, 245), RGBA8(10, 12, 18, 245));
    draw_soft(ui_glow(), 190, 230, 360, 360, (C_ACCENT & 0x00FFFFFF) | 0x30000000);
    draw_soft(ui_soft_shadow(), 190, 368, 250, 34, RGBA8(0, 0, 0, 160));
    draw_cover(t, 60, 100, 260, 1);

    text_fit(bold, 360, 150, C_TEXT, 30, t->title, 480);
    char sub[200];
    snprintf(sub, sizeof(sub), "%s%s%s", t->artist, is_single(t) ? "" : "  \xC2\xB7  ", is_single(t) ? "" : t->album);
    text_fit(font, 360, 180, C_DIM, 17, sub, 480);
    heart(880, 146, 30, liked[playing], liked[playing] ? RGBA8(255, 90, 110, 255) : C_DIM);

    unsigned int pos = music_playing() ? video_pos_ms() : 0, dur = t->dur ? t->dur : video_duration_ms();
    draw_round_rect(360, 300, 540, 5, 2, RGBA8(255, 255, 255, 40));
    float f = dur ? (float)pos / dur : 0;
    if (f > 1) f = 1;
    if (f > 0.01f) draw_round_rect(360, 300, 540 * f, 5, 2, C_ACCENT);
    vita2d_draw_fill_circle(360 + 540 * f, 302, 7, C_TEXT);
    char a[12], b[12];
    fmt_time(pos, a, sizeof(a)); fmt_time(dur, b, sizeof(b));
    text(font, 360, 330, C_FAINT, 14, a);
    text_right(font, 900, 330, C_FAINT, 14, b);

    unsigned int on = C_TEXT, off = C_FAINT;
    icon(I_SHUFFLE, 355, 390, 22, shuffle ? C_ACCENT : off);
    icon(I_PREV, 415, 390, 22, on);
    vita2d_draw_fill_circle(480, 390, 30, RGBA8(245, 245, 250, 255));
    icon(music_playing() && !video_paused() ? I_PAUSE : I_PLAY, 482, 390, 24, RGBA8(15, 15, 20, 255));
    icon(I_NEXT, 545, 390, 22, on);
    icon(I_REPEAT, 605, 390, 22, repeat ? C_ACCENT : off);
    if (repeat == 2) text(bold, 613, 380, C_ACCENT, 11, "1");

    text(bold, 690, 228, C_DIM, 13, "UP NEXT");
    for (int k = 1; k <= 3 && qpos + k < nqueue; ++k)
        text_fit(font, 690, 256 + (k - 1) * 30, C_TEXT, 14, tracks[queue[qpos + k]].title, 230);
    if (qpos + 1 >= nqueue) text(font, 690, 256, C_FAINT, 14, repeat == 1 ? "Back to the start" : "The end of the list");
}

/* The mini player at the bottom of every Music screen. */
static void mini_player(const Input *in) {
    if (playing < 0) return;
    const Track *t = &tracks[playing];
    int y = H - 40 - 64;
    if (in->tapped && in->tap_y >= y && in->tap_y < y + 64) {
        if (in->tap_x > W - 90) { if (music_playing()) video_pause(!video_paused()); else start(playing); }
        else now_open = 1;
    }
    vita2d_draw_rectangle(0, y, W, 64, RGBA8(24, 27, 38, 250));
    unsigned int pos = music_playing() ? video_pos_ms() : 0, dur = t->dur ? t->dur : video_duration_ms();
    vita2d_draw_rectangle(0, y, dur ? W * (float)pos / dur : 0, 2, C_ACCENT);
    draw_cover(t, 16, y + 9, 46, 1);
    text_fit(bold, 76, y + 29, C_TEXT, 16, t->title, 560);
    text_fit(font, 76, y + 49, C_DIM, 13, t->artist, 560);
    heart(W - 130, y + 30, 20, liked[playing], liked[playing] ? RGBA8(255, 90, 110, 255) : C_FAINT);
    icon(music_playing() && !video_paused() ? I_PAUSE : I_PLAY, W - 60, y + 32, 20, C_TEXT);
}

/* The footer's player, on every other tab: cover, title, previous, play or
 * pause, next, in [x, x + w] of the bottom bar. Returns 1 when the title was
 * tapped, to open Now Playing (playtest 2026-09-25). */
int music_bar(const Input *in, int x, int w) {
    if (playing < 0 || !music_playing()) return 0;
    const Track *t = &tracks[playing];
    int cy = H - 20, bx = x + w - 18;
    if (in->tapped && in->tap_y >= H - 40) {
        if (in->tap_x >= bx - 17 && in->tap_x < x + w) advance(1, 1);
        else if (in->tap_x >= bx - 51 && in->tap_x < bx - 17) video_pause(!video_paused());
        else if (in->tap_x >= bx - 85 && in->tap_x < bx - 51) advance(-1, 1);
        else if (in->tap_x >= x && in->tap_x < bx - 85) { now_open = 1; return 1; }
    }
    draw_round_rect(x - 6, H - 36, w + 6, 32, 10, RGBA8(255, 255, 255, 14));
    unsigned int pos = video_pos_ms(), dur = t->dur ? t->dur : video_duration_ms();
    if (dur) vita2d_draw_rectangle(x + 4, H - 6, (w - 14) * (float)pos / dur, 2, C_ACCENT);
    draw_cover(t, x, H - 33, 26, 1);
    text_fit(bold, x + 34, cy - 2, C_TEXT, 13, t->title, w - 34 - 100);
    text_fit(font, x + 34, cy + 11, C_DIM, 11, t->artist, w - 34 - 100);
    icon(I_PREV, bx - 68, cy, 8, C_TEXT);
    icon(music_playing() && !video_paused() ? I_PAUSE : I_PLAY, bx - 34, cy, 9, C_TEXT);
    icon(I_NEXT, bx, cy, 8, C_TEXT);
    return 0;
}

/* ---------- the Home section ---------- */

static void home_section(const Input *in) {
    unsigned int p = in->pressed;
    /* Rows: [Liked songs, Shuffle all, All songs], Listen again, Albums for you. */
    static int picks[12], npicks;
    if (!npicks && nalbums) for (int k = 0; k < 12 && k < nalbums; ++k) picks[npicks++] = rand() % nalbums;
    int counts[3] = {3, nrecent < 10 ? nrecent : 10, npicks};
    if (p & SCE_CTRL_DOWN) { int r = home_row + 1; while (r <= 2 && !counts[r]) r++; if (r <= 2) home_row = r; }
    if (p & SCE_CTRL_UP) { if (home_row == 0) chips = 1; else { int r = home_row - 1; while (r > 0 && !counts[r]) r--; home_row = r; } }
    int *col = &home_col[home_row];
    if (p & SCE_CTRL_LEFT && *col > 0) (*col)--;
    if (p & SCE_CTRL_RIGHT && *col < counts[home_row] - 1) (*col)++;
    if (*col >= counts[home_row]) *col = counts[home_row] - 1;
    if (*col < 0) *col = 0;
    int act = (p & SCE_CTRL_CROSS) != 0;
    if (p & SCE_CTRL_TRIANGLE && home_row == 1 && counts[1]) toggle_like(recent[*col]);

    /* The page scrolls: the rows run past the mini player (playtest
     * 2026-09-25: it would not scroll down). D-pad rows ease it; a swipe moves it. */
    static float vs, vt;
    int nrows = 0;
    for (int r = 1; r <= 2; ++r) if (counts[r]) nrows++;
    float bottom = H - 40 - (playing >= 0 ? 64 : 0) - 8, content = TOP_Y + 104 + nrows * 180;
    float vmax = content > bottom ? content - bottom : 0;
    if (p & (SCE_CTRL_UP | SCE_CTRL_DOWN)) vt = home_row == 0 ? 0 : home_row == 1 ? vmax * (counts[2] ? 0.5f : 1) : vmax;
    if (in->touching && in->drag_dy) vt = vs - in->drag_dy;
    Input below = *in;                                 /* a tap on the mini player is not a tap on a row under it */
    if (below.tapped && below.tap_y >= bottom) below.tapped = 0;
    in = &below;
    if (vt < 0) vt = 0;
    if (vt > vmax) vt = vmax;
    vs += (vt - vs) * (in->touching ? 1.0f : 0.25f);

    float y = TOP_Y - vs;
    const char *cards[3] = {"Liked songs", "Shuffle all", "All songs"};
    char subs[3][48];
    snprintf(subs[0], sizeof(subs[0]), "%d song%s", nliked, nliked == 1 ? "" : "s");
    snprintf(subs[1], sizeof(subs[1]), "%d songs, any order", ntracks);
    snprintf(subs[2], sizeof(subs[2]), "A to Z");
    static const unsigned int g[3][2] = {{0xEC4899, 0x7C3AED}, {0x2563EB, 0x06B6D4}, {0x059669, 0x84CC16}};
    for (int k = 0; k < 3; ++k) {
        float x = 40 + k * 300, w = 280, h = 78;
        int on = home_row == 0 && home_col[0] == k && !chips;
        if (in->tapped && in->tap_x >= x && in->tap_x < x + w && in->tap_y >= y && in->tap_y < y + h) { home_row = 0; home_col[0] = k; act = 1; }
        if (on) draw_focus(x, y, w, h, 1);
#define C_(c, a) RGBA8(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255, a)
        draw_round_gradient(x, y, w, h, ui_corner(w, h), C_(g[k][0], 255), C_(g[k][1], 255));
#undef C_
        if (k == 0) heart(x + 36, y + 38, 28, 1, RGBA8(255, 255, 255, 240));
        else icon(k == 1 ? I_SHUFFLE : I_PLAY, x + 36, y + 39, 22, RGBA8(255, 255, 255, 240));
        text(bold, (int)x + 64, (int)y + 36, RGBA8(255, 255, 255, 255), 18, cards[k]);
        text(font, (int)x + 64, (int)y + 58, RGBA8(255, 255, 255, 200), 13, subs[k]);
    }
    if (act && home_row == 0) {
        if (home_col[0] == 0) { section = S_LIKED; sel_page = -1; scroll_page = 0; }
        else if (home_col[0] == 1) { for (int i = 0; i < ntracks; ++i) list_buf[i] = i; play_list(list_buf, ntracks, -1, 1); }
        else { section = S_SONGS; sel_page = -1; scroll_page = 0; }
        return;
    }
    y += 104;
    const char *titles[3] = {"", "Listen again", "Albums for you"};
    for (int r = 1; r <= 2; ++r) {
        if (!counts[r]) continue;
        text(bold, 40, (int)y + 4, C_TEXT, 19, titles[r]);
        y += 16;
        float s = 118;
        float target = home_col[r] > 5 ? home_col[r] - 5 : 0;
        home_scroll[r] += (target - home_scroll[r]) * 0.25f;
        for (int k = 0; k < counts[r]; ++k) {
            float x = 40 + (k - home_scroll[r]) * (s + 22);
            if (x < -s || x > W) continue;
            const Track *t = r == 1 ? &tracks[recent[k]] : &tracks[albums[picks[k]].first];
            int on = home_row == r && home_col[r] == k && !chips;
            if (in->tapped && in->tap_x >= x && in->tap_x < x + s && in->tap_y >= y && in->tap_y < y + s) { home_row = r; home_col[r] = k; act = 1; }
            if (on) draw_focus(x, y, s, s, 1);
            draw_cover(t, x, y, s, 1);
            if (r == 1 && recent[k] == playing && music_playing()) { vita2d_draw_rectangle(x, y, s, s, RGBA8(0, 0, 0, 110)); eq_bars(x + s / 2 - 7, y + s / 2 + 8, C_TEXT); }
            text_fit(on ? bold : font, (int)x, (int)(y + s + 18), on ? C_TEXT : C_DIM, 14, r == 1 ? t->title : (is_single(t) ? t->artist : t->album), (int)s);
            text_fit(font, (int)x, (int)(y + s + 35), C_FAINT, 12, t->artist, (int)s);
        }
        if (act && home_row == r) {
            if (r == 1) play_list(recent, counts[1], home_col[1], 0);
            else { section = S_ALBUMS; page = P_ALBUM; page_ix = picks[home_col[2]]; sel_page = -1; scroll_page = 0; }
            return;
        }
        y += s + 46;
    }
}

/* ---------- the tab ---------- */

const char *music_hint(void) {
    if (now_open) return "X play/pause   <- -> previous/next   UP DOWN 10 s   /\\ like   [] shuffle   O back";
    if (chips) return "<- -> section   X open   L R tabs";
    if (section == S_HOME) return "X open   /\\ like   UP sections   START now playing   L R tabs";
    if ((section == S_ALBUMS || section == S_ARTISTS) && page == P_NONE) return "X open   UP sections   START now playing   L R tabs";
    return "X play  /\\ like  [] delete  O back  START now playing  L R tabs";
}

static void go_section(int s) { section = s; page = P_NONE; sel = 0; sel_page = -1; scroll_page = 0; }

static int handle_list(int r, const int *list, int n) {
    if (r == -2) play_list(list, n, 0, 0);
    else if (r == -3) play_list(list, n, -1, 1);
    else if (r >= 0) play_list(list, n, r, 0);
    return r;
}

void music_update(const Input *in) {
    if (!loaded) { text(font, 40, 140, C_DIM, 18, "Loading your music\xE2\x80\xA6"); return; }
    if (!ntracks) {
        text(bold, 40, 140, C_TEXT, 22, "No music yet");
        text(font, 40, 176, C_DIM, 18, "Run home/tools/import_music.py on the Mac: it copies the NAS library here.");
        return;
    }
    unsigned int p = in->pressed;
    if (p & SCE_CTRL_START && playing >= 0 && !now_open) { now_open = 1; return; }
    if (now_open && playing >= 0) {
        ui_theme_from(cover(&tracks[playing]));
        now_playing(in);
        return;
    }
    now_open = 0;

    /* the section chips */
    if (chips) {
        if (p & SCE_CTRL_LEFT && section > 0) go_section(section - 1);
        if (p & SCE_CTRL_RIGHT && section < NSECT - 1) go_section(section + 1);
        if (p & (SCE_CTRL_DOWN | SCE_CTRL_CROSS)) chips = 0;
        p = 0;
    }
    int cx = 40;
    for (int k = 0; k < NSECT; ++k) {                    /* taps now; drawn last, over the content */
        int w = text_w(font, 15, SECT[k]) + 30;
        if (in->tapped && in->tap_x >= cx && in->tap_x < cx + w && in->tap_y >= 74 && in->tap_y < 112) { go_section(k); chips = 0; p = 0; }
        cx += w + 10;
    }
    Input in2 = *in;
    in2.pressed = p;
    if (in2.tapped && in2.tap_y < 112) in2.tapped = 0;
    if (p & SCE_CTRL_CIRCLE && page != P_NONE) { page = P_NONE; sel_page = -1; scroll_page = 0; in2.pressed = p = 0; }

    if (section == S_HOME) {
        ui_theme_default();
        home_section(&in2);
    } else if (section == S_LIKED) {
        if (!nliked) {
            page_header("Liked songs", "Press \xE2\x96\xB3 on any song to like it", NULL, 1, -1);
            text(font, 40, TOP_Y + 220, C_DIM, 17, "Songs you like show up here, newest first.");
            if (p & SCE_CTRL_UP) chips = 1;
        } else {
            char sub[64];
            snprintf(sub, sizeof(sub), "Playlist  \xC2\xB7  %d song%s", nliked, nliked == 1 ? "" : "s");
            int was = sel_page;
            handle_list(list_page(&in2, liked_list, nliked, "Liked songs", sub, NULL, 1, 0), liked_list, nliked);
            if (was < 0 && (p & SCE_CTRL_UP)) chips = 1;
        }
    } else if (section == S_SONGS) {
        char sub[64];
        snprintf(sub, sizeof(sub), "%d songs, A to Z", ntracks);
        int was = sel_page;
        handle_list(list_page(&in2, by_title, ntracks, "Songs", sub, &tracks[by_title[0]], 0, 0), by_title, ntracks);
        if (was < 0 && (p & SCE_CTRL_UP)) chips = 1;
    } else if (section == S_ALBUMS && page == P_ALBUM) {
        const Album *a = &albums[page_ix];
        const Track *t0 = &tracks[a->first];
        int n = 0;
        for (int k = 0; k < a->count; ++k) list_buf[n++] = a->first + k;
        char sub[128];
        snprintf(sub, sizeof(sub), "Album  \xC2\xB7  %s  \xC2\xB7  %d song%s", t0->artist, n, n == 1 ? "" : "s");
        ui_theme_from(cover(t0));
        handle_list(list_page(&in2, list_buf, n, is_single(t0) ? "Singles" : t0->album, sub, t0, 0, 1), list_buf, n);
    } else if (section == S_ARTISTS && page == P_ARTIST) {
        const Artist *ar = &artists[page_ix];
        int n = 0;
        for (int k = 0; k < ar->count; ++k) list_buf[n++] = ar->first + k;
        char sub[64];
        snprintf(sub, sizeof(sub), "Artist  \xC2\xB7  %d song%s", n, n == 1 ? "" : "s");
        handle_list(list_page(&in2, list_buf, n, ar->name, sub, &tracks[ar->first], 0, 0), list_buf, n);
    } else if (section == S_ALBUMS || section == S_ARTISTS) {   /* a grid of albums, or of artists */
        int artists_view = section == S_ARTISTS, count = artists_view ? nartists : nalbums;
        const int COLS = 6;
        const float CW = 146, CH = artists_view ? 168 : 178, CS = artists_view ? 110 : 128;
        if (p & SCE_CTRL_LEFT) sel = sel > 0 ? sel - 1 : 0;
        if (p & SCE_CTRL_RIGHT) sel = sel < count - 1 ? sel + 1 : sel;
        if (p & SCE_CTRL_UP) { if (sel >= COLS) sel -= COLS; else chips = 1; }
        if (p & SCE_CTRL_DOWN) sel = sel + COLS < count ? sel + COLS : count - 1;
        static GridScroll gs[2];
        GridScroll *g = &gs[artists_view];
        grid_scroll(g, &sel, COLS, count, 2, CH, &in2);
        if (in2.tapped && in2.tap_y > TOP_Y && in2.tap_y < list_bottom()) {
            int c = (in2.tap_x - 40) / (int)CW, idx = (int)(g->top + (in2.tap_y - TOP_Y) / CH) * COLS + c;
            if (c >= 0 && c < COLS && idx >= 0 && idx < count) { if (idx == sel) p |= SCE_CTRL_CROSS; else sel = idx; }
        }
        if (p & SCE_CTRL_CROSS && count) { page = artists_view ? P_ARTIST : P_ALBUM; page_ix = sel; sel_page = -1; scroll_page = 0; return; }
        if (!artists_view && count) ui_theme_from(cover(&tracks[albums[sel].first]));
        for (int i = 0; i < count; ++i) {
            float y = TOP_Y + (i / COLS - g->top) * CH;
            if (y < TOP_Y - CH || y > list_bottom()) continue;
            float x = 40 + (i % COLS) * CW;
            int on = i == sel && !chips;
            if (!artists_view) {
                const Track *t = &tracks[albums[i].first];
                if (on) draw_focus(x, y, CS, CS, 1);
                draw_cover(t, x, y, CS, 1);
                text_fit(on ? bold : font, (int)x, (int)(y + CS + 18), on ? C_TEXT : C_DIM, 14, is_single(t) ? "Singles" : t->album, (int)CS);
                text_fit(font, (int)x, (int)(y + CS + 34), C_FAINT, 12, t->artist, (int)CS);
            } else {                                     /* round portraits, YouTube Music's artist look */
                const Artist *ar = &artists[i];
                float px = x + (CW - 18 - CS) / 2;
                if (on) vita2d_draw_fill_circle(px + CS / 2, y + CS / 2, CS / 2 + 5, C_ACCENT);
                vita2d_draw_fill_circle(px + CS / 2, y + CS / 2, CS / 2, RGBA8(40, 45, 60, 255));
                draw_made_cover(ar->name, "", px + CS * 0.15f, y + CS * 0.15f, CS * 0.7f, !on);
                text_fit(on ? bold : font, (int)x, (int)(y + CS + 22), on ? C_TEXT : C_DIM, 14, ar->name, (int)CW - 16);
                char n[24];
                snprintf(n, sizeof(n), "%d song%s", ar->count, ar->count == 1 ? "" : "s");
                text(font, (int)x, (int)(y + CS + 38), C_FAINT, 12, n);
            }
        }
    }
    /* the chips, on a band of background, so rows scrolled up pass under them */
    draw_gradient(0, 65, W, 50, C_BG, C_BG, C_BG, (C_BG & 0x00FFFFFF) | 0xE0000000);
    cx = 40;
    for (int k = 0; k < NSECT; ++k) {
        int w = text_w(font, 15, SECT[k]) + 30;
        if (chips && k == section) draw_focus(cx, 78, w, 30, 1);
        draw_round_rect(cx, 78, w, 30, 15, k == section ? RGBA8(245, 245, 250, 255) : RGBA8(255, 255, 255, 26));
        text(font, cx + 15, 99, k == section ? RGBA8(15, 15, 20, 255) : C_TEXT, 15, SECT[k]);
        cx += w + 10;
    }
    mini_player(&in2);
}
