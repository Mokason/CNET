#ifndef CCE_WEIGHT_STORE_H
#define CCE_WEIGHT_STORE_H

/* Content-addressed weight store: specialists stored ONCE by digest,
 * models become manifests of references.
 *
 * The codebase-memory-mcp move applied to weights: their team-shared graph
 * artifact means an indexed repo is never re-derived; here a specialist
 * (cascade) or parameter tensor is written once under its content digest
 * (git-objects style: one file per digest in a store directory) and every
 * model that contains the same bytes just references it. Two fine-tunes
 * sharing 90% of layers cost the store 10% extra. Tied embeddings dedup
 * automatically (same bytes => same digest).
 *
 * Honesty rule: a "reused" verdict is BYTE-VERIFIED against the stored
 * payload, never trusted on the 64-bit hash alone; a digest collision with
 * different bytes is detected and refused (CCE_ERR_UNSUPPORTED).
 *
 * cce_weight_store_ingest_model writes a manifest (flat text: hparams +
 * "spec <branch> <digest>" / "tensor <slot> <digest>" rows);
 * cce_weight_store_restore_transformer / _restore_ssm rebuild a runnable
 * model from a manifest — the round-trip gate is bit-identical logits.
 */

#include "cce_defs.h"
#include "cce_cascade.h"
#include "cce_detect.h"
#include "cce_ssm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_weight_store cce_weight_store;

cce_result cce_weight_store_open(cce_weight_store** out, const char* dir);
void cce_weight_store_close(cce_weight_store* s);

/* Store a cascade under its content digest. reused_out=1 when an identical
 * payload was already present (byte-verified). */
cce_result cce_weight_store_put(cce_weight_store* s, const cce_cascade* cas,
                                uint64_t* digest_out, int* reused_out);

/* Store a raw parameter tensor (norms, embeddings, ...) the same way. */
cce_result cce_weight_store_put_tensor(cce_weight_store* s, const cce_tensor* t,
                                       uint64_t* digest_out, int* reused_out);

/* Materialize a HOT cascade / owned tensor from the store. Tensor payloads
 * are bytes-only (shape is manifest metadata), so the caller supplies the
 * shape; numel must match the stored payload. */
cce_result cce_weight_store_get(cce_weight_store* s, uint64_t digest, cce_cascade** out);

/* get_opt flags */
#define CCE_WS_GET_RAW_QUANT 1
#define CCE_WS_GET_SLIM      2
/* CCE_WS_GET_SLIM: restore blocks WITHOUT Adam moment tensors (2x FP-sized
 * dead VA per block for inference consumers; the mmap/munmap churn dominated
 * streamed-expert fetch cost). Slim cascades must never train. */
/* CCE_WS_GET_RAW_QUANT: restore quantized payloads (int8/trit/int4) WITHOUT
 * dequantizing into weights.data — the FP tensor of a quant block is left
 * UNINITIALIZED. The quantized forward never reads it (w_trit > w_q > FP),
 * and at scale the dequant writes dominate fetch cost, so the tier runtime
 * streams with this flag. Only forward-path consumers may use such a
 * cascade; FP payloads are unaffected. */
cce_result cce_weight_store_get_opt(cce_weight_store* s, uint64_t digest, cce_cascade** out,
                                    int flags);
cce_result cce_weight_store_get_tensor(cce_weight_store* s, uint64_t digest,
                                       const int* shape, int ndim, cce_tensor* out);

int    cce_weight_store_contains(const cce_weight_store* s, uint64_t digest);
int    cce_weight_store_count(const cce_weight_store* s);   /* payload files */
size_t cce_weight_store_bytes(const cce_weight_store* s);   /* payload bytes on disk */
long   cce_weight_store_payload_size(const cce_weight_store* s, uint64_t digest); /* bytes, -1 if absent */

/* Ingest every specialist + parameter tensor of a loaded model; write the
 * manifest. Counts report the dedup outcome. Transformer + ssm supported. */
cce_result cce_weight_store_ingest_model(cce_weight_store* s, const cce_anymodel* m,
                                         const char* model_name, const char* manifest_path,
                                         int* n_total, int* n_new, int* n_reused);

/* Rebuild a runnable transformer from a manifest (forest specialists +
 * norms + embedding + kv cache + numerics). forest_archive_path backs the
 * rebuilt forest (caller picks a unique temp path). */
cce_result cce_weight_store_restore_transformer(cce_weight_store* s, const char* manifest_path,
                                                const char* forest_archive_path,
                                                cce_gguf_qwen2** out);

/* Rebuild a runnable mamba-1 SSM from a "family ssm" manifest (specialist
 * branches + per-layer small tensors + fresh recurrent state). Same
 * round-trip contract: bit-identical logits vs the original loader. */
cce_result cce_weight_store_restore_ssm(cce_weight_store* s, const char* manifest_path,
                                        const char* forest_archive_path,
                                        cce_ssm_model** out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_WEIGHT_STORE_H */
