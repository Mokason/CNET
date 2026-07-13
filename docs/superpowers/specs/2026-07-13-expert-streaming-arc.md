# Expert-streaming arc — scope (Dense + MoE)

Status: **scoping** (2026-07-13). Feasibility: **confirmed** — the streaming rails
exist and are gated. This spec scopes two arcs that sit on them: **Dense**
(near-term, mostly assembly) and **MoE** (the frontier). Motivated by Colibri
(github.com/JustVugg/colibri — GLM-5.2 744B MoE on ~25 GB RAM via NVMe expert
streaming), whose architecture is the production form of CNET's decomposition
thesis. The goal: **run and compress models far larger than RAM**, by streaming
per-specialist quantized weights from disk with a bounded resident working set.

## What already exists (do NOT rebuild)

- **`cce_tier_runtime`** (`cce_tier_attach(rt, model, store, manifest, hot_cap)`):
  makes a model's specialists *evictable* — loaded on demand via
  `cce_forest_get_resident`, **LRU-evicted whenever > hot_cap are resident**.
  Gated (`cce_tiers_test`): capped streaming logits are **BIT-IDENTICAL** to the
  all-resident model; `high_water <= cap`; one forward = one streaming pass
  (`rehydrations == specialist count`); `detach` rehydrates to standalone.
  `hot_cap >= 8` (one transformer layer's working set).
- **`cce_weight_store`** (`open/put/get/ingest_model/restore_transformer`):
  content-addressed, disk-backed, byte-verified specialist + tensor payloads.
- **Per-projection data-aware quantization** (this session): `proj_qat_gemma_e2e`
  (mixed-precision, real gemma), `gptq_solver` (efficient OBQ), `proj_qat_bitwidth`
  (component-dependent policy), `proj_qat_gpu` (dual-GPU dispatch, 2.18×).
- **Quantized specialist forward**: `cce_block` runs int8 + packed-ternary
  (1.6 bit/weight, 5 trits/byte) bit-identically to its debug path.

So the streaming ⟂ quantization pieces both exist; the arcs are about **combining**
them and, for MoE, adding the expert/router layer.

## Colibri techniques → CNET mapping

| Colibri technique | CNET home | status |
|---|---|---|
| disk-as-L3 expert streaming | `cce_weight_store` + `cce_tier_runtime` | **exists** (dense) |
| per-layer LRU cache | tier runtime LRU eviction | **exists** |
| learned frequency hot-pinning | new: usage stats → pin hot specialists resident | to build |
| async `WILLNEED` readahead | new: prefetch next layer's specialists during current matmul | to build |
| router-lookahead prefetch | new (MoE): predict next-layer experts from router | to build |
| batch-union (each expert once) | new (MoE): dedup routed experts per batch | to build |
| shape-dependent bit-width | `proj_qat_bitwidth` policy | **exists** |
| MTP head int8 / dense int4 | mixed-precision e2e finding | **validated** |

## Arc A — Dense expert-streaming (near-term)

Stream a **quantized** dense model from disk under a bounded resident cap. Almost
entirely assembly of existing parts; the only new quality question is whether
data-aware-quantized specialists stream + run within the cap at preserved quality.

- **A1 (first slice):** quantize each specialist post-hoc (int8, then the
  mixed-precision policy from `proj_qat_bitwidth`) *before* `ingest_model`; attach
  the tier runtime with a small `hot_cap`; run a streaming forward. Gate: resident
  ≤ cap, rehydrations = spec count, and the quantized streaming output matches the
  quantized all-resident output bit-for-bit (streaming changes residency, not math);
  report peak-RAM and on-disk-bytes reduction vs FP.
- **A2:** data-aware (GPTQ) specialist quantization at ingest, calibrated on a
  short pass; measure held-out quality vs FP alongside the RAM/disk win.
- **A3:** async readahead (prefetch layer l+1's specialists during layer l) +
  learned hot-pinning (keep the most-fetched specialists resident) → throughput.
- **Deliverable:** run a model whose FP weights exceed the hot-cap budget, at
  compressed size, bounded RAM, quality characterized. This is CNET's existing
  tier story **plus compression** — the honest near-term win.

## Arc B — MoE expert-streaming (the frontier)

The real prize: run GLM-class / qwen35-hybrid MoE (hundreds of GB of experts, ~a
few % active per token) on commodity RAM. New machinery, but on the same rails.

- **B1 — MoE loader:** extend `cce_gguf` to parse MoE tensors (per-layer × N
  experts: `ffn_gate_exps`/`ffn_up_exps`/`ffn_down_exps` or per-expert blocks +
  the router `ffn_gate_inp`), each expert → a forest cascade (a streamable
  specialist). Shared/dense components load normally.
- **B2 — MoE forward + router:** sigmoid/softmax top-k router selects experts per
  token; demand-load selected experts through the tier runtime (LRU/store); combine
  with router weights. Gate: matches a reference (llama.cpp) top-k selection +
  output within tolerance on a few tokens.
- **B3 — Colibri enhancements:** batch-union (each unique expert read once per
  batch, applied to all matching positions), async router-lookahead prefetch,
  learned frequency-based expert pinning (hot experts stay VRAM/RAM-resident).
- **B4 — per-expert data-aware quantization:** each expert is an independent FFN →
  our per-projection GPTQ calibrated on that expert's *routed* activations
  (captured while the router sends real tokens to it). Mixed-precision per the
  bit-width policy. Experts stream quantized.
- **Constraint:** training/joint-QAT of a full MoE is infeasible (memory); the
  method is **post-hoc per-expert reconstruction** — which is exactly why the
  no-global-backward decomposed approach is the right tool here.

## Recommended sequence

1. **Arc A1** (quantized dense streaming, bit-identity + RAM/disk report) — proves
   the streaming ⟂ quantization combination end-to-end, small, verifiable, now.
2. **Arc A2** (data-aware at ingest + quality) — banks the quality story.
3. **Arc B1–B2** (MoE loader + forward) — the big new capability; needs a real
   small MoE checkpoint + a reference for parity.
4. **Arc B3–B4** (Colibri enhancements + per-expert QAT) — throughput + compression
   at MoE scale.

The honest gate at each step: bit-identity where math is unchanged (streaming),
held-out quality where it is (quantization), and a reference (llama.cpp) where CNET
can't yet self-verify (MoE forward).
