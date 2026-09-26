/* Small self-contained SHA-256 (FIPS 180-4). No libc: builds -nostdlib inside
 * the bridge, and the same file compiles on the Mac for a test against
 * hashlib (tools/test_sha256.sh). */
#ifndef VABRIDGE_SHA256_H
#define VABRIDGE_SHA256_H

typedef struct {
    unsigned int h[8];
    unsigned long long len;   /* bytes hashed so far */
    unsigned char blk[64];
    unsigned int n;           /* bytes waiting in blk */
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, unsigned int len);
void sha256_final(Sha256 *c, unsigned char out[32]);
void sha256_hex(const unsigned char d[32], char out[65]);

#endif
