#ifndef CCE_SAFETENSORS_H
#define CCE_SAFETENSORS_H

/* Safe, header-first safetensors loader for CNet / CCE.
 *
 * Safetensors format: 8-byte LE header_len + JSON header + raw tensor bytes.
 * We parse header first, validate ALL metadata + offsets against file size
 * before allocating or reading any tensor data. No pickle, no code exec.
 *
 * Supports mapping extracted tensors into:
 *  - cce_block / cce_cascade / cce_forest (primary, compositional)
 *  - BinaryTransformNetwork (BTN) for legacy CNet primitives
 *
 * Philosophy: seed or initialize frozen specialists from external weights
 * (e.g. small exported MLPs), never treat as a full PyTorch replacement.
 * Transpose handling is explicit because conventions differ (torch [out,in] vs
 * CCE row-per-input [in,out] layout in many blocks).
 */

#include "cce_defs.h"
#include "cce_tensor.h"
#include "cce_block.h"
#include "cce_cascade.h"
#include "cce_forest.h"
#include "cce_sparse_kv.h"
#include "cce_compression.h"
#include "../nn.h"   /* for BinaryTransformNetwork (sibling under include/) */

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_ST_MAX_TENSORS 1024
#define CCE_ST_MAX_NAME    128
#define CCE_ST_MAX_DTYPE   16
#define CCE_ST_MAX_SHARDS  64   /* max shard files per index.json checkpoint */

typedef struct {
    char     name[CCE_ST_MAX_NAME];
    int      shape[CCE_MAX_DIMS];
    int      ndim;
    char     dtype[CCE_ST_MAX_DTYPE];  /* "F32","F16","BF16","I32",... */
    uint64_t data_offset;              /* bytes from start of data section */
    uint64_t data_size;                /* bytes */
} cce_safetensor_meta;

typedef struct cce_safetensors cce_safetensors;  /* opaque */

/* Load and validate entire file (header first).
 * Returns OK or error; on OK, *st owns parsed metadata (no tensor data yet).
 * Rejects: insane header_len, truncated, offset overflow, bad json, unsupported.
 *
 * Also accepts an HF sharded-checkpoint index (model.safetensors.index.json):
 * a plain-JSON path with a "weight_map" auto-dispatches to
 * cce_safetensors_load_sharded. A real single-file .safetensors (binary
 * 8-byte header) always wins the sniff.
 */
cce_result cce_safetensors_load(const char* path, cce_safetensors** st);

/* Load an HF sharded checkpoint via its index json. Shard files are resolved
 * as plain siblings of the index (no path components allowed). Refuses on ANY
 * mismatch: tensor missing from its shard, tensor not in the weight_map,
 * duplicate names, missing shard file. The returned handle is used exactly
 * like a single-file one; reads go to the owning shard.
 */
cce_result cce_safetensors_load_sharded(const char* index_json_path, cce_safetensors** st);

/* 0 for single-file checkpoints, number of shard files when index-loaded. */
int cce_safetensors_shard_count(const cce_safetensors* st);

/* Free the loader state (metadata + any cached data). */
void cce_safetensors_free(cce_safetensors* st);

/* Query */
int  cce_safetensors_count(const cce_safetensors* st);
cce_result cce_safetensors_get_meta(const cce_safetensors* st, int idx,
                                    cce_safetensor_meta* out_meta);

/* Find by exact name (case sensitive). Returns index or -1. */
int cce_safetensors_find(const cce_safetensors* st, const char* name);

/* Look up a string value in the header's "__metadata__" map (free-form
 * string->string pairs real safetensors files may carry, e.g. {"n_head":"12"}).
 * Returns 1 and copies the value into buf when the key is present, 0 otherwise.
 * Single-file checkpoints only (sharded indexes expose no metadata here). */
int cce_safetensors_meta_lookup(const cce_safetensors* st, const char* key,
                                char* buf, size_t cap);

/* Load one tensor as owned cce_tensor (always F32; converts F16 on the fly if possible).
 * Caller must cce_tensor_free the result if owns_memory.
 * For large models, prefer loading specific tensors rather than everything.
 */
cce_result cce_safetensors_load_as_tensor(const cce_safetensors* st, int idx,
                                          cce_tensor* out_tensor);

/* Load raw F32 data directly into user buffer (no cce_tensor). */
cce_result cce_safetensors_load_f32(const cce_safetensors* st, int idx,
                                    float* buf, size_t cap_elems);

/* ---- CCE structure population (safe copy + optional transpose) ---- */

/* Populate an already-initialized cce_block's weights/bias from named tensors.
 * If bias_name == NULL or not found, bias left as-is (often zero).
 * If transpose_weight != 0, transposes on copy (common for torch Linear.weight).
 * Requires matching element counts; does not reallocate block tensors.
 */
cce_result cce_safetensors_populate_block(cce_block* blk,
                                          const cce_safetensors* st,
                                          const char* weight_name,
                                          const char* bias_name,
                                          int transpose_weight);

/* Create + populate a simple linear cascade from a sequence of layers.
 * For each layer i, expects names like prefix_i_weight and prefix_i_bias (or custom).
 * Example prefix="layer", count=2 -> looks for layer0.weight + layer0.bias etc.
 * The created cascade owns its blocks; use cce_cascade_destroy to free.
 * If head_last !=0 the final block is created as LINEAR_HEAD (no post-act).
 */
cce_result cce_safetensors_build_linear_cascade(cce_cascade** out_cas,
                                                const cce_safetensors* st,
                                                const char* layer_prefix,
                                                int num_layers,
                                                int head_last,
                                                float init_scale_fallback);

/* Add a whole branch (linear cascade) to a forest from safetensors weights.
 * Convenience over build + add. Name is the branch name.
 */
cce_result cce_safetensors_add_linear_branch(cce_forest* forest,
                                             const cce_safetensors* st,
                                             const char* branch_name,
                                             const char* layer_prefix,
                                             int num_layers,
                                             int head_last,
                                             float init_scale_fallback,
                                             int* out_branch_idx);

/* ---- Full CNet / BTN population ---- */

/* Populate a BinaryTransformNetwork from safetensors.
 * This is best-effort layout mapping. Caller must have called btn_init with
 * correct input/output/hidden counts beforehand.
 *
 * Mapping (by name, caller chooses convention or we try common keys):
 *   "input_hidden" or "fc1.weight" or "0.weight" -> input->hidden weights
 *   "hidden_bias" / "fc1.bias"                  -> hidden biases
 *   "hidden_output_weights" / "fc2.weight"      -> hidden->output (transposed layout)
 *   "output_bias" / "fc2.bias"
 *
 * If transpose is requested for a matrix, we handle the row/col swap to match
 * btn_input_hidden_index / btn_hidden_output_index.
 * Returns 0 on success or partial; negative on hard failure.
 */
int btn_populate_from_safetensors(BinaryTransformNetwork* btn,
                                  const cce_safetensors* st,
                                  const char* ih_name,
                                  const char* hb_name,
                                  const char* ho_name,
                                  const char* ob_name,
                                  int transpose_ih,
                                  int transpose_ho);

/* Simple heuristic populate: tries a few common naming patterns used by
 * exported tiny MLPs. Good for quick import of 1-3 layer models.
 * Returns number of weight tensors successfully copied or -1 on error.
 */
int btn_populate_from_safetensors_heuristic(BinaryTransformNetwork* btn,
                                            const cce_safetensors* st,
                                            int transpose);

/* ---- Diagnostics / safety ---- */

/* Return human readable last error (thread-local, best effort). */
const char* cce_safetensors_last_error(void);

/* Dump metadata (debug). */
void cce_safetensors_print_info(const cce_safetensors* st, const char* label);

/* ---- Hugging Face Hub + direct URL support ----
 *
 * The project already fetches datasets from https://huggingface.co/datasets.
 * These functions extend that to **model weights** using the safetensors loader.
 *
 * Downloads use direct resolve URLs:
 *   https://huggingface.co/{repo}/resolve/{revision}/{filename}
 *
 * Authentication: for huggingface.co hosts only, reads HF_TOKEN or
 * HUGGING_FACE_HUB_TOKEN from the environment and sends an Authorization
 * header (works for gated/private models).
 *
 * Safety: HTTPS only; TLS verification remains enabled; redirects are limited
 * to five HTTPS hops; connect/transfer/low-speed timeouts apply; and each
 * download is capped at 8 GiB before the normal header-first parser runs.
 */

/* Load from a direct HTTPS URL without embedded credentials (any safetensors file).
 * Example: "https://huggingface.co/bert-base-uncased/resolve/main/model.safetensors"
 */
cce_result cce_safetensors_load_url(const char* url, cce_safetensors** st);

/* Convenience loader for Hugging Face models.
 * repo:     "bert-base-uncased" or "google/bert-base-uncased"
 * filename: "model.safetensors" (or "model-00001-of-00002.safetensors" etc.)
 * revision: "main", "refs/pr/1", a commit hash, or NULL for default ("main")
 *
 * This is the primary "connect to Hugging Face" entry point for weights.
 */
cce_result cce_safetensors_load_hf(cce_safetensors** st,
                                   const char* repo,
                                   const char* filename,
                                   const char* revision /* may be NULL */);

/* Helper: construct a HF resolve URL (returns bytes written or -1).
 * Useful if you want to inspect or customize before download.
 */
int cce_hf_build_resolve_url(char* buf, size_t cap,
                             const char* repo,
                             const char* filename,
                             const char* revision);

/* ---- GPT-style decomposed loader (non-monolithic; Supra is one input) ---- */

/* Decomposed representation of a GPT-shaped transformer checkpoint.
 * All heavy linear projections live in the forest as independent tiny cascades.
 * Embed tables and VQ conv weights are kept as first-class cce_tensor so they
 * can be used with cce_tensor_embed / custom conv without forcing a monolithic object.
 *
 * The loader is schema-driven: it recognizes Supra's tensor naming
 * ("blocks.{i}.attn.qkv", torch Linear [out,in] weights) and GPT-2-style naming
 * ("h.{i}.attn.c_attn", Conv1D [in,out] weights, tied lm_head), with n_layer
 * counted from the actual per-layer tensors and n_head taken from the file's
 * "__metadata__" (key "n_head") or a sibling config.json ("n_head" /
 * "num_attention_heads"), defaulting to Supra's documented 4.
 */
typedef struct {
    cce_forest*   forest;          /* owns all the linear specialist cascades */
    cce_tensor    tok_emb;         /* [vocab, dim] */
    cce_tensor    pos_emb;         /* [block_size, dim] */
    cce_tensor    *ln1_w, *ln1_b;  /* heap arrays [n_layer] (was fixed [4]) */
    cce_tensor    *ln2_w, *ln2_b;  /* heap arrays [n_layer] */
    cce_tensor    ln_f_w, ln_f_b;
    cce_tensor    vq_enc_w[3], vq_enc_b[3];   /* full encoder conv weights (4D) */
    cce_tensor    vq_dec_w[3], vq_dec_b[3];   /* decoder transposed conv */
    cce_tensor    vq_codebook;     /* [256, 64] */
    /* model hyperparams recovered from weights */
    int           n_layer;
    int           n_embd;
    int           block_size;
    int           n_head;
    int           vocab_size;
    int           vq_codebook_size;
    int           vq_code_dim;
    /* BitNet ternary tok_emb: debug (ternarize FP per lookup) or packed (trits, no FP) */
    int           tok_emb_ternary;     /* 1 = ternarize FP tok_emb per lookup */
    int           tok_emb_packed;      /* 1 = use tok_emb_trit/scale (FP tok_emb freed) */
    uint8_t*      tok_emb_trit;        /* [vocab * ceil(n_embd/5)] */
    float*        tok_emb_scale;       /* [vocab] per-row absmean */
    int           tok_emb_trit_bpr;    /* bytes per row = ceil(n_embd/5) */
    /* provenance of the load (informational) */
    const char*   naming_schema;       /* "supra" / "gpt2" (static string); NULL for packed reloads */
    int           head_tied;           /* 1 = logits head tied to tok_emb (no separate head tensor) */
    cce_context_routing_mode context_routing_mode;
    cce_specialist_kv_budget kv_budget;
    cce_compression_grads compression_grads;
} cce_supra_decomposed;

/* Load (and decompose) a GPT-shaped model into independent CNet specialists.
 * cache_dir: where the weights live (NULL = "supra_cache"). A missing
 *            model.safetensors is downloaded ONCE from the Supra repo and
 *            reused every session; any pre-populated cache dir (e.g. a local
 *            GPT-2-style checkpoint) is used as-is, no network.
 * revision:  HF revision (NULL = "main").
 * n_layer / tensor names are discovered per the schema table (see the struct
 * comment); n_head comes from safetensors "__metadata__" or config.json. */
cce_result cce_supra_load_decomposed(cce_supra_decomposed** out,
                                     const char* cache_dir /* NULL = "supra_cache" */,
                                     const char* revision  /* NULL = main */);
void       cce_supra_free_decomposed(cce_supra_decomposed* m);

/* Select full KV or sparse per-specialist KV routing for autoregressive generation. */
cce_result cce_supra_set_context_routing(cce_supra_decomposed* m,
                                         cce_context_routing_mode mode,
                                         const cce_specialist_kv_budget* budget);

/* Optional Grads[] buffer for compression-aware recovery passes. It preserves
   identity and residual backward paths while quantization/packing changes the
   forward representation. */
cce_result cce_supra_enable_gradient_accumulation(cce_supra_decomposed* m,
                                                  size_t grad_count);
void       cce_supra_clear_gradient_accumulation(cce_supra_decomposed* m);
cce_result cce_supra_accumulate_compression_gradient(cce_supra_decomposed* m,
                                                     const float* identity_grad,
                                                     const float* residual_grad,
                                                     size_t grad_count,
                                                     float identity_scale,
                                                     float residual_scale);

/* int8 weight-only post-training quantization of all linear specialists
   (per-output-channel, symmetric). Returns #blocks quantized or -1.
   Weight-only: activations stay FP32, so quality loss is small. */
int        cce_supra_quantize_int8(cce_supra_decomposed* m);

/* BitNet b1.58 ternary PTQ of all specialists (weights in {-1,0,+1}, absmean
   scale). Returns #blocks quantized or -1. Post-hoc; full quality needs QAT. */
int        cce_supra_quantize_ternary(cce_supra_decomposed* m);

/* Pack every specialist (incl. head) AND tok_emb to 1.6-bit trits (5/byte), freeing
   the int8 codes and the FP embedding. The Supra forward then runs directly on
   packed weights. Returns packed byte size or -1. Bit-exact with the ternary forward. */
long       cce_supra_pack_trits(cce_supra_decomposed* m);

/* Serialize a packed model (call cce_supra_pack_trits first) to a self-contained
   1.6-bit artifact: packed-trit specialists + packed tok_emb + FP pos_emb/LN/biases. */
cce_result cce_supra_export_packed(cce_supra_decomposed* m, const char* path);

/* Reload a packed artifact into an inference-only model (NO FP big tensors). Runs
   through the same cce_supra_gpt_forward. */
cce_result cce_supra_load_packed(cce_supra_decomposed** out, const char* path);

/* Inference using the decomposed CNet pieces (no monolithic model) */
cce_result cce_supra_gpt_forward(cce_supra_decomposed* m, const int* token_ids, int n_tokens,
                                 float* logits_out, int logits_cap);
int        cce_supra_generate_text(cce_supra_decomposed* m, const int* prompt, int prompt_len,
                                   int* out_ids, int max_new, float temperature, int top_k);

/* Head-only QAT support. cce_supra_hidden_last runs the frozen forward through
   ln_f and copies the final-position hidden vector (n_embd values) into h_out
   (cap >= n_embd; n_tokens <= block_size) -- the (h,target) cache. cce_supra_head_fp
   borrows the FP head weight matrix ([in,out] row-major: logit[o] = bias[o] +
   sum_i in[i]*weights[i*out_dim+o]) + bias; CCE_ERR_INVALID_ARG if no FP weights. */
cce_result cce_supra_hidden_last(cce_supra_decomposed* m, const int* token_ids,
                                 int n_tokens, float* h_out, int cap);
/* All positions' hidden ([n_tokens, n_embd] row-major); one forward -> many pairs. */
cce_result cce_supra_hidden_all(cce_supra_decomposed* m, const int* token_ids,
                                int n_tokens, float* h_out, int cap);
cce_result cce_supra_head_fp(cce_supra_decomposed* m, const float** weights,
                             const float** bias, int* in_dim, int* out_dim);

/* Basic tokenizer support for Supra (loads tokenizer.json, handles control tokens + BPE) */
typedef struct cce_supra_tokenizer cce_supra_tokenizer;

cce_result cce_supra_tokenizer_load(cce_supra_tokenizer** tok, const char* path_or_url);
void cce_supra_tokenizer_free(cce_supra_tokenizer* tok);

int cce_supra_encode_text(cce_supra_tokenizer* tok, const char* text, int* ids, int max_ids);
int cce_supra_decode_text(cce_supra_tokenizer* tok, const int* ids, int n_ids, char* out, int max_out);

/* High level SupraA2A handle (nicer API) */
typedef struct {
    cce_supra_decomposed* model;
    cce_supra_tokenizer* tokenizer;
} cce_supra_a2a;

cce_result cce_supra_a2a_load(cce_supra_a2a** a2a, const char* weights_dir_or_repo);
void cce_supra_a2a_free(cce_supra_a2a* a2a);

int cce_supra_a2a_complete_text(cce_supra_a2a* a2a, const char* prompt, char* out, int max_out, int max_new_tokens, float temp, int topk);
int cce_supra_a2a_chat_step(cce_supra_a2a* a2a, const char* user_text, char* response, int max_response);

/* Expose the internal CCE forest of specialists for use with CceForest, CceModel,
   SpecialistLibrary, router, perceptual leaves, and contract-based composition in .NET.
   For packed 1.6-bit artifacts, the branches use w_trit packed weights (transparent to forwards). */
cce_forest* cce_supra_a2a_get_forest(cce_supra_a2a* a2a);

/* Load a packed 1.6-bit Supra artifact directly into the a2a wrapper (for inference + forest exposure). */
cce_result cce_supra_a2a_load_packed(cce_supra_a2a** out, const char* packed_path);

#ifdef __cplusplus
}
#endif

#endif /* CCE_SAFETENSORS_H */
