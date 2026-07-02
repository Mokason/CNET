/* cce_st_llama: HF-llama safetensors -> the SAME decomposed transformer the
 * GGUF loader builds.
 *
 * Oracle: write one set of deterministic tiny-llama weights as BOTH a GGUF
 * file and an HF-named safetensors dir (with config.json), load each through
 * its loader, and run the shared cce_gguf_qwen2_forward. The GGUF path is the
 * already-verified runner, so bit-identical logits prove the new mapping
 * (names, orientation, biases, norms, tied head, kv metadata vs config.json)
 * is exact. Also: kv-cache continuity, tied-head handling, config.json
 * refusal, and anymodel dispatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "../include/cce/cce_st_llama.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_detect.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

/* dims: GQA (H=2, KV=1), biases on q/k/v (qwen2-style), tied head */
#define L_   2
#define D_   8
#define H_   2
#define KV_  1
#define HD_  4
#define FFN_ 16
#define V_   24

static uint64_t g_seed = 0x9E3779B97F4A7C15ULL;
static float rndf(void) {
    g_seed = g_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(g_seed >> 33) / 2147483648.0 - 1.0) * 0.35f;
}

typedef struct {
    float emb[V_][D_];
    float q_w[L_][H_ * HD_][D_];   float q_b[L_][H_ * HD_];
    float k_w[L_][KV_ * HD_][D_];  float k_b[L_][KV_ * HD_];
    float v_w[L_][KV_ * HD_][D_];  float v_b[L_][KV_ * HD_];
    float o_w[L_][D_][H_ * HD_];
    float gate_w[L_][FFN_][D_];
    float up_w[L_][FFN_][D_];
    float down_w[L_][D_][FFN_];
    float attn_norm[L_][D_];
    float ffn_norm[L_][D_];
    float out_norm[D_];
} llama_weights;

static void gen_weights(llama_weights* w) {
    float* p = (float*)w;
    size_t n = sizeof(*w) / sizeof(float);
    for (size_t i = 0; i < n; i++) p[i] = rndf();
    for (int l = 0; l < L_; l++)
        for (int i = 0; i < D_; i++) {
            w->attn_norm[l][i] = 1.0f + 0.1f * w->attn_norm[l][i];
            w->ffn_norm[l][i]  = 1.0f + 0.1f * w->ffn_norm[l][i];
        }
    for (int i = 0; i < D_; i++) w->out_norm[i] = 1.0f + 0.1f * w->out_norm[i];
}

/* ---- shared entry list ---- */

typedef struct { char name[128]; int shape[4]; int ndim; const float* data; size_t numel; } wentry;

static int add_entry(wentry* e, int n, const char* name, int d0, int d1, int nd,
                     const float* data, size_t numel) {
    snprintf(e[n].name, sizeof(e[n].name), "%s", name);
    e[n].shape[0] = d0; e[n].shape[1] = d1; e[n].ndim = nd;
    e[n].data = data; e[n].numel = numel;
    return n + 1;
}

static int build_entries(const llama_weights* w, wentry* e, int hf) {
    int n = 0;
    char nm[128];
    n = add_entry(e, n, hf ? "model.embed_tokens.weight" : "token_embd.weight",
                  V_, D_, 2, &w->emb[0][0], (size_t)V_ * D_);
    n = add_entry(e, n, hf ? "model.norm.weight" : "output_norm.weight",
                  D_, 0, 1, w->out_norm, D_);
    for (int l = 0; l < L_; l++) {
#define P(hf_pat, gg_pat) do { snprintf(nm, sizeof(nm), hf ? hf_pat : gg_pat, l); } while (0)
        P("model.layers.%d.self_attn.q_proj.weight", "blk.%d.attn_q.weight");
        n = add_entry(e, n, nm, H_ * HD_, D_, 2, &w->q_w[l][0][0], (size_t)H_ * HD_ * D_);
        P("model.layers.%d.self_attn.q_proj.bias", "blk.%d.attn_q.bias");
        n = add_entry(e, n, nm, H_ * HD_, 0, 1, w->q_b[l], (size_t)H_ * HD_);
        P("model.layers.%d.self_attn.k_proj.weight", "blk.%d.attn_k.weight");
        n = add_entry(e, n, nm, KV_ * HD_, D_, 2, &w->k_w[l][0][0], (size_t)KV_ * HD_ * D_);
        P("model.layers.%d.self_attn.k_proj.bias", "blk.%d.attn_k.bias");
        n = add_entry(e, n, nm, KV_ * HD_, 0, 1, w->k_b[l], (size_t)KV_ * HD_);
        P("model.layers.%d.self_attn.v_proj.weight", "blk.%d.attn_v.weight");
        n = add_entry(e, n, nm, KV_ * HD_, D_, 2, &w->v_w[l][0][0], (size_t)KV_ * HD_ * D_);
        P("model.layers.%d.self_attn.v_proj.bias", "blk.%d.attn_v.bias");
        n = add_entry(e, n, nm, KV_ * HD_, 0, 1, w->v_b[l], (size_t)KV_ * HD_);
        P("model.layers.%d.self_attn.o_proj.weight", "blk.%d.attn_output.weight");
        n = add_entry(e, n, nm, D_, H_ * HD_, 2, &w->o_w[l][0][0], (size_t)D_ * H_ * HD_);
        P("model.layers.%d.mlp.gate_proj.weight", "blk.%d.ffn_gate.weight");
        n = add_entry(e, n, nm, FFN_, D_, 2, &w->gate_w[l][0][0], (size_t)FFN_ * D_);
        P("model.layers.%d.mlp.up_proj.weight", "blk.%d.ffn_up.weight");
        n = add_entry(e, n, nm, FFN_, D_, 2, &w->up_w[l][0][0], (size_t)FFN_ * D_);
        P("model.layers.%d.mlp.down_proj.weight", "blk.%d.ffn_down.weight");
        n = add_entry(e, n, nm, D_, FFN_, 2, &w->down_w[l][0][0], (size_t)D_ * FFN_);
        P("model.layers.%d.input_layernorm.weight", "blk.%d.attn_norm.weight");
        n = add_entry(e, n, nm, D_, 0, 1, w->attn_norm[l], D_);
        P("model.layers.%d.post_attention_layernorm.weight", "blk.%d.ffn_norm.weight");
        n = add_entry(e, n, nm, D_, 0, 1, w->ffn_norm[l], D_);
#undef P
    }
    /* no lm_head / output.weight: tied head via the embedding in BOTH loaders */
    return n;
}

/* ---- writers ---- */

static void st_write(const char* path, const wentry* ents, int n_ents) {
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

static void gg_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void gg_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void gg_str(FILE* f, const char* s) { gg_u64(f, strlen(s)); fwrite(s, 1, strlen(s), f); }
static void gg_kv_u32(FILE* f, const char* k, uint32_t v) { gg_str(f, k); gg_u32(f, 4); gg_u32(f, v); }
static void gg_kv_f32(FILE* f, const char* k, float v) { gg_str(f, k); gg_u32(f, 6); fwrite(&v, 4, 1, f); }

/* deliberately non-default numerics: prove rope_theta / rms_norm_eps plumb
   through BOTH containers identically */
#define ROPE_THETA_ 50000.0f
#define RMS_EPS_    1e-5f

static void gguf_write(const char* path, const wentry* ents, int n_ents) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    gg_u32(f, 3);
    gg_u64(f, (uint64_t)n_ents);
    gg_u64(f, 9);
    gg_str(f, "general.architecture"); gg_u32(f, 8); gg_str(f, "qwen2");
    gg_kv_u32(f, "qwen2.block_count", L_);
    gg_kv_u32(f, "qwen2.embedding_length", D_);
    gg_kv_u32(f, "qwen2.attention.head_count", H_);
    gg_kv_u32(f, "qwen2.attention.head_count_kv", KV_);
    gg_kv_u32(f, "qwen2.context_length", 64);
    gg_kv_u32(f, "qwen2.feed_forward_length", FFN_);
    gg_kv_f32(f, "qwen2.rope.freq_base", ROPE_THETA_);
    gg_kv_f32(f, "qwen2.attention.layer_norm_rms_epsilon", RMS_EPS_);
    /* dims in ggml ne order (innermost first, reverse of torch), matching
       real gguf files; bytes stay row-major [out][in] */
    uint64_t off = 0;
    for (int i = 0; i < n_ents; i++) {
        gg_str(f, ents[i].name);
        gg_u32(f, (uint32_t)ents[i].ndim);
        for (int d = ents[i].ndim - 1; d >= 0; d--) gg_u64(f, (uint64_t)ents[i].shape[d]);
        gg_u32(f, 0 /* F32 */);
        gg_u64(f, off);
        off += ents[i].numel * 4;
    }
    for (int i = 0; i < n_ents; i++) fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
}

static void write_config_json(const char* path) {
    FILE* f = fopen(path, "wb");
    fprintf(f, "{\"architectures\":[\"LlamaForCausalLM\"],"
               "\"num_attention_heads\": %d, \"num_key_value_heads\": %d,"
               "\"max_position_embeddings\": 64, \"bos_token_id\": 1, \"eos_token_id\": 2,"
               "\"rope_theta\": 50000.0, \"rms_norm_eps\": 1e-05}",
            H_, KV_);
    fclose(f);
}

/* ---- test ---- */

int main(void) {
    printf("=== cce_st_llama: HF safetensors == GGUF, one shared runner ===\n");

    llama_weights* w = (llama_weights*)malloc(sizeof(llama_weights));
    gen_weights(w);

    static const int tokens[4] = { 3, 17, 9, 22 };
    const int NT = 4;

    wentry ents[64];
    int n_gg = build_entries(w, ents, 0);
    gguf_write("stll_test.gguf", ents, n_gg);

    MKDIR("stll_tmp");
    int n_st = build_entries(w, ents, 1);
    st_write("stll_tmp/model.safetensors", ents, n_st);
    write_config_json("stll_tmp/config.json");

    /* 1. trusted-oracle logits via the GGUF path */
    float gg_logits[V_];
    {
        cce_gguf_qwen2* m = NULL;
        CHECK(cce_gguf_load_model(&m, "stll_test.gguf") == CCE_OK && m, "gguf oracle loads");
        if (!m) return 1;
        CHECK(m->n_layer == L_ && m->n_embd == D_ && m->n_head == H_ &&
              m->n_kv_head == KV_ && m->vocab_size == V_, "gguf hparams");
        CHECK(m->rope_freq_base == ROPE_THETA_ && fabsf(m->rms_eps - RMS_EPS_) < 1e-9f,
              "rope base + rms eps read from gguf metadata");
        CHECK(cce_gguf_qwen2_forward(m, tokens, NT, gg_logits, V_) == CCE_OK, "gguf forward ok");
        cce_gguf_qwen2_free(m);
    }

    /* 2. same weights through the HF safetensors loader */
    float st_logits[V_];
    {
        cce_gguf_qwen2* m = NULL;
        CHECK(cce_st_llama_load(&m, "stll_tmp/model.safetensors") == CCE_OK && m, "hf st loads");
        if (!m) return 1;
        CHECK(m->n_layer == L_ && m->n_embd == D_ && m->n_head == H_ &&
              m->n_kv_head == KV_ && m->head_dim == HD_ && m->vocab_size == V_ &&
              m->feed_forward_length == FFN_,
              "hparams: layers/dims from tensors, heads from config.json");
        CHECK(m->bos_token_id == 1 && m->eos_token_id == 2, "bos/eos from config.json");
        CHECK(m->rope_freq_base == ROPE_THETA_ && fabsf(m->rms_eps - RMS_EPS_) < 1e-9f,
              "rope_theta + rms_norm_eps (scientific notation) from config.json");
        CHECK(cce_gguf_qwen2_forward(m, tokens, NT, st_logits, V_) == CCE_OK, "st forward ok");

        float dmax = 0;
        for (int v = 0; v < V_; v++) { float d = fabsf(st_logits[v] - gg_logits[v]); if (d > dmax) dmax = d; }
        printf("  max |st - gguf| logit diff: %g\n", (double)dmax);
        CHECK(dmax == 0.0f, "HF safetensors and GGUF paths are bit-identical (mapping exact)");
        cce_gguf_qwen2_free(m);
    }

    /* 3. kv-cache continuity through the new loader (prompt + continue) */
    {
        cce_gguf_qwen2* m = NULL;
        CHECK(cce_st_llama_load(&m, "stll_tmp/model.safetensors") == CCE_OK && m, "hf st reloads");
        if (m) {
            float part[V_];
            CHECK(cce_gguf_qwen2_forward(m, tokens, 2, part, V_) == CCE_OK &&
                  cce_gguf_qwen2_forward(m, tokens + 2, 2, part, V_) == CCE_OK,
                  "two-call forward ok");
            float dmax = 0;
            for (int v = 0; v < V_; v++) { float d = fabsf(part[v] - gg_logits[v]); if (d > dmax) dmax = d; }
            CHECK(dmax == 0.0f, "split calls == batch (kv cache wired correctly)");
            cce_gguf_qwen2_free(m);
        }
    }

    /* 4. universal entry + honest refusal without config.json */
    {
        cce_model_info info;
        CHECK(cce_detect_file("stll_tmp/model.safetensors", &info) == CCE_OK &&
              info.family == CCE_ARCH_FAMILY_LLAMA && info.runnable == 1 &&
              strcmp(info.runner, "cce_st_llama_load") == 0,
              "detect: hf llama st runnable via cce_st_llama_load");

        cce_anymodel* am = NULL;
        CHECK(cce_anymodel_open(&am, "stll_tmp/model.safetensors") == CCE_OK && am && am->transformer,
              "anymodel autoloads hf llama safetensors");
        if (am) cce_anymodel_free(am);

        MKDIR("stll_nocfg");
        st_write("stll_nocfg/model.safetensors", ents, n_st);
        CHECK(cce_detect_file("stll_nocfg/model.safetensors", &info) == CCE_OK && info.runnable == 0,
              "without config.json: detected but honestly not runnable");
        CHECK(strstr(info.notes, "config.json") != NULL, "note names the missing piece");
        cce_gguf_qwen2* m = NULL;
        CHECK(cce_st_llama_load(&m, "stll_nocfg/model.safetensors") != CCE_OK && m == NULL,
              "loader refuses to guess head counts");
        remove("stll_nocfg/model.safetensors");
        remove("stll_nocfg");
    }

    remove("stll_tmp/model.safetensors");
    remove("stll_tmp/config.json");
    remove("stll_tmp");
    remove("stll_test.gguf");
    free(w);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
