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

## Scope / not done (honest)
- This is the **parametrization + training + generalization benchmark** on a
  synthetic deep linear stack. It deliberately does **not** wire deltas into a
  real deep model's serving forward — that interior-layer injection is the
  invasive part flagged in the design note (the current `cce_lora` serve hook
  only adds one delta after `btn_forward`).
- The `registry_lora` certify/orchestrator machinery is parametrization-agnostic
  and would host Lily unchanged (teach → certify → serve, same regression gate).
- Raw-float storage keeps the prototype self-contained; the weight-store /
  streaming path (as in `cce_lora`) can be adopted later.
- Deep chains need EXACT-mode gradients (implemented here) — CNET's per-layer
  local-credit learner would not couple the shared `A` correctly.
