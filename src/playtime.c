/* Play time per game. Home exits for the whole run of a game (play.c's
 * launch()), so there is no process alive to add up seconds while it plays.
 * Instead: on launch, drop the game ref and a tick in playing.txt; next time
 * Home starts, playtime_init() finds it, works out how long that was from
 * the tick that is still there, and folds it into the totals and the day
 * log. Everything here is plain tab files, like the rest of ux0:data/arcadehub/user/. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "playtime.h"

#define ROOT "ux0:data/arcadehub/"
#define USER ROOT "user/"
#define PLAYING USER "playing.txt"
#define TOTALS_FILE USER "playtime.tsv"
#define LOG_FILE USER "playtime-log.tsv"

#define SESSION_MIN 30            /* shorter than this is a bounce, not a play */
#define SESSION_CAP (6 * 3600)    /* Home left running overnight is not one session */
#define LOG_DAYS 30               /* the day log only needs to cover the week card plus slack */

/* ---------- small file helpers (own copies: playtime.c stands alone) ---------- */

static char *slurp(const char *path, int *len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    char *buf = malloc(size + 1);
    int n = buf ? sceIoRead(fd, buf, size) : -1;
    sceIoClose(fd);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static void write_file(const char *path, const char *buf, int n) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

static void append_file(const char *path, const char *buf, int n) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

/* Local midnight, as a day number: contiguous, so a streak is just a run of
 * consecutive integers rather than a calendar walk. */
static long day_id(time_t t) {
    struct tm tmv = *localtime(&t);
    long since_midnight = tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
    return (long)((t - since_midnight) / 86400);
}

/* ---------- totals: playtime.tsv (sys, title, seconds, last played) ---------- */

#define MAX_TOTALS 512
typedef struct { char sys[32], title[160]; long seconds, last; } Total;
static Total totals[MAX_TOTALS];
static int ntotals;

static int total_of(const char *sys, const char *title, int make) {
    for (int i = 0; i < ntotals; ++i)
        if (!strcmp(totals[i].sys, sys) && !strncmp(totals[i].title, title, sizeof(totals[i].title) - 1)) return i;
    if (!make || ntotals >= MAX_TOTALS) return -1;
    snprintf(totals[ntotals].sys, sizeof(totals[0].sys), "%s", sys);
    snprintf(totals[ntotals].title, sizeof(totals[0].title), "%s", title);
    totals[ntotals].seconds = totals[ntotals].last = 0;
    return ntotals++;
}

static char save_buf[64 * 1024];

static void save_totals(void) {
    int len = 0;
    for (int i = 0; i < ntotals && len < (int)sizeof(save_buf) - 240; ++i)
        len += snprintf(save_buf + len, sizeof(save_buf) - len, "%s\t%s\t%ld\t%ld\n",
                         totals[i].sys, totals[i].title, totals[i].seconds, totals[i].last);
    write_file(TOTALS_FILE, save_buf, len);
}

static void load_totals(void) {
    char *text = slurp(TOTALS_FILE, NULL);
    if (!text) return;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line && ntotals < MAX_TOTALS; line = strtok_r(NULL, "\r\n", &save)) {
        char *f[4] = {line, 0, 0, 0};
        for (int k = 1; k < 4 && f[k - 1]; ++k) { f[k] = strchr(f[k - 1], '\t'); if (f[k]) *f[k]++ = 0; }
        if (!f[3]) continue;
        snprintf(totals[ntotals].sys, sizeof(totals[0].sys), "%s", f[0]);
        snprintf(totals[ntotals].title, sizeof(totals[0].title), "%s", f[1]);
        totals[ntotals].seconds = atol(f[2]);
        totals[ntotals++].last = atol(f[3]);
    }
    free(text);
}

long playtime_total_seconds(const char *sys, const char *title) {
    int i = total_of(sys, title, 0);
    return i < 0 ? 0 : totals[i].seconds;
}

long playtime_last_played(const char *sys, const char *title) {
    int i = total_of(sys, title, 0);
    return i < 0 ? 0 : totals[i].last;
}

/* ---------- the launch marker: playing.txt (sys, title, tick) ---------- */

void playtime_record_launch(const char *sys, const char *title) {
    SceRtcTick now;
    sceRtcGetCurrentTickUtc(&now);
    char line[256];
    int n = snprintf(line, sizeof(line), "%s\t%s\t%llu\n", sys, title, (unsigned long long)now.tick);
    write_file(PLAYING, line, n);
}

/* Whatever session playing.txt describes just ended (Home is starting up
 * again). Folds it into the totals and today's line in the day log. */
static void close_out_session(void) {
    char *text = slurp(PLAYING, NULL);
    if (!text) return;
    sceIoRemove(PLAYING);        /* consumed either way: a bad line is not retried forever */
    char *t1 = strchr(text, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (!t2) { free(text); return; }
    *t1 = 0; *t2 = 0;
    const char *sys = text, *title = t1 + 1;
    unsigned long long start_tick = strtoull(t2 + 1, NULL, 10);

    SceRtcTick now;
    sceRtcGetCurrentTickUtc(&now);
    /* now.tick < start_tick means the clock moved backward under the game
     * (a manual RTC change): nothing sane to log, so drop it rather than
     * let the unsigned wraparound turn into a huge signed value. */
    long secs = now.tick >= start_tick ? (long)((now.tick - start_tick) / 1000000ULL) : -1;
    if (secs >= SESSION_MIN) {
        if (secs > SESSION_CAP) secs = SESSION_CAP;
        int i = total_of(sys, title, 1);
        if (i >= 0) {
            totals[i].seconds += secs;
            totals[i].last = (long)time(NULL);
            save_totals();
        }
        char line[256];
        int n = snprintf(line, sizeof(line), "%ld\t%s\t%s\t%ld\n", day_id(time(NULL)), sys, title, secs);
        append_file(LOG_FILE, line, n);
    }
    free(text);
}

/* ---------- the day log: playtime-log.tsv (day, sys, title, seconds) ---------- */

#define MAX_TOP 64   /* distinct games seen this week; the card only shows the top few */
static long week_seconds;
static PlaytimeTop week_top[PLAYTIME_TOP];
static int week_ntop, week_streak, week_any;

/* Reads the whole log, drops anything older than LOG_DAYS, and along the way
 * tallies the last 7 days (for the Home card) and the day streak. One pass:
 * the log is small (pruned every start) and this only runs once, at startup. */
static void prune_and_summarize(void) {
    week_seconds = 0; week_ntop = 0; week_streak = 0; week_any = 0;
    char *text = slurp(LOG_FILE, NULL);
    if (!text) return;

    long today = day_id(time(NULL));
    long cutoff_keep = today - (LOG_DAYS - 1);
    long cutoff_week = today - 6;

    typedef struct { char sys[32], title[160]; long seconds; } TopWork;
    static TopWork top[MAX_TOP];
    int ntop_work = 0;
    signed char have_day[LOG_DAYS] = {0};   /* have_day[today - d] for d in 0..LOG_DAYS-1 */

    int outlen = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *f[4] = {line, 0, 0, 0};
        for (int k = 1; k < 4 && f[k - 1]; ++k) { f[k] = strchr(f[k - 1], '\t'); if (f[k]) *f[k]++ = 0; }
        if (!f[3]) continue;
        long day = atol(f[0]);
        const char *sys = f[1], *title = f[2];
        long secs = atol(f[3]);
        if (day < cutoff_keep || day > today) continue;   /* stale, or a clock rolled back */

        if (outlen < (int)sizeof(save_buf) - 260)
            outlen += snprintf(save_buf + outlen, sizeof(save_buf) - outlen, "%ld\t%s\t%s\t%ld\n", day, sys, title, secs);

        int d = (int)(today - day);
        if (d >= 0 && d < LOG_DAYS) have_day[d] = 1;

        if (day >= cutoff_week) {
            week_seconds += secs;
            week_any = 1;
            int i = -1;
            for (int k = 0; k < ntop_work; ++k)
                if (!strcmp(top[k].sys, sys) && !strncmp(top[k].title, title, sizeof(top[k].title) - 1)) { i = k; break; }
            if (i < 0 && ntop_work < MAX_TOP) {
                i = ntop_work++;
                snprintf(top[i].sys, sizeof(top[0].sys), "%s", sys);
                snprintf(top[i].title, sizeof(top[0].title), "%s", title);
                top[i].seconds = 0;
            }
            if (i >= 0) top[i].seconds += secs;
        }
    }
    free(text);
    write_file(LOG_FILE, save_buf, outlen);

    /* Today counts as day 0 only once it has play; the streak still runs
     * through yesterday so finishing a session doesn't reset it to zero. */
    int start = have_day[0] ? 0 : 1;
    for (int d = start; d < LOG_DAYS && have_day[d]; ++d) week_streak++;

    /* Top few by seconds, most first: MAX_TOP is small, a selection sort is plenty. */
    week_ntop = ntop_work < PLAYTIME_TOP ? ntop_work : PLAYTIME_TOP;
    for (int i = 0; i < week_ntop; ++i) {
        int best = i;
        for (int k = i + 1; k < ntop_work; ++k) if (top[k].seconds > top[best].seconds) best = k;
        if (best != i) { TopWork tmp = top[i]; top[i] = top[best]; top[best] = tmp; }
        snprintf(week_top[i].title, sizeof(week_top[0].title), "%s", top[i].title);
        week_top[i].seconds = top[i].seconds;
    }
}

int playtime_week(long *seconds_out, PlaytimeTop *top_out, int *ntop_out, int *streak_out) {
    if (seconds_out) *seconds_out = week_seconds;
    if (ntop_out) *ntop_out = week_ntop;
    if (streak_out) *streak_out = week_streak;
    if (top_out) for (int i = 0; i < week_ntop; ++i) top_out[i] = week_top[i];
    return week_any;
}

void playtime_init(void) {
    load_totals();
    close_out_session();
    prune_and_summarize();
}
