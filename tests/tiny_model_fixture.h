/* Shared tiny-model fixtures for the specialist-graph and weight-store tests.
 *
 * One deterministic set of tiny-llama weights writable as BOTH containers
 * (HF safetensors dir with config.json, and GGUF with ne-order dims matching
 * real files), plus a tiny mamba-1 safetensors. `variant != 0` perturbs
 * exactly ONE matrix (layer 1 gate_proj) — the synthetic "fine-tune".
 */

#ifndef TINY_MODEL_FIXTURE_H
#define TINY_MODEL_FIXTURE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <direct.h>
#define FIX_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define FIX_MKDIR(p) mkdir(p, 0777)
#endif

/* tiny llama dims (GQA, q/k/v biases, tied head) */
#define TL_L   2
#define TL_D   8
#define TL_H   2
#define TL_KV  1
#define TL_HD  4
#define TL_FFN 16
#define TL_V   24

typedef struct {
    float emb[TL_V][TL_D];
    float q_w[TL_L][TL_H * TL_HD][TL_D];   float q_b[TL_L][TL_H * TL_HD];
    float k_w[TL_L][TL_KV * TL_HD][TL_D];  float k_b[TL_L][TL_KV * TL_HD];
    float v_w[TL_L][TL_KV * TL_HD][TL_D];  float v_b[TL_L][TL_KV * TL_HD];
    float o_w[TL_L][TL_D][TL_H * TL_HD];
    float gate_w[TL_L][TL_FFN][TL_D];
    float up_w[TL_L][TL_FFN][TL_D];
    float down_w[TL_L][TL_D][TL_FFN];
    float attn_norm[TL_L][TL_D];
    float ffn_norm[TL_L][TL_D];
    float out_norm[TL_D];
} tl_weights;

static uint64_t tl_seed_state;
static float tl_rnd(void) {
    tl_seed_state = tl_seed_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(tl_seed_state >> 33) / 2147483648.0 - 1.0) * 0.35f;
}

static void tl_gen(tl_weights* w, int variant) {
    tl_seed_state = 0x9E3779B97F4A7C15ULL;
    float* p = (float*)w;
    size_t n = sizeof(*w) / sizeof(float);
    for (size_t i = 0; i < n; i++) p[i] = tl_rnd();
    for (int l = 0; l < TL_L; l++)
        for (int i = 0; i < TL_D; i++) {
            w->attn_norm[l][i] = 1.0f + 0.1f * w->attn_norm[l][i];
            w->ffn_norm[l][i]  = 1.0f + 0.1f * w->ffn_norm[l][i];
        }
    for (int i = 0; i < TL_D; i++) w->out_norm[i] = 1.0f + 0.1f * w->out_norm[i];
    if (variant == 1) {
        /* the synthetic fine-tune: exactly one matrix differs, materially */
        for (int i = 0; i < TL_FFN; i++)
            for (int j = 0; j < TL_D; j++)
                w->gate_w[1][i][j] += 0.01f * (float)(i + j + 1);
    } else if (variant == 2) {
        /* the epsilon fine-tune: same matrix, behaviorally negligible change */
        for (int i = 0; i < TL_FFN; i++)
            for (int j = 0; j < TL_D; j++)
                w->gate_w[1][i][j] += 1e-5f * (float)(i - j);
    }
}

typedef struct { char name[128]; int shape[4]; int ndim; const float* data; size_t numel; } tl_entry;

static int tl_add(tl_entry* e, int n, const char* name, int d0, int d1, int nd,
                  const float* data, size_t numel) {
    snprintf(e[n].name, sizeof(e[n].name), "%s", name);
    e[n].shape[0] = d0; e[n].shape[1] = d1; e[n].ndim = nd;
    e[n].data = data; e[n].numel = numel;
    return n + 1;
}

static int tl_entries(const tl_weights* w, tl_entry* e, int hf) {
    int n = 0;
    char nm[128];
    n = tl_add(e, n, hf ? "model.embed_tokens.weight" : "token_embd.weight",
               TL_V, TL_D, 2, &w->emb[0][0], (size_t)TL_V * TL_D);
    n = tl_add(e, n, hf ? "model.norm.weight" : "output_norm.weight", TL_D, 0, 1, w->out_norm, TL_D);
    for (int l = 0; l < TL_L; l++) {
#define TLP(hf_pat, gg_pat) snprintf(nm, sizeof(nm), hf ? hf_pat : gg_pat, l)
        TLP("model.layers.%d.self_attn.q_proj.weight", "blk.%d.attn_q.weight");
        n = tl_add(e, n, nm, TL_H * TL_HD, TL_D, 2, &w->q_w[l][0][0], (size_t)TL_H * TL_HD * TL_D);
        TLP("model.layers.%d.self_attn.q_proj.bias", "blk.%d.attn_q.bias");
        n = tl_add(e, n, nm, TL_H * TL_HD, 0, 1, w->q_b[l], (size_t)TL_H * TL_HD);
        TLP("model.layers.%d.self_attn.k_proj.weight", "blk.%d.attn_k.weight");
        n = tl_add(e, n, nm, TL_KV * TL_HD, TL_D, 2, &w->k_w[l][0][0], (size_t)TL_KV * TL_HD * TL_D);
        TLP("model.layers.%d.self_attn.k_proj.bias", "blk.%d.attn_k.bias");
        n = tl_add(e, n, nm, TL_KV * TL_HD, 0, 1, w->k_b[l], (size_t)TL_KV * TL_HD);
        TLP("model.layers.%d.self_attn.v_proj.weight", "blk.%d.attn_v.weight");
        n = tl_add(e, n, nm, TL_KV * TL_HD, TL_D, 2, &w->v_w[l][0][0], (size_t)TL_KV * TL_HD * TL_D);
        TLP("model.layers.%d.self_attn.v_proj.bias", "blk.%d.attn_v.bias");
        n = tl_add(e, n, nm, TL_KV * TL_HD, 0, 1, w->v_b[l], (size_t)TL_KV * TL_HD);
        TLP("model.layers.%d.self_attn.o_proj.weight", "blk.%d.attn_output.weight");
        n = tl_add(e, n, nm, TL_D, TL_H * TL_HD, 2, &w->o_w[l][0][0], (size_t)TL_D * TL_H * TL_HD);
        TLP("model.layers.%d.mlp.gate_proj.weight", "blk.%d.ffn_gate.weight");
        n = tl_add(e, n, nm, TL_FFN, TL_D, 2, &w->gate_w[l][0][0], (size_t)TL_FFN * TL_D);
        TLP("model.layers.%d.mlp.up_proj.weight", "blk.%d.ffn_up.weight");
        n = tl_add(e, n, nm, TL_FFN, TL_D, 2, &w->up_w[l][0][0], (size_t)TL_FFN * TL_D);
        TLP("model.layers.%d.mlp.down_proj.weight", "blk.%d.ffn_down.weight");
        n = tl_add(e, n, nm, TL_D, TL_FFN, 2, &w->down_w[l][0][0], (size_t)TL_D * TL_FFN);
        TLP("model.layers.%d.input_layernorm.weight", "blk.%d.attn_norm.weight");
        n = tl_add(e, n, nm, TL_D, 0, 1, w->attn_norm[l], TL_D);
        TLP("model.layers.%d.post_attention_layernorm.weight", "blk.%d.ffn_norm.weight");
        n = tl_add(e, n, nm, TL_D, 0, 1, w->ffn_norm[l], TL_D);
#undef TLP
    }
    return n; /* no lm_head / output.weight: tied head in both loaders */
}

static void tl_write_st(const char* path, const tl_entry* ents, int n_ents) {
    char json[16384];
    size_t j = 0;
    uint64_t off = 0;
    j += snprintf(json + j, sizeof(json) - j, "{");
    for (int i = 0; i < n_ents; i++) {
        uint64_t sz = ents[i].numel * 4;
        j += snprintf(json + j, sizeof(json) - j, "%s\"%s\":{\"dtype\":\"F32\",\"shape\":[",
                      i ? "," : "", ents[i].name);
        for (int d = 0; d < ents[i].ndim; d++)
            j += snprintf(json + j, sizeof(json) - j, "%s%d", d ? "," : "", ents[i].shape[d]);
        j += snprintf(json + j, sizeof(json) - j, "],\"data_offsets\":[%llu,%llu]}",
                      (unsigned long long)off, (unsigned long long)(off + sz));
        off += sz;
    }
    j += snprintf(json + j, sizeof(json) - j, "}");
    FILE* f = fopen(path, "wb");
    uint64_t hlen = (uint64_t)j;
    fwrite(&hlen, 8, 1, f);
    fwrite(json, 1, j, f);
    for (int i = 0; i < n_ents; i++) fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
}

static void tl_gg_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void tl_gg_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void tl_gg_str(FILE* f, const char* s) { tl_gg_u64(f, strlen(s)); fwrite(s, 1, strlen(s), f); }
static void tl_gg_kv_u32(FILE* f, const char* k, uint32_t v) { tl_gg_str(f, k); tl_gg_u32(f, 4); tl_gg_u32(f, v); }
static void tl_gg_kv_f32(FILE* f, const char* k, float v) { tl_gg_str(f, k); tl_gg_u32(f, 6); fwrite(&v, 4, 1, f); }

#define TL_ROPE 50000.0f
#define TL_EPS  1e-5f
#ifndef TL_CTX
#define TL_CTX  64  /* GGUF context_length; a test may #define TL_CTX before
                       including this header for a longer KV window (the
                       safetensors config.json path stays at 64) */
#endif

static void tl_write_gguf(const char* path, const tl_entry* ents, int n_ents) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    tl_gg_u32(f, 3);
    tl_gg_u64(f, (uint64_t)n_ents);
    tl_gg_u64(f, 9);
    tl_gg_str(f, "general.architecture"); tl_gg_u32(f, 8); tl_gg_str(f, "qwen2");
    tl_gg_kv_u32(f, "qwen2.block_count", TL_L);
    tl_gg_kv_u32(f, "qwen2.embedding_length", TL_D);
    tl_gg_kv_u32(f, "qwen2.attention.head_count", TL_H);
    tl_gg_kv_u32(f, "qwen2.attention.head_count_kv", TL_KV);
    tl_gg_kv_u32(f, "qwen2.context_length", TL_CTX);
    tl_gg_kv_u32(f, "qwen2.feed_forward_length", TL_FFN);
    tl_gg_kv_f32(f, "qwen2.rope.freq_base", TL_ROPE);
    tl_gg_kv_f32(f, "qwen2.attention.layer_norm_rms_epsilon", TL_EPS);
    /* dims in ggml ne order (reverse of torch), matching real gguf files */
    uint64_t off = 0;
    for (int i = 0; i < n_ents; i++) {
        tl_gg_str(f, ents[i].name);
        tl_gg_u32(f, (uint32_t)ents[i].ndim);
        for (int d = ents[i].ndim - 1; d >= 0; d--) tl_gg_u64(f, (uint64_t)ents[i].shape[d]);
        tl_gg_u32(f, 0 /* F32 */);
        tl_gg_u64(f, off);
        off += ents[i].numel * 4;
    }
    /* GGUF tensor offsets are relative to an aligned data section. The
       production loader defaults to the format's 32-byte alignment. */
    {
        long pos = ftell(f);
        int pad = (int)((32 - (pos % 32)) % 32);
        while (pad-- > 0) fputc(0, f);
    }
    for (int i = 0; i < n_ents; i++) fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
}

static void tl_write_config(const char* path) {
    FILE* f = fopen(path, "wb");
    fprintf(f, "{\"architectures\":[\"LlamaForCausalLM\"],"
               "\"num_attention_heads\": %d, \"num_key_value_heads\": %d,"
               "\"max_position_embeddings\": 64, \"bos_token_id\": 1, \"eos_token_id\": 2,"
               "\"rope_theta\": 50000.0, \"rms_norm_eps\": 1e-05}",
            TL_H, TL_KV);
    fclose(f);
}

/* dir gets model.safetensors + config.json */
static void tl_write_st_dir(const char* dir, const tl_weights* w) {
    char path[512];
    FIX_MKDIR(dir);
    tl_entry ents[64];
    int n = tl_entries(w, ents, 1);
    snprintf(path, sizeof(path), "%s/model.safetensors", dir);
    tl_write_st(path, ents, n);
    snprintf(path, sizeof(path), "%s/config.json", dir);
    tl_write_config(path);
}

static void tl_cleanup_st_dir(const char* dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/model.safetensors", dir);
    remove(path);
    snprintf(path, sizeof(path), "%s/config.json", dir);
    remove(path);
    remove(dir);
}

/* ---- tiny mamba-1 (safetensors only) ---- */

#define TM_L 2
#define TM_D 6
#define TM_E 8
#define TM_N 4
#define TM_K 4
#define TM_R 2
#define TM_V 12
#define TM_XDB (TM_R + 2 * TM_N)

typedef struct {
    float norm[TM_L][TM_D];
    float in_w[TM_L][2 * TM_E][TM_D];
    float conv_w[TM_L][TM_E][TM_K];
    float conv_b[TM_L][TM_E];
    float x_w[TM_L][TM_XDB][TM_E];
    float dt_w[TM_L][TM_E][TM_R];
    float dt_b[TM_L][TM_E];
    float a_log[TM_L][TM_E][TM_N];
    float dvec[TM_L][TM_E];
    float out_w[TM_L][TM_D][TM_E];
    float emb[TM_V][TM_D];
    float norm_f[TM_D];
} tm_weights;

static void tm_gen(tm_weights* w) {
    tl_seed_state = 0x243F6A8885A308D3ULL;
    float* p = (float*)w;
    size_t n = sizeof(*w) / sizeof(float);
    for (size_t i = 0; i < n; i++) p[i] = tl_rnd();
    for (int l = 0; l < TM_L; l++)
        for (int i = 0; i < TM_D; i++) w->norm[l][i] = 1.0f + 0.1f * w->norm[l][i];
    for (int i = 0; i < TM_D; i++) w->norm_f[i] = 1.0f + 0.1f * w->norm_f[i];
}

static void tm_write_st(const char* path, const tm_weights* w) {
    tl_entry e[64];
    int n = 0;
    char nm[128];
    n = tl_add(e, n, "backbone.embedding.weight", TM_V, TM_D, 2, &w->emb[0][0], (size_t)TM_V * TM_D);
    n = tl_add(e, n, "backbone.norm_f.weight", TM_D, 0, 1, w->norm_f, TM_D);
    for (int l = 0; l < TM_L; l++) {
        snprintf(nm, sizeof(nm), "backbone.layers.%d.norm.weight", l);
        n = tl_add(e, n, nm, TM_D, 0, 1, w->norm[l], TM_D);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.in_proj.weight", l);
        n = tl_add(e, n, nm, 2 * TM_E, TM_D, 2, &w->in_w[l][0][0], (size_t)2 * TM_E * TM_D);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.conv1d.weight", l);
        n = tl_add(e, n, nm, TM_E, TM_K, 2, &w->conv_w[l][0][0], (size_t)TM_E * TM_K);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.conv1d.bias", l);
        n = tl_add(e, n, nm, TM_E, 0, 1, w->conv_b[l], TM_E);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.x_proj.weight", l);
        n = tl_add(e, n, nm, TM_XDB, TM_E, 2, &w->x_w[l][0][0], (size_t)TM_XDB * TM_E);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.dt_proj.weight", l);
        n = tl_add(e, n, nm, TM_E, TM_R, 2, &w->dt_w[l][0][0], (size_t)TM_E * TM_R);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.dt_proj.bias", l);
        n = tl_add(e, n, nm, TM_E, 0, 1, w->dt_b[l], TM_E);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.A_log", l);
        n = tl_add(e, n, nm, TM_E, TM_N, 2, &w->a_log[l][0][0], (size_t)TM_E * TM_N);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.D", l);
        n = tl_add(e, n, nm, TM_E, 0, 1, w->dvec[l], TM_E);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.out_proj.weight", l);
        n = tl_add(e, n, nm, TM_D, TM_E, 2, &w->out_w[l][0][0], (size_t)TM_D * TM_E);
    }
    tl_write_st(path, e, n);
}

#endif /* TINY_MODEL_FIXTURE_H */
