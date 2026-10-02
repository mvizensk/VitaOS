/* Settings > Battery health (2026-10-01: "the average battery lifespan it's
 * getting and whether that's normal for the model and what is possible if
 * you upgrade").
 *
 * What the battery reports about itself: health (SOH, %), charge cycles, and
 * the charge it holds when full (mAh). What VitaOS measures: how fast the
 * charge falls while it is on battery, sampled once a minute while VitaOS is
 * open and again whenever it comes back (games included), never across a
 * charge or a gap of more than three hours (that is mostly sleep). Kept in
 * ux0:data/arcadehub/battery.txt, weighted toward recent weeks.
 *
 * Sony's figures: both models ship with a 2210 mAh battery; Sony rates the
 * PCH-1000 at about 3 to 5 hours of games and the PCH-2000 at about 4 to 6.
 * Upgrades: we do not quote sellers' capacities (listings claim 3800-4600 mAh
 * in the stock size, which the cells cannot hold); instead the page shows
 * what a new battery would give this Vita, from its own measured use. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include "battery.h"

#define LOG "ux0:data/arcadehub/battery.txt"
#define DESIGN_MAH 2210
#define MAX_GAP (3 * 3600)

static int active, loaded;
static double used_secs, used_pct;      /* time on battery, and the charge it used */
static int last_pct = -1;
static time_t last_t;

static void load(void) {
    loaded = 1;
    SceUID fd = sceIoOpen(LOG, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    char b[64] = {0};
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    sscanf(b, "%lf %lf", &used_secs, &used_pct);
    if (used_secs < 0 || used_pct < 0) used_secs = used_pct = 0;
}

static void save(void) {
    char b[64];
    int n = snprintf(b, sizeof(b), "%.0f %.2f\n", used_secs, used_pct);
    ui_save(LOG, b, n, 0);
}

void battery_tick(void) {
    static unsigned int frames;
    if (!loaded) load();
    if (frames++ % 3600 && last_pct >= 0) return;          /* once a minute, and on the first frame back */
    time_t now = time(NULL);
    int pct = scePowerGetBatteryLifePercent();
    int on_battery = !scePowerIsPowerOnline() && !scePowerIsBatteryCharging();
    if (pct < 0) return;
    if (!on_battery) { last_pct = -1; return; }
    if (last_pct >= 0) {
        double gap = difftime(now, last_t);
        if (gap > 0 && gap <= MAX_GAP && pct <= last_pct) {
            used_secs += gap;
            used_pct += last_pct - pct;
            if (used_secs > 40.0 * 3600) { used_secs /= 2; used_pct /= 2; }   /* recent use counts most */
            static int unsaved;
            if (++unsaved >= 5 || last_pct != pct) { save(); unsaved = 0; }
        }
    }
    last_pct = pct;
    last_t = now;
}

void battery_open(void) { active = 1; if (!loaded) load(); }
void battery_close(void) { active = 0; }
int battery_active(void) { return active; }
const char *battery_hint(void) { return "O back    L R tabs"; }

/* The PCH-2000 has 1 GB of memory built in (imc0:); the PCH-1000 does not. */
static int is_slim(void) { SceIoStat st; return sceIoGetstat("imc0:", &st) >= 0; }

void battery_update(const Input *in) {
    if (in->pressed & SCE_CTRL_CIRCLE) { active = 0; return; }
    int slim = is_slim();
    int soh = scePowerGetBatterySOH(), cycles = scePowerGetBatteryCycleCount();
    int full = scePowerGetBatteryFullCapacity(), remain = scePowerGetBatteryRemainCapacity();
    int temp = scePowerGetBatteryTemp(), volt = scePowerGetBatteryVolt();
    char v[160];

    text(bold, 40, 110, C_TEXT, 24, "Battery health");
    snprintf(v, sizeof(v), "PS Vita %s (%s)", slim ? "slim" : "original", slim ? "PCH-2000" : "PCH-1000");
    text(font, 40, 138, C_DIM, 16, v);

    /* left: what the battery says */
    int x = 40, y = 190;
    if (soh > 0) {
        snprintf(v, sizeof(v), "%d%%", soh);
        text(bold, x, y + 20, soh >= 80 ? C_OK : soh >= 60 ? C_TEXT : C_BAD, 44, v);
        text(font, x + 120, y + 4, C_TEXT, 17, soh >= 80 ? "Good" : soh >= 60 ? "Worn" : "Replace soon");
        text(font, x + 120, y + 26, C_DIM, 14, "of its capacity when new");
        draw_bar(x, y + 40, 380, 6, soh / 100.0f, soh >= 80 ? C_OK : soh >= 60 ? C_ACCENT : C_BAD);
    } else text(font, x, y + 20, C_DIM, 17, "The battery does not report its health.");
    y += 86;
    if (full > 0) { snprintf(v, sizeof(v), "%d mAh  (%d mAh new)", full, DESIGN_MAH); text(font, x, y, C_DIM, 15, "Holds when full"); text(bold, x + 170, y, C_TEXT, 15, v); y += 28; }
    if (remain > 0) { snprintf(v, sizeof(v), "%d mAh", remain); text(font, x, y, C_DIM, 15, "Holds now"); text(bold, x + 170, y, C_TEXT, 15, v); y += 28; }
    if (cycles >= 0) { snprintf(v, sizeof(v), "%d", cycles); text(font, x, y, C_DIM, 15, "Charge cycles"); text(bold, x + 170, y, C_TEXT, 15, v); y += 28; }
    snprintf(v, sizeof(v), "%d.%d C   %d.%02d V", temp / 100, (temp % 100) / 10, volt / 1000, (volt % 1000) / 10);
    text(font, x, y, C_DIM, 15, "Now"); text(bold, x + 170, y, C_TEXT, 15, v);

    /* right: what VitaOS measured, against Sony's rating */
    x = 500; y = 190;
    int lo = slim ? 4 : 3, hi = slim ? 6 : 5;
    text(bold, x, y + 4, C_TEXT, 18, "Time per charge");
    if (used_secs >= 2 * 3600 && used_pct >= 10) {
        double hours = used_secs / 3600.0 * 100.0 / used_pct;
        snprintf(v, sizeof(v), "%.1f h", hours);
        text(bold, x, y + 50, C_TEXT, 36, v);
        snprintf(v, sizeof(v), "measured over %.0f h on battery", used_secs / 3600.0);
        text(font, x, y + 74, C_DIM, 14, v);
        const char *verdict = hours >= lo ? "Normal for this model" : hours >= lo * 0.75 ? "A little short for this model" : "Short for this model";
        text(font, x, y + 104, hours >= lo ? C_OK : C_TEXT, 16, verdict);
        snprintf(v, sizeof(v), "Sony rates it at about %d to %d h of games.", lo, hi);
        text(font, x, y + 126, C_DIM, 14, v);
        if (soh > 0 && soh < 95) {
            snprintf(v, sizeof(v), "A new battery: about %.1f h here.", hours * 100.0 / soh);
            text(font, x, y + 162, C_TEXT, 15, v);
        }
    } else {
        text(font, x, y + 40, C_DIM, 15, "Still measuring. Use the Vita on battery");
        text(font, x, y + 60, C_DIM, 15, "for a couple of hours, games included.");
        snprintf(v, sizeof(v), "Sony rates it at about %d to %d h of games.", lo, hi);
        text(font, x, y + 96, C_DIM, 14, v);
    }
    text(font, x, y + 196, C_FAINT, 13, slim ? "Replacement part: SP86R (PCH-2000)." : "Replacement part: SP65M (PCH-1000).");
    text(font, x, y + 214, C_FAINT, 13, "Listings of 3800 mAh or more in the stock size");
    text(font, x, y + 232, C_FAINT, 13, "rarely hold that; a good one matches a new battery.");
}
