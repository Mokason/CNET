# cce_lora — rank-r low-rank adapter (prototype)

A parameter-efficient alternative to the dense hidden→output SGD teaching path:
capture a per-skill output correction as `(alpha/rank)·(xA)B` (`A:[in,rank]`,
`B:[rank,out]`) instead of a full `in×out` update. The frozen base is untouched;
only `A` and `B` train. Motivated by the frozen-backbone + low-rank-PEFT pattern
in NetLLM (SIGCOMM 2024), adapted to CNET's verify-first, local-credit stack —
we take the low-rank half and drop the RL half.

Files: `include/cce/cce_lora.h`, `src/cce/cce_lora.c`, tests `tests/cce_lora_test.c`
(`make cce_lora_test`) and `tests/bench_cce_lora.c` (`make cce_lora_bench`).

## What it does
- **Apply / merge** — `y += (alpha/rank)·(xA)B`; `cce_lora_merge` folds the delta
  into a dense `W` for zero inference overhead. Math is explicit (not
  `cce_block_forward`, whose LINEAR path applies a sigmoid) so the delta is an
  exact bilinear map. `B` is zero-initialised → an untrained adapter is a proven
  no-op.
- **Train** — full-batch Adam on the residual `teacher_out − base_out`
  (`cce_lora_train` / `cce_lora_fit_residual`); base frozen.
- **Store / stream** — `cce_lora_to_cascade` emits a 2-block `LINEAR_HEAD`
  cascade, so the adapter packs (trit/int4) and demand-loads through the existing
  weight-store/expert path. Round-trips exactly.
- **Integrate (M4)** — `cce_lora_head_forward(head, lo, in, out)` = frozen head +
  optional delta; `lo == NULL` is the exact base with zero overhead. No change to
  the shared `cce_block` struct or its serialization; the adapter is resolved
  per-skill by the caller (router). `cce_lora_fit_residual` is the router-facing
  training hook, **off by default**.

Correctness (`make cce_lora_test`, all green): untrained apply is a bit-exact
no-op; apply-delta equals merged-W (≤1e-6); training drives a planted low-rank
residual to ~3e-14 and generalises to held-out inputs (~7e-7); save/load and the
cascade bridge round-trip exactly; `head_forward(NULL)` is the exact base and
`head_forward(adapter)` reproduces the teacher on held-out x.

## Benchmark (`make cce_lora_bench`) — dense output-SGD vs rank-r
Same `(input, residual)` data, same Adam, `in=256 out=128`, 512 train / 256 held
out, noise 0.03. Isolates the effect of rank.

**Low-rank correction (intrinsic rank k=4 — the skill-teaching regime):**

| student | params | teach ms | bytes fp32 | bytes trit | held MSE | pass% |
|---------|-------:|---------:|-----------:|-----------:|---------:|------:|
| dense   | 32768  | 557 | 131072 | – | 0.00060 | 100% |
| rank-2  | 768    | 182 | 3072   | 2765 | 3.49985 | 0% |
| **rank-4** | **1536** | **238** | **6144** | **3994** | **0.00031** | **100%** |
| rank-8  | 3072   | 352 | 12288  | 6452 | 0.00032 | 100% |
| rank-16 | 6144   | 615 | 24576  | 11367 | 0.00032 | 100% |
| rank-32 | 12288  | 1095 | 49152 | 21197 | 0.00034 | 100% |

→ **rank-4 matches dense at 21.3× fewer params, ~2.3× faster teaching, ~33×
smaller storage** — and slightly *beats* dense held-MSE because the low-rank form
regularises away the noise dense overfits.

**Near-full-rank correction (k=96 — the honest boundary):**

| student | params | held MSE |
|---------|-------:|---------:|
| dense   | 32768  | 0.00060 |
| rank-32 | 12288  | 2.55351 |

→ no rank matches dense; a near-full-rank correction genuinely needs the dense
update.

## Verdict
When a skill's output correction is approximately low-rank — the common case —
a small adapter (rank ≈ intrinsic rank) matches or beats dense teaching at
~20× fewer trained params, faster teach time, and packs like an expert. Dense is
only needed for near-full-rank corrections. Teach-time crossover is ~rank-16 (the
current apply/grad loops are less cache-optimised than the dense path); the win
is at the small ranks that matter.

## Registry wiring (real skill queue)
`include/router/registry_lora.h` + `src/router/registry_lora.c` train and serve
an adapter from a **real** unit's retrain queue. `RegistryEntry` gains one
borrowed, NULL-initialised `struct cce_lora *lora` (forward-declared — core
registry TUs pull in no CCE dependency and no new link deps); all adapter logic
lives in the separate `registry_lora` TU, linked only by targets that opt in.

- `registry_teach_lora(reg, name, opt, stats)` — reads the unit's LABELED
  `RetrainQueue` pairs, forms `residual = teacher_target − btn_forward(base)`,
  trains a rank-r adapter, and attaches it to the entry. The base BTN is never
  modified.
- `registry_forward_with_lora(reg, name, input, out)` — `out = btn_forward(base)
  + delta`; with no adapter it is the exact base (zero overhead).
- `registry_lora_detach` / `registry_has_lora` — lifecycle.

`make registry_lora_test` (all green) builds a real BTN, parks faults +
`registry_supply_label`s teacher-corrected targets into the genuine queue, then
teaches and serves: 300 pairs, in=16/out=8/rank=4, **pre_mse 7.35e-5 →
post_mse 7e-11**, held-out adapter-vs-teacher L1 = 0.0000 (base-vs-teacher
0.0073), 96 params vs 128 dense, and detach restores the byte-exact base.

## Live json_toolcall run (`make jtc_lora_live`)
`registry_teach_lora` on the **real** certified `json_toolcall_v2` unit: mine +
admit the genuine student (external teacher = `cnet_jtc_hermetic_teacher`), fill
its retrain queue with 500 (feature → teacher one-hot) pairs, teach, and serve.
The student is 8/8 on the canonical exemplars but generalises to **32.8%** on a
broader sampled input distribution (sparse feature activations labeled by the
same teacher) — it was certified against a finite exemplar set. The adapter
closes that gap:

| rank | params | held-out acc | vs baseline |
|-----:|-------:|-------------:|------------:|
| 2 | 52 | 58% | +24.8 |
| **4** | **104** | **80%** | **+47.2** |
| 8 | 208 | 82% | +49.2 |

Baseline 32.8%; dense-equivalent output update = 144 params. **rank-4 lifts
held-out accuracy +47 points at *fewer* params than dense** — the sweet spot.
Honest note: for this tiny 18→8 head, rank-8 (208) exceeds dense (144), because
rank-8 is full rank for an 8-output layer — low-rank only compresses when
`rank < min(in,out)`; the parameter win lives in wide layers (see the bench),
while the accuracy win holds even here. The +47 points is measured on the broad
sparse-feature distribution; the production gain depends on the real tool-call
input distribution.

### Not done / next
- Call `registry_forward_with_lora` from the live executors (route.c/dag_full.c)
  behind a registry flag, so a trained adapter reaches production serving.
- Repeat on a production queue populated by real faults (this run samples inputs
  and labels them with the shared teacher rather than harvesting live faults).
- Try `diff_mode=EXACT` via the `cce_learn` cascade path as an alternative
  trainer; measure vs the explicit Adam here.
- Head-width `B:[r,out]` grows with vocab for a true logit head — benchmark the
  hidden→hidden projection variant for large `V`.
- No GPU teacher-lane involvement; trains on CPU over the existing labeled queue.
