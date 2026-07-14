/* Native hybrid attention+SSM runner. See include/cce/cce_hybrid.h.
 *
 * ONE residual stream, threaded through the layers in model order; each layer
 * dispatches to its mixer:
 *   ATTN: rmsnorm -> q/k/v -> NEOX rope -> GQA causal softmax -> o_proj ->
 *         residual -> rmsnorm -> SwiGLU MLP -> residual   (llama/qwen2 core)
 *   SSM : rmsnorm -> in_proj -> depthwise conv -> x/dt proj -> selective scan
 *         -> out_proj -> residual                          (mamba-1 core)
 *
 * The linear projections are the reused CCE primitive: every one is a named
 * cce_cascade specialist in a single cce_forest, built with the same
 * cce_gguf_add_linear_branch the pure-transformer and pure-SSM loaders use.
 * Attention state (a per-attention-layer KV cache) and SSM state (a per-SSM-
 * layer conv ring + scan state) coexist and advance together as tokens stream.
 *
 * Dim-order note: GGUF records a 2D weight in ggml ne order (innermost/input
 * first) — the reverse of torch — with identical row-major [out][in] bytes.
 * cce_gguf_add_linear_branch / cce_gguf_populate_block normalize this, so a
 * branch's block ends up in [in][out] layout regardless of container.
 */

#include "../../include/cce/cce_hybrid.h"
#include "../../include/cce/cce_gguf.h"
#include "../../include/cce/cce_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- small math helpers (mirror the validated pure runners) ---- */

static float hyb_silu(float x)     { return x / (1.0f + expf(-x)); }
static float hyb_softplus(float x) { return (x > 20.0f) ? x : log1pf(expf(x)); }

static void hyb_rms_norm(const float* in, const float* w, int d, float eps, float* out) {
    float ss = 0.0f;
    for (int i = 0; i < d; i++) ss += in[i] * in[i];
    ss = 1.0f / sqrtf(ss / d + eps);
    for (int i = 0; i < d; i++) out[i] = in[i] * ss * w[i];
}

/* NEOX rotary: pair dim i with i+head_dim/2 (identical to gguf_apply_rope). */
static void hyb_rope(float* vec, int head_dim, int pos, float base) {
    int half = head_dim / 2;
    for (int i = 0; i < half; i++) {
        float freq = 1.0f / powf(base, (float)(2 * i) / head_dim);
        float val = (float)pos * freq;
        float c = cosf(val), s = sinf(val);
        float v0 = vec[i], v1 = vec[i + half];
        vec[i]        = v0 * c - v1 * s;
        vec[i + half] = v0 * s + v1 * c;
    }
}

/* Apply a linear specialist to one vector [din] -> [dout]. */
static cce_result apply_row(cce_cascade* cas, const float* in, int din, float* out, int dout) {
    cce_tensor tin = {0};
    int ish[1] = { din };
    if (cce_tensor_alloc(&tin, ish, 1) != CCE_OK) return CCE_ERR_OOM;
    memcpy(tin.data, in, (size_t)din * sizeof(float));
    cce_tensor tout = {0};
    cce_result rc = cce_cascade_forward(cas, &tin, &tout);
    if (rc == CCE_OK) {
        if (tout.numel != (size_t)dout) rc = CCE_ERR_UNSUPPORTED;
        else memcpy(out, tout.data, (size_t)dout * sizeof(float));
    }
    cce_tensor_free(&tout);
    cce_tensor_free(&tin);
    return rc;
}

static cce_cascade* find_cascade(cce_forest* f, const char* name) {
    for (int i = 0; i < f->num_branches; i++)
        if (strcmp(f->branches[i].name, name) == 0) return f->branches[i].cascade;
    return NULL;
}

/* ne-order metadata helpers: shape[0] = innermost = input dim. */
static int gg_out_in(const cce_gguf* g, const char* name, int* out_d, int* in_d) {
    int idx = cce_gguf_find_tensor(g, name);
    if (idx < 0) return -1;
    cce_gguf_tensor_meta m;
    if (cce_gguf_get_tensor_meta(g, idx, &m) != CCE_OK || m.ndim != 2) return -1;
    *in_d = m.shape[0];
    *out_d = m.shape[1];
    return 0;
}
static long gg_numel(const cce_gguf* g, const char* name) {
    int idx = cce_gguf_find_tensor(g, name);
    if (idx < 0) return -1;
    cce_gguf_tensor_meta m;
    if (cce_gguf_get_tensor_meta(g, idx, &m) != CCE_OK) return -1;
    long n = 1;
    for (int d = 0; d < m.ndim; d++) n *= m.shape[d];
    return n;
}
static int gg_has(const cce_gguf* g, const char* name) {
    return cce_gguf_find_tensor(g, name) >= 0;
}

/* ---- loader ---- */

cce_result cce_hybrid_load(cce_hybrid_model** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_gguf* g = NULL;
    cce_result rc = cce_gguf_load(path, &g);
    if (rc != CCE_OK) return rc;

    int L = cce_gguf_get_n_layer(g);
    int D = cce_gguf_get_hidden_size(g);
    if (L <= 0 || L > 4096 || D <= 0) { cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }

    char nm[160], bnm[160];

    /* per-layer kind by STRUCTURE (ssm_in vs attn_q); a layer that is neither
       refuses the load — never silently dropped. */
    int* kind = (int*)calloc(L, sizeof(int));
    if (!kind) { cce_gguf_free(g); return CCE_ERR_OOM; }
    int n_attn = 0, n_ssm = 0;
    int la = -1, ls = -1;              /* first attn / first ssm layer */
    for (int l = 0; l < L; l++) {
        snprintf(nm, sizeof(nm), "blk.%d.ssm_in.weight", l);
        snprintf(bnm, sizeof(bnm), "blk.%d.attn_q.weight", l);
        int is_ssm = gg_has(g, nm), is_attn = gg_has(g, bnm);
        if (is_ssm && !is_attn) { kind[l] = CCE_HYBRID_LAYER_SSM; n_ssm++; if (ls < 0) ls = l; }
        else if (is_attn && !is_ssm) { kind[l] = CCE_HYBRID_LAYER_ATTN; n_attn++; if (la < 0) la = l; }
        else {
            fprintf(stderr, "cce_hybrid: layer %d is neither a complete attention "
                            "nor a complete ssm block — refusing\n", l);
            free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
        }
    }
    /* a hybrid needs BOTH mixers; pure models are owned by the pure loaders */
    if (n_attn == 0 || n_ssm == 0) { free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }

    /* attention geometry from the first attention layer */
    int n_head = cce_gguf_get_n_heads(g);
    int n_kv_head = cce_gguf_get_n_kv_heads(g);
    if (n_kv_head <= 0) n_kv_head = n_head;
    int q_dim = 0, k_dim = 0, v_dim = 0, o_in = 0, o_out = 0, in_chk = 0;
    int ffn_hidden = 0, up_out = 0, down_in = 0, down_out = 0;
    if (n_head <= 0) { free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
    snprintf(nm, sizeof(nm), "blk.%d.attn_q.weight", la);
    if (gg_out_in(g, nm, &q_dim, &in_chk) != 0 || in_chk != D || q_dim % n_head != 0) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    int head_dim = q_dim / n_head;
    snprintf(nm, sizeof(nm), "blk.%d.attn_k.weight", la);
    if (gg_out_in(g, nm, &k_dim, &in_chk) != 0 || in_chk != D || head_dim == 0 || k_dim % head_dim != 0) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    snprintf(nm, sizeof(nm), "blk.%d.attn_v.weight", la);
    if (gg_out_in(g, nm, &v_dim, &in_chk) != 0 || in_chk != D || v_dim % head_dim != 0) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    int n_k = k_dim / head_dim, n_v = v_dim / head_dim;
    if (n_k <= 0 || n_v <= 0 || n_head % n_k != 0 || n_head % n_v != 0) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    snprintf(nm, sizeof(nm), "blk.%d.attn_output.weight", la);
    if (gg_out_in(g, nm, &o_out, &o_in) != 0 || o_out != D || o_in != n_head * head_dim) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    snprintf(nm, sizeof(nm), "blk.%d.ffn_gate.weight", la);
    if (gg_out_in(g, nm, &ffn_hidden, &in_chk) != 0 || in_chk != D) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    snprintf(nm, sizeof(nm), "blk.%d.ffn_up.weight", la);
    if (gg_out_in(g, nm, &up_out, &in_chk) != 0 || up_out != ffn_hidden || in_chk != D) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    snprintf(nm, sizeof(nm), "blk.%d.ffn_down.weight", la);
    if (gg_out_in(g, nm, &down_out, &down_in) != 0 || down_out != D || down_in != ffn_hidden) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }

    /* ssm geometry from the first ssm layer (mamba-1, same as cce_ssm) */
    int E = 0, N = 0, K = 0, R = 0, XDB = 0, sin_out = 0, sx_out = 0, sdt_out = 0, sdt_in = 0, sout_out = 0, sout_in = 0;
    snprintf(nm, sizeof(nm), "blk.%d.ssm_in.weight", ls);
    if (gg_out_in(g, nm, &sin_out, &in_chk) != 0 || in_chk != D || sin_out % 2 != 0) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    E = sin_out / 2;
    long a_n = 0, conv_n = 0;
    snprintf(nm, sizeof(nm), "blk.%d.ssm_a", ls);      a_n = gg_numel(g, nm);
    snprintf(nm, sizeof(nm), "blk.%d.ssm_conv1d.weight", ls); conv_n = gg_numel(g, nm);
    if (a_n <= 0 || E == 0 || a_n % E != 0 || conv_n <= 0 || conv_n % E != 0) {
        free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
    }
    N = (int)(a_n / E);
    K = (int)(conv_n / E);
    snprintf(nm, sizeof(nm), "blk.%d.ssm_x.weight", ls);
    if (gg_out_in(g, nm, &sx_out, &in_chk) != 0 || in_chk != E) { free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
    XDB = sx_out;
    R = XDB - 2 * N;
    if (R <= 0) { free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
    snprintf(nm, sizeof(nm), "blk.%d.ssm_dt.weight", ls);
    if (gg_out_in(g, nm, &sdt_out, &sdt_in) != 0 || sdt_out != E || sdt_in != R) { free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
    snprintf(nm, sizeof(nm), "blk.%d.ssm_out.weight", ls);
    if (gg_out_in(g, nm, &sout_out, &sout_in) != 0 || sout_out != D || sout_in != E) { free(kind); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }

    cce_hybrid_model* m = (cce_hybrid_model*)calloc(1, sizeof(*m));
    if (!m) { free(kind); cce_gguf_free(g); return CCE_ERR_OOM; }
    m->layer_kind = kind;
    m->n_layer = L; m->n_attn_layer = n_attn; m->n_ssm_layer = n_ssm;
    m->d_model = D;
    m->n_head = n_head; m->n_kv_head = n_kv_head; m->head_dim = head_dim;
    m->q_dim = q_dim; m->k_dim = k_dim; m->v_dim = v_dim; m->ffn_hidden = ffn_hidden;
    m->d_inner = E; m->d_state = N; m->d_conv = K; m->dt_rank = R;
    float eps = cce_gguf_get_rms_eps(g);
    m->norm_eps = (eps > 0.0f) ? eps : 1e-5f;
    float rb = cce_gguf_get_rope_freq_base(g);
    m->rope_base = (rb > 0.0f) ? rb : 10000.0f;
    m->bos_token_id = cce_gguf_get_bos_token_id(g);
    m->eos_token_id = cce_gguf_get_eos_token_id(g);
    int ctx = cce_gguf_get_context_length(g);
    m->max_ctx = (ctx > 0 && ctx <= 8192) ? ctx : 4096;

    /* forest of linear specialists; unique scratch per load so pooled loads in
       one process never remove()/rewrite each other's live backing file */
    static int g_hybrid_seq = 0;
    snprintf(m->forest_scratch, sizeof(m->forest_scratch), "hybrid_forest.%d.cce", ++g_hybrid_seq);
    remove(m->forest_scratch);
    if (cce_forest_open(&m->forest, m->forest_scratch, 7 * L + 8) != CCE_OK) {
        cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_IO;
    }

    m->q_cas   = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->k_cas   = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->v_cas   = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->o_cas   = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->gate_cas = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->up_cas  = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->down_cas = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->in_cas  = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->x_cas   = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->dt_cas  = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->out_cas = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->mixer_norm = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->ffn_norm = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->conv_w  = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->conv_b  = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->A_log   = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->Dvec    = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->k_cache = (float**)calloc(L, sizeof(float*));
    m->v_cache = (float**)calloc(L, sizeof(float*));
    if (!m->q_cas || !m->k_cas || !m->v_cas || !m->o_cas || !m->gate_cas ||
        !m->up_cas || !m->down_cas || !m->in_cas || !m->x_cas || !m->dt_cas ||
        !m->out_cas || !m->mixer_norm || !m->ffn_norm || !m->conv_w ||
        !m->conv_b || !m->A_log || !m->Dvec || !m->k_cache || !m->v_cache) {
        cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_OOM;
    }

#define ADD_BR(w, bias, br) do { \
        if (cce_gguf_add_linear_branch(m->forest, g, (w), (bias), (br), 0.0f) < 0) { \
            fprintf(stderr, "cce_hybrid: branch %s failed to build — refusing\n", (br)); \
            cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; } } while (0)
#define LOAD_T(w, dst, want) do { \
        if (cce_gguf_load_tensor_by_name(g, (w), (dst)) != CCE_OK || (dst)->numel != (size_t)(want)) { \
            fprintf(stderr, "cce_hybrid: tensor %s missing/mis-sized — refusing\n", (w)); \
            cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; } } while (0)

    char br[96];
    for (int l = 0; l < L; l++) {
        /* pre-mixer norm (both kinds share attn_norm) */
        snprintf(nm, sizeof(nm), "blk.%d.attn_norm.weight", l);
        LOAD_T(nm, &m->mixer_norm[l], D);

        if (kind[l] == CCE_HYBRID_LAYER_ATTN) {
            int qd, kd, vd, oo, oi, gh, uh, dd, di, chk;
            snprintf(nm, sizeof(nm), "blk.%d.attn_q.weight", l);      if (gg_out_in(g, nm, &qd, &chk) || qd != q_dim || chk != D) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.attn_k.weight", l);      if (gg_out_in(g, nm, &kd, &chk) || kd != k_dim || chk != D) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.attn_v.weight", l);      if (gg_out_in(g, nm, &vd, &chk) || vd != v_dim || chk != D) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.attn_output.weight", l); if (gg_out_in(g, nm, &oo, &oi) || oo != D || oi != n_head * head_dim) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.ffn_gate.weight", l);    if (gg_out_in(g, nm, &gh, &chk) || gh != ffn_hidden || chk != D) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.ffn_up.weight", l);      if (gg_out_in(g, nm, &uh, &chk) || uh != ffn_hidden || chk != D) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.ffn_down.weight", l);    if (gg_out_in(g, nm, &dd, &di) || dd != D || di != ffn_hidden) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }

            snprintf(nm, sizeof(nm), "blk.%d.attn_q.weight", l);      snprintf(br, sizeof(br), "hybrid.blk.%d.q_proj", l);    ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.attn_k.weight", l);      snprintf(br, sizeof(br), "hybrid.blk.%d.k_proj", l);    ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.attn_v.weight", l);      snprintf(br, sizeof(br), "hybrid.blk.%d.v_proj", l);    ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.attn_output.weight", l); snprintf(br, sizeof(br), "hybrid.blk.%d.o_proj", l);    ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.ffn_gate.weight", l);    snprintf(br, sizeof(br), "hybrid.blk.%d.gate_proj", l); ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.ffn_up.weight", l);      snprintf(br, sizeof(br), "hybrid.blk.%d.up_proj", l);   ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.ffn_down.weight", l);    snprintf(br, sizeof(br), "hybrid.blk.%d.down_proj", l); ADD_BR(nm, NULL, br);

            snprintf(nm, sizeof(nm), "blk.%d.ffn_norm.weight", l);    LOAD_T(nm, &m->ffn_norm[l], D);

            m->k_cache[l] = (float*)calloc((size_t)m->max_ctx * k_dim, sizeof(float));
            m->v_cache[l] = (float*)calloc((size_t)m->max_ctx * v_dim, sizeof(float));
            if (!m->k_cache[l] || !m->v_cache[l]) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_OOM; }
        } else { /* SSM layer: same shape checks + branch build as cce_ssm */
            int io, ii, xo, dto, dti, oo2, oi2, chk;
            snprintf(nm, sizeof(nm), "blk.%d.ssm_in.weight", l);  if (gg_out_in(g, nm, &io, &ii) || io != 2 * E || ii != D) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.ssm_x.weight", l);   if (gg_out_in(g, nm, &xo, &chk) || xo != XDB || chk != E) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.ssm_dt.weight", l);  if (gg_out_in(g, nm, &dto, &dti) || dto != E || dti != R) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }
            snprintf(nm, sizeof(nm), "blk.%d.ssm_out.weight", l); if (gg_out_in(g, nm, &oo2, &oi2) || oo2 != D || oi2 != E) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED; }

            snprintf(nm, sizeof(nm), "blk.%d.ssm_in.weight", l);  snprintf(br, sizeof(br), "hybrid.blk.%d.in_proj", l);  ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_x.weight", l);   snprintf(br, sizeof(br), "hybrid.blk.%d.x_proj", l);   ADD_BR(nm, NULL, br);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_dt.weight", l);
            snprintf(bnm, sizeof(bnm), "blk.%d.ssm_dt.bias", l);
            snprintf(br, sizeof(br), "hybrid.blk.%d.dt_proj", l);
            ADD_BR(nm, gg_has(g, bnm) ? bnm : NULL, br);  /* dt bias baked into the cascade */
            snprintf(nm, sizeof(nm), "blk.%d.ssm_out.weight", l); snprintf(br, sizeof(br), "hybrid.blk.%d.out_proj", l); ADD_BR(nm, NULL, br);

            snprintf(nm, sizeof(nm), "blk.%d.ssm_conv1d.weight", l); LOAD_T(nm, &m->conv_w[l], (size_t)E * K);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_conv1d.bias", l);   cce_gguf_load_tensor_by_name(g, nm, &m->conv_b[l]); /* optional */
            snprintf(nm, sizeof(nm), "blk.%d.ssm_a", l);             LOAD_T(nm, &m->A_log[l], (size_t)E * N);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_d", l);             LOAD_T(nm, &m->Dvec[l], (size_t)E);
        }
    }

    /* shared embedding, final norm, head (explicit or tied to the embedding) */
    {
        int te_out, te_in;
        if (gg_out_in(g, "token_embd.weight", &te_out, &te_in) != 0 || te_in != D) {
            cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_UNSUPPORTED;
        }
        m->vocab_size = te_out;
    }
    LOAD_T("token_embd.weight", &m->tok_emb, (size_t)m->vocab_size * D);
    LOAD_T("output_norm.weight", &m->output_norm, (size_t)D);
    const char* head = gg_has(g, "output.weight") ? "output.weight" : "token_embd.weight";
    ADD_BR(head, NULL, "hybrid.lm_head");

#undef ADD_BR
#undef LOAD_T

    /* cache cascade pointers */
    for (int l = 0; l < L; l++) {
        if (kind[l] == CCE_HYBRID_LAYER_ATTN) {
            snprintf(br, sizeof(br), "hybrid.blk.%d.q_proj", l);    m->q_cas[l]    = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.k_proj", l);    m->k_cas[l]    = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.v_proj", l);    m->v_cas[l]    = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.o_proj", l);    m->o_cas[l]    = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.gate_proj", l); m->gate_cas[l] = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.up_proj", l);   m->up_cas[l]   = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.down_proj", l); m->down_cas[l] = find_cascade(m->forest, br);
            if (!m->q_cas[l] || !m->k_cas[l] || !m->v_cas[l] || !m->o_cas[l] ||
                !m->gate_cas[l] || !m->up_cas[l] || !m->down_cas[l]) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_NOT_FOUND; }
        } else {
            snprintf(br, sizeof(br), "hybrid.blk.%d.in_proj", l);  m->in_cas[l]  = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.x_proj", l);   m->x_cas[l]   = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.dt_proj", l);  m->dt_cas[l]  = find_cascade(m->forest, br);
            snprintf(br, sizeof(br), "hybrid.blk.%d.out_proj", l); m->out_cas[l] = find_cascade(m->forest, br);
            if (!m->in_cas[l] || !m->x_cas[l] || !m->dt_cas[l] || !m->out_cas[l]) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_NOT_FOUND; }
        }
    }
    m->head_cas = find_cascade(m->forest, "hybrid.lm_head");
    if (!m->head_cas) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_NOT_FOUND; }

    /* recurrent state: conv ring + scan state per ssm layer (attn slices unused) */
    m->conv_state = (float*)calloc((size_t)L * E * K, sizeof(float));
    m->ssm_state  = (float*)calloc((size_t)L * E * N, sizeof(float));
    if (!m->conv_state || !m->ssm_state) { cce_hybrid_free(m); cce_gguf_free(g); return CCE_ERR_OOM; }
    m->cur_pos = 0;

    cce_gguf_free(g);
    *out = m;
    return CCE_OK;
}

/* ---- forward: one MIXED pass, one residual stream ---- */

cce_result cce_hybrid_forward(cce_hybrid_model* m, const int* tokens, int n_tokens,
                              float* logits_out, int logits_cap) {
    if (!m || !tokens || n_tokens < 1 || !logits_out) return CCE_ERR_INVALID_ARG;

    const int D = m->d_model, V = m->vocab_size;
    const int E = m->d_inner, N = m->d_state, K = m->d_conv, R = m->dt_rank;
    const int hd = m->head_dim, q_dim = m->q_dim, k_dim = m->k_dim, v_dim = m->v_dim;
    const int ffn = m->ffn_hidden;
    const float eps = m->norm_eps;
    const int start_pos = m->cur_pos;

    if (start_pos + n_tokens > m->max_ctx) return CCE_ERR_INVALID_ARG;

    float* x     = (float*)malloc((size_t)D * sizeof(float));
    float* ln    = (float*)malloc((size_t)D * sizeof(float));
    float* q     = (float*)malloc((size_t)(q_dim > 0 ? q_dim : 1) * sizeof(float));
    float* kk    = (float*)malloc((size_t)(k_dim > 0 ? k_dim : 1) * sizeof(float));
    float* vv    = (float*)malloc((size_t)(v_dim > 0 ? v_dim : 1) * sizeof(float));
    float* attn  = (float*)malloc((size_t)(q_dim > 0 ? q_dim : 1) * sizeof(float));
    float* ao    = (float*)malloc((size_t)D * sizeof(float));
    float* ln2   = (float*)malloc((size_t)D * sizeof(float));
    float* gate  = (float*)malloc((size_t)(ffn > 0 ? ffn : 1) * sizeof(float));
    float* up    = (float*)malloc((size_t)(ffn > 0 ? ffn : 1) * sizeof(float));
    float* dn    = (float*)malloc((size_t)D * sizeof(float));
    float* xz    = (float*)malloc((size_t)2 * E * sizeof(float));
    float* xc    = (float*)malloc((size_t)E * sizeof(float));
    float* xdb   = (float*)malloc((size_t)(R + 2 * N) * sizeof(float));
    float* dt    = (float*)malloc((size_t)E * sizeof(float));
    float* y     = (float*)malloc((size_t)E * sizeof(float));
    float* sc    = (float*)malloc((size_t)m->max_ctx * sizeof(float));
    float* logits = (float*)malloc((size_t)V * sizeof(float));
    if (!x || !ln || !q || !kk || !vv || !attn || !ao || !ln2 || !gate || !up ||
        !dn || !xz || !xc || !xdb || !dt || !y || !sc || !logits) {
        free(x); free(ln); free(q); free(kk); free(vv); free(attn); free(ao);
        free(ln2); free(gate); free(up); free(dn); free(xz); free(xc); free(xdb);
        free(dt); free(y); free(sc); free(logits);
        return CCE_ERR_OOM;
    }

    cce_result rc = CCE_OK;
    for (int t = 0; t < n_tokens && rc == CCE_OK; t++) {
        int tok = tokens[t];
        if (tok < 0 || tok >= V) tok = 0;
        memcpy(x, m->tok_emb.data + (size_t)tok * D, (size_t)D * sizeof(float));
        int pos = start_pos + t;

        for (int l = 0; l < m->n_layer && rc == CCE_OK; l++) {
            hyb_rms_norm(x, m->mixer_norm[l].data, D, eps, ln);

            if (m->layer_kind[l] == CCE_HYBRID_LAYER_ATTN) {
                if ((rc = apply_row(m->q_cas[l], ln, D, q, q_dim)) != CCE_OK) break;
                if ((rc = apply_row(m->k_cas[l], ln, D, kk, k_dim)) != CCE_OK) break;
                if ((rc = apply_row(m->v_cas[l], ln, D, vv, v_dim)) != CCE_OK) break;

                for (int h = 0; h < m->n_head; h++)    hyb_rope(q  + (size_t)h * hd, hd, pos, m->rope_base);
                for (int h = 0; h < m->n_kv_head; h++) hyb_rope(kk + (size_t)h * hd, hd, pos, m->rope_base);

                memcpy(m->k_cache[l] + (size_t)pos * k_dim, kk, (size_t)k_dim * sizeof(float));
                memcpy(m->v_cache[l] + (size_t)pos * v_dim, vv, (size_t)v_dim * sizeof(float));

                float scale = 1.0f / sqrtf((float)hd);
                int grp_k = m->n_head / m->n_kv_head;
                for (int h = 0; h < m->n_head; h++) {
                    int kvh = h / grp_k;
                    const float* qh = q + (size_t)h * hd;
                    float mx = -1e30f, sum = 0.0f;
                    for (int j = 0; j <= pos; j++) {
                        const float* kh = m->k_cache[l] + (size_t)j * k_dim + (size_t)kvh * hd;
                        float a = 0.0f;
                        for (int d = 0; d < hd; d++) a += qh[d] * kh[d];
                        sc[j] = a * scale;
                        if (sc[j] > mx) mx = sc[j];
                    }
                    for (int j = 0; j <= pos; j++) { sc[j] = expf(sc[j] - mx); sum += sc[j]; }
                    float* oh = attn + (size_t)h * hd;
                    memset(oh, 0, (size_t)hd * sizeof(float));
                    for (int j = 0; j <= pos; j++) {
                        float wgt = sc[j] / sum;
                        const float* vh = m->v_cache[l] + (size_t)j * v_dim + (size_t)kvh * hd;
                        for (int d = 0; d < hd; d++) oh[d] += wgt * vh[d];
                    }
                }

                if ((rc = apply_row(m->o_cas[l], attn, q_dim, ao, D)) != CCE_OK) break;
                for (int i = 0; i < D; i++) x[i] += ao[i];

                hyb_rms_norm(x, m->ffn_norm[l].data, D, eps, ln2);
                if ((rc = apply_row(m->gate_cas[l], ln2, D, gate, ffn)) != CCE_OK) break;
                if ((rc = apply_row(m->up_cas[l], ln2, D, up, ffn)) != CCE_OK) break;
                for (int i = 0; i < ffn; i++) gate[i] = hyb_silu(gate[i]) * up[i];
                if ((rc = apply_row(m->down_cas[l], gate, ffn, dn, D)) != CCE_OK) break;
                for (int i = 0; i < D; i++) x[i] += dn[i];
            } else { /* SSM mixer (mamba-1) */
                if ((rc = apply_row(m->in_cas[l], ln, D, xz, 2 * E)) != CCE_OK) break;
                const float* z = xz + E;

                float* cs = m->conv_state + (size_t)l * E * K;
                const float* cw = m->conv_w[l].data;
                const float* cb = (m->conv_b[l].numel == (size_t)E) ? m->conv_b[l].data : NULL;
                for (int e = 0; e < E; e++) {
                    float* ce = cs + (size_t)e * K;
                    memmove(ce, ce + 1, (size_t)(K - 1) * sizeof(float));
                    ce[K - 1] = xz[e];
                    float acc = cb ? cb[e] : 0.0f;
                    const float* we = cw + (size_t)e * K;
                    for (int kkk = 0; kkk < K; kkk++) acc += we[kkk] * ce[kkk];
                    xc[e] = hyb_silu(acc);
                }

                if ((rc = apply_row(m->x_cas[l], xc, E, xdb, R + 2 * N)) != CCE_OK) break;
                const float* B = xdb + R;
                const float* C = xdb + R + N;

                if ((rc = apply_row(m->dt_cas[l], xdb, R, dt, E)) != CCE_OK) break;
                for (int e = 0; e < E; e++) dt[e] = hyb_softplus(dt[e]);

                float* h = m->ssm_state + (size_t)l * E * N;
                const float* al = m->A_log[l].data;
                const float* dv = m->Dvec[l].data;
                for (int e = 0; e < E; e++) {
                    float* he = h + (size_t)e * N;
                    const float* ae = al + (size_t)e * N;
                    float acc = 0.0f;
                    for (int n = 0; n < N; n++) {
                        float dA = expf(dt[e] * -expf(ae[n]));
                        he[n] = dA * he[n] + dt[e] * B[n] * xc[e];
                        acc += he[n] * C[n];
                    }
                    y[e] = (acc + dv[e] * xc[e]) * hyb_silu(z[e]);
                }

                if ((rc = apply_row(m->out_cas[l], y, E, dn, D)) != CCE_OK) break;
                for (int i = 0; i < D; i++) x[i] += dn[i];
            }
        }
        if (rc != CCE_OK) break;

        if (t == n_tokens - 1) {
            hyb_rms_norm(x, m->output_norm.data, D, eps, ln);
            rc = apply_row(m->head_cas, ln, D, logits, V);
            if (rc == CCE_OK) {
                int n = (V < logits_cap) ? V : logits_cap;
                memcpy(logits_out, logits, (size_t)n * sizeof(float));
            }
        }
    }

    if (rc == CCE_OK) m->cur_pos = start_pos + n_tokens;

    free(x); free(ln); free(q); free(kk); free(vv); free(attn); free(ao);
    free(ln2); free(gate); free(up); free(dn); free(xz); free(xc); free(xdb);
    free(dt); free(y); free(sc); free(logits);
    return rc;
}

void cce_hybrid_reset(cce_hybrid_model* m) {
    if (!m) return;
    if (m->k_cache && m->v_cache)
        for (int l = 0; l < m->n_layer; l++) {
            if (m->k_cache[l]) memset(m->k_cache[l], 0, (size_t)m->max_ctx * m->k_dim * sizeof(float));
            if (m->v_cache[l]) memset(m->v_cache[l], 0, (size_t)m->max_ctx * m->v_dim * sizeof(float));
        }
    if (m->conv_state) memset(m->conv_state, 0, (size_t)m->n_layer * m->d_inner * m->d_conv * sizeof(float));
    if (m->ssm_state)  memset(m->ssm_state, 0, (size_t)m->n_layer * m->d_inner * m->d_state * sizeof(float));
    m->cur_pos = 0;
}

void cce_hybrid_free(cce_hybrid_model* m) {
    if (!m) return;
    if (m->mixer_norm || m->ffn_norm || m->conv_w || m->conv_b || m->A_log || m->Dvec) {
        for (int l = 0; l < m->n_layer; l++) {
            if (m->mixer_norm) cce_tensor_free(&m->mixer_norm[l]);
            if (m->ffn_norm)   cce_tensor_free(&m->ffn_norm[l]);
            if (m->conv_w)     cce_tensor_free(&m->conv_w[l]);
            if (m->conv_b)     cce_tensor_free(&m->conv_b[l]);
            if (m->A_log)      cce_tensor_free(&m->A_log[l]);
            if (m->Dvec)       cce_tensor_free(&m->Dvec[l]);
        }
    }
    free(m->mixer_norm); free(m->ffn_norm); free(m->conv_w); free(m->conv_b);
    free(m->A_log); free(m->Dvec);
    free(m->q_cas); free(m->k_cas); free(m->v_cas); free(m->o_cas);
    free(m->gate_cas); free(m->up_cas); free(m->down_cas);
    free(m->in_cas); free(m->x_cas); free(m->dt_cas); free(m->out_cas);
    if (m->k_cache) for (int l = 0; l < m->n_layer; l++) free(m->k_cache[l]);
    if (m->v_cache) for (int l = 0; l < m->n_layer; l++) free(m->v_cache[l]);
    free(m->k_cache); free(m->v_cache);
    free(m->conv_state); free(m->ssm_state);
    cce_tensor_free(&m->tok_emb);
    cce_tensor_free(&m->output_norm);
    if (m->forest) cce_forest_close(m->forest);
    if (m->forest_scratch[0]) remove(m->forest_scratch);
    free(m->layer_kind);
    free(m);
}
