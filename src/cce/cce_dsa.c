#include "../../include/cce/cce_dsa.h"
#include "../../include/cce/cce_router.h"
#include "../../include/cce/cce_sparse_kv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void cce_dsa_config_default(cce_dsa_config* cfg) {
    if (!cfg) return;
    /* Quality-first single-user: hybrid lightning index, sleep dust only,
     * no aggressive floor (floor is the quality footgun). */
    cfg->top_k = 0;
    cfg->fraction = 0.25f;
    cfg->sleep_eps = CCE_SLEEP_DEFAULT_EPS;
    cfg->floor_quantum = 0.0f;
    cfg->keep_anchors = 1;
    cfg->enabled = 1;
    cfg->index_mode = CCE_DSA_INDEX_HYBRID;
    cfg->index_heads = 0; /* auto */
    cfg->hybrid_alpha = 0.5f;
}

void cce_dsa_config_speed(cce_dsa_config* cfg) {
    cce_dsa_config_default(cfg);
    cfg->floor_quantum = 1.0e-3f;
    cfg->sleep_eps = 1.0e-4f;
    cfg->fraction = 0.20f;
}

int cce_dsa_resolve_k(int n, const cce_dsa_config* cfg) {
    int k;
    if (n <= 0) return 0;
    if (!cfg) return n;
    if (cfg->top_k > 0) {
        k = cfg->top_k;
    } else {
        float f = cfg->fraction;
        if (f <= 0.0f) f = 0.25f;
        if (f > 1.0f) f = 1.0f;
        k = (int)ceilf(f * (float)n);
    }
    if (k < 1) k = 1;
    if (k > n) k = n;
    /* Local cap unless identity / explicit top_k (DeepSeek long-ctx ~2048). */
    if (cfg->top_k <= 0 && cfg->fraction < 0.999f && k > 512)
        k = 512;
    if (k > n) k = n;
    return k;
}

int cce_round_down_keep_one(float* w, int n, float quantum, float sleep_eps,
                            int fallback_idx) {
    int i, kept = 0;
    float sum = 0.0f;

    if (!w || n <= 0) return 0;
    if (fallback_idx < 0 || fallback_idx >= n) fallback_idx = 0;
    if (sleep_eps < 0.0f) sleep_eps = CCE_SLEEP_DEFAULT_EPS;

    for (i = 0; i < n; ++i) {
        if (!isfinite(w[i]) || w[i] <= 0.0f) {
            w[i] = 0.0f;
            continue;
        }
        if (quantum > 0.0f) {
            float q = floorf(w[i] / quantum) * quantum;
            if (q < 0.0f) q = 0.0f;
            w[i] = q;
        }
        if (w[i] > 0.0f && w[i] < sleep_eps) w[i] = 0.0f;
        if (w[i] > 0.0f) {
            sum += w[i];
            kept++;
        }
    }

    if (kept == 0 || !(sum > 0.0f) || !isfinite(sum)) {
        for (i = 0; i < n; ++i) w[i] = 0.0f;
        w[fallback_idx] = 1.0f;
        return 1;
    }
    if (fabsf(sum - 1.0f) > 1e-6f) {
        for (i = 0; i < n; ++i)
            if (w[i] > 0.0f) w[i] /= sum;
    }
    return kept;
}

static int dsa_auto_heads(int head_dim, int want) {
    int h = want;
    if (h <= 0) {
        h = head_dim >= 64 ? 8 : (head_dim >= 32 ? 4 : 2);
    }
    while (h > 1 && (head_dim % h) != 0) h--;
    if (h < 1) h = 1;
    return h;
}

static float dsa_dot(const float* a, const float* b, int d) {
    float s = 0.0f;
    int i;
    for (i = 0; i < d; ++i) s += a[i] * b[i];
    return s;
}

/* Core lightning: one K row. */
static float dsa_index_one(const float* q, const float* k, int head_dim,
                           int mode, int n_heads, float qk_scale,
                           float hybrid_alpha, const float* head_w) {
    float qk, relu_s = 0.0f;
    int h, gd;

    qk = dsa_dot(q, k, head_dim) * qk_scale;
    if (mode == CCE_DSA_INDEX_QK) return qk;

    n_heads = dsa_auto_heads(head_dim, n_heads);
    gd = head_dim / n_heads;
    for (h = 0; h < n_heads; ++h) {
        float d = dsa_dot(q + h * gd, k + h * gd, gd);
        if (d < 0.0f) d = 0.0f; /* ReLU */
        {
            float w = head_w ? head_w[h] : 1.0f;
            relu_s += w * d;
        }
    }
    if (mode == CCE_DSA_INDEX_RELU_MH) return relu_s;

    /* HYBRID: mix scaled qk with ReLU multi-head (negatives OK on result). */
    {
        float a = hybrid_alpha;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        return (1.0f - a) * qk + a * relu_s;
    }
}

cce_result cce_dsa_lightning_index(const float* q, int head_dim,
                                   const float* k_packed, int n,
                                   const cce_dsa_config* cfg,
                                   float qk_scale,
                                   const float* head_weights,
                                   float* scores_out) {
    int i, mode, nh;
    float alpha;
    cce_dsa_config local;

    if (!q || !k_packed || !scores_out || head_dim <= 0 || n <= 0)
        return CCE_ERR_INVALID_ARG;
    if (!cfg) {
        cce_dsa_config_default(&local);
        cfg = &local;
    }
    mode = cfg->index_mode;
    nh = cfg->index_heads;
    alpha = cfg->hybrid_alpha;
    if (qk_scale <= 0.0f) qk_scale = 1.0f;

    cce_fp_enable_ftz_daz();
    for (i = 0; i < n; ++i) {
        scores_out[i] = dsa_index_one(q, k_packed + (size_t)i * head_dim,
                                      head_dim, mode, nh, qk_scale, alpha,
                                      head_weights);
    }
    return CCE_OK;
}

cce_result cce_dsa_lightning_index_ptr(const float* q, int head_dim,
                                       const float* const* k_ptrs, int n,
                                       const cce_dsa_config* cfg,
                                       float qk_scale,
                                       const float* head_weights,
                                       float* scores_out) {
    int i, mode, nh;
    float alpha;
    cce_dsa_config local;

    if (!q || !k_ptrs || !scores_out || head_dim <= 0 || n <= 0)
        return CCE_ERR_INVALID_ARG;
    if (!cfg) {
        cce_dsa_config_default(&local);
        cfg = &local;
    }
    mode = cfg->index_mode;
    nh = cfg->index_heads;
    alpha = cfg->hybrid_alpha;
    if (qk_scale <= 0.0f) qk_scale = 1.0f;

    cce_fp_enable_ftz_daz();
    for (i = 0; i < n; ++i) {
        if (!k_ptrs[i]) {
            scores_out[i] = -1e30f;
            continue;
        }
        scores_out[i] = dsa_index_one(q, k_ptrs[i], head_dim, mode, nh,
                                      qk_scale, alpha, head_weights);
    }
    return CCE_OK;
}

/* ---- MLA-lite int8 KV ---- */

void cce_mla_kv_quantize(const float* src, int dim, int8_t* q8, float* scale) {
    int i;
    float amax = 0.0f, s;
    if (!src || !q8 || !scale || dim <= 0) return;
    for (i = 0; i < dim; ++i) {
        float a = fabsf(src[i]);
        if (a > amax) amax = a;
    }
    if (amax < 1e-12f) {
        *scale = 1.0f;
        memset(q8, 0, (size_t)dim);
        return;
    }
    s = amax / 127.0f;
    *scale = s;
    for (i = 0; i < dim; ++i) {
        int v = (int)lrintf(src[i] / s);
        if (v > 127) v = 127;
        if (v < -127) v = -127;
        q8[i] = (int8_t)v;
    }
}

void cce_mla_kv_dequant(const int8_t* q8, float scale, int dim, float* dst) {
    int i;
    if (!q8 || !dst || dim <= 0) return;
    for (i = 0; i < dim; ++i) dst[i] = (float)q8[i] * scale;
}

float cce_mla_kv_dot_q8(const float* q, const int8_t* k_q8, float k_scale,
                        int dim) {
    double acc = 0.0;
    int i;
    if (!q || !k_q8 || dim <= 0) return 0.0f;
    for (i = 0; i < dim; ++i) acc += (double)q[i] * (double)k_q8[i];
    return (float)(acc * (double)k_scale);
}

/* ---- select ---- */

static void dsa_pick_topk(const float* scores, int n, int k, int* idx) {
    int i, j;
    unsigned char taken_stk[512];
    unsigned char* taken = taken_stk;
    int heap = 0;

    if (k > n) k = n;
    if (n > 512) {
        taken = (unsigned char*)calloc((size_t)n, 1);
        heap = 1;
        if (!taken) {
            for (i = 0; i < k; ++i) idx[i] = i;
            return;
        }
    } else {
        memset(taken, 0, (size_t)n);
    }

    for (i = 0; i < k; ++i) {
        int best = -1;
        for (j = 0; j < n; ++j) {
            if (taken[j]) continue;
            if (best < 0 || scores[j] > scores[best]) best = j;
        }
        if (best < 0) break;
        taken[best] = 1;
        idx[i] = best;
    }
    if (heap) free(taken);
}

static cce_result dsa_select_core(const float* index_scores,
                                  const float* attn_scores, int n,
                                  const cce_dsa_config* cfg,
                                  int* out_idx, float* out_w,
                                  int out_cap, int* out_n) {
    int k, i, support;
    float maxv, sum;
    cce_dsa_config local;
    int use_anchor;
    const float* wsrc;

    if (!index_scores || !out_idx || !out_w || !out_n || n <= 0 || out_cap < 1)
        return CCE_ERR_INVALID_ARG;

    cce_fp_enable_ftz_daz();
    *out_n = 0;
    if (!cfg) {
        cce_dsa_config_default(&local);
        cfg = &local;
    }
    wsrc = attn_scores ? attn_scores : index_scores;

    k = cce_dsa_resolve_k(n, cfg);
    if (k > out_cap) k = out_cap;
    if (k < 1) k = 1;

    /* Full support: sequential order for bit-identical dense MAC. */
    if (k >= n) {
        for (i = 0; i < n && i < out_cap; ++i) out_idx[i] = i;
        *out_n = n <= out_cap ? n : out_cap;
        support = *out_n;
        maxv = wsrc[out_idx[0]];
        for (i = 1; i < support; ++i)
            if (wsrc[out_idx[i]] > maxv) maxv = wsrc[out_idx[i]];
        sum = 0.0f;
        for (i = 0; i < support; ++i) {
            float z = wsrc[out_idx[i]] - maxv;
            out_w[i] = (z < -60.0f) ? 0.0f : expf(z);
            sum += out_w[i];
        }
        if (!(sum > 0.0f) || !isfinite(sum)) {
            for (i = 0; i < support; ++i) out_w[i] = 0.0f;
            out_w[0] = 1.0f;
            return CCE_OK;
        }
        for (i = 0; i < support; ++i) out_w[i] /= sum;
        return CCE_OK;
    }

    use_anchor = cfg->keep_anchors && k < n;
    if (use_anchor) {
        cce_specialist_kv_budget bud;
        cce_specialist_kv_budget_default(&bud, n);
        bud.max_tokens = k;
        bud.recent_tokens = k / 2;
        if (bud.recent_tokens < 1) bud.recent_tokens = 1;
        if (bud.recent_tokens > 32) bud.recent_tokens = 32;
        bud.initial_tokens = k / 4;
        if (bud.initial_tokens > 4) bud.initial_tokens = 4;
        if (bud.initial_tokens + bud.recent_tokens > k)
            bud.initial_tokens = k - bud.recent_tokens;
        if (cce_specialist_select_kv_tokens(index_scores, n, &bud,
                                            out_idx, out_cap, out_n) != CCE_OK ||
            *out_n < 1) {
            use_anchor = 0;
        } else {
            k = *out_n;
        }
    }
    if (!use_anchor) {
        dsa_pick_topk(index_scores, n, k, out_idx);
        *out_n = k;
    }

    support = *out_n;
    if (support < 1) {
        out_idx[0] = 0;
        out_w[0] = 1.0f;
        *out_n = 1;
        return CCE_OK;
    }

    /* Softmax on TRUE attention scores of the selected support. */
    maxv = wsrc[out_idx[0]];
    for (i = 1; i < support; ++i) {
        float s = wsrc[out_idx[i]];
        if (s > maxv) maxv = s;
    }
    sum = 0.0f;
    for (i = 0; i < support; ++i) {
        float z = wsrc[out_idx[i]] - maxv;
        if (z < -60.0f) out_w[i] = 0.0f;
        else {
            out_w[i] = expf(z);
            sum += out_w[i];
        }
    }
    if (!(sum > 0.0f) || !isfinite(sum)) {
        for (i = 0; i < support; ++i) out_w[i] = 0.0f;
        out_w[0] = 1.0f;
        return CCE_OK;
    }
    for (i = 0; i < support; ++i) out_w[i] /= sum;

    {
        int best = 0;
        for (i = 1; i < support; ++i)
            if (out_w[i] > out_w[best]) best = i;
        cce_round_down_keep_one(out_w, support, cfg->floor_quantum,
                                cfg->sleep_eps, best);
    }
    return CCE_OK;
}

cce_result cce_dsa_select_and_weights(const float* index_scores, int n,
                                      const cce_dsa_config* cfg,
                                      int* out_idx, float* out_w,
                                      int out_cap, int* out_n) {
    return dsa_select_core(index_scores, NULL, n, cfg, out_idx, out_w,
                           out_cap, out_n);
}

cce_result cce_dsa_select_dual(const float* index_scores,
                               const float* attn_scores, int n,
                               const cce_dsa_config* cfg,
                               int* out_idx, float* out_w,
                               int out_cap, int* out_n) {
    return dsa_select_core(index_scores, attn_scores, n, cfg, out_idx, out_w,
                           out_cap, out_n);
}

cce_result cce_dsa_weights_scatter(const float* index_scores, int n,
                                   const cce_dsa_config* cfg,
                                   float* out_probs,
                                   int* out_idx, int* out_n, int out_cap) {
    int i;
    float* w = NULL;
    int* idx = NULL;
    cce_result rc;
    int local_n = 0;
    int cap;

    if (!index_scores || !out_probs || n <= 0) return CCE_ERR_INVALID_ARG;
    for (i = 0; i < n; ++i) out_probs[i] = 0.0f;

    idx = out_idx ? out_idx : (int*)malloc((size_t)n * sizeof(int));
    w = (float*)malloc((size_t)n * sizeof(float));
    if (!idx || !w) {
        if (!out_idx) free(idx);
        free(w);
        return CCE_ERR_OOM;
    }
    cap = out_cap > 0 ? out_cap : n;
    if (cap > n) cap = n;
    rc = cce_dsa_select_and_weights(index_scores, n, cfg, idx, w, cap,
                                    &local_n);
    if (rc == CCE_OK) {
        for (i = 0; i < local_n; ++i)
            if (idx[i] >= 0 && idx[i] < n) out_probs[idx[i]] = w[i];
        if (out_n) *out_n = local_n;
    }
    if (!out_idx) free(idx);
    free(w);
    return rc;
}
