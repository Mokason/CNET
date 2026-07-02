/* cce_detect: universal pre-run model structure detection.
 *
 * Verifies: container-format sniffing (gguf / safetensors / cce / packed),
 * structural architecture fingerprinting (separate-qkv vs fused-qkv vs ssm),
 * hparam extraction without loading tensor data, honest runnable verdicts,
 * and the cce_anymodel_open dispatcher (negative path + guarded real files).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/cce/cce_detect.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

/* ---- tiny GGUF writer (metadata only; parser never reads tensor data) ---- */

static void w_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void w_str(FILE* f, const char* s) { w_u64(f, strlen(s)); fwrite(s, 1, strlen(s), f); }

static void w_kv_str(FILE* f, const char* k, const char* v) {
    w_str(f, k); w_u32(f, 8 /*GGUF_TYPE_STRING*/); w_str(f, v);
}
static void w_kv_u32(FILE* f, const char* k, uint32_t v) {
    w_str(f, k); w_u32(f, 4 /*GGUF_TYPE_UINT32*/); w_u32(f, v);
}
static void w_tensor(FILE* f, const char* name, int d0, int d1, uint32_t ggml_type) {
    w_str(f, name);
    w_u32(f, d1 > 0 ? 2 : 1);
    w_u64(f, (uint64_t)d0);
    if (d1 > 0) w_u64(f, (uint64_t)d1);
    w_u32(f, ggml_type);
    w_u64(f, 0); /* data offset; never dereferenced by detection */
}

static void write_gguf_qwen2ish(const char* path) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);        /* version */
    w_u64(f, 11);       /* n tensors */
    w_u64(f, 7);        /* n kv */
    w_kv_str(f, "general.architecture", "qwen2");
    w_kv_u32(f, "qwen2.block_count", 2);
    w_kv_u32(f, "qwen2.embedding_length", 64);
    w_kv_u32(f, "qwen2.attention.head_count", 8);
    w_kv_u32(f, "qwen2.attention.head_count_kv", 2);
    w_kv_u32(f, "qwen2.context_length", 512);
    w_kv_u32(f, "qwen2.feed_forward_length", 128);
    w_tensor(f, "token_embd.weight", 64, 1000, 8 /*Q8_0*/);
    for (int l = 0; l < 2; l++) {
        char n[96];
        snprintf(n, sizeof(n), "blk.%d.attn_q.weight", l);      w_tensor(f, n, 64, 64, 8);
        snprintf(n, sizeof(n), "blk.%d.attn_k.weight", l);      w_tensor(f, n, 64, 16, 8);
        snprintf(n, sizeof(n), "blk.%d.attn_v.weight", l);      w_tensor(f, n, 64, 16, 8);
        snprintf(n, sizeof(n), "blk.%d.attn_output.weight", l); w_tensor(f, n, 64, 64, 8);
        snprintf(n, sizeof(n), "blk.%d.ffn_gate.weight", l);    w_tensor(f, n, 64, 128, 8);
    }
    /* NOTE: no "output.weight" => tied embeddings */
    fclose(f);
}

static void write_gguf_mambaish(const char* path) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 3);
    w_u64(f, 2);
    w_kv_str(f, "general.architecture", "mamba");
    w_kv_u32(f, "mamba.block_count", 4);
    w_tensor(f, "token_embd.weight", 32, 500, 0 /*F32*/);
    w_tensor(f, "blk.0.ssm_in.weight", 32, 64, 0);
    w_tensor(f, "blk.0.ssm_conv1d.weight", 4, 64, 0);
    fclose(f);
}

static void write_gguf_hybrid(const char* path) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 3);
    w_u64(f, 1);
    w_kv_str(f, "general.architecture", "jamba");
    w_tensor(f, "blk.0.attn_q.weight", 32, 32, 0);
    w_tensor(f, "blk.0.attn_output.weight", 32, 32, 0);
    w_tensor(f, "blk.1.ssm_in.weight", 32, 64, 0);
    fclose(f);
}

/* ---- tiny safetensors writer: valid JSON header + zero-filled data ---- */

static void write_safetensors(const char* path, const char* names[], int n_names,
                              const char* emb_name, int emb_vocab, int emb_hidden) {
    char json[8192];
    size_t j = 0;
    uint64_t off = 0;
    j += snprintf(json + j, sizeof(json) - j, "{");
    if (emb_name) {
        uint64_t sz = (uint64_t)emb_vocab * emb_hidden * 4;
        j += snprintf(json + j, sizeof(json) - j,
                      "\"%s\":{\"dtype\":\"F32\",\"shape\":[%d,%d],\"data_offsets\":[%llu,%llu]}",
                      emb_name, emb_vocab, emb_hidden,
                      (unsigned long long)off, (unsigned long long)(off + sz));
        off += sz;
    }
    for (int i = 0; i < n_names; i++) {
        uint64_t sz = 16; /* 2x2 f32 */
        j += snprintf(json + j, sizeof(json) - j,
                      "%s\"%s\":{\"dtype\":\"F32\",\"shape\":[2,2],\"data_offsets\":[%llu,%llu]}",
                      (emb_name || i > 0) ? "," : "", names[i],
                      (unsigned long long)off, (unsigned long long)(off + sz));
        off += sz;
    }
    j += snprintf(json + j, sizeof(json) - j, "}");

    FILE* f = fopen(path, "wb");
    uint64_t hlen = (uint64_t)j;
    fwrite(&hlen, 8, 1, f);
    fwrite(json, 1, j, f);
    /* data section: zero bytes, enough to satisfy every data_offset */
    char* zeros = (char*)calloc(1, (size_t)off);
    fwrite(zeros, 1, (size_t)off, f);
    free(zeros);
    fclose(f);
}

static void write_magic_file(const char* path, const void* bytes, size_t n, size_t pad) {
    FILE* f = fopen(path, "wb");
    fwrite(bytes, 1, n, f);
    char* zeros = (char*)calloc(1, pad);
    fwrite(zeros, 1, pad, f);
    free(zeros);
    fclose(f);
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

int main(void) {
    printf("=== cce_detect: universal pre-run structure detection ===\n");
    cce_model_info info;

    /* 1. gguf, llama-family, full q/k/v/o, tied embeddings */
    write_gguf_qwen2ish("detect_qwen2ish.gguf");
    CHECK(cce_detect_file("detect_qwen2ish.gguf", &info) == CCE_OK, "gguf probe ok");
    CHECK(info.format == CCE_FMT_GGUF, "gguf format detected");
    CHECK(info.family == CCE_ARCH_FAMILY_LLAMA, "gguf separate-qkv => llama family");
    CHECK(strcmp(info.arch, "qwen2") == 0, "declared arch label carried");
    CHECK(info.n_layer == 2 && info.hidden == 64 && info.n_head == 8 && info.n_kv_head == 2,
          "gguf hparams extracted");
    CHECK(info.vocab == 1000, "vocab derived from token_embd shape when kv missing");
    CHECK(info.ctx_len == 512 && info.ffn == 128, "ctx/ffn extracted");
    CHECK(info.tied_embeddings == 1, "missing output.weight => tied");
    CHECK(info.attention_full_qkv == 1, "full q/k/v/o detected");
    CHECK(strcmp(info.dtype, "Q8_0") == 0, "dominant dtype Q8_0");
    CHECK(info.runnable == 1 && strcmp(info.runner, "cce_gguf_load_model") == 0,
          "llama-family gguf is runnable via cce_gguf_load_model");

    /* 2. gguf mamba: detected structurally, routed to the ssm runner */
    write_gguf_mambaish("detect_mamba.gguf");
    CHECK(cce_detect_file("detect_mamba.gguf", &info) == CCE_OK, "mamba gguf probe ok");
    CHECK(info.family == CCE_ARCH_FAMILY_MAMBA, "ssm tensors => mamba family");
    CHECK(info.runnable == 1 && strcmp(info.runner, "cce_ssm_load") == 0,
          "mamba gguf runnable via cce_ssm_load");

    /* 2b. hybrid attention+ssm: neither runner can express it -> refuse */
    write_gguf_hybrid("detect_hybrid.gguf");
    CHECK(cce_detect_file("detect_hybrid.gguf", &info) == CCE_OK, "hybrid gguf probe ok");
    CHECK(info.family == CCE_ARCH_FAMILY_UNKNOWN && info.runnable == 0,
          "hybrid attention+ssm gguf refuses instead of loading truncated");
    remove("detect_hybrid.gguf");

    /* 3. safetensors, HF llama naming */
    {
        const char* names[] = {
            "model.layers.0.self_attn.q_proj.weight", "model.layers.0.self_attn.k_proj.weight",
            "model.layers.0.self_attn.v_proj.weight", "model.layers.0.self_attn.o_proj.weight",
            "model.layers.0.mlp.gate_proj.weight",
            "model.layers.1.self_attn.q_proj.weight", "model.layers.1.self_attn.k_proj.weight",
            "model.layers.1.self_attn.v_proj.weight", "model.layers.1.self_attn.o_proj.weight",
        };
        write_safetensors("detect_hf_llama.safetensors", names, 9,
                          "model.embed_tokens.weight", 100, 16);
        CHECK(cce_detect_file("detect_hf_llama.safetensors", &info) == CCE_OK, "hf st probe ok");
        CHECK(info.format == CCE_FMT_SAFETENSORS, "safetensors format detected");
        CHECK(info.family == CCE_ARCH_FAMILY_LLAMA, "q/k/v/o_proj => llama family");
        CHECK(strcmp(info.naming, "hf-model.layers") == 0, "hf naming fingerprint");
        CHECK(info.n_layer == 2, "layer count from max index");
        CHECK(info.vocab == 100 && info.hidden == 16, "vocab/hidden from embed_tokens");
        CHECK(info.tied_embeddings == 1, "no lm_head => tied");
        /* no config.json next to it: precheck vetoes and says why */
        CHECK(info.runnable == 0, "hf llama st without config.json not runnable");
        CHECK(strstr(info.notes, "config.json") != NULL, "note names the missing config.json");
    }

    /* 4. safetensors, gpt2 naming */
    {
        const char* names[] = {
            "transformer.h.0.attn.c_attn.weight", "transformer.h.0.attn.c_proj.weight",
            "transformer.h.1.attn.c_attn.weight", "lm_head.weight",
        };
        write_safetensors("detect_gpt2.safetensors", names, 4,
                          "transformer.wte.weight", 50, 8);
        CHECK(cce_detect_file("detect_gpt2.safetensors", &info) == CCE_OK, "gpt2 st probe ok");
        CHECK(info.family == CCE_ARCH_FAMILY_GPT2, "c_attn => gpt2 family");
        CHECK(strcmp(info.naming, "gpt2-transformer.h") == 0, "gpt2 naming fingerprint");
        CHECK(info.n_layer == 2, "gpt2 layer count");
        CHECK(info.tied_embeddings == 0, "lm_head present => untied");
    }

    /* 5. safetensors, supra-blocks naming: runnable */
    {
        const char* names[] = {
            "blocks.0.attn.qkv.weight", "blocks.0.attn.proj.weight",
            "blocks.1.attn.qkv.weight", "blocks.1.attn.proj.weight",
            "head.weight",
        };
        write_safetensors("detect_supra.safetensors", names, 5, "tok_emb.weight", 40, 8);
        CHECK(cce_detect_file("detect_supra.safetensors", &info) == CCE_OK, "supra st probe ok");
        CHECK(info.family == CCE_ARCH_FAMILY_GPT2, "fused qkv => gpt2 family");
        CHECK(strcmp(info.naming, "supra-blocks") == 0, "supra naming fingerprint");
        CHECK(info.runnable == 1 && strcmp(info.runner, "cce_supra_a2a_load") == 0,
              "supra-blocks st runnable via cce_supra_a2a_load");
    }

    /* 6. safetensors, mamba naming */
    {
        const char* names[] = {
            "backbone.layers.0.mixer.A_log", "backbone.layers.0.mixer.conv1d.weight",
            "backbone.layers.0.mixer.x_proj.weight", "backbone.layers.1.mixer.A_log",
        };
        write_safetensors("detect_mamba.safetensors", names, 4,
                          "backbone.embedding.weight", 60, 12);
        CHECK(cce_detect_file("detect_mamba.safetensors", &info) == CCE_OK, "mamba st probe ok");
        CHECK(info.family == CCE_ARCH_FAMILY_MAMBA, "A_log/mixer => mamba family");
        CHECK(info.n_layer == 2, "mamba layer count from backbone.layers");
        CHECK(info.runnable == 1 && strcmp(info.runner, "cce_ssm_load") == 0,
              "mamba st runnable via cce_ssm_load");
    }

    /* 7. packed + native formats by magic */
    {
        /* layout: magic, ver, hp[5] */
        FILE* f = fopen("detect_supra.pack", "wb");
        uint32_t m = 0x4B505553u, v = 1;
        int hp5[5] = {4, 128, 384, 4, 4096};
        fwrite(&m, 4, 1, f); fwrite(&v, 4, 1, f); fwrite(hp5, sizeof(int), 5, f);
        fclose(f);
        CHECK(cce_detect_file("detect_supra.pack", &info) == CCE_OK, "supk probe ok");
        CHECK(info.format == CCE_FMT_SUPRA_PACK, "SUPK magic detected");
        CHECK(info.n_layer == 4 && info.hidden == 128 && info.vocab == 4096,
              "supk hparams read from packed header");
        CHECK(info.runnable == 1, "supk runnable");

        f = fopen("detect_qwen2.pack", "wb");
        m = 0x504b4751u; v = 2;
        int hp8[8] = {24, 896, 14, 2, 64, 151936, 32768, 8192};
        fwrite(&m, 4, 1, f); fwrite(&v, 4, 1, f); fwrite(hp8, sizeof(int), 8, f);
        fclose(f);
        CHECK(cce_detect_file("detect_qwen2.pack", &info) == CCE_OK, "qgkp probe ok");
        CHECK(info.format == CCE_FMT_QWEN2_PACK, "QGKP magic detected");
        CHECK(info.n_layer == 24 && info.n_kv_head == 2 && info.vocab == 151936,
              "qgkp hparams read from packed header");

        write_magic_file("detect_forest.cce", "CCE1", 4, 64);
        CHECK(cce_detect_file("detect_forest.cce", &info) == CCE_OK, "cce1 probe ok");
        CHECK(info.format == CCE_FMT_CCE_ARCHIVE && info.family == CCE_ARCH_FAMILY_CCE,
              "CCE1 archive detected");
        CHECK(info.runnable == 0, "bare forest archive honestly not runnable (no branch restore)");

        /* truncated pack: magic only, header unreadable */
        write_magic_file("detect_trunc.pack", "SUPK", 4, 0);
        CHECK(cce_detect_file("detect_trunc.pack", &info) == CCE_OK &&
              info.format == CCE_FMT_SUPRA_PACK && info.runnable == 0,
              "truncated supra-pack detected but not runnable");
        remove("detect_trunc.pack");

        write_magic_file("detect_model.cmdl", "CMDL", 4, 64);
        CHECK(cce_detect_file("detect_model.cmdl", &info) == CCE_OK, "cmdl probe ok");
        CHECK(info.format == CCE_FMT_CMDL, "CMDL detected");
    }

    /* 8. garbage and edge cases */
    {
        write_magic_file("detect_garbage.bin", "\x7f\x45\x4c\x46junkjunk", 12, 32);
        CHECK(cce_detect_file("detect_garbage.bin", &info) == CCE_OK, "garbage probe ok");
        CHECK(info.format == CCE_FMT_UNKNOWN, "garbage => unknown format");
        CHECK(info.runnable == 0, "garbage not runnable");

        CHECK(cce_detect_file("detect_no_such_file.xyz", &info) == CCE_ERR_IO, "missing file => IO error");
        CHECK(cce_detect_file(NULL, &info) == CCE_ERR_INVALID_ARG, "NULL path rejected");

        cce_anymodel* am = NULL;
        CHECK(cce_anymodel_open(&am, "detect_garbage.bin") == CCE_ERR_UNSUPPORTED && am == NULL,
              "anymodel refuses unknown format");
        CHECK(cce_anymodel_open(&am, "detect_gpt2.safetensors") == CCE_ERR_UNSUPPORTED && am == NULL,
              "anymodel refuses structurally-unsupported model instead of mis-running it");
        CHECK(cce_anymodel_open(&am, "detect_mamba.gguf") != CCE_OK && am == NULL,
              "anymodel fails cleanly on an incomplete mamba gguf (no scan tensors)");
    }

    /* 9. real files, guarded (skip silently when absent) */
    if (file_exists("Models/gemma-4-12B-it-MTP-Q8_0.gguf")) {
        CHECK(cce_detect_file("Models/gemma-4-12B-it-MTP-Q8_0.gguf", &info) == CCE_OK,
              "real 12B gguf probed (metadata only)");
        CHECK(info.format == CCE_FMT_GGUF, "real gguf format");
        CHECK(info.n_layer > 0 && info.hidden > 0, "real gguf hparams extracted");
        cce_detect_print(&info, "Models/gemma-4-12B-it-MTP-Q8_0.gguf");
    } else {
        printf("  (skip) real gemma gguf not present\n");
    }

    if (file_exists("supra_cache/model.safetensors")) {
        CHECK(cce_detect_file("supra_cache/model.safetensors", &info) == CCE_OK,
              "real supra safetensors probed");
        CHECK(info.format == CCE_FMT_SAFETENSORS, "real supra format");
        cce_detect_print(&info, "supra_cache/model.safetensors");

        if (info.runnable) {
            cce_anymodel* am = NULL;
            cce_result rc = cce_anymodel_open(&am, "supra_cache/model.safetensors");
            CHECK(rc == CCE_OK && am && am->supra, "anymodel autoloads real supra from bare file path");
            if (am) cce_anymodel_free(am);
        }
    } else {
        printf("  (skip) real supra safetensors not present\n");
    }

    /* cleanup synthetic files */
    remove("detect_qwen2ish.gguf"); remove("detect_mamba.gguf");
    remove("detect_hf_llama.safetensors"); remove("detect_gpt2.safetensors");
    remove("detect_supra.safetensors"); remove("detect_mamba.safetensors");
    remove("detect_supra.pack"); remove("detect_qwen2.pack");
    remove("detect_forest.cce"); remove("detect_model.cmdl");
    remove("detect_garbage.bin");

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
