#ifndef CCE_ST_LLAMA_H
#define CCE_ST_LLAMA_H

/* HF-llama-style safetensors -> decomposed CCE transformer.
 *
 * Loads a model.safetensors with HF naming (model.layers.N.self_attn.q_proj
 * etc.) into the SAME cce_gguf_qwen2 structure the GGUF loader builds:
 * a forest of named linear specialists ("qwen2.blk.N.q_proj", ...) plus norm
 * tensors. The existing cce_gguf_qwen2_forward then runs it unchanged, so
 * the two container formats share one verified runner.
 *
 * Head counts are not stored in safetensors tensors; they come from the
 * config.json that HF model directories always ship next to the weights
 * (num_attention_heads / num_key_value_heads). Without it the loader
 * refuses (CCE_ERR_NOT_FOUND) rather than guessing head counts.
 *
 * Single-file checkpoints only; sharded model-00001-of-N is not handled yet.
 */

#include "cce_defs.h"
#include "cce_gguf.h"   /* cce_gguf_qwen2 */

#ifdef __cplusplus
extern "C" {
#endif

cce_result cce_st_llama_load(cce_gguf_qwen2** out, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* CCE_ST_LLAMA_H */
