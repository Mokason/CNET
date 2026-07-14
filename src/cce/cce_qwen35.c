/* Native CPU execution core for Qwen3.5 (attention + Gated-DeltaNet hybrid).
 *
 * See include/cce/cce_qwen35.h for the API contract and the honest capability
 * boundary. This file implements three architecture-specific pieces that the
 * generic cce_hybrid (mamba-1 jamba/zamba) runner deliberately does not:
 *   1. the layer-schedule dispatch (default interval-4 3:1 recurrent:full, or
 *      an explicit recurrent mask),
 *   2. the stateful Gated-DeltaNet recurrent step (exactly the recurrence in
 *      transformers' torch_recurrent_gated_delta_rule / llama.cpp qwen35.cpp
 *      build_layer_attn_linear -> build_recurrent_attn),
 *   3. the Qwen3.5 gated causal attention (per-head Q/K RMSNorm, causal GQA
 *      softmax, output gated by sigmoid(gate); qwen35.cpp build_layer_attn).
 *
 * All kernels are small F32 CPU reference kernels operating on already-projected
 * activations. Inputs are treated as read-only; only the caller-provided output
 * buffer and the owned persistent state are written.
 */

#include "../../include/cce/cce_qwen35.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int qwen35_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static int qwen35_count3(int a, int b, int c, size_t *out) {
    size_t ab;
    if (a <= 0 || b <= 0 || c <= 0) return 0;
    return qwen35_mul_size((size_t)a, (size_t)b, &ab) &&
           qwen35_mul_size(ab, (size_t)c, out);
}

/* softplus/sigmoid mirroring the double reference (single x>20 guard). */
static float qwen35_softplusf(float x) { return (x > 20.0f) ? x : log1pf(expf(x)); }
static float qwen35_sigmoidf(float x)  { return 1.0f / (1.0f + expf(-x)); }

/* ---------------------------------------------------------------------------
 * Layer schedule / dispatch
 * ------------------------------------------------------------------------- */

cce_result cce_qwen35_default_schedule(cce_qwen35_layer_kind *kinds_out,
                                       int n_layer, int full_attn_interval) {
    if (!kinds_out || n_layer <= 0 || full_attn_interval <= 0)
        return CCE_ERR_INVALID_ARG;
    for (int i = 0; i < n_layer; i++) {
        kinds_out[i] = ((i + 1) % full_attn_interval != 0)
                           ? CCE_QWEN35_LAYER_DELTANET
                           : CCE_QWEN35_LAYER_FULL_ATTN;
    }
    return CCE_OK;
}

cce_result cce_qwen35_schedule_from_mask(cce_qwen35_layer_kind *kinds_out,
                                         const int *recurrent_mask, int n_layer) {
    if (!kinds_out || !recurrent_mask || n_layer <= 0)
        return CCE_ERR_INVALID_ARG;
    for (int i = 0; i < n_layer; i++) {
        kinds_out[i] = recurrent_mask[i] ? CCE_QWEN35_LAYER_DELTANET
                                         : CCE_QWEN35_LAYER_FULL_ATTN;
    }
    return CCE_OK;
}

int cce_qwen35_layer_is_recurrent(cce_qwen35_layer_kind kind) {
    return kind == CCE_QWEN35_LAYER_DELTANET ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Gated-DeltaNet recurrent core
 * ------------------------------------------------------------------------- */

cce_result cce_qwen35_deltanet_init(cce_qwen35_deltanet_state *st,
                                    const cce_qwen35_deltanet_config *cfg) {
    if (!st || !cfg) return CCE_ERR_INVALID_ARG;
    if (cfg->num_v_heads <= 0 || cfg->num_k_heads <= 0 ||
        cfg->head_k_dim <= 0 || cfg->head_v_dim <= 0)
        return CCE_ERR_INVALID_ARG;
    if (cfg->num_v_heads % cfg->num_k_heads != 0)
        return CCE_ERR_INVALID_ARG;
    if (!cfg->dt_bias || !cfg->ssm_a)
        return CCE_ERR_INVALID_ARG;

    memset(st, 0, sizeof *st);
    st->num_v_heads = cfg->num_v_heads;
    st->num_k_heads = cfg->num_k_heads;
    st->head_k_dim = cfg->head_k_dim;
    st->head_v_dim = cfg->head_v_dim;
    st->l2_eps = (cfg->l2_eps > 0.0f) ? cfg->l2_eps : 1e-6f;

    size_t hv = (size_t)cfg->num_v_heads;
    size_t nstate = 0;
    if (!qwen35_count3(cfg->num_v_heads, cfg->head_k_dim,
                       cfg->head_v_dim, &nstate) ||
        hv > SIZE_MAX / sizeof(float) ||
        nstate > SIZE_MAX / sizeof(float))
        return CCE_ERR_INVALID_ARG;
    st->dt_bias = (float *)malloc(hv * sizeof(float));
    st->ssm_a = (float *)malloc(hv * sizeof(float));
    st->state = (float *)calloc(nstate, sizeof(float));
    if (!st->dt_bias || !st->ssm_a || !st->state) {
        free(st->dt_bias); free(st->ssm_a); free(st->state);
        memset(st, 0, sizeof *st);
        return CCE_ERR_OOM;
    }
    memcpy(st->dt_bias, cfg->dt_bias, hv * sizeof(float));
    memcpy(st->ssm_a, cfg->ssm_a, hv * sizeof(float));
    st->steps = 0;
    return CCE_OK;
}

void cce_qwen35_deltanet_reset(cce_qwen35_deltanet_state *st) {
    if (!st || !st->state) return;
    size_t nstate = 0;
    if (!qwen35_count3(st->num_v_heads, st->head_k_dim,
                       st->head_v_dim, &nstate) ||
        nstate > SIZE_MAX / sizeof(float)) return;
    memset(st->state, 0, nstate * sizeof(float));
    st->steps = 0;
}

void cce_qwen35_deltanet_free(cce_qwen35_deltanet_state *st) {
    if (!st) return;
    free(st->dt_bias);
    free(st->ssm_a);
    free(st->state);
    memset(st, 0, sizeof *st);
}

cce_result cce_qwen35_deltanet_step(cce_qwen35_deltanet_state *st,
                                    const float *q, const float *k,
                                    const float *v, const float *alpha,
                                    const float *beta, float *out) {
    if (!st || !st->state || !st->dt_bias || !st->ssm_a ||
        !q || !k || !v || !alpha || !beta || !out)
        return CCE_ERR_INVALID_ARG;

    const int Hv = st->num_v_heads;
    const int Hk = st->num_k_heads;
    const int Dk = st->head_k_dim;
    const int Dv = st->head_v_dim;
    size_t state_per_head = 0, total_state = 0;
    size_t scratch_count = 0;
    if (Hv <= 0 || Hk <= 0 || Dk <= 0 || Dv <= 0 || Hv % Hk != 0 ||
        !qwen35_count3(1, Dk, Dv, &state_per_head) ||
        !qwen35_count3(Hv, Dk, Dv, &total_state) ||
        !qwen35_mul_size(2u, (size_t)Dk, &scratch_count) ||
        scratch_count > SIZE_MAX - 2u * (size_t)Dv ||
        (scratch_count += 2u * (size_t)Dv) > SIZE_MAX / sizeof(float))
        return CCE_ERR_INVALID_ARG;
    (void)total_state;
    const int rep = Hv / Hk;
    const float scale = 1.0f / sqrtf((float)Dk);
    const double eps = (double)st->l2_eps;

    /* per-head scratch: qn[Dk], kn[Dk], kvm[Dv], delta[Dv] */
    float *scr = (float *)malloc(scratch_count * sizeof(float));
    if (!scr) return CCE_ERR_OOM;
    float *qn = scr, *kn = qn + Dk, *kvm = kn + Dk, *delta = kvm + Dv;

    for (int h = 0; h < Hv; h++) {
        const int kh = h / rep;               /* value head reads this key head */
        const float *qh = q + (size_t)kh * Dk;
        const float *khp = k + (size_t)kh * Dk;

        /* Q/K L2 norm (over head dim), then Q scaled by 1/sqrt(Dk) */
        double sq = 0.0, sk = 0.0;
        for (int a = 0; a < Dk; a++) {
            sq += (double)qh[a] * qh[a];
            sk += (double)khp[a] * khp[a];
        }
        const float invq = (float)(1.0 / sqrt(sq + eps));
        const float invk = (float)(1.0 / sqrt(sk + eps));
        for (int a = 0; a < Dk; a++) {
            qn[a] = qh[a] * invq * scale;
            kn[a] = khp[a] * invk;
        }

        /* gates: g = ssm_a * softplus(alpha + dt_bias) (log-decay, <= 0);
                  write gate = sigmoid(beta) */
        const float g = st->ssm_a[h] * qwen35_softplusf(alpha[h] + st->dt_bias[h]);
        const float decay = expf(g);
        const float bta = qwen35_sigmoidf(beta[h]);

        float *Sh = st->state + (size_t)h * state_per_head; /* [Dk][Dv] row-major */

        /* decay the whole state */
        for (size_t i = 0; i < state_per_head; i++) Sh[i] *= decay;

        /* kv_mem[b] = sum_a S[a][b] * kn[a] */
        for (int b = 0; b < Dv; b++) kvm[b] = 0.0f;
        for (int a = 0; a < Dk; a++) {
            const float ka = kn[a];
            const float *row = Sh + (size_t)a * Dv;
            for (int b = 0; b < Dv; b++) kvm[b] += row[b] * ka;
        }

        /* delta = (v - kv_mem) * beta ; S[a][b] += kn[a] * delta[b] */
        const float *vh = v + (size_t)h * Dv;
        for (int b = 0; b < Dv; b++) delta[b] = (vh[b] - kvm[b]) * bta;
        for (int a = 0; a < Dk; a++) {
            const float ka = kn[a];
            float *row = Sh + (size_t)a * Dv;
            for (int b = 0; b < Dv; b++) row[b] += ka * delta[b];
        }

        /* readout: out[b] = sum_a S[a][b] * qn[a] */
        float *outh = out + (size_t)h * Dv;
        for (int b = 0; b < Dv; b++) outh[b] = 0.0f;
        for (int a = 0; a < Dk; a++) {
            const float qa = qn[a];
            const float *row = Sh + (size_t)a * Dv;
            for (int b = 0; b < Dv; b++) outh[b] += row[b] * qa;
        }
    }

    free(scr);
    st->steps++;
    return CCE_OK;
}

/* ---------------------------------------------------------------------------
 * Gated causal attention core
 * ------------------------------------------------------------------------- */

cce_result cce_qwen35_gated_attention(const cce_qwen35_attn_config *cfg,
                                      int n_tokens, const float *q,
                                      const float *gate, const float *k,
                                      const float *v, const float *q_norm_w,
                                      const float *k_norm_w, float *out) {
    if (!cfg || !q || !gate || !k || !v || !q_norm_w || !k_norm_w || !out)
        return CCE_ERR_INVALID_ARG;
    if (n_tokens <= 0)
        return CCE_ERR_INVALID_ARG;

    const int nh = cfg->n_head;
    const int nkv = cfg->n_kv_head;
    const int hd = cfg->head_dim;
    if (nh <= 0 || nkv <= 0 || hd <= 0 || nh % nkv != 0)
        return CCE_ERR_INVALID_ARG;

    size_t q_count = 0, kv_count = 0, scratch_count = 0;
    if (!qwen35_count3(n_tokens, nh, hd, &q_count) ||
        !qwen35_count3(n_tokens, nkv, hd, &kv_count) ||
        !qwen35_mul_size(2u, (size_t)hd, &scratch_count) ||
        scratch_count > SIZE_MAX - (size_t)n_tokens ||
        (scratch_count += (size_t)n_tokens) > SIZE_MAX / sizeof(float))
        return CCE_ERR_INVALID_ARG;
    (void)q_count;
    (void)kv_count;
    const int group = nh / nkv;
    const float scale = 1.0f / sqrtf((float)hd);
    const double eps = (cfg->rms_eps > 0.0f) ? (double)cfg->rms_eps : 1e-6;

    /* scratch: qn[hd], kn[hd], scores[n_tokens] */
    float *scr = (float *)malloc(scratch_count * sizeof(float));
    if (!scr) return CCE_ERR_OOM;
    float *qn = scr, *kn = qn + hd, *sc = kn + hd;

    for (int t = 0; t < n_tokens; t++) {
        for (int h = 0; h < nh; h++) {
            const int kvh = h / group;
            const float *qh = q + ((size_t)t * nh + h) * hd;

            /* RMSNorm(q) over head dim */
            double ss = 0.0;
            for (int d = 0; d < hd; d++) ss += (double)qh[d] * qh[d];
            const float qinv = (float)(1.0 / sqrt(ss / hd + eps));
            for (int d = 0; d < hd; d++) qn[d] = qh[d] * qinv * q_norm_w[d];

            /* causal scores over keys 0..t, with RMSNorm(k) per key */
            float mx = -INFINITY;
            for (int j = 0; j <= t; j++) {
                const float *kj = k + ((size_t)j * nkv + kvh) * hd;
                double ks = 0.0;
                for (int d = 0; d < hd; d++) ks += (double)kj[d] * kj[d];
                const float kinv = (float)(1.0 / sqrt(ks / hd + eps));
                float dot = 0.0f;
                for (int d = 0; d < hd; d++) dot += qn[d] * (kj[d] * kinv * k_norm_w[d]);
                sc[j] = dot * scale;
                if (sc[j] > mx) mx = sc[j];
            }
            float den = 0.0f;
            for (int j = 0; j <= t; j++) { sc[j] = expf(sc[j] - mx); den += sc[j]; }
            const float inv_den = 1.0f / den;

            /* context, then Qwen3.5 output gate: out = context * sigmoid(gate) */
            const float *gh = gate + ((size_t)t * nh + h) * hd;
            float *outh = out + ((size_t)t * nh + h) * hd;
            for (int d = 0; d < hd; d++) {
                float ctx = 0.0f;
                for (int j = 0; j <= t; j++)
                    ctx += sc[j] * inv_den * v[((size_t)j * nkv + kvh) * hd + d];
                outh[d] = ctx * qwen35_sigmoidf(gh[d]);
            }
        }
    }

    free(scr);
    return CCE_OK;
}

/* ---------------------------------------------------------------------------
 * Honest capability statement
 * ------------------------------------------------------------------------- */

void cce_qwen35_get_caps(cce_qwen35_caps *out) {
    if (!out) return;
    out->layer_schedule_dispatch = 1;
    out->deltanet_recurrent_core = 1;
    out->gated_attention_core = 1;
    /* the qwen35 SUBSYSTEM now ships a GGUF loader + end-to-end runner
       (cce_gguf_qwen35.c: cce_gguf_load_qwen35 -> cce_gguf_qwen2_forward
       dispatch), which supplies the (M)RoPE+YaRN, causal short-conv+SiLU,
       and projections these cores deliberately leave to the caller. The
       two applies_* fields still describe THIS core module's contract. */
    out->gguf_loader = 1;
    out->end_to_end_runner = 1;
    out->applies_rope = 0;
    out->applies_short_conv = 0;
    out->summary =
        "Qwen3.5 native execution core: layer-schedule dispatch + stateful "
        "Gated-DeltaNet recurrent step + gated causal attention (F32 CPU "
        "reference). The end-to-end GGUF runner lives in cce_gguf_qwen35.c "
        "(cce_gguf_load_qwen35), which supplies (M)RoPE+YaRN, the causal "
        "short-conv+SiLU, and the projections around these cores.";
}
