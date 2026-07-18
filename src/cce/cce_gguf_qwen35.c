/* Qwen3.5 hybrid runner: gated causal attention + Gated-DeltaNet recurrent
 * linear attention interleaved on one residual stream (arch "qwen35",
 * e.g. Qwythos-9B). Loads into the SAME cce_gguf_qwen2 struct every oracle
 * consumer binds to (depth_probe / window_discover / flagship_run), so the
 * whole campaign harness works unmodified; cce_gguf_qwen2_forward dispatches
 * here whenever m->qwen35 is set.
 *
 * Numerical reference (every formula below was pinned against it):
 *   llama.cpp src/models/qwen35.cpp        (graph: layer order, projections,
 *                                           Q|gate interleave, gating)
 *   llama.cpp src/models/delta-net-base.cpp (decode recurrence — matches the
 *                                           validated cce_qwen35 core)
 *   ggml ops.cpp rope_yarn/ggml_mrope_cache_init (YaRN NEOX rope; text-only
 *                                           IMROPE reduces to NEOX because all
 *                                           three used position streams carry
 *                                           the same value: llama-graph.cpp
 *                                           llm_graph_input_pos::set_input)
 *   ggml ops.cpp ggml_compute_forward_ssm_conv_f32 (tap 0 = OLDEST column)
 *
 * Architecture facts this runner enforces (refusing on any mismatch):
 *   - trunk = block_count - nextn_predict_layers; the nextn/MTP block is a
 *     draft head llama.cpp never executes in the main pass — skipped whole.
 *   - full-attention layers: fused attn_q = per-head interleaved
 *     [q_h | gate_h] pairs; per-head Q/K RMSNorm BEFORE rope; YaRN NEOX rope
 *     over rope.dimension_count dims; kq_scale 1/sqrt(head_dim); context is
 *     multiplied by sigmoid(gate) BEFORE o_proj.
 *   - DeltaNet layers: qkv/z/alpha/beta ALL project from the post-attn_norm
 *     input (z is NOT convolved); causal depthwise conv (kernel conv_k) over
 *     the fused qkv channels pre-split, then SiLU; K->V head broadcast is
 *     TILED (value head h reads key head h % hk — ggml_repeat_4d semantics,
 *     NOT the grouped h/rep the core would apply), so q/k are pre-expanded
 *     to hv heads and the core runs with hk == hv (grouping inert); gated
 *     norm = per-head RMSNorm(out, ssm_norm) * SiLU(z); then ssm_out.
 *   - every layer: post_attention_norm is the PRE-FFN norm (there is no
 *     ffn_norm tensor) with the FFN residual taken pre-norm — exactly the
 *     m->ffn_norm[l] semantics of the qwen2 forward, so the loader maps it
 *     there and leaves m->post_attention_norm empty.
 *
 * Oracle state protocol: the flagship rewinds m->cur_pos for KV-prefix
 * reuse. A KV cache rewinds positionally; recurrent state does not. The ext
 * keeps stream_pos (where the live state is) and ONE checkpoint slot:
 *   cur_pos == 0          -> full state reset
 *   cur_pos == stream_pos -> continue
 *   cur_pos == ckpt_pos   -> restore the snapshot
 *   anything else         -> loud refusal (never silent garbage)
 * Committing forwards snapshot at entry (so a suffix run never clobbers the
 * prefix checkpoint); probe batches run every row on scratch copies of the
 * per-layer state + conv ring and leave the live state untouched — bit-
 * identical to serial rewind-per-probe, which the flagship startup gate
 * verifies on the real model. */

#include "cce_gguf_qwen35.h"
#include "../../include/cce/cce_clgemm.h"
#include "../../include/cce/cce_cl_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int q35_load_trace(void) {
    const char *e = getenv("CNET_LOAD_TRACE");
    return e && e[0] == '1';
}
#define Q35TRACE(...) do { if (q35_load_trace()) { \
    fprintf(stderr, "[qwen35] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

/* ---------------------------------------------------------------- loader */

static int q35_tensor_2d(const cce_gguf *g, const char *name, int *in_d, int *out_d) {
    cce_gguf_tensor_meta tm;
    int idx = cce_gguf_find_tensor(g, name);
    if (idx < 0 || cce_gguf_get_tensor_meta(g, idx, &tm) != CCE_OK || tm.ndim != 2)
        return 0;
    if (in_d)  *in_d  = tm.shape[0];
    if (out_d) *out_d = tm.shape[1];
    return 1;
}

static int q35_has_tensor(const cce_gguf *g, const char *fmt, int l) {
    char nm[160];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(nm, sizeof nm, fmt, l);
#pragma GCC diagnostic pop
    return cce_gguf_find_tensor(g, nm) >= 0;
}

/* ggml_rope_yarn_corr_dim: n_dims * log(n_ctx_orig / (n_rot * 2pi)) / (2 * log(base)) */
static float q35_yarn_corr_dim(int n_dims, int n_ctx_orig, float n_rot, float base) {
    return (float)n_dims *
           logf((float)n_ctx_orig / (n_rot * 6.283185307179586f)) /
           (2.0f * logf(base));
}

void cce_gguf_qwen35_ext_free(cce_gguf_qwen2 *m) {
    cce_gguf_qwen35_ext *e;
    int l;
    if (!m || !m->qwen35) return;
    e = m->qwen35;
    for (l = 0; l < e->n_layer; l++) {
        if (e->ssm_a)       cce_tensor_free(&e->ssm_a[l]);
        if (e->ssm_dt_bias) cce_tensor_free(&e->ssm_dt_bias[l]);
        if (e->ssm_conv1d)  cce_tensor_free(&e->ssm_conv1d[l]);
        if (e->ssm_norm)    cce_tensor_free(&e->ssm_norm[l]);
        if (e->dstate)      cce_qwen35_deltanet_free(&e->dstate[l]);
        if (e->conv_ring)   free(e->conv_ring[l]);
        if (e->ckpt_state)  free(e->ckpt_state[l]);
        if (e->ckpt_ring)   free(e->ckpt_ring[l]);
    }
    free(e->ssm_a); free(e->ssm_dt_bias); free(e->ssm_conv1d); free(e->ssm_norm);
    free(e->dstate); free(e->conv_ring); free(e->ckpt_state); free(e->ckpt_ring);
    free(e->kind);
    free(e);
    m->qwen35 = NULL;
}

cce_result cce_gguf_load_qwen35(cce_gguf_qwen2 **out, const char *path) {
    cce_gguf *g = NULL;
    cce_gguf_qwen2 *m = NULL;
    cce_gguf_qwen35_ext *e = NULL;
    cce_result rc;
    char nm[192], nm2[192], br[128];
    int l, block_count, trunk, nextn, interval;
    int D, n_head, n_kv_head, head_dim, v_head_dim, rope_dim;
    int hk, hv, dk, dv, conv_k, key_dim, qkv_dim;

    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    rc = cce_gguf_load(path, &g);
    if (rc != CCE_OK) return rc;

    {
        const char *arch = cce_gguf_get_arch(g);
        if (strcmp(arch, "qwen35") != 0 && strcmp(arch, "qwen3.5") != 0) {
            fprintf(stderr, "qwen35: arch '%s' is not qwen35 — refusing\n", arch);
            cce_gguf_free(g);
            return CCE_ERR_UNSUPPORTED;
        }
    }

    {   /* CNET_SPARSE_KV: the sparse-KV read exists only on the classic
           transformer attention path (cce_gguf.c). An armed knob must fail
           LOUD here rather than silently run full attention — the oracle
           must never misrepresent what it executed. Unset/empty/"0" = OFF,
           proceed unchanged. */
        const char *skv = getenv("CNET_SPARSE_KV");
        if (skv && skv[0]) {
            char *end = NULL;
            double f = strtod(skv, &end);
            if (end == skv || (end && *end) || f != 0.0) {
                fprintf(stderr, "qwen35: CNET_SPARSE_KV='%s' set but the "
                                "qwen35 hybrid runner has no sparse-KV path "
                                "— refusing load\n", skv);
                cce_gguf_free(g);
                return CCE_ERR_UNSUPPORTED;
            }
        }
    }

    block_count = cce_gguf_get_n_layer(g);
    nextn = (int)cce_gguf__get_scalar(g, "nextn_predict_layers", 0);
    if (block_count <= 0 || nextn < 0 || nextn > 1 || nextn >= block_count) {
        fprintf(stderr, "qwen35: block_count=%d nextn=%d unsupported — refusing\n",
                block_count, nextn);
        cce_gguf_free(g);
        return CCE_ERR_UNSUPPORTED;
    }
    trunk = block_count - nextn;

    D          = cce_gguf_get_hidden_size(g);
    n_head     = cce_gguf_get_n_heads(g);
    n_kv_head  = cce_gguf_get_n_kv_heads(g);
    if (n_kv_head <= 0) n_kv_head = n_head;
    head_dim   = (int)cce_gguf__get_scalar(g, "attention.key_length", 0);
    v_head_dim = (int)cce_gguf__get_scalar(g, "attention.value_length", head_dim);
    rope_dim   = (int)cce_gguf__get_scalar(g, "rope.dimension_count", 0);
    if (D <= 0 || n_head <= 0 || head_dim <= 0 || v_head_dim <= 0) {
        fprintf(stderr, "qwen35: bad attention hparams (D=%d H=%d hd=%d vhd=%d) "
                        "— refusing\n", D, n_head, head_dim, v_head_dim);
        cce_gguf_free(g);
        return CCE_ERR_UNSUPPORTED;
    }
    if (rope_dim <= 0 || rope_dim > head_dim) rope_dim = head_dim;

    /* DeltaNet geometry from metadata (cross-checked against every tensor
       shape below; either side lying refuses the load) */
    conv_k = (int)cce_gguf__get_scalar(g, "ssm.conv_kernel", 0);
    dk     = (int)cce_gguf__get_scalar(g, "ssm.state_size", 0);
    hk     = (int)cce_gguf__get_scalar(g, "ssm.group_count", 0);
    hv     = (int)cce_gguf__get_scalar(g, "ssm.time_step_rank", 0);
    {
        int inner = (int)cce_gguf__get_scalar(g, "ssm.inner_size", 0);
        dv = (hv > 0) ? inner / hv : 0;
        if (conv_k < 2 || dk <= 0 || hk <= 0 || hv <= 0 || dv <= 0 ||
            hv % hk != 0 || inner != hv * dv) {
            fprintf(stderr, "qwen35: bad ssm hparams (conv=%d dk=%d hk=%d hv=%d "
                            "inner=%d) — refusing\n", conv_k, dk, hk, hv, inner);
            cce_gguf_free(g);
            return CCE_ERR_UNSUPPORTED;
        }
    }
    key_dim = hk * dk;
    qkv_dim = 2 * key_dim + hv * dv;

    m = (cce_gguf_qwen2 *)calloc(1, sizeof *m);
    e = (cce_gguf_qwen35_ext *)calloc(1, sizeof *e);
    if (!m || !e) { free(m); free(e); cce_gguf_free(g); return CCE_ERR_OOM; }
    m->qwen35 = e;

    m->n_layer   = trunk;
    m->n_embd    = D;
    m->n_head    = n_head;
    m->n_kv_head = n_kv_head;
    m->head_dim  = head_dim;
    m->vocab_size = cce_gguf_get_vocab_size(g);
    m->bos_token_id = cce_gguf_get_bos_token_id(g);   /* -1 when absent: keep */
    m->eos_token_id = cce_gguf_get_eos_token_id(g);
    strncpy(m->tokenizer_model, cce_gguf_get_tokenizer_model(g),
            sizeof(m->tokenizer_model) - 1);
    m->ctx_len = cce_gguf_get_context_length(g);
    m->feed_forward_length = cce_gguf_get_feed_forward_length(g);
    m->rope_freq_base = cce_gguf_get_rope_freq_base(g);
    m->rms_eps = cce_gguf_get_rms_eps(g);
    m->embed_scale = 1.0f;
    m->ffn_gelu = 0;
    m->gemma4_attn = 0;
    m->n_suppress = cce_gguf_get_int_array(g, "tokenizer.ggml.suppress_tokens",
                                           m->suppress_ids, 256);

    e->n_layer = trunk;
    e->hk = hk; e->hv = hv; e->dk = dk; e->dv = dv;
    e->conv_k = conv_k; e->key_dim = key_dim; e->qkv_dim = qkv_dim;
    e->rope_dim = rope_dim;
    e->rope_base = (m->rope_freq_base > 0.0f) ? m->rope_freq_base : 10000.0f;
    e->stream_pos = 0;
    e->ckpt_pos = -1;

    /* YaRN (always-on for this family: scaling.type=yarn, factor=4).
       Kernel attn_factor replicates the reference context setup: with
       rope_yarn_log_mul absent, get_mscale(factor) is exactly cancelled by
       the 1/(1+0.1*ln(factor)) correction (llama-context.cpp), so the
       kernel receives 1.0 and re-derives the +0.1*ln(1/freq_scale) inside
       rope_yarn. corr dims use the ggml formula with beta_fast/slow 32/1. */
    e->yarn_freq_scale = 1.0f;
    e->yarn_ext_factor = 0.0f;
    e->yarn_attn_factor = 1.0f;
    {
        char sty[32] = {0};
        int has_type = cce_gguf__get_string(g, "rope.scaling.type", sty, sizeof sty);
        double factor = cce_gguf__get_scalar(g, "rope.scaling.factor", 0.0);
        double orig = cce_gguf__get_scalar(g, "rope.scaling.original_context_length", 0.0);
        if (has_type && strcmp(sty, "yarn") != 0 && strcmp(sty, "none") != 0) {
            fprintf(stderr, "qwen35: rope scaling '%s' unsupported (yarn only) "
                            "— refusing\n", sty);
            goto fail_unsupported;
        }
        if (has_type && strcmp(sty, "yarn") == 0) {
            if (factor <= 0.0 || orig <= 0.0) {
                fprintf(stderr, "qwen35: yarn declared but factor/original_ctx "
                                "missing — refusing\n");
                goto fail_unsupported;
            }
            e->yarn_freq_scale = (float)(1.0 / factor);
            e->yarn_ext_factor = 1.0f;
            e->yarn_attn_factor =
                (float)((0.1 * log(factor) + 1.0) / (1.0 + 0.1 * log(factor)));
            e->yarn_attn_factor *=
                (float)cce_gguf__get_scalar(g, "rope.scaling.attn_factor", 1.0);
            {
                float start = floorf(q35_yarn_corr_dim(rope_dim, (int)orig, 32.0f,
                                                       e->rope_base));
                float end = ceilf(q35_yarn_corr_dim(rope_dim, (int)orig, 1.0f,
                                                    e->rope_base));
                e->yarn_corr0 = start > 0.0f ? start : 0.0f;
                e->yarn_corr1 = end < (float)(rope_dim - 1) ? end
                                                            : (float)(rope_dim - 1);
            }
        }
    }

    /* IMROPE sections: for TEXT input all three used position streams carry
       the same value, so interleaved MRoPE reduces to plain NEOX — but ONLY
       if no sector falls through to the (zeroed) e-stream, which would leave
       those dims unrotated. Verify the reduction is exact or refuse. */
    {
        int64_t sect[8] = {0};
        size_t ns = cce_gguf_get_int_array(g, "rope.dimension_sections", sect, 8);
        if (ns >= 3) {
            int sum = (int)(sect[0] + sect[1] + sect[2] +
                            (ns > 3 ? sect[3] : 0));
            int s_;
            if (sum * 2 != rope_dim || (ns > 3 && sect[3] != 0)) {
                fprintf(stderr, "qwen35: rope sections [%d,%d,%d,%d] do not "
                                "cover rope_dim=%d as t/h/w — refusing\n",
                        (int)sect[0], (int)sect[1], (int)sect[2],
                        (int)(ns > 3 ? sect[3] : 0), rope_dim);
                goto fail_unsupported;
            }
            for (s_ = 0; s_ < sum; s_++) {
                int ok = (s_ % 3 == 0 && s_ < 3 * (int)sect[0]) ||
                         (s_ % 3 == 1 && s_ < 3 * (int)sect[1]) ||
                         (s_ % 3 == 2 && s_ < 3 * (int)sect[2]);
                if (!ok) {
                    fprintf(stderr, "qwen35: IMROPE sector %d maps outside "
                                    "t/h/w — text-only NEOX reduction invalid; "
                                    "refusing\n", s_);
                    goto fail_unsupported;
                }
            }
        }
    }

    /* layer kinds: tensor structure is the truth; the interval schedule is
       the cross-check (an explicit recurrent-layers array would override the
       interval — none of the target checkpoints ship one, so refuse if seen) */
    {
        int64_t recr[128];
        if (cce_gguf_get_int_array(g, "attention.recurrent_layers", recr, 128) > 0) {
            fprintf(stderr, "qwen35: explicit recurrent_layers array present "
                            "but not supported yet — refusing\n");
            goto fail_unsupported;
        }
    }
    interval = (int)cce_gguf__get_scalar(g, "full_attention_interval", 4);
    if (interval < 1) interval = 4;
    e->kind = (cce_qwen35_layer_kind *)calloc((size_t)trunk, sizeof *e->kind);
    if (!e->kind) goto fail_oom;
    for (l = 0; l < trunk; l++) {
        int has_qkv = q35_has_tensor(g, "blk.%d.attn_qkv.weight", l);
        int has_q   = q35_has_tensor(g, "blk.%d.attn_q.weight", l);
        if (has_qkv == has_q) {
            fprintf(stderr, "qwen35: layer %d is %s — refusing\n", l,
                    has_qkv ? "both fused-qkv and attn_q" : "neither kind");
            goto fail_unsupported;
        }
        e->kind[l] = has_qkv ? CCE_QWEN35_LAYER_DELTANET
                             : CCE_QWEN35_LAYER_FULL_ATTN;
        {
            cce_qwen35_layer_kind want = ((l + 1) % interval != 0)
                ? CCE_QWEN35_LAYER_DELTANET : CCE_QWEN35_LAYER_FULL_ATTN;
            if (e->kind[l] != want) {
                fprintf(stderr, "qwen35: layer %d structure (%s) disagrees with "
                                "full_attention_interval=%d — refusing\n", l,
                        has_qkv ? "deltanet" : "attn", interval);
                goto fail_unsupported;
            }
        }
    }

    Q35TRACE("hparams: trunk=%d (blocks=%d nextn=%d) D=%d H=%d KV=%d hd=%d "
             "rope_dim=%d yarn(fs=%.4g ext=%.1f corr=[%.2f,%.2f])",
             trunk, block_count, nextn, D, n_head, n_kv_head, head_dim,
             rope_dim, e->yarn_freq_scale, e->yarn_ext_factor,
             e->yarn_corr0, e->yarn_corr1);
    Q35TRACE("deltanet: hk=%d hv=%d dk=%d dv=%d conv_k=%d qkv_dim=%d",
             hk, hv, dk, dv, conv_k, qkv_dim);

    /* ---- forest: every large linear as an int8-eligible specialist ---- */
    {
        static int g_q35_forest_seq = 0;
        cce_forest *forest = NULL;
        int add_fails = 0;
        snprintf(m->forest_scratch, sizeof m->forest_scratch,
                 "gguf_qwen35_forest.%d.cce", ++g_q35_forest_seq);
        remove(m->forest_scratch);
        if (cce_forest_open(&forest, m->forest_scratch, 512) != CCE_OK)
            goto fail_io;
        m->forest = forest;

        /* every tensor below EXISTS for its layer kind (kind detection above);
           an add failure is therefore always a present-tensor load failure
           (unsupported quant / IO / OOM) -> fatal, never a silent gap */
        #define Q35_ADD(wfmt, brfmt) do { \
            snprintf(nm, sizeof nm, wfmt, l); \
            snprintf(nm2, sizeof nm2, "%s", nm); \
            { size_t nl = strlen(nm2); \
              if (nl > 7) snprintf(nm2 + nl - 7, 8, "%s", ".bias"); } \
            snprintf(br, sizeof br, brfmt, l); \
            if (cce_gguf_add_linear_branch(forest, g, nm, \
                    cce_gguf_find_tensor(g, nm2) >= 0 ? nm2 : NULL, br, 0.0f) < 0) { \
                fprintf(stderr, "qwen35: tensor %s failed to load -> fatal\n", nm); \
                add_fails++; \
            } } while (0)

        for (l = 0; l < trunk; l++) {
            Q35TRACE("forest layer %d/%d (%s)", l, trunk,
                     e->kind[l] == CCE_QWEN35_LAYER_DELTANET ? "deltanet" : "attn");
            if (e->kind[l] == CCE_QWEN35_LAYER_FULL_ATTN) {
                Q35_ADD("blk.%d.attn_q.weight",      "qwen35.blk.%d.qg_proj");
                Q35_ADD("blk.%d.attn_k.weight",      "qwen35.blk.%d.k_proj");
                Q35_ADD("blk.%d.attn_v.weight",      "qwen35.blk.%d.v_proj");
                Q35_ADD("blk.%d.attn_output.weight", "qwen35.blk.%d.o_proj");
            } else {
                Q35_ADD("blk.%d.attn_qkv.weight",  "qwen35.blk.%d.qkv");
                Q35_ADD("blk.%d.attn_gate.weight", "qwen35.blk.%d.zgate");
                Q35_ADD("blk.%d.ssm_alpha.weight", "qwen35.blk.%d.alpha");
                Q35_ADD("blk.%d.ssm_beta.weight",  "qwen35.blk.%d.beta");
                Q35_ADD("blk.%d.ssm_out.weight",   "qwen35.blk.%d.ssm_out");
            }
            Q35_ADD("blk.%d.ffn_gate.weight", "qwen35.blk.%d.gate_proj");
            Q35_ADD("blk.%d.ffn_up.weight",   "qwen35.blk.%d.up_proj");
            Q35_ADD("blk.%d.ffn_down.weight", "qwen35.blk.%d.down_proj");
        }
        #undef Q35_ADD

        /* the head keeps the consumer-bound qwen2 name (depth_probe reads it)
           AND the CNET_ORACLE_INT8 FP-skip that hangs off it */
        {
            const char *head = (cce_gguf_find_tensor(g, "output.weight") >= 0)
                                   ? "output.weight" : "token_embd.weight";
            if (cce_gguf_add_linear_branch(forest, g, head, NULL,
                                           "qwen2.lm_head", 0.0f) < 0) {
                fprintf(stderr, "qwen35: head %s failed to load -> fatal\n", head);
                add_fails++;
            }
        }
        if (add_fails > 0) goto fail_unsupported;
        Q35TRACE("forest built: %d branches", forest->num_branches);
    }

    /* ---- small F32 residents ---- */
    m->attn_norm  = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    m->ffn_norm   = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    m->attn_q_norm = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    m->attn_k_norm = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    m->post_attention_norm = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    m->post_ffw_norm = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    m->layer_output_scale = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    e->ssm_a       = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    e->ssm_dt_bias = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    e->ssm_conv1d  = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    e->ssm_norm    = (cce_tensor *)calloc((size_t)trunk, sizeof(cce_tensor));
    e->dstate = (cce_qwen35_deltanet_state *)calloc((size_t)trunk, sizeof *e->dstate);
    e->conv_ring  = (float **)calloc((size_t)trunk, sizeof(float *));
    e->ckpt_state = (float **)calloc((size_t)trunk, sizeof(float *));
    e->ckpt_ring  = (float **)calloc((size_t)trunk, sizeof(float *));
    if (!m->attn_norm || !m->ffn_norm || !m->attn_q_norm || !m->attn_k_norm ||
        !m->post_attention_norm || !m->post_ffw_norm || !m->layer_output_scale ||
        !e->ssm_a || !e->ssm_dt_bias || !e->ssm_conv1d || !e->ssm_norm ||
        !e->dstate || !e->conv_ring || !e->ckpt_state || !e->ckpt_ring)
        goto fail_oom;

    for (l = 0; l < trunk; l++) {
        snprintf(nm, sizeof nm, "blk.%d.attn_norm.weight", l);
        if (cce_gguf_load_tensor_by_name(g, nm, &m->attn_norm[l]) != CCE_OK ||
            m->attn_norm[l].numel != (size_t)D) {
            fprintf(stderr, "qwen35: layer %d attn_norm missing/mis-sized — "
                            "refusing\n", l);
            goto fail_unsupported;
        }
        /* post_attention_norm IS the pre-FFN norm here (no ffn_norm tensor);
           map it to m->ffn_norm so the shared FFN block applies it with the
           residual taken pre-norm, and leave m->post_attention_norm empty so
           the gemma sandwich path never fires. */
        snprintf(nm, sizeof nm, "blk.%d.post_attention_norm.weight", l);
        if (cce_gguf_load_tensor_by_name(g, nm, &m->ffn_norm[l]) != CCE_OK ||
            m->ffn_norm[l].numel != (size_t)D) {
            fprintf(stderr, "qwen35: layer %d post_attention_norm missing/"
                            "mis-sized — refusing\n", l);
            goto fail_unsupported;
        }

        if (e->kind[l] == CCE_QWEN35_LAYER_FULL_ATTN) {
            int in_d = 0, out_d = 0;
            snprintf(nm, sizeof nm, "blk.%d.attn_q_norm.weight", l);
            if (cce_gguf_load_tensor_by_name(g, nm, &m->attn_q_norm[l]) != CCE_OK ||
                m->attn_q_norm[l].numel != (size_t)head_dim) {
                fprintf(stderr, "qwen35: layer %d attn_q_norm missing/mis-sized "
                                "— refusing\n", l);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.attn_k_norm.weight", l);
            if (cce_gguf_load_tensor_by_name(g, nm, &m->attn_k_norm[l]) != CCE_OK ||
                m->attn_k_norm[l].numel != (size_t)head_dim) {
                fprintf(stderr, "qwen35: layer %d attn_k_norm missing/mis-sized "
                                "— refusing\n", l);
                goto fail_unsupported;
            }
            /* geometry cross-checks from tensor shapes */
            snprintf(nm, sizeof nm, "blk.%d.attn_q.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != D ||
                out_d != 2 * n_head * head_dim) {
                fprintf(stderr, "qwen35: layer %d attn_q [%d,%d] != [D, 2*H*hd] "
                                "— refusing\n", l, in_d, out_d);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.attn_k.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != D ||
                out_d != n_kv_head * head_dim) {
                fprintf(stderr, "qwen35: layer %d attn_k [%d,%d] != [D, KV*hd] "
                                "— refusing\n", l, in_d, out_d);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.attn_v.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != D ||
                out_d != n_kv_head * v_head_dim) {
                fprintf(stderr, "qwen35: layer %d attn_v [%d,%d] != [D, KV*vhd] "
                                "— refusing\n", l, in_d, out_d);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.attn_output.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || out_d != D ||
                in_d != n_head * v_head_dim) {
                fprintf(stderr, "qwen35: layer %d attn_output [%d,%d] != "
                                "[H*vhd, D] — refusing\n", l, in_d, out_d);
                goto fail_unsupported;
            }
        } else {
            int in_d = 0, out_d = 0, i;
            snprintf(nm, sizeof nm, "blk.%d.attn_qkv.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != D || out_d != qkv_dim) {
                fprintf(stderr, "qwen35: layer %d attn_qkv [%d,%d] != [D,%d] — "
                                "refusing\n", l, in_d, out_d, qkv_dim);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.attn_gate.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != D || out_d != hv * dv) {
                fprintf(stderr, "qwen35: layer %d attn_gate [%d,%d] != [D,%d] — "
                                "refusing\n", l, in_d, out_d, hv * dv);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.ssm_alpha.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != D || out_d != hv) {
                fprintf(stderr, "qwen35: layer %d ssm_alpha [%d,%d] != [D,%d] — "
                                "refusing\n", l, in_d, out_d, hv);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.ssm_beta.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != D || out_d != hv) {
                fprintf(stderr, "qwen35: layer %d ssm_beta [%d,%d] != [D,%d] — "
                                "refusing\n", l, in_d, out_d, hv);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.ssm_out.weight", l);
            if (!q35_tensor_2d(g, nm, &in_d, &out_d) || in_d != hv * dv || out_d != D) {
                fprintf(stderr, "qwen35: layer %d ssm_out [%d,%d] != [%d,D] — "
                                "refusing\n", l, in_d, out_d, hv * dv);
                goto fail_unsupported;
            }

            snprintf(nm, sizeof nm, "blk.%d.ssm_a", l);
            if (cce_gguf_load_tensor_by_name(g, nm, &e->ssm_a[l]) != CCE_OK ||
                e->ssm_a[l].numel != (size_t)hv) {
                fprintf(stderr, "qwen35: layer %d ssm_a missing/mis-sized — "
                                "refusing\n", l);
                goto fail_unsupported;
            }
            /* sign tripwire: the tensor is stored as -exp(A_log), strictly
               negative; a positive entry means a converter/sign change and a
               state that GROWS every step */
            for (i = 0; i < hv; i++) {
                if (!(e->ssm_a[l].data[i] <= 0.0f)) {
                    fprintf(stderr, "qwen35: layer %d ssm_a[%d]=%g > 0 (must be "
                                    "-exp(A_log)) — refusing\n", l, i,
                            (double)e->ssm_a[l].data[i]);
                    goto fail_unsupported;
                }
            }
            snprintf(nm, sizeof nm, "blk.%d.ssm_dt.bias", l);
            if (cce_gguf_load_tensor_by_name(g, nm, &e->ssm_dt_bias[l]) != CCE_OK ||
                e->ssm_dt_bias[l].numel != (size_t)hv) {
                fprintf(stderr, "qwen35: layer %d ssm_dt.bias missing/mis-sized "
                                "— refusing\n", l);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.ssm_conv1d.weight", l);
            if (cce_gguf_load_tensor_by_name(g, nm, &e->ssm_conv1d[l]) != CCE_OK ||
                e->ssm_conv1d[l].numel != (size_t)conv_k * qkv_dim) {
                fprintf(stderr, "qwen35: layer %d ssm_conv1d missing/mis-sized "
                                "— refusing\n", l);
                goto fail_unsupported;
            }
            snprintf(nm, sizeof nm, "blk.%d.ssm_norm.weight", l);
            if (cce_gguf_load_tensor_by_name(g, nm, &e->ssm_norm[l]) != CCE_OK ||
                e->ssm_norm[l].numel != (size_t)dv) {
                fprintf(stderr, "qwen35: layer %d ssm_norm missing/mis-sized — "
                                "refusing\n", l);
                goto fail_unsupported;
            }

            /* recurrent state: the wrapper pre-expands q/k to hv heads with
               the TILED map, so the core runs with hk == hv and its internal
               grouped broadcast is inert */
            {
                cce_qwen35_deltanet_config cfg;
                cfg.num_v_heads = hv;
                cfg.num_k_heads = hv;
                cfg.head_k_dim = dk;
                cfg.head_v_dim = dv;
                cfg.l2_eps = (m->rms_eps > 0.0f) ? m->rms_eps : 1e-6f;
                cfg.dt_bias = e->ssm_dt_bias[l].data;
                cfg.ssm_a = e->ssm_a[l].data;
                if (cce_qwen35_deltanet_init(&e->dstate[l], &cfg) != CCE_OK)
                    goto fail_oom;
            }
            e->conv_ring[l] = (float *)calloc((size_t)(conv_k - 1) * qkv_dim,
                                              sizeof(float));
            e->ckpt_state[l] = (float *)calloc((size_t)hv * dk * dv, sizeof(float));
            e->ckpt_ring[l] = (float *)calloc((size_t)(conv_k - 1) * qkv_dim,
                                              sizeof(float));
            if (!e->conv_ring[l] || !e->ckpt_state[l] || !e->ckpt_ring[l])
                goto fail_oom;
        }
    }
    Q35TRACE("norms + ssm residents loaded");

    /* ---- embedding / final norm / head tensors ---- */
    cce_gguf_load_tensor_by_name(g, "token_embd.weight", &m->tok_emb);
    cce_gguf_load_tensor_by_name(g, "output_norm.weight", &m->output_norm);
    if (!m->tok_emb.data || !m->output_norm.data ||
        m->output_norm.numel != (size_t)D) {
        fprintf(stderr, "qwen35: token_embd/output_norm missing — refusing\n");
        goto fail_unsupported;
    }
    if (cce_gguf_find_tensor(g, "output.weight") >= 0) {
        cce_gguf_load_tensor_by_name(g, "output.weight", &m->output);
    } else {
        m->output = m->tok_emb;      /* tied head: alias, never double-free */
        m->output.owns_memory = 0;
    }
    if (m->vocab_size <= 0 && m->tok_emb.ndim == 2) {
        int s0 = m->tok_emb.shape[0], s1 = m->tok_emb.shape[1];
        if (s0 == D) m->vocab_size = s1;
        else if (s1 == D) m->vocab_size = s0;
        else m->vocab_size = (s0 > s1 ? s0 : s1);
    }
    Q35TRACE("embedding/head loaded (V=%d)", m->vocab_size);

    /* ---- attention geometry (attn layers only; deltanet layers carry no
            KV slot — their mixing state lives in the ext) ---- */
    m->geom = (struct cce_attn_geom *)calloc((size_t)trunk, sizeof *m->geom);
    if (!m->geom) goto fail_oom;
    m->k_slot_floats = 0;
    m->v_slot_floats = 0;
    for (l = 0; l < trunk; l++) {
        struct cce_attn_geom *ge = &m->geom[l];
        if (e->kind[l] != CCE_QWEN35_LAYER_FULL_ATTN) continue;
        ge->q_dim = n_head * head_dim;        /* Q half only; gate is split off */
        ge->k_dim = n_kv_head * head_dim;
        ge->v_dim = n_kv_head * v_head_dim;
        ge->head_dim = head_dim;
        ge->v_head_dim = v_head_dim;
        ge->n_q = n_head;
        ge->n_k = n_kv_head;
        ge->n_v = n_kv_head;
        ge->rope_base = e->rope_base;
        ge->rope_dim = rope_dim;
        ge->k_off = m->k_slot_floats;
        ge->v_off = m->v_slot_floats;
        m->k_slot_floats += (size_t)ge->k_dim;
        m->v_slot_floats += (size_t)ge->v_dim;
    }
    if (m->k_slot_floats == 0) {
        fprintf(stderr, "qwen35: no full-attention layers found — refusing\n");
        goto fail_unsupported;
    }
    Q35TRACE("geom: %d layers, k_slot=%zu v_slot=%zu floats/pos",
             trunk, m->k_slot_floats, m->v_slot_floats);

    /* ---- KV cache (same sizing policy as the qwen2 loader) ---- */
    m->max_ctx = (m->ctx_len > 0) ? m->ctx_len : 2048;
    if (m->max_ctx > 8192) m->max_ctx = 8192;
    {
        const char *ce = getenv("CNET_MAX_CTX");
        if (ce) { int v = atoi(ce); if (v >= 8 && v < m->max_ctx) m->max_ctx = v; }
    }
    m->cur_pos = 0;
    m->k_cache = (float *)calloc((size_t)m->max_ctx * m->k_slot_floats,
                                 sizeof(float));
    m->v_cache = (float *)calloc((size_t)m->max_ctx * m->v_slot_floats,
                                 sizeof(float));
    if (!m->k_cache || !m->v_cache) goto fail_oom;

    cce_gguf_free(g);
    Q35TRACE("load complete");
    *out = m;
    return CCE_OK;

fail_oom:
    cce_gguf_free(g);
    cce_gguf_qwen2_free(m);
    return CCE_ERR_OOM;
fail_unsupported:
    cce_gguf_free(g);
    cce_gguf_qwen2_free(m);
    return CCE_ERR_UNSUPPORTED;
fail_io:
    cce_gguf_free(g);
    cce_gguf_qwen2_free(m);
    return CCE_ERR_IO;
}

/* ------------------------------------------------------------- forward */

/* YaRN NEOX rope over the first rope_dim dims of one head row, pairing
   (i, i+rope_dim/2). Matches ggml rope_yarn + the iterative theta chain of
   ggml_rope_cache_init (theta *= base^(-2/rope_dim) per pair). */
static void q35_rope_yarn(const cce_gguf_qwen35_ext *e, float *h, int pos) {
    const int half = e->rope_dim / 2;
    const float theta_scale = powf(e->rope_base, -2.0f / (float)e->rope_dim);
    float theta = (float)pos;
    int i;
    for (i = 0; i < half; i++) {
        const float theta_extrap = theta;
        const float theta_interp = e->yarn_freq_scale * theta_extrap;
        float th = theta_interp;
        float mscale = e->yarn_attn_factor;
        if (e->yarn_ext_factor != 0.0f) {
            float y = ((float)i - e->yarn_corr0) /
                      fmaxf(0.001f, e->yarn_corr1 - e->yarn_corr0);
            float ramp = (1.0f - fminf(1.0f, fmaxf(0.0f, y))) * e->yarn_ext_factor;
            th = theta_interp * (1.0f - ramp) + theta_extrap * ramp;
            mscale *= 1.0f + 0.1f * logf(1.0f / e->yarn_freq_scale);
        }
        {
            const float c = cosf(th) * mscale;
            const float s = sinf(th) * mscale;
            const float x0 = h[i];
            const float x1 = h[i + half];
            h[i]        = x0 * c - x1 * s;
            h[i + half] = x0 * s + x1 * c;
        }
        theta *= theta_scale;
    }
}

static float q35_sigmoidf(float x) { return 1.0f / (1.0f + expf(-x)); }
static float q35_siluf(float x)    { return x / (1.0f + expf(-x)); }

/* state protocol (see file header). Returns CCE_OK or refuses. */
static cce_result q35_state_enter(cce_gguf_qwen35_ext *e, long want,
                                  int is_probe) {
    int l;
    if (want == 0) {
        for (l = 0; l < e->n_layer; l++) {
            if (e->kind[l] != CCE_QWEN35_LAYER_DELTANET) continue;
            cce_qwen35_deltanet_reset(&e->dstate[l]);
            memset(e->conv_ring[l], 0,
                   (size_t)(e->conv_k - 1) * e->qkv_dim * sizeof(float));
        }
        e->stream_pos = 0;
        /* a reset starts a NEW stream: the checkpoint belongs to the old
           one and MUST die with it. Keeping it made ckpt_pos collide with
           the next unit's prefix length, which (a) skipped the fresh
           snapshot on the first suffix and (b) restored the PREVIOUS
           unit's recurrent state on every later suffix — a deterministic,
           self-consistent chimera that certifies its own students and is
           invisible to every same-mode gate. Caught only by the
           prefix-ON/OFF unit-level A/B (tk2590 flip, 2026-07-15). */
        e->ckpt_pos = -1;
    } else if (want == e->stream_pos) {
        /* continue */
    } else if (want == e->ckpt_pos) {
        for (l = 0; l < e->n_layer; l++) {
            if (e->kind[l] != CCE_QWEN35_LAYER_DELTANET) continue;
            memcpy(e->dstate[l].state, e->ckpt_state[l],
                   (size_t)e->hv * e->dk * e->dv * sizeof(float));
            memcpy(e->conv_ring[l], e->ckpt_ring[l],
                   (size_t)(e->conv_k - 1) * e->qkv_dim * sizeof(float));
        }
        e->stream_pos = want;
    } else {
        fprintf(stderr, "qwen35: cur_pos=%ld is neither 0, the live stream "
                        "position (%ld), nor the checkpoint (%ld) — a hybrid "
                        "cannot rewind recurrent state to an arbitrary "
                        "position; refusing\n", want, e->stream_pos, e->ckpt_pos);
        return CCE_ERR_UNSUPPORTED;
    }
    /* committing forwards checkpoint the entry state: the flagship's next
       rewind target is exactly this position. Snapshotting at ENTRY is what
       keeps a suffix run from clobbering the prefix checkpoint. */
    if (!is_probe && want > 0 && want != e->ckpt_pos) {
        for (l = 0; l < e->n_layer; l++) {
            if (e->kind[l] != CCE_QWEN35_LAYER_DELTANET) continue;
            memcpy(e->ckpt_state[l], e->dstate[l].state,
                   (size_t)e->hv * e->dk * e->dv * sizeof(float));
            memcpy(e->ckpt_ring[l], e->conv_ring[l],
                   (size_t)(e->conv_k - 1) * e->qkv_dim * sizeof(float));
        }
        e->ckpt_pos = want;
    }
    return CCE_OK;
}

cce_result cce_gguf_qwen35_forward_impl(cce_gguf_qwen2 *m, const int *tokens,
                                        int n_tokens, float *logits_out,
                                        int logits_cap) {
    cce_gguf_qwen35_ext *e = m->qwen35;
    const int D = m->n_embd;
    const int V = m->vocab_size;
    const int start_pos = m->cur_pos;
    const float eps = (m->rms_eps > 0.0f) ? m->rms_eps : 1e-6f;
    struct cce_clgemm *gpu = m->clgemm ? m->clgemm : cce_gguf__global_clgemm();
    struct cce_hipgemm *hip = m->hipgemm ? m->hipgemm : cce_gguf__global_hipgemm();
    cce_result rc = CCE_OK;
    char name[128];
    int l, t;

    if (!e || !m->geom || m->k_slot_floats == 0 || V <= 0) {
        fprintf(stderr, "qwen35: model not loaded through the qwen35 loader — "
                        "refusing\n");
        return CCE_ERR_UNSUPPORTED;
    }
    if ((m->probe_batch ? start_pos + 1 : start_pos + n_tokens) > m->max_ctx)
        return CCE_ERR_INVALID_ARG;
    if (m->layer_cap > 0) {
        fprintf(stderr, "qwen35: layer_cap is unsupported on the hybrid (a "
                        "capped forward desyncs recurrent state) — refusing\n");
        return CCE_ERR_UNSUPPORTED;
    }
    rc = q35_state_enter(e, (long)start_pos, m->probe_batch);
    if (rc != CCE_OK) return rc;

    cce_tensor x = {0};
    int xsh[2] = {n_tokens, D};
    if (cce_tensor_alloc(&x, xsh, 2) != CCE_OK) return CCE_ERR_OOM;
    for (t = 0; t < n_tokens; t++) {
        int tok = tokens[t];
        if (tok < 0 || tok >= V) tok = 0;
        memcpy(x.data + (size_t)t * D, m->tok_emb.data + (size_t)tok * D,
               (size_t)D * sizeof(float));
    }

    /* Residual stream + device KV for decode (full-attn layers + residual). */
    int use_stream = 0;
    if (gpu && n_tokens == 1 && !m->probe_batch &&
        cce_clgemm_stream_bind(gpu, D, m->max_ctx, m->k_slot_floats,
                               m->v_slot_floats) == 0 &&
        cce_clgemm_stream_set_x(gpu, x.data, D) == 0)
        use_stream = 1;

    float *scores = (float *)malloc((size_t)m->max_ctx * sizeof *scores);
    /* deltanet per-token scratch (widest case), shared across layers */
    float *dn_conv = (float *)malloc((size_t)e->qkv_dim * sizeof(float));
    float *dn_qe = (float *)malloc((size_t)e->hv * e->dk * sizeof(float));
    float *dn_ke = (float *)malloc((size_t)e->hv * e->dk * sizeof(float));
    float *dn_core = (float *)malloc((size_t)e->hv * e->dv * sizeof(float));
    float *dn_scratch_state = m->probe_batch
        ? (float *)malloc((size_t)e->hv * e->dk * e->dv * sizeof(float)) : NULL;
    if (!scores || !dn_conv || !dn_qe || !dn_ke || !dn_core ||
        (m->probe_batch && !dn_scratch_state)) {
        free(scores); free(dn_conv); free(dn_qe); free(dn_ke); free(dn_core);
        free(dn_scratch_state); cce_tensor_free(&x);
        return CCE_ERR_OOM;
    }

    #define Q35_FAIL(msg) do { \
        fprintf(stderr, "qwen35: %s at layer %d — refusing\n", (msg), l); \
        rc = CCE_ERR_UNSUPPORTED; goto done; } while (0)

    for (l = 0; l < m->n_layer; l++) {
        const struct cce_attn_geom *ge = &m->geom[l];
        int lnsh[2] = {n_tokens, D};
        cce_tensor ln1 = {0}, after_attn = {0};
        cce_tensor_alloc(&ln1, lnsh, 2);
        cce_tensor_alloc(&after_attn, lnsh, 2);
        if (!ln1.data || !after_attn.data) {
            cce_tensor_free(&ln1); cce_tensor_free(&after_attn);
            rc = CCE_ERR_OOM; goto done;
        }
        /* Keep host x mirrored when stream is live (GDN needs host residual). */
        if (use_stream && e->kind[l] != CCE_QWEN35_LAYER_FULL_ATTN)
            (void)cce_clgemm_stream_get_x(gpu, x.data, D);

        cce_gguf__rms_norm(&x, &m->attn_norm[l], eps, &ln1);

        if (e->kind[l] == CCE_QWEN35_LAYER_FULL_ATTN) {
            /* ---- gated causal attention ---- */
            const int qg_dim = 2 * ge->q_dim;      /* interleaved [q_h|gate_h] */
            cce_tensor qg = {0}, k = {0}, v = {0}, attn_out = {0};
            const float *qnw = m->attn_q_norm[l].data;
            const float *knw = m->attn_k_norm[l].data;
            const float scale = 1.0f / sqrtf((float)ge->head_dim);
            const int o_in = ge->n_q * ge->v_head_dim;
            int h;
            cce_tensor_alloc(&qg, (int[]){n_tokens, qg_dim}, 2);
            cce_tensor_alloc(&k, (int[]){n_tokens, ge->k_dim}, 2);
            cce_tensor_alloc(&v, (int[]){n_tokens, ge->v_dim}, 2);
            cce_tensor_alloc(&attn_out, (int[]){n_tokens, o_in}, 2);

            snprintf(name, sizeof name, "qwen35.blk.%d.qg_proj", l);
            cce_cascade *qg_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.k_proj", l);
            cce_cascade *k_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.v_proj", l);
            cce_cascade *v_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.o_proj", l);
            cce_cascade *o_cas = cce_forest_get_resident(m->forest, name);
            if (!qg_cas || !k_cas || !v_cas || !o_cas || !qg.data || !k.data ||
                !v.data || !attn_out.data || !qnw || !knw) {
                cce_tensor_free(&qg); cce_tensor_free(&k); cce_tensor_free(&v);
                cce_tensor_free(&attn_out); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("attn cascades/norms missing");
            }
            snprintf(name, sizeof name, "qwen35.blk.%d.qg_proj", l);
            cce_gguf__fire_capture(name, &ln1);
            if (cce_gguf__apply_linear_rows(gpu, hip, qg_cas, &ln1, &qg) != CCE_OK ||
                cce_gguf__apply_linear_rows(gpu, hip, k_cas, &ln1, &k) != CCE_OK ||
                cce_gguf__apply_linear_rows(gpu, hip, v_cas, &ln1, &v) != CCE_OK) {
                cce_tensor_free(&qg); cce_tensor_free(&k); cce_tensor_free(&v);
                cce_tensor_free(&attn_out); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("q/k/v projection failed (never leave uninitialized "
                         "buffers)");
            }

            for (t = 0; t < n_tokens; t++) {
                const int pos = m->probe_batch ? start_pos : start_pos + t;
                for (h = 0; h < ge->n_q; h++) {
                    /* per-head interleaved joint projection: [q_h | gate_h] */
                    float *qh = qg.data + (size_t)t * qg_dim +
                                (size_t)h * 2 * ge->head_dim;
                    float ss = 0.0f;
                    int d2;
                    for (d2 = 0; d2 < ge->head_dim; d2++) ss += qh[d2] * qh[d2];
                    ss = 1.0f / sqrtf(ss / ge->head_dim + eps);
                    for (d2 = 0; d2 < ge->head_dim; d2++)
                        qh[d2] = (qh[d2] * ss) * qnw[d2];
                    q35_rope_yarn(e, qh, pos);
                }
                for (h = 0; h < ge->n_k; h++) {
                    float *kh = k.data + (size_t)t * ge->k_dim +
                                (size_t)h * ge->head_dim;
                    float ss = 0.0f;
                    int d2;
                    for (d2 = 0; d2 < ge->head_dim; d2++) ss += kh[d2] * kh[d2];
                    ss = 1.0f / sqrtf(ss / ge->head_dim + eps);
                    for (d2 = 0; d2 < ge->head_dim; d2++)
                        kh[d2] = (kh[d2] * ss) * knw[d2];
                    q35_rope_yarn(e, kh, pos);
                }
                if (!m->probe_batch) {
                    memcpy(m->k_cache + (size_t)pos * m->k_slot_floats + ge->k_off,
                           k.data + (size_t)t * ge->k_dim,
                           (size_t)ge->k_dim * sizeof(float));
                    memcpy(m->v_cache + (size_t)pos * m->v_slot_floats + ge->v_off,
                           v.data + (size_t)t * ge->v_dim,
                           (size_t)ge->v_dim * sizeof(float));
                    if (use_stream)
                        (void)cce_clgemm_stream_kv_write(
                            gpu, pos, k.data + (size_t)t * ge->k_dim,
                            v.data + (size_t)t * ge->v_dim, ge->k_dim,
                            ge->v_dim, ge->k_off, ge->v_off);
                }
            }

            /* GPU attn: prefer device KV stream (no pack); else host pack ≥64. */
            if (n_tokens == 1 && !m->probe_batch && gpu) {
                float *qplain = (float *)malloc(
                    (size_t)ge->n_q * (size_t)ge->head_dim * sizeof(float));
                int abs_t0 = start_pos;
                int ok_gpu = 0;
                if (qplain) {
                    for (h = 0; h < ge->n_q; h++) {
                        const float *qh =
                            qg.data + (size_t)h * 2 * ge->head_dim;
                        memcpy(qplain + (size_t)h * ge->head_dim, qh,
                               (size_t)ge->head_dim * sizeof(float));
                    }
                    /* ensure this token's K/V are on device cache */
                    if (use_stream) {
                        for (t = 0; t < 1; t++) {
                            const int pos = abs_t0;
                            (void)cce_clgemm_stream_kv_write(
                                gpu, pos,
                                k.data + (size_t)t * ge->k_dim,
                                v.data + (size_t)t * ge->v_dim, ge->k_dim,
                                ge->v_dim, ge->k_off, ge->v_off);
                        }
                        if (cce_clgemm_stream_attn(
                                gpu, qplain, ge->k_off, ge->v_off, ge->n_q,
                                ge->n_k, ge->n_v, ge->head_dim, ge->v_head_dim,
                                0, abs_t0, scale, attn_out.data) == 0)
                            ok_gpu = 1;
                    }
                    if (!ok_gpu && abs_t0 + 1 >= 64 &&
                        cce_clgemm_attn_decode(
                            gpu, qplain, m->k_cache, m->v_cache,
                            m->k_slot_floats, m->v_slot_floats, ge->k_off,
                            ge->v_off, ge->n_q, ge->n_k, ge->n_v, ge->head_dim,
                            ge->v_head_dim, 0, abs_t0, scale,
                            attn_out.data) == 0)
                        ok_gpu = 1;
                    if (ok_gpu) {
                        for (h = 0; h < ge->n_q; h++) {
                            const float *gate_h =
                                qg.data + (size_t)h * 2 * ge->head_dim +
                                ge->head_dim;
                            float *oh = attn_out.data +
                                        (size_t)h * ge->v_head_dim;
                            int d2;
                            for (d2 = 0; d2 < ge->v_head_dim; d2++)
                                oh[d2] *= q35_sigmoidf(gate_h[d2]);
                        }
                    }
                    free(qplain);
                }
                if (ok_gpu) goto q35_attn_gpu_done;
            }
            for (h = 0; h < ge->n_q; h++) {
                const int kh_i = h / (ge->n_q / ge->n_k);
                const int vh_i = h / (ge->n_q / ge->n_v);
                for (t = 0; t < n_tokens; t++) {
                    const int abs_t = m->probe_batch ? start_pos : start_pos + t;
                    const float *qh = qg.data + (size_t)t * qg_dim +
                                      (size_t)h * 2 * ge->head_dim;
                    const float *gate_h = qh + ge->head_dim;
                    float maxs = -1e30f, sum = 0.0f;
                    float *oh = attn_out.data + (size_t)t * o_in +
                                (size_t)h * ge->v_head_dim;
                    int j, d2;
                    for (j = 0; j <= abs_t; j++) {
                        const float *kh = (m->probe_batch && j == abs_t)
                            ? k.data + (size_t)t * ge->k_dim +
                                  (size_t)kh_i * ge->head_dim
                            : m->k_cache + (size_t)j * m->k_slot_floats +
                                  ge->k_off + (size_t)kh_i * ge->head_dim;
                        float sacc = 0.0f;
                        for (d2 = 0; d2 < ge->head_dim; d2++)
                            sacc += qh[d2] * kh[d2];
                        scores[j] = sacc * scale;
                    }
                    for (j = 0; j <= abs_t; j++)
                        if (scores[j] > maxs) maxs = scores[j];
                    for (j = 0; j <= abs_t; j++) {
                        scores[j] = expf(scores[j] - maxs);
                        sum += scores[j];
                    }
                    for (j = 0; j <= abs_t; j++) scores[j] /= sum;
                    memset(oh, 0, (size_t)ge->v_head_dim * sizeof(float));
                    for (j = 0; j <= abs_t; j++) {
                        const float *vh = (m->probe_batch && j == abs_t)
                            ? v.data + (size_t)t * ge->v_dim +
                                  (size_t)vh_i * ge->v_head_dim
                            : m->v_cache + (size_t)j * m->v_slot_floats +
                                  ge->v_off + (size_t)vh_i * ge->v_head_dim;
                        for (d2 = 0; d2 < ge->v_head_dim; d2++)
                            oh[d2] += scores[j] * vh[d2];
                    }
                    /* Qwen3.5 output gate: context * sigmoid(gate) BEFORE
                       o_proj (llama.cpp qwen35.cpp attn_gated) */
                    for (d2 = 0; d2 < ge->v_head_dim; d2++)
                        oh[d2] *= q35_sigmoidf(gate_h[d2]);
                }
            }
        q35_attn_gpu_done: ;

            snprintf(name, sizeof name, "qwen35.blk.%d.o_proj", l);
            cce_gguf__fire_capture(name, &attn_out);
            if (cce_gguf__apply_linear_rows(gpu, hip, o_cas, &attn_out,
                                            &after_attn) != CCE_OK) {
                cce_tensor_free(&qg); cce_tensor_free(&k); cce_tensor_free(&v);
                cce_tensor_free(&attn_out); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("o_proj failed");
            }
            cce_tensor_free(&qg); cce_tensor_free(&k); cce_tensor_free(&v);
            cce_tensor_free(&attn_out);
        } else {
            /* ---- Gated-DeltaNet recurrent linear attention ---- */
            const int qkv_dim = e->qkv_dim, key_dim = e->key_dim;
            const int hv = e->hv, hk = e->hk, dk = e->dk, dv = e->dv;
            const int ring_cols = e->conv_k - 1;
            cce_tensor qkv = {0}, z = {0}, al = {0}, be = {0}, gn = {0};
            const float *convw = e->ssm_conv1d[l].data;
            const float *snw = e->ssm_norm[l].data;
            int h;
            cce_tensor_alloc(&qkv, (int[]){n_tokens, qkv_dim}, 2);
            cce_tensor_alloc(&z, (int[]){n_tokens, hv * dv}, 2);
            cce_tensor_alloc(&al, (int[]){n_tokens, hv}, 2);
            cce_tensor_alloc(&be, (int[]){n_tokens, hv}, 2);
            cce_tensor_alloc(&gn, (int[]){n_tokens, hv * dv}, 2);

            snprintf(name, sizeof name, "qwen35.blk.%d.qkv", l);
            cce_cascade *qkv_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.zgate", l);
            cce_cascade *z_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.alpha", l);
            cce_cascade *a_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.beta", l);
            cce_cascade *b_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.ssm_out", l);
            cce_cascade *so_cas = cce_forest_get_resident(m->forest, name);
            if (!qkv_cas || !z_cas || !a_cas || !b_cas || !so_cas || !qkv.data ||
                !z.data || !al.data || !be.data || !gn.data || !convw || !snw) {
                cce_tensor_free(&qkv); cce_tensor_free(&z); cce_tensor_free(&al);
                cce_tensor_free(&be); cce_tensor_free(&gn); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("deltanet cascades/residents missing");
            }
            /* qkv, z, alpha, beta ALL project from the post-attn_norm input
               (llama.cpp qwen35.cpp:357-368); z is NOT convolved */
            snprintf(name, sizeof name, "qwen35.blk.%d.qkv", l);
            cce_gguf__fire_capture(name, &ln1);
            if (cce_gguf__apply_linear_rows(gpu, hip, qkv_cas, &ln1, &qkv) != CCE_OK ||
                cce_gguf__apply_linear_rows(gpu, hip, z_cas, &ln1, &z) != CCE_OK ||
                cce_gguf__apply_linear_rows(gpu, hip, a_cas, &ln1, &al) != CCE_OK ||
                cce_gguf__apply_linear_rows(gpu, hip, b_cas, &ln1, &be) != CCE_OK) {
                cce_tensor_free(&qkv); cce_tensor_free(&z); cce_tensor_free(&al);
                cce_tensor_free(&be); cce_tensor_free(&gn); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("deltanet projections failed");
            }

            for (t = 0; t < n_tokens; t++) {
                const float *qkv_t = qkv.data + (size_t)t * qkv_dim;
                const float *z_t = z.data + (size_t)t * (size_t)hv * dv;
                float *ring = e->conv_ring[l];
                cce_qwen35_deltanet_state *st = &e->dstate[l];
                cce_qwen35_deltanet_state probe_st;
                int c, tap, d2;

                /* causal depthwise conv over the RAW qkv channels, window =
                   [ring cols oldest..newest | current], tap 0 = OLDEST
                   (ggml ssm_conv), then SiLU over all channels */
                for (c = 0; c < qkv_dim; c++) {
                    float acc = 0.0f;
                    const float *wc = convw + (size_t)c * e->conv_k;
                    for (tap = 0; tap < ring_cols; tap++)
                        acc += wc[tap] * ring[(size_t)tap * qkv_dim + c];
                    acc += wc[ring_cols] * qkv_t[c];
                    dn_conv[c] = q35_siluf(acc);
                }
                if (!m->probe_batch) {
                    /* push the RAW (pre-conv) qkv into the ring */
                    memmove(ring, ring + qkv_dim,
                            (size_t)(ring_cols - 1) * qkv_dim * sizeof(float));
                    memcpy(ring + (size_t)(ring_cols - 1) * qkv_dim, qkv_t,
                           (size_t)qkv_dim * sizeof(float));
                }

                /* split + TILED head expansion (value head h reads key head
                   h % hk — ggml_repeat_4d, the Qwen3.5 broadcast) */
                for (h = 0; h < hv; h++) {
                    const float *qsrc = dn_conv + (size_t)(h % hk) * dk;
                    const float *ksrc = dn_conv + key_dim + (size_t)(h % hk) * dk;
                    memcpy(dn_qe + (size_t)h * dk, qsrc, (size_t)dk * sizeof(float));
                    memcpy(dn_ke + (size_t)h * dk, ksrc, (size_t)dk * sizeof(float));
                }

                if (m->probe_batch) {
                    /* independent probe row: step a scratch COPY of the state;
                       live state and ring stay untouched (bit-identical to a
                       serial rewind-per-probe) */
                    probe_st = *st;
                    probe_st.state = dn_scratch_state;
                    memcpy(dn_scratch_state, st->state,
                           (size_t)hv * dk * dv * sizeof(float));
                    st = &probe_st;
                }
                if (cce_qwen35_deltanet_step(st, dn_qe, dn_ke,
                                             dn_conv + 2 * key_dim,
                                             al.data + (size_t)t * hv,
                                             be.data + (size_t)t * hv,
                                             dn_core) != CCE_OK) {
                    cce_tensor_free(&qkv); cce_tensor_free(&z);
                    cce_tensor_free(&al); cce_tensor_free(&be);
                    cce_tensor_free(&gn); cce_tensor_free(&ln1);
                    cce_tensor_free(&after_attn);
                    Q35_FAIL("deltanet step failed");
                }

                /* gated norm: per-head RMSNorm(out, ssm_norm) * SiLU(z) */
                for (h = 0; h < hv; h++) {
                    const float *oh = dn_core + (size_t)h * dv;
                    const float *zh = z_t + (size_t)h * dv;
                    float *dst = gn.data + (size_t)t * (size_t)hv * dv +
                                 (size_t)h * dv;
                    float ss = 0.0f;
                    for (d2 = 0; d2 < dv; d2++) ss += oh[d2] * oh[d2];
                    ss = 1.0f / sqrtf(ss / dv + eps);
                    for (d2 = 0; d2 < dv; d2++)
                        dst[d2] = (oh[d2] * ss) * snw[d2] * q35_siluf(zh[d2]);
                }
            }

            snprintf(name, sizeof name, "qwen35.blk.%d.ssm_out", l);
            cce_gguf__fire_capture(name, &gn);
            if (cce_gguf__apply_linear_rows(gpu, hip, so_cas, &gn,
                                            &after_attn) != CCE_OK) {
                cce_tensor_free(&qkv); cce_tensor_free(&z); cce_tensor_free(&al);
                cce_tensor_free(&be); cce_tensor_free(&gn); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("ssm_out failed");
            }
            cce_tensor_free(&qkv); cce_tensor_free(&z); cce_tensor_free(&al);
            cce_tensor_free(&be); cce_tensor_free(&gn);
        }

        /* residual (device stream keeps x on GPU when live) */
        if (use_stream && n_tokens == 1 &&
            cce_clgemm_stream_add_x_host(gpu, after_attn.data, D) == 0) {
            (void)cce_clgemm_stream_get_x(gpu, after_attn.data, D);
        } else {
            size_t i;
            for (i = 0; i < (size_t)n_tokens * D; i++)
                after_attn.data[i] += x.data[i];
        }

        /* ---- shared FFN (SwiGLU; pre-FFN norm = post_attention_norm,
                residual from the pre-norm stream) ---- */
        {
            const int mlp_hidden = m->feed_forward_length;
            cce_tensor ln2 = {0}, gate = {0}, upv = {0}, mid = {0}, down = {0};
            cce_tensor_alloc(&ln2, lnsh, 2);
            cce_tensor_alloc(&gate, (int[]){n_tokens, mlp_hidden}, 2);
            cce_tensor_alloc(&upv, (int[]){n_tokens, mlp_hidden}, 2);
            cce_tensor_alloc(&mid, (int[]){n_tokens, mlp_hidden}, 2);
            cce_tensor_alloc(&down, lnsh, 2);

            snprintf(name, sizeof name, "qwen35.blk.%d.gate_proj", l);
            cce_cascade *gate_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.up_proj", l);
            cce_cascade *up_cas = cce_forest_get_resident(m->forest, name);
            snprintf(name, sizeof name, "qwen35.blk.%d.down_proj", l);
            cce_cascade *down_cas = cce_forest_get_resident(m->forest, name);
            if (!gate_cas || !up_cas || !down_cas || mlp_hidden <= 0 ||
                !ln2.data || !gate.data || !upv.data || !mid.data || !down.data) {
                cce_tensor_free(&ln2); cce_tensor_free(&gate);
                cce_tensor_free(&upv); cce_tensor_free(&mid);
                cce_tensor_free(&down); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("ffn cascades missing");
            }
            cce_gguf__rms_norm(&after_attn, &m->ffn_norm[l], eps, &ln2);
            snprintf(name, sizeof name, "qwen35.blk.%d.gate_proj", l);
            cce_gguf__fire_capture(name, &ln2);
            if (cce_gguf__apply_linear_rows(gpu, hip, gate_cas, &ln2, &gate) != CCE_OK ||
                cce_gguf__apply_linear_rows(gpu, hip, up_cas, &ln2, &upv) != CCE_OK) {
                cce_tensor_free(&ln2); cce_tensor_free(&gate);
                cce_tensor_free(&upv); cce_tensor_free(&mid);
                cce_tensor_free(&down); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("gate/up failed");
            }
            {
                size_t i;
                for (i = 0; i < (size_t)n_tokens * mlp_hidden; i++)
                    mid.data[i] = q35_siluf(gate.data[i]) * upv.data[i];
            }
            snprintf(name, sizeof name, "qwen35.blk.%d.down_proj", l);
            cce_gguf__fire_capture(name, &mid);
            if (cce_gguf__apply_linear_rows(gpu, hip, down_cas, &mid, &down) != CCE_OK) {
                cce_tensor_free(&ln2); cce_tensor_free(&gate);
                cce_tensor_free(&upv); cce_tensor_free(&mid);
                cce_tensor_free(&down); cce_tensor_free(&ln1);
                cce_tensor_free(&after_attn);
                Q35_FAIL("down failed");
            }
            if (use_stream && n_tokens == 1 &&
                cce_clgemm_stream_set_x(gpu, after_attn.data, D) == 0 &&
                cce_clgemm_stream_add_x_host(gpu, down.data, D) == 0) {
                (void)cce_clgemm_stream_get_x(gpu, x.data, D);
            } else {
                size_t i;
                for (i = 0; i < (size_t)n_tokens * D; i++)
                    x.data[i] = after_attn.data[i] + down.data[i];
                if (use_stream)
                    (void)cce_clgemm_stream_set_x(gpu, x.data, D);
            }
            cce_tensor_free(&ln2); cce_tensor_free(&gate); cce_tensor_free(&upv);
            cce_tensor_free(&mid); cce_tensor_free(&down);
        }
        cce_tensor_free(&ln1);
        cce_tensor_free(&after_attn);

        cce_gguf__fire_layer_tap(l, x.data, n_tokens, D);
    }

    if (use_stream && gpu)
        (void)cce_clgemm_stream_get_x(gpu, x.data, D);

    /* ---- final norm + head (same contract as the qwen2 head: window head
            bit-identical on window ids, forest lm_head, output fallback,
            suppress erasure, per-probe-row logits) ---- */
    {
        cce_tensor fn = {0}, logits_t = {0};
        cce_cascade *head_cas;
        int row0, brow;
        cce_tensor_alloc(&fn, xsh, 2);
        cce_tensor_alloc(&logits_t, (int[]){1, V}, 2);
        if (!fn.data || !logits_t.data) {
            cce_tensor_free(&fn); cce_tensor_free(&logits_t);
            rc = CCE_ERR_OOM; goto done;
        }
        cce_gguf__rms_norm(&x, &m->output_norm, eps, &fn);

        /* NaN tripwire: a NaN stream satisfies every downstream gate
           vacuously (the gemma4 lesson) — hard-fail instead */
        row0 = m->probe_batch ? 0 : n_tokens - 1;
        for (brow = row0; brow < n_tokens; ++brow) {
            int d2;
            const float *fr = fn.data + (size_t)brow * D;
            for (d2 = 0; d2 < D; d2++) {
                if (isnan(fr[d2])) {
                    fprintf(stderr, "qwen35: NaN in the final-norm stream "
                                    "(row %d dim %d) — refusing\n", brow, d2);
                    cce_tensor_free(&fn); cce_tensor_free(&logits_t);
                    rc = CCE_ERR_UNSUPPORTED; goto done;
                }
            }
        }

        head_cas = cce_forest_get_resident(m->forest, "qwen2.lm_head");
        for (brow = row0; brow < n_tokens; ++brow) {
            float *row_out = m->probe_batch
                ? logits_out + (size_t)(brow - row0) * logits_cap : logits_out;
            cce_tensor fn_last = {0};
            int head_ok = 0;
            fn_last.data = fn.data + (size_t)brow * D;
            fn_last.shape[0] = 1;
            fn_last.shape[1] = D;
            fn_last.ndim = 2;
            fn_last.numel = (size_t)D;

            if (m->head_window_n > 0 && head_cas && head_cas->num_blocks == 1) {
                const cce_block *hb = &head_cas->blocks[0];
                if (hb->weights.data && !hb->w_q && !hb->w_trit &&
                    hb->weights.ndim == 2 && hb->weights.shape[0] == D &&
                    hb->weights.shape[1] == V) {
                    const float *last = fn_last.data;
                    const float *bias =
                        (hb->bias.numel == (size_t)V) ? hb->bias.data : NULL;
                    int wi, d2;
                    for (wi = 0; wi < m->head_window_n; ++wi) {
                        int id = m->head_window[wi];
                        float acc;
                        const float *wcol;
                        if (id < 0 || id >= V) continue;
                        acc = bias ? bias[id] : 0.0f;
                        wcol = hb->weights.data + id;
                        for (d2 = 0; d2 < D; ++d2)
                            acc += last[d2] * wcol[(size_t)d2 * V];
                        logits_t.data[id] = acc;
                    }
                    head_ok = 1;
                }
            }
            if (!head_ok && head_cas) {
                cce_gguf__fire_capture("qwen2.lm_head", &fn_last);
                if (cce_gguf__apply_linear_rows(gpu, hip, head_cas, &fn_last,
                                                &logits_t) == CCE_OK)
                    head_ok = 1;
            }
            if (!head_ok && m->output.data && m->output.ndim == 2) {
                int od0 = m->output.shape[0], od1 = m->output.shape[1];
                const float *last = fn_last.data;
                int vi, d2;
                for (vi = 0; vi < V && vi < logits_cap; vi++) {
                    float sacc = 0.0f;
                    if (od1 == D && od0 >= V) {
                        for (d2 = 0; d2 < D; d2++)
                            sacc += last[d2] * m->output.data[(size_t)vi * D + d2];
                    } else if (od0 == D && od1 >= V) {
                        for (d2 = 0; d2 < D; d2++)
                            sacc += last[d2] * m->output.data[(size_t)d2 * od1 + vi];
                    }
                    logits_t.data[vi] = sacc;
                }
                head_ok = 1;
            }
            if (!head_ok) {
                fprintf(stderr, "qwen35: no usable head — refusing\n");
                cce_tensor_free(&fn); cce_tensor_free(&logits_t);
                rc = CCE_ERR_UNSUPPORTED; goto done;
            }

            {
                size_t si;
                for (si = 0; si < m->n_suppress; si++) {
                    int64_t sid = m->suppress_ids[si];
                    if (sid >= 0 && sid < (int64_t)V)
                        logits_t.data[sid] = -HUGE_VALF;
                }
            }
            {
                int out_len = (V < logits_cap) ? V : logits_cap;
                memcpy(row_out, logits_t.data, (size_t)out_len * sizeof(float));
            }
        }
        cce_tensor_free(&fn);
        cce_tensor_free(&logits_t);
    }

    if (!m->probe_batch) {
        m->cur_pos += n_tokens;
        e->stream_pos = m->cur_pos;
    }
    rc = CCE_OK;

done:
    #undef Q35_FAIL
    free(scores); free(dn_conv); free(dn_qe); free(dn_ke); free(dn_core);
    free(dn_scratch_state);
    cce_tensor_free(&x);
    return rc;
}
