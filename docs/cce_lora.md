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

## Live-serving in the executors (behind a flag)
The route and DAG executors apply attached adapters during execution, gated by
`reg->lora_serving_enabled` (zero-init = OFF, byte-identical legacy path). The
mechanism is a CCE-free global hook `g_cnet_lora_serve_hook` (declared in
router.h, defined in route.c) that `route_execute_ex` and `eval_node` call right
after `btn_forward` — so the delta is added *before* port validation and is what
gets served. Core router TUs stay CCE-free (verified: route.c/dag_full.c/
registry.c compile standalone; route_demo/dag_demo link without CCE). The hook is
installed only by the opt-in adapter layer:

- `registry_lora_enable_serving(reg)` — sets the flag + installs the hook.
- `registry_lora_disable_serving(reg)` — clears both, restoring the frozen base.

`test_registry_lora` proves it end to end through **both** real executors —
`route_execute_ex` (a 1-step RoutePlan) and `dag_execute` (a planned
single-primitive DAG): serving OFF → output is the **byte-exact** base (diff
0.0); serving ON → output == base + adapter delta (diff 0.0), and differs from
the base by the delta. Prototype scope: one active serving registry at a time
(last enable wins).

## Real-fault-queue validation (`make jtc_lora_faultq`)
The stronger test: build the queue from **only the unit's genuine runtime
misclassifications**, then check the adapter fixes them without regressing the
cases the unit already handles. Inputs are streamed through the actual
`route_execute_ex`; a fault is parked (via `registry_record_fault` +
oracle-labeled with `registry_supply_label`) only when the served tool ≠ the
hermetic-teacher tool. The adapter is taught on that fault-only queue and
evaluated on a held-out stream through the executor, serving on vs off:

Phase 1 streamed 900 inputs, parked **600 real faults** (66.7% error rate).
Held-out baseline 28.7%.

| rank | params | acc on | fixes (wrong→right) | regress (right→wrong) | net |
|-----:|-------:|-------:|--------------------:|----------------------:|----:|
| 4 | 104 | 75.7% | 153 | 12 | +141 |
| 8 | 208 | 80.0% | 173 | 19 | +154 |

An adapter trained on **only the real faults** is a clear net win (~13:1 fixes
to regressions at rank-4, +47 points), but the regressions are **real and
non-zero** — fault-only training perturbs a few of the correct cases, so a
production rollout should watch the right→wrong count, not just accuracy. rank-4
(104 params) again beats dense (144) on params.

## Certify-before-serve gate
An adapter reaches the executor only after passing a held-out check — the
regressions the fault-queue run exposed are gated on explicitly. `RegistryEntry`
gains one `int lora_certified` (NULL-init 0); the executor hook serves an adapter
only when `lora && lora_certified`, so `registry_teach_lora` attaches an
**uncertified candidate that never serves**. `registry_certify_lora(reg, name,
inputs, targets, n, policy, &report)` runs the adapter vs the base on held-out
data and sets the gate from the result:

- `policy.argmax_mode` — correctness = argmax match (classifiers) or per-sample
  squared error.
- `policy.max_regressions` — reject if right→wrong exceeds this.
- `policy.min_net_gain` — require `fixes − regressions ≥ this`.

`test_registry_lora` proves the gate: a freshly-taught adapter is uncertified and
the executor returns the **frozen base**; after `registry_certify_lora` PASS the
same executor serves base+delta; a policy the adapter can't meet clears the gate
again. In `jtc_lora_faultq` the gate runs in the real scenario — the fault-only
adapters pass a ≤5% regression policy (rank-4 +98/−4 on the val set) and get
served, while a zero-regression policy rejects (rank-8 +114/−9 → FAIL) and the
executor falls back to the base (accuracy stays at the 86/300 baseline).

## Orchestrator wiring (governed adapter maintenance)
The full lifecycle — teach → certify → serve — runs as a governed action in the
live orchestrator. `personal_ai_tick` (which already drives gap-lane, structure-
mine, and curiosity over `ai->lane.reg`) calls a CCE-free hook
`g_cnet_lora_tick_hook` once per tick; NULL by default, so the tick is
byte-identical unless armed. `registry_lora_install_orchestrator(reg, opts)`
arms both the serve hook and the tick hook (opt-in). Each pass,
`registry_lora_tick`:

- skips units with fewer than `min_faults` labeled queue pairs;
- for the rest, splits the queue train/holdout, teaches a rank-r adapter on the
  train split, and **certifies on the held-out split** (the gate);
- a PASS opens the serve gate (the executor now serves base+delta); a FAIL
  detaches the candidate. So an adapter only ever reaches production after
  clearing the held-out check — automatically, each tick.

`personal_ai.c` stays CCE-free (it only calls the fn-ptr hook); the action lives
in the `registry_lora` TU, installed by whatever entry point opts in.
`test_registry_lora` covers it: one governed `registry_lora_tick` teaches AND
certifies a faulted unit (train/holdout split) leaving it served, and skips units
below `min_faults`. Full orchestrator stack (jtc_lora_live links personal_ai) and
the no-CCE router targets build unchanged.

## End-to-end orchestrator run (`make personal_ai_lora_tick`)
Seals `json_toolcall_v2` into a fresh base, `personal_ai_open()`s it, streams
inputs through the real executor to park 598 oracle-labeled faults into the
unit's queue, installs the orchestrator, and runs `personal_ai_tick` — the actual
live loop, no GPU/teacher lane.

Policy: **cheap-adapter-first, dense-heal-fallback.** The adapter hook runs
*ahead* of `gap_lane`'s dense heal in `personal_ai_tick`, and on a PASS
`registry_lora_tick` marks the unit non-RESET — so `gap_lane`'s heal (gated on
`PRIM_RESET` in `specialist_health`) skips it. The two are mutually exclusive per
unit. Both branches are proven end-to-end through the real tick:

| scenario | gate | certified | base student | served | who retrained |
|----------|------|:---------:|:------------:|:------:|---------------|
| A reasonable | passes | yes | 35% (heal skipped) | **78%** | the low-rank adapter |
| B strict | rejects | no | **97%** (heal ran) | 97% | gap_lane dense heal (fallback) |

So the cheap ~200-param adapter is tried first (78% here); only when it fails to
certify does the expensive dense retrain run (97%). That's the tradeoff the
policy buys — cheaper, slightly less accurate, with dense as the safety net.

**Regression-aware in-tick gate + safe training.**
`registry_lora_tick_opts.validate` is an optional per-unit sampler
`(unit, in, out, inputs, targets, cap, ctx) → n` returning a representative set
(base-correct AND base-wrong). When provided, the tick (a) **trains on the fault
queue ∪ a representative sample**, so the adapter learns delta≈0 on
already-correct cases and doesn't regress a good base, and (b) **certifies on a
fresh representative sample**, so the gate measures *regressions* (a fault-queue
holdout is all base-wrong and can only see fixes). The sampler is
orchestrator-supplied (it owns the unit's oracle); `registry_lora.c` only calls
the pointer. When `validate` is NULL the tick trains on a queue split and
certifies on a fault-queue holdout (fixes only).

**Recorded production traffic stream — wired to the host's real log.** The host
records traffic and the orchestrator replays it, across the language boundary:

- **Host capture (.NET):** `JsonToolCall.ClassifyOrGap` — the choke point every
  real request flows through — calls `JsonToolCall.LogTraffic`, which appends the
  request JSON (one single-line record per line) to `CNET_JTC_TRAFFIC_LOG`.
  Env-gated, read per call, failures never touch serving; off by default.
- **Replay (C):** `personal_ai_lora_tick` reads `CNET_JTC_TRAFFIC_LOG` when set
  and loadable (else a local synthesized capture), and replays it in three
  disjoint slices — fault-mining, in-tick validation, held-out eval — so the
  fault queue and the sampler read the **same captured stream**.

Proven end-to-end across the boundary: a `.NET` test run with the env set produces
a 2000-line host log via `LogTraffic`, and the C replay consumes it
(`HOST LOG via CNET_JTC_TRAFFIC_LOG`). On that real host traffic the certified
student is already ~93%, and the gate makes a **distribution-dependent** call —
here the low-rank adapter regresses just past the reasonable threshold, so the
gate declines it and gap_lane's dense heal serves (96%); on the synthesized
distribution the adapter certifies and serves (+3). The harness now asserts the
**safety invariants** that hold on any traffic (tick runs; unit never ends below
baseline; a certified adapter serves base+delta, a declined one is blocked and
the base is served) and *reports* which branch fired, rather than demanding one.

### Done
The cce_lora arc is complete: adapter core → benchmark → real-queue wiring → live
unit → executor serving (route + DAG) → real-fault validation → certify-before-
serve gate → orchestrator-driven governed maintenance → cheap-first/dense-fallback
ordering → regression-aware gate + safe training → recorded-stream replay.
- Try `diff_mode=EXACT` via the `cce_learn` cascade path as an alternative
  trainer; measure vs the explicit Adam here.
- Head-width `B:[r,out]` grows with vocab for a true logit head — benchmark the
  hidden→hidden projection variant for large `V`.
- No GPU teacher-lane involvement; trains on CPU over the existing labeled queue.
