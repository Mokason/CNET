/*
 * tests/test_conformal.c -- evidence for the split-conformal reject-option.
 *
 * The keystone is the DISTRIBUTION-FREE marginal coverage guarantee: averaged
 * over reshuffles, empirical coverage >= 1 - alpha for every alpha, on a
 * synthetic classifier with genuine Bayes error. Plus the error-reject tradeoff
 * (accepted predictions are more accurate than the unconditional classifier)
 * and BTN integration. Standalone via `make conformal`, or under `make test`.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/contract/conformal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define TWO_PI 6.283185307179586476

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* ---- deterministic RNG (fixed-seed LCG + Box-Muller) -------------------- */
static unsigned long long g_rng = 1;
static void   rng_seed(unsigned long long s) { g_rng = s ? s : 1; }
static double rng_unif(void) {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((g_rng >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
}
static double rng_gauss(void) {
    double u1 = rng_unif(), u2 = rng_unif();
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0 * log(u1)) * cos(TWO_PI * u2);
}

/* A synthetic classifier: true class y ~ U{0..K-1}; logits get a `margin` on y
   plus Gaussian `noise` everywhere (so the argmax is wrong some of the time --
   genuine Bayes error). Returns y, writes the softmax probs. */
static size_t gen_sample(size_t K, double margin, double noise, double *probs) {
    double logits[16];
    size_t j, y = (size_t)(rng_unif() * (double)K);
    if (y >= K) y = K - 1;
    for (j = 0; j < K; ++j) logits[j] = (j == y ? margin : 0.0) + noise * rng_gauss();
    conformal_softmax(logits, K, probs);
    return y;
}

static double avg_coverage(size_t K, double margin, double noise, double alpha,
                           int reshuffles, size_t n_cal, size_t n_test) {
    double total = 0.0;
    int r;
    for (r = 0; r < reshuffles; ++r) {
        double *scores = (double *)malloc(n_cal * sizeof(double));
        double probs[16];
        size_t i, cov = 0;
        double q;
        for (i = 0; i < n_cal; ++i) { size_t y = gen_sample(K, margin, noise, probs); scores[i] = 1.0 - probs[y]; }
        q = conformal_quantile(scores, n_cal, alpha);
        free(scores);
        for (i = 0; i < n_test; ++i) {
            int set[16];
            size_t y = gen_sample(K, margin, noise, probs);
            conformal_set_from_probs(probs, K, q, set, NULL);
            if (set[y]) ++cov;
        }
        total += (double)cov / (double)n_test;
    }
    return total / (double)reshuffles;
}

int run_test_conformal(void);
int run_test_conformal(void) {
    failures = 0;
    printf("== conformal reject-option: distribution-free abstention ==\n");

    /* ---- 1. quantile formula ---------------------------------------------- */
    {
        double sc[10] = {1.0,0.2,0.5,0.9,0.1,0.7,0.3,0.8,0.4,0.6};  /* 0.1..1.0 */
        /* alpha=0.2: k=ceil(11*0.8)=9 -> 9th smallest = 0.9 */
        CHECK(fabs(conformal_quantile(sc, 10, 0.2) - 0.9) < 1e-9,
              "quantile: k=ceil((n+1)(1-a)) picks the 9th-smallest score (0.9)");
        /* alpha=0.5: k=ceil(11*0.5)=6 -> 6th smallest = 0.6 */
        CHECK(fabs(conformal_quantile(sc, 10, 0.5) - 0.6) < 1e-9,
              "quantile: alpha=0.5 picks the 6th-smallest score (0.6)");
        /* alpha=0.01: k=ceil(11*0.99)=11 > n -> threshold admits all classes */
        CHECK(conformal_quantile(sc, 10, 0.01) >= 1.0,
              "quantile: k>n -> threshold >= 1 (cannot certify a singleton)");
    }

    /* ---- 2. prediction-set construction ----------------------------------- */
    {
        double p[3] = {0.7, 0.2, 0.1};
        int set[3], single = -9;
        size_t sz = conformal_set_from_probs(p, 3, 0.5, set, &single);  /* s=(.3,.8,.9) */
        CHECK(sz == 1 && set[0] && !set[1] && !set[2] && single == 0,
              "set: q=0.5 -> singleton {class0}");
        sz = conformal_set_from_probs(p, 3, 0.85, set, &single);        /* s<=.85 */
        CHECK(sz == 2 && set[0] && set[1] && !set[2] && single == -1,
              "set: q=0.85 -> {class0,class1}, not a singleton");
    }

    /* ---- 3. THE guarantee: marginal coverage >= 1 - alpha (any alpha) ------ */
    {
        double cov05, cov10, cov20;
        rng_seed(20260623ULL);
        cov05 = avg_coverage(5, 2.0, 1.5, 0.05, 30, 400, 400);
        cov10 = avg_coverage(5, 2.0, 1.5, 0.10, 30, 400, 400);
        cov20 = avg_coverage(5, 2.0, 1.5, 0.20, 30, 400, 400);
        printf("  [cov] empirical coverage: alpha0.05 -> %.4f | 0.10 -> %.4f | 0.20 -> %.4f\n",
               cov05, cov10, cov20);
        CHECK(cov05 >= 0.95 - 0.01, "distribution-free coverage holds at alpha=0.05 (>= 0.95)");
        CHECK(cov10 >= 0.90 - 0.01, "distribution-free coverage holds at alpha=0.10 (>= 0.90)");
        CHECK(cov20 >= 0.80 - 0.01, "distribution-free coverage holds at alpha=0.20 (>= 0.80)");
        CHECK(cov05 >= cov20 - 1e-9, "tighter alpha yields at least as much coverage (monotone)");
    }

    /* ---- 4. the reject option WORKS: accepted predictions are more accurate */
    {
        size_t K = 5, n_cal = 2000, n_test = 4000, i;
        double probs[16];
        double *scores = (double *)malloc(n_cal * sizeof(double));
        double q, overall_err = 0, acc_err = 0; size_t accepted = 0;
        rng_seed(99ULL);
        for (i = 0; i < n_cal; ++i) { size_t y = gen_sample(K, 2.0, 1.5, probs); scores[i] = 1.0 - probs[y]; }
        q = conformal_quantile(scores, n_cal, 0.10);
        free(scores);
        for (i = 0; i < n_test; ++i) {
            size_t y = gen_sample(K, 2.0, 1.5, probs), j, am = 0; int single;
            for (j = 1; j < K; ++j) if (probs[j] > probs[am]) am = j;
            if (am != y) overall_err += 1.0;
            if (conformal_set_from_probs(probs, K, q, NULL, &single) == 1) {
                ++accepted;
                if (single != (int)y) acc_err += 1.0;
            }
        }
        {
            double overall_rate = overall_err / (double)n_test;
            double acc_rate = accepted ? acc_err / (double)accepted : 0.0;
            double abstain = 1.0 - (double)accepted / (double)n_test;
            printf("  [reject] overall_err=%.3f | accepted_err=%.3f | abstain_rate=%.3f\n",
                   overall_rate, acc_rate, abstain);
            CHECK(acc_rate < overall_rate,
                  "reject option lowers error: accepted (singleton) predictions beat the raw classifier");
            CHECK(abstain > 0.0 && abstain < 1.0, "abstention is non-trivial (some accept, some reject)");
        }
    }

    /* ---- 5. BTN integration: calibrate on a real net, sane decisions ------- */
    {
        enum { N = 8 };
        BinaryTransformNetwork id;
        double in8[N * N], tg8[N * N];
        size_t i, j;
        for (i = 0; i < N * N; ++i) { in8[i] = 0.0; tg8[i] = 0.0; }
        for (i = 0; i < N; ++i) { in8[i * N + i] = 1.0; tg8[i * N + i] = 1.0; }
        btn_init(&id, N, N, N + 6, (N + 6) * 2, 0.5, 1234u);
        { Port p; p.family = PORT_ONEHOT; p.field_width = N; p.field_count = 1; p.tag[0] = 0;
          btn_set_ports(&id, p, p); }
        btn_train_dynamic(&id, in8, tg8, N, 60000, 800, 0.0003, 0.0008);
        btn_train(&id, in8, tg8, N, 8000);

        Contract c8;
        CHECK(contract_init_borrowed(&c8, "id8", &id, in8, tg8, N) == 0,
              "BTN classifier contract built (ONEHOT8 -> ONEHOT8)");

        /* calibration: noisy one-hot inputs, true label = the hot class */
        size_t ncal = 240;
        double *cin = (double *)malloc(ncal * N * sizeof(double));
        size_t *ctrue = (size_t *)malloc(ncal * sizeof(size_t));
        rng_seed(7ULL);
        for (i = 0; i < ncal; ++i) {
            size_t y = (size_t)(rng_unif() * (double)N); if (y >= N) y = N - 1;
            for (j = 0; j < N; ++j) cin[i * N + j] = (j == y ? 1.0 : 0.0) + 0.15 * rng_gauss();
            ctrue[i] = y;
        }
        ConformalCalibrator cal;
        int rc = conformal_calibrate_btn(&cal, &id, &c8, cin, ctrue, ncal, 0.10);
        CHECK(rc == 0 && cal.valid && cal.classes == N,
              "conformal_calibrate_btn calibrates against the live net");
        CHECK(fabs(conformal_coverage_level(&cal) - 0.90) < 1e-9,
              "certified coverage level = 1 - alpha = 0.90 (distribution-free)");

        /* clean confident inputs -> accept the correct class */
        size_t accepted_correct = 0;
        for (i = 0; i < N; ++i) {
            double x[N]; for (j = 0; j < N; ++j) x[j] = (j == i ? 1.0 : 0.0);
            if (conformal_classify_or_abstain(&cal, &id, &c8, x) == (int)i) ++accepted_correct;
        }
        CHECK(accepted_correct >= 6,
              "clean confident inputs are accepted with the correct class (>= 6/8)");

        /* maximally ambiguous input -> abstain */
        double unif[N]; for (j = 0; j < N; ++j) unif[j] = 1.0 / (double)N;
        CHECK(conformal_classify_or_abstain(&cal, &id, &c8, unif) == -1,
              "a maximally ambiguous input ABSTAINS (no singleton)");

        /* ---- benchmark: calibrate + classify throughput ------------------- */
        {
            const int iters = 200;
            clock_t t0 = clock();
            int it; volatile int sink = 0;
            for (it = 0; it < iters; ++it) {
                ConformalCalibrator c2;
                conformal_calibrate_btn(&c2, &id, &c8, cin, ctrue, ncal, 0.10);
                for (i = 0; i < N; ++i) {
                    double x[N]; for (j = 0; j < N; ++j) x[j] = (j == i ? 1.0 : 0.0);
                    sink += conformal_classify_or_abstain(&c2, &id, &c8, x);
                }
            }
            clock_t t1 = clock();
            double us = (double)(t1 - t0) / CLOCKS_PER_SEC / iters * 1e6;
            printf("  [bench] calibrate(240) + 8 classifies: %.2f us/round (%d rounds), sink=%d\n",
                   us, iters, sink);
            CHECK(us < 5000.0, "calibration + classification is sub-5ms (CPU-only, no GPU)");
        }

        free(cin); free(ctrue);
        contract_free(&c8);
        btn_free(&id);
    }

    printf("== conformal tests done: %d failure(s) ==\n", failures);
    return failures;
}

#ifndef TEST_ALL
int main(void) { return run_test_conformal() == 0 ? 0 : 1; }
#endif

