#include "../../include/cce/cce_mla.h"
#include "../../include/cce/cce_router.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void cce_mla_config_default(cce_mla_config* cfg, int d_model, int n_heads) {
    if (!cfg) return;
    if (n_heads < 1) n_heads = 8;
    if (d_model < n_heads) d_model = n_heads * 64;
    cfg->n_heads = n_heads;
    cfg->n_kv_heads = n_heads;
    cfg->qk_nope_head_dim = 64;
    cfg->qk_rope_head_dim = 32;
    cfg->v_head_dim = 64;
    cfg->kv_lora_rank = 128; /* DeepSeek uses 512 at scale; 128 for local */
    cfg->q_lora_rank = 0;
    cfg->d_model = d_model;
    cfg->rope_theta = 10000.0f;
    cfg->attn_scale = 0.0f; /* auto */
    cfg->use_absorb = 1;
}

size_t cce_mla_cache_bytes_per_token(const cce_mla_config* cfg) {
    if (!cfg) return 0;
    return (size_t)(cfg->kv_lora_rank + cfg->qk_rope_head_dim) * sizeof(float);
}

size_t cce_mha_cache_bytes_per_token(int n_kv_heads, int head_dim) {
    if (n_kv_heads < 1 || head_dim < 1) return 0;
    return (size_t)(2 * n_kv_heads * head_dim) * sizeof(float);
}

void cce_mla_matvec(const float* W, const float* x, float* y, int in, int out) {
    int o, i;
    for (o = 0; o < out; ++o) {
        float s = 0.0f;
        const float* row = W + (size_t)o * in;
        for (i = 0; i < in; ++i) s += row[i] * x[i];
        y[o] = s;
    }
}

void cce_mla_apply_rope(float* x, int rope_dim, int pos, float theta) {
    int i, half;
    if (!x || rope_dim < 2) return;
    if (theta <= 0.0f) theta = 10000.0f;
    half = rope_dim / 2;
    /* Interleaved pair layout: (x0,x1), (x2,x3), ... common in many stacks.
     * DeepSeek indexer uses non-interleaved; MLA main path often half-split.
     * Use half-split: first half real, second imag — DeepSeek MLA demo style. */
    for (i = 0; i < half; ++i) {
        float freq = 1.0f / powf(theta, (float)(2 * i) / (float)rope_dim);
        float ang = (float)pos * freq;
        float c = cosf(ang), s = sinf(ang);
        float a = x[i], b = x[i + half];
        x[i] = a * c - b * s;
        x[i + half] = a * s + b * c;
    }
}

static void mla_rms_norm(float* x, int n, const float* w, float eps) {
    int i;
    float ss = 0.0f, inv;
    if (eps <= 0.0f) eps = 1e-6f;
    for (i = 0; i < n; ++i) ss += x[i] * x[i];
    inv = 1.0f / sqrtf(ss / (float)n + eps);
    for (i = 0; i < n; ++i) {
        float v = x[i] * inv;
        x[i] = w ? v * w[i] : v;
    }
}

static uint32_t mla_lcg(uint32_t* s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static float mla_randn(uint32_t* s) {
    /* Box-Muller lite via uniform */
    float u = (float)(mla_lcg(s) >> 8) / (float)(1u << 24);
    float v = (float)(mla_lcg(s) >> 8) / (float)(1u << 24);
    if (u < 1e-7f) u = 1e-7f;
    return sqrtf(-2.0f * logf(u)) * cosf(6.28318530718f * v) * 0.02f;
}

cce_result cce_mla_weights_alloc_synthetic(cce_mla_weights* w,
                                           const cce_mla_config* cfg,
                                           uint32_t seed) {
    int dm, kr, n_h, nope, rope, vd, q_in, i, n;
    float* buf;
    uint32_t s = seed ? seed : 0xC0FFEEu;

    if (!w || !cfg) return CCE_ERR_INVALID_ARG;
    memset(w, 0, sizeof(*w));
    dm = cfg->d_model;
    kr = cfg->kv_lora_rank;
    n_h = cfg->n_heads;
    nope = cfg->qk_nope_head_dim;
    rope = cfg->qk_rope_head_dim;
    vd = cfg->v_head_dim;
    q_in = cfg->q_lora_rank > 0 ? cfg->q_lora_rank : dm;

    n = dm * (kr + rope)           /* w_dkv packs c_kv | k_pe */
      + kr * (n_h * nope)          /* w_uk */
      + kr * (n_h * vd)            /* w_uv */
      + q_in * (n_h * nope)        /* w_uq */
      + q_in * (n_h * rope)        /* w_qr */
      + (n_h * vd) * dm;           /* w_o */
    if (cfg->q_lora_rank > 0) n += dm * cfg->q_lora_rank;
    n += kr; /* kv_norm */

    buf = (float*)malloc((size_t)n * sizeof(float));
    if (!buf) return CCE_ERR_OOM;
    for (i = 0; i < n; ++i) buf[i] = mla_randn(&s);

    {
        float* p = buf;
        w->w_dkv = p; p += dm * (kr + rope);
        w->w_uk  = p; p += kr * (n_h * nope);
        w->w_uv  = p; p += kr * (n_h * vd);
        w->w_kr  = NULL; /* packed in w_dkv */
        if (cfg->q_lora_rank > 0) {
            w->w_dq = p; p += dm * cfg->q_lora_rank;
        }
        w->w_uq = p; p += q_in * (n_h * nope);
        w->w_qr = p; p += q_in * (n_h * rope);
        w->w_o  = p; p += (n_h * vd) * dm;
        w->kv_norm = p; p += kr;
        w->q_norm = NULL;
        w->rms_eps = 1e-6f;
        w->owns = 1;
        (void)p;
    }
    return CCE_OK;
}

void cce_mla_weights_free(cce_mla_weights* w) {
    if (!w || !w->owns) return;
    /* All sub-pointers alias one allocation starting at w_dkv (or w_dq). */
    free((void*)(w->w_dkv ? w->w_dkv : w->w_dq));
    memset(w, 0, sizeof(*w));
}

cce_result cce_mla_init(cce_mla* m, const cce_mla_config* cfg,
                        const cce_mla_weights* w, int max_ctx) {
    int n_h, nope, rope, vd, kr, dm, q_in, scratch;

    if (!m || !cfg || !w || max_ctx < 1) return CCE_ERR_INVALID_ARG;
    if (cfg->kv_lora_rank < 1 || cfg->n_heads < 1 || cfg->d_model < 1)
        return CCE_ERR_INVALID_ARG;
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    m->w = *w;
    m->w.owns = 0; /* caller retains ownership unless they free weights */

    n_h = cfg->n_heads;
    nope = cfg->qk_nope_head_dim;
    rope = cfg->qk_rope_head_dim;
    vd = cfg->v_head_dim;
    kr = cfg->kv_lora_rank;
    dm = cfg->d_model;
    q_in = cfg->q_lora_rank > 0 ? cfg->q_lora_rank : dm;

    m->cache.max_ctx = max_ctx;
    m->cache.cur_len = 0;
    m->cache.kv_lora_rank = kr;
    m->cache.qk_rope_head_dim = rope;
    m->cache.c_kv = (float*)calloc((size_t)max_ctx * kr, sizeof(float));
    m->cache.k_pe = (float*)calloc((size_t)max_ctx * rope, sizeof(float));

    m->q_nope = (float*)malloc((size_t)n_h * nope * sizeof(float));
    m->q_pe   = (float*)malloc((size_t)n_h * rope * sizeof(float));
    m->k_nope = (float*)malloc((size_t)n_h * nope * sizeof(float));
    m->v      = (float*)malloc((size_t)n_h * vd * sizeof(float));
    m->scores = (float*)malloc((size_t)max_ctx * sizeof(float));
    m->attn_out = (float*)malloc((size_t)n_h * vd * sizeof(float));
    m->out = (float*)malloc((size_t)dm * sizeof(float));
    scratch = dm;
    if (q_in > scratch) scratch = q_in;
    if (kr + rope > scratch) scratch = kr + rope;
    if (n_h * (nope + rope) > scratch) scratch = n_h * (nope + rope);
    m->tmp = (float*)malloc((size_t)scratch * sizeof(float));

    if (!m->cache.c_kv || !m->cache.k_pe || !m->q_nope || !m->q_pe ||
        !m->k_nope || !m->v || !m->scores || !m->attn_out || !m->out ||
        !m->tmp) {
        cce_mla_free(m);
        return CCE_ERR_OOM;
    }
    (void)q_in;
    cce_fp_enable_ftz_daz();
    return CCE_OK;
}

void cce_mla_free(cce_mla* m) {
    if (!m) return;
    free(m->cache.c_kv);
    free(m->cache.k_pe);
    free(m->q_nope);
    free(m->q_pe);
    free(m->k_nope);
    free(m->v);
    free(m->scores);
    free(m->attn_out);
    free(m->out);
    free(m->tmp);
    memset(m, 0, sizeof(*m));
}

void cce_mla_reset_cache(cce_mla* m) {
    if (!m) return;
    m->cache.cur_len = 0;
}

cce_result cce_mla_forward_token(cce_mla* m, const float* x, int pos,
                                 float* y) {
    const cce_mla_config* c;
    const cce_mla_weights* w;
    int n_h, nope, rope, vd, kr, dm, q_in, t, h, i, T;
    float scale, *c_kv_row, *k_pe_row;

    if (!m || !x || !y) return CCE_ERR_INVALID_ARG;
    c = &m->cfg;
    w = &m->w;
    if (!w->w_dkv || !w->w_uk || !w->w_uv || !w->w_uq || !w->w_qr || !w->w_o)
        return CCE_ERR_INVALID_ARG;
    if (m->cache.cur_len >= m->cache.max_ctx) return CCE_ERR_UNSUPPORTED;

    n_h = c->n_heads;
    nope = c->qk_nope_head_dim;
    rope = c->qk_rope_head_dim;
    vd = c->v_head_dim;
    kr = c->kv_lora_rank;
    dm = c->d_model;
    q_in = c->q_lora_rank > 0 ? c->q_lora_rank : dm;
    T = m->cache.cur_len; /* positions 0..T-1 already in cache; this is T */

    /* ---- Q path ---- */
    {
        const float* qx = x;
        if (c->q_lora_rank > 0 && w->w_dq) {
            cce_mla_matvec(w->w_dq, x, m->tmp, dm, c->q_lora_rank);
            if (w->q_norm) mla_rms_norm(m->tmp, c->q_lora_rank, w->q_norm, w->rms_eps);
            qx = m->tmp;
        }
        cce_mla_matvec(w->w_uq, qx, m->q_nope, q_in, n_h * nope);
        cce_mla_matvec(w->w_qr, qx, m->q_pe, q_in, n_h * rope);
        for (h = 0; h < n_h; ++h)
            cce_mla_apply_rope(m->q_pe + h * rope, rope, pos, c->rope_theta);
    }

    /* ---- KV compress: c_kv | k_pe = W_dkv x ---- */
    cce_mla_matvec(w->w_dkv, x, m->tmp, dm, kr + rope);
    c_kv_row = m->cache.c_kv + (size_t)T * kr;
    k_pe_row = m->cache.k_pe + (size_t)T * rope;
    memcpy(c_kv_row, m->tmp, (size_t)kr * sizeof(float));
    if (w->kv_norm) mla_rms_norm(c_kv_row, kr, w->kv_norm, w->rms_eps);
    memcpy(k_pe_row, m->tmp + kr, (size_t)rope * sizeof(float));
    cce_mla_apply_rope(k_pe_row, rope, pos, c->rope_theta);
    m->cache.cur_len = T + 1;

    scale = c->attn_scale;
    if (scale <= 0.0f)
        scale = 1.0f / sqrtf((float)(nope + rope));

    /* ---- Attention per head ---- */
    memset(m->attn_out, 0, (size_t)n_h * vd * sizeof(float));
    for (h = 0; h < n_h; ++h) {
        const float* qn = m->q_nope + h * nope;
        const float* qr = m->q_pe + h * rope;
        float* oh = m->attn_out + h * vd;
        float maxs = -1e30f, sum = 0.0f;

        /* Scores vs all cached positions (causal: 0..T).
         * nope: q_nope · (W_uk_h c_kv)  [absorb-equivalent]
         * rope: q_pe · k_pe (shared across heads, MQA-style) */
        for (t = 0; t <= T; ++t) {
            const float* ckv = m->cache.c_kv + (size_t)t * kr;
            const float* kpe = m->cache.k_pe + (size_t)t * rope;
            float s = 0.0f;
            int r, d;

            if (c->use_absorb) {
                for (r = 0; r < kr; ++r) {
                    float wr = 0.0f;
                    for (d = 0; d < nope; ++d)
                        wr += w->w_uk[(size_t)(h * nope + d) * kr + r] * qn[d];
                    s += wr * ckv[r];
                }
            } else {
                float kn[256];
                if (nope > 256) return CCE_ERR_UNSUPPORTED;
                for (d = 0; d < nope; ++d) {
                    float acc = 0.0f;
                    for (r = 0; r < kr; ++r)
                        acc += w->w_uk[(size_t)(h * nope + d) * kr + r] * ckv[r];
                    kn[d] = acc;
                }
                for (d = 0; d < nope; ++d) s += qn[d] * kn[d];
            }
            for (i = 0; i < rope; ++i) s += qr[i] * kpe[i];
            m->scores[t] = s * scale;
            if (m->scores[t] > maxs) maxs = m->scores[t];
        }

        sum = 0.0f;
        for (t = 0; t <= T; ++t) {
            float z = m->scores[t] - maxs;
            m->scores[t] = (z < -60.0f) ? 0.0f : expf(z);
            sum += m->scores[t];
        }
        if (sum <= 0.0f) sum = 1.0f;
        for (t = 0; t <= T; ++t) m->scores[t] /= sum;

        /* V: up-project each cached c_kv to this head's v, weighted sum */
        for (t = 0; t <= T; ++t) {
            const float* ckv = m->cache.c_kv + (size_t)t * kr;
            float wt = m->scores[t];
            int d, r;
            if (!(wt > 0.0f)) continue;
            for (d = 0; d < vd; ++d) {
                float acc = 0.0f;
                for (r = 0; r < kr; ++r)
                    acc += w->w_uv[(size_t)(h * vd + d) * kr + r] * ckv[r];
                oh[d] += wt * acc;
            }
        }
    }

    /* Output projection */
    cce_mla_matvec(w->w_o, m->attn_out, y, n_h * vd, dm);
    return CCE_OK;
}

cce_result cce_mla_forward_seq(cce_mla* m, const float* x, int T, float* y) {
    int t;
    if (!m || !x || !y || T < 1) return CCE_ERR_INVALID_ARG;
    cce_mla_reset_cache(m);
    for (t = 0; t < T; ++t) {
        cce_result rc = cce_mla_forward_token(
            m, x + (size_t)t * m->cfg.d_model, t,
            y + (size_t)t * m->cfg.d_model);
        if (rc != CCE_OK) return rc;
    }
    return CCE_OK;
}
