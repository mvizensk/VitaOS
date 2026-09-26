/* fetch: HTTP(S) download straight to a file on the Vita. Shared by the
 * bridge (the "fetch" background job, inside SceShell) and Workbench.
 *
 * Certificates are verified. Inside SceShell we must never switch that off
 * globally (sceHttpsDisableOption is process-wide and would weaken the
 * shell's own HTTPS), so the only way past a certificate the Vita's 2018 root
 * store cannot check is to name the file's SHA-256 up front: the transfer is
 * then trusted by content, and a mismatch deletes it. */
#ifndef VABRIDGE_FETCH_H
#define VABRIDGE_FETCH_H

typedef struct {
    volatile unsigned long long done, total;   /* total 0 = server did not say */
    volatile int cancel;
    int http_status;                            /* e.g. 200, 404 */
    unsigned int tls_errors;                    /* SCE_HTTPS_ERROR_SSL_* bits seen */
} FetchState;

/* Idempotent. own_pools = 1 in an app (init and later term our own pools),
 * 0 inside SceShell (reuse the shell's, never term them). <0 = no HTTP here. */
int fetch_init(int own_pools);
void fetch_term(void);

/* expect_sha: 64 hex chars or NULL. Writes path.part, renames on success.
 * sha_out receives the digest of what was written. */
int fetch_to_file(const char *url, const char *path, const char *expect_sha,
                  void *buf, unsigned int bufsize, FetchState *st, unsigned char sha_out[32]);

#endif
