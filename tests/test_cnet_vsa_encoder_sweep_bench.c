/* CNET-VSA encoder separability sweep: the gate that decides the default encoder.
 *
 * Markers: CNET_VSA_ENCODER_SWEEP_BENCH_PASS
 *          CNET_VSA_ENCODER_SWEEP_BENCH_SKIP   (no corpus directory on this host)
 *
 * For every corpus under $CNET_VSA_DISTILL_DIR (default var/distill) and every
 * implemented encoder, this runs the SAME calibration the seal path runs
 * (cnet_vsa_gencap_calibrate_ex: leave-one-out corpus sentences + held-out
 * probes vs negatives sampled from other corpora) and reports, per encoder:
 *   - separable fraction at three accept/reject policies
 *   - median achievable in-domain accept at reject >= 0.90
 *   - query encode latency (the runtime cost the router pays)
 *
 * Gate: CNET_VSA_ENCODER_DEFAULT must not be worse than BAG at the 0.80/0.90
 * policy. A candidate that wins on a hermetic pair but loses here does not
 * become the default. Never lower the policy to make this pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_gen_capsule.h"
#include "cnet_vsa_lexicon.h"

#define MAX_CORPORA   4096
#define MAX_LINES     512
#define MAX_PROBES    32
#define N_NEG         512
#define MIN_CORPORA   50

typedef struct {
    char name[128];
    char *lines[MAX_LINES];
    size_t n;
    char *probes[MAX_PROBES];
    size_t np;
} Corpus;

typedef struct { float t_in, t_neg; const char *label; } Policy;
static const Policy POLICIES[] = {
    { 0.90f, 0.95f, "0.90/0.95" },
    { 0.80f, 0.90f, "0.80/0.90" },
    { 0.70f, 0.90f, "0.70/0.90" },
};
#define N_POL (sizeof(POLICIES) / sizeof(POLICIES[0]))
#define GATE_POLICY 1 /* index of 0.80/0.90 */

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static char *trim_dup(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
    if (n == 0 || line[0] == '#') return NULL;
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, line, n);
    d[n] = '\0';
    return d;
}

static size_t read_lines(const char *path, char **out, size_t cap) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[2048];
    size_t n = 0;
    while (n < cap && fgets(buf, sizeof(buf), fp)) {
        char *d = trim_dup(buf);
        if (d) out[n++] = d;
    }
    fclose(fp);
    return n;
}

static int cmp_name(const void *a, const void *b) {
    return strcmp(((const Corpus *)a)->name, ((const Corpus *)b)->name);
}

static size_t load_corpora(const char *dir, Corpus *c, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    size_t n = 0;
    const char *suf = "_corpus.txt";
    size_t sl = strlen(suf);
    while ((de = readdir(d)) != NULL && n < cap) {
        size_t len = strlen(de->d_name);
        if (len <= sl || strcmp(de->d_name + len - sl, suf) != 0) continue;
        Corpus *k = &c[n];
        memset(k, 0, sizeof(*k));
        size_t nl = len - sl;
        if (nl >= sizeof(k->name)) nl = sizeof(k->name) - 1;
        memcpy(k->name, de->d_name, nl);
        k->name[nl] = '\0';
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        k->n = read_lines(path, k->lines, MAX_LINES);
        if (k->n < CNET_VSA_GENCAP_MIN_IN_DOMAIN) {
            for (size_t i = 0; i < k->n; ++i) free(k->lines[i]);
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s_probes.txt", dir, k->name);
        k->np = read_lines(path, k->probes, MAX_PROBES);
        n++;
    }
    closedir(d);
    qsort(c, n, sizeof(Corpus), cmp_name);
    return n;
}

/* deterministic negatives: N_NEG sentences from other corpora, seeded by index */
static size_t sample_negatives(const Corpus *c, size_t nc, size_t self, const char **out, size_t cap) {
    uint64_t s = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)self * 0xD1B54A32D192ED03ULL);
    size_t got = 0, guard = 0;
    if (nc < 2) return 0;
    while (got < cap && guard < cap * 20) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t ci = (size_t)(s % nc);
        guard++;
        if (ci == self) continue;
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t li = (size_t)(s % c[ci].n);
        out[got++] = c[ci].lines[li];
    }
    return got;
}

static int cmp_f(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* same rule as the library: r_in = k-th smallest in-domain, r_neg just below
 * the (j+1)-th smallest negative */
static double snap4(float t) { return floor((double)t * 1e4 + 0.5) / 1e4; }

static int separable(const float *d_in, size_t n_in, const float *d_neg, size_t n_neg, float t_in, float t_neg) {
    size_t k = (size_t)ceil(snap4(t_in) * (double)n_in - 1e-9);
    if (k == 0) k = 1;
    if (k > n_in) k = n_in;
    float r_in = d_in[k - 1];
    size_t j = (size_t)floor((1.0 - snap4(t_neg)) * (double)n_neg + 1e-9);
    if (j >= n_neg) j = n_neg - 1;
    float r_neg = d_neg[j] - 1e-4f;
    return r_in <= r_neg;
}

static float achievable_accept(const float *d_in, size_t n_in, const float *d_neg, size_t n_neg, float t_neg) {
    size_t j = (size_t)floor((1.0 - snap4(t_neg)) * (double)n_neg + 1e-9);
    if (j >= n_neg) j = n_neg - 1;
    float r_neg = d_neg[j] - 1e-4f;
    size_t ok = 0;
    for (size_t i = 0; i < n_in; ++i) if (d_in[i] <= r_neg) ok++;
    return (float)ok / (float)n_in;
}

typedef struct {
    size_t evaluated;
    size_t sep[N_POL];       /* float-512 space */
    size_t sep_b[N_POL];     /* wide binary space (v3 routing space) */
    float *ach;              /* achievable accept per corpus at reject 0.90, float space */
    float *ach_b;            /* same, binary space */
    double calib_us;         /* total calibrate time */
    double encode_us;        /* per-query float encode latency */
    double encode_bits_us;   /* per-query binary signature latency */
} EncResult;

int main(void) {
    const char *dir = getenv("CNET_VSA_DISTILL_DIR");
    if (!dir || !*dir) dir = "var/distill";
    size_t max_corp = 0;
    const char *lim = getenv("CNET_VSA_SWEEP_MAX");
    if (lim && *lim) max_corp = (size_t)atoi(lim);

    printf("=================================================================\n");
    printf(" CNET-VSA Encoder Separability Sweep (default-encoder gate)      \n");
    printf("=================================================================\n\n");

    /* CNET_VSA_LEXICON=<file> activates the learned lexicon so the LEX encoder
     * row is real; without it LEX refuses to encode and its row stays empty */
    static CnetVsaLexicon lexicon;
    const char *lexpath = getenv("CNET_VSA_LEXICON");
    if (lexpath && *lexpath) {
        int lrc = cnet_vsa_lexicon_load(&lexicon, lexpath);
        if (lrc != 0) { printf("  lexicon '%s' failed to load (rc=%d)\n", lexpath, lrc); return 1; }
        cnet_vsa_lexicon_set_active(&lexicon);
        printf("  lexicon: %s (%u words, beta %.2f, window %u, nnz %u, reflective %u, tag 0x%08x)\n",
               lexpath, lexicon.hdr.count, lexicon.hdr.beta, lexicon.hdr.window, lexicon.hdr.nnz,
               lexicon.hdr.reflective, (unsigned)(lexicon.hdr.digest & 0xffffffffu));
    } else {
        printf("  lexicon: none (LEX encoder skipped)\n");
    }

    Corpus *corp = (Corpus *)calloc(MAX_CORPORA, sizeof(Corpus));
    assert(corp);
    size_t loaded = load_corpora(dir, corp, MAX_CORPORA);
    size_t nc = (max_corp && loaded > max_corp) ? max_corp : loaded;
    printf("  corpus dir: %s  corpora: %zu  (min %d to run)\n", dir, nc, MIN_CORPORA);
    if (nc < MIN_CORPORA) {
        printf("\n CNET_VSA_ENCODER_SWEEP_BENCH_SKIP: fewer than %d corpora under %s\n", MIN_CORPORA, dir);
        free(corp);
        return 0;
    }
    size_t with_probes = 0;
    for (size_t i = 0; i < nc; ++i) if (corp[i].np > 0) with_probes++;
    printf("  corpora with held-out probe files: %zu\n", with_probes);

    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    assert(cap);
    const char **negs = (const char **)calloc(N_NEG, sizeof(char *));
    float *d_in = (float *)calloc(MAX_LINES + MAX_PROBES, sizeof(float));
    float *d_neg = (float *)calloc(N_NEG, sizeof(float));
    assert(negs && d_in && d_neg);

    EncResult res[CNET_VSA_ENCODER_COUNT];
    memset(res, 0, sizeof(res));

    /* query encode latency: a realistic 9-word question, 20k encodes */
    const char *q = "how does residual stress in multilayer ceramic coatings affect thermal shock resistance";
    for (uint32_t e = 0; e < CNET_VSA_ENCODER_COUNT; ++e) {
        if (e == CNET_VSA_ENCODER_LEX && !cnet_vsa_lexicon_active()) continue;
        float v[CNET_VSA_DEFAULT_DIM];
        double t0 = now_us();
        for (int i = 0; i < 20000; ++i) {
            assert(cnet_vsa_gencap_encode_intent_ex(q, v, CNET_VSA_DEFAULT_DIM, e) == 0);
        }
        res[e].encode_us = (now_us() - t0) / 20000.0;
        int8_t bits[CNET_VSA_TOPICAL_DIM];
        t0 = now_us();
        for (int i = 0; i < 20000; ++i) {
            assert(cnet_vsa_gencap_encode_intent_q8(q, bits, e) == 0);
        }
        res[e].encode_bits_us = (now_us() - t0) / 20000.0;
    }
    float *b_in = (float *)calloc(MAX_LINES + MAX_PROBES, sizeof(float));
    float *b_neg = (float *)calloc(N_NEG, sizeof(float));
    assert(b_in && b_neg);

    const char *all_env = getenv("CNET_VSA_SWEEP_ALL");
    int sweep_all = (all_env && *all_env && strcmp(all_env, "0") != 0) ? 1 : 0;
    printf("  encoders swept: %s (CNET_VSA_SWEEP_ALL=1 sweeps every encoder)\n",
           sweep_all ? "all" : "bag + default");
    for (uint32_t e = 0; e < CNET_VSA_ENCODER_COUNT; ++e) {
        res[e].ach = (float *)calloc(nc, sizeof(float));
        res[e].ach_b = (float *)calloc(nc, sizeof(float));
        assert(res[e].ach && res[e].ach_b);
        if (!sweep_all && e != CNET_VSA_ENCODER_BAG && e != CNET_VSA_ENCODER_DEFAULT) continue;
        if (e == CNET_VSA_ENCODER_LEX && !cnet_vsa_lexicon_active()) continue;
        double t0 = now_us();
        for (size_t ci = 0; ci < nc; ++ci) {
            const Corpus *k = &corp[ci];
            assert(cnet_vsa_gencap_init(cap, k->name, "SWEEP", CNET_VSA_DEFAULT_DIM) == 0);
            assert(cnet_vsa_gencap_set_encoder(cap, e) == 0);
            for (size_t i = 0; i < k->n; ++i) cnet_vsa_gencap_ingest(cap, k->lines[i]);
            size_t nn = sample_negatives(corp, nc, ci, negs, N_NEG);
            CnetVsaCalibDistances fs, bs;
            fs.d_in = d_in; fs.in_cap = MAX_LINES + MAX_PROBES; fs.d_neg = d_neg; fs.neg_cap = N_NEG;
            bs.d_in = b_in; bs.in_cap = MAX_LINES + MAX_PROBES; bs.d_neg = b_neg; bs.neg_cap = N_NEG;
            int rc = cnet_vsa_gencap_calibrate_dual(cap,
                                                    (const char *const *)k->lines, k->n,
                                                    (const char *const *)k->probes, k->np,
                                                    negs, nn, 0.80f, 0.90f, &fs, &bs);
            if (rc == CNET_VSA_GENCAP_INSUFFICIENT_EVIDENCE || rc == -1 || rc == -4) continue;
            size_t n_in = fs.in_n, n_neg = fs.neg_n;
            /* distances are sorted by the library */
            res[e].evaluated++;
            for (size_t p = 0; p < N_POL; ++p) {
                if (separable(d_in, n_in, d_neg, n_neg, POLICIES[p].t_in, POLICIES[p].t_neg)) res[e].sep[p]++;
                if (separable(b_in, bs.in_n, b_neg, bs.neg_n, POLICIES[p].t_in, POLICIES[p].t_neg)) res[e].sep_b[p]++;
            }
            res[e].ach[ci] = achievable_accept(d_in, n_in, d_neg, n_neg, 0.90f);
            res[e].ach_b[ci] = achievable_accept(b_in, bs.in_n, b_neg, bs.neg_n, 0.90f);
        }
        res[e].calib_us = now_us() - t0;
        qsort(res[e].ach, nc, sizeof(float), cmp_f);
        qsort(res[e].ach_b, nc, sizeof(float), cmp_f);
        printf("  encoder %-8s evaluated %4zu corpora in %.1f s\n",
               cnet_vsa_encoder_name(e), res[e].evaluated, res[e].calib_us / 1e6);
    }

    printf("\n  float-512 space (v1/v2 routing, and mixed registries):\n");
    printf("  | encoder  | %s | %s | %s | median achievable accept @reject 0.90 | encode us/query |\n",
           POLICIES[0].label, POLICIES[1].label, POLICIES[2].label);
    printf("  |----------|-----------|-----------|-----------|----------------|-----------------|\n");
    for (uint32_t e = 0; e < CNET_VSA_ENCODER_COUNT; ++e) {
        if (!res[e].evaluated) continue;
        size_t n = res[e].evaluated;
        printf("  | %-8s | %4zu (%4.1f%%) | %4zu (%4.1f%%) | %4zu (%4.1f%%) | %.3f | %.2f |%s\n",
               cnet_vsa_encoder_name(e),
               res[e].sep[0], 100.0 * (double)res[e].sep[0] / (double)n,
               res[e].sep[1], 100.0 * (double)res[e].sep[1] / (double)n,
               res[e].sep[2], 100.0 * (double)res[e].sep[2] / (double)n,
               res[e].ach[nc / 2], res[e].encode_us,
               e == CNET_VSA_ENCODER_DEFAULT ? "  <- default" : "");
    }
    printf("\n  wide int8 space (%d-d, v3 routing):\n", CNET_VSA_TOPICAL_DIM);
    printf("  | encoder  | %s | %s | %s | median achievable accept @reject 0.90 | signature us/query |\n",
           POLICIES[0].label, POLICIES[1].label, POLICIES[2].label);
    printf("  |----------|-----------|-----------|-----------|----------------|-----------------|\n");
    for (uint32_t e = 0; e < CNET_VSA_ENCODER_COUNT; ++e) {
        if (!res[e].evaluated) continue;
        size_t n = res[e].evaluated;
        printf("  | %-8s | %4zu (%4.1f%%) | %4zu (%4.1f%%) | %4zu (%4.1f%%) | %.3f | %.2f |%s\n",
               cnet_vsa_encoder_name(e),
               res[e].sep_b[0], 100.0 * (double)res[e].sep_b[0] / (double)n,
               res[e].sep_b[1], 100.0 * (double)res[e].sep_b[1] / (double)n,
               res[e].sep_b[2], 100.0 * (double)res[e].sep_b[2] / (double)n,
               res[e].ach_b[nc / 2], res[e].encode_bits_us,
               e == CNET_VSA_ENCODER_DEFAULT ? "  <- default" : "");
    }

    /* gate: default not worse than bag at the gate policy, and default encode cost
     * within 4x of bag (routing must stay cheap) */
    size_t bag_sep = res[CNET_VSA_ENCODER_BAG].sep[GATE_POLICY];
    size_t def_sep = res[CNET_VSA_ENCODER_DEFAULT].sep[GATE_POLICY];
    double bag_us = res[CNET_VSA_ENCODER_BAG].encode_us;
    double def_us = res[CNET_VSA_ENCODER_DEFAULT].encode_us;
    printf("\n  gate: default '%s' separable %zu vs bag %zu at %s; encode %.2f us vs bag %.2f us\n",
           cnet_vsa_encoder_name(CNET_VSA_ENCODER_DEFAULT), def_sep, bag_sep,
           POLICIES[GATE_POLICY].label, def_us, bag_us);

    size_t def_sep_b = res[CNET_VSA_ENCODER_DEFAULT].sep_b[GATE_POLICY];
    printf("  gate: default binary space separable %zu vs its float space %zu at %s (binary must not be worse)\n",
           def_sep_b, def_sep, POLICIES[GATE_POLICY].label);
    size_t floor_n = (size_t)(0.95 * (double)nc);
    int enough = (res[CNET_VSA_ENCODER_BAG].evaluated >= floor_n) && (res[CNET_VSA_ENCODER_DEFAULT].evaluated >= floor_n);
    int wide_fast = (res[CNET_VSA_ENCODER_DEFAULT].encode_bits_us <= 4.0 * res[CNET_VSA_ENCODER_DEFAULT].encode_us + 1.0);
    printf("  gate: evaluated bag %zu / default %zu of %zu corpora (floor %zu); wide encode %.2f us vs float %.2f us (<= 4x + 1)\n",
           res[CNET_VSA_ENCODER_BAG].evaluated, res[CNET_VSA_ENCODER_DEFAULT].evaluated, nc, floor_n,
           res[CNET_VSA_ENCODER_DEFAULT].encode_bits_us, res[CNET_VSA_ENCODER_DEFAULT].encode_us);
    int ok = ((def_sep >= bag_sep) && (def_us <= 4.0 * bag_us + 1.0) && (def_sep_b >= def_sep)) && enough && wide_fast;

    for (size_t i = 0; i < loaded; ++i) {
        for (size_t j = 0; j < corp[i].n; ++j) free(corp[i].lines[j]);
        for (size_t j = 0; j < corp[i].np; ++j) free(corp[i].probes[j]);
    }
    for (uint32_t e = 0; e < CNET_VSA_ENCODER_COUNT; ++e) { free(res[e].ach); free(res[e].ach_b); }
    free(corp); free(cap); free(negs); free(d_in); free(d_neg); free(b_in); free(b_neg);

    if (!ok) {
        printf("\n CNET_VSA_ENCODER_SWEEP_BENCH_FAIL: default encoder worse than bag, too slow, or binary space worse than float\n");
        return 1;
    }
    printf("\n=================================================================\n");
    printf(" CNET_VSA_ENCODER_SWEEP_BENCH_PASS: default encoder holds the separability floor\n");
    printf("=================================================================\n");
    return 0;
}
