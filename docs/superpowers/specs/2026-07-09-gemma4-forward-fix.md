# gemma4 Forward: Faithful Attention + the fp16 Subnormal Root Cause

**Date:** 2026-07-09
**Status:** Fixed and reference-validated same session (user directive: "lets
fix the gemma4 forward next"). BOS gate + Paris-chain gate token-identical to
llama.cpp (ccb0c3422) on the same GGUF; full battery running.
**Scope:** `src/cce/cce_gguf.c`, `include/cce/cce_gguf.h`,
`tests/gemma4_vs_ref.{c,py}`, `tests/test_f16_identity.c`.

## Method

Reference-first: extracted the complete gemma4 semantics from llama.cpp
(`src/models/gemma4.cpp` — a first-class arch, not a gemma3 reuse), dumped
the GGUF's own metadata/tensor shapes as ground truth, then stage-bisected
CNET vs `llama-eval-callback` values at every layer-0 node for the same
token ids. Key bisect lever: at T=1 softmax(single)=1, so attention output
= V exactly — the whole Q/K/rope/mask path drops out of the comparison.

## Fixes, in the order the evidence forced them

1. **Attention scale = 1.0 for gemma4** (`f_attention_scale`; qk-norm
   carries the conditioning). Was 1/sqrt(head_dim) — ÷16 on SWA layers.
2. **GeGLU gate activation = tanh-GELU** (ggml constants), gated
   `m->ffn_gelu` for the gemma family. Was SiLU for everything.
3. **Weightless RMSNorm on V** (new in gemma4 vs gemma3; V never RoPE'd),
   including the tied-V global layers. The loader's earlier inference —
   global layers ship K but no V, V = raw pre-norm K projection — was
   CONFIRMED against the reference (`Vcur = wv ? wv·cur : Kcur`).
4. **rope_freqs.weight frequency FACTORS divide global-layer frequencies**
   (values ramp 1.0 → 1e30: the tail rotary dims of 512-wide global heads
   are effectively position-independent). Tensor was loaded but unused.
5. **layer_output_scale multiplies the whole residual stream at layer end**
   (reference `gemma4.cpp:392`, value-verified: 3.5745 × 0.053 = 0.1894).
   Was applied to the attention branch only. This merged model carries
   0.053-class scales — placement compounds over 48 layers.
6. **suppress_tokens head mask**: `tokenizer.ggml.suppress_tokens` (here
   {258883, 258882}) gets -inf after the softcap, mirroring the reference's
   known-issue-checkpoint guard. CNET now erases the same ids.
7. **THE ROOT CAUSE — fp16 subnormal decode.** `gguf_f16_to_f32`'s
   subnormal branch normalized to bit 0x200 and masked 0x1FF (one bit
   short of the 10-bit mantissa's 0x400/0x3FF). Every subnormal scale
   decoded as (512+m)·2^-24 instead of m·2^-24 — up to 2× too large — so
   any Q4_K/Q6_K superblock whose f16 `d` landed subnormal dequantized a
   whole 256-weight block wrong. Diagnosed from a constant 1.6080 factor
   on the BOS embedding row's nonzeros (zeros unaffected — q−32=0 is
   scale-independent): 1 + 512/842 = 1.60808. Explains the scattered
   corruption signature (embeddings + tied head, V, Q wrong; K fine) and
   the entire broken-oracle era. Invisible to CNET-vs-CNET identity tests
   (both sides shared the decoder) and to the sampled ds4 cross-check
   (no subnormal scale in the sample; Q6_K not covered).

Deliberately unchanged: final softcap on returned logits stays OFF
(monotone → decision-identical; fp32 tanh saturation manufactures ties) —
the reference applies it, the argmax gates pass either way.

## Verification

- `make f16_identity`: all 65,536 fp16 bit patterns through the LIVE
  decoder vs an independently written reference — 65536/65536 bit-exact,
  2,046 subnormals covered. Wired next to the dequant xcheck.
- Independent Q6_K row dequant from raw file bytes == llama.cpp == fixed
  CNET (scratch tool `q6_row.c`).
- Token gates vs llama.cpp on the same GGUF, greedy, id space:
  - `[2]` → argmax 532 (' and') — identical.
  - `[2,818,5279,529,7001,563]` ("The capital of France is") →
    9079 (' Paris'), 236761 ('.'), 106 (<end_of_turn>) — chain-identical.
- `tests/gemma4_vs_ref.py`: 6-prompt × 16-step argmax-identity battery
  (tokenizes via llama-server, compares chains; divergence-step reporting).

## Consequences (their weight acknowledged)

- **Every existing gemma4-era mined base certified a broken oracle** (the
  memory note stands until re-mined). The oracle definition changed —
  per verification discipline, a fresh trusted reference must be re-mined
  and depth_probe re-run before any campaign consumes this forward.
- The fp16 fix touches ALL quantized loads (qwen2 teachers included):
  historical qwen2 digests may shift wherever subnormal scales occurred.
  Re-run the flagship startup gates before trusting old digests.
- The forward remains batch-1-oriented CPU-first (the oracle's shape);
  clgemm GPU seam untouched by these changes except through corrected
  weights.

## Instruments kept (env-gated, zero cost off)

- `CNET_FWD_TRACE=1` now prints per-stage first-4 values (`FWD_V4`) for
  layer 0 alongside the checksums — the stage-bisect harness that found
  the embed corruption, kept for the next drift hunt.
- `GVR_DUMP_EMB=1` in `bin/gemma4_vs_ref` dumps an embedding row for
  direct file-truth comparison.
