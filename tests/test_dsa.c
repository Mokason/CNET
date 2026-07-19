/* Full DSA + lightning + MLA-lite gate — make dsa → DSA_PASS */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_dsa.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void dense_softmax(const float *s, int n, float *p) {
    float mx = s[0], sum = 0.f;
    int i;
    for (i = 1; i < n; ++i) if (s[i] > mx) mx = s[i];
    for (i = 0; i < n; ++i) {
        p[i] = expf(s[i] - mx);
        sum += p[i];
    }
    for (i = 0; i < n; ++i) p[i] /= sum;
}

int main(void) {
    float scores[16], idx[16], dens[16], w[16];
    int out_i[16], n = 16, out_n, i;
    cce_dsa_config cfg;
    float q[32], kpack[16 * 32];

    printf("== DSA full kernel (lightning + dual + MLA-lite) ==\n");

    for (i = 0; i < n; ++i) scores[i] = -2.0f + 0.15f * (float)i;
    scores[3] = 5.0f;
    scores[7] = 4.2f;
    scores[11] = -3.0f;

    cce_dsa_config_default(&cfg);
    cfg.fraction = 0.25f;
    cfg.keep_anchors = 0;
    cfg.floor_quantum = 0.f;

    check(cce_dsa_select_and_weights(scores, n, &cfg, out_i, w, n, &out_n) == CCE_OK,
          "DSA select OK");
    check(out_n >= 1 && out_n <= 4, "support <= resolve_k");
    {
        float sum = 0.f;
        int awake = 0;
        for (i = 0; i < out_n; ++i) {
            sum += w[i];
            if (w[i] > 0.f) awake++;
        }
        check(awake >= 1, "at least one awake");
        check(fabsf(sum - 1.f) < 1e-4f, "weights sum to 1");
    }

    /* Identity path */
    {
        float neg[4] = {-10.f, -1.f, -0.5f, -8.f};
        int ix[4];
        float ww[4];
        int on;
        cfg.fraction = 1.0f;
        cfg.floor_quantum = 0.f;
        cfg.sleep_eps = 0.f;
        check(cce_dsa_select_and_weights(neg, 4, &cfg, ix, ww, 4, &on) == CCE_OK,
              "identity select");
        check(on == 4, "identity k=n");
        dense_softmax(neg, 4, dens);
        {
            int match = 1;
            for (i = 0; i < 4; ++i)
                if (fabsf(ww[i] - dens[ix[i]]) > 1e-5f) match = 0;
            check(match, "identity ≡ dense softmax");
        }
    }

    /* Lightning indexer: multi-head ReLU produces finite scores */
    {
        int hd = 32;
        for (i = 0; i < hd; ++i) q[i] = 0.1f * (float)(i - 10);
        for (i = 0; i < 8; ++i) {
            int d;
            for (d = 0; d < hd; ++d)
                kpack[i * hd + d] = 0.05f * (float)(d + i) - 0.2f;
        }
        cce_dsa_config_default(&cfg);
        cfg.index_mode = CCE_DSA_INDEX_RELU_MH;
        cfg.index_heads = 4;
        check(cce_dsa_lightning_index(q, hd, kpack, 8, &cfg, 1.f / sqrtf(32.f),
                                      NULL, idx) == CCE_OK,
              "lightning ReLU-MH OK");
        {
            int finite = 1;
            for (i = 0; i < 8; ++i) if (!isfinite(idx[i])) finite = 0;
            check(finite, "lightning scores finite");
        }
        cfg.index_mode = CCE_DSA_INDEX_HYBRID;
        check(cce_dsa_lightning_index(q, hd, kpack, 8, &cfg, 1.f / sqrtf(32.f),
                                      NULL, idx) == CCE_OK,
              "lightning hybrid OK");
    }

    /* Dual: select by index, weight by true attn (different peaks) */
    {
        float isc[6] = {10.f, 0.f, 9.f, 1.f, 0.f, 0.f}; /* prefers 0,2 */
        float asc[6] = {0.f, 5.f, 0.1f, 4.f, 0.f, 0.f}; /* true mass on 1,3 */
        int ix[6];
        float ww[6];
        int on;
        cce_dsa_config_default(&cfg);
        cfg.fraction = 0.5f; /* k=3 */
        cfg.keep_anchors = 0;
        cfg.floor_quantum = 0.f;
        cfg.sleep_eps = 0.f;
        check(cce_dsa_select_dual(isc, asc, 6, &cfg, ix, ww, 6, &on) == CCE_OK,
              "dual select OK");
        check(on >= 1, "dual support non-empty");
        /* selected set should prefer high index scores */
        {
            int has0 = 0;
            for (i = 0; i < on; ++i) if (ix[i] == 0) has0 = 1;
            check(has0, "dual selects by index peak");
        }
    }

    /* MLA-lite quantize / dequant / dot */
    {
        float src[16], dst[16];
        int8_t q8[16];
        float sc, err = 0.f;
        for (i = 0; i < 16; ++i) src[i] = sinf(0.3f * (float)i);
        cce_mla_kv_quantize(src, 16, q8, &sc);
        cce_mla_kv_dequant(q8, sc, 16, dst);
        for (i = 0; i < 16; ++i) err += fabsf(src[i] - dst[i]);
        check(err / 16.f < 0.05f, "MLA int8 roundtrip mean abs err < 0.05");
        {
            float d_f = 0.f, d_q;
            for (i = 0; i < 16; ++i) d_f += src[i] * src[i];
            d_q = cce_mla_kv_dot_q8(src, q8, sc, 16);
            check(fabsf(d_f - d_q) / (fabsf(d_f) + 1e-3f) < 0.05f,
                  "MLA q8 dot ≈ f32 self-dot");
        }

        /* ---- Audit regression RED (6b9b5e1): cce_mla_kv_dot_q8 must match a
         * float accumulator reference (the in-line MLA f32 path uses float).
         * The classic bug is double-acc in the q8 helper vs float in the
         * attention loop → quant_kv on/off score divergence. We rebuild the
         * float reference EXACTLY as the production f32 attention loop
         * (wr accumulation in float), then require the q8 helper within 5e-5
         * of it — not of the infinite-precision dot, which is the weaker
         * check above. */
        {
            float ref_f = 0.f;
            for (i = 0; i < 16; ++i) {
                float kq = (float)q8[i] * sc;       /* dequant as float */
                ref_f += src[i] * kq;                /* float×float, float sum */
            }
            {
                float d_q = cce_mla_kv_dot_q8(src, q8, sc, 16);
                float diff = fabsf(ref_f - d_q);
                check(diff < 5e-5f,
                      "q8 dot uses float accumulator (matches MLA f32 path)");
                printf("    q8-f32acc diff=%.3g ref=%.6g q8=%.6g\\n",
                       diff, ref_f, d_q);
            }
        }
    }

    /* round_down keep one */
    {
        float u[3] = {1e-8f, 1e-9f, 1e-10f};
        check(cce_round_down_keep_one(u, 3, 0.01f, 1e-4f, 2) == 1, "all-dust one-hot");
        check(u[2] == 1.f, "fallback kept");
    }

    /* Quality default: no floor quantum */
    {
        cce_dsa_config qcfg, scfg;
        cce_dsa_config_default(&qcfg);
        cce_dsa_config_speed(&scfg);
        check(qcfg.floor_quantum == 0.f, "quality default floor off");
        check(scfg.floor_quantum > 0.f, "speed profile floor on");
    }

    printf("DSA_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
