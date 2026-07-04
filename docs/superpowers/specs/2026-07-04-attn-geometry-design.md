# Geometry-Driven Attention for the Universal Runner — Design

**Date:** 2026-07-04
**Status:** Building same session (user directive: "implement the gemma4
attention rewrite, but remember we plan to use multiple different models").
**Topic:** Replace the uniform-dims attention in `cce_gguf_qwen2_forward`
with PER-LAYER geometry derived from tensor shapes + metadata, validated,
refusing loudly on inconsistency — one code path serving qwen2/llama-style
AND gemma4-style models.

## Ground truth (recon of the actual files)

gemma4 12B (gemma4-v2-Q4_K_M): 48 layers, hidden 3840, 16 q-heads ALWAYS,
but per-layer type from `attention.sliding_window_pattern` (bool[48], 5:1):
- SWA layers: head_dim 256 (`key_length_swa`), q [3840→4096]=16x256,
  k [3840→2048]=8x256, v 8x256; rope theta 10000 (`rope.freq_base_swa`),
  rope dim 256; window 1024 (`attention.sliding_window`).
- GLOBAL layers: head_dim 512 (`key_length`), q [3840→8192]=16x512,
  k [3840→512]=**1 head (MQA)**, and **NO attn_v tensor at all** — V is
  TIED to the raw K projection (1 shared head; the only decomposition
  consistent with o_proj width 16x512 and head_count_kv=1; inference
  flagged below); rope theta 1e6, dim 512.
- `attention.head_count_kv` = int[48] {8,8,8,8,8,1,...} — matches K heads.
- Per-layer RMS q-norm AND k-norm over head_dim (attn_q_norm/attn_k_norm,
  shape == head_dim of that layer). post_attention_norm + post_ffw_norm
  (3840). final_logit_softcapping = 30.0 (monotonic — cannot change
  argmax/top-k decisions, implemented for logit fidelity).
- Gemma-family embedding scale: x *= sqrt(hidden) after embed.

gemma4-assistant (the MTP draft): `shared_kv_layers=4`, NO attn_k/attn_v
tensors — it is a speculative head that shares the BASE model's KV cache;
NOT standalone-runnable. The runner's refusal is CORRECT and permanent.

## Two arch-independent latent gaps fixed with this rewrite

1. **post_ffw_norm loaded but never applied** (the attention residual DID
   exist — an earlier draft of this spec claimed otherwise; corrected after
   reading the full wiring). Now applied to the MLP branch before its
   residual add, mirroring post_attention_norm; identity when absent
   (llama/qwen2 unchanged).
2. **KV metadata arrays skipped**: the GGUF KV parser fseek'd past arrays;
   per-layer lists never reached the runner. Numeric arrays (<= 4096) are
   now retained (`gguf_kv.arr/arr_n`) with an accessor.

Known-unknown, pending external cross-check (no gemma4-capable runner
exists on this box — llama.cpp trees were deleted): the RoPE pairing
convention (this runner: interleaved GPT-J pairs) and gemma4's true
convention may differ; also the head_count_kv list is matched against K
heads with V heads derived from value_length (the only decomposition
consistent with every tensor shape).

## Mechanics

- `cce_attn_geom` per layer in cce_gguf_qwen2: {q_dim,k_dim,v_dim from
  TENSOR SHAPES; head_dim from metadata by layer type; n_q=q_dim/head_dim,
  n_k=k_dim/head_dim, n_v=v_dim/head_dim — integer-divisible or REFUSE;
  o_in==q_dim or REFUSE; rope_base, rope_dim, window per type; k_off/v_off
  into a per-layer-packed KV cache}. Legacy models (no pattern metadata)
  derive uniform geometry from their tensors — same numbers as today.
- GQA mapping: q-head h reads k-head h/(n_q/n_k) and v-head h/(n_q/n_v)
  (both divisibilities validated). Attention out = concat n_q x head_dim →
  o_proj. Scale 1/sqrt(head_dim). Causal + sliding-window mask (swa: keys
  older than pos-window+1 excluded).
- qk-norms: RMS over each head's head_dim slice when tensors present
  (shape == head_dim or REFUSE). RoPE after qk-norm (gemma3 order),
  rotating rope_dim dims with the layer's theta.
- Embedding scale sqrt(D) for gemma-family archs only (arch prefix check).
- KV cache: sum over layers of max_ctx*(k_dim+v_dim) floats, per-layer
  offsets; prefix reuse and layer_cap semantics unchanged.
- Every inconsistency (indivisible heads, o_in mismatch, norm-shape
  mismatch, missing k with n_kv>0) = loud refusal at LOAD, not forward.

## Validation (no external gemma4 runner exists on this box — llama.cpp
trees were deleted; token-level cross-check is follow-up when rebuilt)

1. Non-gemma regression: cce_st_llama, cce_detect, cce_ssm suites green.
2. 12B structural: no NaN, deterministic across processes (FWD_TRACE
   checksums equal), logits vary with context, attention sensitivity
   (perturbing a context token moves logits), window discovery finds a
   diverse attractor set, distinct top-3 decisions across probes.
3. Then: depth probe verdict (finally meaningful), fresh trusted baseline,
   ladder digest re-proof on the REAL oracle, campaign.
