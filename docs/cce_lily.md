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
  recovers the gap. General multi-layer free-running credit needs **serve-in-the-
  loop training** (run the forward *with* the adapter and adjust) — the honest
  next step, not the offline per-layer target used here.

## Scope / not done (honest)
- Serving + collection are wired into the **DS residual runtime**; the GGUF token
  path and the q/k/v/o-projection variants are not.
- **Multi-layer credit assignment through the free-running forward is the open
  problem.** Offline per-layer distillation compounds; last-layer serving works;
  the general fix is serve-in-the-loop training (needs the forward in the training
  loop). The `registry_lora` certify/orchestrator machinery (teach → certify →
  serve, regression gate) is parametrization-agnostic and would host Lily
  unchanged.
- Deep chains need EXACT-mode gradients; CNET's per-layer local-credit learner
  would not couple the shared `A` correctly. Raw-float storage is self-contained;
  the weight-store/streaming path can be adopted later.
- Deep chains need EXACT-mode gradients (implemented here) — CNET's per-layer
  local-credit learner would not couple the shared `A` correctly.
- Raw-float storage keeps the prototype self-contained; the weight-store /
  streaming path (as in `cce_lora`) can be adopted later.
