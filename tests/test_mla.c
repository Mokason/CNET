/* DeepSeek MLA gate — make mla → MLA_PASS */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_mla.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    cce_mla_config cfg;
    cce_mla_weights w;
    cce_mla m;
    int dm = 256, n_h = 4, T = 8, t, i;
    float *x, *y, *y2;

    printf("== DeepSeek Multi-head Latent Attention (MLA) ==\n");

    cce_mla_config_default(&cfg, dm, n_h);
    cfg.qk_nope_head_dim = 32;
    cfg.qk_rope_head_dim = 16;
    cfg.v_head_dim = 32;
    cfg.kv_lora_rank = 64;
    cfg.use_absorb = 1;

    {
        size_t mla_b = cce_mla_cache_bytes_per_token(&cfg);
        size_t mha_b = cce_mha_cache_bytes_per_token(n_h, cfg.qk_nope_head_dim + cfg.qk_rope_head_dim);
        check(mla_b > 0 && mha_b > mla_b, "MLA cache smaller than dense MHA");
        printf("    cache/token: MLA %zu B vs MHA %zu B (%.1fx smaller)\n",
               mla_b, mha_b, (double)mha_b / (double)mla_b);
    }

    check(cce_mla_weights_alloc_synthetic(&w, &cfg, 0xA11CEu) == CCE_OK,
          "synthetic weights alloc");
    check(cce_mla_init(&m, &cfg, &w, 64) == CCE_OK, "mla init");

    x = (float*)malloc((size_t)T * dm * sizeof(float));
    y = (float*)malloc((size_t)T * dm * sizeof(float));
    y2 = (float*)malloc((size_t)T * dm * sizeof(float));
    check(x && y && y2, "scratch alloc");
    for (t = 0; t < T; ++t)
        for (i = 0; i < dm; ++i)
            x[(size_t)t * dm + i] = 0.01f * (float)((t + 1) * (i % 17) - 8);

    check(cce_mla_forward_seq(&m, x, T, y) == CCE_OK, "prefill forward");
    check(m.cache.cur_len == T, "cache length == T");
    {
        int finite = 1;
        double amax = 0;
        for (i = 0; i < T * dm; ++i) {
            if (!isfinite(y[i])) finite = 0;
            if (fabs((double)y[i]) > amax) amax = fabs((double)y[i]);
        }
        check(finite && amax > 0, "output finite and non-zero");
    }

    /* Determinism */
    cce_mla_reset_cache(&m);
    check(cce_mla_forward_seq(&m, x, T, y2) == CCE_OK, "repeat prefill");
    check(memcmp(y, y2, (size_t)T * dm * sizeof(float)) == 0,
          "repeat BIT-IDENTICAL");

    /* Decode step appends one token */
    {
        float x1[256], y1[256];
        int pos = T;
        for (i = 0; i < dm; ++i) x1[i] = 0.02f * (float)(i - 3);
        check(cce_mla_forward_token(&m, x1, pos, y1) == CCE_OK, "decode step");
        check(m.cache.cur_len == T + 1, "cache grew by 1");
        {
            int finite = 1;
            for (i = 0; i < dm; ++i) if (!isfinite(y1[i])) finite = 0;
            check(finite, "decode output finite");
        }
    }

    /* Absorb vs explicit up-project equivalence on scores path */
    {
        cce_mla m2;
        float *ya, *yb;
        cfg.use_absorb = 1;
        check(cce_mla_init(&m2, &cfg, &w, 64) == CCE_OK, "absorb mla init");
        ya = (float*)malloc((size_t)T * dm * sizeof(float));
        yb = (float*)malloc((size_t)T * dm * sizeof(float));
        check(cce_mla_forward_seq(&m2, x, T, ya) == CCE_OK, "absorb prefill");
        cce_mla_free(&m2);
        cfg.use_absorb = 0;
        check(cce_mla_init(&m2, &cfg, &w, 64) == CCE_OK, "explicit mla init");
        check(cce_mla_forward_seq(&m2, x, T, yb) == CCE_OK, "explicit prefill");
        {
            float dmax = 0.f;
            for (i = 0; i < T * dm; ++i) {
                float d = fabsf(ya[i] - yb[i]);
                if (d > dmax) dmax = d;
            }
            check(dmax < 1e-4f, "absorb ≡ explicit up-project");
            printf("    absorb vs explicit dmax=%.6g\n", dmax);
        }
        cce_mla_free(&m2);
        free(ya);
        free(yb);
        cfg.use_absorb = 1;
    }

    /* RoPE changes with position */
    {
        float a[16], b[16];
        for (i = 0; i < 16; ++i) a[i] = b[i] = 0.1f * (float)i;
        cce_mla_apply_rope(a, 16, 0, 10000.f);
        cce_mla_apply_rope(b, 16, 7, 10000.f);
        {
            float d = 0.f;
            for (i = 0; i < 16; ++i) d += fabsf(a[i] - b[i]);
            check(d > 1e-4f, "RoPE differs by position");
        }
    }

    /* ---- Audit regression RED: quant_kv on/off output parity (0a01b50).
     * The f32 and int8 latent dot paths must produce bit-close scores so
     * flipping quant_kv cannot move a token decision. */
    {
        cce_mla mf, mq;
        float *yf, *yq;
        cce_mla_config cf = cfg;
        cf.use_absorb = 1;
        check(cce_mla_init(&mf, &cf, &w, 64) == CCE_OK, "parity: f32 init");
        check(cce_mla_init(&mq, &cf, &w, 64) == CCE_OK, "parity: q8 init");
        check(cce_mla_enable_quant_kv(&mq) == 0, "parity: q8 armed");
        check(mq.cache.quant_kv == 1 && mq.cache.c_kv_q8 != NULL,
              "parity: q8 cache allocated");
        yf = (float*)malloc((size_t)T * dm * sizeof(float));
        yq = (float*)malloc((size_t)T * dm * sizeof(float));
        check(yf && yq, "parity: scratch alloc");
        check(cce_mla_forward_seq(&mf, x, T, yf) == CCE_OK, "parity: f32 prefill");
        check(cce_mla_forward_seq(&mq, x, T, yq) == CCE_OK, "parity: q8 prefill");
        {
            float dmax = 0.f;
            for (i = 0; i < T * dm; ++i) {
                float d = fabsf(yf[i] - yq[i]);
                if (d > dmax) dmax = d;
            }
            /* int8 KV round-trip accumulates < 1/127 per-element; the
             * attention softmax + V up-project keep the L2 distance small.
             * 1e-3 is a generous fail-closed bar — divergence above this
             * is the classic double-vs-float accumulator parity bug. */
            check(dmax < 1e-3f, "quant_kv on/off parity (decision-identity)");
            printf("    quant_kv parity dmax=%.6g\\n", dmax);
        }
        cce_mla_free(&mf);
        cce_mla_free(&mq);
        free(yf);
        free(yq);
    }

    cce_mla_free(&m);
    cce_mla_weights_free(&w);
    free(x);
    free(y);
    free(y2);

    printf("MLA_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
