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
static int brightness = -1, volume = -1, focus;
static SceUInt64 last;

enum { F_BRIGHT, F_VOLUME, F_SFX, F_AMBIENT, F_STORAGE, F_WEATHER, F_BLUETOOTH, F_ABOUT, F_THEME, F_BUBBLES, F_SLEEP, F_RESTART, F_POWEROFF, NFOCUS };
static void (*release_ps)(void);
void settings_on_release_ps(void (*fn)(void)) { release_ps = fn; }
static void (*lib_rescan)(void), (*lib_art)(void);
void settings_on_library(void (*rescan)(void), void (*art)(void)) { lib_rescan = rescan; lib_art = art; }
void settings_leave(void) { storage_close(); focus = 0; }

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

static void card(int x, int y, int w, int h, const char *title) {
    vita2d_draw_rectangle(x, y, w, h, C_PANEL);
    text(bold, x + 18, y + 30, C_TEXT, 19, title);
}

static void row(int x, int y, const char *k, const char *v, unsigned int color) {
    text(font, x, y, C_DIM, 16, k);
    text_fit(font, x + 110, y, color, 16, v, 170);
}

/* A slider row: highlighted when focused, with its value on the right. */
static void slider(int x, int y, const char *k, float frac, const char *v, int on) {
    if (on) vita2d_draw_rectangle(x - 8, y - 18, 290, 36, C_SEL);
    text(font, x, y, on ? C_TEXT : C_DIM, 16, k);
    text_right(font, x + 272, y, on ? C_TEXT : C_DIM, 16, v);
    draw_bar(x, y + 10, 272, 6, frac, C_ACCENT);
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
    if (t) vita2d_draw_texture_tint_scale(t, cx - 16, cy - 16, 0.5f, 0.5f, c);
}

static void button_row(int x, int y, const char *label, const char *sub, int on) {
    if (on) vita2d_draw_rectangle(x - 8, y - 22, 290, 42, C_SEL);
    text(font, x, y, on ? C_TEXT : C_DIM, 17, label);
    text(font, x, y + 16, C_FAINT, 13, sub);
}

const char *settings_hint(void) {
    if (storage_active()) return storage_hint();
    return "UP DOWN choose   <- -> adjust   X select   L R tabs";
}

void settings_update(const Input *in) {
    if (storage_active()) { storage_update(in); return; }
    SceUInt64 now = sceKernelGetProcessTimeWide();
    wanted_until = now + 3000000;
    if (!last) {
        last = now;
        SceUID t = sceKernelCreateThread("settings_refresh", refresher, 0x10000110, 0x4000, 0, 0, NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
    wanted_until = now + 3000000;

    /* The four system buttons sit in a row: left/right moves along it, up
     * leaves it; everywhere else up/down steps through the rows. */
    int sysrow = focus >= F_BUBBLES;
    if (in->pressed & SCE_CTRL_UP) focus = sysrow ? F_THEME : (focus + NFOCUS - 1) % NFOCUS;   /* Theme sits just above the system row */
    if (in->pressed & SCE_CTRL_DOWN) focus = sysrow ? focus : focus + 1;
    if (sysrow && (in->pressed & SCE_CTRL_LEFT) && focus > F_BUBBLES) focus--;
    if (sysrow && (in->pressed & SCE_CTRL_RIGHT) && focus < F_POWEROFF) focus++;
    int dir = sysrow ? 0 : (in->pressed & SCE_CTRL_RIGHT) ? 1 : (in->pressed & SCE_CTRL_LEFT) ? -1 : 0;
    if (dir && focus == F_BRIGHT) set_brightness(brightness + dir * (BRIGHT_MAX / 20));
    if (dir && focus == F_VOLUME) set_volume(volume + dir);
    if (dir && focus == F_SFX) { sfx_set_level(sfx_level() + dir); sfx_play(SFX_SELECT); }   /* hear the new level */
    if (dir && focus == F_AMBIENT) sfx_set_ambient_level(sfx_ambient_level() + dir);
    if (in->pressed & SCE_CTRL_CROSS) {
        if (focus == F_STORAGE) storage_open();
        if (focus == F_BUBBLES && release_ps) release_ps();
        if (focus == F_WEATHER) weather_pick();
        if (focus == F_THEME) theme_pick();
        static char get_label[40], boot_label[48], movies_label[40], music_label[40];
        /* Start at boot: the PS plugin (1.3) opens VitaOS after power-on
         * unless user/boot.off is there (asked for 2026-09-27). */
        SceIoStat bst;
        int boot_on = sceIoGetstat("ux0:data/arcadehub/user/boot.off", &bst) < 0;
        snprintf(boot_label, sizeof(boot_label), "Start at boot: %s", boot_on ? "On" : "Off");
        SceIoStat hst;
        int movies_on = sceIoGetstat("ux0:data/arcadehub/user/hide-movies", &hst) < 0;
        int music_on = sceIoGetstat("ux0:data/arcadehub/user/hide-music", &hst) < 0;
        snprintf(movies_label, sizeof(movies_label), "Movies tab: %s", movies_on ? "Shown" : "Hidden");
        snprintf(music_label, sizeof(music_label), "Music tab: %s", music_on ? "Shown" : "Hidden");
        const char *vitaos_items[7] = {"Find games again", "Download box art", "About VitaOS", boot_label,
                                       movies_label, music_label, get_label};
        const char *newer = update_newer();
        if (newer) snprintf(get_label, sizeof(get_label), "Update to VitaOS %s", newer);
        int pick = focus == F_ABOUT ? ui_menu("VitaOS", (const char *const *)vitaos_items, newer ? 7 : 6) : -1;
        if (pick == 4 || pick == 5) {                /* Reddit, 2026-09-27: hide Movies and Music */
            const char *flag = pick == 4 ? "ux0:data/arcadehub/user/hide-movies" : "ux0:data/arcadehub/user/hide-music";
            int on = pick == 4 ? movies_on : music_on;
            if (on) { SceUID hf = sceIoOpen(flag, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666); if (hf >= 0) sceIoClose(hf); }
            else sceIoRemove(flag);
            ui_toast(on ? (pick == 4 ? "Movies tab hidden" : "Music tab hidden") : (pick == 4 ? "Movies tab shown" : "Music tab shown"), C_OK);
        }
        if (pick == 3) {
            if (boot_on) {
                SceUID bf = sceIoOpen("ux0:data/arcadehub/user/boot.off", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                if (bf >= 0) sceIoClose(bf);
                ui_toast("VitaOS will not open at power-on", C_ACCENT);
            } else {
                sceIoRemove("ux0:data/arcadehub/user/boot.off");
                ui_toast("VitaOS opens at power-on (needs the PS plugin)", C_OK);
            }
        }
        if (pick == 6) update_get();
        if (pick == 0 && lib_rescan) lib_rescan();
        if (pick == 1 && lib_art) lib_art();
        if (pick == 2)
            ui_message("About VitaOS",
                       "VitaOS " VITAOS_VERSION ": a modern home screen for the PS Vita.\n"
                       "Open source (GPLv3): github.com/mvizensk/VitaOS\n"
                       "Not affiliated with or endorsed by Sony Interactive Entertainment. "
                       "PlayStation and PS Vita are trademarks of Sony Interactive Entertainment.\n"
                       "Console photos: Evan-Amos, Wikimedia Commons. Weather: Open-Meteo.");
        if (focus == F_BLUETOOTH) {                  /* pairing lives in the system Settings app */
            if (release_ps) release_ps();            /* so PS can bring Home back */
            if (sceAppMgrLaunchAppByUri(0x20000, "settings_dlg:") < 0) ui_toast("Settings would not open", C_BAD);
            else ui_toast("Devices > Bluetooth Devices. PS comes back here.", C_ACCENT);
        }
        if (focus == F_RESTART && ui_confirm("Restart", "Restart the Vita now? Anything unsaved in a suspended game is lost."))
            scePowerRequestColdReset();
        if (focus == F_SLEEP && ui_confirm("Sleep", "Put the Vita to sleep? Wi-Fi goes off too, so agents cannot reach it until you wake it with the power button."))
            scePowerRequestSuspend();
        if (focus == F_POWEROFF && ui_confirm("Power off", "Turn the Vita off? Hold the power button to turn it back on."))
            scePowerRequestStandby();
    }

    char v[96];
    const int C1 = 12, C2 = 324, C3 = 636, R1 = 76, R2 = 272, CW = 312, CH = 186;

    card(C1, R1, CW, CH, "Display & sound");
    snprintf(v, sizeof(v), "%d%%", (brightness - BRIGHT_MIN) * 100 / (BRIGHT_MAX - BRIGHT_MIN));
    slider(C1 + 18, R1 + 54, "Brightness", (float)(brightness - BRIGHT_MIN) / (BRIGHT_MAX - BRIGHT_MIN), v, focus == F_BRIGHT);
    snprintf(v, sizeof(v), "%d / %d", volume, VOL_MAX);
    slider(C1 + 18, R1 + 90, "Volume", volume / (float)VOL_MAX, v, focus == F_VOLUME);
    if (!sfx_ok()) snprintf(v, sizeof(v), "no audio port");
    else if (sfx_level()) snprintf(v, sizeof(v), "%d / 10", sfx_level()); else snprintf(v, sizeof(v), "off");
    slider(C1 + 18, R1 + 126, "UI sounds", sfx_level() / 10.0f, v, focus == F_SFX);
    if (sfx_ambient_level()) snprintf(v, sizeof(v), "%d / 10", sfx_ambient_level()); else snprintf(v, sizeof(v), "off");
    slider(C1 + 18, R1 + 162, "Home music", sfx_ambient_level() / 10.0f, v, focus == F_AMBIENT);

    card(C2, R1, CW - 12, CH, "Power");
    {
        char w[96];
        const char *pl = weather_place();
        snprintf(w, sizeof(w), "X Weather: %s", pl && *pl ? pl : "off");
        if (in->tapped && in->tap_x >= C2 && in->tap_x < C2 + CW - 12 && in->tap_y >= R1 + CH - 34 && in->tap_y < R1 + CH) focus = F_WEATHER;
        if (focus == F_WEATHER) vita2d_draw_rectangle(C2 + 10, R1 + CH - 34, CW - 32, 28, C_SEL);
        draw_hints(C2 + 18, R1 + CH - 20, w, focus == F_WEATHER ? C_TEXT : C_DIM, C2 + CW - 20);
    }
    snprintf(v, sizeof(v), "%d%%%s", s.battery, s.charging ? "  charging" : s.plugged ? "  plugged in" : "");
    row(C2 + 18, R1 + 66, "Battery", v, s.battery < 15 && !s.plugged ? C_BAD : C_TEXT);
    draw_bar(C2 + 18, R1 + 78, CW - 48, 6, s.battery / 100.0f, s.battery < 15 ? C_BAD : C_OK);
    if (s.minutes > 0 && !s.plugged) snprintf(v, sizeof(v), "%dh %02dm", s.minutes / 60, s.minutes % 60);
    else snprintf(v, sizeof(v), "%s", s.plugged ? "on external power" : "estimating");
    row(C2 + 18, R1 + 116, "Time left", v, C_TEXT);
    snprintf(v, sizeof(v), "%d.%d C   %d.%02d V", s.temp / 100, (s.temp % 100) / 10, s.volt / 1000, (s.volt % 1000) / 10);
    row(C2 + 18, R1 + 146, "Health", v, C_TEXT);

    card(C3, R1, CW, CH, "Network");
    row(C3 + 18, R1 + 62, "Address", s.ip[0] ? s.ip : "not connected", s.ip[0] ? C_TEXT : C_BAD);
    row(C3 + 18, R1 + 88, "Wi-Fi", s.ssid[0] ? s.ssid : "-", C_TEXT);
    if (s.rssi >= 0) { snprintf(v, sizeof(v), "%d%%", s.rssi); row(C3 + 18, R1 + 114, "Signal", v, s.rssi < 30 ? C_BAD : C_TEXT); }
    if (s.remote) row(C3 + 18, R1 + 140, "Agents", "remote listening", C_OK);   /* the developer's agent bridge only */
    if (in->tapped && in->tap_x >= C3 && in->tap_x < C3 + CW && in->tap_y >= R1 + CH - 34 && in->tap_y < R1 + CH) focus = F_BLUETOOTH;
    if (focus == F_BLUETOOTH) vita2d_draw_rectangle(C3 + 10, R1 + CH - 34, CW - 20, 28, C_SEL);
    draw_hints(C3 + 18, R1 + CH - 20, "X Bluetooth devices", focus == F_BLUETOOTH ? C_TEXT : C_DIM, C3 + CW);

    card(C1, R2, CW, CH, "Storage");
    static const char *const devs[] = {"ux0:", "ur0:", "uma0:", "imc0:"};
    int y = R2 + 66;
    for (unsigned int i = 0; i < 4 && y < R2 + CH - 10; ++i) {
        if (!s.dev_ok[i]) continue;
        SceIoDevInfo d = s.dev[i];
        char fr[32], tot[32];
        human_size(d.free_size, fr, sizeof(fr));
        human_size(d.max_size, tot, sizeof(tot));
        snprintf(v, sizeof(v), "%s free of %s", fr, tot);
        text(bold, C1 + 18, y, C_TEXT, 16, devs[i]);
        text(font, C1 + 78, y, C_DIM, 15, v);
        draw_bar(C1 + 18, y + 9, CW - 36, 5, 1.0f - (float)d.free_size / d.max_size, C_ACCENT);
        y += 44;
    }

    if (focus == F_STORAGE) vita2d_draw_rectangle(C1 + 10, R2 + CH - 34, CW - 20, 28, C_SEL);
    draw_hints(C1 + 18, R2 + CH - 20, "X Manage storage and clean up", focus == F_STORAGE ? C_TEXT : C_DIM, C1 + CW);

    card(C2, R2, CW - 12, CH, "Performance (MHz)");
    if (in->tapped && in->tap_x >= C2 && in->tap_x < C2 + CW - 12 && in->tap_y >= R2 + CH - 34 && in->tap_y < R2 + CH) focus = F_ABOUT;
    if (focus == F_ABOUT) vita2d_draw_rectangle(C2 + 10, R2 + CH - 34, CW - 32, 28, C_SEL);
    draw_hints(C2 + 18, R2 + CH - 20, "X VitaOS: games, box art, about", focus == F_ABOUT ? C_TEXT : C_DIM, C2 + CW - 20);
    snprintf(v, sizeof(v), "%d", s.arm);
    row(C2 + 18, R2 + 66, "CPU", v, s.arm < 100 ? C_BAD : C_TEXT);   /* 1 MHz looks like a broken emulator */
    snprintf(v, sizeof(v), "%d", s.bus);
    row(C2 + 18, R2 + 96, "Bus", v, C_TEXT);
    snprintf(v, sizeof(v), "%d / %d", s.gpu, s.xbar);
    row(C2 + 18, R2 + 126, "GPU / xbar", v, C_TEXT);

    card(C3, R2, CW, CH, "System");
    static const char *const names[4] = {"System home", "Sleep", "Restart", "Power off"};
    for (int k = 0; k < 4; ++k) {
        int f = F_BUBBLES + k, bx = C3 + 22 + k * 72, by = R2 + 58;
        if (in->tapped && in->tap_x >= bx && in->tap_x < bx + 56 && in->tap_y >= by && in->tap_y < by + 56) focus = f;
        int on = focus == f;
        if (on) draw_focus_r(bx, by, 56, 56, 1, 16);
        draw_round_rect(bx, by, 56, 56, 16, on ? RGBA8(255, 255, 255, 36) : RGBA8(255, 255, 255, 16));
        /* the cut-outs are painted in the tile's own colour (panel + white wash) */
        sys_icon(k, bx + 28, by + 28, on ? C_TEXT : C_DIM);
        int tw = text_w(font, 12, names[k]);
        text(font, bx + 28 - tw / 2, by + 76, on ? C_TEXT : C_FAINT, 12, names[k]);
    }
    const char *what = focus == F_BUBBLES ? "PS opens the bubbles for 30 s" : focus == F_SLEEP ? "Wi-Fi off: agents lose the console"
                     : focus == F_RESTART ? "Back in about a minute" : focus == F_POWEROFF ? "Hold power to turn it back on" : "";
    text_fit(font, C3 + 18, R2 + CH - 14, C_FAINT, 13, what, CW - 36);

    /* A slim full-width row for Theme, in the gap below the two card rows. */
    {
        char t[160];
        snprintf(t, sizeof(t), "X Theme: %s accent, %s background",
                 ui_theme_accent_name(ui_theme_accent_index()), ui_theme_bg_name(ui_theme_bg()));
        int ty = R2 + CH + 6;
        /* A newer VitaOS takes the right third of the row as an Update button
         * (2026-09-29: "an update button in the settings"): START or a tap. */
        const char *nv = update_newer(), *um = "";
        float uf = 0;
        int st = update_stage(&um, &uf), upd = nv || st;
        int tx_end = upd ? C3 - 8 : C3 + CW;
        if (in->tapped && in->tap_x >= C1 && in->tap_x < tx_end && in->tap_y >= ty && in->tap_y < ty + 28) focus = F_THEME;
        if (focus == F_THEME) vita2d_draw_rectangle(C1 + 2, ty, tx_end - C1 - 4, 28, C_SEL);
        draw_hints(C1 + 18, ty + 14, t, focus == F_THEME ? C_TEXT : C_DIM, tx_end - 20);
        if (upd) {
            int bx = C3, bw = CW;
            if (!st && ((in->pressed & SCE_CTRL_START) ||
                        (in->tapped && in->tap_x >= bx && in->tap_x < bx + bw && in->tap_y >= ty && in->tap_y < ty + 28))) update_get();
            draw_round_rect(bx, ty, bw, 28, 14, st == 9 ? RGBA8(200, 60, 60, 200) : C_ACCENT);
            char label[112];
            if (!st) snprintf(label, sizeof(label), "START  Update to VitaOS %s", nv);
            else if (st == 1) snprintf(label, sizeof(label), "Downloading %d%%", (int)(uf * 100));
            else snprintf(label, sizeof(label), "%s", um);
            if (st == 1) vita2d_draw_rectangle(bx + 14, ty + 23, (bw - 28) * uf, 2, RGBA8(255, 255, 255, 200));
            text_fit(bold, bx + 14, ty + 19, RGBA8(255, 255, 255, 255), 14, label, bw - 28);
        }
    }
}
