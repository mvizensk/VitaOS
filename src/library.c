/* The game library, found on the card (VitaOS: no Mac-side catalog needed).
 *
 * Writes the files Play already reads: systems.tsv and games/<id>.tsv under
 * ux0:data/arcadehub/. Sources, in order, each game once:
 *   1. RetroArch playlists (ux0:data/retroarch/playlists/*.lpl): label, ROM,
 *      core, and the playlist's system.
 *   2. ROM folders: RetroFlow's (ux0:data/RetroFlow/ROMS/<system>/), ux0:roms,
 *      uma0:roms and the like, one folder per system, named the RetroArch way
 *      ("Nintendo - Game Boy") or short ("gb").
 *   3. PSP games in ux0:pspemu (when RetroFlow's launcher bubble is there).
 *   4. Installed Vita games (retail title IDs, and homebrew the store lists
 *      as a game or port).
 * Box art: RetroFlow's COVERS/<system>/<name>.png or RetroArch's thumbnails
 * (Named_Boxarts, Named_Snaps for the backdrop). Folders are listed once, and
 * lookups happen in memory.
 *
 * A catalog built by the developer's Mac tool (build_catalog.py) has no
 * "#vitaos" first line, and is never touched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <curl/curl.h>
#include <zlib.h>
#include "library.h"
#include "apps.h"

static char ROOT_[64] = "ux0:data/arcadehub/";
#define ROOT ROOT_
#define MARK "#vitaos"
#define CORE "app0:/%s_libretro.self"
#define MAX_GAMES 12000

typedef struct {
    const char *id, *name, *accent, *core, *kind;   /* kind: "ra" RetroArch, "n64" DaedalusX64 */
    const char *exts;                                /* " nes fds zip " */
    const char *dirs[6];                             /* folder names, the RetroArch one first */
} Sys;

static const Sys SYS[] = {
    {"psx", "PlayStation", "7a8cff", "pcsx_rearmed", "ra", " cue chd pbp m3u ",
     {"Sony - PlayStation", "Sony - PlayStation - RetroArch", "psx", "ps1", "playstation", 0}},
    {"n64", "Nintendo 64", "3fb950", NULL, "n64", " z64 n64 v64 ", {"Nintendo - Nintendo 64", "n64", 0}},
    {"snes", "Super Nintendo", "a78bfa", "snes9x2005", "ra", " sfc smc zip 7z ",
     {"Nintendo - Super Nintendo Entertainment System", "Super_Nintendo", "snes", "sfc", 0}},
    {"genesis", "Genesis", "4cc9f0", "genesis_plus_gx", "ra", " md gen smd bin zip 7z ",
     {"Sega - Mega Drive - Genesis", "genesis", "megadrive", "md", 0}},
    {"nes", "NES", "ef4444", "fceumm", "ra", " nes fds unf zip 7z ",
     {"Nintendo - Nintendo Entertainment System", "nes", "famicom", "fc", 0}},
    {"gba", "Game Boy Advance", "8b5cf6", "gpsp", "ra", " gba zip 7z ", {"Nintendo - Game Boy Advance", "gba", 0}},
    {"gbc", "Game Boy Color", "facc15", "gambatte", "ra", " gbc zip 7z ", {"Nintendo - Game Boy Color", "gbc", 0}},
    {"gb", "Game Boy", "a3e635", "gambatte", "ra", " gb zip 7z ", {"Nintendo - Game Boy", "gb", 0}},
    {"sms", "Master System", "38bdf8", "genesis_plus_gx", "ra", " sms zip 7z ",
     {"Sega - Master System - Mark III", "Sega - Master System", "sms", "mastersystem", 0}},
    {"gg", "Game Gear", "0ea5e9", "genesis_plus_gx", "ra", " gg zip 7z ", {"Sega - Game Gear", "gamegear", "gg", 0}},
    {"segacd", "Sega CD", "60a5fa", "genesis_plus_gx", "ra", " cue chd ", {"Sega - Mega-CD - Sega CD", "segacd", "megacd", 0}},
    {"32x", "Sega 32X", "f97316", "picodrive", "ra", " 32x zip 7z ", {"Sega - 32X", "32x", "sega32x", 0}},
    {"pce", "PC Engine", "f43f5e", "mednafen_pce_fast", "ra", " pce zip 7z ",
     {"NEC - PC Engine", "NEC - TurboGrafx 16", "pce", "tg16", 0}},
    {"pcecd", "PC Engine CD", "fb7185", "mednafen_pce_fast", "ra", " cue chd ",
     {"NEC - PC Engine CD", "NEC - TurboGrafx CD", "pcecd", 0}},
    {"a2600", "Atari 2600", "f59e0b", "stella2014", "ra", " a26 bin zip 7z ", {"Atari - 2600", "atari2600", "a2600", 0}},
    {"a5200", "Atari 5200", "fbbf24", "a5200", "ra", " a52 bin zip 7z ", {"Atari - 5200", "atari5200", "a5200", 0}},
    {"a7800", "Atari 7800", "fb923c", "prosystem", "ra", " a78 bin zip 7z ", {"Atari - 7800", "Atari_7800", "atari7800", "a7800", 0}},
    {"lynx", "Atari Lynx", "fdba74", "handy", "ra", " lnx zip 7z ", {"Atari - Lynx", "lynx", 0}},
    {"coleco", "ColecoVision", "94a3b8", "bluemsx", "ra", " col zip 7z ",
     {"Coleco - ColecoVision", "ColecoVision", "coleco", 0}},
    {"wswan", "WonderSwan", "e879f9", "mednafen_wswan", "ra", " ws wsc zip 7z ",
     {"Bandai - WonderSwan", "Bandai - WonderSwan Color", "WonderSwan", "wswan", 0}},
    {"ngpc", "Neo Geo Pocket", "a855f7", "race", "ra", " ngc ngp zip 7z ",
     {"SNK - Neo Geo Pocket Color", "SNK - Neo Geo Pocket", "ngpc", "ngp", 0}},
    {"vectrex", "Vectrex", "22d3ee", "vecx", "ra", " vec bin zip 7z ", {"GCE - Vectrex", "Vectrex", 0}},
    {"neogeo", "Neo Geo", "ef4444", "fbalpha2012_neogeo", "ra", " zip 7z ", {"SNK - Neo Geo - FBA 2012", "neogeo", 0}},
    {"arcade", "Arcade", "f472b6", "mame2003_plus", "ra", " zip 7z ", {"MAME 2003 Plus", "MAME 2003", "mame", "arcade", 0}},
    {"mame2000", "Arcade (MAME 2000)", "ec4899", "mame2000", "ra", " zip 7z ", {"MAME 2000", 0}},
    {"fba", "Arcade (FB Alpha)", "d946ef", "fbalpha2012", "ra", " zip 7z ", {"FBA 2012", "fba", 0}},
    {"msx", "MSX", "64748b", "bluemsx", "ra", " rom mx1 mx2 dsk zip 7z ", {"Microsoft - MSX", "Microsoft - MSX2", "MSX", "msx", 0}},
    {"zx", "ZX Spectrum", "22c55e", "fuse", "ra", " tzx tap z80 sna zip 7z ", {"Sinclair - ZX Spectrum", "zxspectrum", 0}},
    {"c64", "Commodore 64", "8b5cf6", "vice_x64", "ra", " d64 t64 prg crt tap zip 7z ", {"Commodore - 64", "c64", 0}},
    {"amiga", "Amiga", "f97316", "puae", "ra", " adf lha hdf zip 7z ", {"Commodore - Amiga", "amiga", 0}},
    {"atarist", "Atari ST", "94a3b8", "hatari", "ra", " st msa stx zip 7z ", {"Atari - ST", "atarist", 0}},
    {"pico8", "PICO-8", "fb7185", "retro8", "ra", " p8 png ", {"Lexaloffle Games - Pico-8", "pico8", 0}},
    {"dos", "DOS", "a3a3a3", "dosbox_pure", "ra", " zip dosz ", {"DOS", "dos", 0}},
};
#define NSYS (int)(sizeof(SYS) / sizeof(SYS[0]))

static const char *const ROM_ROOTS[] = {
    "ux0:data/RetroFlow/ROMS", "ux0:roms", "ux0:ROMS", "ux0:data/roms", "uma0:roms", "uma0:data/RetroFlow/ROMS", 0,
};

/* ---------- small helpers ---------- */

typedef struct { char *title, *kind, *a1, *a2, *cover, *bg; int sys; } Row;
static Row *rows;
static int nrows;
static volatile int *progress;

static int exists(const char *p) { SceIoStat st; return sceIoGetstat(p, &st) >= 0; }

static char *lib_dup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }

static int has_ext(const char *name, const char *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot || strlen(dot) > 6) return 0;
    char key[10];
    snprintf(key, sizeof(key), " %s ", dot + 1);
    for (char *k = key; *k; ++k) if (*k >= 'A' && *k <= 'Z') *k += 32;
    return strstr(exts, key) != NULL;
}

/* "Legend of Zelda, The (USA) (Rev 1).nes" -> "The Legend of Zelda" */
static void nice_title(const char *file, char *out, int max) {
    char t[256];
    snprintf(t, sizeof(t), "%s", file);
    char *dot = strrchr(t, '.');
    if (dot) *dot = 0;
    for (char *p = t; *p; ++p)                       /* drop "(USA)", "[!]" and what follows */
        if ((*p == '(' || *p == '[') && p > t) { *p = 0; break; }
    int n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '_')) t[--n] = 0;
    for (char *p = t; *p; ++p) if (*p == '_') *p = ' ';
    char *comma = strstr(t, ", The");
    if (comma && comma[5] == 0) { *comma = 0; snprintf(out, max, "The %s", t); return; }
    snprintf(out, max, "%s", t);
}

/* RetroArch's thumbnail names: these characters become underscores. */
static void thumb_name(const char *label, char *out, int max) {
    int i = 0;
    for (; label[i] && i < max - 1; ++i) out[i] = strchr("&*/:`<>?\\|\"", label[i]) ? '_' : label[i];
    out[i] = 0;
}

static int sys_for_name(const char *name) {
    for (int s = 0; s < NSYS; ++s)
        for (int k = 0; SYS[s].dirs[k]; ++k)
            if (!strcasecmp(name, SYS[s].dirs[k])) return s;
    return -1;
}

/* A folder's file names, lowercase-insensitively searchable (box art lookups). */
typedef struct { char **names; int n; } List;

static List list_dir(const char *dir) {
    List l = {0, 0};
    SceUID d = sceIoDopen(dir);
    if (d < 0) return l;
    int cap = 0;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (l.n == cap) { cap = cap ? cap * 2 : 64; l.names = realloc(l.names, cap * sizeof(char *)); }
        l.names[l.n++] = lib_dup(e.d_name);
    }
    sceIoDclose(d);
    return l;
}

static void list_free(List *l) { for (int i = 0; i < l->n; ++i) free(l->names[i]); free(l->names); l->names = 0; l->n = 0; }

static int list_has(const List *l, const char *name) {
    for (int i = 0; i < l->n; ++i) if (!strcasecmp(l->names[i], name)) return 1;
    return 0;
}

/* Paths already listed (a hash set: comparing each ROM with every other one
 * was quadratic, seconds with thousands of games). */
#define SEEN 16384
static unsigned int seen[SEEN];                       /* FNV hashes, 0 = empty */

static unsigned int hash_path(const char *p) {
    unsigned int h = 2166136261u;
    for (; *p; ++p) { unsigned char c = (unsigned char)*p; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 16777619u; }
    return h ? h : 1;
}

static int seen_rom(const char *path) {
    unsigned int h = hash_path(path);
    for (unsigned int i = h % SEEN, k = 0; k < SEEN; i = (i + 1) % SEEN, ++k) {
        if (!seen[i]) return 0;
        if (seen[i] == h) return 1;
    }
    return 0;
}

static void mark_seen(const char *path) {
    unsigned int h = hash_path(path);
    for (unsigned int i = h % SEEN, k = 0; k < SEEN; i = (i + 1) % SEEN, ++k)
        if (!seen[i] || seen[i] == h) { seen[i] = h; return; }
}

/* The same ROM file in two folders is one game: keyed by system + file name. */
static const char *name_key(int sys, const char *path) {
    static char k[300];
    const char *f = strrchr(path, '/');
    snprintf(k, sizeof(k), "%d|%s", sys, f ? f + 1 : path);
    return k;
}

static void add(int sys, const char *title, const char *kind, const char *a1, const char *a2, const char *cover, const char *bg) {
    if (nrows >= MAX_GAMES) return;
    Row *r = &rows[nrows++];
    r->sys = sys; r->title = lib_dup(title); r->kind = lib_dup(kind); r->a1 = lib_dup(a1); r->a2 = lib_dup(a2);
    r->cover = lib_dup(cover); r->bg = lib_dup(bg);
    mark_seen(!strcmp(kind, "ra") ? a2 : a1);
    if (!strcmp(kind, "ra")) mark_seen(name_key(sys, a2));
    if (progress) *progress = nrows;
}

/* Box art for a ROM stem: RetroFlow's covers, then RetroArch's thumbnails. */
typedef struct { List rf_covers, rf_std, ra_box, ra_snap; char rf_dir[160], rf_std_dir[160], ra_dir[160]; } Art;

static void art_open(Art *a, int s, const char *folder) {
    memset(a, 0, sizeof(*a));
    snprintf(a->rf_dir, sizeof(a->rf_dir), "ux0:data/RetroFlow/COVERS/%s", folder);
    a->rf_covers = list_dir(a->rf_dir);
    if (strcasecmp(folder, SYS[s].dirs[0])) {         /* "Sony - PlayStation - RetroArch" has its covers under "Sony - PlayStation" */
        snprintf(a->rf_std_dir, sizeof(a->rf_std_dir), "ux0:data/RetroFlow/COVERS/%s", SYS[s].dirs[0]);
        a->rf_std = list_dir(a->rf_std_dir);
    }
    snprintf(a->ra_dir, sizeof(a->ra_dir), "ux0:data/retroarch/thumbnails/%s", SYS[s].dirs[0]);
    char p[200];
    snprintf(p, sizeof(p), "%s/Named_Boxarts", a->ra_dir);
    a->ra_box = list_dir(p);
    snprintf(p, sizeof(p), "%s/Named_Snaps", a->ra_dir);
    a->ra_snap = list_dir(p);
}

static void art_close(Art *a) { list_free(&a->rf_covers); list_free(&a->rf_std); list_free(&a->ra_box); list_free(&a->ra_snap); }

static void art_for(const Art *a, const char *stem, char *cover, char *bg) {
    char f[260], tn[256];
    cover[0] = bg[0] = 0;
    snprintf(f, sizeof(f), "%s.png", stem);
    if (list_has(&a->rf_covers, f)) snprintf(cover, 300, "%s/%s", a->rf_dir, f);
    else if (list_has(&a->rf_std, f)) snprintf(cover, 300, "%s/%s", a->rf_std_dir, f);
    thumb_name(stem, tn, sizeof(tn));
    snprintf(f, sizeof(f), "%s.png", tn);
    if (!cover[0] && list_has(&a->ra_box, f)) snprintf(cover, 300, "%s/Named_Boxarts/%s", a->ra_dir, f);
    if (list_has(&a->ra_snap, f)) snprintf(bg, 300, "%s/Named_Snaps/%s", a->ra_dir, f);
}

/* ---------- 1. RetroArch playlists ---------- */

static char *json_str(char *p, const char *key, char *out, int max) {
    char k[32];
    snprintf(k, sizeof(k), "\"%s\"", key);
    char *at = strstr(p, k);
    if (!at) { out[0] = 0; return NULL; }
    at = strchr(at + strlen(k), '"');
    if (!at) { out[0] = 0; return NULL; }
    int n = 0;
    for (++at; *at && *at != '"' && n < max - 1; ++at) {
        if (*at == '\\' && at[1]) ++at;
        out[n++] = *at;
    }
    out[n] = 0;
    return at;
}

static void scan_playlists(void) {
    List pl = list_dir("ux0:data/retroarch/playlists");
    for (int i = 0; i < pl.n; ++i) {
        const char *name = pl.names[i];
        int len = strlen(name);
        if (len < 5 || strcasecmp(name + len - 4, ".lpl")) continue;
        char sysname[160];
        snprintf(sysname, sizeof(sysname), "%.*s", len - 4, name);
        int s = sys_for_name(sysname);
        if (s < 0 || !SYS[s].core) continue;
        char path[256];
        snprintf(path, sizeof(path), "ux0:data/retroarch/playlists/%s", name);
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) continue;
        int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        char *text = size > 0 ? malloc(size + 1) : NULL;
        int n = text ? sceIoRead(fd, text, size) : 0;
        sceIoClose(fd);
        if (!text) continue;
        text[n > 0 ? n : 0] = 0;
        Art art;
        art_open(&art, s, SYS[s].dirs[0]);
        for (char *item = strstr(text, "\"path\""); item; item = strstr(item + 6, "\"path\"")) {
            char rom[256], label[160], core[160], title[160], cover[300], bg[300];
            json_str(item, "path", rom, sizeof(rom));
            json_str(item, "label", label, sizeof(label));
            json_str(item, "core_path", core, sizeof(core));
            if (!rom[0] || seen_rom(rom) || seen_rom(name_key(s, rom)) || !exists(rom)) continue;
            if (!core[0] || !strcmp(core, "DETECT")) snprintf(core, sizeof(core), CORE, SYS[s].core);
            if (label[0]) snprintf(title, sizeof(title), "%s", label);
            else { const char *f = strrchr(rom, '/'); nice_title(f ? f + 1 : rom, title, sizeof(title)); }
            art_for(&art, label[0] ? label : title, cover, bg);
            char t2[160];
            nice_title(title, t2, sizeof(t2));
            add(s, t2, "ra", core, rom, cover, bg);
        }
        art_close(&art);
        free(text);
    }
    list_free(&pl);
}

/* ---------- 2. ROM folders ---------- */

static void scan_folder(int s, const char *dir, const char *folder, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    Art art;
    art_open(&art, s, folder);
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) { if (depth < 1) scan_folder(s, path, folder, depth + 1); continue; }
        if (!has_ext(e.d_name, SYS[s].exts) || seen_rom(path) || seen_rom(name_key(s, path))) continue;
        char stem[256], title[160], cover[300], bg[300];
        snprintf(stem, sizeof(stem), "%s", e.d_name);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        nice_title(e.d_name, title, sizeof(title));
        art_for(&art, stem, cover, bg);
        if (!strcmp(SYS[s].kind, "n64")) add(s, title, "n64", path, "", cover, bg);
        else {
            char core[96];
            snprintf(core, sizeof(core), CORE, SYS[s].core);
            add(s, title, "ra", core, path, cover, bg);
        }
    }
    sceIoDclose(d);
    art_close(&art);
}

static void scan_roms(void) {
    for (int r = 0; ROM_ROOTS[r]; ++r) {
        List l = list_dir(ROM_ROOTS[r]);
        for (int i = 0; i < l.n; ++i) {
            int s = sys_for_name(l.names[i]);
            if (s < 0) continue;
            if (!strcmp(SYS[s].kind, "n64") && !exists("ux0:app/DEDALOX64")) continue;
            if (SYS[s].core) {                            /* only systems whose RetroArch core is installed */
                char cp[96];
                snprintf(cp, sizeof(cp), "ux0:app/RETROVITA/%s_libretro.self", SYS[s].core);
                if (!exists(cp)) continue;
            }
            char dir[256];
            snprintf(dir, sizeof(dir), "%s/%s", ROM_ROOTS[r], l.names[i]);
            scan_folder(s, dir, l.names[i], 0);
        }
        list_free(&l);
    }
}

/* ---------- 3. PSP, 4. Vita ---------- */

#define PSP (-2)
#define VITA (-3)

/* An EBOOT.PBP's own PARAM.SFO: its TITLE and CATEGORY ("ME" = a PS1 classic
 * run by Adrenaline, anything else = PSP). The PBP header points at it. */
static int pbp_info(const char *path, char *title, int tmax, char *cat, int cmax) {
    title[0] = cat[0] = 0;
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    unsigned char h[40], buf[4096];
    int ok = sceIoRead(fd, h, sizeof(h)) == (int)sizeof(h) && !memcmp(h, "\0PBP", 4);
    unsigned int off = h[8] | h[9] << 8 | h[10] << 16 | (unsigned)h[11] << 24;
    unsigned int end = h[12] | h[13] << 8 | h[14] << 16 | (unsigned)h[15] << 24;
    int n = 0;
    if (ok && end > off && end - off <= sizeof(buf)) {
        sceIoLseek(fd, off, SCE_SEEK_SET);
        n = sceIoRead(fd, buf, end - off);
    }
    sceIoClose(fd);
    if (n < 20 || memcmp(buf, "\0PSF", 4)) return -1;
    unsigned int keys = buf[8] | buf[9] << 8 | buf[10] << 16 | (unsigned)buf[11] << 24;
    unsigned int data = buf[12] | buf[13] << 8 | buf[14] << 16 | (unsigned)buf[15] << 24;
    unsigned int count = buf[16] | buf[17] << 8 | buf[18] << 16 | (unsigned)buf[19] << 24;
    for (unsigned int i = 0; i < count && 20 + i * 16 + 16 <= (unsigned)n; ++i) {
        unsigned char *e = buf + 20 + i * 16;
        unsigned int key = e[0] | e[1] << 8, len = e[4] | e[5] << 8 | e[6] << 16;
        unsigned int vo = e[12] | e[13] << 8 | e[14] << 16 | (unsigned)e[15] << 24;
        if (keys + key >= (unsigned)n || data + vo + len > (unsigned)n) continue;
        const char *k = (const char *)buf + keys + key, *v = (const char *)buf + data + vo;
        if (!strcmp(k, "TITLE")) snprintf(title, tmax, "%.*s", (int)len, v);
        else if (!strcmp(k, "CATEGORY")) snprintf(cat, cmax, "%.*s", (int)len, v);
    }
    return title[0] ? 0 : -1;
}

static int sys_by_id(const char *id) {
    for (int s = 0; s < NSYS; ++s) if (!strcmp(SYS[s].id, id)) return s;
    return -1;
}

/* ---------- PSP art from the game itself ----------
 * Every PSP game carries its own ICON0.PNG (the XMB icon), PIC1.PNG (the XMB
 * backdrop) and PARAM.SFO (the real title): in an ISO under PSP_GAME/, in a
 * CSO the same ISO compressed block by block, in an EBOOT.PBP at offsets its
 * header lists. Read once, saved under media/psp/, so every PSP game gets
 * art with no internet (Reddit, 2026-09-29: "PSP has 0 game art"). */

typedef struct { SceUID fd; int cso; unsigned int align, blocks; } Disc;

static int disc_sector(Disc *d, unsigned int lba, unsigned char *out) {
    if (!d->cso) {
        if (sceIoLseek(d->fd, (SceOff)lba * 2048, SCE_SEEK_SET) < 0) return -1;
        return sceIoRead(d->fd, out, 2048) == 2048 ? 0 : -1;
    }
    if (lba >= d->blocks) return -1;
    unsigned char ix[8];
    if (sceIoLseek(d->fd, 0x18 + (SceOff)lba * 4, SCE_SEEK_SET) < 0 || sceIoRead(d->fd, ix, 8) != 8) return -1;
    unsigned int a = ix[0] | ix[1] << 8 | ix[2] << 16 | (unsigned)ix[3] << 24;
    unsigned int b = ix[4] | ix[5] << 8 | ix[6] << 16 | (unsigned)ix[7] << 24;
    SceOff pos = (SceOff)(a & 0x7FFFFFFF) << d->align, end = (SceOff)(b & 0x7FFFFFFF) << d->align;
    int len = (int)(end - pos);
    if (len <= 0 || len > 4096) return -1;
    static unsigned char raw[4096];
    if (sceIoLseek(d->fd, pos, SCE_SEEK_SET) < 0 || sceIoRead(d->fd, raw, len) != len) return -1;
    if (a & 0x80000000) { memcpy(out, raw, 2048); return 0; }      /* stored */
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) return -1;                   /* raw deflate */
    z.next_in = raw; z.avail_in = len; z.next_out = out; z.avail_out = 2048;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return (r == Z_STREAM_END || z.avail_out == 0) ? 0 : -1;
}

static int disc_open(Disc *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (d->fd < 0) return -1;
    unsigned char h[24];
    if (sceIoRead(d->fd, h, 24) != 24) { sceIoClose(d->fd); return -1; }
    if (!memcmp(h, "CISO", 4)) {
        unsigned long long total = 0;
        for (int i = 0; i < 8; ++i) total |= (unsigned long long)h[8 + i] << (8 * i);
        unsigned int bs = h[16] | h[17] << 8 | h[18] << 16 | (unsigned)h[19] << 24;
        if (bs != 2048) { sceIoClose(d->fd); return -1; }
        d->cso = 1;
        d->align = h[21];
        d->blocks = (unsigned int)(total / 2048);
    }
    return 0;
}

/* A file inside the disc's PSP_GAME folder, read whole (up to max bytes). */
static int disc_file(Disc *d, const char *name, unsigned char *out, int max) {
    static unsigned char sec[2048];
    if (disc_sector(d, 16, sec) < 0 || memcmp(sec + 1, "CD001", 5)) return -1;
    unsigned int dir = sec[156 + 2] | sec[156 + 3] << 8 | sec[156 + 4] << 16 | (unsigned)sec[156 + 5] << 24;
    unsigned int dsize = sec[156 + 10] | sec[156 + 11] << 8 | sec[156 + 12] << 16 | (unsigned)sec[156 + 13] << 24;
    for (int level = 0; level < 2; ++level) {           /* root -> PSP_GAME -> the file */
        const char *want = level ? name : "PSP_GAME";
        unsigned int found = 0, fsize = 0;
        for (unsigned int off = 0; off < dsize && !found; off += 2048) {
            if (disc_sector(d, dir + off / 2048, sec) < 0) return -1;
            for (int i = 0; i < 2048 && sec[i];) {
                int rl = sec[i], nl = sec[i + 32];
                if (rl < 34 || i + rl > 2048) break;
                const char *nm = (const char *)sec + i + 33;
                int wl = (int)strlen(want);
                if (nl >= wl && !strncasecmp(nm, want, wl) && (nl == wl || nm[wl] == ';')) {
                    found = sec[i + 2] | sec[i + 3] << 8 | sec[i + 4] << 16 | (unsigned)sec[i + 5] << 24;
                    fsize = sec[i + 10] | sec[i + 11] << 8 | sec[i + 12] << 16 | (unsigned)sec[i + 13] << 24;
                    break;
                }
                i += rl;
            }
        }
        if (!found) return -1;
        dir = found; dsize = fsize;
    }
    if ((int)dsize > max) return -1;
    for (unsigned int off = 0; off < dsize; off += 2048) {
        if (disc_sector(d, dir + off / 2048, sec) < 0) return -1;
        memcpy(out + off, sec, dsize - off < 2048 ? dsize - off : 2048);
    }
    return (int)dsize;
}

static int sfo_value(const unsigned char *buf, int n, const char *want, char *out, int max) {
    out[0] = 0;
    if (n < 20 || memcmp(buf, "\0PSF", 4)) return -1;
    unsigned int keys = buf[8] | buf[9] << 8 | buf[10] << 16 | (unsigned)buf[11] << 24;
    unsigned int data = buf[12] | buf[13] << 8 | buf[14] << 16 | (unsigned)buf[15] << 24;
    unsigned int count = buf[16] | buf[17] << 8 | buf[18] << 16 | (unsigned)buf[19] << 24;
    for (unsigned int i = 0; i < count && 20 + i * 16 + 16 <= (unsigned)n; ++i) {
        const unsigned char *e = buf + 20 + i * 16;
        unsigned int key = e[0] | e[1] << 8, len = e[4] | e[5] << 8 | e[6] << 16;
        unsigned int vo = e[12] | e[13] << 8 | e[14] << 16 | (unsigned)e[15] << 24;
        if (keys + key >= (unsigned)n || data + vo + len > (unsigned)n) continue;
        if (!strcmp((const char *)buf + keys + key, want)) {
            snprintf(out, max, "%.*s", (int)len, (const char *)buf + data + vo);
            for (char *p = out; *p; ++p) if (*p == '\n') *p = ' ';
            return 0;
        }
    }
    return -1;
}

static int save_blob(const char *path, const unsigned char *b, int n) {
    if (n < 8 || memcmp(b, "\x89PNG", 4)) return -1;
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return -1;
    int w = sceIoWrite(fd, b, n);
    sceIoClose(fd);
    return w == n ? 0 : -1;
}

/* Fills cover/bg with saved PNG paths and title with PARAM.SFO's, when found. */
static void psp_art(const char *path, const char *key, char *title, int tmax, char *cover, char *bg) {
    cover[0] = bg[0] = 0;
    char dir[96], ic[300], pc[300], tt[300];
    snprintf(dir, sizeof(dir), "%smedia", ROOT);
    sceIoMkdir(dir, 0777);
    snprintf(dir, sizeof(dir), "%smedia/psp", ROOT);
    sceIoMkdir(dir, 0777);
    char k[120];
    int j = 0;
    for (const char *c = key; *c && j < (int)sizeof(k) - 1; ++c) k[j++] = (isalnum((unsigned char)*c) ? *c : '_');
    k[j] = 0;
    snprintf(ic, sizeof(ic), "%s/%s-icon0.png", dir, k);
    snprintf(pc, sizeof(pc), "%s/%s-pic1.png", dir, k);
    snprintf(tt, sizeof(tt), "%s/%s-title.txt", dir, k);
    if (exists(ic)) {                                   /* done on an earlier scan */
        snprintf(cover, 300, "%s", ic);
        if (exists(pc)) snprintf(bg, 300, "%s", pc);
        SceUID fd = sceIoOpen(tt, SCE_O_RDONLY, 0);
        if (fd >= 0) { char t[160] = {0}; int n = sceIoRead(fd, t, sizeof(t) - 1); sceIoClose(fd); if (n > 0) snprintf(title, tmax, "%s", t); }
        return;
    }
    static unsigned char big[768 * 1024];
    const char *dot = strrchr(path, '.');
    int pbp = dot && !strcasecmp(dot, ".pbp");
    char t[160] = "";
    if (pbp) {
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) return;
        unsigned char h[40];
        if (sceIoRead(fd, h, 40) == 40 && !memcmp(h, "\0PBP", 4)) {
            unsigned int o[8];
            for (int i = 0; i < 8; ++i) o[i] = h[8 + i * 4] | h[9 + i * 4] << 8 | h[10 + i * 4] << 16 | (unsigned)h[11 + i * 4] << 24;
            /* o[0] param.sfo, o[1] icon0, o[2] icon1, o[3] pic0, o[4] pic1, o[5] snd0, o[6] data.psp */
            for (int which = 0; which < 2; ++which) {
                unsigned int a = which ? o[4] : o[1], b = which ? o[5] : o[2];
                if (b > a && b - a <= sizeof(big) && sceIoLseek(fd, a, SCE_SEEK_SET) >= 0 && sceIoRead(fd, big, b - a) == (int)(b - a))
                    save_blob(which ? pc : ic, big, b - a);
            }
        }
        sceIoClose(fd);
    } else {
        Disc d;
        if (disc_open(&d, path) < 0) return;
        int n = disc_file(&d, "ICON0.PNG", big, sizeof(big));
        if (n > 0) save_blob(ic, big, n);
        n = disc_file(&d, "PIC1.PNG", big, sizeof(big));
        if (n > 0) save_blob(pc, big, n);
        n = disc_file(&d, "PARAM.SFO", big, sizeof(big));
        if (n > 0 && sfo_value(big, n, "TITLE", t, sizeof(t)) == 0 && t[0]) {
            SceUID fd = sceIoOpen(tt, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            if (fd >= 0) { sceIoWrite(fd, t, strlen(t)); sceIoClose(fd); }
            snprintf(title, tmax, "%s", t);
        }
        sceIoClose(d.fd);
    }
    if (exists(ic)) snprintf(cover, 300, "%s", ic);
    if (exists(pc)) snprintf(bg, 300, "%s", pc);
}

/* PSP and PS1-on-Adrenaline games, wherever Adrenaline users keep them
 * (Reddit, 2026-09-27: "it won't recognise my PSP games"): ISO/CSO images in
 * pspemu/ISO on ux0 or uma0, and EBOOT.PBP folders in pspemu/PSP/GAME. PS1
 * classics (CATEGORY "ME") go to the PlayStation list, the rest to PSP. Needs
 * Adrenaline; RetroFlow's launcher, when present, boots a game directly. */
static void scan_psp(void) {
    if (!exists("ux0:app/PSPEMUCFW/eboot.bin")) return;   /* Adrenaline */
    int psx = sys_by_id("psx");
    static const char *const roots[] = {"ux0:pspemu", "uma0:pspemu", "imc0:pspemu", "xmc0:pspemu", "grw0:pspemu"};
    for (int r = 0; r < 5; ++r) {
        char dir[64];
        snprintf(dir, sizeof(dir), "%s/ISO", roots[r]);
        List l = list_dir(dir);
        for (int i = 0; i < l.n; ++i) {
            const char *nm = l.names[i];
            const char *dot = strrchr(nm, '.');
            char path[256];
            if (!dot || (strcasecmp(dot, ".iso") && strcasecmp(dot, ".cso"))) {
                /* one folder deep: ISO/Game Name/game.iso */
                char sub[160];
                snprintf(sub, sizeof(sub), "%s/%s", dir, nm);
                List s2 = list_dir(sub);
                for (int j = 0; j < s2.n; ++j) {
                    const char *d2 = strrchr(s2.names[j], '.');
                    if (!d2 || (strcasecmp(d2, ".iso") && strcasecmp(d2, ".cso"))) continue;
                    snprintf(path, sizeof(path), "%s/%s", sub, s2.names[j]);
                    if (seen_rom(path)) continue;
                    char title[160], cover[300], bg[300];
                    snprintf(title, sizeof(title), "%s", nm);
                    psp_art(path, s2.names[j], title, sizeof(title), cover, bg);
                    add(PSP, title, "psp", path, "", cover, bg);
                }
                list_free(&s2);
                continue;
            }
            snprintf(path, sizeof(path), "%s/%s", dir, nm);
            if (seen_rom(path)) continue;
            char title[160], cover[300], bg[300];
            snprintf(title, sizeof(title), "%.*s", (int)(dot - nm), nm);
            psp_art(path, nm, title, sizeof(title), cover, bg);   /* the disc's own title and art */
            add(PSP, title, "psp", path, "", cover, bg);
        }
        list_free(&l);
        snprintf(dir, sizeof(dir), "%s/PSP/GAME", roots[r]);
        l = list_dir(dir);
        for (int i = 0; i < l.n; ++i) {
            char eboot[256], title[160], cat[8];
            snprintf(eboot, sizeof(eboot), "%s/%s/EBOOT.PBP", dir, l.names[i]);
            if (!exists(eboot) || seen_rom(eboot)) continue;
            if (pbp_info(eboot, title, sizeof(title), cat, sizeof(cat)) < 0) snprintf(title, sizeof(title), "%s", l.names[i]);
            int ps1 = !strcmp(cat, "ME");
            char cover[300], bg[300], keep[160];
            snprintf(keep, sizeof(keep), "%s", title);
            psp_art(eboot, l.names[i], keep, sizeof(keep), cover, bg);
            add(ps1 && psx >= 0 ? psx : PSP, title, "psp", eboot, "", cover, bg);
        }
        list_free(&l);
    }
}

static void sfo_title(const char *tid, char *out, int max) {
    out[0] = 0;
    char path[96];
    snprintf(path, sizeof(path), "ux0:app/%s/sce_sys/param.sfo", tid);
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
        if (!strcmp((const char *)buf + keys + key, "TITLE")) {
            snprintf(out, max, "%.*s", (int)len, (const char *)buf + data + off);
            for (char *p = out; *p; ++p) if (*p == '\n') *p = ' ';
        }
    }
}

/* Homebrew the store calls a game (type 1) or a port (type 2). */
static int store_says_game(const char *store, const char *tid) {
    if (!store) return 0;
    char key[24];
    snprintf(key, sizeof(key), "\"%s\"", tid);
    const char *at = strstr(store, key);
    if (!at) return 0;
    const char *obj = at;
    while (obj > store && *obj != '{') --obj;         /* this entry's "type" */
    const char *type = strstr(obj, "\"type\"");
    if (!type || type > at + 4000) return 0;
    type = strchr(type + 6, '"');
    return type && (type[1] == '1' || type[1] == '2') && type[2] == '"';
}

static void scan_vita(void) {
    char *store = NULL;
    SceUID fd = sceIoOpen("ux0:data/arcadehub/store/apps.json", SCE_O_RDONLY, 0);
    if (fd >= 0) {
        int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        store = size > 0 ? malloc(size + 1) : NULL;
        int n = store ? sceIoRead(fd, store, size) : 0;
        if (store) store[n > 0 ? n : 0] = 0;
        sceIoClose(fd);
    }
    List l = list_dir("ux0:app");
    for (int i = 0; i < l.n; ++i) {
        const char *tid = l.names[i];
        if (strlen(tid) != 9 || !strncmp(tid, "MVZA", 4) || !strncmp(tid, "VITAOS", 6)) continue;
        int retail = !strncmp(tid, "PCS", 3);
        if (!retail && !store_says_game(store, tid)) continue;
        char title[160], icon[96], pic[96];
        sfo_title(tid, title, sizeof(title));
        if (!title[0]) continue;
        app_art(tid, "icon0.png", icon, sizeof(icon));   /* retail art is encrypted in ux0:app */
        app_art(tid, "pic0.png", pic, sizeof(pic));
        add(VITA, title, "app", tid, "", icon, pic);
    }
    list_free(&l);
    free(store);
}

/* ---------- output ---------- */

static int by_title(const void *a, const void *b) { return strcasecmp(((const Row *)a)->title, ((const Row *)b)->title); }

static void write_shelf(const char *id, int sys) {
    char path[128];
    snprintf(path, sizeof(path), "%sgames/%s.tsv", ROOT, id);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, MARK "\n", strlen(MARK) + 1);
    static char line[1600];
    for (int i = 0; i < nrows; ++i) {
        if (rows[i].sys != sys) continue;
        int n = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%s\t%s\t\t\t\t\t\t\n", rows[i].title, rows[i].kind, rows[i].a1,
                         rows[i].a2, rows[i].cover, rows[i].bg);
        sceIoWrite(fd, line, n);
    }
    sceIoClose(fd);
}

static int count_of(int sys) { int n = 0; for (int i = 0; i < nrows; ++i) n += rows[i].sys == sys; return n; }

int library_is_ours(void) {
    char head[16] = {0};
    char sp[96];
    snprintf(sp, sizeof(sp), "%ssystems.tsv", ROOT);
    SceUID fd = sceIoOpen(sp, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;                            /* none yet */
    sceIoRead(fd, head, sizeof(head) - 1);
    sceIoClose(fd);
    return !strncmp(head, MARK, strlen(MARK));
}

/* Testing on a Vita that has a Mac-built catalog: scan into another folder. */
int library_scan_into(const char *root, volatile int *found) {
    char keep[64];
    snprintf(keep, sizeof(keep), "%s", ROOT_);
    snprintf(ROOT_, sizeof(ROOT_), "%s", root);
    int n = library_scan(found);
    snprintf(ROOT_, sizeof(ROOT_), "%s", keep);
    return n;
}

static int art_cap;                                   /* per system; 0 = all (the test hook caps it) */
int library_art_into(const char *root, int cap, volatile int *done, volatile int *got) {
    char keep[64];
    snprintf(keep, sizeof(keep), "%s", ROOT_);
    snprintf(ROOT_, sizeof(ROOT_), "%s", root);
    art_cap = cap;
    int r = library_fetch_art(done, got);
    art_cap = 0;
    snprintf(ROOT_, sizeof(ROOT_), "%s", keep);
    return r;
}

int library_scan(volatile int *found) {
    if (library_is_ours() == 0) return 0;             /* a Mac-built catalog: leave it */
    progress = found;
    rows = calloc(MAX_GAMES, sizeof(Row));
    if (!rows) return -1;
    nrows = 0;
    memset(seen, 0, sizeof(seen));
    if (exists("ux0:app/RETROVITA")) { scan_playlists(); scan_roms(); }
    else scan_roms();                                 /* N64 via DaedalusX64 still counts */
    scan_psp();
    scan_vita();
    qsort(rows, nrows, sizeof(Row), by_title);

    char gd[96];
    snprintf(gd, sizeof(gd), "%.*s", (int)strlen(ROOT) - 1, ROOT);
    sceIoMkdir(gd, 0777);
    snprintf(gd, sizeof(gd), "%sgames", ROOT);
    sceIoMkdir(gd, 0777);
    static char sys_text[4096];
    int n = snprintf(sys_text, sizeof(sys_text), MARK "\n");
    if (count_of(VITA)) { write_shelf("vita", VITA); n += snprintf(sys_text + n, sizeof(sys_text) - n, "vita\tPS Vita\t60a5fa\n"); }
    if (count_of(PSP)) { write_shelf("psp", PSP); n += snprintf(sys_text + n, sizeof(sys_text) - n, "psp\tPSP\tc084fc\n"); }
    for (int s = 0; s < NSYS; ++s) {
        if (!count_of(s)) continue;
        write_shelf(SYS[s].id, s);
        n += snprintf(sys_text + n, sizeof(sys_text) - n, "%s\t%s\t%s\n", SYS[s].id, SYS[s].name, SYS[s].accent);
    }
    char sp[96];
    snprintf(sp, sizeof(sp), "%ssystems.tsv", ROOT);
    SceUID fd = sceIoOpen(sp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, sys_text, n); sceIoClose(fd); }
    int total = nrows;
    for (int i = 0; i < nrows; ++i) { free(rows[i].title); free(rows[i].kind); free(rows[i].a1); free(rows[i].a2); free(rows[i].cover); free(rows[i].bg); }
    free(rows);
    rows = NULL;
    progress = NULL;
    return total;
}

/* ---------- box art from libretro's thumbnail server ----------
 * What RetroArch's own Thumbnails Updater does: for each game without art,
 * thumbnails.libretro.com/<system>/Named_Boxarts/<name>.png, saved where
 * RetroArch keeps them. The next scan picks them up. */

static void url_escape(const char *in, char *out, int max) {
    int n = 0;
    for (; *in && n < max - 4; ++in) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) out[n++] = c;
        else n += snprintf(out + n, max - n, "%%%02X", c);
    }
    out[n] = 0;
}

static FILE *dl_file;
static size_t dl_write(char *p, size_t s, size_t n, void *u) { (void)u; return fwrite(p, s, n, dl_file); }

static int fetch_png(CURL *c, const char *url, const char *dest) {
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.part", dest);
    dl_file = fopen(tmp, "wb");
    if (!dl_file) return -1;
    curl_easy_setopt(c, CURLOPT_URL, url);
    long code = 0;
    int ok = curl_easy_perform(c) == CURLE_OK;
    fclose(dl_file);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (!ok || code != 200) { sceIoRemove(tmp); return -1; }
    sceIoRemove(dest);
    sceIoRename(tmp, dest);
    return 0;
}

/* The server's folder listings, so short names ("Ace of Aces") can be matched
 * to the full No-Intro ones ("Ace of Aces (Europe, Brazil) (En)"). A system
 * can have two: WonderSwan's colour games are in "Bandai - WonderSwan Color". */
typedef struct { char *p; size_t len, cap; } Listing;
static Listing lst[2];
static size_t lst_write(char *p, size_t s, size_t n, void *u) {
    Listing *l = u;
    size_t k = s * n;
    if (l->len + k + 1 > l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256 * 1024;
        while (cap < l->len + k + 1) cap *= 2;
        if (cap > 8 * 1024 * 1024) return 0;
        char *q = realloc(l->p, cap);
        if (!q) return 0;
        l->p = q; l->cap = cap;
    }
    memcpy(l->p + l->len, p, k);
    l->len += k;
    l->p[l->len] = 0;
    return k;
}

static void url_unescape(const char *in, int len, char *out, int max) {
    int n = 0;
    for (int i = 0; i < len && n < max - 1; ++i) {
        if (in[i] == '%' && i + 2 < len) { char h[3] = {in[i + 1], in[i + 2], 0}; out[n++] = (char)strtol(h, NULL, 16); i += 2; }
        else out[n++] = in[i];
    }
    out[n] = 0;
}

/* The listing's best name for a ROM stem: exact, else same title before
 * " (", preferring USA, World, Europe, and not betas or prototypes. */
static int best_match(const Listing *l, const char *stem, char *out, int max) {
    char want[256];
    snprintf(want, sizeof(want), "%s", stem);
    char *paren = strstr(want, " (");
    if (paren) *paren = 0;
    char alt[260] = "";                               /* "The Legend of Zelda" is listed as "Legend of Zelda, The" */
    if (!strncasecmp(want, "The ", 4)) snprintf(alt, sizeof(alt), "%s, The", want + 4);
    int wl = strlen(want), al = strlen(alt), best = -1;
    char name[300];
    out[0] = 0;
    for (char *h = l->len ? strstr(l->p, "href=\"") : NULL; h; h = strstr(h + 6, "href=\"")) {
        char *q = h + 6, *e = strchr(q, '"');
        if (!e || e - q < 5 || strncmp(e - 4, ".png", 4)) continue;
        url_unescape(q, (int)(e - q) - 4, name, sizeof(name));
        if (!strcasecmp(name, stem)) { snprintf(out, max, "%s", name); return 1; }
        int hit = !strncasecmp(name, want, wl) && (!name[wl] || name[wl] == ' ');
        if (!hit && al) hit = !strncasecmp(name, alt, al) && (!name[al] || name[al] == ' ');
        if (!hit) continue;
        int score = 1;
        if (strstr(name, "USA")) score += 8; else if (strstr(name, "World")) score += 6; else if (strstr(name, "Europe")) score += 4;
        if (strstr(name, "Beta") || strstr(name, "Proto") || strstr(name, "Demo") || strstr(name, "Sample") || strstr(name, "Kiosk")) score -= 6;
        if (strstr(name, "Virtual Console") || strstr(name, "Collector") || strstr(name, "Switch Online") || strstr(name, "Aftermarket")) score -= 2;
        if (strstr(name, "(Rev")) score -= 1;
        score -= (int)strlen(name) / 40;                  /* shorter names are usually the plain release */
        if (score > best) { best = score; snprintf(out, max, "%s", name); }
    }
    return best > -1;
}

int library_fetch_art(volatile int *done, volatile int *got) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaOS/1.0 (PS Vita)");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write);
    for (int s = 0; s < NSYS; ++s) {
        char path[128];
        snprintf(path, sizeof(path), "%sgames/%s.tsv", ROOT, SYS[s].id);
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) continue;
        int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        char *text = size > 0 ? malloc(size + 1) : NULL;
        int n = text ? sceIoRead(fd, text, size) : 0;
        sceIoClose(fd);
        if (!text) continue;
        text[n > 0 ? n : 0] = 0;
        char dir[200], esys[200];
        snprintf(dir, sizeof(dir), "ux0:data/retroarch/thumbnails/%s", SYS[s].dirs[0]);
        sceIoMkdir("ux0:data/retroarch/thumbnails", 0777);
        sceIoMkdir(dir, 0777);
        char box[240];
        snprintf(box, sizeof(box), "%s/Named_Boxarts", dir);
        sceIoMkdir(box, 0777);
        url_escape(SYS[s].dirs[0], esys, sizeof(esys));
        char esys2[200] = "";                         /* a second server folder, if the system has one */
        if (SYS[s].dirs[1] && strstr(SYS[s].dirs[1], " - ") && !strstr(SYS[s].dirs[1], "RetroArch"))
            url_escape(SYS[s].dirs[1], esys2, sizeof(esys2));
        for (int li = 0; li < 2; ++li) {              /* the listings, once per system */
            lst[li].len = 0;
            if (lst[li].p) lst[li].p[0] = 0;
            if (li && !esys2[0]) continue;
            char lurl[300];
            snprintf(lurl, sizeof(lurl), "https://thumbnails.libretro.com/%s/Named_Boxarts/", li ? esys2 : esys);
            curl_easy_setopt(c, CURLOPT_URL, lurl);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, lst_write);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &lst[li]);
            if (curl_easy_perform(c) != CURLE_OK) lst[li].len = 0;
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write);
        }
        char *save_line = NULL;
        int tried = 0;
        for (char *line = strtok_r(text, "\n", &save_line); line; line = strtok_r(NULL, "\n", &save_line)) {
            char *f[6] = {0};
            int k = 0;
            for (char *q = line; k < 6 && q; ++k) { f[k] = q; q = strchr(q, '\t'); if (q) *q++ = 0; }
            if (k < 5 || strcmp(f[1], "ra") || *f[4]) continue;   /* has art already */
            const char *file = strrchr(f[3], '/');
            char stem[256], tn[256], etn[512], url[800], dest[400];
            snprintf(stem, sizeof(stem), "%s", file ? file + 1 : f[3]);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            thumb_name(stem, tn, sizeof(tn));
            if (art_cap && tried++ >= art_cap) break;
            char match[300];
            const char *from = esys;
            if (!lst[0].len && !lst[1].len) snprintf(match, sizeof(match), "%s", tn);   /* no listing: try the exact name */
            else if (best_match(&lst[0], tn, match, sizeof(match))) {}
            else if (best_match(&lst[1], tn, match, sizeof(match))) from = esys2;
            else { if (done) ++*done; continue; }     /* not on the server */
            url_escape(match, etn, sizeof(etn));
            snprintf(url, sizeof(url), "https://thumbnails.libretro.com/%s/Named_Boxarts/%s.png", from, etn);
            snprintf(dest, sizeof(dest), "%s/%s.png", box, tn);   /* saved under the ROM's name: the scan looks for that */
            if (fetch_png(c, url, dest) == 0 && got) ++*got;
            if (done) ++*done;
        }
        free(text);
    }
    curl_easy_cleanup(c);
    for (int li = 0; li < 2; ++li) { free(lst[li].p); memset(&lst[li], 0, sizeof(lst[li])); }
    return 0;
}
