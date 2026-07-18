/* Dual inference backend: DS residual host + GGUF token path, CPU/GPU device. */
#include "../../include/cce/cce_infer_backend.h"
#include "../../include/cce/cce_gguf.h"
#include "../../include/cce/cce_clgemm.h"
#include "../../include/cce/cce_hipgemm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Hermetic tiny GGUF writer (same dims as tests/tiny_model_fixture.h).
 * Kept here so tools/benches do not need the test header. */
#define IB_L   2
#define IB_D   8
#define IB_H   2
#define IB_KV  1
#define IB_HD  4
#define IB_FFN 16
#define IB_V   24
#define IB_CTX 256
#define IB_ROPE 50000.0f
#define IB_EPS  1e-5f

typedef struct {
    float emb[IB_V][IB_D];
    float q_w[IB_L][IB_H * IB_HD][IB_D];   float q_b[IB_L][IB_H * IB_HD];
    float k_w[IB_L][IB_KV * IB_HD][IB_D];  float k_b[IB_L][IB_KV * IB_HD];
    float v_w[IB_L][IB_KV * IB_HD][IB_D];  float v_b[IB_L][IB_KV * IB_HD];
    float o_w[IB_L][IB_D][IB_H * IB_HD];
    float gate_w[IB_L][IB_FFN][IB_D];
    float up_w[IB_L][IB_FFN][IB_D];
    float down_w[IB_L][IB_D][IB_FFN];
    float attn_norm[IB_L][IB_D];
    float ffn_norm[IB_L][IB_D];
    float out_norm[IB_D];
} ib_weights;

typedef struct {
    char name[128];
    int shape[4];
    int ndim;
    const float* data;
    size_t numel;
} ib_entry;

struct cce_infer_session {
    cce_infer_kind   kind;
    cce_infer_device device;
    cce_ds_host*     ds;
    cce_gguf_qwen2*  gguf;
    cce_clgemm*      clgemm;       /* owned when device==GPU (OpenCL) */
    cce_hipgemm*     hipgemm;      /* owned when device==GPU (hipBLAS) */
    char             device_name[160];
    char             synthetic_path[256];
    int              owns_synthetic; /* remove path on close */
    int              vocab;
    int              d_model;
    int              n_layer;
    float*           logits_scratch;
};

static uint64_t ib_seed;
static float ib_rnd(void) {
    ib_seed = ib_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(ib_seed >> 33) / 2147483648.0 - 1.0) * 0.35f;
}

static void ib_gen(ib_weights* w) {
    size_t i, n = sizeof(*w) / sizeof(float);
    float* p = (float*)w;
    int l;
    ib_seed = 0x9E3779B97F4A7C15ULL;
    for (i = 0; i < n; i++) p[i] = ib_rnd();
    for (l = 0; l < IB_L; l++)
        for (i = 0; i < (size_t)IB_D; i++) {
            w->attn_norm[l][i] = 1.0f + 0.1f * w->attn_norm[l][i];
            w->ffn_norm[l][i]  = 1.0f + 0.1f * w->ffn_norm[l][i];
        }
    for (i = 0; i < (size_t)IB_D; i++)
        w->out_norm[i] = 1.0f + 0.1f * w->out_norm[i];
}

static void ib_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void ib_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void ib_str(FILE* f, const char* s) {
    ib_u64(f, (uint64_t)strlen(s));
    fwrite(s, 1, strlen(s), f);
}
static void ib_kv_u32(FILE* f, const char* k, uint32_t v) {
    ib_str(f, k); ib_u32(f, 4); ib_u32(f, v);
}
static void ib_kv_f32(FILE* f, const char* k, float v) {
    ib_str(f, k); ib_u32(f, 6); fwrite(&v, 4, 1, f);
}

static int ib_add(ib_entry* e, int n, const char* name, int d0, int d1, int nd,
                  const float* data, size_t numel) {
    snprintf(e[n].name, sizeof e[n].name, "%s", name);
    e[n].shape[0] = d0; e[n].shape[1] = d1; e[n].ndim = nd;
    e[n].data = data; e[n].numel = numel;
    return n + 1;
}

static int ib_entries(const ib_weights* w, ib_entry* e) {
    int n = 0, l;
    char nm[128];
    n = ib_add(e, n, "token_embd.weight", IB_V, IB_D, 2,
               &w->emb[0][0], (size_t)IB_V * IB_D);
    n = ib_add(e, n, "output_norm.weight", IB_D, 0, 1, w->out_norm, IB_D);
    for (l = 0; l < IB_L; l++) {
        snprintf(nm, sizeof nm, "blk.%d.attn_q.weight", l);
        n = ib_add(e, n, nm, IB_H * IB_HD, IB_D, 2,
                   &w->q_w[l][0][0], (size_t)IB_H * IB_HD * IB_D);
        snprintf(nm, sizeof nm, "blk.%d.attn_q.bias", l);
        n = ib_add(e, n, nm, IB_H * IB_HD, 0, 1, w->q_b[l], (size_t)IB_H * IB_HD);
        snprintf(nm, sizeof nm, "blk.%d.attn_k.weight", l);
        n = ib_add(e, n, nm, IB_KV * IB_HD, IB_D, 2,
                   &w->k_w[l][0][0], (size_t)IB_KV * IB_HD * IB_D);
        snprintf(nm, sizeof nm, "blk.%d.attn_k.bias", l);
        n = ib_add(e, n, nm, IB_KV * IB_HD, 0, 1, w->k_b[l], (size_t)IB_KV * IB_HD);
        snprintf(nm, sizeof nm, "blk.%d.attn_v.weight", l);
        n = ib_add(e, n, nm, IB_KV * IB_HD, IB_D, 2,
                   &w->v_w[l][0][0], (size_t)IB_KV * IB_HD * IB_D);
        snprintf(nm, sizeof nm, "blk.%d.attn_v.bias", l);
        n = ib_add(e, n, nm, IB_KV * IB_HD, 0, 1, w->v_b[l], (size_t)IB_KV * IB_HD);
        snprintf(nm, sizeof nm, "blk.%d.attn_output.weight", l);
        n = ib_add(e, n, nm, IB_D, IB_H * IB_HD, 2,
                   &w->o_w[l][0][0], (size_t)IB_D * IB_H * IB_HD);
        snprintf(nm, sizeof nm, "blk.%d.ffn_gate.weight", l);
        n = ib_add(e, n, nm, IB_FFN, IB_D, 2,
                   &w->gate_w[l][0][0], (size_t)IB_FFN * IB_D);
        snprintf(nm, sizeof nm, "blk.%d.ffn_up.weight", l);
        n = ib_add(e, n, nm, IB_FFN, IB_D, 2,
                   &w->up_w[l][0][0], (size_t)IB_FFN * IB_D);
        snprintf(nm, sizeof nm, "blk.%d.ffn_down.weight", l);
        n = ib_add(e, n, nm, IB_D, IB_FFN, 2,
                   &w->down_w[l][0][0], (size_t)IB_D * IB_FFN);
        snprintf(nm, sizeof nm, "blk.%d.attn_norm.weight", l);
        n = ib_add(e, n, nm, IB_D, 0, 1, w->attn_norm[l], IB_D);
        snprintf(nm, sizeof nm, "blk.%d.ffn_norm.weight", l);
        n = ib_add(e, n, nm, IB_D, 0, 1, w->ffn_norm[l], IB_D);
    }
    return n;
}

static int ib_write_gguf(const char* path, const ib_entry* ents, int n_ents) {
    FILE* f = fopen(path, "wb");
    uint64_t off = 0;
    int i, d;
    long pos;
    int pad;
    if (!f) return -1;
    fwrite("GGUF", 1, 4, f);
    ib_u32(f, 3);
    ib_u64(f, (uint64_t)n_ents);
    ib_u64(f, 9);
    ib_str(f, "general.architecture"); ib_u32(f, 8); ib_str(f, "qwen2");
    ib_kv_u32(f, "qwen2.block_count", IB_L);
    ib_kv_u32(f, "qwen2.embedding_length", IB_D);
    ib_kv_u32(f, "qwen2.attention.head_count", IB_H);
    ib_kv_u32(f, "qwen2.attention.head_count_kv", IB_KV);
    ib_kv_u32(f, "qwen2.context_length", IB_CTX);
    ib_kv_u32(f, "qwen2.feed_forward_length", IB_FFN);
    ib_kv_f32(f, "qwen2.rope.freq_base", IB_ROPE);
    ib_kv_f32(f, "qwen2.attention.layer_norm_rms_epsilon", IB_EPS);
    for (i = 0; i < n_ents; i++) {
        ib_str(f, ents[i].name);
        ib_u32(f, (uint32_t)ents[i].ndim);
        for (d = ents[i].ndim - 1; d >= 0; d--)
            ib_u64(f, (uint64_t)ents[i].shape[d]);
        ib_u32(f, 0); /* F32 */
        ib_u64(f, off);
        off += ents[i].numel * 4;
    }
    pos = ftell(f);
    pad = (int)((32 - (pos % 32)) % 32);
    while (pad-- > 0) fputc(0, f);
    for (i = 0; i < n_ents; i++)
        fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
    return 0;
}

static int write_synthetic_gguf(const char* path) {
    ib_weights* w = (ib_weights*)malloc(sizeof(ib_weights));
    ib_entry ents[64];
    int n, rc;
    if (!w) return -1;
    ib_gen(w);
    n = ib_entries(w, ents);
    rc = ib_write_gguf(path, ents, n);
    free(w);
    return rc;
}

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void cce_infer_opts_default(cce_infer_opts* o, cce_infer_kind kind,
                            cce_infer_device device) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->device = device;
    o->synthetic = (kind == CCE_INFER_KIND_GGUF) ? 1 : 0;
    o->max_ctx = 64;
    o->sparse_kv = 0.0f;
    o->dsa_enable = 1;
    o->dsa_fraction = 0.25f;
    if (kind == CCE_INFER_KIND_DS) {
        cce_ds_host_opts_default(&o->ds_opts, "infer_ds.cce", NULL);
        o->ds_opts.synthetic = 1;
        o->ds_opts.max_ctx = 64;
        o->ds_opts.dsa_enable = 1;
        o->ds_opts.dsa_fraction = 0.25f;
        o->ds_opts.cold_autoload = 1;
    }
}

const char* cce_infer_kind_name(cce_infer_kind k) {
    switch (k) {
    case CCE_INFER_KIND_DS:   return "ds";
    case CCE_INFER_KIND_GGUF: return "gguf";
    default: return "?";
    }
}

const char* cce_infer_device_name(cce_infer_device d) {
    switch (d) {
    case CCE_INFER_DEVICE_CPU: return "cpu";
    case CCE_INFER_DEVICE_GPU: return "gpu";
    default: return "?";
    }
}

cce_infer_kind   cce_infer_get_kind(const cce_infer_session* s) {
    return s ? s->kind : (cce_infer_kind)-1;
}
cce_infer_device cce_infer_get_device(const cce_infer_session* s) {
    return s ? s->device : (cce_infer_device)-1;
}
int cce_infer_vocab(const cce_infer_session* s) {
    return s ? s->vocab : 0;
}
int cce_infer_d_model(const cce_infer_session* s) {
    return s ? s->d_model : 0;
}
int cce_infer_n_layer(const cce_infer_session* s) {
    return s ? s->n_layer : 0;
}

cce_ds_host* cce_infer_as_ds(cce_infer_session* s) {
    return (s && s->kind == CCE_INFER_KIND_DS) ? s->ds : NULL;
}
struct cce_gguf_qwen2* cce_infer_as_gguf(cce_infer_session* s) {
    return (s && s->kind == CCE_INFER_KIND_GGUF) ? s->gguf : NULL;
}

void cce_infer_close(cce_infer_session* s) {
    if (!s) return;
    if (s->gguf) {
        if (s->hipgemm) {
            cce_gguf_qwen2_set_hipgemm(s->gguf, NULL);
            cce_hipgemm_close(s->hipgemm);
            s->hipgemm = NULL;
        }
        if (s->clgemm) {
            cce_gguf_qwen2_set_clgemm(s->gguf, NULL);
            cce_clgemm_close(s->clgemm);
            s->clgemm = NULL;
        }
        cce_gguf_qwen2_free(s->gguf);
        s->gguf = NULL;
    }
    if (s->ds) {
        cce_ds_host_close(s->ds);
        s->ds = NULL;
    }
    free(s->logits_scratch);
    if (s->owns_synthetic && s->synthetic_path[0])
        remove(s->synthetic_path);
    free(s);
}

static cce_result open_ds(cce_infer_session* s, const cce_infer_opts* opts) {
    cce_ds_hparams hp_local;
    const cce_ds_hparams* hp = opts->ds_hp;
    cce_ds_host_opts o = opts->ds_opts;
    if (opts->device == CCE_INFER_DEVICE_GPU)
        return CCE_ERR_UNSUPPORTED; /* DS GPU path reserved */
    if (!hp) {
        cce_ds_hparams_default_small(&hp_local);
        hp_local.n_layer = 2;
        hp_local.d_model = 128;
        hp_local.n_heads = 4;
        hp_local.qk_nope_head_dim = 16;
        hp_local.qk_rope_head_dim = 8;
        hp_local.v_head_dim = 16;
        hp_local.kv_lora_rank = 32;
        hp_local.n_expert = 4;
        hp_local.n_expert_used = 2;
        hp_local.n_ff_exp = 64;
        hp_local.vocab = 256;
        hp = &hp_local;
    }
    if (!o.archive_path) o.archive_path = "infer_ds.cce";
    if (o.max_ctx < 1) o.max_ctx = opts->max_ctx > 0 ? opts->max_ctx : 64;
    if (cce_ds_host_open(&s->ds, hp, &o) != CCE_OK || !s->ds)
        return CCE_ERR_IO;
    s->vocab = s->ds->vocab;
    s->d_model = s->ds->d_model;
    s->n_layer = s->ds->n_layer;
    if (s->vocab > 0) {
        s->logits_scratch = (float*)calloc((size_t)s->vocab, sizeof(float));
        if (!s->logits_scratch) return CCE_ERR_OOM;
    }
    return CCE_OK;
}

static cce_result open_gguf(cce_infer_session* s, const cce_infer_opts* opts) {
    const char* path = opts->gguf_path;
    cce_result rc;
    if (opts->synthetic || !path || !path[0]) {
        const char* sp = opts->synthetic_path;
        if (!sp || !sp[0]) sp = "infer_gguf_synth.gguf";
        snprintf(s->synthetic_path, sizeof s->synthetic_path, "%s", sp);
        if (write_synthetic_gguf(s->synthetic_path) != 0)
            return CCE_ERR_IO;
        s->owns_synthetic = 1;
        path = s->synthetic_path;
    }
    rc = cce_gguf_load_qwen2(&s->gguf, path);
    if (rc != CCE_OK || !s->gguf) return rc != CCE_OK ? rc : CCE_ERR_IO;

    if (opts->sparse_kv > 0.0f) {
        if (cce_gguf_qwen2_set_sparse_kv(s->gguf, opts->sparse_kv) != CCE_OK) {
            /* keep model; sparse off on refuse */
        }
    }
    if (opts->dsa_enable && opts->sparse_kv <= 0.0f) {
        /* DSA rides sparse_kv budget; enable a default fraction when DSA on */
        float frac = opts->dsa_fraction > 0.0f ? opts->dsa_fraction : 0.25f;
        (void)cce_gguf_qwen2_set_sparse_kv(s->gguf, frac);
    }

    if (opts->device == CCE_INFER_DEVICE_GPU) {
        /* CNET_GPU_BACKEND=hip|opencl|auto.
           auto: OpenCL first (best full-forward on dual R9700), hip optional
           fallback for large FP GEMMs when OpenCL refuses a matrix. */
        const char *be = getenv("CNET_GPU_BACKEND");
        int want_hip = 1, want_cl = 1;
        char hip_name[128] = {0}, cl_name[128] = {0};
        if (be && be[0]) {
            if (strcmp(be, "hip") == 0) {
                want_cl = 0;
            } else if (strcmp(be, "opencl") == 0 || strcmp(be, "cl") == 0) {
                want_hip = 0;
            }
        }
        if (want_cl) {
            s->clgemm = cce_clgemm_open(NULL, cl_name, sizeof cl_name);
            if (s->clgemm)
                cce_gguf_qwen2_set_clgemm(s->gguf, s->clgemm);
        }
        if (want_hip) {
            s->hipgemm = cce_hipgemm_open(hip_name, sizeof hip_name);
            if (s->hipgemm)
                cce_gguf_qwen2_set_hipgemm(s->gguf, s->hipgemm);
        }
        if (!s->hipgemm && !s->clgemm)
            return CCE_ERR_NOT_FOUND;
        if (s->hipgemm && s->clgemm)
            snprintf(s->device_name, sizeof s->device_name, "%s + %s",
                     hip_name[0] ? hip_name : "hip",
                     cl_name[0] ? cl_name : "opencl");
        else if (s->hipgemm)
            snprintf(s->device_name, sizeof s->device_name, "%s",
                     hip_name[0] ? hip_name : "hipBLAS");
        else
            snprintf(s->device_name, sizeof s->device_name, "%s",
                     cl_name[0] ? cl_name : "OpenCL");
    }

    s->vocab = s->gguf->vocab_size;
    s->d_model = s->gguf->n_embd;
    s->n_layer = s->gguf->n_layer;
    s->logits_scratch = (float*)calloc((size_t)s->vocab, sizeof(float));
    if (!s->logits_scratch) return CCE_ERR_OOM;
    return CCE_OK;
}

cce_result cce_infer_open(cce_infer_session** out, const cce_infer_opts* opts) {
    cce_infer_session* s;
    cce_result rc;
    if (!out || !opts) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    s = (cce_infer_session*)calloc(1, sizeof *s);
    if (!s) return CCE_ERR_OOM;
    s->kind = opts->kind;
    s->device = opts->device;

    if (opts->kind == CCE_INFER_KIND_DS)
        rc = open_ds(s, opts);
    else if (opts->kind == CCE_INFER_KIND_GGUF)
        rc = open_gguf(s, opts);
    else
        rc = CCE_ERR_INVALID_ARG;

    if (rc != CCE_OK) {
        cce_infer_close(s);
        return rc;
    }
    *out = s;
    return CCE_OK;
}

void cce_infer_reset(cce_infer_session* s) {
    if (!s) return;
    if (s->kind == CCE_INFER_KIND_DS && s->ds)
        cce_ds_host_reset(s->ds);
    if (s->kind == CCE_INFER_KIND_GGUF && s->gguf)
        s->gguf->cur_pos = 0;
}

cce_result cce_infer_forward_tokens(cce_infer_session* s,
                                    const int* tokens, int n_tokens,
                                    float* logits_out, int logits_cap) {
    int t, i;
    if (!s || !tokens || n_tokens < 1) return CCE_ERR_INVALID_ARG;
    if (s->kind == CCE_INFER_KIND_GGUF) {
        if (!s->gguf) return CCE_ERR_INVALID_ARG;
        return cce_gguf_qwen2_forward(s->gguf, tokens, n_tokens,
                                      logits_out, logits_cap);
    }
    /* DS: residual steps driven by token-seeded residual */
    if (!s->ds) return CCE_ERR_INVALID_ARG;
    for (t = 0; t < n_tokens; ++t) {
        int tok = tokens[t];
        for (i = 0; i < s->ds->d_model; ++i)
            s->ds->residual[i] =
                0.02f * sinf(0.07f * (float)(i + 1 + tok * 3 + t));
        if (cce_ds_host_forward_token(s->ds) != CCE_OK)
            return CCE_ERR_IO;
    }
    if (logits_out && logits_cap > 0) {
        int n = logits_cap < s->vocab ? logits_cap : s->vocab;
        if (s->ds->logits && n > 0)
            memcpy(logits_out, s->ds->logits, (size_t)n * sizeof(float));
        else
            memset(logits_out, 0, (size_t)logits_cap * sizeof(float));
    }
    return CCE_OK;
}

cce_result cce_infer_bench(cce_infer_session* s, int n_tokens, double* out_tok_s) {
    double t0, t1, dt;
    int t;
    float* lg;
    int* toks;
    if (!s || n_tokens < 1) return CCE_ERR_INVALID_ARG;

    if (s->kind == CCE_INFER_KIND_DS && s->ds) {
        double tps = 0;
        cce_result rc = cce_ds_host_bench(s->ds, n_tokens, &tps);
        if (rc == CCE_OK && out_tok_s) *out_tok_s = tps;
        return rc;
    }

    /* GGUF: single-token teacher-forced steps (token gen shape) */
    if (!s->gguf) return CCE_ERR_INVALID_ARG;
    lg = s->logits_scratch;
    if (!lg) return CCE_ERR_OOM;
    toks = (int*)malloc((size_t)n_tokens * sizeof(int));
    if (!toks) return CCE_ERR_OOM;
    for (t = 0; t < n_tokens; ++t)
        toks[t] = 2 + (t * 3 + 1) % (s->vocab > 4 ? (s->vocab - 2) : 1);

    cce_infer_reset(s);
    t0 = wall_s();
    /* prefill first token, then decode rest one-by-one */
    if (cce_gguf_qwen2_forward(s->gguf, &toks[0], 1, lg, s->vocab) != CCE_OK) {
        free(toks);
        return CCE_ERR_IO;
    }
    for (t = 1; t < n_tokens; ++t) {
        if (s->gguf->cur_pos >= s->gguf->max_ctx) {
            /* wrap: restart KV when synthetic ctx is short */
            s->gguf->cur_pos = 0;
        }
        if (cce_gguf_qwen2_forward(s->gguf, &toks[t], 1, lg, s->vocab) != CCE_OK) {
            free(toks);
            return CCE_ERR_IO;
        }
    }
    t1 = wall_s();
    free(toks);
    dt = t1 - t0;
    if (dt < 1e-9) dt = 1e-9;
    if (out_tok_s) *out_tok_s = (double)n_tokens / dt;
    return CCE_OK;
}
