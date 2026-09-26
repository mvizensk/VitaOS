/* The weather widget, from Open-Meteo (free, no key), refreshed every 30
 * minutes on its own thread, for the town picked in Settings > Weather
 * location (user/weather.cfg: "lat lon F|C name"). No town, no widget. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include "weather.h"

#define CFG "ux0:data/arcadehub/user/weather.cfg"
#define URL "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f" \
            "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min" \
            "&temperature_unit=%s&timezone=auto&forecast_days=1"

static char place[64];
static volatile int stale;                            /* the town changed: fetch now */

/* The town, from user/weather.cfg; 0 when none is set. */
static int read_cfg(float *lat, float *lon, int *fahrenheit) {
    char b[160] = {0};
    SceUID fd = sceIoOpen(CFG, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    char unit = 'F';
    int n = 0;
    if (sscanf(b, "%f %f %c %n", lat, lon, &unit, &n) < 3) return 0;
    *fahrenheit = unit != 'C';
    snprintf(place, sizeof(place), "%s", b + n);
    for (char *q = place; *q; ++q) if (*q == '\n' || *q == '\r') *q = 0;
    return 1;
}

static volatile int ready, temp, hi, lo, code;
static char body[2048];
static int used;

static size_t on_data(char *p, size_t s, size_t n, void *u) {
    (void)u;
    size_t k = s * n;
    if (used + k >= sizeof(body)) k = sizeof(body) - 1 - used;
    memcpy(body + used, p, k);
    used += k;
    body[used] = 0;
    return s * n;
}

/* The number after "key": inside the object that starts at 'section'. */
static float number_after(const char *section, const char *key) {
    const char *s = strstr(body, section);
    if (!s) return -999;
    const char *k = strstr(s, key);
    if (!k) return -999;
    k = strchr(k, ':');
    while (k && (*k == ':' || *k == '[' || *k == ' ')) ++k;
    return k ? strtof(k, NULL) : -999;
}

static int worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        float lat, lon;
        int f = 1;
        stale = 0;
        if (!read_cfg(&lat, &lon, &f)) {                 /* no town yet: nothing to show */
            ready = 0;
            for (int i = 0; i < 60 && !stale; ++i) sceKernelDelayThread(1000 * 1000);
            continue;
        }
        char url[320];
        snprintf(url, sizeof(url), URL, lat, lon, f ? "fahrenheit" : "celsius");
        CURL *c = curl_easy_init();
        if (c) {
            used = 0; body[0] = 0;
            curl_easy_setopt(c, CURLOPT_URL, url);
            curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
            curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaOS/1.0 (PS Vita)");
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
            if (curl_easy_perform(c) == CURLE_OK) {
                float t = number_after("\"current\":{", "\"temperature_2m\""), w = number_after("\"current\":{", "\"weather_code\"");
                float h = number_after("\"daily\":{", "\"temperature_2m_max\""), l = number_after("\"daily\":{", "\"temperature_2m_min\"");
                if (t > -200) {
                    temp = (int)(t + (t < 0 ? -0.5f : 0.5f)); code = (int)w;
                    hi = (int)(h + 0.5f); lo = (int)(l + 0.5f);
                    ready = 1;
                }
            }
            curl_easy_cleanup(c);
        }
        int wait = ready ? 30 * 60 : 60;                 /* retry sooner if it failed */
        for (int i = 0; i < wait && !stale; ++i) sceKernelDelayThread(1000 * 1000);
    }
    return 0;
}

void weather_init(void) {
    SceUID t = sceKernelCreateThread("weather", worker, 0x10000100, 0x8000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* WMO weather codes, in words. */
static const char *words(int c) {
    if (c == 0) return "Clear";
    if (c <= 2) return "Partly cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return c >= 65 ? "Heavy rain" : "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Showers";
    if (c >= 85 && c <= 86) return "Snow showers";
    if (c >= 95) return "Thunderstorms";
    return "";
}

int weather_line(char *out, int max, char *sub, int submax, int *kind) {
    if (!ready) return 0;
    *kind = code == 0 ? 0 : code <= 3 || code == 45 || code == 48 ? 1 : (code >= 71 && code <= 77) || code == 85 || code == 86 ? 3 : 2;
    snprintf(out, max, "%d\xC2\xB0  %s", temp, words(code));
    snprintf(sub, submax, "%s  \xC2\xB7  H %d\xC2\xB0  L %d\xC2\xB0", place, hi, lo);
    return 1;
}

/* Settings > Weather location: search Open-Meteo's geocoder, then save.
 * Returns the number of matches written to names/coords (up to max). */
static char sbody[16384];
static int sused;
static size_t on_search(char *p, size_t s, size_t n, void *u) {
    (void)u;
    size_t k = s * n;
    if (sused + k >= sizeof(sbody)) k = sizeof(sbody) - 1 - sused;
    memcpy(sbody + sused, p, k);
    sused += k;
    sbody[sused] = 0;
    return s * n;
}

int weather_search(const char *query, char names[][96], float *lat, float *lon, int *us, int max) {
    char q[160];
    int n = 0;
    for (const char *p = query; *p && n < (int)sizeof(q) - 4; ++p) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) q[n++] = ch;
        else n += snprintf(q + n, sizeof(q) - n, "%%%02X", ch);
    }
    q[n] = 0;
    char url[256];
    snprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?count=%d&language=en&name=%s", max, q);
    CURL *c = curl_easy_init();
    if (!c) return 0;
    sused = 0; sbody[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, "app0:assets/cacert.pem");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "VitaOS/1.0 (PS Vita)");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_search);
    int ok = curl_easy_perform(c) == CURLE_OK;
    curl_easy_cleanup(c);
    if (!ok) return -1;
    int k = 0;
    for (const char *r = strstr(sbody, "\"name\""); r && k < max; r = strstr(r + 6, "\"name\"")) {
        char name[48] = "", admin[48] = "", country[48] = "";
        const char *end = strchr(r, '}');
        sscanf(r, "\"name\":\"%47[^\"]", name);
        const char *a = strstr(r, "\"admin1\":\""), *co = strstr(r, "\"country\":\""), *cc = strstr(r, "\"country_code\":\"");
        const char *la = strstr(r, "\"latitude\":"), *lo2 = strstr(r, "\"longitude\":");
        if (a && end && a < end) sscanf(a, "\"admin1\":\"%47[^\"]", admin);
        if (co && end && co < end) sscanf(co, "\"country\":\"%47[^\"]", country);
        if (!la || !lo2) break;
        lat[k] = strtof(la + 11, NULL);
        lon[k] = strtof(lo2 + 12, NULL);
        us[k] = cc && end && cc < end && !strncmp(cc + 16, "US", 2);
        snprintf(names[k], 96, "%s%s%s%s%s", name, *admin ? ", " : "", admin, *country ? ", " : "", country);
        ++k;
    }
    return k;
}

void weather_set(const char *name, float lat, float lon, int fahrenheit) {
    char line[160];
    char shortname[64];
    snprintf(shortname, sizeof(shortname), "%s", name);
    char *comma = strchr(shortname, ',');
    if (comma) *comma = 0;                            /* the widget shows the town only */
    int n = snprintf(line, sizeof(line), "%.4f %.4f %c %s\n", lat, lon, fahrenheit ? 'F' : 'C', shortname);
    SceUID fd = sceIoOpen(CFG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
    ready = 0;
    stale = 1;
}

void weather_off(void) { sceIoRemove(CFG); ready = 0; stale = 1; }
const char *weather_place(void) { return place; }
