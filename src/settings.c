#include <math.h>
/* Settings: the few things you actually change, and everything you check,
 * on one page instead of the system Settings app's menus. Six cards; the
 * focusable rows are brightness, volume, restart and sleep. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/power.h>
#include <psp2/avconfig.h>
#include <psp2/registrymgr.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/kernel/processmgr.h>

#include "settings.h"
#include "version.h"
#include "update.h"
#include "battery.h"
#include "lang.h"
#include "weather.h"
#include "sfx.h"
#include "storage.h"

#define BRIGHT_MIN 21
#define BRIGHT_MAX 65536
#define VOL_MAX 30

static struct {
    int battery, charging, plugged, minutes, temp, volt;
    int arm, bus, gpu, xbar;
    char ip[20], ssid[40];
    int rssi, remote;
    SceIoDevInfo dev[4];
    int dev_ok[4];
} s;
static volatile SceUInt64 wanted_until;     /* the tab is on screen: keep the numbers fresh */
static int brightness = -1, volume = -1;
static SceUInt64 last;

static void (*release_ps)(void);
void settings_on_release_ps(void (*fn)(void)) { release_ps = fn; }
static void (*lib_rescan)(void), (*lib_art)(void);
void settings_on_library(void (*rescan)(void), void (*art)(void)) { lib_rescan = rescan; lib_art = art; }
static void page_reset(void);
void settings_leave(void) { storage_close(); battery_close(); page_reset(); }

static int remote_listening(void) {
    int fd = sceNetSocket("home_probe", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int one = 1;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons(1348);
    sceNetInetPton(SCE_NET_AF_INET, "127.0.0.1", &a.sin_addr);
    int ok = sceNetConnect(fd, (SceNetSockaddr *)&a, sizeof(a)) >= 0;
    for (int i = 0; i < 10 && !ok; ++i) {
        sceKernelDelayThread(5000);
        SceNetSockaddrIn peer;
        unsigned int len = sizeof(peer);
        ok = sceNetGetpeername(fd, (SceNetSockaddr *)&peer, &len) >= 0;
    }
    sceNetSocketClose(fd);
    return ok;
}

static void refresh(void) {
    STAGE("settings: refresh");
    s.battery = scePowerGetBatteryLifePercent();
    s.charging = scePowerIsBatteryCharging();
    s.plugged = scePowerIsPowerOnline();
    s.minutes = scePowerGetBatteryLifeTime();
    s.temp = scePowerGetBatteryTemp();
    s.volt = scePowerGetBatteryVolt();
    s.arm = scePowerGetArmClockFrequency();
    s.bus = scePowerGetBusClockFrequency();
    s.gpu = scePowerGetGpuClockFrequency();
    s.xbar = scePowerGetGpuXbarClockFrequency();
    SceNetCtlInfo info;
    s.ip[0] = s.ssid[0] = 0;
    s.rssi = -1;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) snprintf(s.ip, sizeof(s.ip), "%s", info.ip_address);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &info) >= 0) snprintf(s.ssid, sizeof(s.ssid), "%s", info.ssid);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info) >= 0) s.rssi = info.rssi_percentage;
    s.remote = remote_listening();
    if (brightness < 0 && sceRegMgrGetKeyInt("/CONFIG/DISPLAY", "brightness", &brightness) < 0) brightness = BRIGHT_MAX / 2;
    /* The registry is what the system's own slider reads; AVConfig can report
     * 0 before anything has set it in this process. */
    if (volume < 0 && sceRegMgrGetKeyInt("/CONFIG/SOUND", "main_volume", &volume) < 0 &&
        sceAVConfigGetSystemVol(&volume) < 0)
        volume = VOL_MAX / 2;
    static const char *const devs[] = {"ux0:", "ur0:", "uma0:", "imc0:"};
    for (int i = 0; i < 4; ++i) {
        memset(&s.dev[i], 0, sizeof(s.dev[i]));
        s.dev_ok[i] = sceIoDevctl(devs[i], 0x3001, NULL, 0, &s.dev[i], sizeof(s.dev[i])) >= 0 && s.dev[i].max_size;
    }
}

/* All of this touches the card, the network stack or the registry, any of
 * which can block here for a second or more: never on the main thread. */
static int refresher(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        if (sceKernelGetProcessTimeWide() < wanted_until) refresh();
        sceKernelDelayThread(2000 * 1000);
    }
    return 0;
}

/* The same brightness the system slider sets, saved so it survives a reboot. */
static void set_brightness(int v) {
    if (v < BRIGHT_MIN) v = BRIGHT_MIN;
    if (v > BRIGHT_MAX) v = BRIGHT_MAX;
    brightness = v;
    sceAVConfigSetDisplayBrightness(v);
    sceRegMgrSetKeyInt("/CONFIG/DISPLAY", "brightness", v);
}

static void set_volume(int v) {
    volume = v < 0 ? 0 : v > VOL_MAX ? VOL_MAX : v;
    sceAVConfigSetSystemVol(volume);
    sceRegMgrSetKeyInt("/CONFIG/SOUND", "main_volume", volume);
}

/* For the movie player's swipes (Plex style): 0..1 levels. `save` writes the
 * registry too, which the player does once, when the finger lifts. */
static void levels_known(void) {
    if (brightness < 0 && sceRegMgrGetKeyInt("/CONFIG/DISPLAY", "brightness", &brightness) < 0) brightness = BRIGHT_MAX / 2;
    if (volume < 0 && sceRegMgrGetKeyInt("/CONFIG/SOUND", "main_volume", &volume) < 0 &&
        sceAVConfigGetSystemVol(&volume) < 0)
        volume = VOL_MAX / 2;
}
float settings_brightness(void) { levels_known(); return (float)(brightness - BRIGHT_MIN) / (BRIGHT_MAX - BRIGHT_MIN); }
float settings_volume(void) { levels_known(); return volume / (float)VOL_MAX; }
void settings_set_brightness(float f, int save) {
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    brightness = BRIGHT_MIN + (int)(f * (BRIGHT_MAX - BRIGHT_MIN));
    sceAVConfigSetDisplayBrightness(brightness);
    if (save) sceRegMgrSetKeyInt("/CONFIG/DISPLAY", "brightness", brightness);
}
void settings_set_volume(float f, int save) {
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    volume = (int)(f * VOL_MAX + 0.5f);
    sceAVConfigSetSystemVol(volume);
    if (save) sceRegMgrSetKeyInt("/CONFIG/SOUND", "main_volume", volume);
}

/* Settings > Weather: find a town with Open-Meteo's geocoder, or turn it off. */
static void weather_pick(void) {
    const char *pl = weather_place();
    if (pl && *pl) {
        static const char *const items[] = {"Change town", "Turn the weather off"};
        int k = ui_menu("Weather", items, 2);
        if (k == 1) { weather_off(); ui_toast("Weather off", C_ACCENT); return; }
        if (k != 0) return;
    }
    char q[64] = "";
    if (!ui_ask_text("Town or city", q, sizeof(q)) || !q[0]) return;
    static char names[6][96];
    float lat[6], lon[6];
    int us[6];
    int n = weather_search(q, names, lat, lon, us, 6);
    if (n < 0) { ui_message("Weather", "Could not reach the weather service. Check Wi-Fi and try again."); return; }
    if (n == 0) { ui_message("Weather", "No town by that name. Try the nearest city."); return; }
    const char *items[6];
    for (int i = 0; i < n; ++i) items[i] = names[i];
    int k = n == 1 ? 0 : ui_menu("Which one?", items, n);
    if (k < 0) return;
    weather_set(names[k], lat[k], lon[k], us[k]);
    char msg[128];
    snprintf(msg, sizeof(msg), "Weather: %.90s", names[k]);
    ui_toast(msg, C_OK);
}

#define WALLPAPER_DIR "ux0:data/arcadehub/wallpapers/"
#define THEME_CFG "ux0:data/arcadehub/user/theme.cfg"

static void theme_save(void) {
    char b[128];
    const char *w = ui_theme_wallpaper();
    int n = snprintf(b, sizeof(b), "%d %d %s\n", ui_theme_accent_index(), (int)ui_theme_bg(), w[0] ? w : "-");
    ui_save(THEME_CFG, b, n, 0);
}

/* Settings > Theme > Background > Wallpaper: whatever JPG/PNG sits in the folder. */
static void wallpaper_pick(void) {
    static char names[8][256];   /* SceIoDirent's own d_name size: no truncation warning */
    int n = 0;
    SceUID d = sceIoDopen(WALLPAPER_DIR);
    if (d >= 0) {
        SceIoDirent e;
        while (n < 8) {
            memset(&e, 0, sizeof(e));
            if (sceIoDread(d, &e) <= 0) break;
            if (e.d_name[0] == '.' || SCE_S_ISDIR(e.d_stat.st_mode)) continue;
            const char *dot = strrchr(e.d_name, '.');
            if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png"))) continue;
            snprintf(names[n], sizeof(names[n]), "%s", e.d_name);
            ++n;
        }
        sceIoDclose(d);
    }
    if (!n) {
        ui_message("Wallpaper", "No pictures yet. Put a JPG or PNG in ux0:data/arcadehub/wallpapers/ and come back.");
        return;
    }
    const char *items[8];
    for (int i = 0; i < n; ++i) items[i] = names[i];
    int k = ui_menu("Wallpaper", items, n);
    if (k < 0) return;
    ui_theme_set_bg(THEME_BG_WALLPAPER, names[k]);
    theme_save();
    ui_toast("Wallpaper set", C_OK);
}

/* Settings > Theme: the accent (follow the art, or a fixed colour) and the
 * background behind tabs with no art of their own. */
static void theme_pick(void) {
    char row1[64], row2[64];
    snprintf(row1, sizeof(row1), "Accent: %s", ui_theme_accent_name(ui_theme_accent_index()));
    snprintf(row2, sizeof(row2), "Background: %s", ui_theme_bg_name(ui_theme_bg()));
    const char *items[2] = {row1, row2};
    int k = ui_menu("Theme", items, 2);
    if (k == 0) {
        static const char *const acc_items[] = {"Match the art", "Blue", "Purple", "Pink", "Orange", "Green", "Teal"};
        int a = ui_menu("Accent colour", acc_items, 7);
        if (a < 0) return;
        ui_theme_set_accent(a - 1);
        theme_save();
        ui_toast(a == 0 ? "Accent: match the art" : "Accent set", C_ACCENT);
    } else if (k == 1) {
        static const char *const bg_items[] = {"Aurora", "Plain", "Midnight", "Wallpaper"};
        int b = ui_menu("Background", bg_items, 4);
        if (b < 0) return;
        if (b == THEME_BG_WALLPAPER) { wallpaper_pick(); return; }   /* its own picker, and its own save */
        ui_theme_set_bg((ThemeBg)b, NULL);
        theme_save();
        ui_toast("Background set", C_ACCENT);
    }
}

/* House, moon, circular arrow, power: small white images made once (with
 * real transparency, so they sit on any tile colour), tinted when drawn. */
static float inside_icon(int k, float x, float y) {   /* 64 x 64 space; 1 = ink */
    float dx = x - 32, dy = y - 32, d = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx) * 57.2958f;
    switch (k) {
    case 0: {                                            /* house */
        int roof = y >= 10 && y <= 32 && fabsf(dx) <= (y - 10) * 1.25f;
        int body = y > 30 && y <= 52 && fabsf(dx) <= 17;
        int door = y > 38 && fabsf(dx) <= 5;
        return (roof || body) && !door;
    }
    case 1: {                                            /* moon: a disc minus a disc */
        float ex = x - 41, ey = y - 24;
        return d <= 21 && sqrtf(ex * ex + ey * ey) > 17;
    }
    case 2: {                                            /* restart: a ring with a gap, arrow at its end */
        int ring = d >= 15 && d <= 21 && !(a > -95 && a < -35);
        float tx = x - 36, ty = y - 13;                  /* arrowhead near the top of the gap */
        int arrow = tx >= -9 && tx <= 9 && ty >= -9 && ty <= 9 && tx >= -ty * 0.1f - 1 && fabsf(ty) <= 9 - fabsf(tx) * 0.2f && tx <= 9 - fabsf(ty);
        return ring || arrow;
    }
    default: {                                           /* power: ring open at the top, and a bar */
        int ring = d >= 15 && d <= 21 && !(a > -125 && a < -55);
        int bar = fabsf(dx) <= 3.2f && y >= 5 && y <= 32;
        return ring || bar;
    }
    }
}

static vita2d_texture *sys_tex(int k) {
    static vita2d_texture *t[4];
    if (t[k]) return t[k];
    t[k] = vita2d_create_empty_texture(64, 64);
    if (!t[k]) return NULL;
    unsigned int *px = vita2d_texture_get_datap(t[k]), stride = vita2d_texture_get_stride(t[k]) / 4;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            float c = 0;                                 /* 4x supersampled edges */
            for (int sy = 0; sy < 2; ++sy) for (int sx = 0; sx < 2; ++sx) c += inside_icon(k, x + 0.25f + sx * 0.5f, y + 0.25f + sy * 0.5f);
            px[y * stride + x] = 0x00FFFFFFu | ((unsigned int)(c / 4 * 255) << 24);
        }
    vita2d_texture_set_filters(t[k], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t[k];
}

static void sys_icon(int k, float cx, float cy, unsigned int c) {
    vita2d_texture *t = sys_tex(k);
    if (t) vita2d_draw_texture_tint_scale(t, cx - 20, cy - 20, 0.625f, 0.625f, c);   /* 40 px */
}

/* ---------- the page: sections on the left, their rows on the right ----------
 * (2026-10-01: six cards had grown "chaotic and cramped"; one section at a
 * time, like the PS5's and the Switch's Settings, leaves room to grow.) */

enum { S_DISPLAY, S_POWER, S_NETWORK, S_STORAGE, S_VITAOS, S_ABOUT, NSECT };
static const char *const sect_names[NSECT] = {"Display & sound", "Battery & power", "Network", "Storage", "VitaOS", "About this Vita"};
static int sect, in_rows, rsel;             /* in_rows: focus is on the right-hand rows */
static int pbtn = -1;                       /* the power buttons under the sections: 0 sleep 1 restart 2 power off */
static float scroll, scroll_to;
static void page_reset(void) { in_rows = 0; rsel = 0; pbtn = -1; scroll = scroll_to = 0; }

enum { R_INFO, R_SLIDER, R_ACTION, R_TOGGLE };
enum { A_NONE, A_BRIGHT, A_VOLUME, A_SFX, A_AMBIENT, A_THEME, A_BATTERY, A_BUBBLES, A_SLEEP, A_RESTART, A_POWEROFF,
       A_LANG, A_TEXTSIZE, A_WIFI, A_BLUETOOTH, A_WEATHER, A_STORAGE, A_UPDATE, A_BOOT, A_MOVIES, A_MUSIC, A_KIOSK, A_RESCAN, A_ART, A_ABOUT };
typedef struct { int kind, act; const char *label, *sub; char value[96]; float frac; int on; unsigned int color; } Row;
#define MAXROWS 12
static Row rows[MAXROWS];
static int nrows;

static Row *add(int kind, int act, const char *label) {
    if (nrows >= MAXROWS) return &rows[MAXROWS - 1];
    Row *r = &rows[nrows++];
    memset(r, 0, sizeof(*r));
    r->kind = kind; r->act = act; r->label = label; r->frac = -1; r->color = C_TEXT;
    return r;
}

#define BOOT_OFF "ux0:data/arcadehub/user/boot.off"
#define HIDE_MOVIES "ux0:data/arcadehub/user/hide-movies"
#define HIDE_MUSIC "ux0:data/arcadehub/user/hide-music"
#ifdef VITAOS_KIOSK
#define KIOSK_ON "ux0:data/arcadehub/user/kiosk.on"   /* read by home/plugin/kiosk.h */
#endif
static int flag_set(const char *path) { SceIoStat st; return sceIoGetstat(path, &st) >= 0; }
static void flag_write(const char *path, int on) {
    if (on) {
        sceIoMkdir("ux0:data/arcadehub/user", 0777);
        SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (f >= 0) sceIoClose(f);
    } else sceIoRemove(path);
}

static void build_rows(void) {
    nrows = 0;
    Row *r;
    switch (sect) {
    case S_DISPLAY:
        r = add(R_SLIDER, A_BRIGHT, "Brightness");
        r->frac = (float)(brightness - BRIGHT_MIN) / (BRIGHT_MAX - BRIGHT_MIN);
        snprintf(r->value, sizeof(r->value), "%d%%", (int)(r->frac * 100 + 0.5f));
        r = add(R_SLIDER, A_VOLUME, "Volume");
        r->frac = volume / (float)VOL_MAX;
        snprintf(r->value, sizeof(r->value), "%d / %d", volume, VOL_MAX);
        r = add(R_SLIDER, A_SFX, "UI sounds");
        r->frac = sfx_level() / 10.0f;
        if (!sfx_ok()) snprintf(r->value, sizeof(r->value), "no audio port");
        else if (sfx_level()) snprintf(r->value, sizeof(r->value), "%d / 10", sfx_level());
        else snprintf(r->value, sizeof(r->value), "Off");
        r = add(R_SLIDER, A_AMBIENT, "Home music");
        r->frac = sfx_ambient_level() / 10.0f;
        if (sfx_ambient_level()) snprintf(r->value, sizeof(r->value), "%d / 10", sfx_ambient_level());
        else snprintf(r->value, sizeof(r->value), "Off");
        r = add(R_ACTION, A_THEME, "Theme");
        snprintf(r->value, sizeof(r->value), "%s accent, %s", ui_theme_accent_name(ui_theme_accent_index()), ui_theme_bg_name(ui_theme_bg()));
        r = add(R_TOGGLE, A_TEXTSIZE, "Large text");
        r->on = text_large();
        r = add(R_ACTION, A_LANG, "Language");
        if (lang_choice()) snprintf(r->value, sizeof(r->value), "%s", lang_name(lang_choice()));
        else snprintf(r->value, sizeof(r->value), "%s (%s)", lang_name(0), lang_active_name());
        break;
    case S_POWER: {
        r = add(R_INFO, A_NONE, "Battery");
        snprintf(r->value, sizeof(r->value), "%d%%%s", s.battery, s.charging ? "  charging" : s.plugged ? "  plugged in" : "");
        r->frac = s.battery / 100.0f;
        if (s.battery < 15 && !s.plugged) r->color = C_BAD;
        r = add(R_INFO, A_NONE, "Time left");
        if (s.minutes > 0 && !s.plugged) snprintf(r->value, sizeof(r->value), "%dh %02dm", s.minutes / 60, s.minutes % 60);
        else snprintf(r->value, sizeof(r->value), "%s", s.plugged ? "On external power" : "Estimating");
        r = add(R_ACTION, A_BATTERY, "Battery health");
        int soh = scePowerGetBatterySOH();
        if (soh > 0) snprintf(r->value, sizeof(r->value), "%d%%", soh);
        r->sub = "Time per charge, and what a new battery would give";
        r = add(R_ACTION, A_BUBBLES, "System home");
        r->sub = "PS opens the system's bubbles for 30 s";
        break;
    }
    case S_NETWORK:
        r = add(R_INFO, A_NONE, "Wi-Fi");
        snprintf(r->value, sizeof(r->value), "%s", s.ssid[0] ? s.ssid : "Not connected");
        if (!s.ssid[0]) r->color = C_BAD;
        if (s.rssi >= 0) {
            r = add(R_INFO, A_NONE, "Signal");
            snprintf(r->value, sizeof(r->value), "%d%%", s.rssi);
            r->frac = s.rssi / 100.0f;
            if (s.rssi < 30) r->color = C_BAD;
        }
        r = add(R_INFO, A_NONE, "Address");
        snprintf(r->value, sizeof(r->value), "%s", s.ip[0] ? s.ip : "-");
        if (s.remote) { r = add(R_INFO, A_NONE, "Agents"); snprintf(r->value, sizeof(r->value), "Remote listening"); r->color = C_OK; }
        r = add(R_ACTION, A_WIFI, "Wi-Fi networks");
        r->sub = "Join or switch networks in the system's Wi-Fi page; PS comes back";
        r = add(R_ACTION, A_BLUETOOTH, "Bluetooth devices");
        r->sub = "Pairing opens the system Settings; PS comes back here";
        r = add(R_ACTION, A_WEATHER, "Weather");
        {
            const char *pl = weather_place();
            snprintf(r->value, sizeof(r->value), "%s", pl && *pl ? pl : "Off");
        }
        r->sub = "The town on Home's weather widget";
        break;
    case S_STORAGE: {
        static const char *const devs[] = {"ux0:", "ur0:", "uma0:", "imc0:"};
        static const char *const what[] = {"Memory card", "System", "USB / SD2Vita", "Internal"};
        static char labels[4][40];
        for (int i = 0; i < 4; ++i) {
            if (!s.dev_ok[i]) continue;
            char fr[32], tot[32];
            human_size(s.dev[i].free_size, fr, sizeof(fr));
            human_size(s.dev[i].max_size, tot, sizeof(tot));
            snprintf(labels[i], sizeof(labels[i]), "%s  %s", devs[i], what[i]);
            r = add(R_INFO, A_NONE, labels[i]);
            snprintf(r->value, sizeof(r->value), "%s free of %s", fr, tot);
            r->frac = 1.0f - (float)s.dev[i].free_size / s.dev[i].max_size;
        }
        r = add(R_ACTION, A_STORAGE, "Manage storage");
        r->sub = "Biggest folders, and clean up crash dumps and logs";
        break;
    }
    case S_VITAOS: {
        const char *nv = update_newer(), *um = "";
        float uf = 0;
        int st = update_stage(&um, &uf);
        if (nv || st) {
            r = add(R_ACTION, A_UPDATE, "Update");
            if (!st) snprintf(r->value, sizeof(r->value), "VitaOS %s", nv);
            else if (st == 1) { snprintf(r->value, sizeof(r->value), "Downloading %d%%", (int)(uf * 100)); r->frac = uf; }
            else snprintf(r->value, sizeof(r->value), "%s", um);
            r->sub = st ? NULL : "Press X (or START anywhere in Settings)";
            r->color = st == 9 ? C_BAD : C_ACCENT;
        }
        r = add(R_TOGGLE, A_BOOT, "Start at boot");
        r->on = !flag_set(BOOT_OFF);
        r->sub = "Open VitaOS at power-on (needs the PS plugin)";
#ifdef VITAOS_KIOSK
        r = add(R_TOGGLE, A_KIOSK, "Hide the bubbles");
        r->on = flag_set(KIOSK_ON);
        r->sub = "VitaOS comes back when a game ends or the bubbles sit idle";
#endif
        r = add(R_TOGGLE, A_MOVIES, "Movies tab");
        r->on = !flag_set(HIDE_MOVIES);
        r = add(R_TOGGLE, A_MUSIC, "Music tab");
        r->on = !flag_set(HIDE_MUSIC);
        r = add(R_ACTION, A_RESCAN, "Find games again");
        r->sub = "Look through the card for new games";
        r = add(R_ACTION, A_ART, "Download box art");
        r->sub = "For games that have none yet";
        break;
    }
    default:
        r = add(R_ACTION, A_ABOUT, "VitaOS");
        snprintf(r->value, sizeof(r->value), "%s", VITAOS_VERSION);
        r->sub = "Licence, credits and source";
        r = add(R_INFO, A_NONE, "CPU");
        snprintf(r->value, sizeof(r->value), "%d MHz", s.arm);
        if (s.arm < 100) r->color = C_BAD;               /* 1 MHz looks like a broken emulator */
        r = add(R_INFO, A_NONE, "Bus");
        snprintf(r->value, sizeof(r->value), "%d MHz", s.bus);
        r = add(R_INFO, A_NONE, "GPU / crossbar");
        snprintf(r->value, sizeof(r->value), "%d / %d MHz", s.gpu, s.xbar);
        break;
    }
}

static int focusable(int i) { return i >= 0 && i < nrows && rows[i].kind != R_INFO; }

static void adjust(int act, int dir) {
    if (act == A_BRIGHT) set_brightness(brightness + dir * (BRIGHT_MAX / 20));
    if (act == A_VOLUME) set_volume(volume + dir);
    if (act == A_SFX) { sfx_set_level(sfx_level() + dir); sfx_play(SFX_SELECT); }   /* hear the new level */
    if (act == A_AMBIENT) sfx_set_ambient_level(sfx_ambient_level() + dir);
}

static void set_slider(int act, float f) {
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    if (act == A_BRIGHT) set_brightness(BRIGHT_MIN + (int)(f * (BRIGHT_MAX - BRIGHT_MIN)));
    if (act == A_VOLUME) set_volume((int)(f * VOL_MAX + 0.5f));
    if (act == A_SFX) sfx_set_level((int)(f * 10 + 0.5f));
    if (act == A_AMBIENT) sfx_set_ambient_level((int)(f * 10 + 0.5f));
}

static void activate(Row *r) {
    switch (r->act) {
    case A_THEME: theme_pick(); break;
    case A_TEXTSIZE: text_set_large(!r->on); break;
    case A_LANG: {
        const char *items[8];
        int n = lang_count();
        for (int i = 0; i < n && i < 8; ++i) items[i] = lang_name(i);
        int k = ui_menu("Language", items, n);
        if (k >= 0) lang_set(k);
        break;
    }
    case A_BATTERY: battery_open(); break;
    case A_STORAGE: storage_open(); break;
    case A_WEATHER: weather_pick(); break;
    case A_BUBBLES: if (release_ps) release_ps(); break;
    case A_WIFI:                                         /* apps cannot scan or join networks themselves */
        if (release_ps) release_ps();
        if (sceAppMgrLaunchAppByUri(0x20000, "settings_dlg:wifi") < 0 && sceAppMgrLaunchAppByUri(0x20000, "settings_dlg:") < 0)
            ui_toast("Settings would not open", C_BAD);
        break;
    case A_BLUETOOTH:                                    /* pairing lives in the system Settings app */
        if (release_ps) release_ps();                    /* so PS can bring Home back */
        if (sceAppMgrLaunchAppByUri(0x20000, "settings_dlg:") < 0) ui_toast("Settings would not open", C_BAD);
        else ui_toast("Devices > Bluetooth Devices. PS comes back here.", C_ACCENT);
        break;
    case A_SLEEP:
        if (ui_confirm("Sleep", "Put the Vita to sleep? Wi-Fi goes off too, so agents cannot reach it until you wake it with the power button."))
            scePowerRequestSuspend();
        break;
    case A_RESTART:
        if (ui_confirm("Restart", "Restart the Vita now? Anything unsaved in a suspended game is lost.")) scePowerRequestColdReset();
        break;
    case A_POWEROFF:
        if (ui_confirm("Power off", "Turn the Vita off? Hold the power button to turn it back on.")) scePowerRequestStandby();
        break;
    case A_UPDATE: { const char *um; float uf; if (!update_stage(&um, &uf)) update_get(); break; }
    case A_BOOT:
        flag_write(BOOT_OFF, r->on);
        ui_toast(r->on ? "VitaOS will not open at power-on" : "VitaOS opens at power-on (needs the PS plugin)", r->on ? C_ACCENT : C_OK);
        break;
    case A_MOVIES:                                       /* Reddit, 2026-09-27: hide Movies and Music */
        flag_write(HIDE_MOVIES, r->on);
        ui_toast(r->on ? "Movies tab hidden" : "Movies tab shown", C_OK);
        break;
    case A_MUSIC:
        flag_write(HIDE_MUSIC, r->on);
        ui_toast(r->on ? "Music tab hidden" : "Music tab shown", C_OK);
        break;
#ifdef VITAOS_KIOSK
    case A_KIOSK:
        if (r->on) { flag_write(KIOSK_ON, 0); ui_toast("The bubbles are back", C_ACCENT); }
        else if (ui_confirm("Hide the bubbles",
                            /* four lines at most: the card's buttons sit below them */
                            "VitaOS comes back by itself when a game ends or the bubbles sit idle.\n"
                            "Hold L while turning the Vita on to skip VitaOS's plugins once, or turn this off here.")) {
            flag_write(KIOSK_ON, 1);
            ui_toast(flag_set(KIOSK_ON) ? "Bubbles hidden (needs the PS plugin 1.4)" : "Could not save the setting",
                     flag_set(KIOSK_ON) ? C_OK : C_BAD);
        }
        break;
#endif
    case A_RESCAN: if (lib_rescan) lib_rescan(); break;
    case A_ART: if (lib_art) lib_art(); break;
    case A_ABOUT:
        ui_message("About VitaOS",
                   "VitaOS " VITAOS_VERSION ": a modern home screen for the PS Vita.\n"
                   "Open source (GPLv3): github.com/mvizensk/VitaOS\n"
                   "Not affiliated with or endorsed by Sony Interactive Entertainment. "
                   "PlayStation and PS Vita are trademarks of Sony Interactive Entertainment.\n"
                   "Console photos: Evan-Amos, Wikimedia Commons. Weather: Open-Meteo.");
        break;
    }
}

const char *settings_hint(void) {
    if (storage_active()) return storage_hint();
    if (battery_active()) return battery_hint();
    if (pbtn >= 0) return "\xE2\x86\x90 \xE2\x86\x92 choose    X select    \xE2\x86\x91 sections    L R tabs";
    if (!in_rows) return "\xE2\x86\x91 \xE2\x86\x93 section    X open    L R tabs";
    if (focusable(rsel) && rows[rsel].kind == R_SLIDER) return "\xE2\x86\x91 \xE2\x86\x93 choose    \xE2\x86\x90 \xE2\x86\x92 adjust    O back    L R tabs";
    return "\xE2\x86\x91 \xE2\x86\x93 choose    X select    O back    L R tabs";
}

#define SIDE_W 262
#define SIDE_Y 84
#define SIDE_H 50
#define PB_S 62                      /* the power buttons: size, gap, where */
#define PB_GAP 18
#define PB_X0 ((SIDE_W - 3 * PB_S - 2 * PB_GAP) / 2)
#define PB_Y 398
#define CX (SIDE_W + 34)
#define CW_ (W - CX - 34)
#define CY 116
#define RH 56

void settings_update(const Input *in) {
    if (storage_active()) { storage_update(in); return; }
    if (battery_active()) { battery_update(in); return; }
    SceUInt64 now = sceKernelGetProcessTimeWide();
    wanted_until = now + 3000000;
    if (!last) {
        last = now;
        SceUID t = sceKernelCreateThread("settings_refresh", refresher, 0x10000110, 0x4000, 0, 0, NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
    build_rows();
    unsigned int p = in->pressed;

    /* START updates from anywhere on the page (2026-09-29: "an update button in the settings") */
    if (p & SCE_CTRL_START) { const char *um; float uf; if (update_newer() && !update_stage(&um, &uf)) update_get(); }

    static const int pacts[3] = {A_SLEEP, A_RESTART, A_POWEROFF};
    if (pbtn >= 0) {                                                   /* the power buttons, lower left */
        if (p & SCE_CTRL_UP) pbtn = -1;
        if (p & SCE_CTRL_LEFT && pbtn > 0) pbtn--;
        if (p & SCE_CTRL_RIGHT && pbtn < 2) pbtn++;
        if (p & SCE_CTRL_CROSS) { Row tmp; memset(&tmp, 0, sizeof(tmp)); tmp.act = pacts[pbtn]; activate(&tmp); }
    } else if (!in_rows) {
        if (p & SCE_CTRL_UP && sect > 0) { sect--; scroll = scroll_to = 0; }
        if (p & SCE_CTRL_DOWN) { if (sect < NSECT - 1) { sect++; scroll = scroll_to = 0; } else pbtn = 0; }
        if (p & (SCE_CTRL_CROSS | SCE_CTRL_RIGHT)) {
            build_rows();
            rsel = 0;
            while (rsel < nrows && !focusable(rsel)) rsel++;
            if (rsel < nrows) in_rows = 1;
        }
    } else {
        if (!focusable(rsel)) { rsel = 0; while (rsel < nrows && !focusable(rsel)) rsel++; }
        if (p & SCE_CTRL_UP) { int k = rsel - 1; while (k >= 0 && !focusable(k)) k--; if (k >= 0) rsel = k; }
        if (p & SCE_CTRL_DOWN) { int k = rsel + 1; while (k < nrows && !focusable(k)) k++; if (k < nrows) rsel = k; }
        Row *r = rsel < nrows ? &rows[rsel] : NULL;
        int dir = (p & SCE_CTRL_RIGHT) ? 1 : (p & SCE_CTRL_LEFT) ? -1 : 0;
        if (r && r->kind == R_SLIDER && dir) adjust(r->act, dir);
        else if (dir < 0 || (p & SCE_CTRL_CIRCLE)) in_rows = 0;
        if (r && (p & SCE_CTRL_CROSS) && r->kind != R_SLIDER) activate(r);
    }

    /* touch: a section, or a row (a slider takes the spot you touched) */
    if (in->tapped) {
        int tx = in->tap_x, ty = in->tap_y;
        if (tx < SIDE_W && ty >= PB_Y - 6 && ty < PB_Y + PB_S + 6) {   /* a power button */
            int k = (tx - PB_X0 + PB_GAP / 2) / (PB_S + PB_GAP);
            if (k >= 0 && k < 3) { pbtn = k; in_rows = 0; Row tmp; memset(&tmp, 0, sizeof(tmp)); tmp.act = pacts[k]; activate(&tmp); }
        } else if (tx < SIDE_W && ty >= SIDE_Y) {
            int k = (ty - SIDE_Y) / SIDE_H;
            if (k >= 0 && k < NSECT) { if (k != sect) scroll = scroll_to = 0; sect = k; in_rows = 0; pbtn = -1; }
        } else if (tx >= CX - 12 && ty >= CY - 8) {
            build_rows();
            int k = (int)((ty - CY + 8 + scroll) / RH);
            if (focusable(k)) {
                in_rows = 1; rsel = k; pbtn = -1;
                if (rows[k].kind == R_SLIDER) set_slider(rows[k].act, (float)(tx - CX) / CW_);
                else activate(&rows[k]);
            }
        }
    }
    if (in->touching && in_rows && focusable(rsel) && rows[rsel].kind == R_SLIDER && in->tx >= CX - 12) {
        float y = CY - 8 + rsel * RH - scroll;                        /* drag along the focused slider */
        if (in->ty >= y && in->ty < y + RH) set_slider(rows[rsel].act, (float)(in->tx - CX) / CW_);
    }
    build_rows();                                                      /* values after this frame's changes */

    /* ---- the sidebar ---- */
    vita2d_draw_rectangle(0, 65, SIDE_W, H - 105, C_PANEL);
    const char *um; float uf;
    int update_dot = update_newer() && !update_stage(&um, &uf);
    for (int i = 0; i < NSECT; ++i) {
        int y = SIDE_Y + i * SIDE_H, on = i == sect;
        if (on) {
            draw_round_rect(12, y + 2, SIDE_W - 24, SIDE_H - 6, 12, in_rows || pbtn >= 0 ? C_SEL_DIM : C_SEL);
            vita2d_draw_rectangle(12, y + 12, 3, SIDE_H - 26, C_ACCENT);
        }
        text(on ? bold : font, 30, y + 31, on ? C_TEXT : C_DIM, 17, sect_names[i]);
        if (i == S_VITAOS && update_dot) draw_round_rect(SIDE_W - 34, y + 19, 10, 10, 5, C_ACCENT);   /* an update waits */
    }

    /* Sleep, Restart and Power off: big icons in the lower left, easy to
     * find and to tap (2026-10-01). */
    static const char *const pnames[3] = {"Sleep", "Restart", "Power off"};
    for (int k = 0; k < 3; ++k) {
        int bx = PB_X0 + k * (PB_S + PB_GAP), on = pbtn == k;
        if (on) draw_focus_r(bx, PB_Y, PB_S, PB_S, 1, 18);
        draw_round_rect(bx, PB_Y, PB_S, PB_S, 18, on ? RGBA8(255, 255, 255, 40) : RGBA8(255, 255, 255, 16));
        sys_icon(k + 1, bx + PB_S / 2, PB_Y + PB_S / 2, k == 2 ? (on ? RGBA8(255, 120, 120, 255) : RGBA8(230, 110, 110, 220)) : on ? C_TEXT : C_DIM);
        int tw = text_w(font, 13, pnames[k]);
        text(font, bx + PB_S / 2 - tw / 2, PB_Y + PB_S + 18, on ? C_TEXT : C_FAINT, 13, pnames[k]);
    }

    /* ---- the rows ---- */
    text(bold, CX, CY - 22, C_TEXT, 22, sect_names[sect]);
    {                                                                  /* keep the focused row in view */
        float vh = H - 52 - (CY - 8), ry = in_rows ? rsel * RH : 0;
        if (!in_rows) scroll_to = 0;
        else if (ry < scroll_to) scroll_to = ry;
        else if (ry + RH > scroll_to + vh) scroll_to = ry + RH - vh;
        scroll += (scroll_to - scroll) * 0.25f;
    }
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(CX - 16, CY - 8, W, H - 44);
    for (int i = 0; i < nrows; ++i) {
        Row *r = &rows[i];
        float y = CY - 8 + i * RH - scroll;
        if (y < CY - 8 - RH || y > H) continue;
        int on = in_rows && i == rsel;
        if (on) draw_round_rect(CX - 14, y, CW_ + 28, RH - 6, 12, C_SEL);
        else if (i) vita2d_draw_rectangle(CX, y - 3, CW_, 1, C_LINE);
        unsigned int lc = r->kind == R_INFO ? C_DIM : on ? C_TEXT : RGBA8(220, 224, 232, 255);
        int ly = r->sub || r->kind == R_SLIDER || r->frac >= 0 ? (int)y + 22 : (int)y + 31;
        text(on ? bold : font, CX, ly, lc, 17, r->label);
        if (r->sub) text_fit(font, CX, (int)y + 41, on ? RGBA8(236, 239, 244, 170) : C_FAINT, 13, r->sub, CW_ - 230);
        if (r->kind == R_TOGGLE) {                                     /* a switch */
            int sx = CX + CW_ - 52, sy = (int)y + 13;
            draw_round_rect(sx, sy, 52, 26, 13, r->on ? C_OK : RGBA8(255, 255, 255, 40));
            draw_round_rect(r->on ? sx + 28 : sx + 2, sy + 2, 22, 22, 11, RGBA8(255, 255, 255, 240));
        } else {
            int vx = CX + CW_ - (r->kind == R_ACTION ? 22 : 0);
            if (r->value[0]) text_right(font, vx, ly, r->color == C_TEXT ? C_DIM : r->color, 16, r->value);
            if (r->kind == R_ACTION) text_right(bold, CX + CW_, ly, on ? C_TEXT : C_FAINT, 18, "\xE2\x80\xBA");
        }
        if (r->frac >= 0) {
            int bw = r->kind == R_SLIDER ? CW_ : CW_ - 230;
            draw_bar(CX, (int)y + 34, bw, 6, r->frac, r->kind == R_SLIDER ? C_ACCENT : r->color == C_BAD ? C_BAD : C_OK);
        }
    }
    vita2d_disable_clipping();
}
