/* VitaOS Updater (VTOSUPDTR): installs a VitaOS update while VitaOS is closed.
 *
 * An app cannot safely install over itself while it runs, so VitaOS downloads
 * the update, unpacks it to ux0:data/arcadehub/update/pkg (head.bin written,
 * ready for the promoter), writes the title ID to reopen in .../update/go, and
 * hands over to this app. This app installs the folder with the system's
 * promoter (as VitaShell and VitaOS's Store do), deletes the handoff, and
 * reopens VitaOS. With no handoff it just says so and reopens VitaOS. */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/appmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/promoterutil.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>

#define DIR "ux0:data/arcadehub/update"
#define PKG DIR "/pkg"
#define GO DIR "/go"

static vita2d_pgf *font;
static char line1[96] = "Updating VitaOS", line2[160] = "Do not turn off your PS Vita.";

static void frame(void) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(font, 60, 250, RGBA8(255, 255, 255, 255), 1.6f, line1);
    vita2d_pgf_draw_text(font, 60, 300, RGBA8(170, 180, 210, 255), 1.0f, line2);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void say(const char *a, const char *b) {
    snprintf(line1, sizeof(line1), "%s", a);
    snprintf(line2, sizeof(line2), "%s", b);
    for (int i = 0; i < 3; ++i) frame();
}

static int exists(const char *p) { SceIoStat st; return sceIoGetstat(p, &st) >= 0; }

static void remove_tree(const char *dir) {
    SceUID d = sceIoDopen(dir);
    if (d >= 0) {
        SceIoDirent e;
        char p[512];
        for (;;) {
            memset(&e, 0, sizeof(e));
            if (sceIoDread(d, &e) <= 0) break;
            snprintf(p, sizeof(p), "%s/%s", dir, e.d_name);
            if (SCE_S_ISDIR(e.d_stat.st_mode)) remove_tree(p); else sceIoRemove(p);
        }
        sceIoDclose(d);
    }
    sceIoRmdir(dir);
}

static int promoter_up(void) {
    unsigned int paf_args[] = {0x180000, -1, -1, 1, -1, -1};
    int entry = -1;
    SceSysmoduleOpt opt = {sizeof(opt), &entry, {-1, -1}};
    int rc = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(paf_args), paf_args, &opt);
    if (rc < 0 && rc != (int)0x805A1002) return rc;
    if ((rc = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL)) < 0) return rc;
    return scePromoterUtilityInit();
}

/* VitaShell's launchAppByUriExit: ask twice and leave at once, so the shell
 * never stops to ask "the following application will close" (it did, 2 s
 * of waiting here was long enough for the prompt, 2026-09-29). */
static void reopen(const char *tid) {
    char uri[48];
    snprintf(uri, sizeof(uri), "psgm:play?titleid=%s", tid);
    sceKernelDelayThread(10000);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelDelayThread(10000);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelExitProcess(0);
}

int main(void) {
    vita2d_init();
    vita2d_set_clear_color(RGBA8(10, 12, 30, 255));
    font = vita2d_load_default_pgf();
    say("Updating VitaOS", "Do not turn off your PS Vita.");

    char tid[16] = "VITAOS001";
    SceUID fd = sceIoOpen(GO, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char b[16] = {0};
        int n = sceIoRead(fd, b, 9);
        sceIoClose(fd);
        if (n == 9) memcpy(tid, b, 10);
    }
    if (!exists(GO) || !exists(PKG "/eboot.bin")) {
        say("Nothing to install", "Open VitaOS and choose Update in Settings.");
        sceKernelDelayThread(3 * 1000 * 1000);
        reopen(tid);
    }
    int rc = promoter_up();
    if (rc >= 0) rc = scePromoterUtilityPromotePkgWithRif(PKG, 1);
    for (int i = 0; rc >= 0 && i < 1200; ++i) {             /* up to 2 minutes */
        int state = 0;
        if (scePromoterUtilityGetState(&state) < 0 || !state) break;
        frame();
        sceKernelDelayThread(100 * 1000);
    }
    int result = rc;
    if (rc >= 0) scePromoterUtilityGetResult(&result);
    sceIoRemove(GO);
    remove_tree(PKG);
    if (result < 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Error 0x%08X. Your current VitaOS still works.", result);
        say("The update did not install", msg);
        sceKernelDelayThread(5 * 1000 * 1000);
    } else {
        say("VitaOS is up to date", "Opening VitaOS...");
        sceKernelDelayThread(1 * 1000 * 1000);
    }
    reopen(tid);
    return 0;
}
