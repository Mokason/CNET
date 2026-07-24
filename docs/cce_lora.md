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

### Not done / next
- Wire `cce_lora_fit_residual` into `registry_label_via_teacher` behind an opt-in
  flag, and measure on a real `json_toolcall_v*` unit's port-validated queue
  (this prototype uses synthetic low-rank+noise residuals).
- Try `diff_mode=EXACT` via the `cce_learn` cascade path as an alternative
  trainer; measure vs the explicit Adam here.
- Head-width `B:[r,out]` grows with vocab for a true logit head — benchmark the
  hidden→hidden projection variant for large `V`.
- No GPU teacher-lane involvement; trains on CPU over the existing labeled queue.
