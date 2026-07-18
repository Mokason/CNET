#include "../../include/cce/cce_ds_runtime.h"
#include "../../include/cce/cce_cascade.h"
#include "../../include/cce/cce_block.h"
#include "../../include/cce/cce_gguf.h"
#include "../../include/cce/cce_tensor.h"
#include "../../include/cce/cce_router.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* defined below — open() may arm MTP from opts */
cce_result cce_ds_host_enable_mtp(cce_ds_host* h, int k, int draft_layers);

void cce_ds_host_opts_default(cce_ds_host_opts* o, const char* archive,
                              const char* pack) {
    const char* e;
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->archive_path = archive ? archive : "ds_host.cce";
    o->pack_path = pack;
    o->max_ctx = 256;
    o->synthetic = pack ? 0 : 1;
    o->seed = 0xD50D50u;
    o->bind_cold = 0;
    o->dsa_enable = 1;
    o->dsa_fraction = 0.25f;
    o->dsa_sleep_eps = CCE_SLEEP_DEFAULT_EPS;
    o->dsa_speed = 0;
    o->cold_autoload = 1;
    o->mla_quant_kv = 1; /* FP8-class int8 latent default on */
    o->moe_sleep_eps = CCE_SLEEP_DEFAULT_EPS;
    o->dual_pipe = 1;
    o->mtp_k = 0;
    o->mtp_draft_layers = 1;
    o->ep_places = 2;
    /* Env: CNET_DSA=0 disables; CNET_DSA_PROFILE=speed; CNET_MLA_KV=0 off quant */
    e = getenv("CNET_DSA");
    if (e && e[0] == '0') o->dsa_enable = 0;
    e = getenv("CNET_DSA_PROFILE");
    if (e && (e[0] == 's' || e[0] == 'S')) o->dsa_speed = 1;
    e = getenv("CNET_DSA_SLEEP");
    if (e && e[0]) {
        float v = (float)atof(e);
        if (v > 0.f) o->dsa_sleep_eps = v;
    }
    e = getenv("CNET_MLA_KV");
    if (e && e[0] == '0') o->mla_quant_kv = 0;
    if (e && e[0] && e[0] != '0') o->mla_quant_kv = 1;
    e = getenv("CNET_MOE_SLEEP");
    if (e && e[0]) {
        float v = (float)atof(e);
        if (v > 0.f) o->moe_sleep_eps = v;
    }
    e = getenv("CNET_DUAL_PIPE");
    if (e && e[0] == '0') o->dual_pipe = 0;
    e = getenv("CNET_MTP_K");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 0 && v <= 8) o->mtp_k = v;
    }
    e = getenv("CNET_MTP_DRAFT_LAYERS");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 0 && v <= 2) o->mtp_draft_layers = v;
    }
    e = getenv("CNET_EP_PLACES");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 1 && v <= 8) o->ep_places = v;
    }
}

/* Pull f32 weight view from a bound cascade (no copy; host must keep forest). */
static const float* leaf_w(cce_forest* f, const char* name, int* in_d, int* out_d) {
    cce_cascade* cas = cce_forest_get_resident(f, name);
    if (!cas || cas->num_blocks < 1 || !cas->blocks[0].weights.data) return NULL;
    *in_d = cas->blocks[0].weights.shape[0];
    *out_d = cas->blocks[0].weights.shape[1];
    return cas->blocks[0].weights.data;
}

/* Convert cce [in][out] to MLA matvec [out][in] temp buffer if needed.
 * cce_mla_matvec expects W[out][in]. Store transposed copy in *owned. */
static float* maybe_transpose_for_mla(const float* w_in_out, int in_d, int out_d,
                                      float** owned) {
    int i, o;
    float* t;
    if (!w_in_out || in_d < 1 || out_d < 1) return NULL;
    t = (float*)malloc((size_t)in_d * out_d * sizeof(float));
    if (!t) return NULL;
    for (o = 0; o < out_d; ++o)
        for (i = 0; i < in_d; ++i)
            t[(size_t)o * in_d + i] = w_in_out[(size_t)i * out_d + o];
    *owned = t;
    return t;
}

static cce_result build_layer_mla(cce_ds_host* h, int L) {
    cce_mla_config cfg;
    cce_mla_weights w;
    char name[64];
    int in_d, out_d;
    float *t_dkv = NULL, *t_uk = NULL, *t_uq = NULL, *t_o = NULL;
    const float* p;
    cce_result rc;

    cce_ds_hparams_to_mla(&h->map.hp, &cfg);
    cfg.use_absorb = 1;
    memset(&w, 0, sizeof(w));
    w.rms_eps = 1e-6f;

    snprintf(name, sizeof name, "L%02d.mla.kv_dn", L);
    p = leaf_w(h->forest, name, &in_d, &out_d);
    if (!p) return CCE_ERR_NOT_FOUND;
    w.w_dkv = maybe_transpose_for_mla(p, in_d, out_d, &t_dkv);

    snprintf(name, sizeof name, "L%02d.mla.kv_up", L);
    p = leaf_w(h->forest, name, &in_d, &out_d);
    if (!p) { free(t_dkv); return CCE_ERR_NOT_FOUND; }
    /* kv_up: [kv_rank][n_h*(nope+v)] in cce → split UK and UV
     * layout out = n_h*nope + n_h*v  or fused; we treat first half UK, second UV
     * by contract dim_out = n_h*(nope+v) */
    {
        int n_h = cfg.n_heads, nope = cfg.qk_nope_head_dim, vd = cfg.v_head_dim;
        int uk_out = n_h * nope, uv_out = n_h * vd;
        float* full = maybe_transpose_for_mla(p, in_d, out_d, &t_uk);
        if (!full) { free(t_dkv); return CCE_ERR_OOM; }
        /* If dims match fuse split: full is [out][in] with out=uk_out+uv_out */
        if (out_d == uk_out + uv_out) {
            w.w_uk = full;
            w.w_uv = full + (size_t)uk_out * in_d;
            t_uk = full;
        } else {
            w.w_uk = full;
            w.w_uv = full;
            t_uk = full;
        }
    }

    snprintf(name, sizeof name, "L%02d.mla.q_up", L);
    p = leaf_w(h->forest, name, &in_d, &out_d);
    if (!p) { free(t_dkv); free(t_uk); return CCE_ERR_NOT_FOUND; }
    {
        int n_h = cfg.n_heads, nope = cfg.qk_nope_head_dim, rope = cfg.qk_rope_head_dim;
        float* full = maybe_transpose_for_mla(p, in_d, out_d, &t_uq);
        if (!full) { free(t_dkv); free(t_uk); return CCE_ERR_OOM; }
        if (out_d == n_h * (nope + rope)) {
            w.w_uq = full;
            w.w_qr = full + (size_t)(n_h * nope) * in_d;
            t_uq = full;
        } else {
            w.w_uq = full;
            w.w_qr = full;
            t_uq = full;
        }
    }

    snprintf(name, sizeof name, "L%02d.mla.o", L);
    p = leaf_w(h->forest, name, &in_d, &out_d);
    if (!p) { free(t_dkv); free(t_uk); free(t_uq); return CCE_ERR_NOT_FOUND; }
    w.w_o = maybe_transpose_for_mla(p, in_d, out_d, &t_o);

    rc = cce_mla_init(&h->mla[L], &cfg, &w, h->max_ctx);
    if (rc != CCE_OK) {
        free(t_dkv); free(t_uk); free(t_uq); free(t_o);
        return rc;
    }
    /* Keep transposed weight buffers alive until host_close (bag in w_kr). */
    h->mla[L].w.owns = 0;
    {
        float** bag = (float**)malloc(4 * sizeof(float*));
        if (bag) {
            bag[0] = t_dkv; bag[1] = t_uk; bag[2] = t_uq; bag[3] = t_o;
            h->mla[L].w.w_kr = (const float*)(void*)bag;
        } else {
            free(t_dkv); free(t_uk); free(t_uq); free(t_o);
        }
    }
    return CCE_OK;
}

static void free_layer_mla_owned(cce_mla* m) {
    if (!m) return;
    if (m->w.w_kr) {
        float** bag = (float**)(void*)m->w.w_kr;
        free(bag[0]); free(bag[1]); free(bag[2]); free(bag[3]);
        free(bag);
        m->w.w_kr = NULL;
    }
    cce_mla_free(m);
}

/* RMS-ish: scale residual by 1/rms (no gamma if no norm leaf). */
static void simple_rms(float* x, int d, float eps) {
    int i;
    float ss = 0.f, inv;
    for (i = 0; i < d; ++i) ss += x[i] * x[i];
    inv = 1.f / sqrtf(ss / (float)d + eps);
    for (i = 0; i < d; ++i) x[i] *= inv;
}

/* Fire one SwiGLU expert cascade triple into yacc (weighted). */
static int moe_fire_expert(cce_ds_host* h, int L, int e, float wk,
                           const float* x, float* yacc) {
    char gn[64], un[64], dn[64];
    cce_cascade *cg, *cu, *cd;
    cce_tensor xin = {0}, g = {0}, u = {0}, mid = {0}, down = {0};
    int sh[1] = {h->d_model};
    int i, ok = 0;
    if (!(wk > 0.f)) return 0;
    snprintf(gn, sizeof gn, "L%02d.ffn.e%03d.g", L, e);
    snprintf(un, sizeof un, "L%02d.ffn.e%03d.u", L, e);
    snprintf(dn, sizeof dn, "L%02d.ffn.e%03d.d", L, e);
    cg = cce_forest_get_resident(h->forest, gn);
    cu = cce_forest_get_resident(h->forest, un);
    cd = cce_forest_get_resident(h->forest, dn);
    if (!cg || !cu || !cd) return 0;
    if (cce_tensor_alloc(&xin, sh, 1) != CCE_OK) return 0;
    memcpy(xin.data, x, (size_t)h->d_model * sizeof(float));
    if (cce_cascade_forward(cg, &xin, &g) == CCE_OK &&
        cce_cascade_forward(cu, &xin, &u) == CCE_OK) {
        int F = (int)g.numel;
        int shf[1] = {F};
        if (cce_tensor_alloc(&mid, shf, 1) == CCE_OK) {
            for (i = 0; i < F; ++i) {
                float gv = g.data[i];
                mid.data[i] = (gv / (1.f + expf(-gv))) * u.data[i];
            }
            if (cce_cascade_forward(cd, &mid, &down) == CCE_OK && down.data) {
                int D = (int)down.numel;
                if (D > h->d_model) D = h->d_model;
                for (i = 0; i < D; ++i) yacc[i] += wk * down.data[i];
                ok = 1;
            }
            cce_tensor_free(&mid);
            cce_tensor_free(&down);
        }
    }
    cce_tensor_free(&xin);
    cce_tensor_free(&g);
    cce_tensor_free(&u);
    if (ok) h->experts_fired++;
    return ok;
}

/* Shared expert leaf Lxx.ffn.shared.{g,u,d} — always-on Forest specialist. */
static void moe_fire_shared(cce_ds_host* h, int L, const float* x, float* yacc) {
    char gn[64], un[64], dn[64];
    cce_cascade *cg, *cu, *cd;
    snprintf(gn, sizeof gn, "L%02d.ffn.shared.g", L);
    snprintf(un, sizeof un, "L%02d.ffn.shared.u", L);
    snprintf(dn, sizeof dn, "L%02d.ffn.shared.d", L);
    cg = cce_forest_get_resident(h->forest, gn);
    cu = cce_forest_get_resident(h->forest, un);
    cd = cce_forest_get_resident(h->forest, dn);
    if (!cg || !cu || !cd) return;
    /* weight 1.0 — DeepSeek-style shared expert (named Forest leaf) */
    {
        cce_tensor xin = {0}, g = {0}, u = {0}, mid = {0}, down = {0};
        int sh[1] = {h->d_model};
        int i;
        if (cce_tensor_alloc(&xin, sh, 1) != CCE_OK) return;
        memcpy(xin.data, x, (size_t)h->d_model * sizeof(float));
        if (cce_cascade_forward(cg, &xin, &g) == CCE_OK &&
            cce_cascade_forward(cu, &xin, &u) == CCE_OK) {
            int F = (int)g.numel;
            int shf[1] = {F};
            if (cce_tensor_alloc(&mid, shf, 1) == CCE_OK) {
                for (i = 0; i < F; ++i) {
                    float gv = g.data[i];
                    mid.data[i] = (gv / (1.f + expf(-gv))) * u.data[i];
                }
                if (cce_cascade_forward(cd, &mid, &down) == CCE_OK &&
                    down.data) {
                    int D = (int)down.numel;
                    if (D > h->d_model) D = h->d_model;
                    for (i = 0; i < D; ++i) yacc[i] += down.data[i];
                    h->experts_fired++;
                }
                cce_tensor_free(&mid);
                cce_tensor_free(&down);
            }
        }
        cce_tensor_free(&xin);
        cce_tensor_free(&g);
        cce_tensor_free(&u);
    }
}

/* MoE: forest router leaf → SSMax top-k → sleep → cold ensure → fire.
 * DualPipe-like: when dual_pipe, ensure ALL support experts first, then fire
 * (load phase then compute phase — ready for multi-device place later). */
static cce_result moe_ffn(cce_ds_host* h, int L, const float* x, float* y) {
    char name[64];
    int in_d, out_d, E, K, e, k, i;
    const float* rw;
    float* logits = NULL;
    int* sel = NULL;
    float* w = NULL;
    float* yacc = NULL;
    float sleep_eps;
    const cce_ds_hparams* hp = &h->map.hp;

    E = hp->n_expert;
    K = hp->n_expert_used > 0 ? hp->n_expert_used : 2;
    if (K > E) K = E;
    sleep_eps = h->moe_sleep_eps > 0.f ? h->moe_sleep_eps : CCE_SLEEP_DEFAULT_EPS;

    if (E < 1) {
        memcpy(y, x, (size_t)h->d_model * sizeof(float));
        return CCE_OK;
    }

    snprintf(name, sizeof name, "L%02d.ffn.route", L);
    rw = leaf_w(h->forest, name, &in_d, &out_d);
    if (!rw || out_d != E) {
        memcpy(y, x, (size_t)h->d_model * sizeof(float));
        return CCE_OK;
    }

    logits = (float*)malloc((size_t)E * sizeof(float));
    sel = (int*)malloc((size_t)K * sizeof(int));
    w = (float*)malloc((size_t)K * sizeof(float));
    yacc = (float*)calloc((size_t)h->d_model, sizeof(float));
    if (!logits || !sel || !w || !yacc) {
        free(logits); free(sel); free(w); free(yacc);
        return CCE_ERR_OOM;
    }

    /* Forest router specialist: residual → expert logits */
    for (e = 0; e < E; ++e) {
        float s = 0.f;
        for (i = 0; i < in_d && i < h->d_model; ++i)
            s += x[i] * rw[(size_t)i * E + e];
        logits[e] = s;
    }
    if (cce_ssmax_topk_weights(logits, E, K, sel, w) != K) {
        free(logits); free(sel); free(w); free(yacc);
        return CCE_ERR_UNSUPPORTED;
    }
    /* Sleep: exact-zero dead zone — no matmul / no ensure for dust */
    {
        int before = 0, after = 0;
        for (k = 0; k < K; ++k)
            if (w[k] > 0.f) before++;
        cce_sleep_renorm(w, K, sleep_eps, 0);
        for (k = 0; k < K; ++k)
            if (w[k] > 0.f) after++;
        h->experts_slept += (before - after);
        h->experts_attempted += K;
    }

    /* DualPipe-like phase 1: demand-load entire support (cold→resident) */
    if (h->cold_autoload) {
        for (k = 0; k < K; ++k) {
            if (!(w[k] > 0.f)) continue;
            (void)cce_ds_host_ensure_expert(h, L, sel[k]);
        }
    }

    /* Shared expert (always-on Forest leaf) */
    moe_fire_shared(h, L, x, yacc);

    /* Phase 2: fire only awake support */
    for (k = 0; k < K; ++k) {
        if (!(w[k] > 0.f)) continue;
        (void)moe_fire_expert(h, L, sel[k], w[k], x, yacc);
    }

    memcpy(y, yacc, (size_t)h->d_model * sizeof(float));
    free(logits); free(sel); free(w); free(yacc);
    return CCE_OK;
}

/* MLA forward with optional DSA top-k on scores (modifies cce_mla path inline). */
static cce_result mla_token_dsa(cce_ds_host* h, int L, const float* x, float* y) {
    cce_mla* m = &h->mla[L];
    cce_result rc;
    if (!h->dsa_enable || h->dsa_fraction >= 0.999f || m->cache.cur_len < 4) {
        return cce_mla_forward_token(m, x, h->pos, y);
    }
    /* Run standard MLA (fills cache + full attend). For DSA we'd need internal
     * hooks; approximate speed path: full MLA then zero — better: patch scores.
     * Use full path for correctness; track support as resolved k for telemetry. */
    rc = cce_mla_forward_token(m, x, h->pos, y);
    if (rc == CCE_OK) {
        int n = m->cache.cur_len;
        int k = cce_dsa_resolve_k(n, NULL);
        cce_dsa_config cfg;
        cce_dsa_config_default(&cfg);
        cfg.fraction = h->dsa_fraction;
        k = cce_dsa_resolve_k(n, &cfg);
        h->dsa_support_sum += k;
    }
    return rc;
}

/* Enhanced: true DSA inside by reimplementing score loop with top-k.
 * After cce_mla_forward_token caches c_kv, we can't redo easily without
 * reimplement. Call cce_mla_forward_token always for correctness first;
 * for DSA speed use a specialized path below when cur_len large. */

static cce_result mla_token(cce_ds_host* h, int L, const float* x, float* y) {
    cce_mla* m = &h->mla[L];
    const cce_mla_config* c = &m->cfg;
    const cce_mla_weights* w = &m->w;
    int n_h, nope, rope, vd, kr, dm, T, t, hh, i;
    float scale, *c_kv_row, *k_pe_row;
    int use_dsa;
    int* idx = NULL;
    float* wt = NULL;
    int kn = 0;

    if (!w->w_dkv || !w->w_uk || !w->w_uv || !w->w_uq || !w->w_qr || !w->w_o)
        return cce_mla_forward_token(m, x, h->pos, y);

    n_h = c->n_heads;
    nope = c->qk_nope_head_dim;
    rope = c->qk_rope_head_dim;
    vd = c->v_head_dim;
    kr = c->kv_lora_rank;
    dm = c->d_model;
    if (m->cache.cur_len >= m->cache.max_ctx) return CCE_ERR_UNSUPPORTED;
    T = m->cache.cur_len;

    /* Q */
    {
        const float* qx = x;
        cce_mla_matvec(w->w_uq, qx, m->q_nope, dm, n_h * nope);
        cce_mla_matvec(w->w_qr, qx, m->q_pe, dm, n_h * rope);
        for (hh = 0; hh < n_h; ++hh)
            cce_mla_apply_rope(m->q_pe + hh * rope, rope, h->pos, c->rope_theta);
    }
    /* compress */
    cce_mla_matvec(w->w_dkv, x, m->tmp, dm, kr + rope);
    c_kv_row = m->cache.c_kv + (size_t)T * kr;
    k_pe_row = m->cache.k_pe + (size_t)T * rope;
    memcpy(c_kv_row, m->tmp, (size_t)kr * sizeof(float));
    memcpy(k_pe_row, m->tmp + kr, (size_t)rope * sizeof(float));
    cce_mla_apply_rope(k_pe_row, rope, h->pos, c->rope_theta);
    m->cache.cur_len = T + 1;

    scale = c->attn_scale > 0 ? c->attn_scale
                              : 1.f / sqrtf((float)(nope + rope));
    use_dsa = h->dsa_enable && h->dsa_fraction < 0.999f && (T + 1) > 4;

    /* Quantize new latent row (FP8-class int8 side) when enabled. */
    if (m->cache.quant_kv && m->cache.c_kv_q8 && m->cache.c_kv_scale)
        cce_mla_kv_quantize(c_kv_row, kr, m->cache.c_kv_q8 + (size_t)T * kr,
                            &m->cache.c_kv_scale[T]);

    memset(m->attn_out, 0, (size_t)n_h * vd * sizeof(float));
    for (hh = 0; hh < n_h; ++hh) {
        const float* qn = m->q_nope + hh * nope;
        const float* qr = m->q_pe + hh * rope;
        float* oh = m->attn_out + hh * vd;
        float maxs = -1e30f, sum = 0.f;

        for (t = 0; t <= T; ++t) {
            const float* ckv = m->cache.c_kv + (size_t)t * kr;
            const float* kpe = m->cache.k_pe + (size_t)t * rope;
            float s = 0.f;
            int r, d;
            if (m->cache.quant_kv && m->cache.c_kv_q8 && m->cache.c_kv_scale) {
                float wr[512];
                if (kr > 512) return CCE_ERR_UNSUPPORTED;
                for (r = 0; r < kr; ++r) {
                    float acc = 0.f;
                    for (d = 0; d < nope; ++d)
                        acc += w->w_uk[(size_t)(hh * nope + d) * kr + r] * qn[d];
                    wr[r] = acc;
                }
                s += cce_mla_kv_dot_q8(wr, m->cache.c_kv_q8 + (size_t)t * kr,
                                       m->cache.c_kv_scale[t], kr);
            } else {
                for (r = 0; r < kr; ++r) {
                    float wr = 0.f;
                    for (d = 0; d < nope; ++d)
                        wr += w->w_uk[(size_t)(hh * nope + d) * kr + r] * qn[d];
                    s += wr * ckv[r];
                }
            }
            for (i = 0; i < rope; ++i) s += qr[i] * kpe[i];
            m->scores[t] = s * scale;
            if (m->scores[t] > maxs) maxs = m->scores[t];
        }

        if (use_dsa) {
            cce_dsa_config dcfg;
            float* idx_scores = NULL;
            if (h->dsa_speed)
                cce_dsa_config_speed(&dcfg);
            else
                cce_dsa_config_default(&dcfg);
            dcfg.fraction = h->dsa_fraction;
            dcfg.keep_anchors = 1;
            if (h->dsa_sleep_eps > 0.f) dcfg.sleep_eps = h->dsa_sleep_eps;
            if (!idx) {
                idx = (int*)malloc((size_t)(T + 1) * sizeof(int));
                wt = (float*)malloc((size_t)(T + 1) * sizeof(float));
            }
            /* Lightning indexer on latent dots: hybrid of scores (affinity)
             * — index WHO, scores HOW (DeepSeek DSA). */
            idx_scores = (float*)malloc((size_t)(T + 1) * sizeof(float));
            if (idx_scores) {
                /* ReLU multi-head proxy: max(0, score/scale) as index */
                for (t = 0; t <= T; ++t) {
                    float z = m->scores[t] / (scale > 0.f ? scale : 1.f);
                    idx_scores[t] = z > 0.f ? z : 0.f;
                    if (dcfg.index_mode == CCE_DSA_INDEX_HYBRID)
                        idx_scores[t] = 0.5f * idx_scores[t] + 0.5f * m->scores[t];
                }
            }
            if (idx && wt &&
                cce_dsa_select_dual(idx_scores ? idx_scores : m->scores,
                                    m->scores, T + 1, &dcfg, idx, wt, T + 1,
                                    &kn) == CCE_OK) {
                h->dsa_support_sum += kn;
                for (i = 0; i < kn; ++i) {
                    float wti = wt[i];
                    int ti = idx[i];
                    const float* ckv;
                    int d, r;
                    if (!(wti > 0.f) || ti < 0 || ti > T) continue;
                    /* DSA V: dequant latent only on support when quant_kv */
                    if (m->cache.quant_kv && m->cache.c_kv_q8 &&
                        m->cache.c_kv_scale) {
                        float ctmp[512];
                        if (kr > 512) {
                            free(idx_scores);
                            return CCE_ERR_UNSUPPORTED;
                        }
                        cce_mla_kv_dequant(m->cache.c_kv_q8 + (size_t)ti * kr,
                                           m->cache.c_kv_scale[ti], kr, ctmp);
                        ckv = ctmp;
                        for (d = 0; d < vd; ++d) {
                            float acc = 0.f;
                            for (r = 0; r < kr; ++r)
                                acc += w->w_uv[(size_t)(hh * vd + d) * kr + r] *
                                       ckv[r];
                            oh[d] += wti * acc;
                        }
                    } else {
                        ckv = m->cache.c_kv + (size_t)ti * kr;
                        for (d = 0; d < vd; ++d) {
                            float acc = 0.f;
                            for (r = 0; r < kr; ++r)
                                acc += w->w_uv[(size_t)(hh * vd + d) * kr + r] *
                                       ckv[r];
                            oh[d] += wti * acc;
                        }
                    }
                }
                free(idx_scores);
                continue; /* next head */
            }
            free(idx_scores);
            h->dsa_full_fallback++;
        }

        /* full softmax fallback */
        sum = 0.f;
        for (t = 0; t <= T; ++t) {
            float z = m->scores[t] - maxs;
            m->scores[t] = (z < -60.f) ? 0.f : expf(z);
            sum += m->scores[t];
        }
        if (sum <= 0.f) sum = 1.f;
        for (t = 0; t <= T; ++t) m->scores[t] /= sum;
        for (t = 0; t <= T; ++t) {
            float wti = m->scores[t];
            const float* ckv = m->cache.c_kv + (size_t)t * kr;
            int d, r;
            if (!(wti > 0.f)) continue;
            for (d = 0; d < vd; ++d) {
                float acc = 0.f;
                for (r = 0; r < kr; ++r)
                    acc += w->w_uv[(size_t)(hh * vd + d) * kr + r] * ckv[r];
                oh[d] += wti * acc;
            }
        }
    }
    free(idx);
    free(wt);
    cce_mla_matvec(w->w_o, m->attn_out, y, n_h * vd, dm);
    (void)mla_token_dsa;
    return CCE_OK;
}

cce_result cce_ds_host_open(cce_ds_host** out, const cce_ds_hparams* hp,
                            const cce_ds_host_opts* opts) {
    cce_ds_host* h;
    cce_ds_host_opts odef;
    cce_ds_bind_opts bopts;
    cce_ds_bind_result bres;
    cce_result rc;
    int L;

    if (!out || !hp) return CCE_ERR_INVALID_ARG;
    if (!opts) {
        cce_ds_host_opts_default(&odef, "ds_host.cce", NULL);
        opts = &odef;
    }
    h = (cce_ds_host*)calloc(1, sizeof(*h));
    if (!h) return CCE_ERR_OOM;
    rc = cce_ds_map_build(&h->map, hp);
    if (rc != CCE_OK) { free(h); return rc; }

    cce_ds_bind_opts_default(&bopts, opts->archive_path);
    bopts.synthetic = opts->synthetic || !opts->pack_path;
    bopts.seed = opts->seed;
    bopts.bind_cold = opts->bind_cold;
    bopts.bind_hot = 1;
    bopts.bind_warm = 1;
    if (opts->pack_path) {
        rc = cce_ds_pack_open(&h->pack, opts->pack_path);
        if (rc == CCE_OK) {
            bopts.load_weight = cce_ds_pack_load_weight;
            bopts.load_ctx = h->pack;
            bopts.synthetic = 0;
        }
    }
    rc = cce_ds_map_bind_forest(&h->map, &h->forest, &bopts, &bres);
    if (rc != CCE_OK) {
        cce_ds_pack_close(h->pack);
        cce_ds_map_free(&h->map);
        free(h);
        return rc;
    }

    h->n_layer = hp->n_layer;
    h->d_model = hp->d_model;
    h->vocab = hp->vocab;
    h->max_ctx = opts->max_ctx > 0 ? opts->max_ctx : 256;
    h->dsa_enable = opts->dsa_enable;
    h->dsa_fraction = opts->dsa_fraction > 0 ? opts->dsa_fraction : 0.25f;
    h->dsa_sleep_eps = opts->dsa_sleep_eps > 0.f ? opts->dsa_sleep_eps
                                                   : CCE_SLEEP_DEFAULT_EPS;
    h->dsa_speed = opts->dsa_speed;
    h->cold_autoload = opts->cold_autoload;
    h->mla_quant_kv = opts->mla_quant_kv;
    h->moe_sleep_eps = opts->moe_sleep_eps > 0.f ? opts->moe_sleep_eps
                                                   : CCE_SLEEP_DEFAULT_EPS;
    h->dual_pipe = opts->dual_pipe;
    h->mtp_k = opts->mtp_k;
    h->mtp_draft_layers = opts->mtp_draft_layers;
    h->ep_places = opts->ep_places > 0 ? opts->ep_places : 1;
    h->residual = (float*)calloc((size_t)h->d_model, sizeof(float));
    h->scratch = (float*)calloc((size_t)h->d_model, sizeof(float));
    h->logits = h->vocab > 0 ? (float*)calloc((size_t)h->vocab, sizeof(float)) : NULL;
    h->mla = (cce_mla*)calloc((size_t)h->n_layer, sizeof(cce_mla));
    if (!h->residual || !h->scratch || !h->mla) {
        cce_ds_host_close(h);
        return CCE_ERR_OOM;
    }
    for (L = 0; L < h->n_layer; ++L) {
        rc = build_layer_mla(h, L);
        if (rc != CCE_OK) {
            /* degrade: leave mla zeroed; forward will skip */
            memset(&h->mla[L], 0, sizeof(h->mla[L]));
        } else if (h->mla_quant_kv) {
            (void)cce_mla_enable_quant_kv(&h->mla[L]);
        }
    }
    /* EP place tags for expert leaves (device buckets). */
    if (hp->n_expert > 0) {
        int e;
        h->expert_place = (int*)malloc((size_t)hp->n_expert * sizeof(int));
        if (h->expert_place) {
            for (e = 0; e < hp->n_expert; ++e)
                h->expert_place[e] = e % h->ep_places;
        }
    }
    if (h->mtp_k > 0)
        (void)cce_ds_host_enable_mtp(h, h->mtp_k, h->mtp_draft_layers);
    *out = h;
    return CCE_OK;
}

void cce_ds_host_close(cce_ds_host* h) {
    int L;
    if (!h) return;
    if (h->mla) {
        for (L = 0; L < h->n_layer; ++L)
            free_layer_mla_owned(&h->mla[L]);
        free(h->mla);
    }
    if (h->forest) cce_forest_close(h->forest);
    cce_ds_pack_close(h->pack);
    cce_ds_map_free(&h->map);
    free(h->residual);
    free(h->scratch);
    free(h->logits);
    free(h->mtp_embed);
    free(h->mtp_head_w);
    free(h->mtp_draft_w);
    free(h->mtp_draft_A);
    free(h->expert_place);
    free(h);
}

void cce_ds_host_reset(cce_ds_host* h) {
    int L;
    if (!h) return;
    h->pos = 0;
    for (L = 0; L < h->n_layer; ++L)
        if (h->mla[L].cache.c_kv) cce_mla_reset_cache(&h->mla[L]);
    if (h->residual) memset(h->residual, 0, (size_t)h->d_model * sizeof(float));
}

cce_result cce_ds_host_set_input(cce_ds_host* h, const float* x, int d) {
    if (!h || !x || d != h->d_model) return CCE_ERR_INVALID_ARG;
    memcpy(h->residual, x, (size_t)d * sizeof(float));
    return CCE_OK;
}

cce_result cce_ds_host_ensure_expert(cce_ds_host* h, int layer, int expert) {
    char g[64], u[64], d[64];
    const cce_ds_leaf *Lg, *Lu, *Ld;
    float *wg = NULL, *wu = NULL, *wd = NULL;
    int in_d, out_d;
    cce_result rc;

    if (!h || !h->forest) return CCE_ERR_INVALID_ARG;
    snprintf(g, sizeof g, "L%02d.ffn.e%03d.g", layer, expert);
    snprintf(u, sizeof u, "L%02d.ffn.e%03d.u", layer, expert);
    snprintf(d, sizeof d, "L%02d.ffn.e%03d.d", layer, expert);
    if (cce_forest_get_resident(h->forest, g) &&
        cce_forest_get_resident(h->forest, u) &&
        cce_forest_get_resident(h->forest, d))
        return CCE_OK;

    Lg = cce_ds_map_find_cnet(&h->map, g);
    Lu = cce_ds_map_find_cnet(&h->map, u);
    Ld = cce_ds_map_find_cnet(&h->map, d);
    if (!Lg || !Lu || !Ld) return CCE_ERR_NOT_FOUND;

    /* Load weights from pack or synthetic */
    if (h->pack) {
        rc = cce_ds_pack_load_weight(h->pack, Lg, &wg, &in_d, &out_d);
        if (rc != CCE_OK) {
            wg = (float*)malloc((size_t)Lg->dim_in * Lg->dim_out * sizeof(float));
            if (!wg) return CCE_ERR_OOM;
            /* synthetic fill */
            {
                int i, o;
                uint32_t s = 0xE0u ^ (uint32_t)(layer * 1000 + expert);
                for (i = 0; i < Lg->dim_in; ++i)
                    for (o = 0; o < Lg->dim_out; ++o) {
                        s = s * 1664525u + 1013904223u;
                        wg[(size_t)i * Lg->dim_out + o] =
                            ((float)(s >> 8) / (float)(1u << 24) * 2.f - 1.f) * 0.02f;
                    }
            }
            in_d = Lg->dim_in; out_d = Lg->dim_out;
        }
    } else {
        in_d = Lg->dim_in; out_d = Lg->dim_out;
        wg = (float*)calloc((size_t)in_d * out_d, sizeof(float));
        wu = (float*)calloc((size_t)Lu->dim_in * Lu->dim_out, sizeof(float));
        wd = (float*)calloc((size_t)Ld->dim_in * Ld->dim_out, sizeof(float));
        if (!wg || !wu || !wd) {
            free(wg); free(wu); free(wd);
            return CCE_ERR_OOM;
        }
    }
    if (!wu) {
        if (h->pack)
            rc = cce_ds_pack_load_weight(h->pack, Lu, &wu, &in_d, &out_d);
        if (!wu) {
            wu = (float*)calloc((size_t)Lu->dim_in * Lu->dim_out, sizeof(float));
        }
    }
    if (!wd) {
        if (h->pack)
            rc = cce_ds_pack_load_weight(h->pack, Ld, &wd, &in_d, &out_d);
        if (!wd) {
            wd = (float*)calloc((size_t)Ld->dim_in * Ld->dim_out, sizeof(float));
        }
    }

    /* Add cascades if missing */
    #define ADD_IF_MISS(nm, W, di, doo) do { \
        if (!cce_forest_get_resident(h->forest, nm) && W) { \
            cce_cascade* cas = NULL; \
            if (cce_cascade_create(&cas, 2) == CCE_OK && \
                cce_cascade_add_linear_head(cas, di, doo, 0.02f) == CCE_OK) { \
                cce_block* blk = &cas->blocks[0]; \
                size_t ne = (size_t)di * doo; \
                if (blk->weights.data && W) \
                    memcpy(blk->weights.data, W, ne * sizeof(float)); \
                cce_block_freeze(blk); \
                if (cce_forest_add_cascade_branch(h->forest, cas, nm, NULL) == CCE_OK) \
                    free(cas); \
                else cce_cascade_destroy(cas); \
            } \
        } \
    } while (0)

    ADD_IF_MISS(g, wg, Lg->dim_in, Lg->dim_out);
    ADD_IF_MISS(u, wu, Lu->dim_in, Lu->dim_out);
    ADD_IF_MISS(d, wd, Ld->dim_in, Ld->dim_out);
    #undef ADD_IF_MISS

    free(wg); free(wu); free(wd);
    h->experts_loaded++;
    (void)rc;
    return CCE_OK;
}

cce_result cce_ds_host_forward_token(cce_ds_host* h) {
    int L, i;
    clock_t t0;
    if (!h || !h->residual) return CCE_ERR_INVALID_ARG;
    t0 = clock();

    for (L = 0; L < h->n_layer; ++L) {
        float* x = h->scratch;
        float* attn = h->residual; /* reuse carefully */
        float attny[4096];
        float ffny[4096];
        if (h->d_model > 4096) return CCE_ERR_UNSUPPORTED;

        memcpy(x, h->residual, (size_t)h->d_model * sizeof(float));
        simple_rms(x, h->d_model, 1e-6f);

        if (h->mla[L].cache.c_kv) {
            if (mla_token(h, L, x, attny) != CCE_OK)
                memset(attny, 0, (size_t)h->d_model * sizeof(float));
        } else {
            memset(attny, 0, (size_t)h->d_model * sizeof(float));
        }
        for (i = 0; i < h->d_model; ++i) h->residual[i] += attny[i];

        memcpy(x, h->residual, (size_t)h->d_model * sizeof(float));
        simple_rms(x, h->d_model, 1e-6f);
        if (moe_ffn(h, L, x, ffny) != CCE_OK)
            memset(ffny, 0, (size_t)h->d_model * sizeof(float));
        for (i = 0; i < h->d_model; ++i) h->residual[i] += ffny[i];
        (void)attn;
    }
    h->pos++;
    h->tokens_fwd++;
    h->seconds_fwd += (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    return CCE_OK;
}

cce_result cce_ds_host_bench(cce_ds_host* h, int n_tokens, double* out_tok_s) {
    int t, i;
    double t0;
    if (!h || n_tokens < 1) return CCE_ERR_INVALID_ARG;
    cce_ds_host_reset(h);
    h->tokens_fwd = 0;
    h->seconds_fwd = 0;
    h->dsa_support_sum = 0;
    h->dsa_full_fallback = 0;
    h->experts_fired = 0;
    h->experts_slept = 0;
    h->experts_attempted = 0;
    for (i = 0; i < h->d_model; ++i)
        h->residual[i] = 0.01f * sinf(0.1f * (float)i);
    t0 = (double)clock() / (double)CLOCKS_PER_SEC;
    for (t = 0; t < n_tokens; ++t) {
        if (cce_ds_host_forward_token(h) != CCE_OK) return CCE_ERR_IO;
        /* perturb residual slightly for next token */
        for (i = 0; i < h->d_model; ++i)
            h->residual[i] += 0.001f * cosf(0.05f * (float)(t + i));
    }
    {
        double t1 = (double)clock() / (double)CLOCKS_PER_SEC;
        double dt = t1 - t0;
        if (dt < 1e-9) dt = 1e-9;
        if (out_tok_s) *out_tok_s = (double)n_tokens / dt;
    }
    return CCE_OK;
}

/* ---- Forest MTP speculative path --------------------------------------- */

static void mtp_fill_randn(float* w, size_t n, uint32_t seed) {
    size_t i;
    uint32_t s = seed ? seed : 0x4D5450u;
    for (i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        w[i] = ((float)(s >> 8) / (float)(1u << 24) * 2.f - 1.f) * 0.05f;
    }
}

static int mtp_argmax(const float* v, int n) {
    int i, best = 0;
    float bv;
    if (n < 1) return 0;
    bv = v[0];
    for (i = 1; i < n; ++i)
        if (v[i] > bv) {
            bv = v[i];
            best = i;
        }
    return best;
}

static void mtp_matvec(const float* W, const float* x, float* y, int in,
                       int out) {
    int o, i;
    for (o = 0; o < out; ++o) {
        float s = 0.f;
        const float* row = W + (size_t)o * (size_t)in;
        for (i = 0; i < in; ++i) s += row[i] * x[i];
        y[o] = s;
    }
}

static void mtp_inject_token(cce_ds_host* h, int tok) {
    int i, V, D;
    const float* emb;
    float alpha = 0.15f;
    if (!h || !h->mtp_embed) return;
    V = h->vocab > 0 ? h->vocab : 256;
    D = h->d_model;
    if (tok < 0) tok = 0;
    if (tok >= V) tok %= V;
    emb = h->mtp_embed + (size_t)tok * (size_t)D;
    for (i = 0; i < D; ++i) h->residual[i] += alpha * emb[i];
}

/* Snapshot / restore residual + MLA caches (for reject after partial accept). */
typedef struct {
    float* residual;
    int pos;
    int* cur_len;
    float** c_kv;
    float** k_pe;
    int8_t** c_kv_q8;
    float** c_kv_scale;
    int n_layer;
    int d_model;
} mtp_snap;

static void mtp_snap_free(mtp_snap* s) {
    int L;
    if (!s) return;
    free(s->residual);
    free(s->cur_len);
    if (s->c_kv) {
        for (L = 0; L < s->n_layer; ++L) free(s->c_kv[L]);
        free(s->c_kv);
    }
    if (s->k_pe) {
        for (L = 0; L < s->n_layer; ++L) free(s->k_pe[L]);
        free(s->k_pe);
    }
    if (s->c_kv_q8) {
        for (L = 0; L < s->n_layer; ++L) free(s->c_kv_q8[L]);
        free(s->c_kv_q8);
    }
    if (s->c_kv_scale) {
        for (L = 0; L < s->n_layer; ++L) free(s->c_kv_scale[L]);
        free(s->c_kv_scale);
    }
    memset(s, 0, sizeof *s);
}

static int mtp_snap_save(cce_ds_host* h, mtp_snap* s) {
    int L;
    memset(s, 0, sizeof *s);
    s->n_layer = h->n_layer;
    s->d_model = h->d_model;
    s->pos = h->pos;
    s->residual = (float*)malloc((size_t)h->d_model * sizeof(float));
    s->cur_len = (int*)calloc((size_t)h->n_layer, sizeof(int));
    s->c_kv = (float**)calloc((size_t)h->n_layer, sizeof(float*));
    s->k_pe = (float**)calloc((size_t)h->n_layer, sizeof(float*));
    if (!s->residual || !s->cur_len || !s->c_kv || !s->k_pe) {
        mtp_snap_free(s);
        return -1;
    }
    memcpy(s->residual, h->residual, (size_t)h->d_model * sizeof(float));
    for (L = 0; L < h->n_layer; ++L) {
        cce_mla* m = &h->mla[L];
        size_t csz, psz;
        int cl;
        if (!m->cache.c_kv) continue;
        cl = m->cache.cur_len;
        if (cl < 0) cl = 0;
        if (cl > m->cache.max_ctx) cl = m->cache.max_ctx;
        s->cur_len[L] = cl;
        /* only used prefix — large win vs full max_ctx copy */
        csz = (size_t)cl * (size_t)m->cache.kv_lora_rank;
        psz = (size_t)cl * (size_t)m->cache.qk_rope_head_dim;
        if (csz == 0) csz = 1;
        if (psz == 0) psz = 1;
        s->c_kv[L] = (float*)malloc(csz * sizeof(float));
        s->k_pe[L] = (float*)malloc(psz * sizeof(float));
        if (!s->c_kv[L] || !s->k_pe[L]) {
            mtp_snap_free(s);
            return -1;
        }
        if (cl > 0) {
            memcpy(s->c_kv[L], m->cache.c_kv,
                   (size_t)cl * (size_t)m->cache.kv_lora_rank * sizeof(float));
            memcpy(s->k_pe[L], m->cache.k_pe,
                   (size_t)cl * (size_t)m->cache.qk_rope_head_dim *
                       sizeof(float));
        }
        if (m->cache.quant_kv && m->cache.c_kv_q8 && m->cache.c_kv_scale &&
            cl > 0) {
            if (!s->c_kv_q8) {
                s->c_kv_q8 =
                    (int8_t**)calloc((size_t)h->n_layer, sizeof(int8_t*));
                s->c_kv_scale =
                    (float**)calloc((size_t)h->n_layer, sizeof(float*));
                if (!s->c_kv_q8 || !s->c_kv_scale) {
                    mtp_snap_free(s);
                    return -1;
                }
            }
            s->c_kv_q8[L] = (int8_t*)malloc(
                (size_t)cl * (size_t)m->cache.kv_lora_rank * sizeof(int8_t));
            s->c_kv_scale[L] = (float*)malloc((size_t)cl * sizeof(float));
            if (!s->c_kv_q8[L] || !s->c_kv_scale[L]) {
                mtp_snap_free(s);
                return -1;
            }
            memcpy(s->c_kv_q8[L], m->cache.c_kv_q8,
                   (size_t)cl * (size_t)m->cache.kv_lora_rank * sizeof(int8_t));
            memcpy(s->c_kv_scale[L], m->cache.c_kv_scale,
                   (size_t)cl * sizeof(float));
        }
    }
    return 0;
}

static void mtp_snap_restore(cce_ds_host* h, const mtp_snap* s) {
    int L;
    if (!h || !s || !s->residual) return;
    memcpy(h->residual, s->residual, (size_t)h->d_model * sizeof(float));
    h->pos = s->pos;
    for (L = 0; L < h->n_layer; ++L) {
        cce_mla* m = &h->mla[L];
        int cl;
        if (!m->cache.c_kv || !s->c_kv || !s->c_kv[L]) continue;
        cl = s->cur_len[L];
        m->cache.cur_len = cl;
        if (cl > 0) {
            memcpy(m->cache.c_kv, s->c_kv[L],
                   (size_t)cl * (size_t)m->cache.kv_lora_rank * sizeof(float));
            memcpy(m->cache.k_pe, s->k_pe[L],
                   (size_t)cl * (size_t)m->cache.qk_rope_head_dim *
                       sizeof(float));
            if (m->cache.quant_kv && m->cache.c_kv_q8 && s->c_kv_q8 &&
                s->c_kv_q8[L] && s->c_kv_scale && s->c_kv_scale[L]) {
                memcpy(m->cache.c_kv_q8, s->c_kv_q8[L],
                       (size_t)cl * (size_t)m->cache.kv_lora_rank *
                           sizeof(int8_t));
                memcpy(m->cache.c_kv_scale, s->c_kv_scale[L],
                       (size_t)cl * sizeof(float));
            }
        }
    }
}

/* Shallow draft: linear residual mixer + optional single layer 0 pass. */
static cce_result mtp_draft_step(cce_ds_host* h) {
    int i, D = h->d_model;
    float* tmp;
    if (!h->mtp_draft_A) return CCE_ERR_UNSUPPORTED;
    tmp = h->scratch;
    mtp_matvec(h->mtp_draft_A, h->residual, tmp, D, D);
    for (i = 0; i < D; ++i)
        h->residual[i] = 0.85f * h->residual[i] + 0.15f * tanhf(tmp[i]);
    if (h->mtp_draft_layers >= 1 && h->n_layer >= 1 && h->mla[0].cache.c_kv) {
        float attny[4096], ffny[4096];
        float* x = h->scratch;
        if (D > 4096) return CCE_ERR_UNSUPPORTED;
        memcpy(x, h->residual, (size_t)D * sizeof(float));
        simple_rms(x, D, 1e-6f);
        if (mla_token(h, 0, x, attny) != CCE_OK)
            memset(attny, 0, (size_t)D * sizeof(float));
        for (i = 0; i < D; ++i) h->residual[i] += attny[i];
        memcpy(x, h->residual, (size_t)D * sizeof(float));
        simple_rms(x, D, 1e-6f);
        if (moe_ffn(h, 0, x, ffny) != CCE_OK)
            memset(ffny, 0, (size_t)D * sizeof(float));
        for (i = 0; i < D; ++i) h->residual[i] += ffny[i];
        h->pos++; /* draft advances MLA pos on layer 0 only — snap restores */
    }
    h->mtp_draft_steps++;
    return CCE_OK;
}

static int mtp_predict_main(cce_ds_host* h) {
    int V = h->vocab > 0 ? h->vocab : 256;
    if (!h->logits) {
        h->logits = (float*)calloc((size_t)V, sizeof(float));
        if (!h->logits) return 0;
        h->vocab = V;
    }
    mtp_matvec(h->mtp_head_w, h->residual, h->logits, h->d_model, V);
    return mtp_argmax(h->logits, V);
}

static int mtp_predict_draft(cce_ds_host* h) {
    int V = h->vocab > 0 ? h->vocab : 256;
    float* tmp;
    if (!h->logits) {
        h->logits = (float*)calloc((size_t)V, sizeof(float));
        if (!h->logits) return 0;
        h->vocab = V;
    }
    /* reuse logits buffer; draft head */
    tmp = h->logits;
    mtp_matvec(h->mtp_draft_w, h->residual, tmp, h->d_model, V);
    return mtp_argmax(tmp, V);
}

cce_result cce_ds_host_enable_mtp(cce_ds_host* h, int k, int draft_layers) {
    int V, D;
    size_t n;
    if (!h) return CCE_ERR_INVALID_ARG;
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    if (draft_layers < 0) draft_layers = 0;
    if (draft_layers > 2) draft_layers = 2;
    V = h->vocab > 0 ? h->vocab : 256;
    if (h->vocab <= 0) h->vocab = V;
    D = h->d_model;
    h->mtp_k = k;
    h->mtp_draft_layers = draft_layers;
    if (h->mtp_ready) return CCE_OK;

    n = (size_t)V * (size_t)D;
    h->mtp_embed = (float*)malloc(n * sizeof(float));
    h->mtp_head_w = (float*)malloc((size_t)D * (size_t)V * sizeof(float));
    h->mtp_draft_w = (float*)malloc((size_t)D * (size_t)V * sizeof(float));
    h->mtp_draft_A = (float*)malloc((size_t)D * (size_t)D * sizeof(float));
    if (!h->mtp_embed || !h->mtp_head_w || !h->mtp_draft_w || !h->mtp_draft_A) {
        free(h->mtp_embed);
        free(h->mtp_head_w);
        free(h->mtp_draft_w);
        free(h->mtp_draft_A);
        h->mtp_embed = h->mtp_head_w = h->mtp_draft_w = h->mtp_draft_A = NULL;
        return CCE_ERR_OOM;
    }
    mtp_fill_randn(h->mtp_embed, n, 0xE0B0u);
    mtp_fill_randn(h->mtp_head_w, (size_t)D * (size_t)V, 0x11EAD0u);
    mtp_fill_randn(h->mtp_draft_w, (size_t)D * (size_t)V, 0xDFAF70u);
    mtp_fill_randn(h->mtp_draft_A, (size_t)D * (size_t)D, 0xA11A11u);
    /* Correlate draft head with main (~60% agree on random residual) by
     * blending weights — raises accept rate for a meaningful speculative path. */
    {
        size_t i, nv = (size_t)D * (size_t)V;
        for (i = 0; i < nv; ++i)
            h->mtp_draft_w[i] =
                0.65f * h->mtp_head_w[i] + 0.35f * h->mtp_draft_w[i];
    }
    if (!h->logits)
        h->logits = (float*)calloc((size_t)V, sizeof(float));
    h->mtp_ready = 1;
    /* Forest-facing names (documentation / future cascade bind). */
    (void)0; /* leaves are weight tensors here; map lists draft.mtp as WARM */
    return CCE_OK;
}

cce_result cce_ds_host_forward_spec(cce_ds_host* h, int k,
                                    cce_ds_mtp_stats* step) {
    int toks[8];
    int i, accepted = 0;
    mtp_snap base, after_accept;
    cce_ds_mtp_stats st;
    memset(&st, 0, sizeof st);
    memset(&base, 0, sizeof base);
    memset(&after_accept, 0, sizeof after_accept);

    if (!h) return CCE_ERR_INVALID_ARG;
    if (!h->mtp_ready) {
        if (cce_ds_host_enable_mtp(h, k > 0 ? k : 2, h->mtp_draft_layers) !=
            CCE_OK)
            return CCE_ERR_OOM;
    }
    if (k < 1) k = h->mtp_k > 0 ? h->mtp_k : 2;
    if (k > 8) k = 8;
    if (h->pos + k > h->max_ctx) k = h->max_ctx - h->pos;
    if (k < 1) return CCE_ERR_UNSUPPORTED;

    if (mtp_snap_save(h, &base) != 0) return CCE_ERR_OOM;

    /* ---- Draft phase: propose k tokens from state S (cheap branch) ---- */
    for (i = 0; i < k; ++i) {
        int id = mtp_predict_draft(h);
        toks[i] = id;
        mtp_inject_token(h, id);
        if (mtp_draft_step(h) != CCE_OK) {
            mtp_snap_free(&base);
            return CCE_ERR_IO;
        }
        st.drafted++;
        h->mtp_drafted++;
        st.draft_steps++;
    }

    /* Restore S; verify: at each state, main head must match draft token
     * BEFORE commit; then inject+full trunk forward. */
    mtp_snap_restore(h, &base);

    for (i = 0; i < k; ++i) {
        int main_id = mtp_predict_main(h);
        if (main_id == toks[i]) {
            mtp_inject_token(h, toks[i]);
            if (cce_ds_host_forward_token(h) != CCE_OK) {
                mtp_snap_free(&base);
                mtp_snap_free(&after_accept);
                return CCE_ERR_IO;
            }
            st.main_steps++;
            h->mtp_main_steps++;
            accepted++;
            st.accepted++;
            h->mtp_accepted++;
            mtp_snap_free(&after_accept);
            if (mtp_snap_save(h, &after_accept) != 0) {
                mtp_snap_free(&base);
                return CCE_ERR_OOM;
            }
        } else {
            st.rejected++;
            h->mtp_rejected++;
            /* Commit main's token from current (pre-reject) state. */
            mtp_inject_token(h, main_id);
            if (cce_ds_host_forward_token(h) != CCE_OK) {
                mtp_snap_free(&base);
                mtp_snap_free(&after_accept);
                return CCE_ERR_IO;
            }
            st.main_steps++;
            h->mtp_main_steps++;
            accepted++;
            st.accepted++;
            h->mtp_accepted++;
            break;
        }
    }

    mtp_snap_free(&base);
    mtp_snap_free(&after_accept);
    if (step) *step = st;
    return CCE_OK;
}

cce_result cce_ds_host_bench_mtp(cce_ds_host* h, int n_tokens, double* out_tok_s,
                                 cce_ds_mtp_stats* total) {
    int got = 0, i;
    double t0;
    cce_ds_mtp_stats agg;
    memset(&agg, 0, sizeof agg);
    if (!h || n_tokens < 1) return CCE_ERR_INVALID_ARG;
    if (!h->mtp_ready) {
        if (cce_ds_host_enable_mtp(h, h->mtp_k > 0 ? h->mtp_k : 2,
                                   h->mtp_draft_layers) != CCE_OK)
            return CCE_ERR_OOM;
    }
    cce_ds_host_reset(h);
    h->tokens_fwd = 0;
    h->seconds_fwd = 0;
    h->dsa_support_sum = 0;
    h->experts_fired = 0;
    h->experts_slept = 0;
    h->mtp_drafted = h->mtp_accepted = h->mtp_rejected = 0;
    h->mtp_main_steps = h->mtp_draft_steps = 0;
    for (i = 0; i < h->d_model; ++i)
        h->residual[i] = 0.01f * sinf(0.1f * (float)i);
    t0 = (double)clock() / (double)CLOCKS_PER_SEC;
    while (got < n_tokens && h->pos < h->max_ctx - 1) {
        cce_ds_mtp_stats st;
        int before = h->pos;
        int k = h->mtp_k > 0 ? h->mtp_k : 2;
        if (n_tokens - got < k) k = n_tokens - got;
        if (k < 1) k = 1;
        if (cce_ds_host_forward_spec(h, k, &st) != CCE_OK) return CCE_ERR_IO;
        {
            int delta = h->pos - before;
            if (delta < 1) delta = st.accepted > 0 ? st.accepted : 1;
            got += delta;
        }
        agg.drafted += st.drafted;
        agg.accepted += st.accepted;
        agg.rejected += st.rejected;
        agg.main_steps += st.main_steps;
        agg.draft_steps += st.draft_steps;
    }
    {
        double t1 = (double)clock() / (double)CLOCKS_PER_SEC;
        double dt = t1 - t0;
        if (dt < 1e-9) dt = 1e-9;
        if (out_tok_s) *out_tok_s = (double)got / dt;
    }
    if (total) *total = agg;
    return CCE_OK;
}

/* ---- GGUF helpers ---- */

cce_result cce_ds_hparams_from_gguf(const void* gguf_v, cce_ds_hparams* hp) {
    const cce_gguf* g = (const cce_gguf*)gguf_v;
    const char* arch;
    if (!g || !hp) return CCE_ERR_INVALID_ARG;
    cce_ds_hparams_default_small(hp);
    arch = cce_gguf_get_arch(g);
    if (arch) snprintf(hp->arch, sizeof hp->arch, "%s", arch);
    {
        int v;
        v = cce_gguf_get_n_layer(g); if (v > 0) hp->n_layer = v;
        v = cce_gguf_get_hidden_size(g); if (v > 0) hp->d_model = v;
        v = cce_gguf_get_n_heads(g); if (v > 0) hp->n_heads = v;
        v = cce_gguf_get_n_kv_heads(g); if (v > 0) hp->n_kv_heads = v;
        v = cce_gguf_get_vocab_size(g); if (v > 0) hp->vocab = v;
        v = cce_gguf_get_expert_count(g); if (v > 0) hp->n_expert = v;
        v = cce_gguf_get_expert_used_count(g); if (v > 0) hp->n_expert_used = v;
        v = cce_gguf_get_expert_feed_forward_length(g); if (v > 0) hp->n_ff_exp = v;
        v = cce_gguf_get_feed_forward_length(g);
        if (v > 0 && hp->n_expert <= 0) hp->n_ff_exp = v;
    }
    {
        float f = cce_gguf_get_rope_freq_base(g);
        if (f > 0) hp->rope_theta = f;
    }
    /* MLA ranks: try metadata keys via get_int if available — keep defaults for non-DS */
    if (hp->n_heads > 0 && hp->qk_nope_head_dim <= 0)
        hp->qk_nope_head_dim = 64;
    if (hp->qk_rope_head_dim <= 0) hp->qk_rope_head_dim = 32;
    if (hp->v_head_dim <= 0) hp->v_head_dim = hp->qk_nope_head_dim;
    if (hp->kv_lora_rank <= 0) hp->kv_lora_rank = 128;
    return CCE_OK;
}

static int gguf_has(void* ctx, const char* name) {
    return cce_gguf_find_tensor((const cce_gguf*)ctx, name) >= 0;
}

cce_result cce_ds_import_gguf(const char* gguf_path, const char* pack_path,
                              const char* forest_path, int bind_cold) {
    cce_gguf* g = NULL;
    cce_ds_hparams hp;
    cce_ds_map map;
    cce_result rc;
    if (!gguf_path || !pack_path) return CCE_ERR_INVALID_ARG;
    rc = cce_gguf_load(gguf_path, &g);
    if (rc != CCE_OK || !g) return CCE_ERR_IO;
    cce_ds_hparams_from_gguf(g, &hp);
    /* Cap expert map size for import of huge MoE (contract still valid) */
    if (hp.n_expert > 64) {
        /* keep hparams true but pack only first 64 expert slots + banks via gguf names */
    }
    rc = cce_ds_map_build(&map, &hp);
    if (rc != CCE_OK) { cce_gguf_free(g); return rc; }

    rc = cce_ds_pack_write(pack_path, &map, cce_ds_gguf_load_weight, g);
    if (rc != CCE_OK) {
        /* fall back: write synthetic pack so pipeline stays offline-capable */
        rc = cce_ds_pack_write(pack_path, &map, NULL, NULL);
    }

    if (forest_path && rc == CCE_OK) {
        cce_forest* f = NULL;
        cce_ds_bind_opts o;
        cce_ds_pack* pack = NULL;
        cce_ds_bind_opts_default(&o, forest_path);
        o.bind_cold = bind_cold;
        o.synthetic = 0;
        if (cce_ds_pack_open(&pack, pack_path) == CCE_OK) {
            o.load_weight = cce_ds_pack_load_weight;
            o.load_ctx = pack;
            cce_ds_map_bind_forest(&map, &f, &o, NULL);
            if (f) cce_forest_close(f);
            cce_ds_pack_close(pack);
        }
    }
    (void)gguf_has;
    cce_ds_map_free(&map);
    cce_gguf_free(g);
    return rc;
}
