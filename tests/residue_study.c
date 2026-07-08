/*
 * residue_study.c -- does composing one frozen transition generalize beyond
 * enumeration where a flat monolith can't? Composed path: train delta on its
 * k*b table, scan over the string. Flat path: train one net on S sampled
 * strings. Both evaluated on a held-out set neither trained on. Budgeted study;
 * NOT part of make test.
 *
 * Also includes length-extrapolation and extensibility (swap delta for other k).
 */
#include "residue_common.h"

#include <stdio.h>

#define B 4
#define K 7
#define HELDOUT 20000     /* held-out strings for evaluation */
#define FLAT_BUDGET 5000  /* strings the flat monolith trains on */

static unsigned int rng_state = 0xC0FFEEu;
static unsigned int rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5; return rng_state;
}
static void rand_string(int *digits, size_t n, int b) {
    size_t i; for (i = 0; i < n; ++i) digits[i] = (int)(rng() % (unsigned)b);
}

/* Bijection between an N-digit base-b string (MSB-first, matching string_residue)
   and an index in [0, b^N). Used only for the small N=8 head-to-head space so the
   disjointness bitset can enumerate it -- feasible only while b^N stays enumerable. */
static size_t string_index(const int *digits, size_t n, int b) {
    size_t i, idx = 0;
    for (i = 0; i < n; ++i) idx = idx * (size_t)b + (size_t)digits[i];
    return idx;
}
static void index_string(size_t idx, int *digits, size_t n, int b) {
    size_t i;
    for (i = n; i-- > 0; ) { digits[i] = (int)(idx % (size_t)b); idx /= (size_t)b; }
}

int main(void) {
    double dstep_in[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double dstep_tg[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta, flat;
    CertifyReport rep;
    size_t dsamples, t;
    const size_t N = 8;
    double *flat_in, *flat_tg;
    size_t composed_ok = 0, flat_ok = 0;
    static int heldout[HELDOUT][RES_MAX_N];
    size_t i, space_size, n_allowed, ix;
    unsigned char *seen;
    unsigned int *allowed;

    /* --- composed path: train + certify delta (k*b pairs) --- */
    memset(&delta, 0, sizeof delta);
    dsamples = build_residue_step_data(B, K, dstep_in, dstep_tg);
    train_residue_step(&delta, B, K, dstep_in, dstep_tg, dsamples, 12345u);
    if (certify_residue_step(&delta, K, dstep_in, dstep_tg, dsamples, 0.0, &rep) != 0) {
        fprintf(stderr, "delta did not certify; aborting study\n");
        btn_free(&delta); return 1;
    }
    printf("residue study: n mod %d, base %d, length N=%lu\n\n", K, B, (unsigned long)N);
    printf("delta certified %lu/%lu (margin %.4f) on %lu pairs\n\n",
           (unsigned long)rep.passed, (unsigned long)dsamples, rep.min_margin,
           (unsigned long)dsamples);

    /* --- held-out evaluation set --- */
    rng_state = 0xBEEF01u;
    for (t = 0; t < HELDOUT; ++t) rand_string(heldout[t], N, B);

    /* --- set-theoretic disjointness: mark every held-out string in a bitset over
       the whole b^N head-to-head space, then have the flat sampler draw from the
       COMPLEMENT (b^N minus held-out). This makes "held-out" literally leakage-free,
       not just stream-separated. Feasible only because N=8, b=4 keeps b^N = 65536
       enumerable -- the same enumerable boundary the study is about. --- */
    space_size = 1; for (i = 0; i < N; ++i) space_size *= (size_t)B;
    seen = (unsigned char *)calloc(space_size / 8 + 1, 1);
    allowed = (unsigned int *)malloc(space_size * sizeof *allowed);
    if (!seen || !allowed) { fprintf(stderr, "OOM\n"); return 1; }
    for (t = 0; t < HELDOUT; ++t) {
        ix = string_index(heldout[t], N, B);
        seen[ix >> 3] |= (unsigned char)(1u << (ix & 7));
    }
    n_allowed = 0;
    for (ix = 0; ix < space_size; ++ix)
        if (!(seen[ix >> 3] & (unsigned char)(1u << (ix & 7))))
            allowed[n_allowed++] = (unsigned int)ix;

    /* --- flat path: FLAT_BUDGET strings sampled (with replacement) from the
       held-out-cleared space -- distinct RNG stream AND zero held-out leakage --- */
    flat_in = malloc((size_t)FLAT_BUDGET * N * B * sizeof(double));
    flat_tg = malloc((size_t)FLAT_BUDGET * K * sizeof(double));
    if (!flat_in || !flat_tg) { fprintf(stderr, "OOM\n"); return 1; }
    rng_state = 0xF1A7u;
    for (t = 0; t < FLAT_BUDGET; ++t) {
        int digits[RES_MAX_N];
        index_string(allowed[rng() % n_allowed], digits, N, B);
        build_flat_row(B, K, N, digits, flat_in + t * N * B, flat_tg + t * K);
    }
    memset(&flat, 0, sizeof flat);
    train_flat(&flat, B, K, N, flat_in, flat_tg, FLAT_BUDGET, 777u);

    /* --- evaluate both on the held-out set --- */
    for (t = 0; t < HELDOUT; ++t) {
        int want = string_residue(heldout[t], N, B, K);
        if (run_scan(&delta, B, K, heldout[t], N) == want) ++composed_ok;
        if (flat_predict(&flat, B, N, heldout[t], K) == want) ++flat_ok;
    }

    printf(" path     | train data        | held-out exact (%d)\n", HELDOUT);
    printf(" ---------|-------------------|--------------------\n");
    printf(" composed | %lu transition pairs | %lu/%d (%.1f%%)\n",
           (unsigned long)dsamples, (unsigned long)composed_ok, HELDOUT,
           100.0 * composed_ok / HELDOUT);
    printf(" flat     | %d strings        | %lu/%d (%.1f%%)\n",
           FLAT_BUDGET, (unsigned long)flat_ok, HELDOUT,
           100.0 * flat_ok / HELDOUT);
    printf("\n composed trains on %lu pairs; flat on %d strings (ratio ~%.0fx more data)\n",
           (unsigned long)dsamples, FLAT_BUDGET, (double)FLAT_BUDGET / (double)dsamples);
    printf(" flat sampled from b^N=%lu minus the held-out set (%lu distinct strings cleared):"
           " zero leakage\n",
           (unsigned long)space_size, (unsigned long)(space_size - n_allowed));

    /* --- length-extrapolation: the SAME certified delta, composed at lengths
       far beyond anything trained; the flat net is fixed-width and N/A here. --- */
    {
        size_t lengths[] = {4, 8, 16, 32};
        size_t li;
        printf("\nlength-extrapolation (same delta, %lu pairs; flat is fixed-width, N/A):\n",
               (unsigned long)dsamples);
        printf(" N   | composed exact (5000 random)\n");
        printf(" ----|------------------------------\n");
        for (li = 0; li < sizeof lengths / sizeof lengths[0]; ++li) {
            size_t n = lengths[li], ok = 0, s;
            rng_state = 0x5A17u + (unsigned)n;
            for (s = 0; s < 5000; ++s) {
                int digits[RES_MAX_N];
                rand_string(digits, n, B);
                if (run_scan(&delta, B, K, digits, n) == string_residue(digits, n, B, K)) ++ok;
            }
            printf(" %-3lu | %lu/5000 (%.1f%%)\n", (unsigned long)n, (unsigned long)ok,
                   100.0 * ok / 5000.0);
        }
    }

    /* --- extensibility: swap delta for other moduli; the SAME scan composes. --- */
    {
        int moduli[] = {3, 5, 7};
        size_t mi;
        printf("\nextensibility (swap delta -> new modulus, scan unchanged):\n");
        printf(" k   | delta cert | composed exact at N=8 (5000 random)\n");
        printf(" ----|------------|------------------------------------\n");
        for (mi = 0; mi < sizeof moduli / sizeof moduli[0]; ++mi) {
            int kk = moduli[mi];
            double in2[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
            double tg2[RES_MAX_K * RES_MAX_B * RES_MAX_K];
            BinaryTransformNetwork d2;
            CertifyReport r2;
            size_t s2, ns, ok = 0;
            memset(&d2, 0, sizeof d2);
            ns = build_residue_step_data(B, kk, in2, tg2);
            train_residue_step(&d2, B, kk, in2, tg2, ns, 24680u + (unsigned)kk);
            s2 = (certify_residue_step(&d2, kk, in2, tg2, ns, 0.0, &r2) == 0);
            rng_state = 0x90D5u + (unsigned)kk;
            { size_t s; for (s = 0; s < 5000; ++s) {
                int digits[RES_MAX_N];
                rand_string(digits, 8, B);
                if (run_scan(&d2, B, kk, digits, 8) == string_residue(digits, 8, B, kk)) ++ok;
            } }
            printf(" %-3d | %s | %lu/5000 (%.1f%%)\n", kk,
                   s2 ? "yes" : "NO ", (unsigned long)ok, 100.0 * ok / 5000.0);
            btn_free(&d2);
        }
    }

    free(flat_in); free(flat_tg);
    free(seen); free(allowed);
    btn_free(&delta); btn_free(&flat);

    return 0;
}
