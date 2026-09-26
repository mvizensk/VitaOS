/* Preview video: hardware H.264 decode via sceAvPlayer, drawn as a vita2d texture.
 * Recipe verified by VitaMediaDeck (spyro-98) and Vita-Media-Player (SonicMastr):
 *  - load the patched reAvPlayer.suprx (MIT, bundled in app0:modules/) before
 *    sceAvPlayerInit; the stock module crashes deterministically;
 *  - frame buffers must be GPU-mapped CDRAM, 256 KiB aligned, zeroed;
 *  - the handle is an opaque 0x81xxxxxx value: only 0 / 0x806Axxxx are errors;
 *  - wait for rendering before freeing frame memory.
 * Attract mode plays the clip's sound; browsing stays silent, so video_play
 * takes a flag and only then starts the audio thread. */
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/dmac.h>
#include <vita2d.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include "video.h"
#include "ui.h"

#define CDRAM_ALIGN 0x40000u
#define ALIGN_UP(x, a) (((x) + ((a) - 1u)) & ~((a) - 1u))

static int module_ready = -1;
static SceUID audio_thread = -1;
static int audio_port = -1;
static volatile int audio_run;
static SceAvPlayerHandle player;
static int active;
static vita2d_texture frame_tex;
static int have_frame;
/* Our own copy of the last decoded frame. Drawing the player's buffer
 * directly showed torn, half-decoded bands (worst on 30 fps N64 clips): a
 * GetVideoData call that finds no new frame hands the previous buffer back to
 * the decoder, which starts writing the next frame into it while it is still
 * on screen. So each new frame is DMA-copied here and only this is drawn. */
static void *copy_buf;
static unsigned int copy_size;
static int owner, paused, finished, looping;
static unsigned int pending_seek, duration_ms;

static void *av_alloc(void *arg, uint32_t alignment, uint32_t size) {
    (void)arg;
    if (alignment < 8) alignment = 8;
    void *p = memalign(alignment, size);
    if (p) memset(p, 0, size);
    return p;
}

static void av_free(void *arg, void *ptr) { (void)arg; free(ptr); }

static void *av_alloc_tex(void *arg, uint32_t alignment, uint32_t size) {
    (void)arg;
    if (alignment < CDRAM_ALIGN) alignment = CDRAM_ALIGN;
    size = ALIGN_UP(size, alignment);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = alignment;
    SceUID block = sceKernelAllocMemBlock("hub_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    if (block < 0) return NULL;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(block, &base) < 0 || !base) { sceKernelFreeMemBlock(block); return NULL; }
    if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
        sceKernelFreeMemBlock(block);
        return NULL;
    }
    memset(base, 0, size);
    return base;
}

static volatile int closing;                 /* the closer thread is tearing a player down */

/* GPU memory is only ever unmapped on the main thread, between frames, with
 * the GPU idle: unmapping it from the closer thread while the GPU was drawing
 * crashed the GPU (2026-09-24). The closer queues; video_collect() frees. */
#define DEFER 32
static void *deferred[DEFER];
static volatile int ndeferred;
static SceUID defer_lock = -1;

static SceUID main_tid = -1;                 /* the only thread that may wait on the GPU */

static void av_free_tex(void *arg, void *ptr) {
    (void)arg;
    if (!ptr) return;
    if (closing || sceKernelGetThreadId() != main_tid) {
        if (defer_lock < 0) defer_lock = sceKernelCreateSema("av_defer", 0, 1, 1, NULL);
        sceKernelWaitSema(defer_lock, 1, NULL);
        if (ndeferred < DEFER) { deferred[ndeferred++] = ptr; ptr = NULL; }
        sceKernelSignalSema(defer_lock, 1);
        if (!ptr) return;                            /* queued (if the queue is full, leak rather than crash) */
        return;
    }
    SceUID block = sceKernelFindMemBlockByAddr(ptr, 0);
    if (block < 0) return;
    vita2d_wait_rendering_done();
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(block);
}

void video_collect(void) {
    if (!ndeferred) return;
    sceKernelWaitSema(defer_lock, 1, NULL);
    vita2d_wait_rendering_done();
    for (int i = 0; i < ndeferred; ++i) {
        SceUID block = sceKernelFindMemBlockByAddr(deferred[i], 0);
        if (block < 0) continue;
        sceGxmUnmapMemory(deferred[i]);
        sceKernelFreeMemBlock(block);
    }
    ndeferred = 0;
    sceKernelSignalSema(defer_lock, 1);
}

static int is_error(SceAvPlayerHandle h) {
    unsigned int raw = (unsigned int)h;
    return raw == 0 || (raw & 0xFFFF0000u) == 0x806A0000u;
}

static void alog(const char *fmt, int a, int b, int c) {
    char line[160];
    int n = sceClibSnprintf(line, sizeof(line), fmt, a, b, c);
    SceUID fd = sceIoOpen("ux0:data/arcadehub/audio.log", SCE_O_CREAT | SCE_O_APPEND | SCE_O_WRONLY, 0666);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}

/* Pulls decoded audio frames and pushes them to the BGM port. sceAudioOutOutput
 * blocks until the buffer drains, which paces this thread for us. */
static int audio_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (audio_run) {
        SceAvPlayerFrameInfo f;
        memset(&f, 0, sizeof(f));
        if (active && sceAvPlayerIsActive(player) && sceAvPlayerGetAudioData(player, &f) && f.pData) {
            if (audio_port < 0) {
                audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024,
                                                 f.details.audio.sampleRate,
                                                 f.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO
                                                                                   : SCE_AUDIO_OUT_MODE_STEREO);
                if (audio_port < 0) alog("audio port failed %08X %d %d\n", audio_port, 0, 0);
                else {
                    /* Full scale here; the hardware volume buttons set the level. */
                    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
                    sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
                }
            }
            if (audio_port >= 0) sceAudioOutOutput(audio_port, f.pData);
        } else sceKernelDelayThread(5000);
    }
    if (audio_port >= 0) { sceAudioOutOutput(audio_port, NULL); sceAudioOutReleasePort(audio_port); audio_port = -1; }
    return sceKernelExitDeleteThread(0);
}

static void audio_start(void) {
    if (audio_thread >= 0) return;
    audio_run = 1;
    /* Default priority and a real stack: at 0x10000060 with 0x4000 the thread
     * never ran and attract mode stayed silent (2026-09-19). */
    audio_thread = sceKernelCreateThread("hub_audio", audio_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (audio_thread < 0) { audio_run = 0; return; }
    sceKernelStartThread(audio_thread, 0, NULL);
}

static void audio_stop(void) {
    if (audio_thread < 0) return;
    audio_run = 0;
    SceUInt timeout = 2 * 1000 * 1000;
    sceKernelWaitThreadEnd(audio_thread, NULL, &timeout);
    audio_thread = -1;
}

int video_init(void) {
    if (module_ready >= 0) return module_ready;
    int status = 0;
    SceUID mod = sceKernelLoadStartModule("app0:modules/reAvPlayer.suprx", 0, NULL, 0, NULL, &status);
    module_ready = mod >= 0 ? 0 : mod;
    return module_ready;
}

/* Stopping a player (its audio thread, its decoder) took up to 2 s, all on
 * the main thread: a freeze every time a preview or film ended. The GPU wait
 * stays here; the rest runs on the closer thread. Opening took as long
 * (waiting for the last close, then the file header off the card: 3.6 s
 * stalls in the 2026-09-25 smoke run), so that runs on the opener thread;
 * until it is done the owner is set and video_frame gives nothing. */
#define CLOSE_Q 4
static struct { SceAvPlayerHandle h; int audio; } close_q[CLOSE_Q];
static volatile int close_n;
static SceUID close_sema = -1, closed_sema = -1, q_lock = -1;
static SceUID open_sema = -1, open_lock = -1;
static struct { char path[256]; int with_sound, loop, who; unsigned int start_ms; } req;
static volatile unsigned int req_gen;
static volatile int opening;

static int closer(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(close_sema, 1, NULL);
        for (;;) {
            sceKernelWaitSema(q_lock, 1, NULL);
            SceAvPlayerHandle h = close_n ? close_q[0].h : 0;
            int audio = close_n ? close_q[0].audio : 0;
            sceKernelSignalSema(q_lock, 1);
            if (!h) break;
            if (audio) audio_stop();                /* only the published player ever had sound */
            sceAvPlayerStop(h);
            sceAvPlayerClose(h);
            sceKernelWaitSema(q_lock, 1, NULL);
            for (int i = 1; i < close_n; ++i) close_q[i - 1] = close_q[i];
            if (--close_n == 0) closing = 0;
            sceKernelSignalSema(q_lock, 1);
            sceKernelSignalSema(closed_sema, 1);
        }
    }
    return 0;
}

static int threads_up(void);

/* Frees nothing itself: frames are released later by video_collect. */
static void hand_to_closer(SceAvPlayerHandle h, int audio) {
    sceKernelWaitSema(q_lock, 1, NULL);
    if (close_n == CLOSE_Q) {                       /* full: wait for room (only the opener can get here twice) */
        sceKernelSignalSema(q_lock, 1);
        while (close_n == CLOSE_Q) sceKernelWaitSema(closed_sema, 1, NULL);
        sceKernelWaitSema(q_lock, 1, NULL);
    }
    closing = 1;
    close_q[close_n].h = h;
    close_q[close_n++].audio = audio;
    sceKernelSignalSema(q_lock, 1);
    sceKernelSignalSema(close_sema, 1);
}

static void wait_closed(void) {
    while (closing) sceKernelWaitSema(closed_sema, 1, NULL);   /* a stale count just loops once more */
}

static int opener(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(open_sema, 1, NULL);
        sceKernelWaitSema(open_lock, 1, NULL);
        unsigned int gen = req_gen;
        char path[256];
        memcpy(path, req.path, sizeof(path));
        int with_sound = req.with_sound, loop = req.loop, who = req.who;
        unsigned int start_ms = req.start_ms;
        int want = opening;
        sceKernelSignalSema(open_lock, 1);
        if (!want) continue;
        wait_closed();                               /* one decoder at a time */
        SceAvPlayerHandle h = 0;
        int ok = video_init() >= 0;
        if (ok) {
            SceAvPlayerInitData init;
            memset(&init, 0, sizeof(init));
            init.memoryReplacement.allocate = av_alloc;
            init.memoryReplacement.deallocate = av_free;
            init.memoryReplacement.allocateTexture = av_alloc_tex;
            init.memoryReplacement.deallocateTexture = av_free_tex;
            init.basePriority = 0xA0;
            /* 2 buffers let the decoder start overwriting the frame we are still
             * drawing, which showed as torn rows; 4 gives it room to stay ahead. */
            init.numOutputVideoFrameBuffers = 4;
            init.autoStart = SCE_TRUE;
            h = sceAvPlayerInit(&init);
            if (is_error(h)) { h = 0; ok = 0; }
        }
        if (ok && sceAvPlayerAddSource(h, path) < 0) ok = 0;
        if (ok) sceAvPlayerSetLooping(h, loop ? SCE_TRUE : SCE_FALSE);
        sceKernelWaitSema(open_lock, 1, NULL);
        int current = gen == req_gen;
        if (current && ok) {                         /* publish: from here the main thread owns it */
            player = h;
            finished = 0;
            duration_ms = 0;
            pending_seek = start_ms;
            looping = loop;
            active = 1;
            if (with_sound) audio_start();
        }
        if (current) opening = 0;
        sceKernelSignalSema(open_lock, 1);
        if (current && !ok) {
            if (who == OWN_MOVIE || who == OWN_MUSIC) ui_toast("That file would not play", C_BAD);
        }
        if (h && !(current && ok)) {
            closing = 1;                             /* its frames were never drawn: no GPU wait */
            hand_to_closer(h, 0);
        }
    }
    return 0;
}

static int threads_up(void) {
    if (close_sema >= 0) return 0;
    main_tid = sceKernelGetThreadId();               /* first called from the UI loop */
    q_lock = sceKernelCreateSema("av_q", 0, 1, 1, NULL);
    open_lock = sceKernelCreateSema("av_open_lock", 0, 1, 1, NULL);
    open_sema = sceKernelCreateSema("av_open", 0, 0, 16, NULL);
    closed_sema = sceKernelCreateSema("av_closed", 0, 0, 1, NULL);
    close_sema = sceKernelCreateSema("av_close", 0, 0, 16, NULL);
    SceUID c = sceKernelCreateThread("av_closer", closer, 0x10000100, 0x4000, 0, 0, NULL);
    SceUID o = sceKernelCreateThread("av_opener", opener, 0x10000100, 0x4000, 0, 0, NULL);
    if (c < 0 || o < 0 || sceKernelStartThread(c, 0, NULL) < 0 || sceKernelStartThread(o, 0, NULL) < 0) return -1;
    return 0;
}

void video_stop(void) {
    if (threads_up() < 0) return;
    sceKernelWaitSema(open_lock, 1, NULL);
    ++req_gen;                                       /* an open in flight is dropped when it lands */
    opening = 0;
    owner = OWN_NONE;
    paused = 0;
    int was = active;
    active = 0;
    sceKernelSignalSema(open_lock, 1);
    if (!was) return;
    have_frame = 0;
    vita2d_wait_rendering_done();                   /* nothing on screen uses its frames now */
    hand_to_closer(player, 1);
}

int video_play(const char *path, int with_sound) {
    return video_play_opts(path, with_sound, 1, 0, OWN_PREVIEW);
}

int video_play_opts(const char *path, int with_sound, int loop, unsigned int start_ms, int who) {
    STAGE("video: open");
    video_stop();
    if (threads_up() < 0) return -1;
    sceKernelWaitSema(open_lock, 1, NULL);
    ++req_gen;
    snprintf(req.path, sizeof(req.path), "%s", path);
    req.with_sound = with_sound;
    req.loop = loop;
    req.start_ms = start_ms;
    req.who = who;
    opening = 1;
    owner = who;
    finished = 0;
    sceKernelSignalSema(open_lock, 1);
    sceKernelSignalSema(open_sema, 1);
    return 0;
}

void video_stop_owned(int who) { if (owner == who) video_stop(); }
int video_owner(void) { return active || opening ? owner : OWN_NONE; }
int video_paused(void) { return paused; }

/* Stream info and seeking only work once the player has opened the file, so
 * both are retried from video_tick until they take. */
static void video_tick(void) {
    if (!active) return;
    if (!sceAvPlayerIsActive(player)) {
        if (!looping) finished = 1;
        video_stop();
        return;
    }
    if (!duration_ms)
        for (uint32_t i = 0; i < 2; ++i) {
            SceAvPlayerStreamInfo info;
            memset(&info, 0, sizeof(info));
            if (sceAvPlayerGetStreamInfo(player, i, &info) >= 0 && info.duration > duration_ms)
                duration_ms = (unsigned int)info.duration;
        }
    if (pending_seek && sceAvPlayerJumpToTime(player, pending_seek) >= 0) pending_seek = 0;
}

int video_finished(void) {
    video_tick();
    int f = finished;
    finished = 0;
    return f;
}

void video_pause(int p) {
    if (!active || p == paused) return;
    if (p) sceAvPlayerPause(player); else sceAvPlayerResume(player);
    paused = p;
}

void video_seek(unsigned int ms) {
    if (!active) return;
    if (duration_ms && ms >= duration_ms) ms = duration_ms - 1000;
    pending_seek = ms ? ms : 1;
    video_tick();
}

unsigned int video_pos_ms(void) { return active ? (unsigned int)sceAvPlayerCurrentTime(player) : 0; }
unsigned int video_duration_ms(void) { video_tick(); return duration_ms; }

vita2d_texture *video_frame(void) {
    STAGE("video: frame");
    video_tick();
    if (!active) return NULL;
    SceAvPlayerFrameInfo f;
    memset(&f, 0, sizeof(f));
    /* The player may hand back the buffer the GPU is still sampling, which
     * showed as half-decoded blocks on the right of some clips. Let the last
     * draw finish before asking for the next frame. */
    vita2d_wait_rendering_done();
    if (sceAvPlayerGetVideoData(player, &f) && f.pData) {
        unsigned int w = f.details.video.width, h = f.details.video.height, need = w * h * 3 / 2;  /* Y + VU */
        if (need > copy_size) {
            if (copy_buf) av_free_tex(NULL, copy_buf);
            copy_size = ALIGN_UP(need, CDRAM_ALIGN);
            copy_buf = av_alloc_tex(NULL, CDRAM_ALIGN, copy_size);
            if (!copy_buf) { copy_size = 0; have_frame = 0; return NULL; }
        }
        if (sceDmacMemcpy(copy_buf, f.pData, need) < 0) memcpy(copy_buf, f.pData, need);
        sceGxmTextureInitLinear(&frame_tex.gxm_tex, copy_buf, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, w, h, 0);
        sceGxmTextureSetMinFilter(&frame_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&frame_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        have_frame = 1;
    }
    return have_frame ? &frame_tex : NULL;
}
