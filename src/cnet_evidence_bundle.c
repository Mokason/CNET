/*
 * cnet_evidence_bundle.c — per-unit evidence bundles + JSONL sidecar store.
 */
#include "../include/cnet_evidence_bundle.h"
#include "../include/acquire.h"   /* cnet_runtime_libs_digest */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- FNV-1a (same family as CNB / contract seals) ---- */

static uint64_t ev_fnv(const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- compact SHA-256 (buffer) ---- */

typedef struct {
    uint32_t h[8];
    unsigned char block[64];
    size_t block_len;
    uint64_t total_len;
} EvSha;

static const uint32_t ev_sha_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t ev_rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void ev_sha_transform(EvSha *s, const unsigned char b[64]) {
    uint32_t w[64], a, bb, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ev_rotr(w[i - 15], 7) ^ ev_rotr(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = ev_rotr(w[i - 2], 17) ^ ev_rotr(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; bb = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ev_rotr(e, 6) ^ ev_rotr(e, 11) ^ ev_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + ev_sha_k[i] + w[i];
        {
            uint32_t S0 = ev_rotr(a, 2) ^ ev_rotr(a, 13) ^ ev_rotr(a, 22);
            uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
            t2 = S0 + maj;
        }
        h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void ev_sha_init(EvSha *s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u; s->h[2] = 0x3c6ef372u;
    s->h[3] = 0xa54ff53au; s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->block_len = 0; s->total_len = 0;
}

static void ev_sha_update(EvSha *s, const unsigned char *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        s->block[s->block_len++] = p[i];
        if (s->block_len == 64) {
            ev_sha_transform(s, s->block);
            s->block_len = 0;
        }
    }
    s->total_len += n;
}

static void ev_sha_final(EvSha *s, unsigned char out[32]) {
    uint64_t bits = s->total_len * 8ULL;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    int i;
    ev_sha_update(s, &pad, 1);
    while (s->block_len != 56) ev_sha_update(s, &zero, 1);
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    ev_sha_update(s, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
}

static void ev_sha256(const unsigned char *p, size_t n, unsigned char out[32]) {
    EvSha s;
    ev_sha_init(&s);
    if (p && n) ev_sha_update(&s, p, n);
    ev_sha_final(&s, out);
}

static void hex32(const unsigned char in[32], char out[65]) {
    static const char *h = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2]     = h[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = h[in[i] & 0xf];
    }
    out[64] = '\0';
}

static int parse_hex32(const char *hex, unsigned char out[32]) {
    int i;
    if (!hex || strlen(hex) < 64) return -1;
    for (i = 0; i < 32; i++) {
        unsigned v = 0;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        if (a >= '0' && a <= '9') v = (unsigned)(a - '0') << 4;
        else if (a >= 'a' && a <= 'f') v = (unsigned)(a - 'a' + 10) << 4;
        else if (a >= 'A' && a <= 'F') v = (unsigned)(a - 'A' + 10) << 4;
        else return -1;
        if (b >= '0' && b <= '9') v |= (unsigned)(b - '0');
        else if (b >= 'a' && b <= 'f') v |= (unsigned)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') v |= (unsigned)(b - 'A' + 10);
        else return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

static void apply_opts(CnetEvidenceBundle *b, const CnetEvidenceOpts *opts) {
    if (!opts) {
        b->counterfactual_stability = -1.0f;
        return;
    }
    b->toolchain_digest = opts->toolchain_digest;
    b->counterfactual_stability = opts->counterfactual_stability;
    b->recipe_fp = opts->recipe_fp;
    b->rollback_artifact_digest = opts->rollback_artifact_digest;
    if (opts->rollback_unit && opts->rollback_unit[0]) {
        snprintf(b->rollback_unit, sizeof b->rollback_unit, "%s",
                 opts->rollback_unit);
    }
}

static uint64_t dataset_hash_from_contract(const Contract *c) {
    size_t in_tot = 0, out_tot = 0, i;
    uint64_t h;
    if (!c || c->exemplar_count == 0) return 0;
    for (i = 0; i < c->input_port_count; ++i)
        in_tot += c->input_ports[i].field_width * c->input_ports[i].field_count;
    for (i = 0; i < c->output_port_count; ++i)
        out_tot += c->output_ports[i].field_width *
                   c->output_ports[i].field_count;
    h = ev_fnv(c->inputs, c->exemplar_count * in_tot * sizeof(double));
    h ^= ev_fnv(c->outputs, c->exemplar_count * out_tot * sizeof(double));
    h *= 1099511628211ULL;
    h ^= (uint64_t)c->exemplar_count;
    return h ? h : 1ull;
}

static void fill_reliability(CnetEvidenceBundle *b,
                             const BinaryTransformNetwork *btn) {
    unsigned long s = 0, f = 0;
    if (btn) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
        s = (unsigned long)btn->output_successes;
        f = (unsigned long)btn->output_failures;
#else
        s = btn->output_successes;
        f = btn->output_failures;
#endif
    }
    b->reliability_successes = s;
    b->reliability_failures = f;
    /* Laplace: (s+1)/(s+f+2) */
    b->reliability = (double)(s + 1ull) / (double)(s + f + 2ull);
}

int cnet_evidence_bundle_from_parts(
    const char *unit_name,
    const BinaryTransformNetwork *btn,
    const Contract *c,
    const unsigned char *blob_bytes, size_t blob_len,
    const CnetEvidenceOpts *opts,
    CnetEvidenceBundle *out) {
    if (!unit_name || !unit_name[0] || !btn || !c || !out) return -1;
    memset(out, 0, sizeof *out);
    out->version = CNET_EVIDENCE_BUNDLE_VERSION;
    snprintf(out->unit_name, sizeof out->unit_name, "%s", unit_name);
    out->behavior_digest = (uint64_t)contract_btn_digest(btn);
    out->contract_digest = (uint64_t)contract_content_digest(c);
    out->dataset_hash = dataset_hash_from_contract(c);
    if (blob_bytes && blob_len > 0) {
        out->artifact_digest = ev_fnv(blob_bytes, blob_len);
        if (!out->artifact_digest) out->artifact_digest = 1;
        ev_sha256(blob_bytes, blob_len, out->artifact_sha256);
    }
    out->runtime_libs_digest = cnet_runtime_libs_digest();
    fill_reliability(out, btn);
    apply_opts(out, opts);
    out->complete = cnet_evidence_bundle_is_complete(out);
    return 0;
}

static int find_unit_index(const CnetBase *b, const char *name) {
    size_t i;
    if (!b || !name) return -1;
    for (i = 0; i < b->unit_count; ++i)
        if (strcmp(b->units[i].name, name) == 0) return (int)i;
    return -1;
}

int cnet_evidence_bundle_from_base(const CnetBase *b, const char *unit_name,
                                  const CnetEvidenceOpts *opts,
                                  CnetEvidenceBundle *out) {
    BinaryTransformNetwork btn;
    Contract c;
    int ui;
    const CnbBlob *blob;
    const char *prov;
    if (!b || !unit_name || !out) return -1;
    ui = find_unit_index(b, unit_name);
    if (ui < 0) return -1;
    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    if (cnb_get_unit(b, unit_name, &btn, &c) != 0) return -1;
    blob = &b->blobs[b->units[(size_t)ui].blob_index];
    if (cnet_evidence_bundle_from_parts(unit_name, &btn, &c, blob->bytes,
                                        blob->len, opts, out) != 0) {
        btn_free(&btn);
        contract_free(&c);
        return -1;
    }
    /* Prefer base-recorded behavior digest when present. */
    if (b->units[(size_t)ui].behavior_digest)
        out->behavior_digest =
            (uint64_t)b->units[(size_t)ui].behavior_digest;
    if (blob->digest) out->artifact_digest = (uint64_t)blob->digest;
    prov = cnb_unit_provenance(b, unit_name);
    if (prov && prov[0])
        snprintf(out->provenance, sizeof out->provenance, "%s", prov);
    /* Overlay stats if stored for this unit. */
    {
        size_t i;
        for (i = 0; i < b->stats_count; ++i) {
            if (strcmp(b->stats[i].name, unit_name) == 0) {
                out->reliability_successes =
                    (unsigned long)b->stats[i].successes;
                out->reliability_failures =
                    (unsigned long)b->stats[i].failures;
                out->reliability =
                    (double)(b->stats[i].successes + 1ull) /
                    (double)(b->stats[i].successes + b->stats[i].failures +
                             2ull);
                break;
            }
        }
    }
    out->complete = cnet_evidence_bundle_is_complete(out);
    btn_free(&btn);
    contract_free(&c);
    return 0;
}

int cnet_evidence_bundle_is_complete(const CnetEvidenceBundle *b) {
    if (!b) return 0;
    if (!b->unit_name[0]) return 0;
    if (!b->behavior_digest || !b->contract_digest || !b->dataset_hash ||
        !b->artifact_digest)
        return 0;
    return 1;
}

int cnet_evidence_bundle_verify(const CnetBase *base,
                                const CnetEvidenceBundle *b) {
    CnetEvidenceBundle live;
    int i;
    if (!base || !b) return -2;
    if (cnet_evidence_bundle_from_base(base, b->unit_name, NULL, &live) != 0)
        return -1;
    if (live.behavior_digest != b->behavior_digest) return -1;
    if (live.contract_digest != b->contract_digest) return -1;
    if (live.dataset_hash != b->dataset_hash) return -1;
    if (live.artifact_digest != b->artifact_digest) return -1;
    for (i = 0; i < 32; i++)
        if (live.artifact_sha256[i] != b->artifact_sha256[i]) return -1;
    return 0;
}

int cnet_evidence_bundle_format_json(const CnetEvidenceBundle *b, char *buf,
                                     size_t cap) {
    char sha[65];
    int n;
    if (!b || !buf || cap < 64) return -1;
    hex32(b->artifact_sha256, sha);
    n = snprintf(
        buf, cap,
        "{\"version\":%d,\"unit\":\"%s\",\"provenance\":\"%s\","
        "\"behavior_digest\":\"%016llx\",\"contract_digest\":\"%016llx\","
        "\"dataset_hash\":\"%016llx\",\"artifact_digest\":\"%016llx\","
        "\"artifact_sha256\":\"%s\","
        "\"toolchain_digest\":\"%016llx\",\"runtime_libs_digest\":\"%016llx\","
        "\"reliability_successes\":%lu,\"reliability_failures\":%lu,"
        "\"reliability\":%.6f,\"counterfactual_stability\":%.6f,"
        "\"rollback_unit\":\"%s\",\"rollback_artifact_digest\":\"%016llx\","
        "\"recipe_fp\":\"%016llx\",\"complete\":%d}",
        b->version, b->unit_name, b->provenance,
        (unsigned long long)b->behavior_digest,
        (unsigned long long)b->contract_digest,
        (unsigned long long)b->dataset_hash,
        (unsigned long long)b->artifact_digest, sha,
        (unsigned long long)b->toolchain_digest,
        (unsigned long long)b->runtime_libs_digest,
        b->reliability_successes, b->reliability_failures, b->reliability,
        (double)b->counterfactual_stability, b->rollback_unit,
        (unsigned long long)b->rollback_artifact_digest,
        (unsigned long long)b->recipe_fp, b->complete ? 1 : 0);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

int cnet_evidence_store_path_for_base(const char *base_path, char *out,
                                      size_t cap) {
    size_t n;
    if (!base_path || !base_path[0] || !out || cap < 8) return -1;
    n = strlen(base_path);
    if (n + 16 >= cap) return -1;
    snprintf(out, cap, "%s.evidence.jsonl", base_path);
    return 0;
}

const char *cnet_evidence_store_path_from_env(void) {
    const char *p = getenv("CNET_EVIDENCE_STORE");
    if (!p || !p[0]) return NULL;
    return p;
}

/* Minimal field extractors from a single JSON object line. */
static int json_get_str(const char *line, const char *key, char *out,
                        size_t cap) {
    char pat[96];
    const char *p, *q;
    size_t klen;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(line, pat);
    if (!p) {
        out[0] = '\0';
        return -1;
    }
    p += strlen(pat);
    q = p;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1]) q += 2;
        else q++;
    }
    klen = (size_t)(q - p);
    if (klen + 1 > cap) return -1;
    memcpy(out, p, klen);
    out[klen] = '\0';
    return 0;
}

static int json_get_u64_hex(const char *line, const char *key, uint64_t *out) {
    char pat[96], tmp[32];
    const char *p;
    char *end = NULL;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(line, pat);
    if (!p) {
        *out = 0;
        return -1;
    }
    p += strlen(pat);
    memcpy(tmp, p, 16);
    tmp[16] = '\0';
    *out = strtoull(tmp, &end, 16);
    return 0;
}

static int json_get_ulong(const char *line, const char *key,
                          unsigned long *out) {
    char pat[96];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(line, pat);
    if (!p) {
        *out = 0;
        return -1;
    }
    p += strlen(pat);
    *out = strtoul(p, NULL, 10);
    return 0;
}

static int json_get_double(const char *line, const char *key, double *out) {
    char pat[96];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(line, pat);
    if (!p) {
        *out = 0;
        return -1;
    }
    p += strlen(pat);
    *out = strtod(p, NULL);
    return 0;
}

static int parse_line(const char *line, CnetEvidenceBundle *b) {
    char sha[80];
    double cf = -1.0, rel = 0.5;
    int ver = 1, complete = 0;
    const char *vp;
    memset(b, 0, sizeof *b);
    b->version = CNET_EVIDENCE_BUNDLE_VERSION;
    b->counterfactual_stability = -1.0f;
    if (json_get_str(line, "unit", b->unit_name, sizeof b->unit_name) != 0)
        return -1;
    (void)json_get_str(line, "provenance", b->provenance, sizeof b->provenance);
    (void)json_get_u64_hex(line, "behavior_digest", &b->behavior_digest);
    (void)json_get_u64_hex(line, "contract_digest", &b->contract_digest);
    (void)json_get_u64_hex(line, "dataset_hash", &b->dataset_hash);
    (void)json_get_u64_hex(line, "artifact_digest", &b->artifact_digest);
    if (json_get_str(line, "artifact_sha256", sha, sizeof sha) == 0)
        (void)parse_hex32(sha, b->artifact_sha256);
    (void)json_get_u64_hex(line, "toolchain_digest", &b->toolchain_digest);
    (void)json_get_u64_hex(line, "runtime_libs_digest", &b->runtime_libs_digest);
    (void)json_get_ulong(line, "reliability_successes",
                         &b->reliability_successes);
    (void)json_get_ulong(line, "reliability_failures",
                         &b->reliability_failures);
    if (json_get_double(line, "reliability", &rel) == 0) b->reliability = rel;
    if (json_get_double(line, "counterfactual_stability", &cf) == 0)
        b->counterfactual_stability = (float)cf;
    (void)json_get_str(line, "rollback_unit", b->rollback_unit,
                       sizeof b->rollback_unit);
    (void)json_get_u64_hex(line, "rollback_artifact_digest",
                           &b->rollback_artifact_digest);
    (void)json_get_u64_hex(line, "recipe_fp", &b->recipe_fp);
    vp = strstr(line, "\"version\":");
    if (vp) ver = (int)strtol(vp + 10, NULL, 10);
    b->version = ver > 0 ? ver : CNET_EVIDENCE_BUNDLE_VERSION;
    vp = strstr(line, "\"complete\":");
    if (vp) complete = (int)strtol(vp + 11, NULL, 10);
    b->complete = complete || cnet_evidence_bundle_is_complete(b);
    return 0;
}

int cnet_evidence_store_load(const char *path, CnetEvidenceBundle **out,
                             size_t *out_n) {
    FILE *f;
    char line[2048];
    CnetEvidenceBundle *arr = NULL;
    size_t n = 0, acap = 0;
    if (!path || !out || !out_n) return -1;
    *out = NULL;
    *out_n = 0;
    f = fopen(path, "r");
    if (!f) return 0; /* empty store */
    while (fgets(line, sizeof line, f)) {
        CnetEvidenceBundle b;
        if (parse_line(line, &b) != 0) continue;
        if (n == acap) {
            size_t nc = acap ? acap * 2 : 8;
            CnetEvidenceBundle *na =
                (CnetEvidenceBundle *)realloc(arr, nc * sizeof *na);
            if (!na) {
                free(arr);
                fclose(f);
                return -1;
            }
            arr = na;
            acap = nc;
        }
        arr[n++] = b;
    }
    fclose(f);
    *out = arr;
    *out_n = n;
    return 0;
}

int cnet_evidence_store_put(const char *path, const CnetEvidenceBundle *bundle) {
    CnetEvidenceBundle *arr = NULL;
    size_t n = 0, i, found = (size_t)-1;
    FILE *f;
    char line[2048];
    if (!path || !path[0] || !bundle || !bundle->unit_name[0]) return -1;
    if (cnet_evidence_store_load(path, &arr, &n) != 0) return -1;
    for (i = 0; i < n; ++i) {
        if (strcmp(arr[i].unit_name, bundle->unit_name) == 0) {
            found = i;
            break;
        }
    }
    if (found != (size_t)-1) {
        arr[found] = *bundle;
    } else {
        CnetEvidenceBundle *na =
            (CnetEvidenceBundle *)realloc(arr, (n + 1) * sizeof *na);
        if (!na) {
            free(arr);
            return -1;
        }
        arr = na;
        arr[n++] = *bundle;
    }
    f = fopen(path, "w");
    if (!f) {
        free(arr);
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (cnet_evidence_bundle_format_json(&arr[i], line, sizeof line) != 0) {
            fclose(f);
            free(arr);
            return -1;
        }
        if (fprintf(f, "%s\n", line) < 0) {
            fclose(f);
            free(arr);
            return -1;
        }
    }
    free(arr);
    if (fclose(f) != 0) return -1;
    return 0;
}

int cnet_evidence_store_get(const char *path, const char *unit_name,
                            CnetEvidenceBundle *out) {
    CnetEvidenceBundle *arr = NULL;
    size_t n = 0, i;
    if (!path || !unit_name || !out) return -1;
    if (cnet_evidence_store_load(path, &arr, &n) != 0) return -1;
    for (i = 0; i < n; ++i) {
        if (strcmp(arr[i].unit_name, unit_name) == 0) {
            *out = arr[i];
            free(arr);
            return 0;
        }
    }
    free(arr);
    return -1;
}

int cnet_evidence_record(const CnetBase *base, const char *unit_name,
                         const char *store_path,
                         const CnetEvidenceOpts *opts) {
    CnetEvidenceBundle b;
    char pathbuf[576];
    const char *path = store_path;
    if (!base || !unit_name) return -1;
    if (!path || !path[0]) path = cnet_evidence_store_path_from_env();
    if (!path || !path[0]) return -1;
    if (cnet_evidence_bundle_from_base(base, unit_name, opts, &b) != 0)
        return -1;
    /* keep pathbuf unused unless caller passed relative special; no-op */
    (void)pathbuf;
    return cnet_evidence_store_put(path, &b);
}
