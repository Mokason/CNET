/* cnet_roe_gold_id.h — the ONE definition of ROE gold/skill identity.
 *
 * A gold answer lives at <packs>/gold/<sha1_16>.txt and its promoted capsule is
 * catalogued as auto_<sha1_16>, where the digest is SHA-1 over the *normalised*
 * query. Every producer and every consumer of those names must agree bit for
 * bit: if `roe_gold_put` normalises differently from `roe_evolve_tick`, the
 * gold file is written where nothing will ever look for it and the promote
 * silently never happens.
 *
 * So the identity lives here, once, and both tools include it. Do not copy
 * these functions into a tool — include the header.
 *
 * Digest is plain SHA-1, byte-compatible with `sha1sum` and Python hashlib, so
 * gold minted before the C port keeps resolving. Guarded by RFC 3174 vectors in
 * `bin/roe_evolve_tick --selftest`.
 *
 * Header-only, all static inline: no link changes, no unused-function warnings
 * under -Wall -Wextra -Werror.
 */
#ifndef CNET_ROE_GOLD_ID_H
#define CNET_ROE_GOLD_ID_H

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROE_NORMMAX 256   /* norm_q truncates the query to 200 chars */
#define ROE_PATHMAX 1024

/* ----------------------------------------------------------------- SHA-1 */
typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} RoeSha1;

static inline uint32_t roe_rol32(uint32_t v, int s) {
    return (v << s) | (v >> (32 - s));
}

static inline void roe_sha1_init(RoeSha1 *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
}

static inline void roe_sha1_block(RoeSha1 *c, const uint8_t *p) {
    uint32_t w[80], a, b, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = roe_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t g;
        if (i < 20)      { g = (b & d) | (~b & e);          k = 0x5A827999u; }
        else if (i < 40) { g = b ^ d ^ e;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { g = (b & d) | (b & e) | (d & e); k = 0x8F1BBCDCu; }
        else             { g = b ^ d ^ e;                   k = 0xCA62C1D6u; }
        t = roe_rol32(a, 5) + g + f + k + w[i];
        f = e; e = d; d = roe_rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static inline void roe_sha1_update(RoeSha1 *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->len += (uint64_t)len;
    while (len > 0) {
        size_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { roe_sha1_block(c, c->buf); c->n = 0; }
    }
}

static inline void roe_sha1_hex(const char *s, char out[41]) {
    RoeSha1 c;
    uint8_t pad[72];
    uint64_t bits;
    size_t padn;
    int i;
    roe_sha1_init(&c);
    roe_sha1_update(&c, s, strlen(s));
    bits = c.len * 8u;
    padn = (c.n < 56) ? (56 - c.n) : (120 - c.n);
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padn + i] = (uint8_t)(bits >> (56 - 8 * i));
    roe_sha1_update(&c, pad, padn + 8);
    for (i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", c.h[i]);
    out[40] = 0;
}

/* --------------------------------------------------------- normalisation */
/* lower + collapse internal whitespace + strip ends + truncate to `limit` */
static inline void roe_norm_trunc(const char *in, char *out, size_t cap, size_t limit) {
    size_t o = 0, i = 0;
    int sp = 0;
    if (limit > cap - 1) limit = cap - 1;
    if (!in) { out[0] = 0; return; }
    while (in[i] && isspace((unsigned char)in[i])) i++;
    for (; in[i] && o < limit; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (isspace(ch)) { sp = 1; continue; }
        if (sp && o > 0) { out[o++] = ' '; if (o >= limit) break; }
        sp = 0;
        out[o++] = (char)tolower(ch);
    }
    out[o] = 0;
}

static inline void roe_norm_q(const char *q, char out[ROE_NORMMAX]) {
    roe_norm_trunc(q, out, ROE_NORMMAX, 200);
}

/* the 16-hex identity used for gold filenames and auto_<id> skill ids */
static inline void roe_q_hash16(const char *q, char out[17]) {
    char nq[ROE_NORMMAX], hex[41];
    roe_norm_q(q, nq);
    roe_sha1_hex(nq, hex);
    memcpy(out, hex, 16);
    out[16] = 0;
}

static inline void roe_skill_id_for(const char *q, char out[32]) {
    char h[17];
    roe_q_hash16(q, h);
    snprintf(out, 32, "auto_%s", h);
}

/* ------------------------------------------------------------ packs root */
/* Same precedence the evolve tick uses, so a gold written by one tool is
 * always found by the other: CNET_PACKS_ROOT > CNET_MINIMAL_ROOT/data/... >
 * <root>/artifacts/roe_daily_packs. */
static inline int roe_is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static inline void roe_packs_root(char *out, size_t cap, const char *repo_root) {
    const char *pk = getenv("CNET_PACKS_ROOT");
    const char *mn = getenv("CNET_MINIMAL_ROOT");
    char cand[ROE_PATHMAX];
    if (pk && pk[0]) { snprintf(out, cap, "%s", pk); return; }
    if (mn && mn[0]) {
        snprintf(cand, sizeof cand, "%s/data/roe_daily_packs", mn);
        if (roe_is_dir(cand)) { snprintf(out, cap, "%s", cand); return; }
    }
    snprintf(out, cap, "%s/artifacts/roe_daily_packs",
             (repo_root && repo_root[0]) ? repo_root : ".");
}

static inline void roe_gold_path(char *out, size_t cap, const char *packs, const char *q) {
    char h[17];
    roe_q_hash16(q, h);
    snprintf(out, cap, "%s/gold/%s.txt", packs, h);
}

#endif /* CNET_ROE_GOLD_ID_H */
