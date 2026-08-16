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

/* MoE hparams (0 when absent => dense checkpoint) */
int cce_gguf_get_expert_count(const cce_gguf* gguf);
int cce_gguf_get_expert_used_count(const cce_gguf* gguf);
int cce_gguf_get_expert_feed_forward_length(const cce_gguf* gguf);

/* Dequantize a BLOCK-ALIGNED element range of tensor idx into buf — slices
 * one expert out of a 3D [ne0, ne1, n_expert] bank without materializing the
 * whole tensor. elem_off must be a multiple of the quant block (32 for
 * Q8_0/Q4_0/..., 256 for K-quants); n_elems too, unless the range ends the
 * tensor. Supports every type cce_gguf_load_f32 supports. */
cce_result cce_gguf_load_f32_slice(const cce_gguf* gguf, int idx, size_t elem_off,
                                   size_t n_elems, float* buf);

/* ---- MoE loader (Arc B1): per-expert streamable specialists ----
 *
 * Canonical llama.cpp MoE layout (qwen2moe/qwen3moe/mixtral):
 *   blk.N.ffn_{gate,up}_exps.weight [n_embd, ff_exp, n_expert]
 *   blk.N.ffn_down_exps.weight      [ff_exp, n_embd, n_expert]
 *   blk.N.ffn_gate_inp.weight       [n_embd, n_expert]   (the router)
 * gemma4 fuses gate+up ([n_embd, 2*ff_exp, n_expert]) and adds side scales;
 * both layouts parse. Dims are validated against metadata — a mismatch
 * refuses to open (a mis-dimensioned expert must never forward). */
typedef struct cce_gguf_moe {
    cce_gguf* g;               /* owned container handle */
    char arch[32];
    int n_layer, n_embd;
    int n_ff_shared;           /* dense/shared-expert FFN width (0 if absent) */
    int n_expert, n_expert_used, n_ff_exp;
    int fused_gate_up;         /* 1 = gemma4 ffn_gate_up_exps layout */
    /* per-layer tensor indices into g (-1 = absent) */
    int* t_gate;               /* gate bank (the fused gate_up bank when fused) */
    int* t_up;                 /* up bank (-1 when fused) */
    int* t_down;
    int* t_router;
    int* t_down_scale;         /* gemma4: per-EXPERT scalar on the down output (llama.cpp build_lora_mm_id w_s) */
    int* t_router_scale;       /* gemma4: per-CHANNEL scale on the router input (ffn_gate_inp_s) */
    int* t_pre_norm;           /* gemma4: expert-input RMS norm weight (pre_ffw_norm_2) */
} cce_gguf_moe;

cce_result cce_gguf_moe_open(const char* path, cce_gguf_moe** out);
void cce_gguf_moe_free(cce_gguf_moe* m);

/* One expert's FFN as a standalone 3-block cascade [gate, up, down], weights
 * in CCE [in][out] layout, no bias. Peak RAM = one expert (block-aligned
 * slice reads). A streamable specialist: weight-store put/get round-trips
 * it bit-exact. Caller owns the cascade. */
cce_result cce_gguf_moe_load_expert(const cce_gguf_moe* m, int layer, int expert,
                                    cce_cascade** out);

/* The layer's router as a 1-block cascade [n_embd -> n_expert]. */
cce_result cce_gguf_moe_load_router(const cce_gguf_moe* m, int layer, cce_cascade** out);

struct cce_weight_store; /* cce_weight_store.h; the MoE runtime can stream from one */

/* ---- MoE forward runtime (Arc B2): routed, demand-loaded expert FFN ----
 *
 * Routers stay resident (tiny); expert branches start COLD in the forest and
 * are demand-loaded from the gguf when routed, LRU-evicted past hot_cap —
 * peak expert RAM = hot_cap experts, not the bank.
 *
 * Conventions are pinned against llama.cpp's build_moe_ffn + per-arch call
 * sites (the B2 reference):
 *   qwen3moe-style: logits = router . x; probs = softmax over ALL experts;
 *     top-k by prob; selected weights renormalized to sum 1; SwiGLU experts
 *     (silu(gate.x) * up.x -> down).
 *   gemma4: router input = rms_norm(x) * 1/sqrt(n_embd) * ffn_gate_inp_s
 *     (x = attn_out); expert input = rms_norm(x) * pre_ffw_norm_2; GEGLU
 *     (gelu_tanh); fused bank rows [0,F) = gate, [F,2F) = up; expert e's down
 *     output is multiplied by ffn_down_exps.scale[e]; same softmax/top-k/
 *     renorm. The output is the combined expert sum (the caller owns
 *     post_ffw norms / shared-expert add / residual). */
typedef struct cce_gguf_moe_rt {
    cce_gguf_moe* m;           /* borrowed */
    cce_forest* forest;        /* owned: routers resident, experts cold-until-routed */
    int hot_cap;
    /* gemma4 per-layer side vectors (NULL when absent) */
    float** gate_inp_scale;    /* [n_layer][n_embd] */
    float** down_scale;        /* [n_layer][n_expert] */
    float** pre_norm;          /* [n_layer][n_embd] */
    /* the most recent routing decision (gates + router-lookahead) */
    int   last_k;
    int   last_experts[64];
    float last_weights[64];
    float* last_logits;        /* [n_expert] router logits of the last forward */

    /* ---- B3: streaming throughput layer ----
     * store-backed payloads: experts ingest into the content-addressed store
     * on first touch (optionally int8, ~4x smaller) and re-stream from it
     * (RAW_QUANT: the expert matvec runs w_q directly, no dequant). */
    struct cce_weight_store* store;  /* borrowed; NULL = always load from the gguf */
    int store_mode;            /* CCE_MOE_STORE_* payload mode */
    uint64_t* digests;         /* [n_layer*n_expert], 0 = not ingested yet */
    int gguf_loads;            /* full expert loads (slice + dequant + transpose) */
    int store_hits;            /* cheap re-streams from the store */
    /* learned routing frequency (pin_hot's signal) + pinning */
    int* route_count;          /* [n_layer*n_expert] times selected by the router */
    int pinned;
    /* async lookahead prefetch (opaque worker state; NULL = off) */
    void* la;
    int prefetch_hits;         /* staged payload adopted with no waiting */
    int prefetch_waits;        /* adopted after waiting on an in-flight fetch */
    int sync_fetches;          /* forward thread fetched synchronously */
    double stall_sec;          /* forward-thread time blocked on expert fetches */

    /* ---- B4: per-expert calibration on ROUTED activations ----
     * While collecting, every routed token's expert input (xe) is appended
     * to that expert's sample buffer; the data-aware store modes quantize
     * each expert against ITS OWN routed traffic at ingest. */
    float** calib;             /* [n_layer*n_expert] -> [calib_cap x n_embd] */
    int* calib_n;
    int calib_cap;             /* buffer capacity (fixed at first enable) */
    int calib_on;              /* 1 = collecting */
} cce_gguf_moe_rt;

/* store payload modes (attach_store). The int4 modes apply the PROVEN
 * bit-width policy: gate/up at int4 (packed 2/byte by the store), down at
 * int8 — naive ternary/int4 on the wide down-proj is where quality dies. */
#define CCE_MOE_STORE_FP       0
#define CCE_MOE_STORE_INT8     1   /* naive per-column absmax int8 */
#define CCE_MOE_STORE_INT8_DA  2   /* data-aware int8 (routed-activation OBQ) */
#define CCE_MOE_STORE_INT4     3   /* naive int4 gate/up + naive int8 down */
#define CCE_MOE_STORE_INT4_DA  4   /* data-aware int4 gate/up + data-aware int8 down */

cce_result cce_gguf_moe_rt_open(cce_gguf_moe* m, const char* forest_archive_path,
                                int hot_cap, cce_gguf_moe_rt** out);
void cce_gguf_moe_rt_free(cce_gguf_moe_rt* rt);

/* One MoE FFN layer on one token: route x, demand-load the top-k experts,
 * run each, weight-combine into out[n_embd]. x is the layer's MoE input
 * (qwen3moe: the ffn-normed hidden; gemma4: attn_out). */
cce_result cce_gguf_moe_ffn_forward(cce_gguf_moe_rt* rt, int layer,
                                    const float* x, float* out);

/* ---- B3: streaming throughput ---- */

/* Attach a weight store: experts ingest on first touch (quantized per
 * `mode`, see CCE_MOE_STORE_*; the forward runs the codes directly) and
 * re-stream from the store afterwards. Switching store or mode resets the
 * ingest map (experts re-ingest on next touch). NULL detaches. */
cce_result cce_gguf_moe_rt_attach_store(cce_gguf_moe_rt* rt, struct cce_weight_store* s,
                                        int mode);

/* B4: collect per-expert calibration samples (each routed token's expert
 * input) up to per_expert_cap per expert. 0 pauses collection (samples are
 * kept for the data-aware store modes); the cap is fixed at first enable. */
cce_result cce_gguf_moe_rt_set_calibration(cce_gguf_moe_rt* rt, int per_expert_cap);
int cce_gguf_moe_rt_calib_samples(const cce_gguf_moe_rt* rt, int layer, int expert);

/* Async lookahead: a background worker prefetches routed experts while the
 * forward computes. Intra-layer is automatic (the whole top-k queues before
 * the first expert runs); cross-layer via _hint. CCE_ERR_UNSUPPORTED on
 * builds without threads (streaming stays synchronous, still correct). */
cce_result cce_gguf_moe_rt_set_lookahead(cce_gguf_moe_rt* rt, int on);

/* Cross-layer speculation: route x through `layer`'s router and prefetch the
 * predicted experts. x is the caller's best estimate of that layer's input
 * (e.g. the current residual stream); a misprediction costs only a wasted
 * prefetch. Does not perturb route_count. */
cce_result cce_gguf_moe_rt_lookahead_hint(cce_gguf_moe_rt* rt, int layer, const float* x);

/* Pin the n most-ROUTED experts resident (learned frequency pinning): they
 * leave the LRU pool and are never evicted. RAM = hot_cap + pinned. */
cce_result cce_gguf_moe_rt_pin_hot(cce_gguf_moe_rt* rt, int n);

/* ---- gemma4 single-token full-stack forward (end-to-end parity) ----
 *
 * The complete gemma4 stack for ONE token at position 0, where attention is
 * EXACT with no kv/rope/window machinery (softmax over one score is 1, so
 * attention = o_proj(repeat_gqa(rms_per_head(v)))). The dense stack loads
 * resident (~7 GB fp32 on the 26B); experts stream through the MoE runtime.
 * Gated against llama.cpp per-layer checkpoints + final logits. */
typedef struct cce_gemma4_stack cce_gemma4_stack;
cce_result cce_gemma4_stack_open(cce_gguf_moe* m, cce_gemma4_stack** out);
void cce_gemma4_stack_free(cce_gemma4_stack* st);
/* logits[vocab] for `token` at position 0; l_out_dbg (optional, [n_layer x
 * n_embd]) captures each layer's output for parity bisection. */
cce_result cce_gemma4_token_logits(cce_gemma4_stack* st, cce_gguf_moe_rt* rt,
                                   int token, float* logits, float* l_out_dbg);

/* Multi-token: decode ONE token at the next position (real NEOX rope, QK
 * norms, causal attention over a per-layer kv cache; scores unscaled per
 * gemma4). Contexts are clamped to the sliding window (1024 on the 26B) —
 * beyond it swa != full causal and decode refuses rather than drift.
 * logits (optional) are softcapped + suppress-biased; l_out_dbg (optional,
 * [n_layer x n_embd]) captures the ladder for parity bisection. */
cce_result cce_gemma4_decode(cce_gemma4_stack* st, cce_gguf_moe_rt* rt,
                             int token, float* logits, float* l_out_dbg);
void cce_gemma4_reset(cce_gemma4_stack* st);

/* Batch-union prefill: route all n_tokens first, load each unique expert
 * ONCE, apply it to every token that selected it. x/out are [n_tokens x
 * n_embd]. Bit-identical to n_tokens single-token forwards (same per-expert
 * math, same per-token accumulation order); fetches = |union of selections|.
 * last_* reflect the final token. */
cce_result cce_gguf_moe_ffn_forward_batch(cce_gguf_moe_rt* rt, int layer,
                                          const float* x, float* out, int n_tokens);

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
struct cce_hipgemm;
struct cce_cudagemm;
struct cce_gguf_qwen35_ext; /* qwen35 hybrid extension (cce_gguf_qwen35.c) */

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
    /* Async paged KV (CNET_KV_PAGE=1). When set, k_cache/v_cache may be NULL
       or a small fallback; use cce_gguf_qwen2_k_row / kv_write_slice. */
    struct cce_kv_pager *kv_pager;
    int kv_legal_max; /* model context_length (e.g. 1M); max_ctx may be hot */

    /* Optional streaming-aware KV index (not owned). When set, dense attention
     * applies a pre-attention block mask: only stream_ix active positions
     * (plus current token) participate in softmax/V — not mid-GEMM filtering.
     * Bump/clear with weight_epoch via host on MTK flush. */
    struct cce_kv_stream_index *stream_ix;

    /* Weight-cartridge epoch (MTK apply/revert). Neural KV is only valid for
     * the current epoch; CERT/text memory is epoch-invariant. Bumped in
     * cce_mtk_gguf_kv_flush. */
    uint64_t weight_epoch;

    /* Optional per-instance GPU handles (override process-global).
       OpenCL (AMD dual-GPU production) + hipBLAS (large FP) + optional
       cuBLAS (NVIDIA, CNET_GPU_BACKEND=cuda). Concurrent instances need
       their own handles (shared queues race). */
    struct cce_clgemm *clgemm;
    struct cce_hipgemm *hipgemm;
    struct cce_cudagemm *cudagemm;

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

    /* Qwen3.5 hybrid extension (gated attention + Gated-DeltaNet layers).
       NULL = classic transformer; every legacy path is byte-identical. When
       set, cce_gguf_qwen2_forward dispatches whole-forward to the qwen35
       runner (cce_gguf_qwen35.c), which owns the per-layer kind schedule,
       recurrent/conv state, and the prefix-rewind checkpoint protocol. */
    struct cce_gguf_qwen35_ext *qwen35;

    /* OPT-IN sparse per-specialist KV routing (Phase 2 execution slice).
       0.0 (every loader callocs it) = OFF: the attention loop is
       byte-identical to the historical forward. (0, 1]: each attention
       step's softmax/V-read is restricted to the rows chosen by the ONE
       cce_sparse_kv selector (cce_specialist_select_kv_tokens) under a
       max_tokens budget of ceil(fraction * visible rows); 1.0 selects
       every row and is gated BIT-IDENTICAL to OFF. Never write directly:
       set through cce_gguf_qwen2_set_sparse_kv (malformed values refused)
       or the CNET_SPARSE_KV env knob read at load. Classic transformer
       path only — the qwen35 hybrid runner has no sparse read. */
    float sparse_kv_fraction;

    /* Full DSA attention (DeepSeek-style). When sparse_kv_fraction > 0 the
       forward uses lightning index → dual select (index WHO, q·k HOW) →
       sleep/floor-keep-one → skip zero V. Quality default: sleep only.
       CNET_DSA_PROFILE=speed enables floor grid. */
    float dsa_floor_quantum;
    float dsa_sleep_eps;
    int   dsa_keep_anchors;
    int   dsa_index_mode;   /* cce_dsa_index_mode */
    int   dsa_index_heads;

    /* MLA-lite int8 KV (CNET_MLA_KV=1): store int8+scale alongside f32;
       DSA scores via q8 dot; V dequant only on awake support. */
    int     mla_kv;
    int8_t *k_mla_q8;       /* max_ctx * k_slot_floats */
    float  *k_mla_scale;    /* max_ctx * n_layer (per-pos, per-layer) */
    int8_t *v_mla_q8;
    float  *v_mla_scale;    /* max_ctx * n_layer */
} cce_gguf_qwen2;

/* The live GGUF fp16->fp32 decoder (all quant superblock scales flow through
 * it). Exported so the exhaustive identity test exercises the REAL code. */
float cce_gguf_f16_to_f32(uint16_t h);

/* Load a full Qwen2 model from GGUF into decomposed CCE form (forest of specialists + norms).
 * Similar to cce_supra_load_decomposed.
 */
cce_result cce_gguf_load_qwen2(cce_gguf_qwen2** out, const char* path);

/* Load a Qwen3.5 hybrid GGUF (gated attention + Gated-DeltaNet, arch
 * "qwen35") into the SAME cce_gguf_qwen2 struct every oracle consumer
 * binds to (depth_probe / window_discover / flagship). The nextn/MTP draft
 * block is loaded by llama.cpp as an extra decoder block but never executed
 * in the main pass — here it is skipped entirely (n_layer = trunk only). */
cce_result cce_gguf_load_qwen35(cce_gguf_qwen2** out, const char* path);

/* Phase 4: architecture dispatcher. Detects arch from GGUF metadata and builds
 * the appropriate forest. Falls back to qwen2/llama-style builder for compatible models.
 */
cce_result cce_gguf_load_model(cce_gguf_qwen2** out, const char* path);

/* Free the model */
void cce_gguf_qwen2_free(cce_gguf_qwen2* m);

/* Paged / dense KV accessors (pager if CNET_KV_PAGE=1, else dense slab). */
float *cce_gguf_qwen2_k_row(cce_gguf_qwen2 *m, int pos);
float *cce_gguf_qwen2_v_row(cce_gguf_qwen2 *m, int pos);
/* Prefer HOT; rehydrate COLD when CNET_KV_REHYDRATE=1. */
const float *cce_gguf_qwen2_k_row_ex(cce_gguf_qwen2 *m, int pos);
const float *cce_gguf_qwen2_v_row_ex(cce_gguf_qwen2 *m, int pos);
int cce_gguf_qwen2_kv_prepare(cce_gguf_qwen2 *m, int pos);
int cce_gguf_qwen2_kv_write_slice(cce_gguf_qwen2 *m, int pos, size_t k_off,
                                  const float *k, int k_dim, size_t v_off,
                                  const float *v, int v_dim);
int cce_gguf_qwen2_kv_jmin(const cce_gguf_qwen2 *m, int jmin);
/* Opt-in: open async pager (CNET_KV_PAGE=1), free dense slabs. */
void cce_gguf_qwen2_enable_kv_page(cce_gguf_qwen2 *m);

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
void cce_gguf_set_hipgemm(struct cce_hipgemm *h);
void cce_gguf_qwen2_set_hipgemm(cce_gguf_qwen2 *m, struct cce_hipgemm *h);
void cce_gguf_set_cudagemm(struct cce_cudagemm *h);
void cce_gguf_qwen2_set_cudagemm(cce_gguf_qwen2 *m, struct cce_cudagemm *h);

/* Restrict the forward's head to `n` token ids (bit-identical to the full
   head on those ids; ~1/4 less work per forward). NULL/0 = full head. The
   ids array must outlive the model; the campaign's discovered window fits. */
void cce_gguf_qwen2_set_head_window(cce_gguf_qwen2 *m, const int *ids, int n);

/* OPT-IN sparse per-specialist KV routing on THIS runner's attention path.
   budget_fraction == 0.0 disables (the default: byte-identical forward);
   0 < f <= 1 restricts every attention step's softmax/V-read to the rows
   chosen by the cce_sparse_kv selector under max_tokens =
   ceil(f * visible rows) — 1.0 selects every row and is gated bit-identical
   to OFF. Malformed budgets (negative, > 1, NaN) are REFUSED with
   CCE_ERR_INVALID_ARG (state unchanged); qwen35 hybrids are refused with
   CCE_ERR_UNSUPPORTED (that runner has no sparse read). Env knob:
   CNET_SPARSE_KV=<fraction>, read by cce_gguf_load_qwen2 at load — a
   malformed value refuses the load. */
cce_result cce_gguf_qwen2_set_sparse_kv(cce_gguf_qwen2 *m, float budget_fraction);

/* Bind optional stream index for pre-attention HOT mask (not owned).
 * NULL unbinds. Does not enable DSA; filters dense attend support. */
void cce_gguf_qwen2_bind_stream_index(cce_gguf_qwen2 *m,
                                      struct cce_kv_stream_index *ix);

/* Observation-only tap for the sparse-KV selection (gate/probe tooling):
   fires per (layer, head, query step) AFTER selection, with the raw
   pre-softmax scores of the visible rows (scores[0..n_rows-1], row r =
   absolute position jmin + r) and the selected row indices (relative to
   jmin, ascending). NULL (the default) = zero cost; it can never fire while
   sparse KV is OFF. Observes only; must not mutate. */
void cce_gguf_set_sparse_kv_tap(void (*fn)(int layer, int head, int q_pos,
                                           int jmin, const float *scores,
                                           int n_rows, const int *selected,
                                           int n_selected, void *uctx),
                                void *uctx);

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

/* Optional residual adapter hook (NULL = byte-identical). Mirrors DS lily hook. */
typedef void (*cce_gguf_layer_adapt_fn)(int layer, float *residual, int width, void *ctx);
extern cce_gguf_layer_adapt_fn g_cce_gguf_layer_adapt_hook;

#ifdef __cplusplus
}
#endif

#endif /* CCE_GGUF_H */