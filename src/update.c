/* Is there a newer VitaOS? At most once a day, GitHub's latest release is
 * compared with this build. A newer one is announced with a toast and a banner
 * at the top of Settings; one press downloads it and the VitaOS Updater app
 * installs it (see update_get below). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include "update.h"
#include "version.h"
#include "store.h"
#include "ui.h"

#define API "https://api.github.com/repos/mvizensk/VitaOS/releases/latest"
#define JSON "ux0:data/arcadehub/update.json"
#define SEEN "ux0:data/arcadehub/user/update-check.txt"   /* "<unix time> <latest tag>" */

static char latest[16], vpk_url[300];
static volatile int newer, busy;

static int parse_ver(const char *s, int v[3]) {
    if (*s == 'v') ++s;
    v[0] = v[1] = v[2] = 0;
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 2;
}

static int is_newer(const char *tag) {
    int a[3], b[3];
    if (!parse_ver(tag, a) || !parse_ver(VITAOS_VERSION, b)) return 0;
    for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

static unsigned long long now_s(void) {
    SceRtcTick t;
    sceRtcGetCurrentTickUtc(&t);
    return t.tick / 1000000ull;
}

/* "key":"value" from the release JSON; for the asset, the first URL ending in .vpk. */
static void json_str(const char *j, const char *key, const char *suffix, char *out, int max) {
    char k[48];
    snprintf(k, sizeof(k), "\"%s\"", key);
    out[0] = 0;
    for (const char *p = strstr(j, k); p; p = strstr(p + 1, k)) {
        const char *q = strchr(p + strlen(k), '"');
        const char *e = q ? strchr(q + 1, '"') : NULL;
        if (!e || e - q - 1 >= max) continue;
        if (suffix && (e - q - 1 < (int)strlen(suffix) || strncmp(e - strlen(suffix), suffix, strlen(suffix)))) continue;
        memcpy(out, q + 1, e - q - 1);
        out[e - q - 1] = 0;
        return;
    }
}

#define TEST "ux0:data/arcadehub/user/update-test"   /* "<version> <vpk url>": test the updater end to end */

static int check_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    FILE *tf = fopen(TEST, "r");
    if (tf) {
        char v[16] = "";
        if (fscanf(tf, "%15s %299s", v, vpk_url) == 2 && is_newer(v)) { snprintf(latest, sizeof(latest), "%s", v); newer = 1; }
        fclose(tf);
        return sceKernelExitDeleteThread(0);
    }
    unsigned long long last = 0;
    char seen[16] = "";
    FILE *f = fopen(SEEN, "r");
    if (f) { if (fscanf(f, "%llu %15s", &last, seen) < 1) last = 0; fclose(f); }
    unsigned long long now = now_s();
    if (last && now - last < 24 * 3600) {             /* checked today: reuse the answer */
        if (seen[0] && is_newer(seen)) { snprintf(latest, sizeof(latest), "%s", seen[0] == 'v' ? seen + 1 : seen); newer = 1; }
        if (!newer) return sceKernelExitDeleteThread(0);
    }
    if (store_fetch(API, JSON) >= 0) {
        static char j[16384];
        f = fopen(JSON, "r");
        int n = f ? (int)fread(j, 1, sizeof(j) - 1, f) : 0;
        if (f) fclose(f);
        j[n > 0 ? n : 0] = 0;
        char tag[16];
        json_str(j, "tag_name", NULL, tag, sizeof(tag));
        json_str(j, "browser_download_url", ".vpk", vpk_url, sizeof(vpk_url));
        if (tag[0]) {
            f = fopen(SEEN, "w");
            if (f) { fprintf(f, "%llu %s\n", now, tag); fclose(f); }
            if (is_newer(tag) && vpk_url[0]) {
                snprintf(latest, sizeof(latest), "%s", tag[0] == 'v' ? tag + 1 : tag);
                if (!newer) {
                    char msg[80];
                    snprintf(msg, sizeof(msg), "VitaOS %s is out: update it in Settings", latest);
                    ui_toast(msg, C_ACCENT);
                }
                newer = 1;
            }
        }
    }
    return sceKernelExitDeleteThread(0);
}

void update_init(void) {
    SceUID t = sceKernelCreateThread("update", check_thread, 0x10000100, 0x8000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

const char *update_newer(void) { return newer ? latest : NULL; }

/* ---------- one-button update (asked for 2026-09-29: "an update button in
 * the settings so they don't have to do any manual installs") ----------
 * Download the release VPK, unpack it ready for the promoter, make sure the
 * small VitaOS Updater app (VTOSUPDTR, shipped in app0:assets/updater.vpk) is
 * installed, and hand over to it: VitaOS closes, the updater installs the new
 * VitaOS and reopens it. A build whose title ID differs from the release's
 * (the developer's) only saves the VPK, as before. */
#define UPD "ux0:data/arcadehub/update"
#define UPDATER_TID "VTOSUPDTR"
#define UPDATER_VER UPD "/updater-ver"
#define UPDATER_BUILD "1.01"                          /* bump with home/updater's VITA_VERSION */

static volatile int stage;                            /* 0 idle, 1 downloading, 2 preparing, 3 ready, 9 failed */
static char stage_msg[96];

static int self_tid(char *out) {
    memset(out, 0, 10);
    return sceAppMgrAppParamGetString(sceKernelGetProcessId(), 12, out, 10);
}

static int updater_current(void) {
    SceIoStat st;
    if (sceIoGetstat("ux0:app/" UPDATER_TID "/eboot.bin", &st) < 0) return 0;
    char v[16] = {0};
    SceUID fd = sceIoOpen(UPDATER_VER, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    sceIoRead(fd, v, sizeof(v) - 1);
    sceIoClose(fd);
    return !strcmp(v, UPDATER_BUILD);
}

static int install_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    char vpk[64] = UPD "/VitaOS.vpk", tid[10], mine[10];
    sceIoMkdir(UPD, 0777);
    if (!vpk_url[0]) snprintf(vpk_url, sizeof(vpk_url), "https://github.com/mvizensk/VitaOS/releases/download/v%s/VitaOS.vpk", latest);
    stage = 1;
    snprintf(stage_msg, sizeof(stage_msg), "Downloading VitaOS %s", latest);
    if (store_fetch(vpk_url, vpk) < 0) {
        snprintf(stage_msg, sizeof(stage_msg), "The download did not finish. Check Wi-Fi and try again.");
        stage = 9; busy = 0;
        return sceKernelExitDeleteThread(0);
    }
    stage = 2;
    snprintf(stage_msg, sizeof(stage_msg), "Getting it ready");
    int rc = store_prepare_pkg(vpk, UPD "/pkg", tid);
    if (rc >= 0 && (self_tid(mine) < 0 || strcmp(tid, mine))) {
        /* Not this build's title ID (a developer build): keep the old path. */
        char dest[96];
        sceIoMkdir("ux0:downloads", 0777);
        snprintf(dest, sizeof(dest), "ux0:downloads/VitaOS-%s.vpk", latest);
        sceIoRemove(dest);
        sceIoRename(vpk, dest);
        snprintf(stage_msg, sizeof(stage_msg), "Saved %s: install it with VitaShell", dest);
        stage = 9; busy = 0;
        return sceKernelExitDeleteThread(0);
    }
    sceIoRemove(vpk);
    if (rc >= 0 && !updater_current()) {
        char utid[10];
        rc = store_prepare_pkg("app0:assets/updater.vpk", UPD "/updater-pkg", utid);
        if (rc >= 0) rc = store_install_dir(UPD "/updater-pkg");
        if (rc >= 0) {
            SceUID fd = sceIoOpen(UPDATER_VER, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            if (fd >= 0) { sceIoWrite(fd, UPDATER_BUILD, strlen(UPDATER_BUILD)); sceIoClose(fd); }
        }
    }
    if (rc < 0) {
        snprintf(stage_msg, sizeof(stage_msg), "Could not get the update ready (0x%08X)", rc);
        stage = 9; busy = 0;
        return sceKernelExitDeleteThread(0);
    }
    SceUID fd = sceIoOpen(UPD "/go", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, mine, 9); sceIoClose(fd); }
    snprintf(stage_msg, sizeof(stage_msg), "Restarting VitaOS to update");
    stage = 3;
    return sceKernelExitDeleteThread(0);
}

void update_get(void) {
    if (!newer || busy) return;
    busy = 1;
    stage = 1;
    SceUID t = sceKernelCreateThread("update-get", install_thread, 0x10000100, 0x8000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) { busy = 0; stage = 0; }
}

/* Each frame from the main loop: hand over to the updater once it is ready. */
void update_tick(void) {
    if (stage != 3) return;
    stage = 4;
    /* VitaShell's launchAppByUriExit: ask twice and leave at once, or the shell
     * stops to ask "the following application will close: VitaOS". */
    sceKernelDelayThread(10000);
    sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" UPDATER_TID);
    sceKernelDelayThread(10000);
    sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" UPDATER_TID);
    sceKernelExitProcess(0);
}

int update_stage(const char **msg, float *frac) {
    if (msg) *msg = stage_msg;
    if (frac) *frac = stage == 1 ? store_job_frac() : stage >= 2 ? 1.0f : 0.0f;
    return stage;
}
