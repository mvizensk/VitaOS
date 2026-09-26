/* Voice Memos, the third built-in tile in Apps: the Vita's microphone.
 *
 * X on "New recording" records 16 kHz mono from the mic until X or O; each
 * memo is a plain WAV in ux0:data/arcadehub/memos/, newest first. X on a memo
 * plays it, [] deletes it (after asking). Recording and playback each run on
 * their own thread; the main thread only draws the level and the clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/rtc.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include "memo.h"
#include "video.h"

#define DIR_ "ux0:data/arcadehub/memos"
#define RATE 16000
#define GRAIN 256
#define MAX_MEMOS 200
#define ROW_H 58
#define LIST_Y 128

typedef struct { char name[64]; unsigned int bytes; SceDateTime when; } Memo;
static Memo memos[MAX_MEMOS];
static int nmemos, sel, open_;
static float top;

/* ---------- WAV ---------- */

static void wav_header(unsigned char *h, unsigned int data_bytes) {
    unsigned int v[] = {0x46464952, 36 + data_bytes, 0x45564157, 0x20746d66, 16};
    memcpy(h, v, 20);
    unsigned short fmt[] = {1, 1};
    memcpy(h + 20, fmt, 4);
    unsigned int rate = RATE, bps = RATE * 2;
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &bps, 4);
    unsigned short align[] = {2, 16};
    memcpy(h + 32, align, 4);
    unsigned int d[] = {0x61746164, data_bytes};
    memcpy(h + 36, d, 8);
}

static int newer(const void *a, const void *b) {
    SceUInt64 x, y;
    sceRtcGetTick(&((const Memo *)a)->when, (SceRtcTick *)&x);
    sceRtcGetTick(&((const Memo *)b)->when, (SceRtcTick *)&y);
    return x < y ? 1 : x > y ? -1 : 0;
}

/* A small folder, read only when the tile opens or a memo is saved. */
static void scan(void) {
    nmemos = 0;
    SceUID d = sceIoDopen(DIR_);
    if (d < 0) return;
    SceIoDirent e;
    while (nmemos < MAX_MEMOS) {
        memset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        int n = strlen(e.d_name);
        if (n < 5 || strcmp(e.d_name + n - 4, ".wav")) continue;
        Memo *m = &memos[nmemos++];
        snprintf(m->name, sizeof(m->name), "%s", e.d_name);
        m->bytes = e.d_stat.st_size > 44 ? (unsigned int)e.d_stat.st_size - 44 : 0;
        m->when = e.d_stat.st_mtime;
    }
    sceIoDclose(d);
    qsort(memos, nmemos, sizeof(Memo), newer);
}

/* ---------- recording ---------- */

static volatile int rec_run, rec_level;
static volatile unsigned int rec_bytes;
static SceUID rec_thread = -1;
static char rec_path[96];

static volatile int rec_peak;

/* The raw port has no automatic gain: bring the loudest moment up to about
 * -1.5 dB (at most 8x, so a silent memo stays quiet), in place, 1 s at a time. */
static void normalize(void) {
    if (rec_peak < 64) return;
    int gain256 = 27500 * 256 / rec_peak;
    if (gain256 > 8 * 256) gain256 = 8 * 256;
    if (gain256 < 300) return;                           /* already loud enough */
    SceUID fd = sceIoOpen(rec_path, SCE_O_RDWR, 0);
    if (fd < 0) return;
    static short buf[RATE];
    for (unsigned int at = 44; ; at += sizeof(buf)) {
        sceIoLseek(fd, at, SCE_SEEK_SET);
        int n = sceIoRead(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n / 2; ++i) {
            int v = buf[i] * gain256 / 256;
            buf[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        sceIoLseek(fd, at, SCE_SEEK_SET);
        sceIoWrite(fd, buf, n);
    }
    sceIoClose(fd);
}

static int rec_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    /* The raw port first: the voice port's noise gate turned a quiet room into
     * digital silence (only 0 and -4 in a 2026-09-25 test memo). */
    int port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_RAW, GRAIN, RATE, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port < 0) port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, GRAIN, RATE, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port < 0) {
        char m[64];
        snprintf(m, sizeof(m), "The microphone would not open (0x%08X)", port);
        ui_toast(m, C_BAD);
        rec_run = 0;
        return sceKernelExitDeleteThread(0);
    }
    sceIoMkdir(DIR_, 0777);
    SceUID fd = sceIoOpen(rec_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    unsigned char hdr[44];
    wav_header(hdr, 0);
    if (fd >= 0) sceIoWrite(fd, hdr, 44);
    /* Samples go to the card in 1 s batches, not every 16 ms grain. */
    static short batch[RATE];
    int filled = 0;
    short grain[GRAIN];
    while (rec_run && fd >= 0) {
        sceAudioInInput(port, grain);
        int peak = 0;
        for (int i = 0; i < GRAIN; ++i) { int a = grain[i] < 0 ? -grain[i] : grain[i]; if (a > peak) peak = a; }
        rec_level = peak;
        if (peak > rec_peak) rec_peak = peak;
        memcpy(batch + filled, grain, sizeof(grain));
        filled += GRAIN;
        if (filled + GRAIN > RATE) { sceIoWrite(fd, batch, filled * 2); rec_bytes += filled * 2; filled = 0; }
    }
    if (fd >= 0) {
        if (filled) { sceIoWrite(fd, batch, filled * 2); rec_bytes += filled * 2; }
        wav_header(hdr, rec_bytes);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        sceIoWrite(fd, hdr, 44);
        sceIoClose(fd);
        normalize();
    }
    sceAudioInReleasePort(port);
    rec_level = 0;
    return sceKernelExitDeleteThread(0);
}

static void rec_start(void) {
    if (rec_run) return;
    if (video_owner() == OWN_MUSIC) video_stop();       /* one voice at a time */
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(rec_path, sizeof(rec_path), DIR_ "/Memo %04d-%02d-%02d %02d.%02d.%02d.wav", t.year, t.month, t.day,
             t.hour, t.minute, t.second);
    rec_bytes = 0;
    rec_peak = 0;
    rec_run = 1;
    rec_thread = sceKernelCreateThread("memo_rec", rec_main, 0x10000100, 0x8000, 0, 0, NULL);
    if (rec_thread < 0 || sceKernelStartThread(rec_thread, 0, NULL) < 0) rec_run = 0;
}

static void rec_stop(void) {
    if (!rec_run) return;
    rec_run = 0;
    SceUInt timeout = 3 * 1000 * 1000;
    sceKernelWaitThreadEnd(rec_thread, NULL, &timeout);
    if (rec_bytes < RATE / 2) sceIoRemove(rec_path);    /* under a quarter second: a slip of the thumb */
    else ui_toast("Memo saved", C_OK);
    scan();
    sel = 0;
}

/* ---------- playback ---------- */

static volatile int play_run, play_level;
static volatile unsigned int play_pos, play_len;
static SceUID play_thread = -1;
static char play_path[96];
static int playing_idx = -1;

static int play_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUID fd = sceIoOpen(play_path, SCE_O_RDONLY, 0);
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (fd >= 0 && port >= 0) {
        int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
        sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
        sceIoLseek(fd, 44, SCE_SEEK_SET);
        static short buf[RATE];                          /* 1 s read at a time */
        int have = 0, at = 0;
        while (play_run) {
            if (at >= have) {
                int n = sceIoRead(fd, buf, sizeof(buf));
                if (n <= 0) break;
                have = n / 2; at = 0;
                if (have < GRAIN) { memset(buf + have, 0, (GRAIN - have) * 2); have = GRAIN; }
            }
            int peak = 0;
            for (int i = 0; i < GRAIN && at + i < have; ++i) { int a = buf[at + i] < 0 ? -buf[at + i] : buf[at + i]; if (a > peak) peak = a; }
            play_level = peak;
            sceAudioOutOutput(port, buf + at);
            at += GRAIN;
            play_pos += GRAIN * 2;
        }
        sceAudioOutOutput(port, NULL);
    }
    if (port >= 0) sceAudioOutReleasePort(port);
    if (fd >= 0) sceIoClose(fd);
    play_level = 0;
    play_run = 0;
    return sceKernelExitDeleteThread(0);
}

static void play_stop(void) {
    if (play_thread < 0) return;
    play_run = 0;
    SceUInt timeout = 2 * 1000 * 1000;
    sceKernelWaitThreadEnd(play_thread, NULL, &timeout);
    play_thread = -1;
    playing_idx = -1;
}

static void play_start(int i) {
    play_stop();
    if (video_owner() == OWN_MUSIC) video_stop();
    snprintf(play_path, sizeof(play_path), DIR_ "/%s", memos[i].name);
    play_pos = 0;
    play_len = memos[i].bytes;
    play_run = 1;
    playing_idx = i;
    play_thread = sceKernelCreateThread("memo_play", play_main, 0x10000100, 0x8000, 0, 0, NULL);
    if (play_thread < 0 || sceKernelStartThread(play_thread, 0, NULL) < 0) { play_run = 0; play_thread = -1; playing_idx = -1; }
}

/* ---------- the screen ---------- */

void memo_open(void) { open_ = 1; sel = 0; scan(); }
int memo_active(void) { return open_; }
void memo_leave(void) { rec_stop(); play_stop(); open_ = 0; }

const char *memo_hint(void) {
    if (rec_run) return "X stop   O stop";
    return sel == 0 ? "X record   O back   L R tabs" : "X play/stop   [] delete   O back   L R tabs";
}

static void clock_text(char *out, int max, unsigned int bytes) {
    unsigned int s = bytes / (RATE * 2);
    snprintf(out, max, "%u:%02u", s / 60, s % 60);
}

void memo_update(const Input *in) {
    if (playing_idx >= 0 && !play_run) play_stop();      /* reached the end */
    int count = nmemos + 1;                              /* row 0: New recording */
    if (rec_run) {
        if (in->pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) rec_stop();
    } else {
        if (in->pressed & SCE_CTRL_CIRCLE) { memo_leave(); return; }
        if (in->pressed & SCE_CTRL_UP) sel = sel > 0 ? sel - 1 : 0;
        if (in->pressed & SCE_CTRL_DOWN) sel = sel < count - 1 ? sel + 1 : sel;
        int tap_row = in->tapped && in->tap_y > LIST_Y - 40 && in->tap_y < H - 40
                      ? (int)(top + (in->tap_y - (LIST_Y - 40)) / ROW_H) : -1;
        int go = in->pressed & SCE_CTRL_CROSS;
        if (tap_row >= 0 && tap_row < count) { if (tap_row == sel) go = 1; else sel = tap_row; }
        if (go) {
            if (sel == 0) { play_stop(); rec_start(); }
            else if (playing_idx == sel - 1) play_stop();
            else play_start(sel - 1);
        }
        if ((in->pressed & SCE_CTRL_SQUARE) && sel > 0) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Delete \"%.*s\"? This can't be undone.", (int)strlen(memos[sel - 1].name) - 4, memos[sel - 1].name);
            if (ui_confirm("Delete memo", msg)) {
                play_stop();
                char p[96];
                snprintf(p, sizeof(p), DIR_ "/%s", memos[sel - 1].name);
                if (sceIoRemove(p) >= 0) ui_toast("Memo deleted", C_ACCENT);
                scan();
                if (sel > nmemos) sel = nmemos;
            }
        }
    }
    count = nmemos + 1;
    static GridScroll gs;
    grid_scroll(&gs, &sel, 1, count, 6, ROW_H, in);
    top = gs.top;

    text(bold, 36, 104, C_TEXT, 22, "Voice Memos");
    char sub[48];
    snprintf(sub, sizeof(sub), "%d memo%s", nmemos, nmemos == 1 ? "" : "s");
    text_right(font, W - 36, 104, C_FAINT, 15, sub);

    for (int i = 0; i < count; ++i) {
        float y = LIST_Y + (i - top) * ROW_H;
        if (y < LIST_Y - ROW_H || y > H - 40) continue;
        int focus = i == sel;
        draw_round_rect(28, y, W - 56, ROW_H - 8, 12, focus ? RGBA8(44, 52, 72, 235) : RGBA8(26, 30, 42, 200));
        if (focus) draw_round_ring(28, y, W - 56, ROW_H - 8, 12, 2, C_ACCENT);
        float cy = y + (ROW_H - 8) / 2;
        if (i == 0) {
            float pulse = rec_run ? 0.6f + 0.4f * ui_pulse() : 1;
            vita2d_draw_fill_circle(62, cy, 13, RGBA8(240, 70, 70, (int)(255 * pulse)));
            if (rec_run) {
                char t[16];
                clock_text(t, sizeof(t), rec_bytes);
                text(bold, 88, (int)cy + 7, C_TEXT, 18, "Recording");
                text(bold, 200, (int)cy + 7, RGBA8(240, 90, 90, 255), 18, t);
                /* a live level meter */
                float lv = rec_level / 32768.0f;
                lv = lv > 0 ? sqrtf(lv) : 0;
                for (int k = 0; k < 24; ++k) {
                    float on = k / 24.0f < lv;
                    vita2d_draw_rectangle(290 + k * 14, cy - 10, 9, 20, on ? RGBA8(240, 90, 90, 230) : RGBA8(255, 255, 255, 30));
                }
            } else text(bold, 88, (int)cy + 7, C_TEXT, 18, "New recording");
            continue;
        }
        Memo *m = &memos[i - 1];
        int me = playing_idx == i - 1;
        /* play or stop glyph */
        if (me) vita2d_draw_rectangle(55, cy - 7, 14, 14, C_ACCENT);
        else {
            vita2d_color_vertex *v = vita2d_pool_memalign(3 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
            if (v) {
                v[0] = (vita2d_color_vertex){56, cy - 9, 0.5f, C_TEXT};
                v[1] = (vita2d_color_vertex){56, cy + 9, 0.5f, C_TEXT};
                v[2] = (vita2d_color_vertex){71, cy, 0.5f, C_TEXT};
                vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
            }
        }
        char label[64];
        snprintf(label, sizeof(label), "%.*s", (int)strlen(m->name) - 4, m->name);
        text_fit(font, 88, (int)cy + 7, C_TEXT, 17, label, 520);
        char t[32];
        clock_text(t, sizeof(t), m->bytes);
        text_right(font, W - 50, (int)cy + 7, C_DIM, 16, t);
        if (me && play_len) {                            /* progress along the row's foot */
            float f = play_pos / (float)play_len;
            f = f > 1 ? 1 : f;
            vita2d_draw_rectangle(40, y + ROW_H - 14, (W - 80) * f, 3, C_ACCENT);
        }
    }
}

void memo_draw_icon(float x, float y, float size) {
    draw_soft(ui_soft_shadow(), x + size / 2, y + size + 2, size * 0.9f, size * 0.2f, RGBA8(0, 0, 0, 120));
    draw_round_rect(x, y, size, size, size * 0.22f, RGBA8(236, 72, 72, 255));
    draw_gradient(x + size * 0.1f, y, size * 0.8f, size * 0.5f, RGBA8(255, 255, 255, 40), RGBA8(255, 255, 255, 40),
                  RGBA8(255, 255, 255, 0), RGBA8(255, 255, 255, 0));
    float cx = x + size / 2;                             /* a waveform of bars */
    static const float hs[] = {0.18f, 0.34f, 0.52f, 0.30f, 0.62f, 0.40f, 0.22f};
    for (int k = 0; k < 7; ++k) {
        float bh = size * hs[k], bw = size * 0.065f, bx = cx + (k - 3) * size * 0.11f - bw / 2;
        draw_round_rect(bx, y + size / 2 - bh / 2, bw, bh, bw / 2, RGBA8(255, 255, 255, 245));
    }
}
