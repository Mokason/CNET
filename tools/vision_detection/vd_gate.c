/* Fit and evaluate the continuous-domain coverage gate.
 *
 *   --fit    build the reference set from the certified training features and
 *            calibrate tau on the calibration slice (disjoint from fitting and
 *            from the final holdout), then publish gate.bin
 *   --eval   score a pack against a published gate and report admission
 *
 * Scoring is embarrassingly parallel and order-independent: every proposal is
 * scored into its own slot from a fixed partition, so the result does not
 * depend on thread count or scheduling.
 *
 * Protocol: plans/cnet_vision_portable_specialist_20260728.md
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vd_coverage.h"
#include "vd_io.h"
#include "vd_pack.h"
#include "vd_sha256.h"

#define REF_SEED 20260728ULL
#define REF_N    4096
#define GATE_K   8
#define GATE_Q   0.95

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    const VdCoverage *cov;
    const VdPack *pk;
    double *out;
    size_t *index;      /* flat proposal index -> (img, prop) */
    size_t n;
    size_t lo, hi;
} Job;

static void *worker(void *arg) {
    Job *j = (Job *)arg;
    size_t t;
    for (t = j->lo; t < j->hi; t++) {
        size_t im = j->index[2 * t], pr = j->index[2 * t + 1];
        const VdPackImg *I = &j->pk->imgs[im];
        double x[VD_MAX_DIM];
        int d;
        for (d = 0; d < j->pk->dim; d++) x[d] = I->pf[pr * (size_t)j->pk->dim + d];
        j->out[t] = vd_cov_score(j->cov, x);
    }
    return NULL;
}

static size_t flatten(const VdPack *pk, size_t **idx_out) {
    size_t n = 0, i, k = 0;
    size_t *idx;
    for (i = 0; i < pk->n; i++) n += pk->imgs[i].n_prop;
    idx = (size_t *)malloc(n * 2 * sizeof(size_t));
    if (!idx) return 0;
    for (i = 0; i < pk->n; i++) {
        size_t p;
        for (p = 0; p < pk->imgs[i].n_prop; p++) { idx[2*k] = i; idx[2*k+1] = p; k++; }
    }
    *idx_out = idx;
    return n;
}

static double *score_pack(const VdCoverage *cov, const VdPack *pk, size_t *n_out,
                          int threads, double *secs) {
    size_t *idx = NULL, n = flatten(pk, &idx), t;
    double *out;
    pthread_t th[64];
    Job jobs[64];
    double t0 = now_s();
    if (!n) { free(idx); return NULL; }
    out = (double *)malloc(n * sizeof(double));
    if (!out) { free(idx); return NULL; }
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    for (t = 0; t < (size_t)threads; t++) {
        jobs[t].cov = cov; jobs[t].pk = pk; jobs[t].out = out;
        jobs[t].index = idx; jobs[t].n = n;
        jobs[t].lo = n * t / (size_t)threads;
        jobs[t].hi = n * (t + 1) / (size_t)threads;
        if (pthread_create(&th[t], NULL, worker, &jobs[t]) != 0) { jobs[t].hi = jobs[t].lo; th[t] = 0; }
    }
    for (t = 0; t < (size_t)threads; t++) if (th[t]) pthread_join(th[t], NULL);
    free(idx);
    if (secs) *secs = now_s() - t0;
    *n_out = n;
    return out;
}

/* Deterministic reference draw: a seeded stride over every training proposal,
   so the set depends only on the pack contents and the protocol seed. */
static double *draw_reference(const VdPack *pk, size_t want, size_t *got) {
    size_t total = 0, i, k = 0;
    double *ref;
    uint64_t st = REF_SEED;
    for (i = 0; i < pk->n; i++) total += pk->imgs[i].n_prop;
    if (total == 0) return NULL;
    if (want > total) want = total;
    ref = (double *)malloc(want * (size_t)pk->dim * sizeof(double));
    if (!ref) return NULL;
    {
        size_t *idx = NULL, n = flatten(pk, &idx);
        size_t j;
        if (!n) { free(ref); return NULL; }
        /* Fisher-Yates prefix of length `want` over the flat index */
        for (j = n; j > 1 && k < want; j--) {
            size_t r = (size_t)(splitmix(&st) % j);
            size_t ai = idx[2*(j-1)], ap = idx[2*(j-1)+1];
            idx[2*(j-1)] = idx[2*r]; idx[2*(j-1)+1] = idx[2*r+1];
            idx[2*r] = ai; idx[2*r+1] = ap;
        }
        for (j = 0; j < want; j++) {
            const VdPackImg *I = &pk->imgs[idx[2*j]];
            int d;
            for (d = 0; d < pk->dim; d++)
                ref[k * (size_t)pk->dim + d] = I->pf[idx[2*j+1] * (size_t)pk->dim + d];
            k++;
        }
        free(idx);
    }
    *got = k;
    return ref;
}

static int write_gate(const char *path, const double *ref, size_t n, size_t dim,
                      size_t k, double tau) {
    size_t blob = 4 * sizeof(uint64_t) + n * dim * sizeof(double);
    unsigned char *buf = (unsigned char *)malloc(blob);
    uint64_t hdr[4];
    int rc;
    if (!buf) return -1;
    hdr[0] = (uint64_t)n; hdr[1] = (uint64_t)dim; hdr[2] = (uint64_t)k;
    memcpy(&hdr[3], &tau, sizeof(double));
    memcpy(buf, hdr, sizeof hdr);
    memcpy(buf + sizeof hdr, ref, n * dim * sizeof(double));
    rc = vd_publish_file(path, buf, blob);
    free(buf);
    return rc;
}

int main(int argc, char **argv) {
    const char *train = NULL, *cal = NULL, *evalp = NULL, *gate = NULL;
    int threads = 8, i;
    int do_fit = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fit")) do_fit = 1;
        else if (!strcmp(argv[i], "--train") && i + 1 < argc) train = argv[++i];
        else if (!strcmp(argv[i], "--cal") && i + 1 < argc) cal = argv[++i];
        else if (!strcmp(argv[i], "--eval") && i + 1 < argc) evalp = argv[++i];
        else if (!strcmp(argv[i], "--gate") && i + 1 < argc) gate = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
    }
    printf("vd_gate: CPU-only (ROCR/HIP/CUDA_VISIBLE_DEVICES empty) threads=%d\n", threads);

    if (do_fit) {
        VdPack tp, cp;
        VdCoverage cov;
        double *ref, *sc, tau, secs = 0;
        size_t nref = 0, nsc = 0;
        if (!train || !cal || !gate) { printf("VD_GATE_FAIL fit_needs_train_cal_gate\n"); return 2; }
        if (vd_pack_load(train, &tp) != VD_PACK_OK) { printf("VD_GATE_FAIL train_pack\n"); return 3; }
        ref = draw_reference(&tp, REF_N, &nref);
        if (!ref) { printf("VD_GATE_FAIL reference_draw\n"); return 3; }
        printf("reference: %zu rows x %d dims (seed %llu)\n", nref, tp.dim,
               (unsigned long long)REF_SEED);
        if (vd_cov_init(&cov, ref, nref, (size_t)tp.dim, GATE_K, INFINITY) != 0) {
            printf("VD_GATE_FAIL cov_init\n"); return 3;
        }
        if (vd_pack_load(cal, &cp) != VD_PACK_OK) { printf("VD_GATE_FAIL cal_pack\n"); return 3; }
        sc = score_pack(&cov, &cp, &nsc, threads, &secs);
        if (!sc) { printf("VD_GATE_FAIL cal_score\n"); return 3; }
        {   /* non-finite scores mean unusable vectors; they are refusals, not data */
            size_t j, ok = 0;
            for (j = 0; j < nsc; j++) if (isfinite(sc[j])) sc[ok++] = sc[j];
            printf("calibration: %zu proposals, %zu scorable\n", nsc, ok);
            tau = vd_cov_quantile(sc, ok, GATE_Q);
        }
        printf("tau(q=%.2f) = %.9f   calibration_score_seconds=%.1f\n", GATE_Q, tau, secs);
        if (write_gate(gate, ref, nref, (size_t)tp.dim, GATE_K, tau) != 0) {
            printf("VD_GATE_FAIL gate_publish\n"); return 4;
        }
        {
            char hex[65];
            if (vd_sha256_file(gate, hex) == 0) printf("gate sha256 %s\n", hex);
        }
        printf("VD_GATE_FIT_OK rows=%zu dim=%d k=%d tau=%.9f\n", nref, tp.dim, GATE_K, tau);
        vd_cov_free(&cov); free(ref); free(sc);
        vd_pack_free(&tp); vd_pack_free(&cp);
        return 0;
    }

    if (evalp && gate) {
        VdPack pk;
        VdCoverage cov;
        unsigned char *buf = NULL;
        size_t nsc = 0, admitted = 0, unusable = 0, j;
        double *sc, secs = 0, tau = 0;
        uint64_t hdr[4];
        FILE *f = fopen(gate, "rb");
        long sz;
        if (!f) { printf("VD_GATE_FAIL gate_open\n"); return 3; }
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        buf = (unsigned char *)malloc((size_t)sz);
        if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { printf("VD_GATE_FAIL gate_read\n"); return 3; }
        fclose(f);
        memcpy(hdr, buf, sizeof hdr);
        memcpy(&tau, &hdr[3], sizeof(double));
        if (vd_cov_init(&cov, (const double *)(buf + sizeof hdr), (size_t)hdr[0],
                        (size_t)hdr[1], (size_t)hdr[2], tau) != 0) {
            printf("VD_GATE_FAIL cov_init\n"); return 3;
        }
        if (vd_pack_load(evalp, &pk) != VD_PACK_OK) { printf("VD_GATE_FAIL eval_pack\n"); return 3; }
        if ((size_t)pk.dim != (size_t)hdr[1]) { printf("VD_GATE_FAIL dim_mismatch\n"); return 3; }
        sc = score_pack(&cov, &pk, &nsc, threads, &secs);
        if (!sc) { printf("VD_GATE_FAIL eval_score\n"); return 3; }
        for (j = 0; j < nsc; j++) {
            if (!isfinite(sc[j])) { unusable++; continue; }
            if (sc[j] <= tau) admitted++;
        }
        {   /* score distribution: the diagnosis lives here, not in the rate */
            double *fin = (double *)malloc(nsc * sizeof(double));
            size_t nf = 0, q;
            if (fin) {
                for (q = 0; q < nsc; q++) if (isfinite(sc[q])) fin[nf++] = sc[q];
                if (nf) {
                    double med = vd_cov_quantile(fin, nf, 0.5);
                    double p05 = fin[(size_t)(0.05 * (double)(nf - 1))];
                    double p95 = fin[(size_t)(0.95 * (double)(nf - 1))];
                    printf("VD_GATE_DIST median=%.6f p05=%.6f p95=%.6f min=%.6f max=%.6f\n",
                           med, p05, p95, fin[0], fin[nf - 1]);
                }
                free(fin);
            }
        }
        printf("VD_GATE_EVAL pack=%s proposals=%zu admitted=%zu refused=%zu unusable=%zu "
               "admit_rate=%.6f refuse_rate=%.6f tau=%.9f seconds=%.1f us_per_proposal=%.3f\n",
               evalp, nsc, admitted, nsc - admitted, unusable,
               nsc ? (double)admitted / (double)nsc : 0.0,
               nsc ? (double)(nsc - admitted) / (double)nsc : 0.0,
               tau, secs, nsc ? secs * 1e6 / (double)nsc : 0.0);
        vd_cov_free(&cov); free(sc); free(buf); vd_pack_free(&pk);
        return 0;
    }
    printf("VD_GATE_FAIL no_mode\n");
    return 2;
}
