#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>

#include "fetch.h"
#include "sha256.h"

#define ERR_BADURL   ((int)0x80AB0011)
#define ERR_STATUS   ((int)0x80AB0012)   /* HTTP status other than 200 */
#define ERR_SHA      ((int)0x80AB0013)   /* content did not match expect_sha */
#define ERR_CANCELED ((int)0x80AB0014)
#define ERR_SHORTW   ((int)0x80AB0015)
#define ERR_TLS      ((int)0x80AB0016)   /* certificate refused, no sha given */

static int state, owned;

int fetch_init(int own_pools) {
    if (state) return state;
    /* In SceShell these are normally initialised already; "already" is fine. */
    int rc = sceSslInit(300 * 1024);
    if (rc < 0 && own_pools) { state = rc; return rc; }
    rc = sceHttpInit(512 * 1024);
    if (rc < 0 && rc != (int)SCE_HTTP_ERROR_ALREADY_INITED) { state = rc; return rc; }
    owned = own_pools && rc >= 0;
    state = 1;
    return state;
}

void fetch_term(void) {
    if (owned) { sceHttpTerm(); sceSslTerm(); }
    state = owned = 0;
}

typedef struct { FetchState *st; int trust_by_content; } TlsCtx;

/* Called only when verification fails. Refuse unless the caller pinned the
 * content; then remember what was wrong and carry on. */
static int tls_check(unsigned int verify_err, void *const cert[], int n, void *arg) {
    (void)cert; (void)n;
    TlsCtx *c = arg;
    c->st->tls_errors |= verify_err;
    return c->trust_by_content ? 0 : -1;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int fetch_to_file(const char *url, const char *path, const char *expect_sha,
                  void *buf, unsigned int bufsize, FetchState *st, unsigned char sha_out[32]) {
    unsigned char want[32];
    int pinned = 0;
    if (expect_sha && expect_sha[0]) {
        for (int i = 0; i < 32; ++i) {
            int hi = hexval(expect_sha[2 * i]), lo = hi < 0 ? -1 : hexval(expect_sha[2 * i + 1]);
            if (hi < 0 || lo < 0) return ERR_BADURL;
            want[i] = (unsigned char)(hi << 4 | lo);
        }
        pinned = 1;
    }
    if (sceClibStrncmp(url, "http://", 7) && sceClibStrncmp(url, "https://", 8)) return ERR_BADURL;
    if (state != 1) return state ? state : ERR_BADURL;

    char part[520];
    int n = (int)sceClibStrnlen(path, 510);
    sceClibMemcpy(part, path, n);
    sceClibMemcpy(part + n, ".part", 6);

    TlsCtx ctx = {st, pinned};
    int tpl = -1, conn = -1, req = -1, rc;
    SceUID fd = -1;
    st->done = st->total = 0;
    st->http_status = 0;
    st->tls_errors = 0;

    tpl = rc = sceHttpCreateTemplate("vabridge-fetch/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (rc < 0) goto out;
    sceHttpSetConnectTimeOut(tpl, 15 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 30 * 1000 * 1000);
    sceHttpSetAutoRedirect(tpl, 1);                   /* GitHub release links redirect */
    sceHttpsSetSslCallback(tpl, tls_check, &ctx);
    conn = rc = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (rc < 0) goto out;
    req = rc = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
    if (rc < 0) goto out;
    rc = sceHttpSendRequest(req, NULL, 0);
    if (rc < 0) {
        if ((unsigned int)rc == SCE_HTTPS_ERROR_CERT || (unsigned int)rc == SCE_HTTPS_ERROR_HANDSHAKE)
            rc = pinned ? rc : ERR_TLS;
        goto out;
    }
    rc = sceHttpGetStatusCode(req, &st->http_status);
    if (rc < 0) goto out;
    if (st->http_status != 200) { rc = ERR_STATUS; goto out; }
    unsigned long long len = 0;
    if (sceHttpGetResponseContentLength(req, &len) >= 0) st->total = len;

    fd = rc = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (rc < 0) goto out;
    Sha256 h;
    sha256_init(&h);
    for (;;) {
        if (st->cancel) { rc = ERR_CANCELED; break; }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);   /* no standby mid-download */
        int got = sceHttpReadData(req, buf, bufsize);
        if (got < 0) { rc = got; break; }
        if (got == 0) { rc = 0; break; }
        sha256_update(&h, buf, (unsigned int)got);
        int w = sceIoWrite(fd, buf, got);
        if (w != got) { rc = w < 0 ? w : ERR_SHORTW; break; }
        st->done += (unsigned int)got;
    }
    sceIoClose(fd);
    fd = -1;
    if (rc >= 0 && st->total && st->done != st->total) rc = ERR_SHORTW;
    if (rc >= 0) {
        sha256_final(&h, sha_out);
        if (pinned)
            for (int i = 0; i < 32; ++i) if (sha_out[i] != want[i]) rc = ERR_SHA;
    }
    if (rc >= 0) {
        sceIoRemove(path);
        rc = sceIoRename(part, path);
    }
out:
    if (fd >= 0) sceIoClose(fd);
    if (rc < 0) sceIoRemove(part);                    /* never leave a partial download */
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return rc;
}
