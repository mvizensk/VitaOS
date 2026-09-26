/* Is there a newer VitaOS? At most once a day, GitHub's latest release is
 * compared with this build. A newer one is announced with a toast and offered
 * in Settings > VitaOS, which saves the VPK to ux0:downloads: a running app
 * cannot safely install over itself, so VitaShell does the install. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
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

static int check_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
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
                    snprintf(msg, sizeof(msg), "VitaOS %s is out: Settings > VitaOS", latest);
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

static int get_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    char dest[96], msg[176];
    sceIoMkdir("ux0:downloads", 0777);
    snprintf(dest, sizeof(dest), "ux0:downloads/VitaOS-%s.vpk", latest);
    if (!vpk_url[0]) snprintf(vpk_url, sizeof(vpk_url), "https://github.com/mvizensk/VitaOS/releases/download/v%s/VitaOS.vpk", latest);
    if (store_fetch(vpk_url, dest) >= 0)
        snprintf(msg, sizeof(msg), "Saved %s: install it with VitaShell", dest);
    else
        snprintf(msg, sizeof(msg), "The download did not finish. Check Wi-Fi and try again.");
    ui_toast(msg, C_OK);
    busy = 0;
    return sceKernelExitDeleteThread(0);
}

void update_get(void) {
    if (!newer || busy) return;
    busy = 1;
    char msg[64];
    snprintf(msg, sizeof(msg), "Downloading VitaOS %s", latest);
    ui_toast(msg, C_ACCENT);
    SceUID t = sceKernelCreateThread("update-get", get_thread, 0x10000100, 0x8000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL); else busy = 0;
}
