/* Global search: SELECT from any tab. One field, Home's own keyboard (touch
 * or buttons, no Sony dialog), suggestions as you type, and results from
 * every corner at once: games, apps, films, music and settings. Drawn over a
 * blur of the screen you came from, like the PS5's. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>

#include "search.h"
#include "sfx.h"

#define MAXQ 40
#define MAXHITS 24

static int active, pending, fade, dirty;      /* dirty: frames since the last key, results not yet redone */
static char q[MAXQ + 1];
static Hit hits[MAXHITS];
static int nhits, more_games;
static char suggestions[4][48];
static int nsugg;

/* Focus: the keyboard (row, col), the suggestion chips, or the results. */
enum { Z_KEYS, Z_CHIPS, Z_RESULTS };
static int zone, kr, kc, chip, rsel;
static float rtop;

static const char *const rows[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.&"};
enum { K_SPACE, K_BACK, K_CLEAR, K_DONE };     /* the fifth row */
static const char *const special[4] = {"space", "\xE2\x8C\xAB", "clear", "results"};

#define KX 40
#define KY 176
#define KW 44
#define KH 52
#define KG 5

static const struct { const char *name, *where; } settings_words[] = {
    {"Brightness", "Settings \xC2\xB7 Display & sound"}, {"Volume", "Settings \xC2\xB7 Display & sound"},
    {"UI sounds", "Settings \xC2\xB7 Display & sound"}, {"Home music", "Settings \xC2\xB7 Display & sound"},
    {"Battery", "Settings \xC2\xB7 Power"}, {"Wi-Fi network", "Settings \xC2\xB7 Network"},
    {"Storage", "Settings \xC2\xB7 Storage"}, {"Clock speed", "Settings \xC2\xB7 Performance"},
    {"Restart", "Settings \xC2\xB7 System"}, {"Sleep", "Settings \xC2\xB7 System"},
    {"System home (bubbles)", "Settings \xC2\xB7 System"},
};

int match_score(const char *title, const char *needle) {
    if (!title || !*title || !needle || !*needle) return 0;
    int best = 0;
    for (const char *h = title; *h; ++h) {
        const char *a = h, *b = needle;
        while (*a && *b && ((*a | 32) == (*b | 32))) { ++a; ++b; }
        if (*b) continue;
        int sc = h == title ? 3 : (h[-1] == ' ' || h[-1] == '-' || h[-1] == ':' || h[-1] == '(') ? 2 : 1;
        if (sc > best) best = sc;
        if (best == 3) break;
    }
    return best;
}

static void add(Hit *list, int n) {
    for (int i = 0; i < n && nhits < MAXHITS; ++i) hits[nhits++] = list[i];
}

/* Everything that matches, best first; within a score, games, films, music,
 * apps, then settings. Suggestions are the distinct best titles. */
static void free_art(void) {}                  /* the image cache owns the pictures */

static void load_one_art(int first, int last) {
    for (int i = first; i <= last && i < nhits; ++i)
        if (hits[i].path[0]) hits[i].art = ui_image(hits[i].path);
}

static void refresh(void) {
    free_art();
    nhits = more_games = nsugg = 0;
    rsel = 0; rtop = 0;
    if (!*q) return;
    static Hit g[200], a[8], f[8], m[8], st[4];
    int ng = play_find(q, g, 200), na = apps_find(q, a, 4), nf = movies_find(q, f, 4), nm = music_find(q, m, 4), ns = 0;
    for (unsigned int i = 0; i < sizeof(settings_words) / sizeof(settings_words[0]) && ns < 3; ++i) {
        int sc = match_score(settings_words[i].name, q);
        if (sc) st[ns++] = (Hit){H_SETTING, (int)i, 0, sc, settings_words[i].name, settings_words[i].where, NULL, "", 0};
    }
    int cap = na + nf + nm + ns ? 4 : 8;              /* leave room for films, music, apps */
    int shown = ng < cap ? ng : cap;
    more_games = ng > shown ? ng : 0;
    for (int sc = 3; sc >= 1; --sc) {
        Hit tmp[8];
        int k;
        k = 0; for (int i = 0; i < shown; ++i) if (g[i].score == sc) tmp[k++] = g[i]; add(tmp, k);
        k = 0; for (int i = 0; i < nf; ++i) if (f[i].score == sc) tmp[k++] = f[i]; add(tmp, k);
        k = 0; for (int i = 0; i < nm; ++i) if (m[i].score == sc) tmp[k++] = m[i]; add(tmp, k);
        k = 0; for (int i = 0; i < na; ++i) if (a[i].score == sc) tmp[k++] = a[i]; add(tmp, k);
        k = 0; for (int i = 0; i < ns; ++i) if (st[i].score == sc) tmp[k++] = st[i]; add(tmp, k);
    }
    for (int i = 0; i < nhits && nsugg < 4; ++i) {
        int dup = 0;
        for (int j = 0; j < nsugg; ++j) if (!strcasecmp(suggestions[j], hits[i].title)) dup = 1;
        if (!dup && hits[i].kind != H_SETTING) snprintf(suggestions[nsugg++], sizeof(suggestions[0]), "%s", hits[i].title);
    }
}

void search_open(void) { if (!active) pending = 1; }
int search_active(void) { return active || pending; }
void search_close(void) { active = pending = 0; }

/* Called by main between frames: the blur needs a finished frame. */
void search_prepare(void) {
    if (!pending) return;
    pending = 0;
    ui_blur_capture();
    active = 1;
    fade = 0;
    zone = Z_KEYS; kr = 1; kc = 0;
    refresh();
}

static void type_key(int r, int c) {
    int len = strlen(q);
    if (r < 4) {
        if (len < MAXQ) { q[len] = rows[r][c]; q[len + 1] = 0; }
    } else if (c == K_SPACE) { if (len && len < MAXQ && q[len - 1] != ' ') { q[len] = ' '; q[len + 1] = 0; } }
    else if (c == K_BACK) { if (len) q[len - 1] = 0; }
    else if (c == K_CLEAR) q[0] = 0;
    else if (c == K_DONE) { if (dirty) { refresh(); dirty = 0; } if (nhits || more_games) { zone = Z_RESULTS; rsel = 0; } return; }
    dirty = 1;                                   /* redo the results once typing pauses */
}

static int open_hit(int i) {
    if (i == nhits) { play_show_all(q); active = 0; return SEARCH_TO_PLAY; }   /* "see all games" */
    Hit *h = &hits[i];
    active = 0;
    switch (h->kind) {
    case H_GAME: play_open_hit(h); return SEARCH_TO_PLAY;
    case H_APP: apps_open_hit(h); return -1;
    case H_FILM: movies_open_hit(h); return SEARCH_TO_MOVIES;
    case H_ALBUM: music_open_hit(h); return SEARCH_TO_MUSIC;
    default: return SEARCH_TO_SETTINGS;
    }
}

static void key_rect(int r, int c, int *x, int *y, int *w) {
    *y = KY + r * (KH + KG);
    if (r < 4) { *x = KX + c * (KW + KG) + (r == 2 ? 12 : r == 3 ? 24 : 0); *w = KW; return; }
    static const int x5[4] = {0, 205, 290, 375}, w5[4] = {200, 80, 80, 110};   /* space, delete, clear, results */
    *x = KX + x5[c];
    *w = w5[c];
}

int search_update(const Input *in) {
    if (!active) return -1;
    if (fade < 12) fade++;
    if (dirty && ++dirty > 9) { refresh(); dirty = 0; }   /* ~150 ms after the last key */
    int total = nhits + (more_games ? 1 : 0);

    /* ---- input ---- */
    unsigned int p = in->pressed;
    if (p & SCE_CTRL_SELECT) { active = 0; free_art(); return -1; }
    if (p & SCE_CTRL_CIRCLE) {
        if (zone == Z_RESULTS || zone == Z_CHIPS) zone = Z_KEYS;
        else if (*q) { q[strlen(q) - 1] = 0; dirty = 1; }    /* O on the keys: delete, like a phone */
        else { active = 0; return -1; }
    }
    if (p & SCE_CTRL_SQUARE) type_key(4, K_BACK);
    if (p & SCE_CTRL_TRIANGLE) type_key(4, K_SPACE);
    if (p & SCE_CTRL_START && total) { zone = Z_RESULTS; rsel = 0; }
    if (zone == Z_KEYS) {
        int cols = kr < 4 ? 10 : 4;
        if (p & SCE_CTRL_LEFT) kc = kc > 0 ? kc - 1 : 0;
        if (p & SCE_CTRL_RIGHT) {
            if (kc < cols - 1) kc++;
            else if (total) { zone = Z_RESULTS; rsel = rsel < total ? rsel : 0; }
        }
        if (p & SCE_CTRL_UP) {
            if (kr > 0) { kr--; if (kr < 4 && kc > 9) kc = 9; if (kr == 3 && cols == 4) kc = kc * 3; }
            else if (nsugg) { zone = Z_CHIPS; chip = 0; }
        }
        if (p & SCE_CTRL_DOWN && kr < 4) { kr++; if (kr == 4) kc = kc < 2 ? K_SPACE : kc < 4 ? K_BACK : kc < 6 ? K_CLEAR : K_DONE; }
        if (p & SCE_CTRL_CROSS) type_key(kr, kc);
    } else if (zone == Z_CHIPS) {
        if (p & SCE_CTRL_LEFT) chip = chip > 0 ? chip - 1 : 0;
        if (p & SCE_CTRL_RIGHT) chip = chip < nsugg - 1 ? chip + 1 : chip;
        if (p & SCE_CTRL_DOWN) zone = Z_KEYS;
        if (p & SCE_CTRL_CROSS && chip < nsugg) {
            snprintf(q, sizeof(q), "%.*s", MAXQ, suggestions[chip]);
            refresh();
            zone = total ? Z_RESULTS : Z_KEYS;
        }
    } else {
        if (p & SCE_CTRL_UP) rsel = rsel > 0 ? rsel - 1 : 0;
        if (p & SCE_CTRL_DOWN) rsel = rsel < total - 1 ? rsel + 1 : rsel;
        if (p & SCE_CTRL_LEFT) zone = Z_KEYS;
        if (p & SCE_CTRL_CROSS && rsel < total) return open_hit(rsel);
    }
    if (in->tapped) {
        int tx = in->tap_x, ty = in->tap_y;
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < (r < 4 ? 10 : 4); ++c) {
                int x, y, w;
                key_rect(r, c, &x, &y, &w);
                if (tx >= x && tx < x + w && ty >= y && ty < y + KH) { zone = Z_KEYS; kr = r; kc = c; type_key(r, c); sfx_play(SFX_MOVE); }
            }
        if (ty > 150 && tx > 540) {
            int i = (int)(rtop + (ty - 160) / 48.0f);
            if (i >= 0 && i < total) return open_hit(i);
        }
        int cx = 40;
        for (int i = 0; i < nsugg; ++i) {
            int w = text_w(font, 15, suggestions[i]) + 28;
            if (w > 220) w = 220;
            if (tx >= cx && tx < cx + w && ty > 104 && ty < 140) {
                snprintf(q, sizeof(q), "%.*s", MAXQ, suggestions[i]);
                refresh();
            }
            cx += w + 10;
        }
        if (ty < 90 && tx > W - 120) { active = 0; return -1; }   /* "Close" */
    }

    /* ---- drawing ---- */
    float a = fade / 12.0f;
    ui_blur_draw();
    vita2d_draw_rectangle(0, 0, W, H, RGBA8(10, 12, 18, (int)(120 * a)));
    unsigned int acc = C_ACCENT;

    vita2d_draw_rectangle(40, 30, W - 180, 58, RGBA8(30, 34, 46, 235));
    vita2d_draw_rectangle(40, 86, W - 180, 2, acc);
    if (*q) {
        text_fit(bold, 60, 70, C_TEXT, 26, q, W - 240);
        int cw = text_w(bold, 26, q);
        if (cw > W - 240) cw = W - 240;
        if ((int)(ui_pulse() * 2)) vita2d_draw_rectangle(62 + cw, 44, 2, 30, acc);   /* caret */
    } else {
        text(font, 60, 70, C_FAINT, 22, "Search games, films, music, apps and settings");
    }
    text(font, W - 118, 66, C_DIM, 16, "SELECT close");

    int cx = 40;
    for (int i = 0; i < nsugg; ++i) {                          /* suggestion chips */
        int w = text_w(font, 15, suggestions[i]) + 28;
        if (w > 220) w = 220;
        int on = zone == Z_CHIPS && chip == i;
        vita2d_draw_rectangle(cx, 106, w, 32, on ? acc : RGBA8(40, 45, 60, 220));
        text_fit(font, cx + 14, 128, on ? RGBA8(10, 12, 18, 255) : C_TEXT, 15, suggestions[i], w - 28);
        cx += w + 10;
    }

    for (int r = 0; r < 5; ++r)                                /* the keyboard */
        for (int c = 0; c < (r < 4 ? 10 : 4); ++c) {
            int x, y, w;
            key_rect(r, c, &x, &y, &w);
            int on = zone == Z_KEYS && kr == r && kc == c;
            if (on) draw_focus(x, y, w, KH, 1);
            vita2d_draw_rectangle(x, y, w, KH, on ? RGBA8(52, 58, 78, 255) : RGBA8(32, 36, 48, 230));
            char label[4] = {0};
            const char *s = r < 4 ? (label[0] = rows[r][c], label) : special[c];
            int size = r < 4 ? 22 : 15;
            int lw = text_w(r < 4 ? bold : font, size, s);
            text(r < 4 ? bold : font, x + (w - lw) / 2, y + KH / 2 + size / 3, on ? C_TEXT : C_DIM, size, s);
        }
    draw_hints(KX, KY + 5 * (KH + KG) + 14, "X type  [] delete  /\\ space  START results  O back", C_FAINT, W);

    /* results */
    int rx = 548, ry = 160;
    if (!*q) text(font, rx, ry + 24, C_FAINT, 17, "Start typing: results appear here.");
    else if (!total) text(font, rx, ry + 24, C_DIM, 17, "Nothing matches that yet.");
    load_one_art((int)rtop, (int)rtop + 7);
    float target = rsel > rtop + 6 ? rsel - 6 : rsel < rtop ? rsel : rtop;
    rtop += (target - rtop) * 0.3f;
    static const char *const kinds[] = {"GAME", "APP", "FILM", "MUSIC", "SETTING"};
    for (int i = 0; i < total; ++i) {
        float y = ry + (i - rtop) * 48;
        if (y < ry - 4 || y > H - 60) continue;
        int on = zone == Z_RESULTS && rsel == i;
        if (on) vita2d_draw_rectangle(rx - 8, y, W - rx - 24, 44, RGBA8(52, 58, 78, 240));
        if (on) vita2d_draw_rectangle(rx - 8, y, 3, 44, acc);
        if (i == nhits) {
            char all[64];
            snprintf(all, sizeof(all), "See all %d games in Play", more_games);
            text(bold, rx + 48, y + 28, acc, 16, all);
            continue;
        }
        Hit *h = &hits[i];
        if (h->art) {
            float tw = vita2d_texture_get_width(h->art), th = vita2d_texture_get_height(h->art), sc = 36 / (tw > th ? tw : th);
            vita2d_draw_texture_scale(h->art, rx + (36 - tw * sc) / 2, y + 4 + (36 - th * sc) / 2, sc, sc);
        } else vita2d_draw_rectangle(rx, y + 4, 36, 36, RGBA8(40, 45, 60, 255));
        text_fit(on ? bold : font, rx + 48, y + 20, C_TEXT, 16, h->title, W - rx - 150);
        text_fit(font, rx + 48, y + 38, C_FAINT, 13, h->sub, W - rx - 150);
        text_right(font, W - 40, y + 26, C_FAINT, 12, kinds[h->kind]);
    }
    return -1;
}
