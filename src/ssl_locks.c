/* OpenSSL 1.0.2 is only thread-safe when the program gives it locks.
 * VitaOS fetches over HTTPS from several threads at once (store, news,
 * weather, downloads, box art), and without these callbacks two of them
 * racing through the random-number pool trip an assert in md_rand.c and
 * abort the whole app (crash dump 2026-09-27: thread "store",
 * ssleay_rand_add -> __assert_func -> abort, shown as C2-12828-1).
 *
 * Call ssl_threads_init() first thing in main, before any thread exists;
 * it also runs curl_global_init, which is not thread-safe either. */
#include <psp2/kernel/threadmgr.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <psp2/kernel/rng.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include "ssl_locks.h"

static SceUID *locks;

static void lock_cb(int mode, int n, const char *file, int line) {
    (void)file; (void)line;
    if (mode & CRYPTO_LOCK) sceKernelLockMutex(locks[n], 1, NULL);
    else sceKernelUnlockMutex(locks[n], 1);
}

static unsigned long id_cb(void) { return (unsigned long)sceKernelGetThreadId(); }

/* The THREADID form is the one 1.0.2 consults first; without it (or when the
 * old id callback is compiled out) every thread looks the same (the address
 * of errno), md_rand thinks the lock is already held and skips it, and the
 * crash above came back (dump 2026-09-27 20:40). */
static void threadid_cb(CRYPTO_THREADID *id) { CRYPTO_THREADID_set_numeric(id, (unsigned long)sceKernelGetThreadId()); }

/* The real fix. This OpenSSL is built without OPENSSL_THREADS, so md_rand.c
 * keeps an assert (md_c[1] == md_count[1], checked after the lock is dropped)
 * that any two threads using the pool at once can trip, locks or not. Hand
 * OpenSSL the Vita's hardware RNG instead: thread-safe, and md_rand never runs. */
static int vita_rand_bytes(unsigned char *buf, int num) {
    while (num > 0) {
        int n = num > 64 ? 64 : num;              /* the kernel hands out at most 64 bytes a call */
        if (sceKernelGetRandomNumber(buf, n) < 0) return 0;
        buf += n;
        num -= n;
    }
    return 1;
}
static void vita_rand_seed(const void *buf, int num) { (void)buf; (void)num; }
static void vita_rand_add(const void *buf, int num, double e) { (void)buf; (void)num; (void)e; }
static void vita_rand_cleanup(void) {}
static int vita_rand_status(void) { return 1; }
static const RAND_METHOD vita_rand = {vita_rand_seed, vita_rand_bytes, vita_rand_cleanup, vita_rand_add, vita_rand_bytes,
                                      vita_rand_status};

int ssl_threads_init(void) {
    RAND_set_rand_method(&vita_rand);
    int n = CRYPTO_num_locks();
    locks = calloc(n, sizeof(SceUID));
    if (!locks) return -1;
    for (int i = 0; i < n; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "ssl_lock_%d", i);
        locks[i] = sceKernelCreateMutex(name, SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
        if (locks[i] < 0) return -1;
    }
    CRYPTO_THREADID_set_callback(threadid_cb);
    CRYPTO_set_id_callback(id_cb);
    CRYPTO_set_locking_callback(lock_cb);
    return curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK ? 0 : -1;
}
