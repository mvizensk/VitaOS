/* A game's description from Wikipedia, for libraries with no scraped text
 * (a DM, 2026-09-29: "I don't have the game verbiage on games that you have").
 * The game databases with synopses need accounts and keys; Wikipedia does not.
 * On a game's details page: search "<title> video game", take the top page,
 * and keep its summary only if it says it is a video game. Cached per title
 * in ux0:data/arcadehub/desc/ (an empty file means "looked, nothing"), so a
 * game is looked up once. One lookup at a time, on its own thread. */
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gamedesc.h"

#define DIR "ux0:data/arcadehub/desc"
#define MAX_BODY (64 * 1024)

static char want[160], got_for[160], result[900];
static volatile int busy, ready;

static int has_ci(const char *hay, const char *needle) {   /* newlib has no strcasestr */
    for (int n = (int)strlen(needle); *hay; ++hay) if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

static void cache_path(const char *title, char *out, int max) {
    unsigned int h = 2166136261u;                         /* FNV-1a of the title */
    for (const unsigned char *p = (const unsigned char *)title; *p; ++p) h = (h ^ *p) * 16777619u;
    snprintf(out, max, DIR "/%08x.txt", h);
}

int gamedesc_cached(const char *title, char *out, int max) {
    char path[64];
    cache_path(title, path, sizeof(path));
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, out, max - 1);
    sceIoClose(fd);
    out[n > 0 ? n : 0] = 0;
    return n > 0;
}

static void cache_put(const char *title, const char *text) {
    char path[64];
    sceIoMkdir(DIR, 0777);
    cache_path(title, path, sizeof(path));
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, text, strlen(text)); sceIoClose(fd); }
}

static char *body;
static int used;
static size_t on_data(char *p, size_t s, size_t n, void *u) {
    (void)u;
    size_t len = s * n;
    if (used + (int)len >= MAX_BODY) len = MAX_BODY - 1 - used;
    memcpy(body + used, p, len);
    used += (int)len;
    body[used] = 0;
    return s * n;
}

static int get(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    used = 0; body[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaOS/1.5 (PS Vita; github.com/mvizensk/VitaOS)");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    long code = 0;
    int ok = curl_easy_perform(c) == CURLE_OK;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return ok && code == 200 ? 0 : -1;
}

static void escape(const char *in, char *out, int max) {
    int n = 0;
    for (; *in && n < max - 4; ++in) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || strchr("-_.~", c)) out[n++] = c;
        else if (c == ' ') out[n++] = '_';               /* page titles take underscores */
        else n += snprintf(out + n, max - n, "%%%02X", c);
    }
    out[n] = 0;
}

static void query_escape(const char *in, char *out, int max) {
    int n = 0;
    for (; *in && n < max - 4; ++in) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || strchr("-_.~", c)) out[n++] = c;
        else n += snprintf(out + n, max - n, "%%%02X", c);
    }
    out[n] = 0;
}

/* The JSON string value after "key":"; \uXXXX becomes UTF-8. */
static int json_string(const char *from, const char *key, char *out, int max) {
    const char *p = strstr(from, key);
    if (!p) return -1;
    p += strlen(key);
    int n = 0;
    while (*p && *p != '"' && n < max - 4) {
        if (*p != '\\') { out[n++] = *p++; continue; }
        ++p;
        char e = *p++;
        if (e == 'n') out[n++] = ' ';
        else if (e == 't') out[n++] = ' ';
        else if (e == 'u' && isxdigit((unsigned char)p[0])) {
            unsigned int cp = (unsigned int)strtoul((char[]){p[0], p[1], p[2], p[3], 0}, NULL, 16);
            p += 4;
            if (cp < 0x80) out[n++] = (char)cp;
            else if (cp < 0x800) { out[n++] = (char)(0xC0 | cp >> 6); out[n++] = (char)(0x80 | (cp & 63)); }
            else { out[n++] = (char)(0xE0 | cp >> 12); out[n++] = (char)(0x80 | (cp >> 6 & 63)); out[n++] = (char)(0x80 | (cp & 63)); }
        } else out[n++] = e;
    }
    out[n] = 0;
    return n ? 0 : -1;
}

/* "Legend of Zelda, The - A Link to the Past (USA)" -> "The Legend of Zelda: A Link to the Past". */
static void clean_title(const char *in, char *out, int max) {
    char t[160];
    snprintf(t, sizeof(t), "%s", in);
    for (char *c = t; *c; ++c) if (*c == '(' || *c == '[') { *c = 0; break; }
    char *sep = strstr(t, " - ");
    char rest[160] = "";
    if (sep) { snprintf(rest, sizeof(rest), "%s", sep + 3); *sep = 0; }
    int len = (int)strlen(t);
    while (len && t[len - 1] == ' ') t[--len] = 0;
    char head[160];
    if (len > 5 && !strcmp(t + len - 5, ", The")) { t[len - 5] = 0; snprintf(head, sizeof(head), "The %s", t); }
    else snprintf(head, sizeof(head), "%s", t);
    if (rest[0]) snprintf(out, max, "%s: %s", head, rest);
    else snprintf(out, max, "%s", head);
}

/* The Wikipedia page for a game: its summary (kept only if it says it is a
 * game) and its infobox picture, usually the box art. `body` must be set. */
static int net_failed;                    /* a lookup hit an error (429, timeout), not just "no page" */
static void lookup(const char *title, char *text, int tmax, char *img, int imax) {
    net_failed = 0;
    char q[200], eq[400], url[600], page[200] = "", ep[400];
    text[0] = 0;
    if (img) img[0] = 0;
    clean_title(title, q, sizeof(q));
    char search[240];
    snprintf(search, sizeof(search), "%s video game", q);
    query_escape(search, eq, sizeof(eq));
    snprintf(url, sizeof(url),
             "https://en.wikipedia.org/w/api.php?action=query&list=search&srlimit=5&srprop=&format=json&srsearch=%s", eq);
    if (get(url) != 0) { net_failed = 1; return; }
    {
        /* The top hit can be a compilation or a remake ("A Link to the Past and
         * Four Swords" for A Link to the Past): prefer the page named exactly
         * like the game, or "<name> (... video game)", then the top hit. */
        char first[200] = "";
        int ql = (int)strlen(q);
        for (const char *r = body; (r = strstr(r, "\"title\":\"")) != NULL; r += 9) {
            char t[200];
            if (json_string(r, "\"title\":\"", t, sizeof(t)) != 0) continue;
            if (!first[0]) snprintf(first, sizeof(first), "%s", t);
            if (!strcasecmp(t, q) || (!strncasecmp(t, q, ql) && t[ql] == ' ' && t[ql + 1] == '(')) {
                snprintf(page, sizeof(page), "%s", t);
                break;
            }
        }
        if (!page[0]) snprintf(page, sizeof(page), "%s", first);
    }
    if (!page[0]) return;
    escape(page, ep, sizeof(ep));
    snprintf(url, sizeof(url), "https://en.wikipedia.org/api/rest_v1/page/summary/%s", ep);
    char t[1200];
    /* "platform game", "Metroidvania game", "video game": a game page says game */
    if (get(url) != 0) { net_failed = 1; return; }
    if (json_string(body, "\"extract\":\"", t, sizeof(t)) != 0 || !has_ci(t, " game")) return;
    snprintf(text, tmax, "%s", t);
    if (img && json_string(body, "\"thumbnail\":{\"source\":\"", img, imax) == 0) {   /* ~320 px, plenty for a tile */
        char *qm = strchr(img, '?');                 /* "...Box.jpg?utm_source=...": the file type is before it */
        if (qm) *qm = 0;
    }
}

static int fetch(SceSize args, void *argp) {
    (void)args; (void)argp;
    char title[160];
    snprintf(title, sizeof(title), "%s", want);
    body = malloc(MAX_BODY);
    result[0] = 0;
    if (body) {
        lookup(title, result, sizeof(result), NULL, 0);
        free(body);
        body = NULL;
    }
    if (!net_failed) cache_put(title, result);            /* even when empty: do not ask again (unless it was an error) */
    snprintf(got_for, sizeof(got_for), "%s", title);
    ready = 1;
    busy = 0;
    return sceKernelExitDeleteThread(0);
}

/* Box art from the game's Wikipedia page, saved to dest_noext + ".jpg" or
 * ".png" (what Wikipedia serves); the path written to out. Blocking, for a
 * background thread; waits for a details-page lookup to finish first, and
 * fills the description cache on the way. 1 saved, 0 none, -1 the network
 * or Wikipedia said no (rate limit): try again another time. */
static size_t to_file(char *p, size_t s, size_t n, void *u) { return sceIoWrite(*(SceUID *)u, p, s * n) < 0 ? 0 : s * n; }

int gamedesc_cover(const char *title, const char *dest_noext, char *out, int max) {
    while (busy) sceKernelDelayThread(100 * 1000);
    busy = 1;
    int ok = 0;
    body = malloc(MAX_BODY);
    if (body) {
        char text[900], img[600];
        lookup(title, text, sizeof(text), img, sizeof(img));
        free(body);
        body = NULL;
        char probe[160];
        if (net_failed) { busy = 0; return -1; }
        if (text[0] || gamedesc_cached(title, probe, sizeof(probe)) < 0) cache_put(title, text);   /* a found text beats an old "none" */
        const char *dot = strrchr(img, '.');
        if (img[0] && dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png"))) {
            snprintf(out, max, "%s%s", dest_noext, !strcasecmp(dot, ".png") ? ".png" : ".jpg");
            SceUID fd = sceIoOpen(out, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            CURL *c = fd >= 0 ? curl_easy_init() : NULL;
            if (c) {
                curl_easy_setopt(c, CURLOPT_URL, img);
                curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
                curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaOS/1.7 (PS Vita; github.com/mvizensk/VitaOS)");
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &fd);
                long code = 0;
                ok = curl_easy_perform(c) == CURLE_OK;
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
                ok = ok && code == 200;
                curl_easy_cleanup(c);
            }
            if (fd >= 0) sceIoClose(fd);
            if (!ok) { sceIoRemove(out); busy = 0; return -1; }
        }
    }
    busy = 0;
    return ok;
}

void gamedesc_request(const char *title) {
    if (busy || !title || !*title) return;
    busy = 1;
    ready = 0;
    snprintf(want, sizeof(want), "%s", title);
    SceUID t = sceKernelCreateThread("game_desc", fetch, 0x10000100, 0x8000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) busy = 0;
}

const char *gamedesc_result(const char *title) {
    if (!ready || strcmp(got_for, title)) return NULL;
    return result;
}
