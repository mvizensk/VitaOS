/* fileops: native file management for vabridge. See fileops.h for the
 * contract. Everything here runs inside SceShell, so it must never take the
 * shell down: every handle is closed on every path, buffers come from one
 * memblock per operation and go back when it ends, and recursion depth is
 * capped so a deep tree cannot overflow the job thread's stack. */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/net/net.h>
#include <psp2/kernel/processmgr.h>

#include "fileops.h"
#include "sha256.h"
#include "fetch.h"

#define ERR_EXIST   ((int)0x80010011)   /* SCE_ERROR_ERRNO_EEXIST */
#define ERR_POLICY  ((int)0x80AB0001)   /* refused by fo_can_* */
#define ERR_BUSY    ((int)0x80AB0002)   /* a job is already running */
#define ERR_NOMEM   ((int)0x80AB0003)   /* no transfer buffer */
#define ERR_DEPTH   ((int)0x80AB0004)   /* tree deeper than MAX_DEPTH */
#define ERR_LONG    ((int)0x80AB0005)   /* path would exceed FO_PATH_MAX */
#define ERR_CANCEL  ((int)0x80AB0006)
#define ERR_SHORT   ((int)0x80AB0007)   /* short write: card full */
#define ERR_EOF     ((int)0x80AB0008)   /* client hung up mid-upload */
#define ERR_ARG     ((int)0x80AB0009)   /* e.g. copying a folder into itself */
#define ERR_ISDIR   ((int)0x80AB000A)

#define MAX_DEPTH 24                  /* ~500 B of stack per level */
#define NET_TIMEOUT_US (20 * 1000 * 1000)

/* Auto-suspend mid-transfer killed a 2 GB on-device copy with EIO (and took
 * Wi-Fi with it), so every long loop holds the console awake. Cheap enough
 * to call per block; the system only needs a tick every few seconds. */
static void hold_awake(void) { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND); }

/* ---------- paths and policy ---------- */

static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int slen(const char *s) { return (int)sceClibStrnlen(s, FO_PATH_MAX + 1); }

/* Length of the device name before ':', or -1. */
static int dev_len(const char *p) {
    int i = 0;
    while (p[i] && p[i] != ':') { if (p[i] == '/') return -1; ++i; }
    return p[i] == ':' && i > 0 ? i : -1;
}

/* The part after "dev:", with leading slashes dropped: "ux0:/data" -> "data". */
static const char *rest_of(const char *p) {
    p += dev_len(p) + 1;
    while (*p == '/') ++p;
    return p;
}

static int starts_ci(const char *s, const char *pre) {
    for (; *pre; ++s, ++pre) if (lower(*s) != lower(*pre)) return 0;
    return 1;
}

/* s names pre itself, or something under it. */
static int under_ci(const char *s, const char *pre) {
    int n = slen(pre);
    return starts_ci(s, pre) && (s[n] == 0 || s[n] == '/');
}

static int same_device(const char *a, const char *b) {
    int n = dev_len(a);
    if (n < 0 || n != dev_len(b)) return 0;
    for (int i = 0; i < n; ++i) if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}

/* a is b or somewhere under it, however either is spelled ("ux0:/x", "ux0:x"). */
static int inside(const char *a, const char *b) {
    const char *rb = rest_of(b);
    return same_device(a, b) && (!*rb || under_ci(rest_of(a), rb));
}

int fo_can_read(const char *p) {
    if (!p || dev_len(p) < 0 || slen(p) >= FO_PATH_MAX - 8) return ERR_POLICY;
    for (const char *q = p; *q; ++q)
        if (q[0] == '.' && q[1] == '.' && (q == p || q[-1] == '/' || q[-1] == ':') &&
            (q[2] == 0 || q[2] == '/'))
            return ERR_POLICY;
    return 0;
}

int fo_can_write(const char *p) {
    static const char *const devices[] = {"ux0:", "ur0:", "uma0:", "imc0:", "grw0:", "xmc0:"};
    if (fo_can_read(p) < 0) return ERR_POLICY;
    int ok = 0;
    for (unsigned int i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) ok |= starts_ci(p, devices[i]);
    if (!ok) return ERR_POLICY;
    /* tai/config.txt decides what loads at boot; a bad write there bricks the
     * boot chain. Edits go through FTP with a backup, by hand. */
    if (under_ci(rest_of(p), "tai")) return ERR_POLICY;
    return 0;
}

int fo_can_remove(const char *p) {
    if (fo_can_write(p) < 0) return ERR_POLICY;
    const char *r = rest_of(p);
    int parts = 0, in = 0;
    for (; *r; ++r) {
        if (*r != '/' && !in) { ++parts; in = 1; }
        else if (*r == '/') in = 0;
    }
    if (parts == 0) return ERR_POLICY;              /* never a device root */
    if (parts == 1) {                               /* nor a top-level folder like ux0:app; */
        SceIoStat st;                               /* a loose file up there is fine */
        if (sceIoGetstat(p, &st) < 0 || SCE_S_ISDIR(st.st_mode)) return ERR_POLICY;
    }
    if (under_ci(rest_of(p), "data/vita-agent") ||                   /* the remote itself, and */
        under_ci(rest_of(p), "data/vita-agent-bridge")) return ERR_POLICY; /* the public build's token */
    return 0;
}

/* Appends "/name" (or just name after "dev:" or a trailing '/'); returns the
 * new length or ERR_LONG. The caller truncates back with path[len] = 0. */
static int append(char *path, int len, const char *name) {
    int n = slen(name), sep = len && path[len - 1] != ':' && path[len - 1] != '/';
    if (len + sep + n >= FO_PATH_MAX) return ERR_LONG;
    if (sep) path[len++] = '/';
    sceClibMemcpy(path + len, name, n);
    path[len + n] = 0;
    return len + n;
}

static int is_dot(const char *n) { return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)); }

static void copy_str(char *dst, const char *src, int max) {
    int n = slen(src);
    if (n >= max) n = max - 1;
    sceClibMemcpy(dst, src, n);
    dst[n] = 0;
}

/* ---------- transfer buffers ---------- */

typedef struct { SceUID uid; unsigned char *p; unsigned int size; } IoBuf;

/* Large chunks are what make the SD card fast; the Wi-Fi is the slower link
 * either way. SceShell's budget is small, so step down rather than fail. */
static int iobuf_get_from(IoBuf *b, unsigned int first) {
    static const unsigned int sizes[] = {512 * 1024, 256 * 1024, 128 * 1024, 64 * 1024};
    b->uid = -1;
    for (unsigned int i = first; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        SceUID u = sceKernelAllocMemBlock("vabridge_io", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sizes[i], NULL);
        if (u < 0) continue;
        void *base = NULL;
        if (sceKernelGetMemBlockBase(u, &base) < 0 || !base) { sceKernelFreeMemBlock(u); continue; }
        b->uid = u; b->p = base; b->size = sizes[i];
        return 0;
    }
    return ERR_NOMEM;
}

/* Jobs start at 512 KB (card to card, where size pays). Streams start at
 * 256 KB: the Wi-Fi is the limit there, and three concurrent 512 KB buffers
 * were more than SceShell could give (third upload failed, 2026-09-24). */
static int iobuf_get(IoBuf *b) { return iobuf_get_from(b, 0); }
static int iobuf_get_stream(IoBuf *b) { return iobuf_get_from(b, 1); }

static void iobuf_put(IoBuf *b) {
    if (b->uid >= 0) sceKernelFreeMemBlock(b->uid);
    b->uid = -1;
}

/* ---------- synchronous ops ---------- */

int fo_mkdirs(const char *path) {
    if (fo_can_write(path) < 0) return ERR_POLICY;
    char tmp[FO_PATH_MAX];
    copy_str(tmp, path, sizeof(tmp));
    int start = dev_len(tmp) + 1, n = slen(tmp);
    while (n > start && tmp[n - 1] == '/') tmp[--n] = 0;
    for (int i = start + 1; i <= n; ++i) {
        if (tmp[i] != '/' && tmp[i] != 0) continue;
        char c = tmp[i];
        tmp[i] = 0;
        int rc = sceIoMkdir(tmp, 0777);
        tmp[i] = c;
        if (rc < 0 && rc != ERR_EXIST && i == n) return rc;
    }
    SceIoStat st;
    int rc = sceIoGetstat(tmp, &st);
    return rc < 0 ? rc : (SCE_S_ISDIR(st.st_mode) ? 0 : ERR_EXIST);
}

/* mkdir -p of the folder a file will land in. */
static void make_parent(const char *path) {
    char parent[FO_PATH_MAX];
    copy_str(parent, path, sizeof(parent));
    int cut = slen(parent), root = dev_len(parent) + 1;
    while (cut > root && parent[cut - 1] != '/') --cut;
    if (cut > root) { parent[cut - 1] = 0; fo_mkdirs(parent); }
}

int fo_stat_line(const char *path, char *out, int max) {
    SceIoStat st;
    int rc = fo_can_read(path) < 0 ? ERR_POLICY : sceIoGetstat(path, &st);
    if (rc < 0) { sceClibSnprintf(out, max, "ERR stat rc=0x%08X\n", rc); return rc; }
    SceDateTime *t = &st.st_mtime;
    sceClibSnprintf(out, max, "OK stat %c %llu %04d-%02d-%02dT%02d:%02d:%02d\n",
                    SCE_S_ISDIR(st.st_mode) ? 'd' : 'f', (unsigned long long)st.st_size,
                    t->year, t->month, t->day, t->hour, t->minute, t->second);
    return 0;
}

/* ---------- streaming over the client's socket ---------- */

/* The bridge's sockets are non-blocking with a 5-10 ms poll, which caps a
 * transfer well below the Wi-Fi. Streams switch to blocking with a timeout
 * and big socket buffers instead. */
static void socket_for_streaming(int s) {
    /* 128 KB, not more: three streams with 256 KB each way exhausted
     * SceShell's network pool (socket=0x80410137, ENOBUFS) and the Wi-Fi
     * dropped for five minutes (2026-09-24). */
    int zero = 0, big = 128 * 1024, timeo = NET_TIMEOUT_US;
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &zero, sizeof(zero));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &big, sizeof(big));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, &big, sizeof(big));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeo, sizeof(timeo));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeo, sizeof(timeo));
}

static int send_block(int s, const unsigned char *p, unsigned int len) {
    while (len) {
        int n = sceNetSend(s, p, len, 0);
        if (n <= 0) return n < 0 ? n : ERR_EOF;
        p += n; len -= n;
    }
    return 0;
}

/* Uploads are pipelined: this thread only receives, a writer thread hashes
 * and writes the other half of a double buffer. Done in one thread, receive,
 * hash and card write add up and a single stream ran at 1.0-1.9 MB/s while two
 * streams reached 2.6 (measured 2026-09-24). */
typedef struct {
    SceUID fd, full, empty;
    unsigned char *half[2];
    unsigned int len[2];
    volatile int rc;
    Sha256 h;
} PutPipe;

static int put_writer(SceSize args, void *argp) {
    (void)args;
    PutPipe *pp = *(PutPipe **)argp;
    for (int i = 0;; i ^= 1) {
        sceKernelWaitSema(pp->full, 1, NULL);
        unsigned int n = pp->len[i];
        if (!n) break;                                /* end of stream */
        if (pp->rc >= 0) {
            sha256_update(&pp->h, pp->half[i], n);
            int w = sceIoWrite(pp->fd, pp->half[i], n);
            if (w != (int)n) pp->rc = w < 0 ? w : ERR_SHORT;
        }
        sceKernelSignalSema(pp->empty, 1);
    }
    return 0;
}

int fo_put(int s, const char *path, unsigned long long size, char *out, int max) {
    if (fo_can_write(path) < 0) { sceClibSnprintf(out, max, "ERR put: not writable\n"); return ERR_POLICY; }
    char part[FO_PATH_MAX];
    int n = slen(path);
    if (n + 6 >= FO_PATH_MAX) { sceClibSnprintf(out, max, "ERR put: path too long\n"); return ERR_LONG; }
    sceClibMemcpy(part, path, n);
    sceClibMemcpy(part + n, ".part", 6);
    make_parent(path);

    IoBuf b;
    if (iobuf_get_stream(&b) < 0) { sceClibSnprintf(out, max, "ERR put: no buffer\n"); return ERR_NOMEM; }
    static PutPipe pipes[4];          /* one per concurrent upload; see put_slot */
    PutPipe *pp = NULL;
    for (unsigned int i = 0; i < sizeof(pipes) / sizeof(pipes[0]) && !pp; ++i)
        if (__sync_bool_compare_and_swap(&pipes[i].fd, 0, -1)) pp = &pipes[i];
    if (!pp) { iobuf_put(&b); sceClibSnprintf(out, max, "ERR busy\n"); return ERR_BUSY; }
    pp->fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (pp->fd < 0) {
        int rc = pp->fd;
        pp->fd = 0;
        iobuf_put(&b);
        sceClibSnprintf(out, max, "ERR put open rc=0x%08X\n", rc);
        return rc;
    }
    unsigned int halfsz = b.size / 2;
    pp->half[0] = b.p;
    pp->half[1] = b.p + halfsz;
    pp->rc = 0;
    sha256_init(&pp->h);
    pp->full = sceKernelCreateSema("vabridge_full", 0, 0, 2, NULL);
    pp->empty = sceKernelCreateSema("vabridge_empty", 0, 2, 2, NULL);
    SceUID writer = sceKernelCreateThread("vabridge_writer", put_writer, 0x10000100, 0x2000, 0, 0, NULL);
    int rc = pp->full < 0 ? pp->full : pp->empty < 0 ? pp->empty : writer, started = 0;
    if (rc >= 0) { rc = sceKernelStartThread(writer, sizeof(pp), &pp); started = rc >= 0; }

    socket_for_streaming(s);
    unsigned long long left = size;
    for (int i = 0; rc >= 0 && left > 0; i ^= 1) {
        sceKernelWaitSema(pp->empty, 1, NULL);
        hold_awake();
        unsigned int fill = 0;
        while (fill < halfsz && left > 0) {
            unsigned int want = left < halfsz - fill ? (unsigned int)left : halfsz - fill;
            int got = sceNetRecv(s, pp->half[i] + fill, want, 0);
            if (got <= 0) { rc = got < 0 ? got : ERR_EOF; break; }
            fill += (unsigned int)got;
            left -= (unsigned int)got;
        }
        if (!fill) { sceKernelSignalSema(pp->empty, 1); break; }   /* nothing to hand over */
        pp->len[i] = fill;
        sceKernelSignalSema(pp->full, 1);
        if (pp->rc < 0) rc = pp->rc;                  /* card error: stop receiving */
    }
    if (started) {
        /* The writer hands back every half it takes, error or not, so both
         * coming back means it is idle; an empty half then ends its loop. */
        sceKernelWaitSema(pp->empty, 2, NULL);
        pp->len[0] = pp->len[1] = 0;
        sceKernelSignalSema(pp->full, 1);
        sceKernelWaitThreadEnd(writer, NULL, NULL);
    }
    if (writer >= 0) sceKernelDeleteThread(writer);
    if (pp->full >= 0) sceKernelDeleteSema(pp->full);
    if (pp->empty >= 0) sceKernelDeleteSema(pp->empty);
    if (rc >= 0 && pp->rc < 0) rc = pp->rc;
    sceIoClose(pp->fd);
    iobuf_put(&b);
    unsigned char d[32];
    sha256_final(&pp->h, d);
    pp->fd = 0;                                       /* slot free */
    if (rc < 0) {
        sceIoRemove(part);
        sceClibSnprintf(out, max, "ERR put rc=0x%08X after %llu of %llu bytes\n", rc, size - left, size);
        return rc;
    }
    sceIoRemove(path);                                /* rename will not replace */
    rc = sceIoRename(part, path);
    if (rc < 0) {
        sceIoRemove(part);
        sceClibSnprintf(out, max, "ERR put rename rc=0x%08X\n", rc);
        return rc;
    }
    char hex[65];
    sha256_hex(d, hex);
    sceClibSnprintf(out, max, "OK put %llu sha256=%s\n", size, hex);
    return 0;
}

int fo_get(int s, const char *path) {
    char line[80];
    if (fo_can_read(path) < 0) { send_block(s, (const unsigned char *)"ERR get: bad path\n", 18); return ERR_POLICY; }
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        int n = sceClibSnprintf(line, sizeof(line), "ERR get rc=0x%08X\n", fd);
        send_block(s, (const unsigned char *)line, n);
        return fd;
    }
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    IoBuf b;
    if (size < 0 || iobuf_get_stream(&b) < 0) {
        sceIoClose(fd);
        int n = sceClibSnprintf(line, sizeof(line), "ERR get rc=0x%08X\n", size < 0 ? (int)size : ERR_NOMEM);
        send_block(s, (const unsigned char *)line, n);
        return ERR_NOMEM;
    }
    socket_for_streaming(s);
    int n = sceClibSnprintf(line, sizeof(line), "OK get %llu\n", (unsigned long long)size);
    int rc = send_block(s, (const unsigned char *)line, n);
    SceOff left = size;
    while (rc >= 0 && left > 0) {
        hold_awake();
        int got = sceIoRead(fd, b.p, left > (SceOff)b.size ? (int)b.size : (int)left);
        if (got <= 0) break;             /* short file: the client sees fewer bytes than promised */
        rc = send_block(s, b.p, (unsigned int)got);
        left -= got;
    }
    sceIoClose(fd);
    iobuf_put(&b);
    return rc;
}

/* ---------- background jobs ---------- */

enum { ST_NONE, ST_RUNNING, ST_DONE, ST_FAILED, ST_CANCELLED };

static struct {
    volatile int state, kind, id, rc, phase;       /* phase 0 = sizing, 1 = working */
    volatile unsigned int files_done, files_total;
    volatile unsigned long long bytes_done, bytes_total;
    volatile int cancel;
    char src[FO_PATH_MAX], dst[FO_PATH_MAX];
    char note[80];                                 /* sha256 hex, or where it failed */
    char sha[68];                                  /* fetch: expected digest, "" = none */
} job;
static FetchState fetch_st;
static char pending_sha[68];

static void note_path(const char *p) {
    int n = slen(p), from = n > 70 ? n - 70 : 0;
    copy_str(job.note, p + from, sizeof(job.note));
}

static int size_tree(char *path, int len, int depth) {
    SceIoStat st;
    int rc = sceIoGetstat(path, &st);
    if (rc < 0) { note_path(path); return rc; }
    if (!SCE_S_ISDIR(st.st_mode)) { job.files_total++; job.bytes_total += st.st_size; return 0; }
    if (depth > MAX_DEPTH) { note_path(path); return ERR_DEPTH; }
    SceUID d = sceIoDopen(path);
    if (d < 0) { note_path(path); return d; }
    SceIoDirent e;
    while (!job.cancel) {
        sceClibMemset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (is_dot(e.d_name)) continue;
        int nl = append(path, len, e.d_name);
        if (nl < 0) { note_path(path); rc = nl; path[len] = 0; break; }
        rc = size_tree(path, nl, depth + 1);
        path[len] = 0;
        if (rc < 0) break;
    }
    sceIoDclose(d);
    return job.cancel ? ERR_CANCEL : rc;
}

static int copy_file(const char *src, const char *dst, IoBuf *b) {
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0) { note_path(src); return in; }
    SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) { sceIoClose(in); note_path(dst); return out; }
    int rc = 0;
    for (;;) {
        if (job.cancel) { rc = ERR_CANCEL; break; }
        hold_awake();
        int n = sceIoRead(in, b->p, b->size);
        if (n < 0) { rc = n; note_path(src); break; }
        if (n == 0) break;
        int w = sceIoWrite(out, b->p, n);
        if (w != n) { rc = w < 0 ? w : ERR_SHORT; note_path(dst); break; }
        job.bytes_done += (unsigned int)n;
    }
    sceIoClose(in);
    sceIoClose(out);
    if (rc < 0) sceIoRemove(dst);                  /* never leave half a file behind */
    else job.files_done++;
    return rc;
}

static int copy_tree(char *src, int sl, char *dst, int dl, int depth, IoBuf *b) {
    SceIoStat st;
    int rc = sceIoGetstat(src, &st);
    if (rc < 0) { note_path(src); return rc; }
    if (!SCE_S_ISDIR(st.st_mode)) return copy_file(src, dst, b);
    if (depth > MAX_DEPTH) { note_path(src); return ERR_DEPTH; }
    rc = sceIoMkdir(dst, 0777);
    if (rc < 0 && rc != ERR_EXIST) { note_path(dst); return rc; }
    SceUID d = sceIoDopen(src);
    if (d < 0) { note_path(src); return d; }
    SceIoDirent e;
    rc = 0;
    while (!job.cancel) {
        sceClibMemset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        if (is_dot(e.d_name)) continue;
        int ns = append(src, sl, e.d_name), nd = append(dst, dl, e.d_name);
        if (ns < 0 || nd < 0) { note_path(src); rc = ERR_LONG; }
        else rc = copy_tree(src, ns, dst, nd, depth + 1, b);
        src[sl] = 0;
        dst[dl] = 0;
        if (rc < 0) break;
    }
    sceIoDclose(d);
    return job.cancel ? ERR_CANCEL : rc;
}

/* Deleting while a directory is open for reading can skip entries, so reread
 * it until a pass finds nothing left. */
static int remove_tree(char *path, int len, int depth) {
    SceIoStat st;
    int rc = sceIoGetstat(path, &st);
    if (rc < 0) { note_path(path); return rc; }
    if (!SCE_S_ISDIR(st.st_mode)) {
        rc = sceIoRemove(path);
        if (rc < 0) note_path(path);
        else { job.files_done++; job.bytes_done += st.st_size; }
        return rc;
    }
    if (depth > MAX_DEPTH) { note_path(path); return ERR_DEPTH; }
    for (int pass = 0; pass < 64 && rc >= 0 && !job.cancel; ++pass) {
        SceUID d = sceIoDopen(path);
        if (d < 0) { note_path(path); return d; }
        SceIoDirent e;
        int seen = 0;
        while (!job.cancel) {
            sceClibMemset(&e, 0, sizeof(e));
            if (sceIoDread(d, &e) <= 0) break;
            if (is_dot(e.d_name)) continue;
            ++seen;
            int nl = append(path, len, e.d_name);
            rc = nl < 0 ? nl : remove_tree(path, nl, depth + 1);
            path[len] = 0;
            if (rc < 0) break;
        }
        sceIoDclose(d);
        if (!seen) break;
    }
    if (job.cancel) return ERR_CANCEL;
    if (rc < 0) return rc;
    rc = sceIoRmdir(path);
    if (rc < 0) note_path(path);
    return rc;
}

static int hash_file(const char *path, IoBuf *b) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { note_path(path); return fd; }
    Sha256 h;
    sha256_init(&h);
    int rc = 0;
    for (;;) {
        if (job.cancel) { rc = ERR_CANCEL; break; }
        hold_awake();
        int n = sceIoRead(fd, b->p, b->size);
        if (n < 0) { rc = n; break; }
        if (n == 0) break;
        sha256_update(&h, b->p, (unsigned int)n);
        job.bytes_done += (unsigned int)n;
    }
    sceIoClose(fd);
    if (rc >= 0) {
        unsigned char d[32];
        char hex[65];
        sha256_final(&h, d);
        sha256_hex(d, hex);
        copy_str(job.note, hex, sizeof(job.note));
        job.files_done = 1;
    }
    return rc;
}

static int job_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    static char src[FO_PATH_MAX], dst[FO_PATH_MAX];   /* one job at a time */
    copy_str(src, job.src, sizeof(src));
    copy_str(dst, job.dst, sizeof(dst));
    int sl = slen(src), dl = slen(dst), rc = 0;
    IoBuf b = {-1, NULL, 0};

    if (job.kind == FO_JOB_FETCH) {
        job.phase = 1;
        rc = fetch_init(0);                            /* SceShell's pools: never term them */
        make_parent(dst);
        if (rc >= 0) rc = iobuf_get_stream(&b);
        if (rc >= 0) {
            unsigned char d[32];
            rc = fetch_to_file(src, dst, job.sha[0] ? job.sha : NULL, b.p, b.size, &fetch_st, d);
            if (rc >= 0) {
                char hex[65];
                sha256_hex(d, hex);
                copy_str(job.note, hex, sizeof(job.note));
                job.files_done = 1;
            } else if (fetch_st.http_status && fetch_st.http_status != 200) {
                sceClibSnprintf(job.note, sizeof(job.note), "http-%d", fetch_st.http_status);
            } else if (fetch_st.tls_errors) {
                sceClibSnprintf(job.note, sizeof(job.note), "tls-errors-0x%02X", fetch_st.tls_errors);
            }
        }
    } else if (job.kind == FO_JOB_MOVE && same_device(src, dst)) {
        /* Same partition: a rename is instant, whatever the size. */
        job.phase = 1;
        rc = sceIoRename(src, dst);
        if (rc < 0) note_path(src);
        else job.files_done = job.files_total = 1;
    } else {
        rc = size_tree(src, sl, 0);
        job.phase = 1;
        if (rc >= 0 && job.kind == FO_JOB_SHA256 && job.files_total != 1) rc = ERR_ISDIR;
        if (rc >= 0 && job.kind != FO_JOB_REMOVE && iobuf_get(&b) < 0) rc = ERR_NOMEM;
        if (rc >= 0) {
            switch (job.kind) {
            case FO_JOB_COPY: rc = copy_tree(src, sl, dst, dl, 0, &b); break;
            case FO_JOB_REMOVE: rc = remove_tree(src, sl, 0); break;
            case FO_JOB_SHA256: rc = hash_file(src, &b); break;
            case FO_JOB_MOVE:   /* across partitions: copy, and delete only if all of it landed */
                rc = copy_tree(src, sl, dst, dl, 0, &b);
                if (rc >= 0) { job.files_done = 0; job.bytes_done = 0; rc = remove_tree(src, sl, 0); }
                break;
            }
        }
    }
    iobuf_put(&b);
    job.rc = rc;
    job.state = job.cancel ? ST_CANCELLED : rc < 0 ? ST_FAILED : ST_DONE;
    return sceKernelExitDeleteThread(0);
}

int fo_job_start(int kind, const char *src, const char *dst) {
    if (job.state == ST_RUNNING) return ERR_BUSY;
    int ok;
    switch (kind) {
    case FO_JOB_COPY:   ok = fo_can_read(src) >= 0 && fo_can_write(dst) >= 0; break;
    case FO_JOB_MOVE:   ok = fo_can_remove(src) >= 0 && fo_can_write(dst) >= 0; break;
    case FO_JOB_REMOVE: ok = fo_can_remove(src) >= 0; break;
    case FO_JOB_SHA256: ok = fo_can_read(src) >= 0; break;
    case FO_JOB_FETCH:
        ok = src && (!sceClibStrncmp(src, "http://", 7) || !sceClibStrncmp(src, "https://", 8)) &&
             slen(src) < FO_PATH_MAX - 8 && fo_can_write(dst) >= 0;
        break;
    default: return ERR_ARG;
    }
    if (!ok) return ERR_POLICY;
    copy_str(job.sha, kind == FO_JOB_FETCH ? pending_sha : "", sizeof(job.sha));
    if ((kind == FO_JOB_COPY || kind == FO_JOB_MOVE) && (inside(dst, src) || inside(src, dst)))
        return ERR_ARG;                                    /* a folder into itself, or over its parent */
    copy_str(job.src, src, sizeof(job.src));
    copy_str(job.dst, dst ? dst : "", sizeof(job.dst));
    job.kind = kind;
    job.rc = 0;
    job.phase = 0;
    job.files_done = job.files_total = 0;
    job.bytes_done = job.bytes_total = 0;
    job.cancel = 0;
    fetch_st.cancel = 0;
    job.note[0] = 0;
    job.id++;
    job.state = ST_RUNNING;
    /* Below the bridge's own priority, so status and screenshots stay quick
     * while a long copy runs. */
    SceUID t = sceKernelCreateThread("vabridge_job", job_main, 0x10000110, 0x10000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
        if (t >= 0) sceKernelDeleteThread(t);
        job.rc = t < 0 ? t : ERR_NOMEM;
        job.state = ST_FAILED;
        return job.rc;
    }
    return job.id;
}

int fo_job_start_fetch(const char *url, const char *dst, const char *expect_sha) {
    if (expect_sha && expect_sha[0] && slen(expect_sha) != 64) return ERR_ARG;
    copy_str(pending_sha, expect_sha ? expect_sha : "", sizeof(pending_sha));
    return fo_job_start(FO_JOB_FETCH, url, dst);   /* only the control thread starts jobs */
}

void fo_job_status(char *out, int max) {
    static const char *const kinds[] = {"-", "copy", "move", "remove", "sha256", "fetch"};
    static const char *const states[] = {"none", "running", "done", "failed", "cancelled"};
    if (job.state == ST_NONE) { sceClibSnprintf(out, max, "OK job none\n"); return; }
    sceClibSnprintf(out, max, "OK job %d %s %s %s files=%u/%u bytes=%llu/%llu rc=0x%08X %s\n",
                    job.id, kinds[job.kind], states[job.state],
                    job.state == ST_RUNNING ? (job.phase ? "working" : "sizing") : "-",
                    job.files_done, job.files_total,
                    job.kind == FO_JOB_FETCH ? fetch_st.done : job.bytes_done,
                    job.kind == FO_JOB_FETCH ? fetch_st.total : job.bytes_total,
                    job.rc, job.note[0] ? job.note : "-");
}

void fo_job_info(FoJobInfo *o) {
    o->id = job.id; o->kind = job.kind; o->state = job.state; o->phase = job.phase; o->rc = job.rc;
    o->files_done = job.files_done; o->files_total = job.files_total;
    o->bytes_done = job.kind == FO_JOB_FETCH ? fetch_st.done : job.bytes_done;
    o->bytes_total = job.kind == FO_JOB_FETCH ? fetch_st.total : job.bytes_total;
    copy_str(o->note, job.note, sizeof(o->note));
}

int fo_rename(const char *src, const char *dst) {
    if (fo_can_remove(src) < 0 || fo_can_write(dst) < 0 || !same_device(src, dst)) return ERR_POLICY;
    if (inside(dst, src)) return ERR_ARG;
    return sceIoRename(src, dst);
}

int fo_job_cancel(void) {
    if (job.state != ST_RUNNING) return ERR_ARG;
    job.cancel = 1;
    fetch_st.cancel = 1;
    return 0;
}

int fo_shutdown(void) {
    if (job.state == ST_RUNNING) job.cancel = 1;
    for (int i = 0; i < 200 && job.state == ST_RUNNING; ++i) sceKernelDelayThread(50 * 1000);
    return job.state == ST_RUNNING ? ERR_BUSY : 0;
}
