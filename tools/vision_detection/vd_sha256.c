#include "vd_sha256.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>

static uint32_t rr(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }

static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

static void blk(VdSha256 *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rr(w[i-15],7) ^ rr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rr(w[i-2],17) ^ rr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->s[0]; b=c->s[1]; cc=c->s[2]; d=c->s[3]; e=c->s[4]; f=c->s[5]; g=c->s[6]; h=c->s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rr(e,6) ^ rr(e,11) ^ rr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rr(a,2) ^ rr(a,13) ^ rr(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a; c->s[1]+=b; c->s[2]+=cc; c->s[3]+=d;
    c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}

void vd_sha256_init(VdSha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                                   0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    memcpy(c->s, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

void vd_sha256_update(VdSha256 *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    while (len) {
        size_t t = 64 - c->n;
        if (t > len) t = len;
        memcpy(c->buf + c->n, p, t);
        c->n += t; p += t; len -= t;
        if (c->n == 64) { blk(c, c->buf); c->n = 0; }
    }
}

void vd_sha256_hex(VdSha256 *c, char *out) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0, e[8];
    int i;
    vd_sha256_update(c, &pad, 1);
    while (c->n != 56) vd_sha256_update(c, &z, 1);
    for (i = 0; i < 8; i++) e[i] = (uint8_t)(bits >> (56 - i * 8));
    vd_sha256_update(c, e, 8);
    for (i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", c->s[i]);
    out[64] = 0;
}

int vd_sha256_file(const char *path, char *out) {
    FILE *f;
    VdSha256 c;
    unsigned char buf[65536];
    size_t got;
    if (!path || !out) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    vd_sha256_init(&c);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) vd_sha256_update(&c, buf, got);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    vd_sha256_hex(&c, out);
    return 0;
}

int vd_sha256_fd(int fd, char *out) {
    VdSha256 c;
    unsigned char buf[65536];
    off_t off = 0;
    if (fd < 0 || !out) return -1;
    vd_sha256_init(&c);
    for (;;) {
        ssize_t got = pread(fd, buf, sizeof buf, off);
        if (got < 0) return -1;
        if (got == 0) break;
        vd_sha256_update(&c, buf, (size_t)got);
        off += got;
    }
    vd_sha256_hex(&c, out);
    return 0;
}
