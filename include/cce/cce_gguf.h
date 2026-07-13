#ifndef CCE_GGUF_H
#define CCE_GGUF_H

/* Minimal GGUF loader for CNet / CCE (Phase 1).
 *
 * Focus: parse standard GGUFv3, extract metadata, load FP32 (or dequant later)
 * tensors into cce_tensor / cce_block / cce_forest the same way as safetensors.
 *
 * This is intentionally narrow:
 * - Supports F32 (and later F16/Q8_0 dequant).
 * - Extracts key hparams (arch, n_layer, hidden_size, n_heads, etc.).
 * - Allows name-based tensor lookup and population into CCE structures.
 * - Does NOT implement full llama.cpp-style GGUF loading or every quant.
 *
 * GGUF spec: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
 */

#include "cce_defs.h"
#include "cce_tensor.h"
#include "cce_block.h"
#include "cce_cascade.h"
#include "cce_forest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_GGUF_MAX_TENSORS 4096
#define CCE_GGUF_MAX_NAME    256
#define CCE_GGUF_MAX_KV      256

typedef enum {
    GGUF_TYPE_UINT8   = 0,
    GGUF_TYPE_INT8    = 1,
    GGUF_TYPE_UINT16  = 2,
    GGUF_TYPE_INT16   = 3,
    GGUF_TYPE_UINT32  = 4,
    GGUF_TYPE_INT32   = 5,
    GGUF_TYPE_FLOAT32 = 6,
    GGUF_TYPE_BOOL    = 7,
    GGUF_TYPE_STRING  = 8,
    GGUF_TYPE_ARRAY   = 9,
    GGUF_TYPE_UINT64  = 10,
    GGUF_TYPE_INT64   = 11,
    GGUF_TYPE_FLOAT64 = 12,
} gguf_value_type;

typedef struct {
    char     name[CCE_GGUF_MAX_NAME];
    int      shape[CCE_MAX_DIMS];
    int      ndim;
    uint32_t ggml_type;     /* raw ggml type from file */
    uint64_t data_offset;   /* absolute offset in file for the tensor data */
    uint64_t nbytes;        /* size of this tensor's data in file */
} cce_gguf_tensor_meta;

typedef struct cce_gguf cce_gguf; /* opaque */

/* Load and validate GGUF file (header + tensor table first).
 * Does basic sanity checks. For f32 files, tensors are directly usable.
 * Returns CCE_OK on success. Caller owns the handle.
 */
cce_result cce_gguf_load(const char* path, cce_gguf** out);

/* Free resources. */
void cce_gguf_free(cce_gguf* gguf);

/* Basic queries */
int cce_gguf_tensor_count(const cce_gguf* gguf);
int cce_gguf_find_tensor(const cce_gguf* gguf, const char* name); /* -1 if not found */
cce_result cce_gguf_get_tensor_meta(const cce_gguf* gguf, int idx, cce_gguf_tensor_meta* out);

/* Load a tensor as F32 cce_tensor (converts if needed; for pure f32.gguf it's copy).
 * Caller must free the tensor if owns_memory.
 */
cce_result cce_gguf_load_as_tensor(const cce_gguf* gguf, int idx, cce_tensor* out);

/* Convenience: load by name */
cce_result cce_gguf_load_tensor_by_name(const cce_gguf* gguf, const char* name, cce_tensor* out);

/* Load raw F32 data (dequantizing if needed) into a caller buffer. */
cce_result cce_gguf_load_f32(const cce_gguf* gguf, int idx, float* buf, size_t cap_elems);

/* Direct pointer to a tensor's raw (still-quantized) bytes inside the mmap —
   the single-hop DMA source for the resident-quantized VRAM forward. Only
   valid under CNET_GGUF_MMAP=1; returns CCE_ERR_UNSUPPORTED otherwise. */
cce_result cce_gguf_tensor_bytes(const cce_gguf* gguf, int idx,
                                 const void** ptr, size_t* nbytes);

/* Extract common metadata (best effort, returns defaults on missing) */
const char* cce_gguf_get_arch(const cce_gguf* gguf);      /* "qwen2", "llama", etc. */
int cce_gguf_get_n_layer(const cce_gguf* gguf);
int cce_gguf_get_hidden_size(const cce_gguf* gguf);
int cce_gguf_get_n_heads(const cce_gguf* gguf);
int cce_gguf_get_n_kv_heads(const cce_gguf* gguf); /* may be same as n_heads */
int cce_gguf_get_vocab_size(const cce_gguf* gguf);
int cce_gguf_get_context_length(const cce_gguf* gguf);
int cce_gguf_get_feed_forward_length(const cce_gguf* gguf);
float cce_gguf_get_rope_freq_base(const cce_gguf* gguf); /* 0 if absent in metadata */
float cce_gguf_get_rms_eps(const cce_gguf* gguf);        /* 0 if absent in metadata */

/* Tokenizer data embedded from GGUF metadata (Phase 4) */
const char* cce_gguf_get_tokenizer_model(const cce_gguf* gguf);
int cce_gguf_get_bos_token_id(const cce_gguf* gguf);
int cce_gguf_get_eos_token_id(const cce_gguf* gguf);

/* ---- Population helpers, modeled after safetensors ---- */

/* Populate a cce_block from GGUF tensor names (weight + optional bias).
 * Handles common transpose conventions (GGUF for HF models often [out,in] like torch).
 */
cce_result cce_gguf_populate_block(cce_block* blk,
                                   const cce_gguf* gguf,
                                   const char* weight_name,
                                   const char* bias_name,
                                   int transpose_weight);

/* Add a linear specialist branch to a forest from GGUF, same naming pattern as Supra.
 * Useful for building qkv / attn_proj / mlp_* cascades per layer.
 */
int cce_gguf_add_linear_branch(cce_forest* forest,
                               const cce_gguf* gguf,
                               const char* weight_name,
                               const char* bias_name,
                               const char* branch_name,
                               float init_scale_fallback);

/* Higher level: attempt to build a basic Qwen2 / Llama-style specialist forest
 * from the GGUF. Looks for standard names and creates one cascade per block.
 * Returns the forest on success (caller must close). This is the "convert same as Supra" part.
 */
cce_result cce_gguf_build_qwen2_forest(cce_forest** out_forest,
                                       const cce_gguf* gguf,
                                       float init_scale);

/* Full Qwen2 model holder with forest + norms (for forward) */
struct cce_clgemm;

typedef struct cce_gguf_qwen2 {
    cce_forest* forest;

    cce_tensor* attn_norm;
    cce_tensor* ffn_norm;

    cce_tensor tok_emb;
    cce_tensor output_norm;
    cce_tensor output;

    /* MTP (Multi-Token Prediction / nextn) projections for gemma4-assistant etc. */
    cce_tensor mtp_pre;
    cce_tensor mtp_post;

    /* Extra per-layer tensors for non-standard Gemma4-assistant style */
    cce_tensor* attn_q_norm;
    cce_tensor* post_attention_norm;
    cce_tensor* post_ffw_norm;
    cce_tensor* layer_output_scale;

    /* Custom RoPE if present */
    cce_tensor rope_freqs;

    int n_layer;
    int n_embd;
    int n_head;
    int n_kv_head;
    int head_dim;
    int vocab_size;
    int ctx_len;
    int feed_forward_length;

    /* Numerics config/metadata may override; 0 = defaults (10000 / 1e-6),
       kept as fallback for callers that calloc the struct directly. */
    float rope_freq_base;
    float rms_eps;

    /* Embedded tokenizer from GGUF (Phase 4) */
    char tokenizer_model[64];
    int bos_token_id;
    int eos_token_id;

    float *k_cache;
    float *v_cache;
    int cur_pos;
    int probe_batch;   /* internal: forward treats the n_tokens rows as
                          INDEPENDENT single-token probes at cur_pos (same
                          position, attend prefix+self only, no KV writes,
                          per-row logits). Set via
                          cce_gguf_qwen2_forward_probes, never directly. */
    int max_ctx;

    /* Optional per-instance GPU handle (overrides the process-global one
       from cce_gguf_set_clgemm). Required when several instances forward
       concurrently — a shared handle means shared queues, i.e. races. */
    struct cce_clgemm *clgemm;

    /* This instance's forest scratch archive. Unique per load so oracle-pool
       lanes in one process never remove()/rewrite each other's live backing
       file; "" = legacy fixed-name cleanup. */
    char forest_scratch[160];

    /* > 0: forward stops after this many layers (head reads the current
       stream). A capped forward is a DIFFERENT function from the full model:
       enable ONLY behind a decision-equivalence gate (same posture as the
       GPU path). 0 (default) = full depth, byte-identical. */
    int layer_cap;

    /* Restricted head: when head_window_n > 0, the forward computes ONLY the
       logits for these token ids (written at logits_out[ids[i]]) instead of
       the full [D x vocab] head GEMM. The mining oracle reads only a small
       window, so this drops ~1/4 of every mining forward's cost — and the
       computed values are BIT-IDENTICAL to the full head's (same per-column
       dot, same k-ascending order). Startup gates keep the full head. */
    const int *head_window;
    int head_window_n;

    /* Per-layer attention geometry, derived from TENSOR SHAPES + metadata
       at load and validated (indivisible head counts, o_proj width
       mismatches, norm-shape mismatches all REFUSE the load). NULL only
       for structs calloc'd outside the loaders (legacy paths refuse to
       forward without it). */
    struct cce_attn_geom {
        int q_dim, k_dim, v_dim;   /* projection widths from the tensors */
        int head_dim;              /* q/k head width per layer type (swa vs global) */
        int v_head_dim;            /* v head width (value_length; == head_dim in gemma4) */
        int n_q, n_k, n_v;         /* head counts; K and V may differ (MQA/GQA) */
        int v_tied;                /* no attn_v tensor: V = raw K projection
                                      (one shared head; inference from
                                      o_proj/value_length shapes) */
        int swa;                   /* sliding-window layer? */
        int window;                /* swa window size, 0 = unlimited */
        float rope_base;           /* per layer type */
        int rope_dim;              /* dims rotated (== head_dim in gemma4) */
        size_t k_off, v_off;       /* float offsets within a position's k/v slot */
    } *geom;
    size_t k_slot_floats, v_slot_floats; /* per-position slot widths, all layers */
    float embed_scale;             /* gemma-family sqrt(D); 1.0 otherwise */
    float final_softcap;           /* logits = c*tanh(l/c); 0 = off (monotonic: cannot change decisions) */
    cce_tensor* attn_k_norm;       /* per-layer k-norm (may be absent) */
    int ffn_gelu;                  /* gemma family: GeGLU = gelu_tanh(gate)*up (qwen/llama: silu) */
    int gemma4_attn;               /* gemma4(+assistant): kq_scale 1.0 (no 1/sqrt(d)),
                                      plain weightless RMSNorm on V, rope freq
                                      factors on global layers, layer_output_scale
                                      applied to the whole stream at layer end */
    int64_t suppress_ids[256];     /* tokenizer.ggml.suppress_tokens: ids the
                                      checkpoint must never emit; logits forced
                                      to -inf, mirroring the reference head */
    size_t n_suppress;
} cce_gguf_qwen2;

/* The live GGUF fp16->fp32 decoder (all quant superblock scales flow through
 * it). Exported so the exhaustive identity test exercises the REAL code. */
float cce_gguf_f16_to_f32(uint16_t h);

/* Load a full Qwen2 model from GGUF into decomposed CCE form (forest of specialists + norms).
 * Similar to cce_supra_load_decomposed.
 */
cce_result cce_gguf_load_qwen2(cce_gguf_qwen2** out, const char* path);

/* Phase 4: architecture dispatcher. Detects arch from GGUF metadata and builds
 * the appropriate forest. Falls back to qwen2/llama-style builder for compatible models.
 */
cce_result cce_gguf_load_model(cce_gguf_qwen2** out, const char* path);

/* Free the model */
void cce_gguf_qwen2_free(cce_gguf_qwen2* m);

/* Forward for one token or sequence (basic, for small tests) */
cce_result cce_gguf_qwen2_forward(cce_gguf_qwen2* m, const int* tokens, int n_tokens, float* logits_out, int logits_cap);

/* Batched probes: run n_probes INDEPENDENT single-token continuations of
   the pinned prefix (rows share position cur_pos, each attends to the
   prefix + itself only; the KV cache is not modified). Row b's logits land
   at logits_out + b*logits_cap. Bit-identical per row to n_probes serial
   forward calls — every projection is row-independent — while the GEMMs
   run n_probes-wide instead of as GEMVs. cur_pos is left unchanged. */
CCE_API cce_result cce_gguf_qwen2_forward_probes(cce_gguf_qwen2* m,
    const int* probe_tokens, int n_probes, float* logits_out, int logits_cap);

/* Optional GPU acceleration for the forward's linear seam (see
 * cce_clgemm.h). NULL (the default) = CPU path, byte-for-byte unchanged.
 * The process-global default serves the one-model-per-process shape; the
 * per-instance setter overrides it for oracle POOLS (one model per GPU,
 * forwarding concurrently — each instance MUST have its own handle). */
void cce_gguf_set_clgemm(struct cce_clgemm *h);
void cce_gguf_qwen2_set_clgemm(cce_gguf_qwen2 *m, struct cce_clgemm *h);

/* Restrict the forward's head to `n` token ids (bit-identical to the full
   head on those ids; ~1/4 less work per forward). NULL/0 = full head. The
   ids array must outlive the model; the campaign's discovered window fits. */
void cce_gguf_qwen2_set_head_window(cce_gguf_qwen2 *m, const int *ids, int n);

/* Uniform-geometry synthesizer for models populated OUTSIDE the GGUF
 * loader (safetensors/llama path, packed-.cce reader): derives one geometry
 * for every layer from the scalar hparams already in the struct. Returns
 * CCE_OK or refuses on inconsistency. */
cce_result cce_gguf_qwen2_geom_uniform(cce_gguf_qwen2 *m);

/* Per-layer numeric metadata list by key suffix; returns count (0=absent). */
size_t cce_gguf_get_int_array(const cce_gguf* gguf, const char* key_suffix,
                              int64_t* out, size_t cap);

/* Depth instrumentation: tap called after every layer of the forward with
 * the residual stream (probe tooling; NULL = off, zero cost). */
void cce_gguf_set_layer_tap(void (*fn)(int layer, const float *x,
                                       int n_tokens, int dim, void *uctx),
                            void *uctx);

/* OPT-IN per-specialist activation capture (Arc A2: data-aware quantization
 * at ingest). When a hook is registered, the forward invokes it IMMEDIATELY
 * BEFORE applying each linear specialist, passing the specialist's branch name
 * (e.g. "qwen2.blk.3.gate_proj", "qwen2.lm_head"), the input activation rows
 * that are about to be matmul'd, the row count, and the input dimension. Rows
 * are contiguous [n_rows][in_dim] (the exact buffer fed to the matvec).
 *
 * DEFAULT OFF: with fn==NULL the forward is byte-for-byte unchanged (one NULL
 * test per specialist per layer, zero other cost). The hook observes only; it
 * must not mutate the rows. Set fn=NULL to detach. Not thread-safe against a
 * concurrent forward (a calibration pass is single-threaded by construction). */
void cce_gguf_set_capture_hook(void (*fn)(const char *spec_name,
                                          const float *rows, int n_rows,
                                          int in_dim, void *uctx),
                               void *uctx);

/* Quantize all linear specialists (q/k/v/o/gate/up/down/head) to int8 PTQ.
 * Returns number of blocks quantized, or -1 on error. Mirrors cce_supra_quantize_int8.
 * After this, forward will use the int8 weight-only path.
 */
int cce_gguf_qwen2_quantize_int8(cce_gguf_qwen2* m);

/* Quantize all linear specialists to ternary {-1,0,1} (BitNet-style per-output absmean).
 * Returns number of blocks quantized. Mirrors cce_supra_quantize_ternary.
 * This is the step before pack_trits for the 1.6-bit format.
 */
int cce_gguf_qwen2_quantize_ternary(cce_gguf_qwen2* m);

/* Pack the current (quantized) model specialists to 1.6-bit trit (like supra).
 * If not yet ternary-quantized, it will quantize on the fly (for convenience).
 */
cce_result cce_gguf_qwen2_pack_trits(cce_gguf_qwen2* m);

/* Export packed 1.6-bit artifact */
cce_result cce_gguf_qwen2_export_packed(cce_gguf_qwen2* m, const char* path);

/* Load packed 1.6-bit version */
cce_result cce_gguf_qwen2_load_packed(cce_gguf_qwen2** out, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* CCE_GGUF_H */