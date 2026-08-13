#include "../../include/cce/cce_campaign_provenance.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compact SHA-256 implementation kept local so replay validation has no
 * shell, OpenSSL, or platform-package dependency. */
typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(sha256_ctx *c, const unsigned char b[64]) {
    uint32_t w[64], a, d, e, f, g, h, i0, j, t1, t2;
    for (j = 0; j < 16; ++j)
        w[j] = ((uint32_t)b[j*4] << 24) | ((uint32_t)b[j*4+1] << 16) |
               ((uint32_t)b[j*4+2] << 8) | (uint32_t)b[j*4+3];
    for (; j < 64; ++j) {
        uint32_t s0 = rotr32(w[j-15], 7) ^ rotr32(w[j-15], 18) ^ (w[j-15] >> 3);
        uint32_t s1 = rotr32(w[j-2], 17) ^ rotr32(w[j-2], 19) ^ (w[j-2] >> 10);
        w[j] = w[j-16] + s0 + w[j-7] + s1;
    }
    a=c->h[0]; i0=c->h[1]; d=c->h[2]; e=c->h[3];
    f=c->h[4]; g=c->h[5]; h=c->h[6]; j=c->h[7];
    for (uint32_t r = 0; r < 64; ++r) {
        uint32_t s1 = rotr32(f,6) ^ rotr32(f,11) ^ rotr32(f,25);
        uint32_t ch = (f & g) ^ ((~f) & h);
        uint32_t s0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t maj = (a & i0) ^ (a & d) ^ (i0 & d);
        t1 = j + s1 + ch + sha256_k[r] + w[r];
        t2 = s0 + maj;
        j=h; h=g; g=f; f=e+t1; e=d; d=i0; i0=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=i0; c->h[2]+=d; c->h[3]+=e;
    c->h[4]+=f; c->h[5]+=g; c->h[6]+=h; c->h[7]+=j;
}

static void sha256_init(sha256_ctx *c) {
    static const uint32_t init[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(c->h, init, sizeof init);
    c->bits = 0; c->used = 0;
}

static void sha256_update(sha256_ctx *c, const unsigned char *p, size_t n) {
    c->bits += (uint64_t)n * 8u;
    while (n > 0) {
        size_t take = 64u - c->used;
        if (take > n) take = n;
        memcpy(c->block + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64u) {
            sha256_transform(c, c->block);
            c->used = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[32]) {
    uint64_t bits = c->bits;
    c->block[c->used++] = 0x80u;
    if (c->used > 56u) {
        memset(c->block + c->used, 0, 64u - c->used);
        sha256_transform(c, c->block);
        c->used = 0;
    }
    memset(c->block + c->used, 0, 56u - c->used);
    for (unsigned i = 0; i < 8; ++i)
        c->block[63u-i] = (unsigned char)(bits >> (i*8u));
    sha256_transform(c, c->block);
    for (unsigned i = 0; i < 8; ++i) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)c->h[i];
    }
}

static void digest_hex(const unsigned char digest[32], char hex_out[65]) {
    static const char hd[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 32; ++i) {
        hex_out[i * 2u] = hd[digest[i] >> 4];
        hex_out[i * 2u + 1u] = hd[digest[i] & 15u];
    }
    hex_out[64] = '\0';
}

int cce_sha256_bytes_hex(const void *bytes, size_t length, char hex_out[65]) {
    unsigned char digest[32];
    sha256_ctx context;
    if (hex_out == NULL || (bytes == NULL && length != 0)) return -1;
    sha256_init(&context);
    if (length != 0)
        sha256_update(&context, (const unsigned char *)bytes, length);
    sha256_final(&context, digest);
    digest_hex(digest, hex_out);
    return 0;
}

int cce_sha256_file_hex(const char *path, char hex_out[65]) {
    unsigned char buf[65536], digest[32];
    sha256_ctx c;
    FILE *f;
    size_t n;
    if (!path || !*path || !hex_out) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    sha256_init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, n);
    if (ferror(f) || fclose(f) != 0) return -1;
    sha256_final(&c, digest);
    digest_hex(digest, hex_out);
    return 0;
}

static void set_error(char *out, size_t cap, const char *what) {
    if (out && cap) snprintf(out, cap, "%s", what ? what : "provenance refusal");
}

static char *read_small_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        n > 1024L * 1024L || fseek(f, 0, SEEK_SET) != 0) {
        if (f) fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n || fclose(f) != 0) {
        free(buf); return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static const char *json_value(const char *doc, const char *key) {
    char needle[128];
    const char *p;
    if (snprintf(needle, sizeof needle, "\"%s\"", key) < 0) return NULL;
    p = strstr(doc, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return NULL;
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

static int json_string(const char *doc, const char *key, char *out, size_t cap) {
    const char *p = json_value(doc, key);
    size_t n = 0;
    if (!p || *p++ != '"' || !out || cap == 0) return -1;
    while (*p && *p != '"') {
        char ch = *p++;
        if (ch == '\\') {
            ch = *p++;
            if (ch == 'n') ch = '\n';
            else if (ch == 'r') ch = '\r';
            else if (ch == 't') ch = '\t';
            else if (ch != '\\' && ch != '"' && ch != '/') return -1;
        }
        if (n + 1u >= cap) return -1;
        out[n++] = ch;
    }
    if (*p != '"') return -1;
    out[n] = '\0';
    return 0;
}

static int json_int(const char *doc, const char *key, int *out) {
    const char *p = json_value(doc, key);
    char *end;
    long v;
    if (!p || !out) return -1;
    v = strtol(p, &end, 10);
    if (end == p || v < -2147483647L-1L || v > 2147483647L) return -1;
    *out = (int)v;
    return 0;
}

static int valid_hash(const char *s) {
    if (!s || strlen(s) != 64u) return 0;
    for (size_t i = 0; i < 64u; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

static int verify_file_field(const char *doc, const char *path_key,
                             const char *hash_key, const char *label,
                             int allow_empty, char *error, size_t error_cap) {
    char path[1024], expected[65], actual[65], msg[192];
    if (json_string(doc, path_key, path, sizeof path) != 0) {
        snprintf(msg, sizeof msg, "missing %s path (%s)", label, path_key);
        set_error(error, error_cap, msg); return -1;
    }
    if (json_string(doc, hash_key, expected, sizeof expected) != 0) {
        snprintf(msg, sizeof msg, "missing %s fingerprint (%s)", label, hash_key);
        set_error(error, error_cap, msg); return -1;
    }
    if (!*path && allow_empty) {
        if (*expected) {
            snprintf(msg, sizeof msg, "%s hash present without artifact", label);
            set_error(error, error_cap, msg); return -1;
        }
        return 0;
    }
    if (!*path || !valid_hash(expected)) {
        snprintf(msg, sizeof msg, "invalid %s provenance fields", label);
        set_error(error, error_cap, msg); return -1;
    }
    if (cce_sha256_file_hex(path, actual) != 0) {
        snprintf(msg, sizeof msg, "%s artifact unreadable: %.120s", label, path);
        set_error(error, error_cap, msg); return -1;
    }
    if (strcmp(expected, actual) != 0) {
        snprintf(msg, sizeof msg, "%s sha256 mismatch", label);
        set_error(error, error_cap, msg); return -1;
    }
    return 0;
}

int cce_campaign_provenance_verify(const char *manifest_path,
                                   const char *executable_path,
                                   const char *live_build_rev,
                                   int live_source_dirty,
                                   char *error,
                                   size_t error_cap) {
    char *doc, expected[128], actual[65], msg[192];
    int version, dirty;
    if (error && error_cap) error[0] = '\0';
    doc = read_small_file(manifest_path);
    if (!doc) { set_error(error, error_cap, "manifest unreadable"); return -1; }
    if (json_int(doc, "provenance_version", &version) != 0 || version != 1) {
        set_error(error, error_cap, "missing/unsupported provenance_version");
        free(doc); return -1;
    }
    if (json_string(doc, "build_rev", expected, sizeof expected) != 0 ||
        !live_build_rev || strcmp(expected, live_build_rev) != 0) {
        set_error(error, error_cap, "build_rev mismatch"); free(doc); return -1;
    }
    if (json_int(doc, "source_dirty", &dirty) != 0 ||
        dirty != (live_source_dirty ? 1 : 0)) {
        set_error(error, error_cap, "source_dirty mismatch"); free(doc); return -1;
    }
    if (json_string(doc, "executable_sha256", expected, sizeof expected) != 0) {
        set_error(error, error_cap, "missing executable_sha256"); free(doc); return -1;
    }
    if (!valid_hash(expected) || cce_sha256_file_hex(executable_path, actual) != 0 ||
        strcmp(expected, actual) != 0) {
        snprintf(msg, sizeof msg, "executable_sha256 mismatch");
        set_error(error, error_cap, msg); free(doc); return -1;
    }
    if (verify_file_field(doc, "model", "model_sha256", "model", 0,
                          error, error_cap) != 0 ||
        verify_file_field(doc, "window_source", "window_sha256", "window", 0,
                          error, error_cap) != 0 ||
        verify_file_field(doc, "oracle_golden", "oracle_golden_sha256", "golden", 1,
                          error, error_cap) != 0 ||
        verify_file_field(doc, "base", "base_sha256", "base", 0,
                          error, error_cap) != 0) {
        free(doc); return -1;
    }
    free(doc);
    set_error(error, error_cap, "ok");
    return 0;
}
