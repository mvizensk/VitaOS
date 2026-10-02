/* Send a port's game files from a computer over Wi-Fi (2026-10-02). From a
 * Store page that still needs files, VitaOS shows an address (and a QR code
 * for a phone); the page there takes a folder or files and sends each one
 * with a PUT, straight into the folder the port reads, for example
 * ux0:data/Mania. Only while this screen is open, only on the local network,
 * only into that one folder ("..", absolute paths and drive names are
 * refused). One request at a time; files of any size are streamed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include "drop.h"
#include "qrcodegen.h"

#define PORT 8080
static volatile int active, stop, files_done, failed;
static volatile long long bytes_done, cur_size, cur_got;
static char target[160], title[96], cur_name[160], ip[20], url[64];
static SceUID thread = -1;
static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
static int qr_ok;

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>VitaOS: send files</title><style>body{font:16px system-ui;background:#151821;color:#eceff4;max-width:640px;margin:40px auto;padding:0 16px}"
"button,label{display:inline-block;background:#3b82f6;color:#fff;border:0;border-radius:8px;padding:10px 16px;margin:6px 6px 6px 0;cursor:pointer}"
"input{display:none}#log{white-space:pre-wrap;color:#9aa3b2;margin-top:16px}progress{width:100%%}</style>"
"<h2>Send files to %s</h2><p>They go into <b>%s</b> on the Vita. Choose the folder that holds the game files: its contents land there.</p>"
"<label>Choose a folder<input id=d type=file webkitdirectory multiple></label><label>Choose files<input id=f type=file multiple></label>"
"<p><progress id=p value=0 max=1></progress></p><div id=log></div><script>"
"const log=t=>document.getElementById('log').textContent=t;"
"async function send(list,strip){let total=0,done=0;for(const f of list)total+=f.size;let n=0;"
"for(const f of list){let rel=f.webkitRelativePath||f.name;if(strip){rel=rel.split('/').slice(1).join('/')}"
"log('Sending '+rel+' ('+(++n)+' of '+list.length+')');"
"const r=await fetch('/up?path='+encodeURIComponent(rel),{method:'PUT',body:f});"
"if(!r.ok){log('Failed: '+rel+' ('+r.status+')');return}done+=f.size;document.getElementById('p').value=done/total}"
"log('Done: '+list.length+' files. You can close this page.')}"
"document.getElementById('d').onchange=e=>send([...e.target.files],true);"
"document.getElementById('f').onchange=e=>send([...e.target.files],false);</script>";

static int send_all(int s, const char *b, int n) {
    while (n > 0) { int w = sceNetSend(s, b, n, 0); if (w <= 0) return -1; b += w; n -= w; }
    return 0;
}

static void reply(int s, int code, const char *type, const char *body, int len) {
    char h[192];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                     code, code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error", type, len);
    send_all(s, h, n);
    if (len) send_all(s, body, len);
}

static int unhex(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* The relative path from "?path=", decoded and checked: no "..", no drive, no leading slash. */
static int safe_path(const char *q, char *out, int max) {
    const char *p = strstr(q, "path=");
    if (!p) return 0;
    p += 5;
    int n = 0;
    for (; *p && *p != '&' && *p != ' ' && n < max - 1; ++p) {
        if (*p == '%' && unhex(p[1]) >= 0 && unhex(p[2]) >= 0) { out[n++] = (char)(unhex(p[1]) * 16 + unhex(p[2])); p += 2; }
        else if (*p == '+') out[n++] = ' ';
        else out[n++] = *p;
    }
    out[n] = 0;
    if (!n || out[0] == '/' || strchr(out, ':') || strchr(out, '\\')) return 0;
    for (const char *s = out; (s = strstr(s, "..")) != NULL; ++s)
        if ((s == out || s[-1] == '/') && (s[2] == '/' || !s[2])) return 0;
    return 1;
}

static void mkdirs(const char *path) {                 /* every folder above the file */
    char p[320];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 5; *s; ++s)
        if (*s == '/') { *s = 0; sceIoMkdir(p, 0777); *s = '/'; }
}

static void serve(int s) {
    static char buf[256 * 1024];
    int got = 0, head_end = -1;
    while (got < 8192 && head_end < 0) {
        int r = sceNetRecv(s, buf + got, 8192 - got, 0);
        if (r <= 0) return;
        got += r;
        buf[got] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e) head_end = (int)(e - buf) + 4;
    }
    if (head_end < 0) { reply(s, 400, "text/plain", "", 0); return; }
    if (!strncmp(buf, "GET / ", 6) || !strncmp(buf, "GET /?", 6)) {
        static char page[sizeof(PAGE) + 400];
        int n = snprintf(page, sizeof(page), PAGE, title, target);
        reply(s, 200, "text/html; charset=utf-8", page, n);
        return;
    }
    if (strncmp(buf, "PUT /up?", 8)) { reply(s, 404, "text/plain", "", 0); return; }
    char rel[200], full[400];
    char line[300];
    snprintf(line, sizeof(line), "%.*s", (int)(strchr(buf, '\r') - buf), buf);
    if (!safe_path(line, rel, sizeof(rel))) { reply(s, 400, "text/plain", "bad path", 8); return; }
    const char *cl = NULL;
    for (char *h = buf; h < buf + head_end; ++h)
        if ((*h | 32) == 'c' && !strncasecmp(h, "content-length:", 15)) { cl = h + 15; break; }
    long long len = cl ? atoll(cl) : -1;
    if (len < 0) { reply(s, 400, "text/plain", "no length", 9); return; }
    snprintf(full, sizeof(full), "%s/%s", target, rel);
    mkdirs(full);
    SceUID fd = sceIoOpen(full, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { failed++; reply(s, 500, "text/plain", "cannot write", 12); return; }
    snprintf(cur_name, sizeof(cur_name), "%s", rel);
    cur_size = len; cur_got = 0;
    int extra = got - head_end;
    if (extra > 0) { sceIoWrite(fd, buf + head_end, extra); cur_got += extra; bytes_done += extra; }
    int ok = 1;
    while (cur_got < len && !stop) {
        int want = len - cur_got > (long long)sizeof(buf) ? (int)sizeof(buf) : (int)(len - cur_got);
        int r = sceNetRecv(s, buf, want, 0);
        if (r <= 0) { ok = 0; break; }
        if (sceIoWrite(fd, buf, r) != r) { ok = 0; break; }
        cur_got += r; bytes_done += r;
    }
    sceIoClose(fd);
    if (!ok || cur_got < len) { sceIoRemove(full); failed++; reply(s, 500, "text/plain", "incomplete", 10); return; }
    files_done++;
    reply(s, 200, "text/plain", "ok", 2);
}

static int server(SceSize args, void *argp) {
    (void)args; (void)argp;
    int ls = sceNetSocket("vitaos_drop", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (ls < 0) { thread = -1; return sceKernelExitDeleteThread(0); }
    int one = 1;
    sceNetSetsockopt(ls, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons(PORT);
    a.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    if (sceNetBind(ls, (SceNetSockaddr *)&a, sizeof(a)) < 0 || sceNetListen(ls, 2) < 0) {
        sceNetSocketClose(ls); thread = -1; return sceKernelExitDeleteThread(0);
    }
    int tmo = 500 * 1000;                              /* accept wakes twice a second to see `stop` */
    sceNetSetsockopt(ls, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
    while (!stop) {
        SceNetSockaddrIn peer;
        unsigned int pl = sizeof(peer);
        int c = sceNetAccept(ls, (SceNetSockaddr *)&peer, &pl);
        if (c < 0) continue;
        int t = 30 * 1000 * 1000;
        sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &t, sizeof(t));
        serve(c);
        sceNetSocketClose(c);
    }
    sceNetSocketClose(ls);
    thread = -1;
    return sceKernelExitDeleteThread(0);
}

void drop_open(const char *dir, const char *app_title) {
    if (active || strncmp(dir, "ux0:", 4)) return;      /* the memory card only, never the system partitions */
    snprintf(target, sizeof(target), "%s", dir);
    snprintf(title, sizeof(title), "%s", app_title);
    sceIoMkdir(target, 0777);
    SceNetCtlInfo info;
    ip[0] = 0;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) snprintf(ip, sizeof(ip), "%s", info.ip_address);
    snprintf(url, sizeof(url), "http://%s:%d", ip, PORT);
    static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    qr_ok = ip[0] && qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);
    files_done = failed = 0; bytes_done = cur_size = cur_got = 0; cur_name[0] = 0;
    stop = 0;
    active = 1;
    if (thread < 0 && ip[0]) {
        thread = sceKernelCreateThread("vitaos_drop", server, 0x10000100, 0x10000, 0, 0, NULL);
        if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
    }
}

int drop_active(void) { return active; }
const char *drop_hint(void) { return "O close (stops receiving)"; }

void drop_update(const Input *in) {
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);   /* no sleep mid-transfer */
    if (in->pressed & SCE_CTRL_CIRCLE) { stop = 1; active = 0; return; }
    vita2d_draw_rectangle(0, 0, W, H, RGBA8(21, 24, 33, 255));
    text(bold, 50, 100, C_TEXT, 24, "Send files from a computer");
    char line[240];
    snprintf(line, sizeof(line), "For %s, into %s", title, target);
    text_fit(font, 50, 132, C_DIM, 16, line, 520);
    if (!ip[0]) { text(font, 50, 200, C_BAD, 18, "Connect to Wi-Fi first."); return; }
    draw_wrapped_text("On a computer or phone on the same Wi-Fi, open this address in a web browser, then choose the "
                      "folder that holds the game files:", 50, 180, 520, 16, 4, C_DIM);
    text(bold, 50, 270, C_ACCENT, 30, url);
    if (qr_ok) {
        int size = qrcodegen_getSize(qr), cell = 8, qx = W - 60 - size * cell, qy = 110;
        draw_round_rect(qx - 14, qy - 14, size * cell + 28, size * cell + 28, 10, RGBA8(255, 255, 255, 255));
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (qrcodegen_getModule(qr, x, y)) vita2d_draw_rectangle(qx + x * cell, qy + y * cell, cell, cell, RGBA8(0, 0, 0, 255));
    }
    if (cur_name[0] && cur_got < cur_size) {
        snprintf(line, sizeof(line), "Receiving %s", cur_name);
        text_fit(font, 50, 340, C_TEXT, 16, line, 520);
        draw_bar(50, 352, 520, 6, cur_size ? (float)cur_got / cur_size : 0, C_ACCENT);
    } else if (files_done || failed) {
        snprintf(line, sizeof(line), "%d file%s received (%.1f MB)%s", files_done, files_done == 1 ? "" : "s",
                 bytes_done / 1048576.0, failed ? ", some failed: send them again" : "");
        text_fit(font, 50, 340, failed ? C_BAD : C_OK, 16, line, 520);
    } else text(font, 50, 340, C_FAINT, 16, "Waiting for your computer...");
}
