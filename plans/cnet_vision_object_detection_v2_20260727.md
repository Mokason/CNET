# CNET vision object-detection benchmark — V2 FROZEN PROTOCOL (2026-07-27)

**Status: §1–§7 were written as pre-registration — before V2 features were extracted,
before the V2 head was trained, and before any V2 holdout image was scored. §8–§11 were
filled afterwards from the single scored run.**

V1 (`plans/cnet_vision_object_detection_20260727.md`, commit `6ffc5f1`) is **closed and
immutable**. Its verdict `VISION_DETECTION_MECHANISM_WITHHELD` at AP50 0.046434 stands,
its 1000 scored test images are **spent**, and nothing in V2 may re-score, re-tune
against, or re-interpret them.

Branch `feature/vision-object-detection-benchmark`. Local commits only.

---

## 1. The one hypothesis under test

V1's own §14 named the feature description as the top blocker: 32×32 **grayscale** HOG
reduced to 64-D cannot describe a car. The intended change is exactly that:

| | V1 (spent) | V2 |
|---|---|---|
| Proposal crop → | 32×32 **gray** | 64×64 **colour (BGR)** |
| HOG | 324-D | **1764-D** |
| PCA (train-only) | 64-D | **256-D** |

Held fixed by construction: Selective Search Fast at width 300, class-agnostic,
`MAX_PROP` 300, min proposal side 16 px, class `car`, seed `20260727`, NMS IoU 0.30, the
evaluator, the label rule (IoU≥0.5 pos, 0.3–0.5 ignore), hard-negative mining (2 rounds,
threshold 0.5, 15/image, train only), BTN shape (24/192), and every PASS bar.

**Two further things do differ between the V1 and V2 runs, and are named here rather than
buried:** the holdout is a different 1000 images (§2 — V1's are spent), and *which* 300
Selective Search candidates are kept changed as a result of the determinism repair in §3b.
Both were fixed before any V2 number existed. Their consequence for what may be claimed is
spelled out in §10: the V2 **frontend as a whole** is what the bars are evidence for, and
attribution of the improvement to the descriptor alone is withheld.

**Colour HOG is a real change, not a relabelling.** Probed directly: OpenCV's
`HOGDescriptor::compute` on a 3-channel BGR crop uses the per-pixel maximum-magnitude
channel gradient, giving L1 difference 66.4 against the grayscale descriptor of the same
crop at identical 1764-D. Recorded in `logs/vision/v2_hog_probe.log`.

## 2. Held-out source reservation — V1 test is spent

The official VOC2007 test list (4952 IDs) is shuffled **once**, with the same
seed-`20260727` splitmix Fisher–Yates V1 used. That shuffle is a fixed permutation:

- `shuf[0 … 999]` — **V1 test. Spent. Never touched again.**
- `shuf[1000 … 1999]` — **V2 holdout (1000 images). Reserved here, before training.**
- `shuf[2000 … 4951]` — unspent (2952 images), reserved for any V3.

V2's holdout is therefore disjoint from V1's **by construction**, drawn from the 3952
official test images V1 never saw. Class-agnostic as in V1: images are taken regardless
of whether they contain a car, so false-positive opportunity is preserved.

**Asserted, not assumed.** Three independent checks, each fatal:

1. `vd_prep` loads the actual scored V1 holdout IDs out of `data/vision_cache/test.pack`
   (the real artefact, not a re-derivation) and fails on any ID intersection with V2.
2. `vd_prep` SHA-256s every V1 holdout JPEG and every V2 holdout JPEG and fails on any
   **content** intersection — a duplicate image under a different ID cannot slip through.
3. `vd_bench` repeats the ID check at the scoring boundary against the V1 pack, so a
   mismatched cache directory cannot produce a scored number.

Train/val are rebuilt from the official trainval list with the **same content-hash split**
V1 used (FNV-1a of the image SHA-256, `%5==0` → val), so train/val membership is
unchanged from V1 and V2's holdout is disjoint from them by the existing assertions.
The three duplicate pairs V1 found inside trainval remain handled by content hashing.

## 3. PCA fitting — train only, bounded

1764-D raw features for all ~1.05 M train proposals would need ~7.4 GB resident plus a
mean-subtracted copy inside the PCA solve. To stay inside the CPU/memory budget, the V2
PCA basis is fitted on the proposals of the **first 600 images of the train split**
(deterministic list order, ~160 k proposals — 625× the 256 components being estimated).

This is a **memory/CPU parameter fixed here, before any validation or test number
exists.** It is not tuned, and it will not be changed after seeing results. The fitted
mean and eigenbasis are frozen and applied verbatim to train, val and holdout.

Extraction is two-phase so proposals are generated once: phase A runs Selective Search and
stores boxes/labels for every image (retaining raw features only for the PCA-fit
subsample); phase B recomputes HOG for those exact stored boxes and projects. Boxes are
therefore identical between the phase that fits PCA and the phase that uses it.

## 3b. Determinism repair to proposal selection (found during V2 build)

Probing Selective Search directly (`logs/vision/v2_ss_determinism.log`) showed that
OpenCV returns a **deterministic candidate set in a nondeterministic order** — it perturbs
region priority with the global `rand()`. Over 30 images:

| | |
|---|---|
| candidate set differs (order-insensitive) | **0 / 30** |
| returned sequence differs | **30 / 30** |
| naive "first 300" subset differs | **29 / 30** |

So V1's `MAX_PROP = 300` truncation was selecting a *different random subsample* of the
~900 candidates on every run and at every worker count. V1's internal comparisons are
unaffected — every V1 arm scored the same baked proposal set — but V1's extraction was
not reproducible, and V2's would not have been either.

**Fix, pre-registered here before any V2 number exists:** canonically sort the candidate
rects by `(x, y, w, h)`, then, if there are more than 300, subsample deterministically
with a splitmix shuffle seeded by `fnv1a(image_id) ^ 20260727` and re-sort. Selection now
depends only on the image, not on run order or thread count. The semantics are unchanged
— still a class-agnostic uniform subsample of the same candidate set, still capped at 300.

This is a correctness repair, not a tuning knob: it is adopted for reproducibility, chosen
before any V2 validation or holdout number was produced, and it is not permitted to be
revisited after results are seen. Consequence to state plainly: re-running V1 prep today
would produce a different (now reproducible) proposal subsample than the V1 artefact in
commit `6ffc5f1`. That artefact and its verdict stay frozen as the V1 record.

## 4. Inference contract (unchanged)

At test time the detector receives **only the decoded full image**. No holdout ground
truth, annotation count, or object count reaches the model. GT is used only to label
train/val proposals and inside the evaluator after predictions exist. No
GT-proposals-at-test path exists.

## 5. What is fixed vs learned (unchanged in kind)

| Component | Status |
|---|---|
| Selective Search Fast, class-agnostic, width 300 | fixed algorithm, nothing learned |
| 64×64 colour HOG, 1764-D | fixed, deterministic |
| PCA → 256-D | **learned on train only** (§3), frozen |
| BTN head `PORT_RAW` 256 → `PORT_ONEHOT` 2 | **the CNET-learned component under test** |
| Pretrained detector / weights | **none, anywhere** |

## 6. PASS bars — carried over from V1 unchanged

**Level A — `VISION_DETECTION_MECHANISM_PASS`** requires all of:

1. `AP50_holdout ≥ max(0.10, 0.50 × AP50_val)`
2. `AP50_holdout(trained) ≥ 3 × AP50_holdout(randomized head)` **and** absolute margin `≥ 0.05`
3. Proposal recall reported (diagnostic, no bar)
4. Deterministic rerun `|AP50 − AP50_rerun| ≤ 1e-9`
5. Zero ID and content-hash leakage, including V1↔V2 holdout disjointness
6. Machine-readable JSON with honest counts

The floors are **identical to V1's**. They are not lowered, and the near-miss margin V1
recorded (0.0457 against a 0.05 bar) is not an argument for moving the bar.

**The single parameter chosen on V2 validation** is the reported precision/recall
confidence threshold, swept over 0.05–0.95 on **validation only** and maximising
validation F1. V2 removes V1's hardcoded `0.55` and threads the validation-selected value
into the scored run programmatically, so no test number can influence it. AP50 — the
bar-bearing metric — is threshold-free.

**Level B — `VISION_CAPSULE_PORTABILITY_PASS`**: unchanged, and still gated on its own
independent requirement. `coverage_family_gated()` (`src/hybrid_ai.c:140`) still admits
only `PORT_ONEHOT`/`PORT_BINARY_*`, so `PORT_RAW` continuous features have **no honest
coverage gate**: the strict query refuses everything, the legacy query admits everything.
V2 does not change that code, so **Level B stays WITHHELD regardless of Level A.**

**Level C — `VISION_SPECIALIST_COMPETES_PASS`**: no pretrained reference detector is run
in this pass either. **Stays WITHHELD**, reason recorded.

## 7. Controls re-run in full

Randomized/untrained head of identical shape · logistic baseline on identical features ·
label-shuffle control, evaluated with the identical full pipeline on **both** validation
and the holdout · proposal-recall ceiling · determinism rerun · ID and content-hash
leakage assertions · V1↔V2 holdout disjointness · evaluator's analytic fixtures ·
evidence-gate negative controls · sanitizer coverage as scoped in §12.

## 8. Validation results and frozen threshold

```
VAL proposal_recall=0.806867 (188/233)
VAL ap50_cnet=0.088587 ap50_random=0.000875 ap50_linear=0.010822 best_thr=0.70 best_f1=0.1852
VAL ap50_labelshuffle=0.002049
VAL_FROZEN_BARS ap50_floor=0.100000 ratio_floor=3.0 margin_floor=0.05 thr=0.70
```

Hard-negative mining converged as in V1: round 0 added 38 611 confident false positives
(V1: 14 972 — the richer features produce more confident mistakes to learn from), round 1
only 1 338.

Floor from the protocol formula: `max(0.10, 0.50 × 0.088587) = 0.10`. Threshold frozen at
**0.70** on validation F1. Nothing below fed back into either.

## 9. Holdout results — single scored run

```
TEST proposal_recall=0.828244 (217/262)
TEST ap50_cnet=0.114518 ap50_random=0.000988 ap50_linear=0.009932 ap50_labelshuffle=0.002435
TEST precision@0.70=0.156951 recall@0.70=0.267176 detections=75989 gt=262
TEST determinism |ap-ap_rerun|=0
BARS floor=0.100 -> PASS | ratio>=3.0 -> PASS (115.9x) | margin>=0.050 -> PASS (0.1135) | det -> PASS
EVIDENCE manifest=1 prev_holdout=1 leakage=1 eval_fault=0
```

### Evidence binding (hardening round 2)

Identity is no longer self-described. The scoring target passes `--protocol v2`, and the
protocol constants live in `tools/vision_detection/vd_protocol.c`: schema version, dataset,
class, seed, holdout offset/count, train/val counts, PCA-fit provenance, descriptor and
proposal parameters, the required previous-holdout count, its pinned digest, and canonical
**ID and content roots** (sha256 over the sorted canonical lines) derived from the official
VOC archives and committed in `vd_roots.h`. The bench recomputes every root from the
cache's sidecars and requires it to equal both the manifest's claim and the pinned
constant. `requires_prev` is a protocol constant, so a cache cannot declare its way out of
the spent-holdout check.

The bench also **measures** leakage rather than reading it: cross-split ID overlap 0,
cross-split content overlap 0, and **3 intra-split duplicate images** — an independent
confirmation, from the artefacts, of the three VOC2007 trainval duplicate pairs recorded in
§2. Since content-addressed splitting puts both copies of a duplicate on the same side,
intra-split duplicates are expected and are reported separately from leakage.

Level A now requires all four evidence predicates alongside the metric bars. This was not
theoretical: one scored run in this round returned **WITHHELD with all four metric bars
passing**, because a gate rewrite had removed the bench's own ID-disjointness computation
and `leakage` was therefore 0. The verdict was withheld until the check was restored — the
fail-closed design behaving exactly as intended on a real run.

**Reproduced three times** with identical bar-bearing metrics, the last run under the full
hardened evidence gate (verified artefact hashes, hash-bound spent-holdout check,
transactional publication). The label-shuffle control now also runs the identical full
evaluation on the holdout: **0.002435**, i.e. the head scores 47× that with real labels.

| Bar | Required | Measured | Result |
|---|---|---|---|
| Evidence: manifest / spent-holdout / leakage / eval-fault | all | 1 / 1 / 1 / 0 | **PASS** |
| Absolute AP50 | ≥ 0.100 | **0.114518** | **PASS** |
| Ratio vs randomized head | ≥ 3.0× | **115.9×** | **PASS** |
| Absolute margin vs randomized | ≥ 0.050 | **0.113530** | **PASS** |
| Determinism | ≤ 1e-9 | **0** (exact) | **PASS** |
| Leakage (ID, content, spent V1 holdout) | zero | zero / zero / zero | **PASS** |

### Verdict

```
VISION_DETECTION_MECHANISM_PASS
VISION_CAPSULE_PORTABILITY_WITHHELD   (PORT_RAW still has no honest coverage gate)
VISION_SPECIALIST_COMPETES_WITHHELD   (no pretrained reference detector run)
```

Level A is **earned under the bars set before the run**, on 1000 VOC2007 test images that
had never been scored. Level B and C are unchanged and remain withheld — neither of their
independent gates was solved in this pass, and Level A passing does not unlock them.

## 10. V1 → V2: what the feature change bought

| | V1 (32×32 gray → PCA64) | V2 (64×64 colour → PCA256) |
|---|---|---|
| holdout images | 1000 (slice 0–999, spent) | 1000 (slice 1000–1999) |
| holdout GT | 237 | 262 |
| proposal recall | 0.772152 | **0.828244** |
| AP50 CNET head | 0.046434 | **0.114518** (2.47×) |
| AP50 randomized head | 0.000733 | 0.000988 |
| ratio | 63.3× | **115.9×** |
| AP50 linear baseline | 0.017864 | 0.009932 |
| AP50 label-shuffle (holdout) | not run on test | **0.002435** |
| verdict | WITHHELD (floor + margin) | **MECHANISM PASS** |

**What passed is the complete V2 frontend, and causal attribution of the 2.47× to the
descriptor alone is WITHHELD.** Three things differ between the V1 and V2 rows above, not
one: the feature descriptor (32×32 gray/PCA64 → 64×64 colour/PCA256), the holdout (slice
`[0,1000)` → `[1000,2000)`, different images and different GT counts), and the proposal
selection (the §3b determinism repair changed *which* 300 candidates are kept, and roughly
doubled the positive proposals available for training: 3572 → 7027).

The descriptor hypothesis from V1 §14 remains **consistent** with the result, and no
observation contradicts it — but a 2.47× movement across three simultaneous changes cannot
be assigned to one of them. Establishing that would need an ablation holding the holdout
and the proposal set fixed and varying only the descriptor. That ablation was not run, so
the honest claim is the weaker one: **the V2 frontend as a whole clears the bars.**

Two honest caveats on the surrounding numbers:

1. **Holdout scored above validation** (0.1145 vs 0.0886). Different 1000-image samples
   with different difficulty; the holdout's proposal recall is also higher (0.828 vs
   0.807). No tuning touched the holdout, and the binding floor was the absolute 0.10, not
   the `0.5 × val` term. Still worth stating rather than presenting the higher number as
   if it were the expected one.
2. **The linear baseline got *worse* from V1 to V2** (0.0179 → 0.0099) despite strictly
   richer features. Its learning rate and epoch count were carried over unchanged from the
   64-D configuration and are almost certainly ill-conditioned for 256-D PCA inputs, whose
   leading components have far larger variance. So the baseline is **under-fit, and is a
   weak reference** — "CNET beats logistic regression 11.5×" should not be read as a
   strong claim. It was deliberately **not** retuned, because retuning a control after
   seeing holdout numbers is exactly the move this protocol exists to prevent. Fixing the
   baseline (feature standardisation, swept lr) is listed as V3 work below.

The bar-bearing evidence does not depend on that baseline: the randomized-head ablation
(115.9×, same shape, same features, same proposals) and the label-shuffle collapse
(0.0020) are what establish that the CNET-learned head is causally responsible.

## 10b. Multicore policy — pre-registered before the run

The benchmark is dominated by four strictly sequential BTN trainings whose update
order is fixed by the protocol and must not change. Profiling the accepted serial run:
main-head training `train_s` ≈ 2667–2975 s of a ≈ 3540 s wall, with the label-shuffle
control training a further ≈ 700–900 s and everything else (pack loading, the linear
baseline, all evaluations) under ≈ 200 s.

**Strategy: process-level overlap of one independent arm.** After the final
hard-negative round appends its rows, the training matrix is frozen. The final main-head
training and the label-shuffle control training both consume that same frozen matrix and
are independent of each other, so the control is run in a `fork()`ed worker while the
parent performs the final main training. Only the *final* main training is deferred past
the fork — the earlier hard-negative retrainings must exist before the next scan, so they
stay in place. The child inherits the frozen matrix copy-on-write, starts from the same
RNG state, uses the same seed, and issues the identical `btn_train_dynamic` call.
**No BTN update order is changed and no training is multithreaded.**

Concurrency is capped at **2 processes** (`--jobs 2`, clamped in code — only one
independent arm exists). `OMP_NUM_THREADS=1` and `OPENBLAS_NUM_THREADS=1` are set to
prevent library oversubscription, and OpenCV is not involved in the scoring binary.

**Pre-registered tolerance: exact equality, tolerance 0.** Every metric below must be
bit-identical to the serial run or the optimisation is reverted:

| quantity | required value |
|---|---|
| `ap50_cnet_test` | 0.114518 |
| `ap50_random_head_test` | 0.000988 |
| `ap50_linear_baseline_test` | 0.009932 |
| `ap50_label_shuffle_test` | 0.002435 |
| `VAL ap50_cnet` / `ap50_labelshuffle` | 0.088587 / 0.002049 |
| proposal recall (holdout) | 0.828244 |
| detections | 75989 |
| determinism `|ap − ap_rerun|` | 0 |
| artifact root | `8c70c932…4ce4908b` |

If any differs, the fork is reverted and the run repeated serially. The holdout is scored
once, after the worker has been reaped; there is no additional peeking.

### Measured result

| | serial baseline | multicore run |
|---|---|---|
| processes / live core cap | 1 | **2** (`--jobs 2`, clamped in code) |
| main-head training `train_s` | 2975.0 s | **2975.0 s** (identical) |
| final main training (post-fork) | — | 1115.4 s |
| label-shuffle control training | — | 1117.0 s (concurrent) |
| average CPU utilisation | ~100 % | **137 %** |
| peak RSS | — | **1.92 GiB** |
| **wall clock** | **3540 s (59:00)** | **2984.9 s (49:45)** |
| **speedup** | — | **1.19×** |

**Every pre-registered quantity reproduced exactly**, tolerance 0: AP50 0.114518, random
0.000988, linear 0.009932, shuffle 0.002435, val 0.088587 / 0.002049, proposal recall
0.828244, detections 75989, determinism 0, artifact root `8c70c932…`. `train_s` being
identical to the baseline is the control that makes the wall comparison meaningful.

**The honest speedup is 1.19×, not the 1.37× a naive overlap accounting suggests.** The
concurrent window is 1115 s, but the two trainings contend for memory bandwidth and both
dilate, so the real saving is the measured 555 s. The console field is named
`overlap_window_s` rather than `saved` for exactly this reason.

### Second multicore run (round-4 code)

| | baseline | run 1 | run 2 | run 3 (round-5 code) |
|---|---|---|---|---|
| `train_s` (main head) | 2975.0 s | **2975.0 s** | 2611.6 s | 2665.9 s |
| wall | 3540 s | 2984.9 s | 2622.4 s | **2674.8 s** |
| apparent speedup | — | **1.19×** | 1.35× | 1.32× |
| CPU / peak RSS | ~100 % | 137 % / 1.92 GiB | 137 % / 1.92 GiB | 137 % / 1.92 GiB |

**The claimed speedup is 1.19×.** Only run 1 is a controlled comparison: its `train_s`
matched the baseline *exactly* (2975.0 s both times), so its wall reduction is attributable
to the overlap. Runs 2 and 3 each had a `train_s` 10–12 % below the baseline, meaning part
of their larger apparent speedups is machine variance in the training phase itself, not
parallelism. The observed range is **1.19×–1.35×** and the conservative end is the one
reported; the larger figures are recorded but not claimed.

Every multicore run produced bit-identical metrics and the identical
`btn_calls_total = 3427852`.

The multicore runs produced bit-identical metrics and an identical
`btn_calls_total = 3427852`, which is a further determinism signal: the same number of BTN
initialisations and forward passes was executed in each.

**What the worker evaluates, precisely.** The forked worker trains the label-shuffle head
and evaluates it on validation *and* on the holdout before the parent reaps it. That
holdout evaluation is a **pre-registered control** (§7, §10): the shuffle arm's AP is
recorded as evidence that the head is learning labels rather than a proposal artefact. It
is not tuning — nothing about the model, threshold, bars or protocol is derived from it,
and it is computed with the identical full pipeline used for every other arm. The
bar-bearing holdout evaluation of the main head happens once, in the parent, after the
worker has been collected.

**Staging is never deleted, by anything.** Identity-bound directory removal is not
expressible in portable POSIX — the inode can only be checked and then removed by
pathname, and that gap cannot be closed. So `vd_stage_abort` deletes nothing under any
condition: it releases held descriptors and reports the unique staging path, and every
`vd_prep` failure after staging begins funnels through one cleanup that prints
`VD_STAGE_QUARANTINE <path>`. Reclaiming quarantined directories is deliberately an
operator/offline task; no automatic collection is performed.

**Remaining Medium, stated precisely rather than overclaimed.** Publication checks the
staging inode immediately before `renameat2(RENAME_NOREPLACE)` and again immediately
after. If the staging *pathname* were re-pointed at a different directory between those
two operations, a wrong inode could become visible at the destination. The post-check
detects this and fails closed, marking the destination as quarantine — but it does **not**
delete or undo it, because that would reintroduce exactly the deletion gap this design
removed. The practical consequence is bounded: a cache published that way cannot be
scored, because `vd_bench` recomputes the artifact root from its own held descriptors and
requires it to equal the constant pinned in `vd_protocol.c`. This is a fail-closed
residual, not a prevented one, and is recorded here as such.

**Publication is fresh-only.** In-place cache replacement was retired entirely rather than
made safer. Publication now only ever creates an **absent** destination, via
`renameat2(RENAME_NOREPLACE)` from the held parent dirfd under a parent-local no-follow
lock; an existing destination is a hard refusal and no path — successful or otherwise —
deletes or swaps anything. `vd_prep --replace` returns a nonzero retired diagnostic and
callers choose a fresh `--out` path.

**No verdict survives an interruption.** The signal handler has two
async-signal-safe states. While the worker is outstanding it records the signal and TERMs
the child, returning so normal control performs the bounded reap. Once the child has been
collected there is nothing left to reap, so a caught SIGINT/SIGTERM exits immediately with
`128 + sig`. That closes the window in which a signal arriving after the reap but before
handler restoration could still have produced a results file and a verdict. In addition,
a recorded interruption is rechecked before the worker result is accepted, before handlers
are restored, before the JSON is built, before it is published, and before every verdict
marker — so no bar-bearing artefact can outlive an interruption.

**Worker signal safety.** `SIGINT`/`SIGTERM` are blocked and `sigaction` handlers installed
*before* `fork()`, so there is no interval in which a signal can arrive with no handler, or
with a handler but no child PID to act on. The child restores default dispositions and the
original mask before doing any work. The parent publishes the child PID and the absolute
deadline while still blocked and only then restores the mask, so a signal delivered inside
that window is merely pending and reaches a fully initialised handler. The original actions
and mask are restored exactly on every exit path.

**What was deliberately not done.** The dominant cost is four sequential BTN trainings
(`train_s` 2975 s of a 3540 s serial wall) whose update order is fixed by the protocol.
Parallelising *inside* `btn_train_dynamic` would change that order and is out of scope, so
it was not attempted. The remaining candidates were measured and rejected as below run
variance: parallelising the hard-negative scan and the detector forward passes together
account for well under 60 s, and the linear baseline under 20 s. Only one genuinely
independent arm exists at 2 processes, which is why `--jobs` is clamped to 2 rather than
consuming available cores.

## 11a. Sanitizer scope — stated precisely

ASan+UBSan coverage is **not** the whole benchmark, and should not be read as such:

| Component | Sanitizer | How |
|---|---|---|
| Evaluator (`vd_eval.c`: IoU, NMS, AP50, PR, proposal recall) | **ASan+UBSan** | `make vision_detection_eval_asan`, **17** analytic fixtures |
| Pack parser, manifest schema, artifact root, component-wise traversal, directory-atomic publication, SHA-256 (`vd_pack.c`, `vd_protocol.c`, `vd_io.c`, `vd_sha256.c`) | **ASan+UBSan** | `make vision_detection_integrity_asan`, **146** hostile-input fixtures |
| Allocation-failure paths in the evaluator, pack loader and manifest parser | **UBSan** (`make vision_detection_allocfail_ubsan`, **13** checks) — **not ASan** | ASan replaces `malloc`/`calloc`/`realloc`, so the linker's `--wrap` cannot intercept them and the injection would silently never fire. The same functions are covered under ASan by the integrity lane above; the injection lane runs under UBSan so the two together cover both concerns. |
| Evidence gate end to end (`vd_bench` protocol / artifact root / snapshot / prev-holdout / worker / publish paths) | **no sanitizer** — functional negative controls only | `make vision_detection_evidence_test`, **62** fail-closed controls |
| The scored training run itself (~44–50 min) | **no sanitizer** | run optimised and unsanitized; not attempted |
| Extraction (`vd_prep.cpp`, OpenCV) | **no sanitizer** | not sanitized; it produces the cache, and the cache is bound by the artifact root that the scorer verifies |

All focused lanes, and the real `vd_bench` and `vd_prep` binaries, compile with `-Werror`.

So: the metric path and the file/parse paths that turn bytes into evidence are sanitized
against hostile input. The long training run is not, and no claim is made that it is.

## 11. Where it still stands, honestly

AP50 0.115 is a **real but weak** detector. A modern VOC car detector scores several times
higher. What is now established is the mechanism claim — a CNET-native BTN, trained on
class-agnostic proposals with no pretrained weights anywhere, learns real object detection
on real images — not a competitiveness claim, which stays withheld by design.

Remaining ceilings, in order:

1. **Proposal recall 0.828** — 17% of cars remain unreachable before scoring.
2. **Precision.** 75 989 surviving detections against 262 GT. A pre-NMS score threshold
   and a per-image top-k cap would cut this sharply. Deliberately **not** applied here:
   the protocol forbids rescuing or inflating a result with test-side thresholding.
3. **Under-fit linear control** (§10, caveat 2) — must be repaired before any future claim
   compares CNET to "a simple learned baseline".
3b. **No same-holdout/same-proposal descriptor ablation**, so the 2.47× stays attributed to
   the V2 frontend as a whole rather than to the descriptor (§10).
4. **Level C** still needs a pretrained reference detector on the identical holdout.
5. **Level B** still needs an honest continuous-domain coverage gate for `PORT_RAW`;
   `coverage_family_gated()` is unchanged.

Unspent for V3: **2952 official test images** (slice 2000–4951).

## 10. Honest failure modes stated in advance

If V2's AP50 lands below the 0.10 floor again, the verdict is
`VISION_DETECTION_MECHANISM_WITHHELD` a second time and the feature-ceiling hypothesis is
recorded as **not sufficient on its own** — that is a real result about where the ceiling
is, not a reason to relax the protocol. Explicitly forbidden as a response: lowering any
bar, re-scoring V1's spent images, sweeping the threshold or a top-k cap on holdout, or
switching the target class.
