/* Held-out capability fixture consumption — see include/cnet_heldout.h.
 *
 * The JSON reader here is deliberately small and total: fixtures are committed
 * files with a fixed shape, so it only has to walk objects/arrays/strings well
 * enough to find a member, and it must never read past the buffer. Anything it
 * does not understand is an error, not a guess. SHA-256 is inlined rather than
 * shelled out to sha256sum so the digest the evaluator prints comes from the
 * same bytes it parsed, with no PATH or quoting in between.
 */
#include "../include/cnet_heldout.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- SHA-256 ----------------------------------------------------------- */

typedef struct {
    uint32_t s[8];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
} Sha256;

static uint32_t sha_ror(uint32_t x, unsigned r) {
    return (x >> r) | (x << (32u - r));
}

static void sha_block(Sha256 *c, const unsigned char *p) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = sha_ror(w[i - 15], 7) ^ sha_ror(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = sha_ror(w[i - 2], 17) ^ sha_ror(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->s[0]; b = c->s[1]; cc = c->s[2]; d = c->s[3];
    e = c->s[4]; f = c->s[5]; g = c->s[6]; h = c->s[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = sha_ror(e, 6) ^ sha_ror(e, 11) ^ sha_ror(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t S0 = sha_ror(a, 2) ^ sha_ror(a, 13) ^ sha_ror(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + K[i] + w[i];
        t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->s[0] += a; c->s[1] += b; c->s[2] += cc; c->s[3] += d;
    c->s[4] += e; c->s[5] += f; c->s[6] += g; c->s[7] += h;
}

static void sha_init(Sha256 *c) {
    c->s[0] = 0x6a09e667u; c->s[1] = 0xbb67ae85u;
    c->s[2] = 0x3c6ef372u; c->s[3] = 0xa54ff53au;
    c->s[4] = 0x510e527fu; c->s[5] = 0x9b05688cu;
    c->s[6] = 0x1f83d9abu; c->s[7] = 0x5be0cd19u;
    c->len = 0;
    c->n = 0;
}

static void sha_update(Sha256 *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += (uint64_t)len;
    while (len > 0) {
        size_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        len -= take;
        if (c->n == 64) {
            sha_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha_hex(Sha256 *c, char *out) {
    unsigned char pad[72];
    uint64_t bits = c->len * 8u;
    size_t padlen = (c->n < 56) ? (56 - c->n) : (120 - c->n);
    unsigned i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    sha_update(c, pad, padlen);
    for (i = 0; i < 8; i++) pad[i] = (unsigned char)(bits >> (56 - i * 8));
    sha_update(c, pad, 8);
    for (i = 0; i < 8; i++)
        snprintf(out + i * 8, 9, "%08x", (unsigned)c->s[i]);
    out[64] = '\0';
}

/* ---- minimal JSON walking ---------------------------------------------- */

static const char *js_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* End of the string literal starting at p (which must point at '"'). */
static const char *js_string_end(const char *p, const char *end) {
    if (p >= end || *p != '"') return NULL;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

/* End of the value starting at p, or NULL when malformed. */
static const char *js_value_end(const char *p, const char *end) {
    p = js_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return js_string_end(p, end);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                const char *q = js_string_end(p, end);
                if (!q) return NULL;
                p = q;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) return p + 1;
                if (depth < 0) return NULL;
            }
            p++;
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\n' && *p != '\t' && *p != '\r')
        p++;
    return p;
}

/* Find member `key` in the object spanning [p,end) (p points at '{').
   Sets the vs/ve out-parameters to the value span. Returns 0 on success. */
static int js_member(const char *p, const char *end, const char *key,
                     const char **vs, const char **ve) {
    size_t klen = strlen(key);
    const char *q;
    p = js_ws(p, end);
    if (p >= end || *p != '{') return -1;
    p++;
    for (;;) {
        const char *ks, *ke, *val_end;
        p = js_ws(p, end);
        if (p >= end) return -1;
        if (*p == '}') return -1;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') return -1;
        ks = p + 1;
        ke = js_string_end(p, end);
        if (!ke) return -1;
        p = js_ws(ke, end);
        if (p >= end || *p != ':') return -1;
        p++;
        q = js_ws(p, end);
        val_end = js_value_end(q, end);
        if (!val_end) return -1;
        if ((size_t)((ke - 1) - ks) == klen && memcmp(ks, key, klen) == 0) {
            *vs = q;
            *ve = val_end;
            return 0;
        }
        p = val_end;
    }
}

/* Copy a JSON string value (span includes the quotes) into out. */
static int js_string_copy(const char *vs, const char *ve, char *out,
                          size_t cap) {
    size_t n = 0;
    const char *p;
    if (ve - vs < 2 || *vs != '"' || ve[-1] != '"') return -1;
    for (p = vs + 1; p < ve - 1; p++) {
        char ch = *p;
        if (ch == '\\') {
            p++;
            if (p >= ve - 1) return -1;
            switch (*p) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case '"': case '\\': case '/': ch = *p; break;
                default: return -1; /* \u and friends: refuse, never guess */
            }
        }
        if (n + 1 >= cap) return -1;
        out[n++] = ch;
    }
    out[n] = '\0';
    return 0;
}

/* ---- fixture loading ---------------------------------------------------- */

static void heldout_reset(CnetHeldOut *h) {
    size_t i;
    memset(h, 0, sizeof *h);
    for (i = 0; i < CNET_HELDOUT_MAX_CASES; i++) h->verdict[i] = -1;
}

static int heldout_slurp(CnetHeldOut *h, const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;
    size_t got;
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    size = ftell(fp);
    if (size <= 0 || (unsigned long)size > CNET_HELDOUT_MAX_BYTES) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    h->text = (char *)malloc((size_t)size + 1);
    if (!h->text) { fclose(fp); return -1; }
    got = fread(h->text, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(h->text);
        h->text = NULL;
        return -1;
    }
    h->text[got] = '\0';
    h->len = got;
    {
        Sha256 c;
        sha_init(&c);
        sha_update(&c, h->text, h->len);
        sha_hex(&c, h->sha256);
    }
    return 0;
}

int cnet_heldout_open(CnetHeldOut *h, const char *capability_id) {
    const char *path = getenv("CNET_HELD_OUT_FIXTURE");
    const char *end, *vs, *ve, *p;
    if (!h || !capability_id || !capability_id[0]) return -1;
    heldout_reset(h);
    if (!path || !path[0]) return 1; /* standalone: nothing declared */
    h->required = 1;
    snprintf(h->path, sizeof h->path, "%s", path);
    if (heldout_slurp(h, path) != 0) {
        fprintf(stderr, "heldout: cannot read fixture %s\n", path);
        return -1;
    }
    end = h->text + h->len;
    if (js_member(h->text, end, "capability_id", &vs, &ve) != 0 ||
        js_string_copy(vs, ve, h->capability, sizeof h->capability) != 0) {
        fprintf(stderr, "heldout: fixture %s has no capability_id\n", path);
        return -1;
    }
    if (strcmp(h->capability, capability_id) != 0) {
        fprintf(stderr, "heldout: fixture %s declares capability %s, evaluator "
                        "is %s\n", path, h->capability, capability_id);
        return -1;
    }
    if (js_member(h->text, end, "cases", &vs, &ve) != 0 || *vs != '[') {
        fprintf(stderr, "heldout: fixture %s has no cases array\n", path);
        return -1;
    }
    p = vs + 1;
    for (;;) {
        const char *cs, *cend, *ids, *ide;
        p = js_ws(p, ve);
        if (p >= ve || *p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') {
            fprintf(stderr, "heldout: fixture %s has a non-object case\n", path);
            return -1;
        }
        cs = p;
        cend = js_value_end(p, ve);
        if (!cend) {
            fprintf(stderr, "heldout: fixture %s has a malformed case\n", path);
            return -1;
        }
        if (h->case_count >= CNET_HELDOUT_MAX_CASES) {
            fprintf(stderr, "heldout: fixture %s declares more than %d cases\n",
                    path, CNET_HELDOUT_MAX_CASES);
            return -1;
        }
        if (js_member(cs, cend, "id", &ids, &ide) != 0 ||
            js_string_copy(ids, ide, h->case_id[h->case_count],
                           CNET_HELDOUT_ID_MAX) != 0 ||
            h->case_id[h->case_count][0] == '\0') {
            fprintf(stderr, "heldout: fixture %s has a case without a string "
                            "id\n", path);
            return -1;
        }
        {
            size_t k;
            for (k = 0; k < h->case_count; k++)
                if (strcmp(h->case_id[k], h->case_id[h->case_count]) == 0) {
                    fprintf(stderr, "heldout: fixture %s repeats case id %s\n",
                            path, h->case_id[k]);
                    return -1;
                }
        }
        h->case_off[h->case_count] = (size_t)(cs - h->text);
        h->case_len[h->case_count] = (size_t)(cend - cs);
        h->case_count++;
        p = cend;
    }
    if (h->case_count == 0) {
        fprintf(stderr, "heldout: fixture %s declares no cases\n", path);
        return -1;
    }
    h->opened = 1;
    return 0;
}

void cnet_heldout_close(CnetHeldOut *h) {
    if (!h) return;
    free(h->text);
    h->text = NULL;
    h->len = 0;
    h->opened = 0;
}

static int heldout_index(CnetHeldOut *h, const char *case_id) {
    size_t i;
    for (i = 0; i < h->case_count; i++)
        if (strcmp(h->case_id[i], case_id) == 0) return (int)i;
    return -1;
}

/* Resolve `key` inside `case_id`. Returns 0 and the value span on success. */
static int heldout_value(CnetHeldOut *h, const char *case_id, const char *key,
                         const char **vs, const char **ve) {
    int idx;
    const char *cs, *ce;
    if (!h->opened) return -1;
    idx = heldout_index(h, case_id);
    if (idx < 0) {
        fprintf(stderr, "heldout: fixture %s has no case %s\n", h->path,
                case_id);
        h->errors++;
        return -1;
    }
    cs = h->text + h->case_off[idx];
    ce = cs + h->case_len[idx];
    if (js_member(cs, ce, key, vs, ve) != 0) {
        fprintf(stderr, "heldout: case %s has no key %s\n", case_id, key);
        h->errors++;
        return -1;
    }
    h->reads[idx]++;
    return 0;
}

double cnet_heldout_num(CnetHeldOut *h, const char *case_id, const char *key,
                        double fallback) {
    const char *vs, *ve;
    char buf[64];
    size_t n;
    char *endp;
    double v;
    if (!h || !h->required) return fallback;
    if (heldout_value(h, case_id, key, &vs, &ve) != 0) return fallback;
    n = (size_t)(ve - vs);
    if (n == 0 || n >= sizeof buf) {
        fprintf(stderr, "heldout: case %s key %s is not a number\n", case_id,
                key);
        h->errors++;
        return fallback;
    }
    memcpy(buf, vs, n);
    buf[n] = '\0';
    if (strcmp(buf, "true") == 0) return 1.0;
    if (strcmp(buf, "false") == 0) return 0.0;
    v = strtod(buf, &endp);
    if (endp == buf || *endp != '\0') {
        fprintf(stderr, "heldout: case %s key %s is not a number (%s)\n",
                case_id, key, buf);
        h->errors++;
        return fallback;
    }
    return v;
}

const char *cnet_heldout_str(CnetHeldOut *h, const char *case_id,
                             const char *key, char *out, size_t cap,
                             const char *fallback) {
    const char *vs, *ve;
    if (!h || !out || cap == 0) return fallback;
    if (!h->required) {
        snprintf(out, cap, "%s", fallback ? fallback : "");
        return out;
    }
    if (heldout_value(h, case_id, key, &vs, &ve) != 0) {
        snprintf(out, cap, "%s", fallback ? fallback : "");
        return out;
    }
    if (js_string_copy(vs, ve, out, cap) != 0) {
        fprintf(stderr, "heldout: case %s key %s is not a string\n", case_id,
                key);
        h->errors++;
        snprintf(out, cap, "%s", fallback ? fallback : "");
        return out;
    }
    return out;
}

size_t cnet_heldout_array_len(CnetHeldOut *h, const char *case_id,
                              const char *key, size_t fallback) {
    const char *vs, *ve, *p;
    size_t n = 0;
    if (!h || !h->required) return fallback;
    if (heldout_value(h, case_id, key, &vs, &ve) != 0) return fallback;
    if (*vs != '[') {
        fprintf(stderr, "heldout: case %s key %s is not an array\n", case_id,
                key);
        h->errors++;
        return fallback;
    }
    p = vs + 1;
    for (;;) {
        const char *e;
        p = js_ws(p, ve);
        if (p >= ve || *p == ']') break;
        if (*p == ',') { p++; continue; }
        e = js_value_end(p, ve);
        if (!e) {
            h->errors++;
            return fallback;
        }
        n++;
        p = e;
    }
    return n;
}

void cnet_heldout_verdict(CnetHeldOut *h, const char *case_id, int ok) {
    int idx;
    if (!h || !h->required || !h->opened) return;
    idx = heldout_index(h, case_id);
    if (idx < 0) {
        fprintf(stderr, "heldout: verdict for unknown case %s\n", case_id);
        h->errors++;
        return;
    }
    if (h->verdict[idx] == 0) return; /* a failure is never upgraded */
    h->verdict[idx] = (signed char)(ok ? 1 : 0);
}

int cnet_heldout_finish(CnetHeldOut *h) {
    size_t i, consumed = 0, passed = 0;
    if (!h || !h->required) return 0;
    if (!h->opened) return -1;
    for (i = 0; i < h->case_count; i++) {
        printf("HELDOUT_CASE id=%s reads=%u\n", h->case_id[i], h->reads[i]);
        if (h->reads[i] > 0) consumed++;
        if (h->verdict[i] == 1) passed++;
        else
            printf("HELDOUT_CASE_FAIL id=%s verdict=%s\n", h->case_id[i],
                   h->verdict[i] < 0 ? "not_evaluated" : "failed");
    }
    printf("HELDOUT_FIXTURE capability=%s sha256=%s cases=%zu consumed=%zu\n",
           h->capability, h->sha256, h->case_count, consumed);
    printf("HELDOUT_METRIC cases_passed=%zu cases_declared=%zu metric=%.6f "
           "errors=%zu\n",
           passed, h->case_count,
           h->case_count ? (double)passed / (double)h->case_count : 0.0,
           h->errors);
    if (h->errors || consumed != h->case_count || passed != h->case_count)
        return -1;
    return 0;
}
