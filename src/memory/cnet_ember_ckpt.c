#include "cnet_ember_ckpt.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Minimal SHA-1 (public-domain style) for prefix keys. */
typedef struct {
    uint32_t h[5];
    uint64_t nbits;
    uint8_t buf[64];
    size_t nb;
} sha1_ctx;

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_init(sha1_ctx *c) {
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->nbits = 0;
    c->nb = 0;
}

static void sha1_block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80], a, b, cc, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->nbits += (uint64_t)len * 8ull;
    while (len) {
        size_t n = 64 - c->nb;
        if (n > len) n = len;
        memcpy(c->buf + c->nb, p, n);
        c->nb += n;
        p += n;
        len -= n;
        if (c->nb == 64) {
            sha1_block(c, c->buf);
            c->nb = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    size_t i;
    c->buf[c->nb++] = 0x80;
    if (c->nb > 56) {
        while (c->nb < 64) c->buf[c->nb++] = 0;
        sha1_block(c, c->buf);
        c->nb = 0;
    }
    while (c->nb < 56) c->buf[c->nb++] = 0;
    for (i = 0; i < 8; i++) c->buf[56 + i] = (uint8_t)((c->nbits >> (56 - 8 * i)) & 0xff);
    sha1_block(c, c->buf);
    for (i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->h[i]);
    }
}

void cnet_ember_sha1_hex(const void *ptr, size_t len, char out[CNET_EMBER_SHA_HEX]) {
    sha1_ctx c;
    uint8_t d[20];
    int i;
    static const char *hex = "0123456789abcdef";
    sha1_init(&c);
    if (ptr && len) sha1_update(&c, ptr, len);
    sha1_final(&c, d);
    for (i = 0; i < 20; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 15];
    }
    out[40] = '\0';
}

const char *cnet_ember_ckpt_reason_name(CnetEmberCkptReason r) {
    switch (r) {
    case CNET_EMBER_CKPT_COLD:
        return "cold";
    case CNET_EMBER_CKPT_CONTINUED:
        return "continued";
    case CNET_EMBER_CKPT_EVICT:
        return "evict";
    case CNET_EMBER_CKPT_COMPACT:
        return "compact";
    case CNET_EMBER_CKPT_SHUTDOWN:
        return "shutdown";
    default:
        return "unknown";
    }
}

void cnet_ember_ckpt_opts_default(CnetEmberCkptStore *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->budget_bytes = 64ull * 1024ull * 1024ull;
    s->min_chars = 64;
}

static uint64_t now_s(void) { return (uint64_t)time(NULL); }

static int ensure_dir(const char *d) {
    struct stat st;
    if (!d || !d[0]) return -1;
    if (stat(d, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (cnet_mkdir(d, 0755) != 0) return -1;
    return 0;
}

static int grow(CnetEmberCkptStore *s) {
    int nc;
    CnetEmberCkptEntry *nv;
    if (s->n < s->cap) return 0;
    nc = s->cap ? s->cap * 2 : 32;
    if (nc > CNET_EMBER_CKPT_MAX) nc = CNET_EMBER_CKPT_MAX;
    if (s->n >= nc) return -1;
    nv = (CnetEmberCkptEntry *)realloc(s->v, (size_t)nc * sizeof *nv);
    if (!nv) return -1;
    s->v = nv;
    s->cap = nc;
    return 0;
}

static uint64_t total_bytes(const CnetEmberCkptStore *s) {
    int i;
    uint64_t t = 0;
    for (i = 0; i < s->n; i++) t += s->v[i].file_size;
    return t;
}

static void evict_one(CnetEmberCkptStore *s) {
    int i, worst = -1;
    double score = 1e300, sc;
    uint64_t now = now_s();
    if (s->n <= 0) return;
    for (i = 0; i < s->n; i++) {
        double age = (double)(now > s->v[i].last_used ? now - s->v[i].last_used : 0);
        double hits = s->v[i].hits + 1.0;
        sc = age / hits;
        if (sc > score || worst < 0) {
            score = sc;
            worst = i;
        }
    }
    if (worst < 0) return;
    unlink(s->v[worst].path);
    if (worst != s->n - 1) s->v[worst] = s->v[s->n - 1];
    s->n--;
}

static int load_index(CnetEmberCkptStore *s) {
    DIR *d;
    struct dirent *de;
    d = opendir(s->dir);
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        char path[CNET_EMBER_CKPT_PATH];
        FILE *f;
        CnetEmberCkptEntry e;
        char magic[8];
        uint32_t tlen = 0;
        size_t n;
        if (strncmp(de->d_name, "e_", 2) != 0) continue;
        if (!strstr(de->d_name, ".eck")) continue;
        snprintf(path, sizeof path, "%.400s/%.80s", s->dir, de->d_name);
        f = fopen(path, "rb");
        if (!f) continue;
        memset(&e, 0, sizeof e);
        n = fread(magic, 1, 4, f);
        if (n != 4 || memcmp(magic, "ECK1", 4) != 0) {
            fclose(f);
            continue;
        }
        if (fread(e.sha, 1, 40, f) != 40) {
            fclose(f);
            continue;
        }
        e.sha[40] = '\0';
        if (fread(&e.reason, 1, 1, f) != 1 || fread(&e.turns, 4, 1, f) != 1 ||
            fread(&e.hits, 4, 1, f) != 1 || fread(&e.created_at, 8, 1, f) != 1 ||
            fread(&e.last_used, 8, 1, f) != 1 || fread(&tlen, 4, 1, f) != 1) {
            fclose(f);
            continue;
        }
        e.text_bytes = tlen;
        {
            struct stat st;
            if (stat(path, &st) == 0) e.file_size = (uint64_t)st.st_size;
        }
        snprintf(e.path, sizeof e.path, "%s", path);
        fclose(f);
        if (grow(s) != 0) break;
        s->v[s->n++] = e;
    }
    closedir(d);
    return 0;
}

int cnet_ember_ckpt_open(CnetEmberCkptStore *s, const char *dir, uint64_t budget_mb) {
    if (!s || !dir || !dir[0]) return -1;
    cnet_ember_ckpt_opts_default(s);
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    if (budget_mb > 0) s->budget_bytes = budget_mb * 1024ull * 1024ull;
    if (ensure_dir(s->dir) != 0) return -1;
    if (load_index(s) != 0) return -1;
    s->open = 1;
    return 0;
}

void cnet_ember_ckpt_close(CnetEmberCkptStore *s) {
    if (!s) return;
    free(s->v);
    memset(s, 0, sizeof *s);
}

int cnet_ember_ckpt_find_prefix(CnetEmberCkptStore *s, const char *text, size_t text_len) {
    int i, best = -1;
    size_t best_len = 0;
    if (!s || !s->open || !text) return -1;
    for (i = 0; i < s->n; i++) {
        char buf[8192];
        size_t got = 0;
        size_t want;
        FILE *f = fopen(s->v[i].path, "rb");
        if (!f) continue;
        if (fseek(f, 4 + 40 + 1 + 4 + 4 + 8 + 8 + 4, SEEK_SET) != 0) {
            fclose(f);
            continue;
        }
        want = s->v[i].text_bytes;
        if (want > sizeof buf - 1) want = sizeof buf - 1;
        got = fread(buf, 1, want, f);
        fclose(f);
        buf[got] = '\0';
        if (got == 0) continue;
        if (text_len >= got && memcmp(text, buf, got) == 0) {
            if (got > best_len) {
                best_len = got;
                best = i;
            }
        }
    }
    if (best >= 0) {
        s->v[best].hits++;
        s->v[best].last_used = now_s();
    }
    return best;
}

int cnet_ember_ckpt_load(CnetEmberCkptStore *s, int idx, char *buf, size_t cap,
                         size_t *out_len) {
    FILE *f;
    uint32_t tlen = 0;
    size_t n;
    if (!s || !s->open || idx < 0 || idx >= s->n || !buf || cap == 0) return -1;
    f = fopen(s->v[idx].path, "rb");
    if (!f) return -1;
    if (fseek(f, 4 + 40 + 1 + 4 + 4 + 8 + 8, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if (fread(&tlen, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    if ((size_t)tlen + 1 > cap) tlen = (uint32_t)(cap - 1);
    n = fread(buf, 1, tlen, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    s->v[idx].hits++;
    s->v[idx].last_used = now_s();
    return (int)n;
}

int cnet_ember_ckpt_store(CnetEmberCkptStore *s, const char *text, size_t text_len,
                          uint32_t turns, CnetEmberCkptReason reason) {
    char sha[CNET_EMBER_SHA_HEX];
    char path[CNET_EMBER_CKPT_PATH];
    FILE *f;
    uint32_t tlen;
    uint64_t now;
    CnetEmberCkptEntry e;
    int i;
    if (!s || !s->open || !text) return -1;
    if ((int)text_len < s->min_chars) return 1;
    cnet_ember_sha1_hex(text, text_len, sha);
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->v[i].sha, sha) == 0) {
            s->v[i].hits++;
            s->v[i].last_used = now_s();
            return 0;
        }
    }
    while (total_bytes(s) + text_len + 128 > s->budget_bytes && s->n > 0) evict_one(s);
    snprintf(path, sizeof path, "%.400s/e_%.40s.eck", s->dir, sha);
    f = fopen(path, "wb");
    if (!f) return -1;
    now = now_s();
    tlen = (uint32_t)text_len;
    fwrite("ECK1", 1, 4, f);
    fwrite(sha, 1, 40, f);
    {
        uint8_t r = (uint8_t)reason;
        fwrite(&r, 1, 1, f);
    }
    fwrite(&turns, 4, 1, f);
    {
        uint32_t hits = 1;
        fwrite(&hits, 4, 1, f);
    }
    fwrite(&now, 8, 1, f);
    fwrite(&now, 8, 1, f);
    fwrite(&tlen, 4, 1, f);
    fwrite(text, 1, text_len, f);
    fclose(f);
    memset(&e, 0, sizeof e);
    memcpy(e.sha, sha, sizeof e.sha);
    snprintf(e.path, sizeof e.path, "%s", path);
    e.reason = (uint8_t)reason;
    e.turns = turns;
    e.hits = 1;
    e.created_at = now;
    e.last_used = now;
    e.text_bytes = text_len;
    {
        struct stat st;
        if (stat(path, &st) == 0) e.file_size = (uint64_t)st.st_size;
    }
    if (grow(s) != 0) return -1;
    s->v[s->n++] = e;
    return 0;
}
