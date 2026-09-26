/* Play: the game library (Arcade Hub's shelves, previews and attract reel),
 * as one tab of Home. Home owns the frame, the tab bar and input; this module
 * draws the content between them. */
/* Play: every game on the card, by console and by collection.
 * Reads ux0:data/arcadehub/ (built on the Mac by build_catalog.py):
 *   systems.tsv          id  name  accent_hex
 *   games/<id>.tsv       title kind a1 a2 cover bg logo video year genre players desc
 * Launch kinds and URIs: see ../LAUNCHING.md and ../DESIGN.md. */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <psp2/power.h>
#include <vita2d.h>
#include <taihen.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/io/dirent.h>
#include <time.h>
#include "video.h"
#include "ui.h"
#include "play.h"
#include "store.h"
#include "sfx.h"
#include "search.h"

/* vita-elf-create refuses layouts where its SCE data would not fit at the end
 * of segment 0 ("segment 1 overlaps"); a little slack in .data moves it on. */
__attribute__((used)) static const volatile char elf_slack[8192] = {1};

#define ROOT "ux0:data/arcadehub/"
#define MAX_SYS 48
#define CACHE 24

typedef struct {
    char *title, *kind, *a1, *a2, *cover, *bg, *logo, *video, *year, *genre, *players, *desc;
    int origin;  /* index of the real system this game belongs to */
} Game;

typedef struct {
    char id[32], name[64];
    unsigned int accent;
    Game *games;
    int count;
} System;

static System systems[MAX_SYS];
static int nsys;

/* Slots 0..VIRT-1 are lists built from the real systems after them. They are
 * always present (possibly empty) so system indexes never shift. */
#define VIRT 4
enum { V_RECENT, V_FAV, V_MULTI, V_SEARCH };
#define MAX_COLL 14     /* smart shelves (genre, decade, unplayed, short sessions), after the real systems */
#define MAX_SEARCH 200
#define USER ROOT "user/"
#define MAX_RECENT 24
#define MAX_FAV 512
#define MAX_HIDDEN 1024

/* ---------- small helpers ---------- */

static char *slurp(const char *path, int *len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    char *buf = malloc(size + 1);
    int n = buf ? sceIoRead(fd, buf, size) : -1;
    sceIoClose(fd);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static int exists(const char *p) {
    SceIoStat st;
    return p && *p && sceIoGetstat(p, &st) >= 0;
}

/* Splits a line in place on tabs; returns field count. "\n" escapes become newlines. */
static int split(char *line, char **out, int max) {
    int n = 0;
    out[n++] = line;
    for (char *c = line; *c && n < max; ++c)
        if (*c == '\t') { *c = 0; out[n++] = c + 1; }
    for (int i = 0; i < n; ++i) {
        char *r = out[i], *w = out[i];
        while (*r) {
            if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r += 2; }
            else *w++ = *r++;
        }
        *w = 0;
    }
    return n;
}

static unsigned int hexcolor(const char *s) {
    unsigned int v = strtoul(s, NULL, 16);
    return RGBA8((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
}

static void load_catalog(void) {
    char *text = slurp(ROOT "systems.tsv", NULL);
    if (!text) return;
    nsys = VIRT;
    for (char *line = strtok(text, "\r\n"); line && nsys < MAX_SYS; line = strtok(NULL, "\r\n")) {
        char *f[4];
        if (split(line, f, 4) < 3) continue;
        System *s = &systems[nsys];
        snprintf(s->id, sizeof(s->id), "%s", f[0]);
        snprintf(s->name, sizeof(s->name), "%s", f[1]);
        s->accent = hexcolor(f[2]);
        char path[256];
        snprintf(path, sizeof(path), ROOT "games/%s.tsv", s->id);
        int len = 0;
        char *g = slurp(path, &len);
        if (!g) continue;
        int lines = 1;
        for (int i = 0; i < len; ++i) lines += g[i] == '\n';
        s->games = calloc(lines, sizeof(Game));
        char *save = NULL;
        for (char *gl = strtok_r(g, "\n", &save); gl; gl = strtok_r(NULL, "\n", &save)) {
            char *x[12] = {0};
            int n = split(gl, x, 12);
            if (n < 3) continue;
            static char empty[] = "";
            for (int i = n; i < 12; ++i) x[i] = empty;
            Game *gm = &s->games[s->count++];
            gm->origin = nsys;
            gm->title = x[0]; gm->kind = x[1]; gm->a1 = x[2]; gm->a2 = x[3];
            gm->cover = x[4]; gm->bg = x[5]; gm->logo = x[6]; gm->video = x[7];
            gm->year = x[8]; gm->genre = x[9]; gm->players = x[10]; gm->desc = x[11];
        }
        if (s->count) nsys++;
    }
}

/* ---------- user lists: Continue, Favourites, Multiplayer ---------- */

typedef struct { char sys[32]; char title[160]; } Ref;
static Ref recent[MAX_RECENT], favs[MAX_FAV], hidden[MAX_HIDDEN];
static int nrecent, nfav, nhidden;
static Ref plays[MAX_RECENT * 8];
static int nplays, play_count[MAX_RECENT * 8];
static int first_real = VIRT;   /* systems[first_real..last_real) are the real ones; */
static int last_real;           /* genre collections follow them (Home, 2026-09-24:
                                 * consoles come right after the lists now) */
static void rebuild_derived(void);  /* refresh every derived list after a change */
static int sort_mode;           /* 0 name, 1 year, 2 most played */
static char search_text[64];

static int ref_is(const Ref *r, const Game *g) {
    return !strcmp(r->sys, systems[g->origin].id) && !strncmp(r->title, g->title, sizeof(r->title) - 1);
}

static void ref_set(Ref *r, const Game *g) {
    snprintf(r->sys, sizeof(r->sys), "%s", systems[g->origin].id);
    snprintf(r->title, sizeof(r->title), "%s", g->title);
}

static int load_refs(const char *path, Ref *out, int max) {
    char *text = slurp(path, NULL);
    int n = 0;
    if (!text) return 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line && n < max; line = strtok_r(NULL, "\r\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        snprintf(out[n].sys, sizeof(out[n].sys), "%s", line);
        snprintf(out[n].title, sizeof(out[n].title), "%s", tab + 1);
        n++;
    }
    free(text);
    return n;
}

/* Saves go to the background writer, except around a game launch: Home is
 * about to close then, and a queued write could be lost with it. */
static int launching;
static void write_file(const char *path, const char *buf, int n) {
    if (!launching) { ui_save(path, buf, n, 0); return; }
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, buf, n); sceIoClose(fd); }
}

static char save_buf[64 * 1024];

static void save_refs(const char *path, const Ref *r, int n) {
    int len = 0;
    for (int i = 0; i < n && len < (int)sizeof(save_buf) - 200; ++i)
        len += snprintf(save_buf + len, sizeof(save_buf) - len, "%s\t%s\n", r[i].sys, r[i].title);
    write_file(path, save_buf, len);
}

static const Game *find_ref(const Ref *r) {
    for (int s = first_real; s < last_real; ++s) {
        if (strcmp(systems[s].id, r->sys)) continue;
        for (int i = 0; i < systems[s].count; ++i)
            if (ref_is(r, &systems[s].games[i])) return &systems[s].games[i];
    }
    return NULL;
}

static void fill_list(int v, const Ref *r, int n) {
    System *s = &systems[v];
    s->count = 0;
    for (int i = 0; i < n; ++i) {
        const Game *g = find_ref(&r[i]);
        if (g) s->games[s->count++] = *g;
    }
}

static int is_fav(const Game *g) {
    for (int i = 0; i < nfav; ++i) if (ref_is(&favs[i], g)) return 1;
    return 0;
}

static int plays_of(const Game *g) {
    for (int i = 0; i < nplays; ++i) if (ref_is(&plays[i], g)) return play_count[i];
    return 0;
}

static void load_plays(void) {
    char *text = slurp(USER "plays.tsv", NULL);
    if (!text) return;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line && nplays < (int)(sizeof(plays) / sizeof(plays[0]));
         line = strtok_r(NULL, "\r\n", &save)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (t2) *t2 = 0;
        snprintf(plays[nplays].sys, sizeof(plays[0].sys), "%s", line);
        snprintf(plays[nplays].title, sizeof(plays[0].title), "%s", t1 + 1);
        play_count[nplays] = t2 ? atoi(t2 + 1) : 1;
        nplays++;
    }
    free(text);
}

static void save_plays(void) {
    int len = 0;
    for (int i = 0; i < nplays && len < (int)sizeof(save_buf) - 240; ++i)
        len += snprintf(save_buf + len, sizeof(save_buf) - len, "%s\t%s\t%d\n", plays[i].sys, plays[i].title, play_count[i]);
    write_file(USER "plays.tsv", save_buf, len);
}

static void count_play(const Game *g) {
    for (int i = 0; i < nplays; ++i)
        if (ref_is(&plays[i], g)) { play_count[i]++; save_plays(); return; }
    if (nplays >= (int)(sizeof(plays) / sizeof(plays[0]))) return;
    ref_set(&plays[nplays], g);
    play_count[nplays++] = 1;
    save_plays();
}

/* ---------- play time ---------- */

/* stats.tsv: system, title, last played (unix), seconds played. A launch
 * writes session.tsv; when Home starts again (the game has closed) the
 * difference is the session, so the count is close without hooking games. */
#define MAX_STATS 512
static struct { Ref r; long last; long secs; } stats[MAX_STATS];
static int nstats;

static int stat_of(const Game *g, int make) {
    for (int i = 0; i < nstats; ++i) if (ref_is(&stats[i].r, g)) return i;
    if (!make || nstats == MAX_STATS) return -1;
    ref_set(&stats[nstats].r, g);
    stats[nstats].last = stats[nstats].secs = 0;
    return nstats++;
}

static void save_stats(void) {
    int len = 0;
    for (int i = 0; i < nstats && len < (int)sizeof(save_buf) - 260; ++i)
        len += snprintf(save_buf + len, sizeof(save_buf) - len, "%s\t%s\t%ld\t%ld\n", stats[i].r.sys, stats[i].r.title, stats[i].last, stats[i].secs);
    write_file(USER "stats.tsv", save_buf, len);
}

static void load_stats(void) {
    static char buf[64 * 1024];
    SceUID fd = sceIoOpen(USER "stats.tsv", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        buf[n > 0 ? n : 0] = 0;
        for (char *line = strtok(buf, "\n"); line && nstats < MAX_STATS; line = strtok(NULL, "\n")) {
            char *f[4] = {line, 0, 0, 0};
            for (int k = 1; k < 4 && f[k - 1]; ++k) { f[k] = strchr(f[k - 1], '\t'); if (f[k]) *f[k]++ = 0; }
            if (!f[3]) continue;
            snprintf(stats[nstats].r.sys, sizeof(stats[0].r.sys), "%s", f[0]);
            snprintf(stats[nstats].r.title, sizeof(stats[0].r.title), "%s", f[1]);
            stats[nstats].last = atol(f[2]);
            stats[nstats++].secs = atol(f[3]);
        }
    }
    /* A session left by the last launch: count it now that we are back. */
    char sess[260] = {0};
    fd = sceIoOpen(USER "session.tsv", SCE_O_RDONLY, 0);
    if (fd < 0) return;
    int n = sceIoRead(fd, sess, sizeof(sess) - 1);
    sceIoClose(fd);
    sceIoRemove(USER "session.tsv");
    if (n <= 0) return;
    char *t1 = strchr(sess, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (!t2) return;
    *t1 = *t2 = 0;
    long start = atol(t2 + 1), dur = (long)time(NULL) - start;
    if (dur < 20 || dur > 8 * 3600) return;          /* a cancelled launch, or a console left overnight */
    for (int i = 0; i < nstats; ++i)
        if (!strcmp(stats[i].r.sys, sess) && !strcmp(stats[i].r.title, t1 + 1)) { stats[i].secs += dur; break; }
    save_stats();
}

static void note_session(const Game *g) {
    int i = stat_of(g, 1);
    if (i < 0) return;
    stats[i].last = (long)time(NULL);
    save_stats();
    SceUID fd = sceIoOpen(USER "session.tsv", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    char line[260];
    int n = snprintf(line, sizeof(line), "%s\t%s\t%ld", stats[i].r.sys, stats[i].r.title, stats[i].last);
    sceIoWrite(fd, line, n);
    sceIoClose(fd);
}

/* Most recent first, no duplicates. Runs right after a launch: written now. */
static void note_played(const Game *g) {
    launching = 1;
    note_session(g);
    int at = nrecent < MAX_RECENT ? nrecent : MAX_RECENT - 1;
    for (int i = 0; i < nrecent; ++i) if (ref_is(&recent[i], g)) { at = i; break; }
    memmove(&recent[1], &recent[0], at * sizeof(Ref));
    ref_set(&recent[0], g);
    if (at == nrecent) nrecent++;
    save_refs(USER "recent.tsv", recent, nrecent);
    fill_list(V_RECENT, recent, nrecent);
    count_play(g);
    launching = 0;
}

/* Returns 1 if now a favourite. */
static int toggle_fav(const Game *g) {
    Game copy = *g;  /* g may point into the Favourites list being rebuilt */
    for (int i = 0; i < nfav; ++i) {
        if (!ref_is(&favs[i], &copy)) continue;
        memmove(&favs[i], &favs[i + 1], (nfav - i - 1) * sizeof(Ref));
        nfav--;
        save_refs(USER "favourites.tsv", favs, nfav);
        fill_list(V_FAV, favs, nfav);
        return 0;
    }
    if (nfav >= MAX_FAV) return 0;
    memmove(&favs[1], &favs[0], nfav * sizeof(Ref));  /* newest first */
    ref_set(&favs[0], &copy);
    nfav++;
    save_refs(USER "favourites.tsv", favs, nfav);
    fill_list(V_FAV, favs, nfav);
    return 1;
}

static int max_players(const char *p) {
    int best = 0;
    while (p && *p) {
        if (*p >= '0' && *p <= '9') { int v = atoi(p); if (v > best) best = v; while (*p >= '0' && *p <= '9') ++p; }
        else ++p;
    }
    return best;
}

/* Rebuilt whenever the real systems change (a game is removed). */
static void fill_multi(void) {
    systems[V_MULTI].count = 0;
    for (int s = first_real; s < last_real; ++s)
        for (int i = 0; i < systems[s].count; ++i)
            if (max_players(systems[s].games[i].players) >= 2)
                systems[V_MULTI].games[systems[V_MULTI].count++] = systems[s].games[i];
}

/* Drops every game listed in hidden.tsv from its real system. Runs before the
 * lists are built, so Continue/Favourites/Multiplayer never resolve to one. */
static void apply_hidden(void) {
    for (int s = first_real; s < last_real; ++s) {
        System *sy = &systems[s];
        int w = 0;
        for (int i = 0; i < sy->count; ++i) {
            int drop = 0;
            for (int h = 0; h < nhidden && !drop; ++h) drop = ref_is(&hidden[h], &sy->games[i]);
            if (!drop) sy->games[w++] = sy->games[i];
        }
        sy->count = w;
    }
}

/* The ROM path the launcher would use, or NULL when there is nothing to delete
 * (installed apps and ScummVM folders are left alone). */
static const char *rom_path(const Game *g) {
    if (!strcmp(g->kind, "ra")) return g->a2;
    if (!strcmp(g->kind, "n64") || !strcmp(g->kind, "psp")) return g->a1;
    return NULL;
}

/* Remove from the menu for good; with delete_rom, erase the ROM too. */
static void hide_game(int sys, int sel, int delete_rom) {
    System *sy = &systems[sys];
    Game g = sy->games[sel];
    if (nhidden < MAX_HIDDEN) {
        ref_set(&hidden[nhidden], &g);
        nhidden++;
        save_refs(USER "hidden.tsv", hidden, nhidden);
    }
    if (delete_rom) {
        const char *rom = rom_path(&g);
        if (rom && *rom) sceIoRemove(rom);
        else if (!strcmp(g.kind, "app")) store_uninstall(g.a1, g.title);   /* an installed Vita game */
    }
    /* drop it from its real system, then rebuild every list from those */
    System *real = &systems[g.origin];
    for (int i = 0; i < real->count; ++i) {
        if (strcmp(real->games[i].title, g.title)) continue;
        memmove(&real->games[i], &real->games[i + 1], (real->count - i - 1) * sizeof(Game));
        real->count--;
        break;
    }
    rebuild_derived();
}

/* Genre collections, built from the scraped genre text. They sit between the
 * fixed lists and the real systems; real systems shift up by as many as exist,
 * so every Game.origin is fixed up at the same time. */
/* Smart shelves. kind 0: genre words; 1: release years lo..hi; 2: never
 * played; 3: short sessions (quick-play genres a handheld suits). */
static const struct { const char *name; const char *match[3]; unsigned int accent; int kind, lo, hi; } COLL[] = {
    {"Short sessions", {"puzzle", "shoot", "fight"}, 0x38BDF8, 3, 0, 0},
    {"Never played", {NULL, NULL, NULL},    0x94A3B8, 2, 0, 0},
    {"Shooters",    {"shoot", "shmup", NULL},   0xEF4444, 0, 0, 0},
    {"Platformers", {"platform", NULL, NULL},   0x22C55E, 0, 0, 0},
    {"Puzzle",      {"puzzle", NULL, NULL},     0x8B5CF6, 0, 0, 0},
    {"Racing",      {"racing", "driving", NULL},0xF59E0B, 0, 0, 0},
    {"Fighting",    {"fight", "beat'", NULL},   0xE11D48, 0, 0, 0},
    {"RPGs",        {"role", "rpg", NULL},      0x06B6D4, 0, 0, 0},
    {"Sports",      {"sport", NULL, NULL},      0x84CC16, 0, 0, 0},
    {"Adventure",   {"adventure", NULL, NULL},  0xA78BFA, 0, 0, 0},
    {"The '80s",    {NULL, NULL, NULL},         0xF472B6, 1, 1980, 1989},
    {"The '90s",    {NULL, NULL, NULL},         0xFB923C, 1, 1990, 1999},
    {"The 2000s",   {NULL, NULL, NULL},         0x4ADE80, 1, 2000, 2009},
};
#define NCOLL ((int)(sizeof(COLL) / sizeof(COLL[0])))
#define COLL_MIN 12  /* a collection worth showing */

static int icontains(const char *hay, const char *needle) {
    if (!hay || !*hay) return 0;
    for (const char *h = hay; *h; ++h) {
        const char *a = h, *b = needle;
        while (*a && *b && ((*a | 32) == (*b | 32))) { ++a; ++b; }
        if (!*b) return 1;
    }
    return 0;
}

static int plays_of(const Game *g);

static int in_coll(int c, const Game *g) {
    switch (COLL[c].kind) {
    case 1: { int y = g->year ? atoi(g->year) : 0; return y >= COLL[c].lo && y <= COLL[c].hi; }
    case 2: return plays_of(g) == 0;
    case 3: if (icontains(g->genre, "rpg") || icontains(g->genre, "role")) return 0;   /* never short */
            /* fall through: the quick-play genres */
    default:
        for (int m = 0; m < 3 && COLL[c].match[m]; ++m)
            if (icontains(g->genre, COLL[c].match[m])) return 1;
        return 0;
    }
}

static int coll_of[MAX_COLL];  /* system slot -> COLL index */
static int ncoll;

static void fill_colls(void) {
    for (int c = 0; c < ncoll; ++c) {
        System *sy = &systems[last_real + c];
        sy->count = 0;
        for (int s = first_real; s < last_real; ++s)
            for (int i = 0; i < systems[s].count; ++i)
                if (in_coll(coll_of[c], &systems[s].games[i])) sy->games[sy->count++] = systems[s].games[i];
    }
}

static void build_colls(void) {
    last_real = nsys;
    int total[NCOLL] = {0};
    for (int s = VIRT; s < last_real; ++s)
        for (int i = 0; i < systems[s].count; ++i)
            for (int c = 0; c < NCOLL; ++c) total[c] += in_coll(c, &systems[s].games[i]);
    ncoll = 0;
    for (int c = 0; c < NCOLL && ncoll < MAX_COLL; ++c)
        if (total[c] >= COLL_MIN) coll_of[ncoll++] = c;
    if (!ncoll) return;
    if (nsys + ncoll > MAX_SYS) ncoll = MAX_SYS - nsys;
    /* after the real systems, so the consoles come first in the carousel */
    for (int c = 0; c < ncoll; ++c) {
        System *sy = &systems[last_real + c];
        memset(sy, 0, sizeof(*sy));
        snprintf(sy->id, sizeof(sy->id), "coll-%s", COLL[coll_of[c]].name);
        snprintf(sy->name, sizeof(sy->name), "%s", COLL[coll_of[c]].name);
        unsigned int a = COLL[coll_of[c]].accent;
        sy->accent = RGBA8((a >> 16) & 255, (a >> 8) & 255, a & 255, 255);
        sy->games = calloc(total[coll_of[c]] + 1, sizeof(Game));
    }
    nsys += ncoll;
    fill_colls();
}

/* ---------- sorting ---------- */

static int cmp_games(const void *pa, const void *pb) {
    const Game *a = pa, *b = pb;
    if (sort_mode == 1) {
        int ya = atoi(a->year), yb = atoi(b->year);
        if (ya != yb) return (ya ? ya : 9999) - (yb ? yb : 9999);
    } else if (sort_mode == 2) {
        int pa2 = plays_of(a), pb2 = plays_of(b);
        if (pa2 != pb2) return pb2 - pa2;
    }
    return strcasecmp(a->title, b->title);
}

static void fill_search(const char *text);

static void rebuild_derived(void) {
    fill_list(V_RECENT, recent, nrecent);
    fill_list(V_FAV, favs, nfav);
    fill_multi();
    fill_colls();
    if (*search_text) fill_search(search_text);
}

static void sort_all(int mode) {
    sort_mode = mode;
    for (int s = first_real; s < last_real; ++s) qsort(systems[s].games, systems[s].count, sizeof(Game), cmp_games);
    rebuild_derived();
}

/* ---------- search ---------- */

static void fill_search(const char *text) {
    System *sy = &systems[V_SEARCH];
    sy->count = 0;
    if (!text || !*text) return;
    for (int s = first_real; s < last_real && sy->count < MAX_SEARCH; ++s)
        for (int i = 0; i < systems[s].count && sy->count < MAX_SEARCH; ++i)
            if (icontains(systems[s].games[i].title, text)) sy->games[sy->count++] = systems[s].games[i];
}

/* Index of the first game whose title starts with a different letter. */
static int letter_jump(const System *sy, int sel, int dir) {
    if (sy->count < 2) return sel;
    char c0 = sy->games[sel].title[0] | 32;
    int i = sel;
    for (int step = 0; step < sy->count; ++step) {
        i = (i + dir + sy->count) % sy->count;
        char c = sy->games[i].title[0] | 32;
        if (c != c0) {
            if (dir < 0) {  /* walk back to the first entry of that letter */
                while (1) {
                    int prev = (i - 1 + sy->count) % sy->count;
                    if ((sy->games[prev].title[0] | 32) != c || prev > i) break;
                    i = prev;
                }
            }
            return i;
        }
    }
    return sel;
}

static void setup_lists(void) {
    static const char *names[VIRT][2] = {{"recent", "Continue"}, {"favourites", "Favourites"},
                                         {"multiplayer", "Multiplayer"}, {"search", "Search"}};
    static const unsigned int accents[VIRT] = {0x38BDF8, 0xFACC15, 0xF472B6, 0x38F8C8};
    int multi = 0;
    for (int s = first_real; s < last_real; ++s)
        for (int i = 0; i < systems[s].count; ++i) multi += max_players(systems[s].games[i].players) >= 2;
    int cap[VIRT] = {MAX_RECENT, MAX_FAV, multi, MAX_SEARCH};
    for (int v = 0; v < VIRT; ++v) {
        snprintf(systems[v].id, sizeof(systems[v].id), "%s", names[v][0]);
        snprintf(systems[v].name, sizeof(systems[v].name), "%s", names[v][1]);
        unsigned int a = accents[v];
        systems[v].accent = RGBA8((a >> 16) & 255, (a >> 8) & 255, a & 255, 255);
        systems[v].games = calloc(cap[v] ? cap[v] : 1, sizeof(Game));
        systems[v].count = 0;
    }
    fill_multi();
    nrecent = load_refs(USER "recent.tsv", recent, MAX_RECENT);
    load_stats();
    nfav = load_refs(USER "favourites.tsv", favs, MAX_FAV);
    load_plays();
    fill_list(V_RECENT, recent, nrecent);
    fill_list(V_FAV, favs, nfav);
}

/* ---------- texture cache (lazy, LRU) ---------- */

typedef struct { char path[256]; vita2d_texture *tex; unsigned int used; } Slot;
static Slot cache[CACHE];
static unsigned int tick;

static vita2d_texture *tex(const char *path);

/* Console photos for the real-system shelves: Evan-Amos's, from Wikimedia
 * Commons (public domain, two CC BY-SA 3.0; see assets/licenses/CONSOLE-PHOTOS.md),
 * cut out by arcadehub/consoles_public/make.py and shipped in the app. */
static vita2d_texture *console_art(int i) {
    static signed char known[MAX_SYS];              /* 0 unknown, 1 yes, -1 no */
    if (i < 0 || i >= MAX_SYS || known[i] < 0) return NULL;
    char path[96];
    snprintf(path, sizeof(path), "app0:assets/consoles/%s.png", systems[i].id);
    return tex(path);                               /* a missing render just stays NULL */
}

/* A console on a stage: soft shadow, a faint mirror of its lower half on the
 * floor, and a slow bob for the one you are on, so the carousel reads as
 * objects in a room rather than flat cards. */
static void draw_console(vita2d_texture *t, float cx, float cw, float floor_y, float box_h, int on, unsigned int frames) {
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    float sc = cw * 1.02f / tw;                    /* not wider than the slot: neighbours overlapped */
    if (th * sc > box_h) sc = box_h / th;
    float w = tw * sc, h = th * sc;
    float bob = on ? 4.0f * sinf(frames * 0.045f) : 0.0f;
    float x = cx + cw / 2 - w / 2, y = floor_y - h - 6 + bob;
    /* A soft contact shadow, smaller when the console rises on its bob. */
    float lift = on ? (bob + 4) / 8 : 0.5f;
    draw_soft(ui_soft_shadow(), cx + cw / 2, floor_y + 1, w * (1.05f - 0.1f * lift), 22 - 4 * lift,
              RGBA8(0, 0, 0, (int)((on ? 150 : 110) * (1 - 0.3f * lift))));
    /* The reflection: the bottom quarter, flipped, fading to nothing. */
    float part = th * 0.25f, bh = part / 10;
    for (int k = 0; k < 10; ++k) {
        float f = 1.0f - k / 10.0f;
        unsigned int a = (unsigned int)((on ? 44 : 26) * f * f * f);
        if (!a) break;
        vita2d_draw_texture_tint_part_scale(t, x, floor_y + 2 + (k + 1) * bh * sc, 0, th - (k + 1) * bh, tw, bh, sc, -sc,
                                            RGBA8(255, 255, 255, a));
    }
    vita2d_draw_texture_tint_scale(t, x, y, sc, sc, RGBA8(255, 255, 255, on ? 255 : 150));
}

static vita2d_texture *tex(const char *path) {
    return ui_image(path);                          /* decoded off the main thread */
}

/* Draws t scaled to fit (contain) inside the box; returns drawn width. */
static float draw_fit(vita2d_texture *t, float x, float y, float bw, float bh, unsigned int tint, int cover) {
    if (!t) return 0;
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    float s = cover ? (bw / tw > bh / th ? bw / tw : bh / th) : (bw / tw < bh / th ? bw / tw : bh / th);
    float dw = tw * s, dh = th * s;
    vita2d_draw_texture_tint_scale(t, x + (bw - dw) / 2, y + (bh - dh) / 2, s, s, tint);
    return dw;
}

/* Word-wraps text into lines of at most `width` px; draws up to max_lines. */
static void draw_wrapped(UiFont *f, const char *text, int x, int y, int width, int size, int max_lines, unsigned int color) {
    char line[512] = {0};
    int lines = 0, len = 0;
    const char *p = text;
    while (*p && lines < max_lines) {
        const char *word = p;
        while (*p && *p != ' ' && *p != '\n') ++p;
        int wl = p - word;
        char trial[512];
        snprintf(trial, sizeof(trial), "%s%s%.*s", line, len ? " " : "", wl, word);
        if (len && uifont_width(f, size, trial) > width) {
            uifont_draw(f, x, y + lines * (size + 8), color, size, line);
            lines++;
            snprintf(line, sizeof(line), "%.*s", wl, word);
        } else snprintf(line, sizeof(line), "%s", trial);
        len = strlen(line);
        if (*p == '\n') {
            if (lines < max_lines) uifont_draw(f, x, y + lines * (size + 8), color, size, line);
            lines++; line[0] = 0; len = 0;
        }
        if (*p) ++p;
    }
    if (len && lines < max_lines) uifont_draw(f, x, y + lines * (size + 8), color, size, line);
}

/* ---------- launching ---------- */

/* Boot verification hook: tools/boot_check.py drops
 * ux0:data/arcadehub/test-launch.txt containing "<system id>\t<index>" and the
 * hub launches exactly that game on startup, writing the launch result to
 * test-launch.result. Menu navigation cannot address a game deterministically
 * (the shelf remembers where you were), and a checker that cannot say which
 * game it launched is not worth much. */
#define TEST_REQUEST ROOT "test-launch.txt"
#define TEST_RESULT ROOT "test-launch.result"

static void write_result(const char *fmt, const char *a, int b) {
    SceUID fd = sceIoOpen(TEST_RESULT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    char line[320];
    int n = snprintf(line, sizeof(line), fmt, a, b);
    sceIoWrite(fd, line, n);
    sceIoClose(fd);
}

static int write_psp_boot(const char *path) {
    /* RetroFlow's RETROLNCR bubble + boot.bin contract (LAUNCHING.md case b). */
    int len = 0;
    char *tmpl = slurp("ux0:app/RETROFLOW/payloads/boot.bin", &len);
    if (!tmpl || len < 0x140) { free(tmpl); return -1; }
    unsigned char *b = (unsigned char *)tmpl;
    /* 0x40: 256-byte path, lowercased, device kept, "/pspemu/" -> "pspemu/"
     * (RetroFlow index.lua:6082-6091): ux0:/pspemu/PSP/GAME/X/EBOOT.PBP ->
     * ux0:pspemu/psp/game/x/eboot.pbp. adrbubblebooter maps the device prefix. */
    memset(b + 0x40, 0, 0x100);
    char low[256];
    int i = 0;
    for (; path[i] && i < 255; ++i) low[i] = (path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i];
    low[i] = 0;
    char *hit = strstr(low, "/pspemu/");
    if (hit) memmove(hit, hit + 1, strlen(hit + 1) + 1);
    snprintf((char *)b + 0x40, 0x100, "%s", low);
    unsigned int driver = 1; /* INFERNO, RetroFlow's default */
    unsigned int exe = 0;    /* EBOOT.BIN */
    memcpy(b + 0x04, &driver, 4);
    memcpy(b + 0x08, &exe, 4);
    sceIoRemove("ux0:app/RETROLNCR/data/boot.inf");
    sceIoRemove("ux0:app/RETROLNCR/data/config.bin");
    sceIoMkdir("ux0:app/RETROLNCR/data", 0777);
    SceUID fd = sceIoOpen("ux0:app/RETROLNCR/data/boot.bin", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) { free(tmpl); return fd; }
    sceIoWrite(fd, b, len);
    sceIoClose(fd);
    free(tmpl);
    return 0;
}

static int launch(const Game *g) {
    char uri[1024];
    if (!strcmp(g->kind, "app")) snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", g->a1);
    else if (!strcmp(g->kind, "ra")) snprintf(uri, sizeof(uri), "psgm:play?titleid=RETROVITA&param=%s&param2=%s", g->a1, g->a2);
    else if (!strcmp(g->kind, "n64")) snprintf(uri, sizeof(uri), "psgm:play?titleid=DEDALOX64&param=%s", g->a1);
    else if (!strcmp(g->kind, "scumm")) snprintf(uri, sizeof(uri), "psgm:play?titleid=VSCU00001&path=%s&game_id=%s", g->a1, g->a2);
    else if (!strcmp(g->kind, "psp")) {
        if (write_psp_boot(g->a1) < 0) return -1;
        snprintf(uri, sizeof(uri), "psgm:play?titleid=RETROLNCR");
    } else return -1;
    /* The SceShell bridge (vabridge >= 3.2) taps OK on the "Arcade Hub will
     * close" dialog when it sees this flag. */
    SceUID fd = sceIoOpen(ROOT "confirm.req", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
    sfx_play(SFX_LAUNCH);
    sceKernelDelayThread(300 * 1000);          /* let the chord start before Home closes */
    return sceAppMgrLaunchAppByUri(0xFFFFF, uri);
}

/* Loads the agent remote if it is on the card (same as Agent Recovery's loader):
 * vakern injects its TCP bridge into SceShell. Harmless if already loaded. */
static void start_remote(void) {
    if (exists("ux0:data/vita-agent/vakern.skprx"))
        taiLoadStartKernelModule("ux0:data/vita-agent/vakern.skprx", 0, NULL, 0);
}

/* ---------- on-screen keyboard ---------- */

/* ---------- UI ---------- */

#define WHITE RGBA8(255, 255, 255, 255)
#define DIM RGBA8(255, 255, 255, 150)
#define FAINT RGBA8(255, 255, 255, 70)

static float ease(float cur, float target) { return cur + (target - cur) * 0.22f; }

static char context[128];      /* shown in Home's tab bar */
static const char *hint = "";

static void background_a(unsigned int accent, vita2d_texture *art, int alpha) {
    vita2d_draw_rectangle(0, 0, W, H, RGBA8(21, 24, 33, 255));
    if (art) draw_fit(art, -18 - 14 * ui_tilt_x, -10 + 8 * ui_tilt_y, W + 36, H + 20, RGBA8(255, 255, 255, alpha), 1);
    /* accent wash + bottom shade */
    unsigned int wash = (accent & 0x00FFFFFF) | (40u << 24);
    vita2d_draw_rectangle(0, 0, W, H, wash);
    for (int i = 0; i < 12; ++i)
        vita2d_draw_rectangle(0, H - 200 + i * 17, W, 17, RGBA8(15, 17, 24, 18 + i * 12));
    ui_ambient(art ? 0.5f : 1.0f);
}

static void background(unsigned int accent, vita2d_texture *art) { background_a(accent, art, 70); }

static int view = 0, sel = 0;                /* view 0 = systems, 1 = games */
static int sys;
static float sys_pos, game_pos = 0;
/* Play's two ways in: the consoles (the main one, and where Play opens) and
 * the collections (Continue, Favourites, Multiplayer, Search, genres,
 * decades). The carousel shows one group; sys_pos counts places within it. */
static int group, chips_focus, group_sys[2] = {-1, -1};
static int is_console(int i) { return i >= first_real && i < last_real; }
static int in_group(int i) { return group == 0 ? is_console(i) : !is_console(i); }
static int ord_of(int i) { int n = 0; for (int k = 0; k < i; ++k) n += in_group(k); return n; }
static int at_ord(int n) { for (int k = 0; k < nsys; ++k) if (in_group(k) && n-- == 0) return k; return -1; }
static int group_size(void) { int n = 0; for (int k = 0; k < nsys; ++k) n += in_group(k); return n; }
static void set_group(int g) {
    if (g == group) return;
    group_sys[group] = sys;
    group = g;
    sys = group_sys[g] >= 0 && in_group(group_sys[g]) ? group_sys[g] : at_ord(0);
    if (sys < 0) sys = 0;
    sys_pos = ord_of(sys);
}
static int game_sel[MAX_SYS] = {0};
static char toast[128] = {0};
static int toast_frames = 0, toast_ok = 0;
static int confirm = 0;  /* remove-game prompt is up */
static int help = 0;     /* the controls card is up */
/* previews + attract mode */
static int settle = 0, last_key = -1;          /* frames since the selection changed */
static char playing[256] = {0};
static int pre_sys, pre_sel, pre_view;
static int idle = 0, attract = 0, attract_frames = 0, a_sys = 0, a_sel = 0;
/* touch: drag the strip / carousel, tap to pick */
static int touching = 0, t_x0 = 0, t_y0 = 0, t_moved = 0, tap_play = 0, t_frames = 0, t_x = 0, t_y = 0;
static float drag_base = 0;
/* cross-fade when the shown game changes */
static Game shown;
static int have_shown = 0, has_fade = 0;
static Game fade_from;
static float fade_t = 0;
static unsigned int frames_up = 0;

void play_init(void) {
    SceCommonDialogConfigParam cfg;   /* required before any common dialog */
    sceCommonDialogConfigParamInit(&cfg);
    sceCommonDialogSetConfigParam(&cfg);
    start_remote();
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    load_catalog();
    int test_sys = -1, test_index = -1;
    char test_id[32] = {0};
    {
        char *req = slurp(TEST_REQUEST, NULL);
        if (req) {
            char *tab = strchr(req, '\t');
            if (tab) {
                *tab = 0;
                snprintf(test_id, sizeof(test_id), "%s", req);
                test_index = atoi(tab + 1);
            }
            free(req);
            sceIoRemove(TEST_REQUEST);
        }
    }
    last_real = nsys;                /* before apply_hidden, which walks the real systems */
    if (nsys <= VIRT) nsys = 0;
    else { nhidden = load_refs(USER "hidden.tsv", hidden, MAX_HIDDEN); if (nhidden) apply_hidden();
           build_colls(); setup_lists(); }
    sys = first_real < last_real ? first_real : 0;      /* Play opens on the consoles */
    group = 0;
    sys_pos = ord_of(sys);

    srand(sceKernelGetProcessTimeLow());

    /* Resolve now: build_colls() inserts shelves and shifts every real system. */
    if (*test_id)
        for (int i = 0; i < nsys; ++i)
            if (!strcmp(systems[i].id, test_id)) { test_sys = i; break; }
    if (test_sys >= 0 && test_index >= 0 && test_index < systems[test_sys].count) {
        Game *g = &systems[test_sys].games[test_index];
        int rc = launch(g);
        write_result("%s\tlaunch rc=0x%08X\n", g->title, rc);
        sys = test_sys; sel = test_index; view = 1; game_pos = sel;
    } else if (*test_id) {
        write_result("%s\tnot launched: no such shelf or index %d\n", test_id, test_index);
    }
}

/* The library changed on the card (VitaOS's background scan): read it again.
 * The old games arrays are left behind (a few KB, rarely). */
void play_reload(void) {
    video_stop_owned(OWN_PREVIEW);
    playing[0] = 0;
    memset(systems, 0, sizeof(systems));
    memset(game_sel, 0, sizeof(game_sel));
    nsys = 0;
    first_real = VIRT;
    load_catalog();
    last_real = nsys;
    if (nsys <= VIRT) nsys = 0;
    else { nhidden = load_refs(USER "hidden.tsv", hidden, MAX_HIDDEN); if (nhidden) apply_hidden();
           build_colls(); setup_lists(); }
    sys = first_real < last_real ? first_real : 0;
    group = 0;
    view = 0;
    sel = 0;
    sys_pos = ord_of(sys);
}

static int fullscreen, header_touch;

/* ---------- game details (triangle on a game) ---------- */

static int details;                 /* open? */
static const char *hint_override;
static Game dgame;                  /* the game shown (a copy: lists get rebuilt) */
static Game similar[7];
static int nsimilar, dfocus = -1;   /* -1 = the Play button, else a similar game */
static unsigned int dframes;
#define MAX_STATES 5
static struct { vita2d_texture *t; char label[24]; char path[300]; } states[MAX_STATES];
static int nstates;

static void details_free(void) { nstates = 0; }   /* the image cache owns the pictures */

/* RetroArch's states for this ROM, with the pictures it saves beside them. */
static void details_states(void) {
    details_free();
    if (strcmp(dgame.kind, "ra") || !dgame.a2) return;
    const char *base = strrchr(dgame.a2, '/');
    base = base ? base + 1 : dgame.a2;
    const char *dot = strrchr(base, '.');
    int stem = dot ? (int)(dot - base) : (int)strlen(base);
    SceUID d = sceIoDopen("ux0:data/retroarch/savestates");
    if (d < 0) return;
    SceIoDirent core;
    while (nstates < MAX_STATES) {
        memset(&core, 0, sizeof(core));
        if (sceIoDread(d, &core) <= 0) break;
        if (!SCE_S_ISDIR(core.d_stat.st_mode)) continue;
        char dir[256];
        snprintf(dir, sizeof(dir), "ux0:data/retroarch/savestates/%s", core.d_name);
        SceUID d2 = sceIoDopen(dir);
        if (d2 < 0) continue;
        SceIoDirent e;
        while (nstates < MAX_STATES) {
            memset(&e, 0, sizeof(e));
            if (sceIoDread(d2, &e) <= 0) break;
            int n = strlen(e.d_name);
            if (n < 4 || strcasecmp(e.d_name + n - 4, ".png") || strncmp(e.d_name, base, stem) || strncmp(e.d_name + stem, ".state", 6)) continue;
            const char *slot = e.d_name + stem + 6;
            char path[400];
            snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
            states[nstates].t = ui_image(path);           /* queued; drawn once loaded */
            snprintf(states[nstates].path, sizeof(states[0].path), "%s", path);
            if (!strncmp(slot, ".auto", 5)) snprintf(states[nstates].label, sizeof(states[0].label), "Quick Resume");
            else snprintf(states[nstates].label, sizeof(states[0].label), "Slot %d", atoi(slot));
            nstates++;
        }
        sceIoDclose(d2);
    }
    sceIoDclose(d);
}

static void details_open(const Game *g) {
    dgame = *g;
    details = 1;
    dfocus = -1;
    dframes = 0;
    details_states();
    /* More like this: the first genre word, this system first, then the rest. */
    char genre[24] = {0};
    if (g->genre) sscanf(g->genre, "%23[^ /,]", genre);
    nsimilar = 0;
    for (int pass = 0; pass < 2 && nsimilar < 7; ++pass)
        for (int s2 = first_real; s2 < last_real && nsimilar < 7; ++s2) {
            if ((pass == 0) != (s2 == g->origin)) continue;
            for (int i = 0; i < systems[s2].count && nsimilar < 7; ++i) {
                const Game *o = &systems[s2].games[i];
                if (!strcmp(o->title, g->title) || !*genre || !o->genre || !icontains(o->genre, genre) || !o->cover || !*o->cover) continue;
                if ((i * 7 + s2) % 3) continue;              /* spread picks out, not the first eight A-titles */
                similar[nsimilar++] = *o;
            }
        }
}

static void details_frame(const Input *in, unsigned int pressed) {
    Game *g = &dgame;
    ++dframes;
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) { details = 0; details_free(); return; }
    if (pressed & SCE_CTRL_DOWN && nsimilar && dfocus < 0) dfocus = 0;
    if (pressed & SCE_CTRL_UP) dfocus = -1;
    if (dfocus >= 0 && pressed & SCE_CTRL_LEFT) dfocus = dfocus > 0 ? dfocus - 1 : 0;
    if (dfocus >= 0 && pressed & SCE_CTRL_RIGHT) dfocus = dfocus < nsimilar - 1 ? dfocus + 1 : dfocus;
    if (pressed & SCE_CTRL_SQUARE) {
        int on = toggle_fav(g);
        snprintf(toast, sizeof(toast), on ? "Added to Favourites" : "Removed from Favourites");
        toast_frames = 90; toast_ok = 1;
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (dfocus >= 0) { Game o = similar[dfocus]; details_open(&o); return; }
        video_stop_owned(OWN_PREVIEW); playing[0] = 0;
        int rc = launch(g);
        if (rc >= 0) { note_played(g); snprintf(toast, sizeof(toast), "Starting %s\xE2\x80\xA6", g->title); toast_frames = 600; toast_ok = 1; }
        else { snprintf(toast, sizeof(toast), "Could not launch (0x%08X)", rc); toast_frames = 180; toast_ok = 0; }
    }
    ui_theme_from(tex(g->cover));

    /* backdrop */
    vita2d_texture *bg = g->bg && *g->bg ? tex(g->bg) : NULL;
    if (bg) {
        float tw = vita2d_texture_get_width(bg), th = vita2d_texture_get_height(bg), sc = W / tw > H / th ? W / tw : H / th;
        vita2d_draw_texture_tint_scale(bg, (W - tw * sc) / 2, (H - th * sc) / 2, sc, sc, RGBA8(255, 255, 255, 70));
    }
    for (int k = 0; k < 10; ++k) vita2d_draw_rectangle(k * 56, 64, 56, H - 104, RGBA8(21, 24, 33, 200 - k * 16));

    int i = stat_of(g, 0);
    char meta[160], line[160];
    snprintf(meta, sizeof(meta), "%s%s%s%s%s", systems[g->origin].name, g->year && *g->year ? "   \xC2\xB7   " : "", g->year ? g->year : "",
             g->genre && *g->genre ? "   \xC2\xB7   " : "", g->genre ? g->genre : "");
    text_fit(bold, 40, 118, C_TEXT, 32, g->title, 500);
    text_fit(font, 40, 146, C_DIM, 16, meta, 500);
    int plays = plays_of(g);
    if (i >= 0 && stats[i].last) {
        time_t t = stats[i].last;
        struct tm *tm = localtime(&t);
        static const char *const mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        long h = stats[i].secs / 3600, m = stats[i].secs / 60 % 60;
        if (stats[i].secs >= 60) snprintf(line, sizeof(line), "Played %d time%s   \xC2\xB7   %ldh %02ldm   \xC2\xB7   last %s %d",
                                          plays, plays == 1 ? "" : "s", h, m, mon[tm->tm_mon % 12], tm->tm_mday);
        else snprintf(line, sizeof(line), "Played %d time%s   \xC2\xB7   last %s %d", plays, plays == 1 ? "" : "s", mon[tm->tm_mon % 12], tm->tm_mday);
    } else snprintf(line, sizeof(line), plays ? "Played %d times" : "Never played", plays);
    text(font, 40, 174, C_ACCENT, 16, line);

    int play_on = dfocus < 0;                                        /* the Play button */
    if (play_on) draw_focus(40, 192, 150, 40, 1);
    draw_action_button(40, 192, 150, 40, "X Play", play_on, C_ACCENT);
    draw_hints(206, 212, is_fav(g) ? "[] Favourite \xE2\x98\x85" : "[] Add to favourites", C_DIM, W);
    if (g->desc && *g->desc) draw_wrapped(font, g->desc, 40, 262, 490, 16, 5, C_DIM);

    /* media: the live preview if it is playing, else the screenshot, cycling */
    int mx = 560, my = 92, mw = 360, mh = 202;
    vita2d_draw_rectangle(mx, my, mw, mh, RGBA8(0, 0, 0, 200));
    vita2d_texture *slides[3];
    int ns = 0;
    vita2d_texture *vf = video_owner() == OWN_PREVIEW ? video_frame() : NULL;
    if (vf) slides[ns++] = vf;
    if (bg) slides[ns++] = bg;
    for (int k = 0; k < nstates; ++k) states[k].t = ui_image(states[k].path);
    if (nstates && states[0].t) slides[ns++] = states[0].t;
    if (ns) {
        int k = (dframes / 300) % ns;                               /* five seconds a slide */
        vita2d_texture *t = slides[k];
        float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t), sc = mw / tw < mh / th ? mw / tw : mh / th;
        vita2d_draw_texture_scale(t, mx + (mw - tw * sc) / 2, my + (mh - th * sc) / 2, sc, sc);
        for (int d = 0; d < ns; ++d) vita2d_draw_rectangle(mx + mw / 2 - ns * 9 + d * 18, my + mh + 8, 10, 4, d == k ? C_ACCENT : C_FAINT);
    }
    if (nstates) {                                                   /* save states */
        text(bold, mx, my + mh + 40, C_TEXT, 15, "Save states");
        for (int s3 = 0; s3 < nstates && s3 < 4; ++s3) {
            if (!states[s3].t) continue;
            float tw = vita2d_texture_get_width(states[s3].t), th = vita2d_texture_get_height(states[s3].t);
            float sw = 84, sh = sw * th / tw;
            int sx = mx + s3 * 92;
            vita2d_draw_texture_scale(states[s3].t, sx, my + mh + 50, sw / tw, sh / th);
            text_fit(font, sx, my + mh + 50 + (int)sh + 16, C_FAINT, 12, states[s3].label, 84);
        }
    }
    if (nsimilar) {                                                  /* more like this */
        text(bold, 40, 420, C_TEXT, 15, "More like this");
        for (int k = 0; k < nsimilar; ++k) {
            vita2d_texture *c = tex(similar[k].cover);
            int cx = 40 + k * 66, cy = 432;
            if (dfocus == k) draw_focus(cx, cy, 58, 58, 1);
            if (c) {
                float tw = vita2d_texture_get_width(c), th = vita2d_texture_get_height(c), sc = 58 / (tw > th ? tw : th);
                vita2d_draw_texture_scale(c, cx + (58 - tw * sc) / 2, cy + (58 - th * sc) / 2, sc, sc);
            } else vita2d_draw_rectangle(cx, cy, 58, 58, RGBA8(40, 45, 60, 255));
        }
        if (dfocus >= 0) text_fit(font, 40 + nsimilar * 66 + 8, 466, C_TEXT, 15, similar[dfocus].title, W - (40 + nsimilar * 66 + 8) - 30);
    }
    (void)in;
}

/* ---------- for global search ---------- */

int play_find(const char *q, Hit *out, int max) {
    int n = 0;
    for (int sc = 3; sc >= 1; --sc)                 /* best matches first, across every system */
        for (int s = first_real; s < last_real && n < max; ++s)
            for (int i = 0; i < systems[s].count && n < max; ++i) {
                const Game *g = &systems[s].games[i];
                if (match_score(g->title, q) != sc) continue;
                out[n] = (Hit){H_GAME, s, i, sc, g->title, systems[s].name, NULL, "", 0};
                snprintf(out[n++].path, sizeof(out[0].path), "%s", g->cover ? g->cover : "");
            }
    return n;
}

void play_open_hit(const Hit *h) {
    if (h->a < 0 || h->a >= nsys || h->b < 0 || h->b >= systems[h->a].count) return;
    video_stop_owned(OWN_PREVIEW);
    playing[0] = 0;
    Game g = systems[h->a].games[h->b];
    sys = h->a; sel = h->b; view = 1; game_pos = sel;     /* Play shows it too, if the launch is cancelled */
    if (launch(&g) >= 0) note_played(&g);
    else ui_message("Could not start", "The game would not launch.");
}

void play_show_all(const char *q) {
    snprintf(search_text, sizeof(search_text), "%s", q);
    fill_search(search_text);
    sys = V_SEARCH; sel = 0; game_pos = 0; view = systems[V_SEARCH].count ? 1 : 0;
}

/* ---------- for the Home tab ---------- */

int play_recent_count(void) { return systems[V_RECENT].count < 8 ? systems[V_RECENT].count : 8; }

void play_recent_item(int i, PlayItem *out) {
    const Game *g = &systems[V_RECENT].games[i];
    out->title = g->title;
    out->system = g->origin >= 0 && g->origin < nsys ? systems[g->origin].name : "";
    out->year = g->year ? g->year : "";
    out->genre = g->genre ? g->genre : "";
    out->rom = !strcmp(g->kind, "ra") ? g->a2 : NULL;
    out->cover = tex(g->cover);
    out->art = g->bg && *g->bg ? tex(g->bg) : out->cover;
    out->accent = g->origin >= 0 && g->origin < nsys ? systems[g->origin].accent : C_ACCENT;
}

int play_recent_launch(int i) {
    if (i < 0 || i >= systems[V_RECENT].count) return -1;
    video_stop_owned(OWN_PREVIEW);
    playing[0] = 0;
    Game g = systems[V_RECENT].games[i];
    int rc = launch(&g);
    if (rc >= 0) note_played(&g);
    return rc;
}

void play_frame(const Input *in) {
    STAGE("play: frame");
    frames_up++;
    /* Dynamic theme: the focused game's cover, or the system's own colour. */
    if (view && sys >= 0 && sys < nsys && sel >= 0 && sel < systems[sys].count) ui_theme_from(tex(systems[sys].games[sel].cover));
    else if (sys >= 0 && sys < nsys) ui_theme_color(systems[sys].accent | 0xFF000000);
    /* Home's input: key repeat and the stick-as-D-pad are done there. L and R
     * belong to Home (they switch tabs). */
    unsigned int pressed = in->pressed & ~(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_L1 | SCE_CTRL_R1);
    if (details) { details_frame(in, pressed); hint_override = "\xC3\x97  play    \xE2\x96\xA1  favourite    \xE2\x86\x93  more like this    \xE2\x97\x8B  back"; return; }
    hint_override = NULL;
    if (pressed) idle = 0; else idle++;
    if (attract) {
        if (pressed) {
            attract = 0; idle = 0;
            sys = a_sys; sel = a_sel; game_sel[sys] = sel; view = 1; game_pos = sel;
            if (pressed & SCE_CTRL_CROSS) pressed = SCE_CTRL_CROSS; /* play it now */
            else pressed = 0;
        }
    } else if (idle > 60 * 30 && nsys && video_owner() != OWN_MUSIC) {
        attract = 1; attract_frames = 0;
        pre_sys = sys; pre_sel = sel; pre_view = view;       /* PS brings you back here */
    }

    fullscreen = 0;
    context[0] = 0;
    if (nsys == 0) {
        uifont_draw(bold, 60, 260, WHITE, 28, "No games yet");
        uifont_draw(font, 60, 300, DIM, 20, "No catalog found in ux0:data/arcadehub/");
        hint = "";
        return;
    }

    /* ---- touch: swipe to scroll, tap to choose ---- */
    int tx = in->tx, ty = in->ty, down = in->touching;
    /* A touch that starts on the tab bar belongs to Home. */
    if (down && !touching && ty < 66) header_touch = 1;
    if (!down) header_touch = 0;
    if (header_touch) down = 0;
    System *S = &systems[sys];
    /* The bridge swipes the boot lock screen away; ignore those first
     * seconds of touch so a spare swipe does not drag the carousel. */
    if (frames_up < 60 * 8) { down = 0; touching = 0; }
    if (down && !touching && !confirm) {
        touching = 1; t_moved = 0; t_frames = 0; t_x0 = tx; t_y0 = ty; t_x = tx; t_y = ty;
        drag_base = view == 0 ? sys_pos : game_pos;
        idle = 0;
    } else if (down && touching) {
        int dx = tx - t_x0;
        t_frames++;
        t_x = tx; t_y = ty;
        /* 24 px, not 10: a thumb tap wanders, and treating that as a drag
         * was swallowing taps entirely (playtest, 2026-09-21). */
        if (dx > 24 || dx < -24 || ty - t_y0 > 24 || t_y0 - ty > 24) t_moved = 1;
        idle = 0;
        if (t_moved) {
            if (view == 0) sys_pos = drag_base - dx / 280.0f;
            else if (S->count) {
                game_pos = drag_base - dx / 92.0f;
                if (game_pos < 0) game_pos = 0;
                if (game_pos > S->count - 1) game_pos = S->count - 1;
            }
        }
    } else if (!down && touching) {  /* release */
        touching = 0;
        idle = 0;
        /* A quick contact that ends near where it began is a tap, however
         * much it wobbled on the way. */
        int travel = (t_x - t_x0) * (t_x - t_x0) + (t_y - t_y0) * (t_y - t_y0);
        if (t_frames < 20 && travel < 40 * 40) t_moved = 0;
        if (attract) { attract = 0; sys = a_sys; sel = a_sel; view = 1; game_pos = sel; }
        else if (t_moved) {
            if (view == 0) {
                int n = (int)(sys_pos + (sys_pos < 0 ? -0.5f : 0.5f)), gs = group_size();
                sys = at_ord(n < 0 ? 0 : (n >= gs ? gs - 1 : n));
            } else if (S->count) {
                sel = (int)(game_pos + 0.5f);
                if (sel >= S->count) sel = S->count - 1;
            }
        } else if (view == 0) {           /* tap a system card */
            if (t_y < 112) {                                   /* the Consoles / Collections chips */
                set_group(t_x < 40 + 124 ? 0 : 1);
            } else {
                int n = at_ord((int)(sys_pos + (t_x - W / 2) / 280.0f + 0.5f));
                if (n >= 0 && n < nsys) {
                    if (n == sys && systems[n].count) { view = 1; sel = game_sel[sys] < systems[n].count ? game_sel[sys] : 0; game_pos = sel; }
                    else sys = n;
                }
            }
        } else if (S->count) {
            if (t_y > H - 170 && t_y < H - 40) {   /* tap a cover in the strip */
                int n = (int)(game_pos + (t_x - W / 2) / 92.0f + 0.5f);
                if (n >= 0 && n < S->count) {
                    if (n == sel) tap_play = 1;      /* tap the current one again to play */
                    else sel = n;
                }
            } else if (t_x < 360 && t_y > 70 && t_y < 390) tap_play = 1;   /* tap the big art: play */
            else if (t_y < 70) { game_sel[sys] = sel; view = 0; }       /* tap the header: back */
        }
    }
    if (tap_play) { pressed |= SCE_CTRL_CROSS; tap_play = 0; }
    if (help) {
        if (pressed) help = 0;
        pressed = 0;
    } else if (view == 0 && (pressed & SCE_CTRL_START)) {
        help = 1;
        pressed = 0;
    }
    if (view == 0) {
        if (!in_group(sys)) group = is_console(sys) ? 0 : 1;   /* search or Home put us elsewhere */
        if (chips_focus) {
            if (pressed & SCE_CTRL_LEFT) set_group(0);
            if (pressed & SCE_CTRL_RIGHT) set_group(1);
            if (pressed & (SCE_CTRL_UP | SCE_CTRL_CROSS)) chips_focus = 0;
            pressed = 0;
        }
        int gs = group_size(), o = ord_of(sys);
        if (pressed & SCE_CTRL_LEFT) sys = at_ord((o + gs - 1) % gs);
        if (pressed & SCE_CTRL_RIGHT) sys = at_ord((o + 1) % gs);
        if (pressed & SCE_CTRL_DOWN) { chips_focus = 1; pressed = 0; }   /* the chips sit under the carousel */
        S = &systems[sys];
        if ((pressed & SCE_CTRL_CROSS) && S->count) { view = 1; sel = game_sel[sys] < S->count ? game_sel[sys] : 0; game_pos = sel; }
        if ((pressed & SCE_CTRL_CROSS) && sys == V_SEARCH && !S->count) search_open();   /* the global search */
        if (pressed & SCE_CTRL_TRIANGLE) {
            {
                static const char *sort_name[3] = {"name", "year", "most played"};
                sort_all((sort_mode + 1) % 3);
                snprintf(toast, sizeof(toast), "Sorted by %s", sort_name[sort_mode]);
                toast_frames = 120; toast_ok = 1;
            }
        }
    } else if (!S->count) {
        view = 0;  /* list emptied (last favourite removed) */
        confirm = 0;
    } else if (confirm) {
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) {
            int del = (pressed & SCE_CTRL_SQUARE) != 0;
            const char *rom = rom_path(&S->games[sel]);
            int app = !strcmp(S->games[sel].kind, "app");
            snprintf(toast, sizeof(toast), del && (rom || app) ? (app ? "Uninstalling %.60s" : "Deleted %.60s") : "Removed %.60s", S->games[sel].title);
            hide_game(sys, sel, del);
            toast_frames = 150; toast_ok = 1;
            confirm = 0;
            if (sel >= S->count) sel = S->count ? S->count - 1 : 0;
            if (!S->count) view = 0;
            game_pos = sel;
        } else if (pressed) confirm = 0;
    } else {
        if (sel >= S->count) sel = S->count - 1;
        if (pressed & SCE_CTRL_LEFT) sel = (sel + S->count - 1) % S->count;
        if (pressed & SCE_CTRL_RIGHT) sel = (sel + 1) % S->count;
        if (pressed & SCE_CTRL_TRIANGLE) { details_open(&S->games[sel]); pressed = 0; }
        if (pressed & SCE_CTRL_UP) sel = letter_jump(S, sel, -1);
        if (pressed & SCE_CTRL_DOWN) sel = letter_jump(S, sel, 1);

        if (pressed & SCE_CTRL_CIRCLE) { game_sel[sys] = sel; view = 0; }
        if (pressed & SCE_CTRL_START) confirm = 1;
        if (pressed & SCE_CTRL_SQUARE) {
            int on = toggle_fav(&S->games[sel]);
            snprintf(toast, sizeof(toast), on ? "Added to Favourites" : "Removed from Favourites");
            toast_frames = 90; toast_ok = 1;
            if (sys == V_FAV && sel >= S->count && sel) sel--;
        }
        if (pressed & SCE_CTRL_CROSS) {
            game_sel[sys] = sel;
            video_stop_owned(OWN_PREVIEW); playing[0] = 0;
            int rc = launch(&S->games[sel]);
            if (rc >= 0) {
                Game played = S->games[sel];
                note_played(&played);
                if (sys == V_RECENT) sel = 0;
            }
            /* Stay open: the system closes us when its "will close"
             * dialog is confirmed (the SceShell bridge taps OK); if it is
             * cancelled we simply keep running. Exiting ourselves cancels
             * the launch (seen 2026-09-19). */
            if (rc >= 0) { snprintf(toast, sizeof(toast), "Starting %s\xE2\x80\xA6", S->games[sel].title); /* sel = the played game */ toast_frames = 600; toast_ok = 1; }
            else {
            snprintf(toast, sizeof(toast), "Could not launch (0x%08X)", rc);
            toast_frames = 180; toast_ok = 0;
            }
        }
    }
    if (!touching || !t_moved) {
        sys_pos = ease(sys_pos, ord_of(sys));
        game_pos = ease(game_pos, sel);
    }
    if (view == 1 && S->count) {   /* selection changed: cross-fade the art */
        Game *cur = &S->games[sel < S->count ? sel : 0];
        if (have_shown && (strcmp(shown.title, cur->title) || shown.origin != cur->origin)) {
            fade_from = shown; fade_t = 1.0f; has_fade = 1;
        }
        shown = *cur;
        have_shown = 1;
    }
    if (fade_t > 0) { fade_t -= 0.10f; if (fade_t < 0) fade_t = 0; }

    /* which video should be playing */
    const char *want = NULL;
    /* The reel is silent, and after 5 minutes it stops and the screen goes
     * dark (any button still wakes it). */
    int asleep = attract && attract_frames > 60 * 60 * 5;
    if (asleep) {
        if (playing[0]) { video_stop_owned(OWN_PREVIEW); playing[0] = 0; }
        fullscreen = 1;
        vita2d_draw_rectangle(0, 0, W, H, RGBA8(0, 0, 0, 255));
        attract_frames++;
        return;
    }
    if (attract) {
        if (attract_frames++ % (60 * 15) == 0) {
            for (int tries = 0; tries < 200; ++tries) {  /* random game that has a preview */
                int s2 = VIRT + rand() % (nsys - VIRT);
                if (!systems[s2].count) continue;
                int g2 = rand() % systems[s2].count;
                if (*systems[s2].games[g2].video) { a_sys = s2; a_sel = g2; break; }
            }
        }
        want = systems[a_sys].games[a_sel].video;
    } else if (view == 1) {
        int key = sys * 100000 + sel;
        if (key != last_key) { last_key = key; settle = 0; }
        if (++settle > 48 && *S->games[sel].video) want = S->games[sel].video;
    }
    /* Music or a movie owns the player: no previews until it stops. */
    if (video_owner() == OWN_MUSIC || video_owner() == OWN_MOVIE) want = NULL;
    if (!want || !*want) { if (playing[0]) { video_stop_owned(OWN_PREVIEW); playing[0] = 0; } }
    else if (strcmp(want, playing) != 0 && exists(want)) {
        snprintf(playing, sizeof(playing), "%s", want);
        if (video_play(playing, 0) < 0) playing[0] = 0;   /* always silent, attract mode too */
    }
    vita2d_texture *frame = playing[0] ? video_frame() : NULL;

    if (attract) {
        Game *ag = &systems[a_sys].games[a_sel];
        fullscreen = 1;                    /* the reel takes the whole screen */
        vita2d_draw_rectangle(0, 0, W, H, RGBA8(0, 0, 0, 255));
        if (frame) draw_fit(frame, 0, 0, W, H, WHITE, 0);
        else if (tex(ag->bg)) draw_fit(tex(ag->bg), 0, 0, W, H, WHITE, 1);
        for (int i = 0; i < 10; ++i)
            vita2d_draw_rectangle(0, H - 170 + i * 17, W, 17, RGBA8(0, 0, 0, 20 + i * 18));
        vita2d_texture *lg = tex(ag->logo);
        if (lg) draw_fit(lg, 36, H - 150, 360, 90, WHITE, 0);
        else uifont_draw(bold, 36, H - 80, WHITE, 30, ag->title);
        char line[160];
        snprintf(line, sizeof(line), "%s%s%s", systems[a_sys].name, *ag->year ? "  \xC2\xB7  " : "", ag->year);
        uifont_draw(font, 36, H - 30, systems[a_sys].accent, 18, line);
        const char *reel_hint = "X play this  any button: browse";
        draw_hints(W - 36 - hints_width(reel_hint), H - 36, reel_hint, DIM, W);
        return;
    }

    if (view == 0) {
        Game *hero = S->count ? &S->games[game_sel[sys] < S->count ? game_sel[sys] : 0] : NULL;
        background(S->accent, hero ? tex(hero->bg[0] ? hero->bg : hero->cover) : NULL);

        /* system cards */
        for (int i = 0; i < nsys; ++i) {
            if (!in_group(i)) continue;
            float d = ord_of(i) - sys_pos;
            if (d < -3 || d > 3) continue;
            float scale = 1.0f - 0.18f * (d < 0 ? -d : d);
            if (scale < 0.55f) scale = 0.55f;
            float cw = 250 * scale, ch = 300 * scale;
            float cx = W / 2 + d * 280 - cw / 2, cy = 250 - ch / 2 + 10;
            System *s = &systems[i];
            unsigned int a = (s->accent & 0x00FFFFFF) | ((i == sys ? 255u : 150u) << 24);
            vita2d_texture *hw = console_art(i);
            if (hw) {
                /* A real system: the hardware itself, standing on the floor. */
                draw_console(hw, cx, cw, cy + ch - 92 * scale, ch - 100 * scale, i == sys, frames_up);
                vita2d_draw_rectangle(cx + 14 * scale, cy + ch - 68 * scale, 36 * scale, 3 * scale, a);   /* clear of the name's caps */
            } else {
                vita2d_draw_rectangle(cx, cy, cw, ch, RGBA8(20, 22, 30, i == sys ? 240 : 170));
                vita2d_draw_rectangle(cx, cy, cw, 6 * scale, a);
                /* collage: first game's cover */
                vita2d_texture *t = s->count ? tex(s->games[game_sel[i] < s->count ? game_sel[i] : 0].cover) : NULL;
                if (t) draw_fit(t, cx + 12 * scale, cy + 20 * scale, cw - 24 * scale, ch - 90 * scale, RGBA8(255, 255, 255, i == sys ? 255 : 150), 0);
            }
            int fs = (int)(24 * scale);
            uifont_draw(bold, cx + 14 * scale, cy + ch - 40 * scale, i == sys ? WHITE : DIM, fs, s->name);
            char count[48];
            if (i == V_RECENT && s->count) snprintf(count, sizeof(count), "Last: %.36s", s->games[0].title);
            else if (s->count) snprintf(count, sizeof(count), "%d game%s", s->count, s->count == 1 ? "" : "s");
            else snprintf(count, sizeof(count), "%s", i == V_RECENT ? "Games you play appear here"
                          : (i == V_SEARCH ? "\xC3\x97 to search by title" : "\xE2\x96\xA1 on a game adds it"));
            uifont_draw(font, cx + 14 * scale, cy + ch - 14 * scale, DIM, (int)(17 * scale), count);
        }
        {
            /* The two ways in, as chips; then every name in this group, so all
             * the consoles are visible at once (the playtest asked for that). */
            const char *gname[2] = {"Consoles", "Collections"};
            const int CHIP_Y = 434;                        /* in the space under the carousel (playtest: balance) */
            int chx = 40;
            for (int g = 0; g < 2; ++g) {
                int w = uifont_width(font, 15, gname[g]) + 30;
                if (chips_focus && g == group) draw_focus(chx, CHIP_Y, w, 30, 1);
                draw_round_rect(chx, CHIP_Y, w, 30, 15, g == group ? RGBA8(245, 245, 250, 255) : RGBA8(255, 255, 255, 26));
                uifont_draw(font, chx + 15, CHIP_Y + 21, g == group ? RGBA8(15, 15, 20, 255) : WHITE, 15, gname[g]);
                chx += w + 10;
            }
            int y = CHIP_Y + 6, left = chx + 20, right = W - 110, cx = (left + right) / 2;
            char pos[24];
            snprintf(pos, sizeof(pos), "%d / %d", ord_of(sys) + 1, group_size());
            uifont_draw(font, W - 36 - uifont_width(font, 17, pos), y + 15, DIM, 17, pos);
            float x = cx;
            for (int i = sys; i < nsys && x < right; ++i) {         /* current, then right */
                if (!in_group(i)) continue;
                int w = uifont_width(font, 15, systems[i].name);
                if (i == sys) x = cx - w / 2;
                else if (x + w > right) break;
                uifont_draw(font, x, y + 14, i == sys ? WHITE : FAINT, 15, systems[i].name);
                if (i == sys) vita2d_draw_rectangle(x, y + 20, w, 2, systems[i].accent);
                x += w + 22;
            }
            x = cx - uifont_width(font, 15, systems[sys].name) / 2 - 22;
            for (int i = sys - 1; i >= 0 && x > left; --i) {          /* and left */
                if (!in_group(i)) continue;
                int w = uifont_width(font, 15, systems[i].name);
                if (x - w < left) break;
                x -= w;
                uifont_draw(font, x, y + 14, FAINT, 15, systems[i].name);
                x -= 22;
            }
        }
        hint = chips_focus ? "\xE2\x86\x90 \xE2\x86\x92  Consoles or Collections     \xC3\x97  done     L R  tabs"
                           : "\xE2\x86\x90 \xE2\x86\x92  choose     \xC3\x97  open     DOWN  Consoles / Collections     \xE2\x96\xB3  sort     SELECT  search     L R  tabs";
    } else {
        Game *g = &S->games[sel];
        /* The gameplay preview plays big, behind everything (it was fitted
         * into the 300 px cover square, far too small to see: playtest). */
        if (frame) background_a(S->accent, frame, 150);
        else background(S->accent, tex(g->bg[0] ? g->bg : g->cover));
        char head[96];
        snprintf(head, sizeof(head), "%s   %d / %d%s%s", S->name, sel + 1, S->count,
                 is_fav(g) ? "   \xE2\x98\x85" : "", sort_mode == 1 ? "   by year" : (sort_mode == 2 ? "   most played" : ""));
        snprintf(context, sizeof(context), "%s", head);
        /* hero cover + details */
        vita2d_texture *cover = tex(g->cover);
        unsigned int in_a = (unsigned int)(255 * (1.0f - fade_t));
        unsigned int fade_in = RGBA8(255, 255, 255, in_a);
        int slide = (int)(fade_t * 22);
        vita2d_draw_rectangle(36, 78, 300, 300, RGBA8(0, 0, 0, 90));
        if (has_fade && fade_t > 0) {
            vita2d_texture *old = tex(fade_from.cover);
            if (old) draw_fit(old, 36, 78, 300, 300, RGBA8(255, 255, 255, (unsigned int)(255 * fade_t)), 0);
        }
        if (cover) draw_fit(cover, 36, 78, 300, 300, fade_in, 0);
        else if (frame) draw_fit(frame, 36, 78, 300, 300, fade_in, 0);
        else if (!(has_fade && fade_t > 0.5f)) {
            vita2d_draw_rectangle(36, 78, 300, 300, (S->accent & 0x00FFFFFF) | 0x38000000u);
            vita2d_draw_rectangle(36, 78, 300, 6, S->accent);
            uifont_draw(font, 58, 122, S->accent, 16, S->name);
            draw_wrapped(bold, g->title, 58, 170, 256, 26, 5, WHITE);
        }
        int tx = 366 + slide;
        vita2d_texture *logo = tex(g->logo);
        int ty = 110;
        if (logo) { draw_fit(logo, tx, 80, 400, 80, fade_in, 0); ty = 190; }
        else { draw_wrapped(bold, g->title, tx, ty, W - tx - 40, 30, 2, fade_in); ty += 84; }
        char meta[192] = {0};
        int mo = sys < VIRT ? snprintf(meta, sizeof(meta), "%s%s", systems[g->origin].name, (*g->year || *g->genre) ? "  \xC2\xB7  " : "") : 0;
        snprintf(meta + mo, sizeof(meta) - mo, "%s%s%s%s%s", g->year, (*g->year && *g->genre) ? "  \xC2\xB7  " : "", g->genre,
                 *g->players ? "  \xC2\xB7  " : "", *g->players ? g->players : "");
        if (*meta) { uifont_draw(font, tx, ty, (S->accent & 0x00FFFFFF) | (in_a << 24), 18, meta); ty += 34; }
        if (*g->desc) draw_wrapped(font, g->desc, tx, ty, W - tx - 40, 17, 7, RGBA8(235, 238, 245, in_a * 220 / 255));
        /* cover strip */
        for (int i = sel - 5; i <= sel + 5; ++i) {
            if (i < 0 || i >= S->count) continue;
            float d = i - game_pos;
            float size = i == sel ? 96 : 76;
            float x = W / 2 - size / 2 + d * 92, y = H - 60 - size;
            vita2d_texture *t = tex(S->games[i].cover);
            vita2d_draw_rectangle(x - 3, y - 3, size + 6, size + 6, i == sel ? S->accent : RGBA8(255, 255, 255, 30));
            vita2d_draw_rectangle(x, y, size, size, RGBA8(20, 22, 30, 255));
            if (t) draw_fit(t, x, y, size, size, i == sel ? WHITE : RGBA8(255, 255, 255, 140), 0);
            else {
                char ini[3] = {0};
                const char *n = S->games[i].title;
                ini[0] = n[0];
                for (const char *q = n + 1; *q; ++q)
                    if ((q[-1] == ' ' || q[-1] == '_' || q[-1] == '-') && ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9'))) { ini[1] = *q; break; }
                if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32;
                if (ini[1] >= 'a' && ini[1] <= 'z') ini[1] -= 32;
                int fs = (int)(size * 0.36f);
                uifont_draw(bold, x + size / 2 - uifont_width(bold, fs, ini) / 2, y + size / 2 + fs / 3,
                                      i == sel ? S->accent : DIM, fs, ini);
            }
        }
        if (confirm) {
            const char *rom = rom_path(g);
            int app = !strcmp(g->kind, "app");
            vita2d_draw_rectangle(0, 0, W, H, RGBA8(15, 17, 24, 190));
            draw_round_rect(120, 160, W - 240, 230, 22, RGBA8(30, 34, 46, 250));
            uifont_draw(bold, 150, 212, WHITE, 26, "Remove this game?");
            draw_wrapped(font, g->title, 150, 250, W - 300, 20, 2, DIM);
            draw_hints(150, 312, "X Remove from the menu", WHITE, W - 150);
            draw_hints(150, 344, rom ? "[] Remove and delete the file from the Vita"
                                 : app ? "[] Remove and uninstall it from the Vita" : "[] (no file to delete)",
                       rom || app ? WHITE : FAINT, W - 150);
            draw_hints(150, 376, "O Cancel", DIM, W - 150);
        }
        hint = "X play  O back  /\\ details  [] favourite  START remove  SELECT search";
    }
    if (help) {
        /* Two hand-split columns: sections must not break across them. */
        static const char *left[][2] = {
            {"Browsing", ""},
            {"\xE2\x86\x90 \xE2\x86\x92  or swipe", "move through games"},
            {"\xE2\x86\x91 \xE2\x86\x93", "jump to the next letter"},
            {"L / R", "switch Home's tabs"},
            {"\xC3\x97  or tap the art", "play"},
            {"\xE2\x97\x8B", "back to the shelves"},
            {"", ""},
            {"Lists", ""},
            {"Continue", "what you played last"},
            {"\xE2\x96\xA1", "favourite / unfavourite"},
            {"SELECT", "search everything, from any tab"},
            {"\xE2\x96\xB3 on the shelves", "sort by name, year, plays"},
        };
        static const char *right[][2] = {
            {"Managing", ""},
            {"START on a game", "remove it from the menu"},
            {"\xE2\x96\xA1 at that prompt", "also delete the file"},
            {"\xE2\x96\xB3", "surprise me"},
            {"", ""},
            {"While a game runs", ""},
            {"L + R + SELECT", "quit back here"},
            {"START + SELECT", "RetroArch's own menu"},
            {"", ""},
            {"Idle for 30s", "gameplay reel starts"},
            {"\xC3\x97  in the reel", "play what you see"},
        };
        vita2d_draw_rectangle(0, 0, W, H, RGBA8(6, 7, 10, 225));
        uifont_draw(bold, 60, 60, WHITE, 26, "How this works");
        for (int col = 0; col < 2; ++col) {
            const char *(*rows)[2] = col ? right : left;
            int n = col ? (int)(sizeof(right) / sizeof(right[0])) : (int)(sizeof(left) / sizeof(left[0]));
            int x = col ? 520 : 60;
            for (int i = 0; i < n; ++i) {
                int y = 110 + i * 26;
                if (!*rows[i][1] && *rows[i][0])
                    uifont_draw(bold, x, y, systems[sys].accent, 19, rows[i][0]);
                else if (*rows[i][0]) {
                    uifont_draw(font, x, y, WHITE, 18, rows[i][0]);
                    uifont_draw(font, x + 165, y, DIM, 18, rows[i][1]);
                }
            }
        }
        uifont_draw(font, 60, H - 22, DIM, 18, "any button   close");
    }
    if (toast_frames > 0) {
        toast_frames--;
        int tw = uifont_width(font, 20, toast);
        vita2d_draw_rectangle(W / 2 - tw / 2 - 20, 470, tw + 40, 40, toast_ok ? RGBA8(28, 32, 44, 235) : RGBA8(180, 40, 50, 230));
        if (toast_ok) vita2d_draw_rectangle(W / 2 - tw / 2 - 20, 470, 4, 40, S->accent);
        uifont_draw(font, W / 2 - tw / 2, 497, WHITE, 20, toast);
    }
}

int play_fullscreen(void) { return fullscreen; }

/* PS during the attract reel: back to exactly where you were (a second PS
 * then goes Home, like the Switch). Returns 1 if there was a reel to leave. */
int play_wake(void) {
    if (!attract) return 0;
    attract = 0; idle = 0;
    video_stop_owned(OWN_PREVIEW);
    playing[0] = 0;
    sys = pre_sys; sel = pre_sel; view = pre_view;
    if (view && sel < systems[sys].count) game_pos = sel;
    return 1;
}
const char *play_context(void) { return context; }
const char *play_hint(void) { return hint_override ? hint_override : hint; }

/* Leaving the tab stops the preview so it does not play behind other tabs. */
void play_leave(void) {
    STAGE("play: leave");
    video_stop_owned(OWN_PREVIEW);
    playing[0] = 0;
    attract = 0;
    idle = 0;
}

void play_home(void) {
    if (view == 1 && sel < systems[sys].count) game_sel[sys] = sel;
    if (details) { details = 0; details_free(); }
    hint_override = NULL;
    view = 0;
    attract = 0;
    idle = 0;
}
