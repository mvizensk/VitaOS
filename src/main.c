/* Home: the Vita's new front end. Five tabs, switched with L and R or by
 * tapping the tab bar:
 *   Play       every game, from Arcade Hub's library (shelves, previews, reel)
 *   Apps       everything installed, as an icon grid
 *   Files      dual-pane file manager, background copy/move/delete/hash
 *   Downloads  HTTPS straight to the card
 *   Settings   brightness, volume, power, network, storage, restart
 * It keeps Arcade Hub's title ID (MVZA00010), so autostart, the agent
 * bridge's play command and return-after-game all land here unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/shellutil.h>
#include <psp2/rtc.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "ssl_locks.h"
#include "ui.h"
#include "play.h"
#include "apps.h"
#include "camera.h"
#include "library.h"
#include "update.h"
#include "news.h"
#include "demo.h"
#include "playtime.h"
#include "memo.h"
#include "files.h"
#include "downloads.h"
#include "settings.h"
#include "movies.h"
#include "music.h"
#include "sfx.h"
#include "video.h"
#include "hometab.h"
#include "search.h"
#include "store.h"
#include "weather.h"

int _newlib_heap_size_user = 96 * 1024 * 1024;   /* the game catalog, curl, file lists */

static const char *const TABS[] = {"Home", "Play", "Movies", "Music", "Apps", "Files", "Store", "Settings"};
enum { T_HOME, T_PLAY, T_MOVIES, T_MUSIC, T_APPS, T_FILES, T_DOWNLOADS, T_SETTINGS, NTABS };

/* Movies and Music can be hidden (Reddit, 2026-09-27: "an option to get rid of
 * movies and music would be nice"): Settings > VitaOS writes user/hide-movies
 * or user/hide-music. The header, L/R and taps work on the tabs still shown. */
static int vis[NTABS], nvis;
static const char *vis_names[NTABS];
static void tabs_build(void) {
    SceIoStat st;
    int no_movies = sceIoGetstat("ux0:data/arcadehub/user/hide-movies", &st) >= 0;
    int no_music = sceIoGetstat("ux0:data/arcadehub/user/hide-music", &st) >= 0;
    nvis = 0;
    for (int t = 0; t < NTABS; ++t) {
        if ((t == T_MOVIES && no_movies) || (t == T_MUSIC && no_music)) continue;
        vis[nvis] = t;
        vis_names[nvis++] = TABS[t];
    }
}
static int vis_index(int t) {
    for (int i = 0; i < nvis; ++i) if (vis[i] == t) return i;
    return -1;
}
static int tab_step(int t, int dir) {                 /* the next shown tab, wrapping */
    int i = vis_index(t);
    if (i < 0) return T_HOME;
    return vis[(i + dir + nvis) % nvis];
}

/* The PS button. Home locks it with the system's own lock (the one games
 * use while saving), so a short press brings you to Play instead of dropping
 * you onto the LiveArea bubbles. The lock leaves the hold alone: holding PS
 * still opens the system quick menu (Wi-Fi, brightness, music) after about a
 * second (verified 2026-09-24). To reach the bubbles themselves, Settings >
 * System home lifts the lock for 30 seconds; so does holding PS for three
 * seconds if the quick menu ever stops taking the hold. */
#define PS_BIT 0x10000
#define PS_HOLD_FRAMES (60 * 3)
#define PS_RELEASE_FRAMES (60 * 30)
static int ps_locked, ps_frames, ps_free_frames;
static char banner[120];
static int banner_frames;

static void home_log(const char *fmt, int a, int b) {
    char line[128];
    int n = snprintf(line, sizeof(line), fmt, a, b);
    SceUID fd = sceIoOpen("ux0:data/arcadehub/home.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}

static volatile unsigned int frame_no, cur_tab;
static volatile int ps_note;                 /* a real PS press, reported by the bridge */

/* Holding PS is only right when something will hand the press back: the
 * vitaos_ps plugin (listed in a taiHEN config) or the developer's agent
 * bridge. Without one, locking PS would leave it dead, so VitaOS is then a
 * normal app and PS goes to the system as usual. */
static int ps_forwarder(void) {
    static int known = -1;
    if (known >= 0) return known;
    SceIoStat st;
    known = sceIoGetstat("ux0:data/vita-agent/vabridge.suprx", &st) >= 0;
    static const char *const cfgs[] = {"ur0:tai/config.txt", "ux0:tai/config.txt"};
    for (int i = 0; i < 2 && !known; ++i) {
        static char buf[8192];
        SceUID fd = sceIoOpen(cfgs[i], SCE_O_RDONLY, 0);
        if (fd < 0) continue;
        int n = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        buf[n > 0 ? n : 0] = 0;
        for (char *line = strtok(buf, "\r\n"); line && !known; line = strtok(NULL, "\r\n")) {
            while (*line == ' ' || *line == '\t') ++line;
            if (*line != '#' && strstr(line, "vitaos_ps.suprx")) known = 1;
        }
    }
    return known;
}

static void lock_ps(int on) {
    if (on && !ps_forwarder()) return;
    if (on && !ps_locked) {
        int rc = sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
        home_log("ps lock rc=0x%08X %d\n", rc, 0);
        ps_locked = rc >= 0;
    }
    else if (!on && ps_locked) { sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2); ps_locked = 0; }
}

static void release_ps(void);

/* Home holds the PS button for itself (the lock above), which also kept PS
 * from waking the Vita out of sleep (playtest 2026-09-25). So the lock goes
 * as the console suspends and comes back once it is awake again. */
static volatile int ps_relock;

static int power_cb(int notify_id, int notify_count, int info, void *common) {
    (void)notify_id; (void)notify_count; (void)common;
    if (info & (SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_BUTTON_POWER_PRESS)) {
        lock_ps(0);
        ps_relock = 1;
    }
    if (info & (SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME)) ps_relock = 2;
    return 0;
}

static int power_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUID cb = sceKernelCreateCallback("home_power", 0, power_cb, NULL);
    if (cb >= 0) scePowerRegisterCallback(cb);
    for (;;) sceKernelDelayThreadCB(1000 * 1000);
    return 0;
}

/* Returns 1 on a short press (go Home). */
static int ps_button(const Input *in) {
    if (ps_free_frames && --ps_free_frames == 0) lock_ps(1);
    static int seen;
    if ((in->held & PS_BIT) && !seen) { seen = 1; home_log("ps bit seen by Home 0x%08X %d\n", in->held, 0); }
    /* A tap can begin and end between two frames (it did in attract mode,
     * where frames run long): the press edge counts even if PS is already up. */
    static int down;
    static unsigned int last_press;
    if (in->pressed & PS_BIT) { down = 1; ps_frames = 0; last_press = frame_no; }
    if (ps_note) {                                  /* a real press, via the bridge */
        ps_note = 0;
        if (frame_no - last_press > 40) { last_press = frame_no; return 1; }   /* not the same press twice */
    }
    if (!down) return 0;
    if (in->held & PS_BIT) {
        if (++ps_frames == PS_HOLD_FRAMES) release_ps();
        return 0;
    }
    down = 0;
    return ps_frames < PS_HOLD_FRAMES;
}

static void release_ps(void) {
    lock_ps(0);
    ps_free_frames = PS_RELEASE_FRAMES;
    snprintf(banner, sizeof(banner), "Press PS now for the system home screen (30 s)");
    banner_frames = 60 * 6;
}

/* Long-pressing a header status icon (a Reddit ask, 2026-09-26): jump
 * straight into the matching page of the system Settings app instead of our
 * own Settings tab. A short tap on the same icon still just switches tabs,
 * handled below next to the other header taps. */
#define ICON_HOLD_FRAMES (60 * 6 / 10)   /* ~0.6 s at 60 fps */

static void header_icon_hold(Input *in) {
    static int icon = -1;      /* which icon this touch is tracking, -1 = none */
    static int sx, sy;         /* where that touch began */
    static unsigned int held;  /* frames held so far, this touch */
    static int fired;          /* the long press already acted for this touch */
    if (in->touching) {
        if (icon < 0) {
            int t = header_icon_at(in->tx, in->ty);
            if (t >= 0) { icon = t; sx = in->tx; sy = in->ty; held = 0; fired = 0; }
            return;
        }
        int dx = in->tx - sx, dy = in->ty - sy;
        if (dx * dx + dy * dy > 12 * 12) { icon = -1; return; }   /* dragged off: not a hold */
        if (!fired && ++held >= ICON_HOLD_FRAMES) {
            fired = 1;
            release_ps();                                  /* so PS can bring Home back, as Settings does */
            if (sceAppMgrLaunchAppByUri(0x20000, "settings_dlg:") < 0) ui_toast("Settings would not open", C_BAD);
            else ui_toast(icon == 0 ? "Network > Wi-Fi Settings. PS comes back here."
                                    : "Devices > Bluetooth Devices. PS comes back here.", C_ACCENT);
        }
        return;
    }
    if (icon >= 0 && fired) in->tapped = 0;   /* the hold already acted; don't also tap-navigate */
    icon = -1; fired = 0; held = 0;
}

static void draw_banner(void) {
    if (banner_frames <= 0) return;
    banner_frames--;
    int w = text_w(font, 18, banner) + 48;
    vita2d_draw_rectangle((W - w) / 2, 78, w, 42, RGBA8(28, 32, 44, 240));
    vita2d_draw_rectangle((W - w) / 2, 78, 4, 42, C_ACCENT);
    text(font, (W - w) / 2 + 24, 105, C_TEXT, 18, banner);
}

/* ---------- the in-game quick menu (kernel/vaoverlay.c) ---------- */

#define OVERLAY_MOD "ux0:data/vita-agent/vaoverlay.skprx"
#define OVERLAY_TRY "ux0:data/vita-agent/overlay.try"
#define OVERLAY_ON "ux0:data/vita-agent/overlay.on"

static int file_there(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

/* The overlay's heartbeat is "<boot id> <uptime>" (see vaoverlay.c): alive
 * means the same boot within 5 s and written within the last 30 s. */
static int overlay_alive(void) {
    char b[48];
    SceUID fd = sceIoOpen(OVERLAY_ON, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    unsigned long long v[2] = {0, 0};
    const char *q = b;
    for (int i = 0; i < 2; ++i) {
        while (*q == ' ') ++q;
        while (*q >= '0' && *q <= '9') v[i] = v[i] * 10 + (unsigned)(*q++ - '0');
    }
    SceRtcTick t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentTick(&t);
    unsigned long long up = sceKernelGetSystemTimeWide(), boot = t.tick - up;
    unsigned long long dboot = boot > v[0] ? boot - v[0] : v[0] - boot;
    return v[0] && v[1] && dboot < 5000000ull && v[1] <= up && up - v[1] < 30000000ull;
}

static int bridge_up(void) {
    int fd = sceNetSocket("home_probe", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) return 0;
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons(1348);
    sceNetInetPton(SCE_NET_AF_INET, "127.0.0.1", &a.sin_addr);
    int ok = sceNetConnect(fd, (SceNetSockaddr *)&a, sizeof(a)) >= 0;
    sceNetSocketClose(fd);
    return ok;
}

/* Loads the quick-menu kernel module once per boot, behind a crash guard: a
 * marker is written before loading and removed 20 s later. Finding the marker
 * with no live overlay means the last load took the console down, so the
 * module is set aside (renamed .crashed) instead of loaded again, and a bad
 * build can never crash-loop the boot. */
static int overlay_loader(SceSize args, void *argp) {
    (void)args; (void)argp;
    if (!file_there(OVERLAY_MOD)) return sceKernelExitDeleteThread(0);
    if (file_there(OVERLAY_TRY)) {
        if (overlay_alive()) { sceIoRemove(OVERLAY_TRY); return sceKernelExitDeleteThread(0); }
        sceIoRemove(OVERLAY_MOD ".crashed");
        sceIoRename(OVERLAY_MOD, OVERLAY_MOD ".crashed");
        sceIoRemove(OVERLAY_TRY);
        home_log("overlay set aside: last load did not survive %d %d\n", 0, 0);
        return sceKernelExitDeleteThread(0);
    }
    /* After the bridge: it decides the PS lock from the overlay's heartbeat. */
    for (int i = 0; i < 40 && !bridge_up(); ++i) sceKernelDelayThread(500 * 1000);
    if (overlay_alive()) return sceKernelExitDeleteThread(0);        /* already loaded this boot */
    SceUID fd = sceIoOpen(OVERLAY_TRY, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
    SceUID rc = taiLoadStartKernelModule(OVERLAY_MOD, 0, NULL, 0);
    home_log("overlay load rc=0x%08X %d\n", rc, 0);
    if (rc < 0) { sceIoRemove(OVERLAY_TRY); return sceKernelExitDeleteThread(0); }
    sceKernelDelayThread(20 * 1000 * 1000);
    sceIoRemove(OVERLAY_TRY);
    return sceKernelExitDeleteThread(0);
}

static int net_up(void) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static char mem[1024 * 1024];
    SceNetInitParam p = {mem, sizeof(mem), 0};
    int rc = sceNetInit(&p);
    if (rc < 0 && rc != (int)0x80410110) return rc;   /* already initialised is fine */
    rc = sceNetCtlInit();
    return rc < 0 && rc != (int)0x80412102 ? rc : 0;
}

/* Start-up: the wordmark fades up while an accent line draws out from the
 * centre, then holds while the library loads. t runs 0..1. */
static void splash(float t, float out) {
    ui_begin_frame();
    float e = t < 1 ? 1 - (1 - t) * (1 - t) * (1 - t) : 1;
    int a = (int)(255 * e * (1 - out));
    const char *word = "home";
    int ww = text_w(bold, 64, word);
    for (int k = 5; k >= 1; --k)                       /* soft glow behind the word */
        vita2d_draw_rectangle(W / 2 - ww / 2 - k * 10, 230 - k * 6, ww + k * 20, 60 + k * 12,
                              (C_ACCENT & 0x00FFFFFF) | ((unsigned int)(a * 0.03f) << 24));
    text(bold, W / 2 - ww / 2, (int)(292 - 10 * e), (C_TEXT & 0x00FFFFFF) | ((unsigned int)a << 24), 64, word);
    float lw = 160 * e;
    vita2d_draw_rectangle(W / 2 - lw / 2, 312, lw, 3, (C_ACCENT & 0x00FFFFFF) | ((unsigned int)a << 24));
    int cw = text_w(font, 15, "PS Vita");
    text(font, W / 2 - cw / 2, 344, (C_FAINT & 0x00FFFFFF) | ((unsigned int)(a * 0.27f) << 24), 15, "PS Vita");
    ui_end_frame();
}

#define LAST_TAB "ux0:data/arcadehub/user/last-tab"

/* Stall watchdog: if a frame runs past 300 ms, note where the main thread
 * was (STAGE markers) every 300 ms until it moves on. */
/* The lists behind Movies, Music and Apps, read once in the background. */
static int prewarm(SceSize args, void *argp) {
    (void)args; (void)argp;
    movies_prewarm();
    music_prewarm();
    apps_prewarm();
    return sceKernelExitDeleteThread(0);
}

static volatile SceUInt64 frame_worst;
static volatile int agent_seen;              /* the watchdog saw the bridge's activity stamp change */

/* Card reads for the main loop's notices, done here so the main thread never
 * waits on the card: frame timing (while user/fps-log exists) and the agent
 * activity stamp. */
/* A real PS press, from the bridge (see vabridge.c ps_for_home): Home's own
 * controller reads never see it. Checked every watchdog tick (~300 ms); a
 * note older than ~1.5 s is from a press in a game while Home slept. */

static void ps_note_check(void) {
    SceIoStat st;
    if (sceIoGetstat("ux0:data/arcadehub/ps.tap", &st) < 0) return;
    sceIoRemove("ux0:data/arcadehub/ps.tap");
    SceDateTime now;
    SceRtcTick a, b;
    sceRtcGetCurrentClock(&now, 0);                   /* file times are UTC */
    sceRtcGetTick(&now, &a);
    sceRtcGetTick(&st.st_mtime, &b);
    if (a.tick >= b.tick && a.tick - b.tick < 1500000) { ps_note = 1; home_log("ps via bridge %d %d\n", 0, 0); }
}

static void housekeeping(void) {
    static int ticks, probe = -1, quiet;
    ps_note_check();
    static unsigned int frames_then;
    static SceDateTime seen_at;
    static int seen_once;
    ++ticks;
    if (ticks % 7 == 0) {                            /* ~2 s */
        SceIoStat st;
        if (quiet > 0) quiet--;
        static int active_ticks;
        if (active_ticks > 0 && !--active_ticks) ui_agent_active = 0;
        if (sceIoGetstat("ux0:data/vita-agent/activity", &st) >= 0) {
            if (seen_once && memcmp(&st.st_mtime, &seen_at, sizeof(seen_at))) { ui_agent_active = 1; active_ticks = 60; }   /* ~2 min */
            if (seen_once && memcmp(&st.st_mtime, &seen_at, sizeof(seen_at)) && !quiet) { agent_seen = 1; quiet = 150; }
            seen_at = st.st_mtime;
            seen_once = 1;
        }
    }
    if (ticks % 17 == 0) {                           /* ~5 s */
        SceIoStat st;
        if (probe < 0 || ticks % 170 == 0) probe = sceIoGetstat("ux0:data/arcadehub/user/fps-log", &st) >= 0;
        if (probe) {
            char line[96];
            int n = snprintf(line, sizeof(line), "tab %u fps %u worst %u ms\n", cur_tab, (frame_no - frames_then) / 5,
                             (unsigned int)(frame_worst / 1000));
            SceUID fd = sceIoOpen("ux0:data/arcadehub/fps.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
            if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
        }
        frames_then = frame_no;
        frame_worst = 0;
    }
}

/* ---------- the library on the card (library.c) ---------- */
static volatile int lib_found, lib_done = -1, lib_before = -1;   /* lib_before -2: reload whatever the count */
static int lib_applied;
#define LIB_COUNT "ux0:data/arcadehub/user/library-count.txt"

static int lib_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    lib_done = library_scan(&lib_found);
    return sceKernelExitDeleteThread(0);
}

static void lib_start(void) {
    SceUID t = sceKernelCreateThread("library", lib_thread, 0x10000110, 0x10000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) lib_done = 0;
}

static void lib_save_count(int n) {
    char b[16];
    int len = snprintf(b, sizeof(b), "%d", n);
    ui_save(LIB_COUNT, b, len, 0);
}

static void library_first_scan(void) {
    lib_start();
    while (lib_done < 0) {
        ui_begin_frame();
        ui_ambient(1.0f);
        const char *t = "Finding your games";
        text(bold, (W - text_w(bold, 26, t)) / 2, H / 2 - 6, C_TEXT, 26, t);
        char n[48];
        snprintf(n, sizeof(n), lib_found ? "%d found" : "Looking in RetroArch and your ROM folders\xE2\x80\xA6", lib_found);
        text(font, (W - text_w(font, 17, n)) / 2, H / 2 + 28, C_DIM, 17, n);
        ui_end_frame();
    }
    lib_save_count(lib_done);
}

static void library_rescan_now(void) {                /* Settings > VitaOS > Find games again */
    if (library_is_ours() == 0) { ui_message("Find games again", "This Vita uses a library built on a computer, so VitaOS leaves it as it is."); return; }
    if (lib_done < 0 && lib_before >= 0) { ui_toast("Already looking for games", C_ACCENT); return; }
    SceUID fd = sceIoOpen(LIB_COUNT, SCE_O_RDONLY, 0);
    lib_before = 0;
    if (fd >= 0) { char b[16] = {0}; sceIoRead(fd, b, 15); sceIoClose(fd); lib_before = atoi(b); }
    lib_before = lib_before ? lib_before : 1;
    lib_found = 0;
    lib_done = -1;
    lib_applied = 0;
    lib_start();
    ui_toast("Looking for games\xE2\x80\xA6", C_ACCENT);
}

static volatile int art_done, art_got, art_running, art_quiet;
static int art_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    library_fetch_art(&art_done, &art_got);
    char msg[80];
    snprintf(msg, sizeof(msg), "Box art: %d found for %d games", art_got, art_done);
    if (!art_quiet || art_got > 0) ui_toast(msg, C_OK);
    art_running = 0;
    library_rescan_now();                            /* pick the new art up */
    lib_before = -2;                                 /* same games, new art: reload anyway */
    return sceKernelExitDeleteThread(0);
}

static void library_art_now(void) {                  /* Settings > VitaOS > Download box art */
    if (library_is_ours() == 0) { ui_message("Download box art", "This Vita uses a library built on a computer, which has its own art."); return; }
    if (art_running) { ui_toast("Already downloading box art", C_ACCENT); return; }
    art_running = 1; art_quiet = 0; art_done = art_got = 0;
    SceUID t = sceKernelCreateThread("box_art", art_thread, 0x10000110, 0x10000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) { art_running = 0; return; }
    ui_toast("Downloading box art in the background", C_ACCENT);
}

/* Box art without asking (Reddit and a DM, 2026-09-29: "couldn't pull covers
 * and artwork"; the download sat behind Settings > VitaOS). Once the library
 * is scanned and Wi-Fi is up, fetch art for games that have none; again only
 * when the game count changes (user/art-auto holds the count last tried). */
#define ART_AUTO "ux0:data/arcadehub/user/art-auto"
static void art_auto_tick(void) {
    static int done_this_run;
    if (done_this_run || art_running || lib_done < 0) return;
    if (!(frame_no % 120 == 0)) return;               /* every two seconds is plenty */
    int st = 0;
    if (sceNetCtlInetGetState(&st) < 0 || st != SCE_NETCTL_STATE_CONNECTED) return;
    done_this_run = 1;
    if (library_is_ours() <= 0) return;               /* a Mac-built catalog has its own art */
    char want[16], had[16] = {0};
    snprintf(want, sizeof(want), "%d", lib_done);
    SceUID fd = sceIoOpen(ART_AUTO, SCE_O_RDONLY, 0);
    if (fd >= 0) { sceIoRead(fd, had, sizeof(had) - 1); sceIoClose(fd); }
    if (!strcmp(want, had)) return;
    ui_save(ART_AUTO, want, strlen(want), 0);
    art_running = 1; art_quiet = 1; art_done = art_got = 0;
    SceUID t = sceKernelCreateThread("box_art", art_thread, 0x10000110, 0x10000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) art_running = 0;
}

static void library_rescan_start(void) {
    SceUID fd = sceIoOpen(LIB_COUNT, SCE_O_RDONLY, 0);
    if (fd >= 0) { char b[16] = {0}; sceIoRead(fd, b, 15); sceIoClose(fd); lib_before = atoi(b); }
    lib_start();
}

/* Main loop, each frame: a background rescan that found something new. */
static void library_poll(int on_play) {
#define applied lib_applied
    if (applied || lib_done < 0 || lib_before == -1) return;
    if (lib_done == lib_before) { applied = 1; return; }
    if (on_play) return;                             /* not under the player's feet */
    applied = 1;
    play_reload();
    lib_save_count(lib_done);
    char msg[64];
    int d = lib_before < 0 ? 0 : lib_done - lib_before;
    snprintf(msg, sizeof(msg), d > 0 ? "%d new game%s in Play" : "Play's library was updated", d, d == 1 ? "" : "s");
    ui_toast(msg, C_ACCENT);
#undef applied
}

static int watchdog(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int seen = 0;
    int stuck = 0;
    for (;;) {
        sceKernelDelayThread(300 * 1000);
        housekeeping();
        if (frame_no != seen) { seen = frame_no; stuck = 0; continue; }
        if (++stuck > 60) continue;                       /* 18 s of the same: stop repeating */
        char line[120];
        int n = snprintf(line, sizeof(line), "stall %d00 ms at %s\n", stuck * 3, (const char *)ui_where);
        SceUID fd = sceIoOpen("ux0:data/arcadehub/fps.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
    }
    return 0;
}

/* Cold boot: play the logo video full screen (any button skips). Its last
 * frame stays on screen while the library loads. Returns 0 if there is none. */
static int boot_video_played;

/* The boot video ships in the app (VitaOS); a copy on the card wins, so it
 * can be swapped without reinstalling. */
static const char *boot_video_path(void) {
    SceIoStat st;
    return sceIoGetstat("ux0:data/arcadehub/boot.mp4", &st) >= 0 ? "ux0:data/arcadehub/boot.mp4" : "app0:assets/boot.mp4";
}
static int system_ui_over_us(void) {
    SceAppMgrAppState st;
    memset(&st, 0, sizeof(st));
    return _sceAppMgrGetAppState(&st, sizeof(st), 0x03150021) >= 0 && st.isSystemUiOverlaid;
}

static int boot_video(void) {
    /* At power-on Home starts behind the lock screen: wait (drawing the plain
     * background) until the bridge's swipe or a thumb clears it, up to 30 s,
     * so the logo plays where it can be seen. */
    for (int f = 0; f < 60 * 30 && system_ui_over_us(); ++f) { ui_begin_frame(); ui_end_frame(); }
    if (video_play_opts(boot_video_path(), 0, 0, 0, OWN_PREVIEW) < 0) return 0;
    Input in;
    memset(&in, 0, sizeof(in));
    for (int f = 0; f < 60 * 8; ++f) {                    /* never more than 8 s */
        ui_read_input(&in);
        if (in.pressed) break;
        vita2d_texture *t = video_frame();               /* before drawing: an ended video gives none */
        if (!t && video_owner() == OWN_NONE) break;       /* ended: the last logo frame stays up */
        ui_begin_frame();
        if (t) {
            float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
            vita2d_draw_texture_scale(t, 0, 0, W / tw, H / th);
        }
        ui_end_frame();
    }
    video_stop();
    boot_video_played = 1;
    return 1;
}

int main(void) {
    ssl_threads_init();                      /* before any thread: OpenSSL 1.0.2 needs its locks (ssl_locks.c) */
    vita2d_init_advanced(4 * 1024 * 1024);   /* the per-frame vertex pool: the new visuals need more than 1 MB */
    vita2d_set_clear_color(C_BG);
    ui_init();
    sfx_init();
    /* The splash and chime are for a cold boot only: Home also restarts every
     * time a game closes, and that should feel instant. */
    int cold = sceKernelGetSystemTimeWide() < 180ULL * 1000000;
    if (sceIoRemove("ux0:data/arcadehub/user/boot-once") >= 0) cold = 1;   /* a test hook: one cold start */
    if (cold) {
        sfx_play(SFX_BOOT);
        if (!boot_video())                                     /* the 3D logo (home/boot/logo.py) */
            for (int f = 0; f <= 36; ++f) splash(f / 36.0f, 0);   /* no video: the text splash */
    }
    home_log("init events rc=0x%08X %d\n", sceShellUtilInitEvents(0), 0);
    lock_ps(1);
    {
        SceUID pt = sceKernelCreateThread("home_power", power_thread, 0x10000100, 0x2000, 0, 0, NULL);
        if (pt >= 0) sceKernelStartThread(pt, 0, NULL);
    }
    settings_on_release_ps(release_ps);
    settings_on_library(library_rescan_now, library_art_now);
    /* VitaOS finds the games on the card. The first time there is nothing to
     * show yet, so it looks behind a progress screen; afterwards it looks again
     * in the background and Play reloads if anything changed. */
    {                                                 /* a test hook: scan into a side folder, log the count */
        SceIoStat st;
        if (sceIoGetstat("ux0:data/arcadehub/user/library-test", &st) >= 0) {
            int n = library_scan_into("ux0:data/vitaos-test/", NULL);
            home_log("library test scan: %d games %d\n", n, 0);
            if (st.st_size > 0) {                     /* "art" in the file: fetch box art too, 25 per system */
                volatile int d = 0, g = 0;
                library_art_into("ux0:data/vitaos-test/", 25, &d, &g);
                n = library_scan_into("ux0:data/vitaos-test/", NULL);
                home_log("library test art: tried %d got %d\n", d, g);
                home_log("library test rescan: %d games %d\n", n, 0);
            }
            sceIoRemove("ux0:data/arcadehub/user/library-test");
        }
    }
    int lib_ours = library_is_ours();
    if (lib_ours < 0) library_first_scan();
    play_init();                 /* catalog, lists, the agent remote, launch requests */
    playtime_init();             /* the time of a game Home launched before it closed */
    if (lib_ours > 0) library_rescan_start();
    int net = net_up();
    files_init();
    if (net >= 0) downloads_init();
    if (net >= 0) weather_init();
    if (net >= 0) update_init();                     /* a newer VitaOS? at most once a day */
    news_init();                                     /* r/vitahacks for the Home row; waits for Wi-Fi itself */
    SceUID pw = sceKernelCreateThread("home_prewarm", prewarm, 0x10000100, 0x8000, 0, 0, NULL);
    if (pw >= 0) sceKernelStartThread(pw, 0, NULL);
    SceUID wd = sceKernelCreateThread("home_watchdog", watchdog, 0x10000100, 0x2000, 0, 0, NULL);
    if (wd >= 0) sceKernelStartThread(wd, 0, NULL);
    SceUID loader = sceKernelCreateThread("home_overlay", overlay_loader, 0x10000100, 0x4000, 0, 0, NULL);
    if (loader >= 0) sceKernelStartThread(loader, 0, NULL);

    if (cold && !boot_video_played) for (int f = 0; f <= 18; ++f) splash(1, f / 18.0f);   /* fade out */
    /* Back to the tab you last used (Home the first time). */
    int tab = T_HOME, open_music = 0;
    SceUID lt = sceIoOpen(LAST_TAB, SCE_O_RDONLY, 0);
    if (lt >= 0) {
        char b[4] = {0};
        if (sceIoRead(lt, b, 3) > 0 && atoi(b) >= 0 && atoi(b) < NTABS) tab = atoi(b);
        sceIoClose(lt);
    }
    tabs_build();
    if (vis_index(tab) < 0) tab = T_HOME;             /* the last tab was hidden since */
    Input in;
    memset(&in, 0, sizeof(in));
    for (;;) {
        ++frame_no;
        if (frame_no % 60 == 0) tabs_build();         /* Settings may have hidden or shown one */
        {                                        /* frame timing, logged by the watchdog; at the top so */
            static SceUInt64 last;              /* early-out paths (search) are measured too */
            SceUInt64 now = sceKernelGetProcessTimeWide();
            if (last && now - last > frame_worst) frame_worst = now - last;
            last = now;
            cur_tab = tab;
        }
        STAGE("input");
        ui_read_input(&in);
        demo_poll();                             /* a scripted tour for recordings (demo.req) */
        demo_input(&in);
        if (demo_running()) { ui_agent_active = 0; agent_seen = 0; }   /* the recording shows the OS, not the tooling */
        int was = tab;
        if (ps_relock == 2) { ps_relock = 0; if (!ps_free_frames) lock_ps(1); }   /* awake again */
        int ps = ps_button(&in);
        if (ps && tab == T_PLAY && play_wake()) { ps = 0; sfx_play(SFX_BACK); }   /* first press: wake the reel */
        if (ps) {                              /* short PS, like the Switch's HOME: close everything, go Home */
            search_close();
            settings_leave();
            store_leave();
            if (tab == T_MOVIES) movies_leave();
            camera_leave();
            memo_leave();
            play_home();
            play_leave();
            hometab_reset();
            tab = T_HOME;
            sfx_play(SFX_BACK);
        }
        int movie_full = (tab == T_MOVIES && movies_fullscreen())   /* L R seek there */
                         || (tab == T_APPS && camera_fullscreen());  /* and are the camera's own */
        if ((in.pressed & SCE_CTRL_SELECT) && !movie_full && !search_active()) {
            search_open();
            in.pressed &= ~SCE_CTRL_SELECT;
        }
        search_prepare();                        /* between frames: it blurs the last one */
        int searching = search_active();
        if (!movie_full && !searching && in.pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) tab = tab_step(tab, -1);
        if (!movie_full && !searching && in.pressed & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) tab = tab_step(tab, 1);
        if (!movie_full && !searching) header_icon_hold(&in);   /* may clear in.tapped below */
        if (in.tapped && !movie_full && !searching) {
            int t = header_tab_at(in.tap_x, in.tap_y, vis_names, nvis);
            if (t >= 0) { tab = vis[t]; in.tapped = 0; }
            else if (header_icon_at(in.tap_x, in.tap_y) >= 0) { tab = T_SETTINGS; in.tapped = 0; }
        }
        if (open_music) { tab = T_MUSIC; open_music = 0; }   /* the footer player's title was tapped */
        /* UI sounds, from the input itself: every screen gets them. Not in the
         * movie player, where the buttons are transport controls. */
        static float fade;                       /* the new tab's content fades in */
        if (tab != was) { sfx_play(SFX_TAB); fade = 1.0f; }
        else if (!movie_full) {
            if (in.pressed & SCE_CTRL_CROSS) sfx_play(SFX_SELECT);
            else if (in.pressed & SCE_CTRL_CIRCLE) sfx_play(SFX_BACK);
            else if (in.pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) sfx_play(SFX_MOVE);
        }
        if (tab != was) {
            STAGE("tab switch");
            { char b[4]; int n = snprintf(b, sizeof(b), "%d", tab); ui_save(LAST_TAB, b, n, 0); }
            if (was == T_PLAY) play_leave();
            if (was == T_MOVIES) movies_leave();
            if (was == T_APPS) { camera_leave(); memo_leave(); }
            in.pressed = 0;          /* the switch is not also a press inside the new tab */
        }
        /* A long copy or download must not be cut off by auto-standby. */
        if (files_busy() || downloads_busy()) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        music_tick(&in);           /* the next track, whatever tab is open */
        sfx_ambient(tab == T_HOME && video_owner() == OWN_NONE);   /* only when nothing else plays */
        ui_begin_frame();
        const char *hint = "", *context = "";
        ui_theme_default();                      /* tabs with art override this below */
        if (searching) {
            int to = search_update(&in);
            if (to == SEARCH_TO_MOVIES) tab = T_MOVIES;
            else if (to == SEARCH_TO_MUSIC) tab = T_MUSIC;
            else if (to == SEARCH_TO_SETTINGS) tab = T_SETTINGS;
            else if (to == SEARCH_TO_PLAY) tab = T_PLAY;
            ui_draw_toasts();
            ui_end_frame();
            continue;
        }
        library_poll(tab == T_PLAY);
        art_auto_tick();
        update_tick();                                /* a downloaded update hands over to VitaOS Updater */
        STAGE("tab body");
        if (tab != T_PLAY && tab != T_HOME) ui_ambient(1.0f);   /* Play and Home lay it over their art */
        switch (tab) {
        case T_PLAY: play_frame(&in); hint = play_hint(); context = play_context(); break;
        case T_HOME:
            hometab_update(&in);
            hint = hometab_hint();
            { int w = hometab_wants_tab();
              if (w == HOMETAB_TO_MOVIES) tab = T_MOVIES;                     /* the film opened full screen */
              else if (w == HOMETAB_TO_STORE) tab = T_DOWNLOADS; }            /* a news item's app page */
            break;
        case T_MOVIES: movies_update(&in); hint = movies_hint(); break;
        case T_MUSIC: music_update(&in); hint = music_hint(); break;
        case T_APPS: apps_update(&in); hint = apps_hint(); break;
        case T_FILES: files_update(&in); hint = files_hint(); break;
        case T_DOWNLOADS: downloads_update(&in); hint = downloads_hint(); break;
        case T_SETTINGS: settings_update(&in); hint = settings_hint(); break;
        }
        /* Chrome goes on last so content scrolled under it is covered. */
        if (fade > 0.01f) {
            vita2d_draw_rectangle(0, 65, W, H - 105, RGBA8(21, 24, 33, (int)(fade * 255)));
            fade *= 0.72f;                        /* ~150 ms, eased out */
        } else fade = 0;
        /* What is playing gets a small player in the bottom bar on every other
         * tab: previous, play or pause, next, and the title opens Now Playing. */
        int bar = tab != T_MUSIC && *music_now();
        if (!(tab == T_PLAY && play_fullscreen()) && !(tab == T_MOVIES && movies_fullscreen()) &&
            !(tab == T_APPS && camera_fullscreen())) {
            draw_header(vis_names, nvis, vis_index(tab), context);
            if (demo_running()) ui_agent_active = 0;      /* again: housekeeping may have set it this frame */
            int rx = draw_footer_r(hint, bar ? 330 : 0);
            if (bar && music_bar(&in, rx + 8, 318)) open_music = 1;
        }
        STAGE("chrome");
        draw_banner();
        ui_draw_toasts();
        /* System notices: battery (once per threshold per charge) and agents
         * driving the console (the bridge's agent mode flag file). */
        {
            static int warned = 101, frame;
            if (++frame % 120 == 0) {
                int pct = scePowerGetBatteryLifePercent();
                if (scePowerIsPowerOnline()) warned = 101;
                else if (pct >= 0 && pct <= 5 && warned > 5) { ui_toast("Battery at 5%: plug in soon", C_BAD); warned = 5; }
                else if (pct >= 0 && pct <= 15 && warned > 15) { ui_toast("Battery low (15%)", C_BAD); warned = 15; }
                if (agent_seen) { agent_seen = 0; ui_toast("An agent is working on this Vita", C_ACCENT); }
            }
        }
        STAGE("end frame");
        ui_end_frame();
    }
    return 0;
}
