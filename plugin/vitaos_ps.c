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
 * second. It reads the pad and writes that one file; nothing else. */
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

static volatile int running = 1;
static SceUID thread = -1;

static int vitaos_running(void) {
    SceUID id = -1;
    if (sceAppMgrGetIdByName(&id, "VITAOS001") >= 0 && id > 0) return 1;
    id = -1;
    return sceAppMgrGetIdByName(&id, "MVZA00010") >= 0 && id > 0;   /* the developer's build */
}

static int watch(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int was = 0;
    SceUInt64 last_ts = 0;
    SceCtrlData pads[16];
    while (running) {
        sceKernelDelayThread(50 * 1000);
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
