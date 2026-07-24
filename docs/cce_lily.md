# cce_lily — Low-rank Interconnected Adaptation across Layers (prototype)

An escalation of `cce_lora` for **deep** bases. Where `cce_lora` corrects one
frozen linear map (the output head), Lily adapts a stack of `L` same-width
layers and **interconnects** them by sharing the down-projection basis `A` across
layers (Tied/VeRA-style):

    h_{l+1} = W_l h_l + (alpha/r) · B_l (A h_l)     A shared [d,r], B_l per-layer [r,d]

So the adaptation lives in one coherent rank-`r` subspace instead of `L`
independent ones. Params: `r·d·(L+1)` vs `2·L·r·d` (independent per-layer LoRA)
vs `L·d·d` (dense per-layer). `shared=0` gives the independent baseline with the
same code — the benchmark toggles it.

Files: `include/cce/cce_lily.h`, `src/cce/cce_lily.c`, test+bench
`tests/cce_lily_test.c` (`make cce_lily_test`).

## What it does
- `apply` forwards through the frozen stack with the per-layer deltas; `merge`
  folds each delta into `W_l` (exact — it's linear). Verified: apply==merged to
  1.5e-5.
- `train` fits `A` + `B_l` (base frozen) by **exact backprop through the
  L-layer chain**; the shared `A` accumulates gradient from every layer — that
  cross-layer coupling *is* the interconnection. Verified: recovers a planted
  shared multi-layer target (mse 0.18 → 1.4e-6).

## Interconnection benchmark (`make cce_lily_test`)
`d=24, L=4, rank=4, true rank=2, noise=0.05`, train 256 / held-out 256 (clean
truth). Held-out MSE measures recovery of the true map (overfitting to noise
shows as worse held-out).

**Ground truth: SHARED subspace across layers**

| adapter | params | train_mse | held-out |
|---------|-------:|----------:|---------:|
| **Lily (shared A)** | **480** | 0.00230 | **0.00019** |
| independent | 768 | 0.00225 | 0.00024 |

→ Lily matches/beats held-out at **1.6× fewer params** (dense would be 2304).
The shared basis is a correct prior → regularizes → generalizes slightly better.

**Ground truth: INDEPENDENT per-layer (the honest boundary)**

| adapter | params | train_mse | held-out |
|---------|-------:|----------:|---------:|
| Lily (shared A) | 480 | 0.00976 | 0.00868 |
| **independent** | 768 | 0.00220 | **0.00025** |

→ When the true adaptation is genuinely per-layer, the shared form **underfits**
(0.00868 vs 0.00025); independent is needed. The interconnection helps only when
the layers really do share a subspace.

## Verdict
The interconnection is a prior, not a free win: it buys fewer params + equal-or-
better generalization **when the adaptation shares structure across layers** (the
expected case for a coherent skill correction on a deep model), and underfits
when it does not. That's the honest signal for whether to invest in the deep
serving integration.

## Interior-layer serving on the deep base (`make cce_lily_serve`)
The invasive part is now built: Lily's deltas inject into the **real DS residual
forward** (`cce_ds_runtime.c`), whose loop is a residual stream over `n_layer`
layers of width `d_model` — a one-to-one match for Lily. A CCE-free hook
`g_cce_layer_adapt_hook(layer, residual, width, ctx)` is called after each layer's
residual update; `cce_lily_install_serving(ly)` binds an adapter to it so the
residual gets `+= (alpha/r) B_L (A_L·residual)` at each layer.

Verified against the synthetic DS runtime (real MLA attention + MoE FFN):
- the hook fires once per layer, in order;
- the served residual equals `base + (alpha/r)B(A·residual)` at the layer — diff
  **0.00e+00** (bit-exact);
- a zero-delta adapter and an uninstalled hook leave decode **byte-identical**
  (off by default, zero overhead — `DS_STACK_PASS` is unchanged, 38/38).

The hook is a plain function pointer (no adapter dependency in the DS runtime),
one active adapter at a time (prototype). `ly->width` must equal `d_model` and
`ly->layers` its `n_layer`.

## Training-data collection through the deep forward (`make cce_lily_collect`)
The teach half is now wired end to end — **no autograd through the frozen
MLA+MoE base required**:

- `cce_lily_collect(host, inputs, n, out)` runs the real DS forward on each input
  and captures the residual at **every layer's hook point** into
  `out[n * n_layer * d_model]` (via a capture variant of the same layer hook).
  That is the per-layer training-data collection loop through the live forward.
- `cce_lily_train_residual(ly, base_res, target_res, n)` fits the adapter by
  **residual distillation**: at each layer, `delta_L(base_res_L) ≈ target_res_L −
  base_res_L`, training the shared `A` + per-layer `B_L` with Adam. Each layer's
  target is the collected residual correction, so there is no backprop through
  the base — it works through a frozen deep forward that has no autograd.

`cce_lily_collect` verifies against the synthetic DS runtime: it captures the
live forward's evolved residual (not the input), a planted per-layer correction
distill-trains to mse 1.2e-9, and the adapter **recovers that correction on
held-out residuals** (max diff 2.2e-4) — collect → distill → recover, all through
the real forward. In deployment the `target_res` is a teacher's per-layer
residual stream (a stronger same-width model, or a corrected path); here it is
planted to prove the pipeline.

## Real teacher residual stream (`make cce_lily_teacher`)
A genuine (non-planted) teacher: the SAME base run on the FULL input is the
teacher; the SAME base on a low-rank-COMPRESSED input is the cheap student. Both
residual streams are captured from the real forward via `cce_lily_collect`, and a
Lily adapter is distilled from the teacher's per-layer residuals — the student
learns to recover the quality lost to compression. (The other natural teacher —
same model at more experts / dense attention — is the deployment target, but this
synthetic CI model's forward is near-identity and invariant to those compute
knobs, so it shows no gap there; input compression is a gap the real forward does
exhibit, gap 1.2e-2.)

Two things fall out, one of them important:

- Distillation fits the per-layer gap offline (mse 4.7e-9), and a correctly-
  credited adapter serves the **cheap student at near-teacher quality: the
  final-output gap closes 99%** (1.2e-2 → 6.8e-5).
- **FINDING (real, surfaced by this run):** per-layer residual distillation trains
  each layer to close the *full* gap, so applying all L deltas free-running
  **over-corrects — the deltas compound (−102% here)**. The layer whose delta does
  not compound is the last (it feeds the output directly); serving only it
  recovers the gap (99%).

## Serve-in-the-loop training (fixes the compounding)
`cce_lily_train_serve_loop` solves the multi-layer free-running credit problem
**without autograd through the frozen MLA+MoE base**. DAgger-style, it iterates:
(1) run the forward WITH the current adapter, capturing (via
`cce_lily_collect_served`) the pre-delta residual each layer *actually* sees;
(2) refit the deltas toward the teacher on THOSE served residuals. Because each
layer's target now accounts for earlier layers' corrections, the deltas stop
compounding. Measured through the real forward:

| serving | final gap to teacher | closed |
|---------|---------------------:|-------:|
| student (no adapter) | 1.176e-02 | — |
| offline per-layer, all layers | 2.378e-02 | **−102%** (over-corrects) |
| last-layer-only workaround | 6.85e-05 | 99% |
| **serve-in-the-loop, all layers** | **9.78e-08** | **100%** |

All-layer serving now closes the gap essentially perfectly — better than the
single-layer workaround, using the full multi-layer capacity correctly.


## Hosted by registry_lora's certify gate (`make registry_lily_test`)
`registry_lily_certify` / `registry_lily_teach_certify` host a Lily deep-base
adapter under the SAME `registry_lora_cert_policy` / `registry_lora_cert_report`
used for cce_lora: teach via serve-in-the-loop, then certify on held-out
(input, teacher-output) pairs by counting per-sample error fixes/regressions of
the FROZEN base vs (base+adapter) served through the DS forward, applying the
policy. Verified: the gate REJECTS a no-op adapter (fixes=0, net gain 0) and
ACCEPTS a serve-loop-trained one (fixes=128/128, regress=0, base_mse 2.6e-4 ->
adapter_mse 2.6e-10). The "certify/orchestrator machinery is parametrization-
agnostic" claim is now real: teach -> certify -> serve, same gate.

## The compute-quality teacher (`make registry_lily_compute`)
The other natural teacher is the SAME base at a higher compute budget: `full` =
dense attention + all experts vs `cheap` = sparse (DSA top-k) attention + a
single expert. To make this measurable in the CI model (whose default synthetic
weights are ~0.02, so any gap is ~1e-6), `cce_ds_set_synth_scale` amplifies the
synthetic amplitude — a guarded knob, default-preserving (`ds_stack` stays
38/38). Multi-token collection/serving is wired: `cce_lily_collect_ctx`,
`cce_lily_collect_served_ctx`, `cce_lily_train_serve_loop_ctx` (with a `prefill`
compute config), and `registry_lily_certify_ctx` hosts it under the same gate.

The result is a rigorous, honest characterisation — the compute gap is **not**
like the compression gap:

1. **It is a multi-token phenomenon** — *exactly zero* at a single token
   (attention over one position is trivially dense == sparse; the expert mix
   renormalises), real over a context (gap 5.35 at scale 2.0, T=24). This
   corrects an earlier single-token "zero gap" reading, which was an artifact of
   measuring at one position.
2. **In this synthetic runtime the gap is entirely the sparse-attention (DSA
   top-k) effect.** Dense attention with K=8 vs K=1 experts gives *exactly* zero
   gap (the synthetic experts are degenerate at that margin); attention-only
   (dense vs sparse) equals the full gap to <5%. So the whole gap is the discrete
   top-k attention.
3. **It is only partially recoverable.** The best-case *full-rank* linear map
   (ridge, the ceiling of any linear adapter reading the cheap residual) closes
   only ~32% held-out — the sparse top-k has *discarded* context information no
   adapter can restore. Contrast the compression gap: 100% recoverable.
4. **The low-rank serve-in-the-loop adapter does not recover it** — it stays at
   or under that ~32% ceiling and diverges past the regression budget, because
   sparse top-k re-selection makes the served forward non-smooth in the adapter
   (breaking the DAgger fixed point that the smooth compression gap converged on)
   and a low-rank offline fit overfits the lossy gap. **The certify gate
   correctly REJECTS it** (fixes=100/128 but regress=28 > budget) — a real guard,
   not a rubber stamp: it accepts the adapters that genuinely help (compression,
   `registry_lily_test`) and refuses to serve one that cannot.

**Envelope (the honest bottom line).** Lily's low-rank serve-loop recovers
*smooth, information-preserving* quality gaps — input compression, coherent skill
corrections — at ~100%. A *discrete compute-reduction* gap (sparse top-k
attention) is information-limited (~32% ceiling even full-rank) and the low-rank
serve path cannot reach the ceiling; the gate declines to serve a non-recovering
fit. That boundary is the deployment guidance: use Lily to recover representation
gaps, not to paper over aggressive sparse-attention compute cuts.

## Scope / not done (honest)
- Serving + collection are wired into the **DS residual runtime**; the GGUF token
  path and the q/k/v/o-projection variants are not.
- Multi-layer credit assignment through the free-running forward — the problem
  offline distillation hit — is **solved by serve-in-the-loop training** above
  (all-layer serving closes 100% on the smooth compression gap). The
  `registry_lora` certify/orchestrator machinery (teach → certify → serve,
  regression gate) is parametrization-agnostic and hosts Lily unchanged, for both
  the compression teacher and the compute-quality teacher.
- The **compute-quality teacher** is now characterised end to end (section
  above): real, multi-token, sparse-attention-driven, only ~32% recoverable even
  full-rank, and correctly rejected by the gate when a low-rank fit cannot
  recover it. Recovering more would need a full-rank / smooth-surrogate adapter,
  not a low-rank serve-loop — outside Lily's design point.
- Deep chains need EXACT-mode gradients; CNET's per-layer local-credit learner
  would not couple the shared `A` correctly. Raw-float storage is self-contained;
  the weight-store/streaming path can be adopted later.
- Deep chains need EXACT-mode gradients (implemented here) — CNET's per-layer
  local-credit learner would not couple the shared `A` correctly.
- Raw-float storage keeps the prototype self-contained; the weight-store /
  streaming path (as in `cce_lora`) can be adopted later.

## GGUF residual adapter hook (stub, Tier1.5)

The DS residual path owns Lily serving. A future GGUF token residual hook should
mirror `g_cce_layer_adapt_hook` with NULL-default byte-identical decode. Until a
local GGUF mouth is product-critical, adapters stay on DS + head-LoRA (JTC).
