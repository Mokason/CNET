#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_model_catalog.h"
#include "../include/cce/cce_qgkp.h"

static int checks;
static int failures;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", msg); } \
} while (0)

static void w_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void w_str(FILE *f, const char *s) {
    w_u64(f, (uint64_t)strlen(s));
    fwrite(s, 1, strlen(s), f);
}
static void w_kv_str(FILE *f, const char *key, const char *value) {
    w_str(f, key); w_u32(f, 8); w_str(f, value);
}
static void w_kv_u32(FILE *f, const char *key, uint32_t value) {
    w_str(f, key); w_u32(f, 4); w_u32(f, value);
}
static void w_tensor(FILE *f, const char *name, uint32_t type) {
    w_str(f, name);
    w_u32(f, 2);
    w_u64(f, 64);
    w_u64(f, 64);
    w_u32(f, type);
    w_u64(f, 0);
}

static void write_fixture(const char *path, int moe) {
    FILE *f = fopen(path, "wb");
    uint64_t tensors = moe ? 7 : 6;
    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, tensors);
    w_u64(f, 4);
    w_kv_str(f, "general.architecture", moe ? "mixtral" : "qwen2");
    w_kv_u32(f, moe ? "mixtral.block_count" : "qwen2.block_count", 1);
    w_kv_u32(f, moe ? "mixtral.context_length" : "qwen2.context_length", 4096);
    w_kv_u32(f, moe ? "mixtral.rope.scaling.original_context_length" :
                      "qwen2.rope.scaling.original_context_length", 1024);
    w_tensor(f, "token_embd.weight", 8);
    w_tensor(f, "blk.0.attn_q.weight", 8);
    w_tensor(f, "blk.0.attn_k.weight", 8);
    w_tensor(f, "blk.0.attn_v.weight", 8);
    w_tensor(f, "blk.0.attn_output.weight", 8);
    w_tensor(f, moe ? "blk.0.ffn_gate_inp.weight" : "blk.0.ffn_gate.weight", 8);
    if (moe) w_tensor(f, "blk.0.ffn_up_exps.weight", 8);
    fclose(f);
}

int main(void) {
    CnetModelDescriptor dense;
    CnetModelDescriptor moe;
    cce_model_info info;
    const char *real_dense_path;
    const char *real_moe_path;

    write_fixture("catalog_dense.gguf", 0);
    write_fixture("catalog_moe.gguf", 1);

    cce_qgkp_metadata qgkp_meta;
    memset(&qgkp_meta, 0, sizeof qgkp_meta);
    snprintf(qgkp_meta.architecture, sizeof qgkp_meta.architecture, "qwen2");
    snprintf(qgkp_meta.quantization, sizeof qgkp_meta.quantization, "TQ1_0+Q4_K");
    qgkp_meta.n_layer = 1;
    qgkp_meta.hidden = 64;
    qgkp_meta.context_length = 4096;
    qgkp_meta.flags = CCE_QGKP_FLAG_PACKET_TRITS | CCE_QGKP_FLAG_MIXED_QUANT;
    CHECK(cce_qgkp_pack_gguf("catalog_dense.gguf", "catalog_dense.qgkp", &qgkp_meta) == CCE_OK,
          "dense GGUF wraps into QGKP v3");

    CHECK(cce_model_descriptor_probe(&dense, &info,
                                     "dense-local", "catalog_dense.gguf", "llama.cpp",
                                     CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                                     CNET_MODEL_RESOURCE_GPU1, 1, 20, 2, 64) == CCE_OK,
          "dense GGUF descriptor probes");
    CHECK(dense.model_class == CNET_MODEL_CLASS_DENSE_TRANSFORMER &&
          strcmp(dense.architecture, "qwen2") == 0 &&
          strcmp(dense.backend_name, "llama.cpp") == 0,
          "dense descriptor preserves architecture and backend boundary");
    CHECK(dense.required_resource_count == 1 &&
          dense.allowed_resource_mask ==
              (CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1) &&
          dense.preferred_resource_mask == CNET_MODEL_RESOURCE_GPU1,
          "dense descriptor can use either single R9700");
    CHECK(dense.artifact_bytes > 0 && dense.resident_bytes_per_resource == 20 &&
          dense.workspace_bytes == 2 && dense.kv_bytes_per_token == 64 &&
          dense.context_limit == 4096,
          "dense descriptor carries memory and context accounting");

    CHECK(cce_model_descriptor_probe(&dense, &info,
                                     "dense-qgkp", "catalog_dense.qgkp", "llama.cpp",
                                     CNET_MODEL_RESOURCE_GPU1, CNET_MODEL_RESOURCE_GPU1,
                                     1, 0, 0, 0) == CCE_OK,
          "QGKP v3 descriptor probes header-first");
    CHECK(dense.model_class == CNET_MODEL_CLASS_DENSE_TRANSFORMER &&
          strcmp(dense.architecture, "qwen2") == 0 &&
          strcmp(dense.format, "qwen2-pack") == 0 &&
          strcmp(dense.quantization, "TQ1_0+Q4_K") == 0,
          "QGKP v3 preserves embedded GGUF model identity");
    CHECK(info.runnable == 0 && info.runner[0] == '\0',
          "QGKP v3 does not falsely claim the legacy native Qwen2 runner");

    CHECK(cce_model_descriptor_probe(&moe, &info,
                                     "moe-local", "catalog_moe.gguf", "ds4",
                                     CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                                     CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                                     2, 12, 1, 128) == CCE_OK,
          "MoE GGUF descriptor probes");
    CHECK(moe.model_class == CNET_MODEL_CLASS_MOE &&
          strcmp(moe.architecture, "mixtral") == 0 &&
          moe.required_resource_count == 2,
          "MoE descriptor requires a distributed resource shape");
    CHECK(cce_model_descriptor_probe(&moe, &info,
                                     "missing", "catalog_missing.gguf", "cce",
                                     CNET_MODEL_RESOURCE_CPU, CNET_MODEL_RESOURCE_CPU,
                                     1, 1, 0, 0) == CCE_ERR_IO,
          "missing artifact refuses cleanly");
    CHECK(cce_model_descriptor_probe(&moe, &info,
                                     "bad-shape", "catalog_moe.gguf", "ds4",
                                     CNET_MODEL_RESOURCE_GPU0,
                                     CNET_MODEL_RESOURCE_GPU0,
                                     2, 12, 1, 128) == CCE_ERR_INVALID_ARG,
          "descriptor refuses an impossible resource shape");

    real_dense_path = getenv("CNET_TEST_DENSE_GGUF");
    if (real_dense_path && real_dense_path[0] != '\0') {
        CHECK(cce_model_descriptor_probe(
                  &dense, &info, "real-dense", real_dense_path, "llama.cpp",
                  CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                  CNET_MODEL_RESOURCE_GPU0, 1, 0, 0, 0) == CCE_OK,
              "real local dense GGUF probes header-first");
        CHECK(dense.model_class == CNET_MODEL_CLASS_DENSE_TRANSFORMER &&
              info.is_moe == 0 && dense.artifact_bytes > 0 &&
              strcmp(dense.format, "gguf") == 0,
              "real local dense GGUF has structural dense evidence");
        printf("MODEL_CATALOG_REAL_DENSE arch=%s quant=%s bytes=%llu\n",
               dense.architecture, dense.quantization,
               (unsigned long long)dense.artifact_bytes);
    }

    real_moe_path = getenv("CNET_TEST_MOE_GGUF");
    if (real_moe_path && real_moe_path[0] != '\0') {
        CHECK(cce_model_descriptor_probe(
                  &moe, &info, "real-moe", real_moe_path, "ds4",
                  CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                  CNET_MODEL_RESOURCE_GPU0 | CNET_MODEL_RESOURCE_GPU1,
                  2, 0, 0, 0) == CCE_OK,
              "real local MoE GGUF probes header-first");
        CHECK(moe.model_class == CNET_MODEL_CLASS_MOE && info.is_moe == 1 &&
              moe.artifact_bytes > 0 && strcmp(moe.format, "gguf") == 0,
              "real local MoE GGUF has structural expert evidence");
        printf("MODEL_CATALOG_REAL_MOE arch=%s quant=%s bytes=%llu\n",
               moe.architecture, moe.quantization,
               (unsigned long long)moe.artifact_bytes);
    }

    remove("catalog_dense.gguf");
    remove("catalog_dense.qgkp");
    remove("catalog_moe.gguf");

    if (failures) {
        fprintf(stderr, "MODEL_CATALOG_FAIL checks=%d failures=%d\n", checks, failures);
        return 1;
    }
    printf("MODEL_CATALOG_PASS checks=%d\n", checks);
    return 0;
}
