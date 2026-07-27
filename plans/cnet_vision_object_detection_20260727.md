# CNET vision object-detection benchmark — FROZEN PROTOCOL (2026-07-27)

**Status: protocol frozen before test; §10–§13 filled after the single scored run.**

**Original header — PRE-REGISTRATION. No test image, test annotation, or
test-derived number has been read, scored, or inspected.** Everything below is fixed
before the single scored test run. Numeric PASS floors are derived from *validation*
and frozen in §7 before test is touched.

Branch `feature/vision-object-detection-benchmark`, base `cbbfff6`. Local commits only.

---

## 0. Execution policy (recorded in every log)

- **CPU only.** Every experiment and model command runs with
  `ROCR_VISIBLE_DEVICES='' HIP_VISIBLE_DEVICES='' CUDA_VISIBLE_DEVICES=''`.
  Both AMD GPUs are reserved and must not be touched.
- Every bounded run is wrapped in `timeout`; no orphan compute.
- `cv::setNumThreads(1)` inside workers; parallelism is across images via a fixed
  worker count, so results do not depend on scheduling.
- Nothing is installed into `~/ai-env`. OpenCV 4.6 system C++ stack only
  (`opencv_ximgproc` present — verified).
- Datasets, images, features, model blobs and caches are **not committed**.

## 1. Dataset

| | |
|---|---|
| Dataset | **PASCAL VOC 2007** |
| Splits | official `ImageSets/Main/trainval.txt` (5011) and `test.txt` (4952) |
| Source | `http://host.robots.ox.ac.uk/pascal/VOC/voc2007/` (primary), `https://data.brainchip.com/dataset-mirror/voc/` (mirror) |
| Terms | VOC data is provided for research/benchmarking; images originate from flickr under their respective terms. Used here for non-commercial benchmarking only, not redistributed. |
| `VOCtrainval_06-Nov-2007.tar` MD5 | `c52e279531787c972589f7e41ab4ae64` (verified) |
| `VOCtest_06-Nov-2007.tar` MD5 | `b6e924de25625d8de591ea690078ad9f` (verified) |
| Images on disk | 9963 (5011 + 4952) — verified |

## 2. Target class and seed

- **Single target class: `car`.** Fixed *a priori* before any evaluation — a rigid,
  mid-scale, well-represented VOC class standard in single-class detection studies.
  It was **not** selected by comparing results across classes, and it will **not** be
  swapped after seeing test numbers. This document is committed before the test run.
- **Deterministic seed: `20260727`** for every sampling/shuffle/init decision.
- Fixed worker count for proposal generation: **8**.

## 3. Splits — separation by image ID and content hash

- **Train / Val**: the official *trainval* split is partitioned by a deterministic hash
  of the **image ID string** (FNV-1a 64): `hash % 5 == 0` → val, else train.
  No image contributes to both.
- **Test**: a fixed, **class-agnostic** random subset of the official *test* split —
  seed `20260727`, **1000 images** sampled from all 4952 regardless of whether they
  contain a car. Negatives are retained deliberately: evaluating only on positive
  images inflates AP by removing the false-positive opportunity.
- **Leakage assertions (run fails if violated):** zero image-ID intersection between
  train, val and test; and zero **SHA-256 content-hash** intersection, so a duplicated
  image under a different ID also fails the run.

## 4. Inference contract

At test time the detector receives **only the decoded full image**. It emits a list of
`(x, y, w, h, class, confidence)`. It never receives test ground-truth boxes, test
annotations, or the number of objects present. Ground-truth boxes are used **only**:
(a) to label *training/validation* proposals, and (b) inside the evaluator after
predictions are written. A "GT proposals at test" path does not exist in the code and
its absence is asserted.

## 5. Frontend identity — what is fixed, what is learned

| Component | Status |
|---|---|
| Region proposals | OpenCV **Selective Search Fast** (`ximgproc::segmentation`), **class-agnostic**, run on the full image resized to width 300. Fixed algorithm, nothing learned, no class information. |
| Local features | **HOG**, deterministic: proposal crop → 32×32 gray, 8×8 cell, 16×16 block, stride 8, 9 bins → **324-D**. Fixed, not learned. |
| Dimensionality reduction | **PCA to 64-D, fitted on TRAIN proposals only.** Declared as learned-on-train. Mean/eigenbasis frozen after fit and reused verbatim for val and test. |
| Detection head | **CNET-native BTN** (`btn_init`/`btn_train_dynamic`), input `PORT_RAW` 64-D, output `PORT_ONEHOT` 2 (background / car). **This is the CNET-learned component under test.** |
| Pretrained detector | **None used anywhere in the CNET path.** No pretrained class logits, class-conditioned boxes, or detector features feed the head. |

Nothing pretrained on VOC or on detection is used. The only learned parts are the
train-only PCA and the CNET BTN head.

## 6. Metrics and matching

- **AP50** (single class ⇒ AP50 = mAP50 here), computed by an evaluator kept separate
  from training code.
- Detections sorted by descending confidence; **greedy one-to-one IoU ≥ 0.5** matching
  against ground truth per image; each GT may be matched at most once; later duplicate
  matches are false positives.
- VOC `difficult` objects are **ignored**: a detection matching a difficult GT counts
  as neither TP nor FP, and difficult GTs are excluded from the recall denominator.
- **NMS**: greedy, IoU threshold **0.30**, applied per image before evaluation.
- AP integration: **VOC2010+ style** (all-points interpolation of the precision
  envelope), stated explicitly rather than left ambiguous.
- **Proposal recall** at IoU 0.5 is reported *separately* as the ceiling the head cannot
  exceed, so head quality is never confused with proposal quality.
- Precision and recall reported at the confidence threshold maximizing validation F1
  (threshold chosen on **validation only**).

## 7. PASS bars

Frozen **after validation, before test** (filled in §10 with the validation numbers that
set them). Bars are not lowered afterwards under any circumstance.

**Level A — `VISION_DETECTION_MECHANISM_PASS`** requires *all*:

1. `AP50_test ≥ max(0.10, 0.50 × AP50_val)` — a real, non-trivial detector.
2. `AP50_test(trained head) ≥ 3 × AP50_test(randomized head)` **and** absolute margin
   `≥ 0.05` — the CNET-learned component is causally responsible.
3. Proposal recall reported (diagnostic, no bar).
4. Deterministic rerun: `|AP50 − AP50_rerun| ≤ 1e-9`.
5. Zero ID and content-hash leakage.
6. Machine-readable JSON with honest counts.

**Level B — `VISION_CAPSULE_PORTABILITY_PASS`**: A, plus the head seals through the
existing CNET base/capsule path, imports into a fresh runtime, and reproduces held-out
predictions within tolerance `1e-9`; extractor identity / dims / class map bound in
provenance; incompatible extractor or feature dimension refused.

**Level C — `VISION_SPECIALIST_COMPETES_PASS`**: expected **WITHHELD** on this first
run. No pretrained reference detector is run in this pass, so the honest output is
`VISION_SPECIALIST_COMPETES_WITHHELD` with the reason recorded.

## 8. Known constraint discovered in preflight (affects Level B)

`coverage_family_gated()` (`src/hybrid_ai.c:140`) admits only `PORT_ONEHOT` and
`PORT_BINARY_*`. For `PORT_RAW` continuous features:

- `hybrid_coverage_admits_exact` → **refuses everything** (strict query),
- `hybrid_coverage_admits_unit` → **admits everything** (legacy query).

Neither is an honest continuous-domain coverage gate. Exact-row coverage would admit
only training rows, which is not held-out serving. Therefore, per the task's own
instruction, **capsule portability is reported as a transfer mechanism only, and
production serving of this detector stays `WITHHELD`.** No gate is bypassed to
manufacture a pass.

Preflight also established (log: `logs/vision/preflight_btn_raw.log`) that a BTN *can*
fit 64-D continuous RAW features (400/400) and that such a unit **is certifiable**, so
capsule export is not structurally blocked.

## 9. Controls that must run

- randomized/untrained CNET head (same features, same proposals);
- a simple learned linear/logistic head on **identical** features and proposals;
- proposal-only recall ceiling;
- label-shuffle control (train labels permuted with the fixed seed → AP50 must collapse);
- GT-proposals-at-test sentinel: absent, asserted;
- duplicate/leaked ID or content hash → fail the run;
- corrupt/missing image or malformed annotation → fail loud, not silently skip;
- evaluator unit fixtures with analytically known IoU/AP outcomes;
- no-detections, duplicate-detections and multi-GT matching cases covered.

## 10. Validation results and frozen bars

Two validation rounds. Round 1 was the protocol architecture without hard-negative
mining; round 2 added the mining the architecture named (§ "Preferred honest
architecture"). This is the **one bounded improvement** the stop conditions allow, and it
was decided on validation only.

| | round 1 | round 2 (final) |
|---|---|---|
| AP50 CNET head | 0.036876 | **0.047319** |
| AP50 randomized head | 0.000947 | 0.000947 |
| AP50 linear baseline (same features) | 0.022394 | 0.020122 |
| AP50 label-shuffle control | 0.001703 | 0.001388 |
| best F1 / threshold | 0.0301 @ 0.70 | **0.1064 @ 0.55** |
| proposal recall @IoU0.5 | 0.836910 (195/233) | 0.836910 |

Hard-negative mining converged: round 0 added 14 972 confident false positives, round 1
only 273 — the head had stopped making them.

**Bars frozen from these numbers, before test:**
`ap50_floor = max(0.10, 0.5 × 0.047319) = 0.10`, `ratio_floor = 3.0`,
`margin_floor = 0.05`, `determinism ≤ 1e-9`, threshold `0.55`.

## 11. Test results — single scored run

```
TEST proposal_recall=0.772152 (183/237)
TEST ap50_cnet=0.046434 ap50_random=0.000733 ap50_linear=0.017864
TEST precision@0.55=0.142857 recall@0.55=0.067511 detections=86476 gt=237
TEST determinism |ap-ap_rerun|=0
BARS floor=0.100 -> FAIL | ratio>=3.0 -> PASS (63.3x) | margin>=0.050 -> FAIL (0.0457) | det -> PASS
```

| Bar | Required | Measured | Result |
|---|---|---|---|
| Absolute AP50 | ≥ 0.100 | **0.046434** | **FAIL** |
| Ratio vs randomized head | ≥ 3.0× | **63.3×** | PASS |
| Absolute margin vs randomized | ≥ 0.050 | **0.045701** | **FAIL** (narrowly) |
| Determinism | ≤ 1e-9 | **0** (exact) | PASS |
| Leakage | zero | zero ID, zero content-hash | PASS |

### Verdict

```
VISION_DETECTION_MECHANISM_WITHHELD
VISION_CAPSULE_PORTABILITY_WITHHELD   (Level B requires Level A)
VISION_SPECIALIST_COMPETES_WITHHELD   (no pretrained reference run this pass)
```

**The bars were not lowered.** The margin bar missed by 0.0043. Moving it would have
turned a measured shortfall into a manufactured pass, so it stands.

## 12. What the numbers actually say

**The CNET-learned component is causally responsible for essentially all detection
signal.** The randomized head of identical shape on identical proposals and features
scores 0.000733; the trained head scores 0.046434 — **63×**. The label-shuffle control
collapses to 0.001388, so the head is learning the labels and not an artifact of the
proposal or feature pipeline. It also beats the linear logistic baseline on the same
64-D features (0.046434 vs 0.017864, **2.6×**), so the gain is not merely "any learned
scorer on HOG-PCA".

**Validation → test generalisation is clean**: 0.047319 → 0.046434, a 1.9% relative drop.
Nothing was tuned on test, and the near-identical numbers are consistent with that.

**The absolute detector is weak**, and the honest attribution is a stack of ceilings:

1. **Proposal ceiling.** Test proposal recall is 0.772 — 23% of cars are unreachable by
   any head, before scoring begins.
2. **Feature ceiling.** 32×32 grayscale HOG → 64-D PCA is a very small description of a
   car. This was chosen for CPU cost, and it is almost certainly the dominant limit.
3. **Precision.** 86 476 surviving detections against 237 ground-truth objects: NMS at
   0.30 over ~260 proposals/image leaves far too many low-confidence boxes, which is what
   drags AP down even where recall exists.

None of these are CNET-substrate limits, and none are evidence that the substrate cannot
learn — the 63× ablation gap says the opposite. They are limits of the deliberately cheap
frontend this pass could afford on CPU.

## 13. Findings worth recording

**VOC2007 trainval contains 3 exact-duplicate image pairs under different IDs** —
`008037↔009623`, `000338↔007284`, `000949↔005042`. The content-hash assertion caught
them: an image-ID-based train/val split leaks identical pixels across the boundary. The
split was changed to key on the **SHA-256 of image content**, so every copy lands in the
same split by construction. Separately measured and clean: **0 duplicates inside test and
0 trainval↔test duplicates**, so the official VOC train/test separation is sound at
content level.

**`PORT_RAW` has no honest coverage gate** (§8), confirmed in code, so continuous-feature
capsule serving stays withheld regardless of Level A. Preflight did establish that a BTN
on 64-D continuous RAW features fits (400/400) and **certifies**, so capsule export is not
structurally blocked — only its abstention semantics are.

## 14. Blockers and the smallest next step

1. **Feature ceiling is the top blocker.** 64-D PCA-of-HOG cannot describe a car well.
   Cheapest credible step: colour + larger HOG window (e.g. 64×64, 3 channels) at
   128–256-D, still CPU-only, still no pretrained weights.
2. **Proposal ceiling (0.772).** More SS proposals or a second class-agnostic proposer
   would raise the reachable maximum.
3. **Precision.** Score-threshold before NMS, and a per-image top-k cap, would cut the
   86 k detections dramatically without touching the head.
4. Level C needs a pretrained reference detector run on the identical test subset; not
   attempted here, and correctly withheld.


## 12. Claims and withheld claims

**Claimed only if measured:** a CNET-native learned head performs real object detection
on unseen full VOC2007 images, above a randomized-head ablation, with proposal recall
reported separately.

**Withheld regardless of outcome:** any claim of competing with modern detectors;
general vision capability; multi-class detection; production serving of this detector
(§8); reasoning; AGI or superintelligence. ASI here means **Artificial Specialized
Intelligence** and nothing more.
