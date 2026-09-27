/* Community news for the Home row: the hot posts on r/vitahacks, where new
 * ports and homebrew are announced first. Reddit's JSON wants an account now;
 * its RSS does not, but it rate-limits, so one fetch an hour at most, and the
 * last copy on the card is shown until then. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/net/netctl.h>
#include "news.h"
#include "store.h"

#define FEED "https://www.reddit.com/r/vitahacks/.rss"
#define CACHE "ux0:data/arcadehub/news.rss"
#define SEEN "ux0:data/arcadehub/user/news-seen.txt"
#define STAMP "ux0:data/arcadehub/news.time"
#define NEWS_DIR "ux0:data/arcadehub/news"
#define MAX_NEWS 4

static NewsItem items[MAX_NEWS];
static volatile int nitems, ready;
static char seen_id[24];

static unsigned long long now_s(void) {
    SceRtcTick t;
    sceRtcGetCurrentTickUtc(&t);
    return t.tick / 1000000ull;
}

/* "2026-09-26T21:47:12+00:00" in the same seconds as now_s() */
static unsigned long long iso_s(const char *s) {
    SceDateTime d;
    memset(&d, 0, sizeof(d));
    if (sscanf(s, "%hu-%hu-%huT%hu:%hu:%hu", &d.year, &d.month, &d.day, &d.hour, &d.minute, &d.second) < 6) return 0;
    SceRtcTick t;
    if (sceRtcGetTick(&d, &t) < 0) return 0;
    return t.tick / 1000000ull;
}

static void unescape(char *s) {                       /* &amp; &lt; &gt; &quot; &#39; in place */
    static const char *const from[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&#39;", "&#x27;", "&#32;", "&nbsp;"};
    static const char to[] = {'&', '<', '>', '"', '\'', '\'', ' ', ' '};
    char *w = s;
    for (char *r = s; *r;) {
        int hit = 0;
        for (int k = 0; k < 8 && !hit; ++k) {
            int n = strlen(from[k]);
            if (!strncmp(r, from[k], n)) { *w++ = to[k]; r += n; hit = 1; }
        }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
}

/* The text of <tag>...</tag> inside [p, end), unescaped, tags stripped. */
static void grab(const char *p, const char *end, const char *tag, char *out, int max) {
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    out[0] = 0;
    const char *a = strstr(p, open);
    if (!a || a > end) return;
    a = strchr(a, '>');
    const char *b = a ? strstr(a, close) : NULL;
    if (!b || b > end) return;
    int n = (int)(b - a - 1);
    char *tmp = malloc(n + 1);
    if (!tmp) return;
    memcpy(tmp, a + 1, n);
    tmp[n] = 0;
    unescape(tmp);                                    /* the content is HTML inside escaped XML */
    int o = 0, in_tag = 0, space = 0;
    for (char *c = tmp; *c && o < max - 1; ++c) {
        if (*c == '<') { in_tag = 1; continue; }
        if (*c == '>') { in_tag = 0; if (!space && o) { out[o++] = ' '; space = 1; } continue; }
        if (in_tag) continue;
        if (*c == '\n' || *c == '\r' || *c == '\t' || *c == ' ') { if (!space && o) { out[o++] = ' '; space = 1; } continue; }
        out[o++] = *c;
        space = 0;
    }
    out[o] = 0;
    unescape(out);
    free(tmp);
}

static int has_word(const char *s, const char *w) {   /* case-insensitive substring */
    int n = strlen(w);
    for (; *s; ++s) if (!strncasecmp(s, w, n)) return 1;
    return 0;
}

static int boring(const char *title) {               /* threads, rules, meta */
    static const char *const words[] = {"questions thread", "rules", "weekly", "megathread", "moderator"};
    for (int k = 0; k < 5; ++k) if (has_word(title, words[k])) return 1;
    return strlen(title) < 8;
}

static void parse(void) {
    SceUID fd = sceIoOpen(CACHE, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    char *t = size > 0 && size < 512 * 1024 ? malloc(size + 1) : NULL;
    if (!t) { sceIoClose(fd); return; }
    int n = sceIoRead(fd, t, size);
    sceIoClose(fd);
    t[n > 0 ? n : 0] = 0;
    NewsItem got[MAX_NEWS];
    int k = 0;
    unsigned long long now = now_s();
    for (char *e = strstr(t, "<entry>"); e && k < MAX_NEWS; e = strstr(e + 7, "<entry>")) {
        char *end = strstr(e, "</entry>");
        if (!end) break;
        NewsItem *it = &got[k];
        memset(it, 0, sizeof(*it));
        grab(e, end, "title", it->title, sizeof(it->title));
        if (boring(it->title)) continue;
        char upd[40];
        grab(e, end, "updated", upd, sizeof(upd));
        unsigned long long when = iso_s(upd);
        if (!when || now < when || now - when > 14 * 86400) continue;   /* two weeks at most */
        it->age_s = (unsigned int)(now - when);
        grab(e, end, "name", it->author, sizeof(it->author));
        grab(e, end, "content", it->summary, sizeof(it->summary));
        char *tail = strstr(it->summary, "submitted by");   /* Reddit's footer: by, [link], [comments] */
        if (tail) *tail = 0;
        for (int L = strlen(it->summary); L && it->summary[L - 1] == ' '; --L) it->summary[L - 1] = 0;
        grab(e, end, "id", it->id, sizeof(it->id));
        const char *th = strstr(e, "<media:thumbnail url=\"");   /* the post's picture, if it has one */
        if (th && th < end) {
            th += 22;
            const char *q = strchr(th, '"');
            if (q && q < end && q - th < (int)sizeof(it->image)) {
                memcpy(it->image, th, q - th);
                it->image[q - th] = 0;
                unescape(it->image);
            }
        }
        snprintf(it->image_path, sizeof(it->image_path), NEWS_DIR "/%s.jpg", it->id);
        SceIoStat st;
        if (!it->image[0] || sceIoGetstat(it->image_path, &st) < 0) it->image_path[0] = 0;
        ++k;
    }
    free(t);
    memcpy(items, got, sizeof(got));
    nitems = k;
}

static int worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    FILE *f = fopen(SEEN, "r");
    if (f) { if (!fgets(seen_id, sizeof(seen_id), f)) seen_id[0] = 0; fclose(f); }
    for (char *c = seen_id; *c; ++c) if (*c == '\n') *c = 0;
    parse();                                          /* the last copy first */
    ready = 1;
    /* Home often starts before Wi-Fi is back (after sleep or a boot): wait for
     * it rather than skipping the fetch until the next launch. */
    for (int i = 0; i < 120; ++i) {
        int st = 0;
        if (sceNetCtlInetGetState(&st) >= 0 && st == SCE_NETCTL_STATE_CONNECTED) break;
        sceKernelDelayThread(5 * 1000 * 1000);
    }
    unsigned long long last = 0, now = now_s();       /* when we last asked, in its own file */
    f = fopen(STAMP, "r");
    if (f) { if (fscanf(f, "%llu", &last) != 1) last = 0; fclose(f); }
    if (!last || now < last || now - last >= 3600) {
        f = fopen(STAMP, "w");
        if (f) { fprintf(f, "%llu\n", now); fclose(f); }
        if (store_fetch(FEED, CACHE) >= 0) parse();
    }
    /* the pictures: once each, kept on the card under the post's id */
    sceIoMkdir(NEWS_DIR, 0777);
    for (int i = 0; i < nitems; ++i) {
        NewsItem *it = &items[i];
        if (!it->image[0] || it->image_path[0]) continue;
        char path[80];
        snprintf(path, sizeof(path), NEWS_DIR "/%s.jpg", it->id);
        if (store_fetch(it->image, path) >= 0) snprintf(it->image_path, sizeof(it->image_path), "%s", path);
    }
    return sceKernelExitDeleteThread(0);
}

void news_init(void) {
    SceUID t = sceKernelCreateThread("news", worker, 0x10000100, 0x8000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

int news_count(void) { return ready ? nitems : 0; }
const NewsItem *news_get(int i) { return i >= 0 && i < nitems ? &items[i] : NULL; }
int news_unseen(void) { return nitems && strcmp(items[0].id, seen_id); }

void news_mark_seen(void) {
    if (!nitems || !strcmp(items[0].id, seen_id)) return;
    snprintf(seen_id, sizeof(seen_id), "%s", items[0].id);
    FILE *f = fopen(SEEN, "w");
    if (f) { fprintf(f, "%s\n", seen_id); fclose(f); }
}
