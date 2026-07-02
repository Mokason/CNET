/* Mamba-1 selective SSM runner. See include/cce/cce_ssm.h.
 *
 * Recurrence per token (textbook mamba-1 inference mode):
 *   xz  = in_proj(rmsnorm(x));  xin = xz[:E], z = xz[E:]
 *   conv ring push xin; xc = silu(depthwise_conv(state) + conv_bias)
 *   xdb = x_proj(xc);  dt_in = xdb[:R], B = xdb[R:R+N], C = xdb[R+N:R+2N]
 *   dt  = softplus(dt_proj(dt_in) + dt_bias)
 *   h[e,n] = exp(dt[e] * -exp(A_log[e,n])) * h[e,n] + dt[e] * B[n] * xc[e]
 *   y[e]  = sum_n h[e,n] * C[n] + D[e] * xc[e];  y *= silu(z)
 *   x = x + out_proj(y)
 */

#include "../../include/cce/cce_ssm.h"
#include "../../include/cce/cce_safetensors.h"
#include "../../include/cce/cce_gguf.h"
#include "../../include/cce/cce_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- small math helpers ---- */

static float ssm_silu(float x)     { return x / (1.0f + expf(-x)); }
static float ssm_softplus(float x) { return (x > 20.0f) ? x : log1pf(expf(x)); }

static void ssm_rms_norm(const float* in, const float* w, int d, float eps, float* out) {
    float ss = 0.0f;
    for (int i = 0; i < d; i++) ss += in[i] * in[i];
    ss = 1.0f / sqrtf(ss / d + eps);
    for (int i = 0; i < d; i++) out[i] = in[i] * ss * w[i];
}

/* Apply a linear specialist to one vector. */
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
    for (int i = 0; i < f->num_branches; i++) {
        if (strcmp(f->branches[i].name, name) == 0) return f->branches[i].cascade;
    }
    return NULL;
}

/* ---- unified tensor source: safetensors or gguf behind one probe ----
 *
 * Dim-order note: torch/safetensors record a matrix as [out, in]; GGUF (ggml)
 * records the SAME row-major bytes with the dims reversed, innermost first
 * ([in, out] — verified against the real gguf in Models/: ffn_up dims are
 * [n_embd, ffn]). src_shape therefore normalizes GGUF shapes back to torch
 * order so all downstream logic reasons in one convention; raw data bytes
 * are identical in both containers ([out][in] row-major).
 */

typedef struct {
    cce_safetensors* st;
    cce_gguf* gg;   /* ne-order dims: reversed on read */
} ssm_src;

static int src_find(const ssm_src* s, const char* name) {
    return s->st ? cce_safetensors_find(s->st, name) : cce_gguf_find_tensor(s->gg, name);
}

static cce_result src_load(const ssm_src* s, const char* name, cce_tensor* out) {
    int idx = src_find(s, name);
    if (idx < 0) return CCE_ERR_NOT_FOUND;
    return s->st ? cce_safetensors_load_as_tensor(s->st, idx, out)
                 : cce_gguf_load_as_tensor(s->gg, idx, out);
}

static cce_result src_shape(const ssm_src* s, const char* name, int* shape, int* ndim) {
    int idx = src_find(s, name);
    if (idx < 0) return CCE_ERR_NOT_FOUND;
    if (s->st) {
        cce_safetensor_meta m;
        cce_result rc = cce_safetensors_get_meta(s->st, idx, &m);
        if (rc != CCE_OK) return rc;
        *ndim = m.ndim;
        for (int d = 0; d < m.ndim && d < CCE_MAX_DIMS; d++) shape[d] = m.shape[d];
    } else {
        cce_gguf_tensor_meta m;
        cce_result rc = cce_gguf_get_tensor_meta(s->gg, idx, &m);
        if (rc != CCE_OK) return rc;
        *ndim = m.ndim;
        /* ggml ne order (innermost first) -> torch order */
        for (int d = 0; d < m.ndim && d < CCE_MAX_DIMS; d++) shape[d] = m.shape[m.ndim - 1 - d];
    }
    return CCE_OK;
}

/* Add a linear specialist from a matrix whose DATA is row-major [out][in]
 * (true for torch and ggml alike); out_d/in_d come pre-normalized from
 * src_shape so recorded dim order never matters here. */
static cce_result src_add_linear_branch(cce_forest* forest, const ssm_src* s,
                                        const char* wname, const char* bname,
                                        const char* brname) {
    int shape[CCE_MAX_DIMS], ndim = 0;
    cce_result rc = src_shape(s, wname, shape, &ndim);
    if (rc != CCE_OK || ndim != 2) return CCE_ERR_NOT_FOUND;
    int out_d = shape[0], in_d = shape[1];

    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, 2) != CCE_OK) return CCE_ERR_OOM;
    rc = cce_cascade_add_linear_head(cas, in_d, out_d, 0.0f);
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }
    cce_block* blk = &cas->blocks[cas->num_blocks - 1];

    if (s->st) {
        rc = cce_safetensors_populate_block(blk, s->st, wname,
                                            (bname && src_find(s, bname) >= 0) ? bname : NULL, 1);
    } else {
        /* manual fill: cce_gguf_populate_block trusts recorded dims, which are
           ne-order here; transpose the raw [out][in] bytes ourselves */
        size_t numel = (size_t)out_d * in_d;
        float* tmp = (float*)malloc(numel * sizeof(float));
        int wi = src_find(s, wname);
        if (!tmp || wi < 0 || cce_gguf_load_f32(s->gg, wi, tmp, numel) != CCE_OK) {
            free(tmp); cce_cascade_destroy(cas); return CCE_ERR_IO;
        }
        for (int o = 0; o < out_d; o++)
            for (int i = 0; i < in_d; i++)
                blk->weights.data[(size_t)i * out_d + o] = tmp[(size_t)o * in_d + i];
        free(tmp);
        rc = CCE_OK;
        if (bname) {
            int bi = src_find(s, bname);
            if (bi >= 0) {
                if (blk->bias.numel == 0) {
                    int bsh[1] = { out_d };
                    cce_tensor_alloc(&blk->bias, bsh, 1);
                }
                if (blk->bias.numel == (size_t)out_d)
                    cce_gguf_load_f32(s->gg, bi, blk->bias.data, out_d);
            }
        }
    }
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }

    int idx = -1;
    rc = cce_forest_add_cascade_branch(forest, cas, brname, &idx);
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }
    free(cas); /* forest owns its shallow copy; add_branch zeroed our shell */
    return CCE_OK;
}

/* ---- naming schemes ---- */

typedef struct {
    const char* embed[2];    /* candidates */
    const char* norm_f;
    const char* head;        /* NULL entries mean tied */
    /* per-layer printf patterns (one %d) */
    const char* norm;
    const char* in_w;   const char* in_b;
    const char* conv_w; const char* conv_b;
    const char* x_w;
    const char* dt_w;   const char* dt_b;
    const char* a_log;
    const char* dvec;
    const char* out_w;  const char* out_b;
} ssm_names;

static const ssm_names k_hf_names = {
    { "backbone.embeddings.weight", "backbone.embedding.weight" },
    "backbone.norm_f.weight",
    "lm_head.weight",
    "backbone.layers.%d.norm.weight",
    "backbone.layers.%d.mixer.in_proj.weight",  "backbone.layers.%d.mixer.in_proj.bias",
    "backbone.layers.%d.mixer.conv1d.weight",   "backbone.layers.%d.mixer.conv1d.bias",
    "backbone.layers.%d.mixer.x_proj.weight",
    "backbone.layers.%d.mixer.dt_proj.weight",  "backbone.layers.%d.mixer.dt_proj.bias",
    "backbone.layers.%d.mixer.A_log",
    "backbone.layers.%d.mixer.D",
    "backbone.layers.%d.mixer.out_proj.weight", "backbone.layers.%d.mixer.out_proj.bias",
};

static const ssm_names k_gguf_names = {
    { "token_embd.weight", NULL },
    "output_norm.weight",
    "output.weight",
    "blk.%d.attn_norm.weight",
    "blk.%d.ssm_in.weight",     "blk.%d.ssm_in.bias",
    "blk.%d.ssm_conv1d.weight", "blk.%d.ssm_conv1d.bias",
    "blk.%d.ssm_x.weight",
    "blk.%d.ssm_dt.weight",     "blk.%d.ssm_dt.bias",
    "blk.%d.ssm_a",
    "blk.%d.ssm_d",
    "blk.%d.ssm_out.weight",    "blk.%d.ssm_out.bias",
};

/* ---- loader ---- */

static cce_result ssm_load_from_src(cce_ssm_model** out, const ssm_src* s, const ssm_names* nm) {
    char name[160], bias[160];

    /* layer count: highest index whose A_log exists */
    int L = 0;
    for (;; L++) {
        snprintf(name, sizeof(name), nm->a_log, L);
        if (src_find(s, name) < 0) break;
        if (L > 4096) return CCE_ERR_UNSUPPORTED;
    }
    if (L == 0) return CCE_ERR_NOT_FOUND;

    /* dims from layer-0 tensor shapes; no config file needed */
    int shape[CCE_MAX_DIMS], ndim = 0;
    snprintf(name, sizeof(name), nm->a_log, 0);
    if (src_shape(s, name, shape, &ndim) != CCE_OK || ndim != 2) return CCE_ERR_UNSUPPORTED;
    int E = shape[0], N = shape[1];

    snprintf(name, sizeof(name), nm->in_w, 0);
    if (src_shape(s, name, shape, &ndim) != CCE_OK || ndim != 2) return CCE_ERR_UNSUPPORTED;
    if (shape[0] != 2 * E) return CCE_ERR_UNSUPPORTED;
    int D = shape[1];

    snprintf(name, sizeof(name), nm->x_w, 0);
    if (src_shape(s, name, shape, &ndim) != CCE_OK || ndim != 2 || shape[1] != E) return CCE_ERR_UNSUPPORTED;
    int R = shape[0] - 2 * N;
    if (R <= 0) return CCE_ERR_UNSUPPORTED;

    snprintf(name, sizeof(name), nm->conv_w, 0);
    if (src_shape(s, name, shape, &ndim) != CCE_OK) return CCE_ERR_UNSUPPORTED;
    int K = shape[ndim - 1];                       /* [E,1,K] or [E,K] */
    if (K <= 0 || shape[0] != E) return CCE_ERR_UNSUPPORTED;

    const char* emb_name = NULL;
    for (int c = 0; c < 2; c++) {
        if (nm->embed[c] && src_find(s, nm->embed[c]) >= 0) { emb_name = nm->embed[c]; break; }
    }
    if (!emb_name) return CCE_ERR_NOT_FOUND;

    cce_ssm_model* m = (cce_ssm_model*)calloc(1, sizeof(*m));
    if (!m) return CCE_ERR_OOM;
    m->n_layer = L; m->d_model = D; m->d_inner = E;
    m->d_state = N; m->d_conv = K; m->dt_rank = R;
    m->norm_eps = 1e-5f;
    m->bos_token_id = -1; m->eos_token_id = -1;

    const char* tmpf = "ssm_forest.cce";
    remove(tmpf);
    if (cce_forest_open(&m->forest, tmpf, 4 * L + 8) != CCE_OK) { free(m); return CCE_ERR_IO; }

    m->in_cas  = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->x_cas   = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->dt_cas  = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->out_cas = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
    m->norm   = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->conv_w = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->conv_b = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->A_log  = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->Dvec   = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    if (!m->in_cas || !m->x_cas || !m->dt_cas || !m->out_cas ||
        !m->norm || !m->conv_w || !m->conv_b || !m->A_log || !m->Dvec) {
        cce_ssm_free(m); return CCE_ERR_OOM;
    }

    char brname[96];
    for (int l = 0; l < L; l++) {
        snprintf(name, sizeof(name), nm->in_w, l);
        snprintf(bias, sizeof(bias), nm->in_b, l);
        snprintf(brname, sizeof(brname), "mamba.blk.%d.in_proj", l);
        if (src_add_linear_branch(m->forest, s, name, bias, brname) != CCE_OK) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }

        snprintf(name, sizeof(name), nm->x_w, l);
        snprintf(brname, sizeof(brname), "mamba.blk.%d.x_proj", l);
        if (src_add_linear_branch(m->forest, s, name, NULL, brname) != CCE_OK) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }

        snprintf(name, sizeof(name), nm->dt_w, l);
        snprintf(bias, sizeof(bias), nm->dt_b, l);
        snprintf(brname, sizeof(brname), "mamba.blk.%d.dt_proj", l);
        if (src_add_linear_branch(m->forest, s, name, bias, brname) != CCE_OK) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }

        snprintf(name, sizeof(name), nm->out_w, l);
        snprintf(bias, sizeof(bias), nm->out_b, l);
        snprintf(brname, sizeof(brname), "mamba.blk.%d.out_proj", l);
        if (src_add_linear_branch(m->forest, s, name, bias, brname) != CCE_OK) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }

        /* every per-channel tensor is size-checked against the layer-0 dims:
           the forward reads these raw (no apply_row guard), so an undersized
           layer-1 tensor would be a silent heap overread */
        snprintf(name, sizeof(name), nm->norm, l);
        if (src_load(s, name, &m->norm[l]) != CCE_OK || m->norm[l].numel != (size_t)D) { cce_ssm_free(m); return CCE_ERR_UNSUPPORTED; }
        snprintf(name, sizeof(name), nm->conv_w, l);
        if (src_load(s, name, &m->conv_w[l]) != CCE_OK || m->conv_w[l].numel != (size_t)E * K) { cce_ssm_free(m); return CCE_ERR_UNSUPPORTED; }
        snprintf(name, sizeof(name), nm->conv_b, l);
        src_load(s, name, &m->conv_b[l]); /* optional; used only when numel == E */
        snprintf(name, sizeof(name), nm->a_log, l);
        if (src_load(s, name, &m->A_log[l]) != CCE_OK || m->A_log[l].numel != (size_t)E * N) { cce_ssm_free(m); return CCE_ERR_UNSUPPORTED; }
        snprintf(name, sizeof(name), nm->dvec, l);
        if (src_load(s, name, &m->Dvec[l]) != CCE_OK || m->Dvec[l].numel != (size_t)E) { cce_ssm_free(m); return CCE_ERR_UNSUPPORTED; }
    }

    /* lm head: explicit or tied to the embedding */
    const char* head_name = (nm->head && src_find(s, nm->head) >= 0) ? nm->head : emb_name;
    if (src_add_linear_branch(m->forest, s, head_name, NULL, "mamba.lm_head") != CCE_OK) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }

    /* dims via src_shape (normalized order); the loaded tensor's recorded
       shape would be ne-order for gguf, but its bytes are [V][D] either way */
    {
        int esh[CCE_MAX_DIMS], endim = 0;
        if (src_shape(s, emb_name, esh, &endim) != CCE_OK || endim != 2 || esh[1] != D) {
            cce_ssm_free(m); return CCE_ERR_UNSUPPORTED;
        }
        m->vocab_size = esh[0];
    }
    if (src_load(s, emb_name, &m->tok_emb) != CCE_OK ||
        m->tok_emb.numel != (size_t)m->vocab_size * D) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }
    if (src_load(s, nm->norm_f, &m->norm_f) != CCE_OK || m->norm_f.numel != (size_t)D) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }

    /* cache cascade pointers */
    for (int l = 0; l < L; l++) {
        snprintf(brname, sizeof(brname), "mamba.blk.%d.in_proj", l);  m->in_cas[l]  = find_cascade(m->forest, brname);
        snprintf(brname, sizeof(brname), "mamba.blk.%d.x_proj", l);   m->x_cas[l]   = find_cascade(m->forest, brname);
        snprintf(brname, sizeof(brname), "mamba.blk.%d.dt_proj", l);  m->dt_cas[l]  = find_cascade(m->forest, brname);
        snprintf(brname, sizeof(brname), "mamba.blk.%d.out_proj", l); m->out_cas[l] = find_cascade(m->forest, brname);
        if (!m->in_cas[l] || !m->x_cas[l] || !m->dt_cas[l] || !m->out_cas[l]) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }
    }
    m->head_cas = find_cascade(m->forest, "mamba.lm_head");
    if (!m->head_cas) { cce_ssm_free(m); return CCE_ERR_NOT_FOUND; }

    m->conv_state = (float*)calloc((size_t)L * E * K, sizeof(float));
    m->ssm_state  = (float*)calloc((size_t)L * E * N, sizeof(float));
    if (!m->conv_state || !m->ssm_state) { cce_ssm_free(m); return CCE_ERR_OOM; }
    m->cur_pos = 0;

    *out = m;
    return CCE_OK;
}

cce_result cce_ssm_load(cce_ssm_model** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    FILE* f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    char magic[4] = {0};
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    if (got != 4) return CCE_ERR_IO;

    ssm_src s = {0};
    cce_result rc;
    if (memcmp(magic, "GGUF", 4) == 0) {
        rc = cce_gguf_load(path, &s.gg);
        if (rc != CCE_OK) return rc;
        rc = ssm_load_from_src(out, &s, &k_gguf_names);
        if (rc == CCE_OK && s.gg) {
            (*out)->bos_token_id = cce_gguf_get_bos_token_id(s.gg);
            (*out)->eos_token_id = cce_gguf_get_eos_token_id(s.gg);
        }
        cce_gguf_free(s.gg);
    } else {
        rc = cce_safetensors_load(path, &s.st);
        if (rc != CCE_OK) return rc;
        rc = ssm_load_from_src(out, &s, &k_hf_names);
        cce_safetensors_free(s.st);
    }
    return rc;
}

/* ---- forward ---- */

cce_result cce_ssm_forward(cce_ssm_model* m, const int* tokens, int n_tokens,
                           float* logits_out, int logits_cap) {
    if (!m || !tokens || n_tokens < 1 || !logits_out) return CCE_ERR_INVALID_ARG;

    const int D = m->d_model, E = m->d_inner, N = m->d_state, K = m->d_conv, R = m->dt_rank;
    const int V = m->vocab_size;

    float* x    = (float*)malloc((size_t)D * sizeof(float));
    float* xn   = (float*)malloc((size_t)D * sizeof(float));
    float* xz   = (float*)malloc((size_t)2 * E * sizeof(float));
    float* xc   = (float*)malloc((size_t)E * sizeof(float));
    float* xdb  = (float*)malloc((size_t)(R + 2 * N) * sizeof(float));
    float* dt   = (float*)malloc((size_t)E * sizeof(float));
    float* y    = (float*)malloc((size_t)E * sizeof(float));
    float* dout = (float*)malloc((size_t)D * sizeof(float));
    float* logits = (float*)malloc((size_t)V * sizeof(float));
    if (!x || !xn || !xz || !xc || !xdb || !dt || !y || !dout || !logits) {
        free(x); free(xn); free(xz); free(xc); free(xdb); free(dt); free(y); free(dout); free(logits);
        return CCE_ERR_OOM;
    }

    cce_result rc = CCE_OK;
    for (int t = 0; t < n_tokens && rc == CCE_OK; t++) {
        int tok = tokens[t];
        if (tok < 0 || tok >= V) tok = 0;
        memcpy(x, m->tok_emb.data + (size_t)tok * D, (size_t)D * sizeof(float));

        for (int l = 0; l < m->n_layer && rc == CCE_OK; l++) {
            ssm_rms_norm(x, m->norm[l].data, D, m->norm_eps, xn);

            rc = apply_row(m->in_cas[l], xn, D, xz, 2 * E);
            if (rc != CCE_OK) break;
            const float* xin = xz;
            const float* z   = xz + E;

            /* depthwise causal conv over the ring buffer */
            float* cs = m->conv_state + (size_t)l * E * K;
            const float* cw = m->conv_w[l].data;
            const float* cb = (m->conv_b[l].numel == (size_t)E) ? m->conv_b[l].data : NULL;
            for (int e = 0; e < E; e++) {
                float* ce = cs + (size_t)e * K;
                memmove(ce, ce + 1, (size_t)(K - 1) * sizeof(float));
                ce[K - 1] = xin[e];
                float acc = cb ? cb[e] : 0.0f;
                const float* we = cw + (size_t)e * K;
                for (int k = 0; k < K; k++) acc += we[k] * ce[k];
                xc[e] = ssm_silu(acc);
            }

            rc = apply_row(m->x_cas[l], xc, E, xdb, R + 2 * N);
            if (rc != CCE_OK) break;
            const float* B = xdb + R;
            const float* C = xdb + R + N;

            rc = apply_row(m->dt_cas[l], xdb, R, dt, E);
            if (rc != CCE_OK) break;
            for (int e = 0; e < E; e++) dt[e] = ssm_softplus(dt[e]);

            /* selective scan step */
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
                y[e] = (acc + dv[e] * xc[e]) * ssm_silu(z[e]);
            }

            rc = apply_row(m->out_cas[l], y, E, dout, D);
            if (rc != CCE_OK) break;
            for (int i = 0; i < D; i++) x[i] += dout[i];
        }
        if (rc != CCE_OK) break;

        ssm_rms_norm(x, m->norm_f.data, D, m->norm_eps, xn);
        rc = apply_row(m->head_cas, xn, D, logits, V);
        if (rc != CCE_OK) break;

        if (t == n_tokens - 1) {
            int n = (V < logits_cap) ? V : logits_cap;
            memcpy(logits_out, logits, (size_t)n * sizeof(float));
        }
        m->cur_pos++;
    }

    free(x); free(xn); free(xz); free(xc); free(xdb); free(dt); free(y); free(dout); free(logits);
    return rc;
}

void cce_ssm_reset(cce_ssm_model* m) {
    if (!m) return;
    if (m->conv_state) memset(m->conv_state, 0, (size_t)m->n_layer * m->d_inner * m->d_conv * sizeof(float));
    if (m->ssm_state)  memset(m->ssm_state, 0, (size_t)m->n_layer * m->d_inner * m->d_state * sizeof(float));
    m->cur_pos = 0;
}

void cce_ssm_free(cce_ssm_model* m) {
    if (!m) return;
    if (m->norm || m->conv_w || m->conv_b || m->A_log || m->Dvec) {
        for (int l = 0; l < m->n_layer; l++) {
            if (m->norm)   cce_tensor_free(&m->norm[l]);
            if (m->conv_w) cce_tensor_free(&m->conv_w[l]);
            if (m->conv_b) cce_tensor_free(&m->conv_b[l]);
            if (m->A_log)  cce_tensor_free(&m->A_log[l]);
            if (m->Dvec)   cce_tensor_free(&m->Dvec[l]);
        }
    }
    free(m->norm); free(m->conv_w); free(m->conv_b); free(m->A_log); free(m->Dvec);
    free(m->in_cas); free(m->x_cas); free(m->dt_cas); free(m->out_cas);
    cce_tensor_free(&m->tok_emb);
    cce_tensor_free(&m->norm_f);
    if (m->forest) cce_forest_close(m->forest);
    remove("ssm_forest.cce"); /* temp backing file */
    free(m->conv_state);
    free(m->ssm_state);
    free(m);
}
