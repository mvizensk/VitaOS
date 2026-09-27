/* vitaos_ps: PS returns to VitaOS, the way HOME does on a Switch.
 *
 * VitaOS holds the system's PS lock so the shell leaves PS alone, but a real
 * press never reaches an app's own controller reads. SceShell still sees it,
 * and so does this plugin, loaded into SceShell by taiHEN:
 *
 *     *main
 *     ur0:tai/vitaos_ps.suprx
 *
 * On a PS press while VitaOS is running it leaves VitaOS a note
 * (ux0:data/arcadehub/ps.tap), which VitaOS picks up within a third of a
 * second.
 *
 * 1.1 (2026-09-26): and when a game ends (closed from LiveArea, crashed, or
 * quit by itself), VitaOS comes back about 1.5 s later instead of leaving the
 * player on LiveArea. It only ever launches VitaOS, and only when no app is
 * running at all: a game suspended behind LiveArea (PS pressed) has not ended,
 * and VitaOS suspended there (its "system home" path) means the player chose
 * LiveArea. ux0:data/arcadehub/return.off turns it off. On a Vita with the
 * agent bridge, the bridge does this job and the plugin stands aside. */
#include <taihen.h>
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

static volatile int running = 1;
static SceUID thread = -1;

static int app_running(const char *tid) {
    SceUID id = -1;
    return sceAppMgrGetIdByName(&id, tid) >= 0 && id > 0;
}

static int vitaos_running(void) {
    return app_running("VITAOS001") || app_running("MVZA00010");   /* the developer's build */
}

static int is_home(const char *t) {
    return !sceClibStrncmp(t, "VITAOS001", 9) || !sceClibStrncmp(t, "MVZA00010", 9);
}

static int file_exists(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

/* ---------- return to VitaOS when a game ends ---------- */

#define TICK_MS 250
static char armed[16];                /* title ID of the game whose end brings VitaOS back */
static SceUID armed_pid;
static int gone_ticks;

/* The non-system app the shell lists as running: 1 with its title ID and pid.
 * *home is set if VitaOS is among them. */
static int running_game(char *tid, SceUID *pid, int *home) {
    SceInt32 ids[8];
    int n = sceAppMgrGetRunningAppIdListForShell(ids, 8), found = 0;
    *home = 0;
    for (int i = 0; i < n && i < 8; ++i) {
        SceUID p = sceAppMgrGetProcessIdByAppIdForShell(ids[i]);
        char name[32];
        sceClibMemset(name, 0, sizeof(name));
        if (p <= 0 || sceAppMgrGetNameById(p, name) < 0 || !name[0]) continue;
        if (!sceClibStrncmp(name, "NPXS", 4)) continue;               /* Settings, Music: not games */
        if (is_home(name)) { *home = 1; continue; }
        sceClibStrncpy(tid, name, 15);
        tid[15] = 0;
        *pid = p;
        found = 1;
    }
    return found;
}

/* A game suspended behind LiveArea can be missing from the running list (seen
 * 2026-09-24), so an armed game has ended only when neither its title ID nor
 * its pid resolves. A false "alive" only keeps VitaOS away. */
static int still_alive(const char *tid, SceUID pid) {
    char name[32];
    if (app_running(tid)) return 1;
    return pid > 0 && sceAppMgrGetNameById(pid, name) >= 0;
}

/* The agent bridge (developer Vitas) returns to Home itself. */
static int bridge_loaded(void) {
    tai_module_info_t info;
    sceClibMemset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    return taiGetModuleInfo("VitaAgentBridge", &info) >= 0;
}

static const char *home_tid(void) {
    if (file_exists("ux0:app/VITAOS001/eboot.bin")) return "VITAOS001";
    if (file_exists("ux0:app/MVZA00010/eboot.bin")) return "MVZA00010";
    return 0;
}

/* The first URI launch after a game exits often only opens the target's
 * LiveArea page; a second one starts it (learned on the agent bridge,
 * 2026-09-19). Stops as soon as VitaOS, or anything else, is running. */
static void launch_home(const char *home) {
    char uri[48];
    sceClibSnprintf(uri, sizeof(uri), "psgm:play?titleid=%s", home);
    for (int attempt = 0; attempt < 2 && running; ++attempt) {
        if (!app_running(home)) sceAppMgrLaunchAppByUri(0xFFFFF, uri);
        for (int i = 0; i < (attempt ? 20 : 6) && running; ++i) {
            sceKernelDelayThread(TICK_MS * 1000);
            char t[16];
            SceUID p;
            int h;
            if (app_running(home) || running_game(t, &p, &h)) return;
        }
    }
}

/* 1.2: back to wherever the game was started from. VitaOS marks its own
 * launches (confirm.req, written just before it launches a game); a game
 * started from its bubble ends on LiveArea as it always did. With no bridge
 * to consume the flag, the plugin takes it itself. */
#define LAUNCH_FLAG "ux0:data/arcadehub/confirm.req"
static SceUInt64 home_launch_at;
static int armed_from_home;

static void return_tick(void) {
    char tid[16];
    SceUID pid = -1;
    int home;
    if (!bridge_loaded() && file_exists(LAUNCH_FLAG)) {
        home_launch_at = sceKernelGetSystemTimeWide();
        sceIoRemove(LAUNCH_FLAG);
    }
    if (running_game(tid, &pid, &home)) {
        if (sceClibStrncmp(armed, tid, 16)) {
            sceClibMemcpy(armed, tid, sizeof(armed));
            armed_from_home = home_launch_at && sceKernelGetSystemTimeWide() - home_launch_at < 90ull * 1000 * 1000;
        }
        armed_pid = pid;
        gone_ticks = 0;
        return;
    }
    if (home) { armed[0] = 0; gone_ticks = 0; return; }     /* VitaOS is up, or the player parked it */
    if (!armed[0]) return;
    if (still_alive(armed, armed_pid)) { gone_ticks = 0; return; }
    /* 1.5 s with nothing running; RetroFlow's PSP launcher hands over to
     * Adrenaline, so give that one longer. */
    int settle = sceClibStrncmp(armed, "RETROLNCR", 9) ? 6 : 24;
    if (++gone_ticks < settle) return;
    armed[0] = 0;
    gone_ticks = 0;
    const char *h = home_tid();
    if (!h || !armed_from_home || file_exists("ux0:data/arcadehub/return.off") || bridge_loaded()) return;
    launch_home(h);
}

static int watch(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int was = 0, loops = 0;
    SceUInt64 last_ts = 0;
    SceCtrlData pads[16];
    while (running) {
        sceKernelDelayThread(50 * 1000);
        if (++loops % (TICK_MS / 50) == 0) return_tick();
        int n = sceCtrlPeekBufferPositive2(0, pads, 16);
        if (n <= 0) continue;
        unsigned int down = 0;
        for (int i = 0; i < n; ++i) {
            if (pads[i].timeStamp <= last_ts) continue;
            last_ts = pads[i].timeStamp;
            down |= pads[i].buttons & 0x10000;
        }
        if (down && !was && vitaos_running()) {
            SceUID fd = sceIoOpen("ux0:data/arcadehub/ps.tap", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            if (fd >= 0) { sceIoWrite(fd, "1", 1); sceIoClose(fd); }
        }
        was = pads[n - 1].buttons & 0x10000;
    }
    return sceKernelExitDeleteThread(0);
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
    (void)args; (void)argp;
    thread = sceKernelCreateThread("vitaos_ps", watch, 0x10000100, 0x2000, 0, 0, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    (void)args; (void)argp;
    running = 0;
    if (thread >= 0) { SceUInt timeout = 500 * 1000; sceKernelWaitThreadEnd(thread, NULL, &timeout); }
    return SCE_KERNEL_STOP_SUCCESS;
}
