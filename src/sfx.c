/* UI sounds: the small clicks and chimes that make PS5 and Switch menus feel
 * physical. All synthesized here at start-up (no files, nothing to license),
 * mixed by one thread into their own mono audio port so they sit on top of
 * previews and music. The level (0-10, 0 = off) lives in user/sfx.cfg. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include "sfx.h"
#include "ui.h"

#define RATE 48000
#define GRAIN 256
#define VOICES 4
#define CFG "ux0:data/arcadehub/user/sfx.cfg"

typedef struct { short *pcm; int len; } Sound;
static Sound sounds[SFX_COUNT];
static struct { int id, pos; } voice[VOICES];
static volatile int level = 6, ambient_level = 0;   /* Home music off unless chosen (2026-10-02: "no sound on the lobby") */
/* Home's ambient bed: a 32 s loop of slow chords, made in the background. */
#define BED_SEC 32
static short *bed;
static volatile int bed_ready, bed_want;
static int bed_pos;
static float bed_gain;                     /* eases toward bed_want, so it fades in and out */
static SceUID sema = -1, lock = -1;
static int port = -1;

static float noise(void) { return (float)rand() / RAND_MAX * 2.0f - 1.0f; }

/* A short voice: sum of partials (Hz, weight), attack then exponential decay,
 * with an optional pitch glide. Everything stays quiet: these are accents. */
static Sound synth(float ms, const float *hz, const float *w, int n, float attack_ms, float decay, float glide, float gain) {
    Sound s;
    s.len = (int)(RATE * ms / 1000);
    s.pcm = malloc(s.len * sizeof(short));
    float phase[4] = {0};
    for (int i = 0; i < s.len; ++i) {
        float t = (float)i / RATE, env = t < attack_ms / 1000 ? t / (attack_ms / 1000) : expf(-(t - attack_ms / 1000) * decay);
        float bend = 1.0f + glide * t / (ms / 1000), v = 0;
        for (int k = 0; k < n; ++k) {
            phase[k] += 2 * (float)M_PI * hz[k] * bend / RATE;
            v += w[k] * sinf(phase[k]);
        }
        float fade = i > s.len - 64 ? (s.len - i) / 64.0f : 1.0f;   /* no click at the end */
        s.pcm[i] = (short)(v * env * fade * gain * 32767);
    }
    return s;
}

/* Air moving past: noise through a resonant low-pass whose cutoff sweeps up. */
static Sound whoosh(float ms, float from_hz, float to_hz, float gain) {
    Sound s;
    s.len = (int)(RATE * ms / 1000);
    s.pcm = malloc(s.len * sizeof(short));
    float lp = 0, bp = 0;
    for (int i = 0; i < s.len; ++i) {
        float u = (float)i / s.len, f = from_hz + (to_hz - from_hz) * u;
        float c = 2 * sinf((float)M_PI * f / RATE);           /* state-variable filter */
        float hp = noise() - lp - 0.6f * bp;
        bp += c * hp;
        lp += c * bp;
        float env = sinf((float)M_PI * u);                     /* swell in and out */
        s.pcm[i] = (short)(bp * env * env * gain * 32767);
    }
    return s;
}

static void build(void) {
    { float hz[] = {2100, 4200}, w[] = {1.0f, 0.25f};          /* a soft tick */
      sounds[SFX_MOVE] = synth(28, hz, w, 2, 0.6f, 180, -0.08f, 0.10f); }
    { float hz[] = {880, 1320, 1760}, w[] = {1.0f, 0.55f, 0.2f};   /* a bright two-partial chime */
      sounds[SFX_SELECT] = synth(170, hz, w, 3, 2.0f, 26, 0.0f, 0.11f); }
    { float hz[] = {620, 930}, w[] = {1.0f, 0.3f};             /* the same, falling: going back */
      sounds[SFX_BACK] = synth(120, hz, w, 2, 1.5f, 34, -0.18f, 0.09f); }
    sounds[SFX_TAB] = whoosh(150, 400, 5200, 0.20f);
    { float hz[] = {140, 280}, w[] = {1.0f, 0.4f};             /* a dull knock at the end of a row */
      sounds[SFX_BUMP] = synth(60, hz, w, 2, 1.0f, 60, -0.3f, 0.16f); }
    { float hz[] = {523, 784, 1047, 1568}, w[] = {1.0f, 0.8f, 0.6f, 0.25f};   /* a rising major chord */
      sounds[SFX_LAUNCH] = synth(520, hz, w, 4, 12.0f, 7, 0.06f, 0.08f); }
    { float hz[] = {196, 294, 392, 587}, w[] = {1.0f, 0.7f, 0.5f, 0.3f};      /* start-up: a slow, warm swell */
      sounds[SFX_BOOT] = synth(1400, hz, w, 4, 380.0f, 2.6f, 0.02f, 0.07f); }
}

/* Four soft chords (Cmaj9, Am9, Fmaj7, G6/9), eight seconds each, sine
 * partials with a slow tremolo, crossfaded so the loop has no seam. */
static int make_bed(SceSize args, void *argp) {
    (void)args; (void)argp;
    static const float chords[4][4] = {{130.8f, 196.0f, 246.9f, 293.7f}, {110.0f, 164.8f, 196.0f, 246.9f},
                                       {87.3f, 174.6f, 220.0f, 329.6f}, {98.0f, 146.8f, 220.0f, 293.7f}};
    int len = RATE * BED_SEC, seg = len / 4;
    short *b = malloc(len * sizeof(short));
    if (!b) return 0;
    /* Phases wrap at 2 pi. Left to grow over 1.5 M samples they reached ~60,000
     * radians, where a float keeps too few bits for a 0.04 rad step: the pad
     * came out gritty, a constant glitch on Home (red Vita, 2026-10-02). */
    double ph[4][8] = {{0}};
    const double TAU = 2 * M_PI;
#define STEP(p, inc) ((p) += (inc), (p) >= TAU ? ((p) -= TAU) : (p))   /* cheaper than fmod: steps are tiny */
    for (int i = 0; i < len; ++i) {
        float t = (float)i / RATE, v = 0;   /* t only drives the slow tremolo: float is plenty */
        int c = i / seg;
        float u = (float)(i % seg) / seg;
        float fade = u < 0.25f ? u / 0.25f : u > 0.75f ? (1 - u) / 0.25f : 1;   /* chords swell and give way */
        for (int k = 0; k < 4; ++k) {
            float f = chords[c][k];
            STEP(ph[c][k], TAU * f / RATE);
            STEP(ph[c][k + 4], TAU * f * 2.003 / RATE);                        /* a detuned octave: shimmer */
            float trem = 0.8f + 0.2f * sinf(t * (0.13f + k * 0.05f));
            v += (sinf((float)ph[c][k]) + 0.25f * sinf((float)ph[c][k + 4])) * trem * 0.22f;
        }
        /* the next chord fades in underneath the last quarter */
        if (u > 0.75f) {
            int n = (c + 1) % 4;
            for (int k = 0; k < 4; ++k) {
                STEP(ph[n][k], TAU * chords[n][k] / RATE);
                v += sinf((float)ph[n][k]) * 0.22f * ((u - 0.75f) / 0.25f) * 0.9f;
            }
            v *= 0.8f;
        }
        b[i] = (short)(v * (0.35f + 0.65f * fade) * 0.33f * 32767);
    }
    bed = b;
    bed_ready = 1;
    return 0;
}

static int mixer(SceSize args, void *argp) {
    (void)args; (void)argp;
    static short out[GRAIN];
    for (;;) {
        int busy = 0;
        sceKernelWaitSema(lock, 1, NULL);
        memset(out, 0, sizeof(out));
        int gain = level * 26;                                  /* 0..260, /256 below */
        float bed_to = bed_want && bed_ready ? ambient_level / 10.0f : 0;
        if (bed_ready && (bed_to > 0 || bed_gain > 0)) {
            for (int i = 0; i < GRAIN; ++i) {
                bed_gain += (bed_to - bed_gain) * 0.00008f;       /* about a second */
                out[i] = (short)(bed[bed_pos] * bed_gain * 0.5f);
                bed_pos = (bed_pos + 1) % (RATE * BED_SEC);
            }
            if (bed_gain > 0.001f || bed_to > 0) busy = 1;
            else bed_gain = 0;
        }
        for (int v = 0; v < VOICES; ++v) {
            if (voice[v].id < 0) continue;
            Sound *s = &sounds[voice[v].id];
            int n = s->len - voice[v].pos < GRAIN ? s->len - voice[v].pos : GRAIN;
            for (int i = 0; i < n; ++i) {
                int m = out[i] + s->pcm[voice[v].pos + i] * gain / 256;
                out[i] = m > 32767 ? 32767 : m < -32768 ? -32768 : m;
            }
            voice[v].pos += n;
            if (voice[v].pos >= s->len) voice[v].id = -1;
            else busy = 1;
        }
        sceKernelSignalSema(lock, 1);
        sceAudioOutOutput(port, out);
        if (!busy) sceKernelWaitSema(sema, 1, NULL);            /* sleep until the next sound */
    }
    return 0;
}

void sfx_init(void) {
    SceUID fd = sceIoOpen(CFG, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        char b[16] = {0};
        if (sceIoRead(fd, b, sizeof(b) - 1) > 0) {
            level = atoi(b) < 0 ? 0 : atoi(b) > 10 ? 10 : atoi(b);
            const char *sp = strchr(b, ' ');
            if (sp) ambient_level = atoi(sp + 1) < 0 ? 0 : atoi(sp + 1) > 10 ? 10 : atoi(sp + 1);
        }
        sceIoClose(fd);
    }
    for (int v = 0; v < VOICES; ++v) voice[v].id = -1;
    build();
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (port < 0) return;
    sema = sceKernelCreateSema("sfx_wake", 0, 0, 1, NULL);
    lock = sceKernelCreateSema("sfx_lock", 0, 1, 1, NULL);
    SceUID t = sceKernelCreateThread("sfx", mixer, 0x40, 0x2000, 0, 0, NULL);   /* above the UI: no late clicks */
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    SceUID m = sceKernelCreateThread("sfx_bed", make_bed, 0xBF, 0x2000, 0, 0, NULL);   /* lowest: never slows start-up */
    if (m >= 0) sceKernelStartThread(m, 0, NULL);
}

void sfx_play(int id) {
    if (port < 0 || level <= 0 || id < 0 || id >= SFX_COUNT) return;
    sceKernelWaitSema(lock, 1, NULL);
    for (int v = 0; v < VOICES; ++v)
        if (voice[v].id == id && voice[v].pos < 1200) { sceKernelSignalSema(lock, 1); return; }   /* same sound just started */
    int slot = 0;
    for (int v = 0; v < VOICES; ++v) {
        if (voice[v].id < 0) { slot = v; break; }
        if (voice[v].pos > voice[slot].pos) slot = v;            /* all busy: replace the oldest */
    }
    voice[slot].id = id;
    voice[slot].pos = 0;
    sceKernelSignalSema(lock, 1);
    sceKernelSignalSema(sema, 1);
}

int sfx_level(void) { return level; }

void sfx_ambient(int want) {
    if (want && !bed_want && port >= 0) sceKernelSignalSema(sema, 1);   /* wake the mixer */
    bed_want = want && ambient_level > 0;
}

int sfx_ambient_level(void) { return ambient_level; }

void sfx_set_ambient_level(int v) {
    ambient_level = v < 0 ? 0 : v > 10 ? 10 : v;
    sfx_set_level(level);                    /* rewrites the file with both */
}
int sfx_ok(void) { return port >= 0; }

void sfx_set_level(int v) {
    level = v < 0 ? 0 : v > 10 ? 10 : v;
    char b[16];
    int n = snprintf(b, sizeof(b), "%d %d\n", level, ambient_level);
    ui_save(CFG, b, n, 0);
}
