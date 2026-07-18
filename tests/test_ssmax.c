/* SSMax sparse softmax gate — dense tail mass is the pathology.
 * make ssmax → SSMAX_PASS
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_router.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Reference dense softmax (stable) for contrast — NOT used in production. */
static void dense_softmax(const float *logits, int n, float temp, float *probs) {
    float maxv, sum = 0.0f;
    int i;
    if (temp <= 0) temp = 1.0f;
    maxv = logits[0];
    for (i = 1; i < n; ++i)
        if (logits[i] > maxv) maxv = logits[i];
    for (i = 0; i < n; ++i) {
        probs[i] = expf((logits[i] - maxv) / temp);
        sum += probs[i];
    }
    for (i = 0; i < n; ++i) probs[i] /= sum;
}

int main(void) {
    float logits[16];
    float p_ss[16], p_dense[16];
    int i, n = 16;
    float sum;

    printf("== SSMax (DSA-inspired sparse softmax) ==\n");

    /* One clear winner + long weak tail */
    for (i = 0; i < n; ++i) logits[i] = -8.0f + 0.1f * (float)i;
    logits[3] = 5.0f;  /* peak */
    logits[7] = 4.5f;  /* near peak — may stay if top_k allows */
    logits[11] = -1.0f;

    cce_ssmax(logits, n, 1.0f, p_ss, 4);
    dense_softmax(logits, n, 1.0f, p_dense);

    sum = 0.0f;
    for (i = 0; i < n; ++i) sum += p_ss[i];
    check(fabsf(sum - 1.0f) < 1e-5f, "ssmax probs sum to 1");

    {
        int support = cce_ssmax_support_size(p_ss, n);
        int dense_nonzero = 0;
        for (i = 0; i < n; ++i)
            if (p_dense[i] > 1e-8f) dense_nonzero++;
        check(support >= 1 && support <= 4, "ssmax support <= top_k");
        check(support < dense_nonzero, "ssmax sparser than dense softmax");
        check(p_ss[3] > 0.0f, "peak keeps mass");
        /* Weak tail must be exact zero, not fog */
        check(p_ss[0] == 0.0f && p_ss[1] == 0.0f, "weak tail exact zeros");
        printf("    support_ssmax=%d support_dense~=%d peak_ss=%.4f peak_dense=%.4f\n",
               support, dense_nonzero, p_ss[3], p_dense[3]);
        check(p_ss[3] + 1e-5f >= p_dense[3], "peak mass not diluted vs dense");
    }

    /* top_k=1 → hard one-hot on argmax */
    cce_ssmax(logits, n, 1.0f, p_ss, 1);
    check(cce_ssmax_support_size(p_ss, n) == 1, "top_k=1 → support 1");
    check(fabsf(p_ss[3] - 1.0f) < 1e-5f, "top_k=1 mass on argmax");

    /* Relative margin kills near-miss members of top-k */
    {
        float L[5] = {10.0f, 9.9f, 0.0f, -5.0f, -20.0f};
        float P[5];
        /* margin 0.05 keeps only ~10.0, drops 9.9 */
        cce_ssmax_ex(L, 5, 1.0f, P, 3, 0.05f);
        check(P[0] > 0.99f, "tight margin concentrates on max");
        check(P[2] == 0.0f && P[3] == 0.0f && P[4] == 0.0f, "far scores zero");
    }

    /* Numerical: large logits don't explode */
    {
        float L[3] = {1000.0f, 999.0f, 0.0f};
        float P[3];
        cce_ssmax(L, 3, 1.0f, P, 2);
        check(isfinite(P[0]) && isfinite(P[1]) && isfinite(P[2]), "stable large logits");
        check(P[0] > 0.5f, "large-logit peak dominates");
        check(P[2] == 0.0f, "large gap → exact zero");
    }

    /* MoE path: topk_weights ≡ dense-softmax → top-k → renorm */
    {
        float L[8] = {1.0f, 3.5f, 0.2f, 4.0f, -1.0f, 2.0f, 0.5f, -5.0f};
        float dens[8], w[3];
        int sel[3], k, i, j;
        float ref_w[3], ref_sum = 0.0f;
        int ref_sel[3];
        unsigned char taken[8] = {0};

        dense_softmax(L, 8, 1.0f, dens);
        for (k = 0; k < 3; k++) {
            int best = -1;
            for (i = 0; i < 8; i++)
                if (!taken[i] && (best < 0 || dens[i] > dens[best])) best = i;
            taken[best] = 1;
            ref_sel[k] = best;
            ref_w[k] = dens[best];
            ref_sum += dens[best];
        }
        for (k = 0; k < 3; k++) ref_w[k] /= ref_sum;

        check(cce_ssmax_topk_weights(L, 8, 3, sel, w) == 3, "topk_weights returns k");
        {
            int match = 1;
            for (k = 0; k < 3; k++) {
                if (sel[k] != ref_sel[k]) match = 0;
                if (fabsf(w[k] - ref_w[k]) > 1e-5f) match = 0;
            }
            check(match, "topk_weights ≡ dense→top-k→renorm");
            printf("    sel=[%d,%d,%d] w=[%.4f,%.4f,%.4f]\n",
                   sel[0], sel[1], sel[2], w[0], w[1], w[2]);
        }
        /* selected set is top-3 by logit (same as by dense prob) */
        {
            int ok = 1;
            for (k = 0; k < 3; k++) {
                int found = 0;
                for (j = 0; j < 3; j++) if (sel[j] == ref_sel[k]) found = 1;
                if (!found) ok = 0;
            }
            check(ok, "topk selected set matches dense top-3");
        }
        {
            float ws = 0.0f;
            for (k = 0; k < 3; k++) ws += w[k];
            check(fabsf(ws - 1.0f) < 1e-5f, "topk weights sum to 1");
        }
    }

    /* equal logits → uniform over top_k */
    {
        float L[4] = {2.0f, 2.0f, 2.0f, 2.0f};
        float w[4];
        int sel[4], k;
        check(cce_ssmax_topk_weights(L, 4, 4, sel, w) == 4, "equal logits k=n");
        for (k = 0; k < 4; k++)
            check(fabsf(w[k] - 0.25f) < 1e-5f, "equal logits uniform 0.25");
    }

    /* sleep renorm: tiny residual → exact 0 (physics dead-zone), skip next cycle */
    {
        float w[4] = {0.9997f, 0.00025f, 5e-5f, 0.0f};
        int n;
        n = cce_sleep_renorm(w, 4, 1e-4f, 0);
        check(n == 2, "sleep keeps mass >= eps");
        check(w[2] == 0.0f && w[3] == 0.0f, "sub-eps exact zeros (skippable)");
        check(w[0] > 0.9f && w[1] > 0.0f, "survivors keep relative mass");
        check(fabsf(w[0] + w[1] - 1.0f) < 1e-5f, "sleep renorm sums to 1");
    }
    {
        float w[3] = {1e-7f, 1e-8f, 1e-9f};
        check(cce_sleep_renorm(w, 3, 1e-4f, 1) == 1, "all-asleep → one-hot");
        check(w[1] == 1.0f && w[0] == 0.0f && w[2] == 0.0f, "fallback one-hot");
    }
    cce_fp_enable_ftz_daz(); /* smoke: no crash on x86 / no-op elsewhere */
    check(1, "FTZ/DAZ enable is safe to call");

    printf("SSMAX_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
