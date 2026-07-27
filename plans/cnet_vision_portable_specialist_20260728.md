# CNET portable visual specialist — PRE-REGISTRATION (2026-07-28)

**Status: PRE-REGISTRATION — IMPLEMENTATION INCOMPLETE (WIP).**

Written before the fresh slices are extracted, before the coverage gate is fitted, and
before any transfer holdout image is scored. **None of §2's reserved slices have been
downloaded, extracted, calibrated or scored.** Work was deliberately stopped after the
capsule transport seam (§3) at operator request.

**What exists in this checkpoint:** only the schema-2 capsule transport — the
manifest-bound sidecar asset, its fail-closed import, and its fixtures. Nothing else.

**What does NOT exist yet:** fresh-slice extraction, the coverage gate implementation and
its calibration, the export/import/runner tooling, the fresh-runtime transfer proof,
coexistence/composition, and the results in §11.

**Therefore every claim level in §8 is WITHHELD**, including
`VISION_TRANSFER_ARTIFACT_WITHHELD`, `VISION_CONTINUOUS_COVERAGE_WITHHELD`,
`VISION_CAPSULE_PORTABILITY_WITHHELD`, `VISION_SPECIALIST_COMPOSITION_WITHHELD` and
`VISION_SPECIALIST_COMPETES_WITHHELD`. No completion sentinel is written: the PORT_RAW
assignment is not complete.

Branch `feature/vision-portable-specialist`, from accepted hardening SHA
`bd73047032c509346f8d7e191398756874d96558` (independent Codex
`PASS_NO_CRITICAL_HIGH`). Local commits only.

The target is **ASI = Artificial Specialized Intelligence**: one certified, isolated,
portable kernel that detects one class. Nothing here claims broad vision, arbitrary-task
learning, AGI, or superintelligence.

---

## 1. Carried-forward accepted evidence (not re-derived here)

The V2 detector is the thing being made portable. Its accepted numbers stand unchanged and
are **not** re-run by this pass: AP50 0.114518 on the spent V2 holdout, randomized-head
0.000988 (115.9×), label-shuffle 0.002435, determinism 0, artifact root
`8c70c9327daf39623e5b3cc1fa0c5cffe0389f4c497c8e8a9e66769c4ce4908b`.

## 2. Slice reservation — fixed before any experiment

The official VOC2007 test list is one seed-`20260727` permutation (§2 of the V2 protocol).
Spent: `[0,1000)` = V1, `[1000,2000)` = V2. Reserved **now**, before experimentation:

| slice | role | may be used for |
|---|---|---|
| `shuf[2000 … 2799]` (800) | **calibration** | fitting the coverage threshold only |
| `shuf[2800 … 3799]` (1000) | **transfer holdout (V3)** | ONE frozen scored evaluation |
| `shuf[3800 … 4951]` (1152) | unspent | nothing in this pass |

Calibration is disjoint from model fitting (the head and PCA were fitted on VOC *trainval*)
and disjoint from the transfer holdout. No V1 or V2 holdout image is read.

## 3. Capsule extension seam — the canonical one, not a second format

A capsule today is `dir/{unit.cnb, manifest.cknow}`: the manifest carries identity, typed
ports, `payload_fnv`/`payload_bytes` binding the payload, optional coverage rows, and a
trailing `manifest_fnv` covering **every preceding byte**. It cannot carry a PCA basis, a
HOG/SS configuration, or thresholds.

**Seam: one additional manifest-bound blob, using the identical binding discipline as the
payload.** The manifest gains

```
asset <asset_schema> <bytes> <fnv> <leafname>
```

and the capsule directory gains that one file. Import requires it to be present, of the
declared size, and of the declared FNV — exactly how `unit.cnb` is already bound — and the
line is inside the region the existing `manifest_fnv` already covers. No new checksum
scheme, no new container, no second manifest.

**Compatibility is fail-closed by schema.** Asset-bearing capsules declare
`CNET_CAPSULE 2`; capsules without an asset keep declaring `1`. An older runtime rejects
schema 2 outright rather than silently importing a head without its frontend. The new
runtime accepts 1 and 2. Existing capsule/accumulation/coverage/composition gates keep
producing and consuming schema 1 and must stay green.

The coverage reference set travels in the **existing** coverage section: it is
`HybridCoverage` rows, bounded by the existing `CAP_MAX_COV_ROWS` 4096 and
`CAP_MAX_COV_CELLS` 4 Mi. At 256-D that is 4096 × 256 = 1 048 576 cells, inside both
bounds. The gate therefore adds no new transport.

## 4. What the package must carry

1. BTN weights + exact topology — `unit.cnb`
2. typed contract `PORT_RAW`256 → `PORT_ONEHOT`2 — manifest `in`/`goal`
3. PCA mean + eigenbasis — asset blob
4. HOG configuration + implementation identity — asset blob
5. Selective Search configuration + implementation identity — asset blob
6. class map, confidence threshold, NMS threshold, proposal cap — asset blob
7. continuous coverage model + calibration evidence — coverage rows + asset blob (τ, k)
8. dataset/protocol provenance + hashes — asset blob (protocol name, artifact root)
9. runtime/ABI/schema identity — manifest `cnb_version` + `asset_schema`
10. behaviour digest + corruption protection — manifest `behavior_digest`, `payload_fnv`,
    `asset_fnv`, `manifest_fnv`

## 5. Coverage gate — fixed before fitting

**Family: conformal k-NN distance to the certified in-domain reference set.** Chosen over a
density sketch because the reference rows already have a transport (§3) and the score is
exactly reproducible on a fresh runtime with no fitted state beyond the rows and τ.

- **Reference set** `R`: a deterministic subsample of **train** proposal feature vectors
  (PCA256), seed `20260728`, `|R| = 4096`, drawn from the V2 cache the head was fitted on.
- **Score** `s(x)` = mean Euclidean distance from `x` to its `k = 8` nearest members of `R`.
- **Threshold rule** `τ` = the **95th percentile** of `s` over the **calibration** slice
  (§2), computed once. Admit iff `s(x) ≤ τ`.
- Non-finite or extreme (`|x|∞ > 1e6`) vectors are refused before scoring.

### Pre-registered floors (fixed now; not to be relaxed)

| bar | requirement |
|---|---|
| in-domain admission, V3 holdout proposals | **≥ 0.90** |
| gross synthetic OOD refusal (blank, constant, extreme-magnitude, shuffled-dimension) | **≥ 0.99** |
| external natural-image OOD refusal | **≥ 0.30** *and* **≥ 3×** the in-domain refusal rate |
| AP50 with gate enabled, V3 holdout | **≥ 0.90 ×** AP50 with gate disabled, same holdout |
| gate memory | **≤ 64 MiB** |
| gate latency | **≤ 5 ms** per proposal |

The natural-image floor is deliberately modest and is stated as a **weak-but-real**
discrimination claim, not a strong OOD claim: consumer photographs occupy much the same
photographic domain as VOC, so a large refusal rate there would be surprising rather than
reassuring. The `3×` relative term is what makes it non-vacuous — at a 95th-percentile
threshold the in-domain refusal rate is ≈0.05 by construction, so an indistinguishable
distribution would refuse ≈0.05, not ≈0.30.

### External natural OOD source

**Kodak True Color Image Suite**, 24 photographs, `https://r0k.us/graphics/kodak/`,
long-standing public research image set, fetched with per-file SHA-256 recorded. Verified
reachable by ranged GET (HTTP 206 with real bytes). Supplemented by the 12 system wallpaper
photographs under `/usr/share/backgrounds` as a second, independent non-VOC source. **36
natural scenes is a small scene count** and is reported as such; it yields several thousand
proposal vectors, but scene diversity — not vector count — is the honest limit.

Synthetic controls (blank, constant, extreme, dimension-shuffled) are **required negative
controls and are not sufficient alone**.

## 6. Compatibility refusals — all must fail closed

wrong extractor digest · wrong PCA digest · wrong feature dimension · wrong port family or
tag · wrong class map · wrong runtime/CNB version · wrong asset schema · corrupted or
truncated payload · corrupted or truncated asset · manifest checksum mismatch · missing
asset file · mixed package (payload from one export, asset from another) · non-finite or
extreme vectors · calibrated out-of-domain input.

A failed import must leave the destination base and coverage **unmutated**.

## 7. Fresh-runtime transfer proof

Train/export → record path, size, SHA-256, schema → copy to a fresh temporary directory →
make the vision cache, PCA files and training artifacts unavailable → fresh CNET base →
import → run full-image inference from JPEG only → reproduce reference boxes/scores and
held-out AP within tolerance → exercise the gate and its abstention signal → reject every
compatibility/corruption control without mutating the destination → import a second
unrelated discrete capsule alongside to prove coexistence.

**Replay tolerance:** box coordinates exact; scores and AP50 `≤ 1e-9` absolute. The runner
recomputes from JPEG through the same fixed frontend, so any deviation is a defect.

## 8. Claim levels

`VISION_TRANSFER_ARTIFACT_PASS` · `VISION_CONTINUOUS_COVERAGE_PASS` ·
`VISION_CAPSULE_PORTABILITY_PASS` (requires both) ·
`VISION_SPECIALIST_COMPOSITION_PASS` (only on genuine planner-discovered typed
composition) · `VISION_SPECIALIST_COMPETES_*` (WITHHELD — no fair reference detector is run
on the V3 holdout in this pass).

## 9. CPU policy

Profile before parallelising. Feature extraction, calibration and gate fitting may be
parallelised; **final held-out scoring is one frozen evaluation** and is never repeated for
tuning. A same-data 1-core baseline and the multicore result are both recorded, and the
optimisation is kept only if wall time improves beyond run variance while all bar-bearing
metrics and digests stay exact. Core count is chosen from live headroom, not by consuming
every core. `OMP_NUM_THREADS=1` and `cv::setNumThreads(1)` prevent nested oversubscription.

## 10. Stop condition

If the gate cannot meet these floors honestly, the artifact machinery is committed only if
independently sound, and the verdict is `VISION_CONTINUOUS_COVERAGE_WITHHELD` and therefore
`VISION_CAPSULE_PORTABILITY_WITHHELD`. **The floors are not weakened to obtain a pass.**

## 11. Results — continuous coverage gate: FLOORS NOT MET

Slices extracted against the frozen V2 basis, both asserted disjoint from all 2000 spent
V1+V2 holdout IDs: calibration 800 images / 216 469 proposals, transfer holdout 1000
images / 272 413 proposals.

Gate fitted exactly as pre-registered: 4096 reference rows (seed 20260728) drawn from the
certified training features, `k = 8`, `τ` = 95th percentile of the calibration score
distribution = **4.251561562**. Latency **33 µs/proposal** (cap 5 ms) and ~8 MiB of
reference rows (cap 64 MiB), so the cost bars pass.

| set | proposals | median score | refuse rate | pre-registered floor | result |
|---|---|---|---|---|---|
| in-domain (calibration) | 216 469 | 3.4065 | 0.0500 | — (by construction) | — |
| external natural OOD (Kodak 24 + wallpapers 12) | 8 731 | 3.3122 | **0.0906** | ≥ 0.30 **and** ≥ 3× in-domain | **FAIL** |
| gross synthetic OOD (blank/grey/black/hue/noise) | 163 | **1.8082** | **0.0245** | ≥ 0.99 | **FAIL** |

**`VISION_CONTINUOUS_COVERAGE_WITHHELD`.** The floors were not met and are not being
relaxed.

### Why it failed — the useful part

The synthetic row is the diagnosis. Degenerate images do not merely evade the gate, they
score **lower** than real in-domain proposals: median 1.81 against 3.41. A texture-free
crop has near-zero HOG response, its PCA projection therefore lands near the training
mean, and the training mean is precisely where the reference rows are densest. So
distance-to-reference is *anti-correlated* with out-of-domain-ness for the most important
OOD class — the degenerate inputs a coverage gate most needs to refuse.

Natural OOD is the second half of the same story: median 3.31 against in-domain 3.41, i.e.
no separation at all. Consumer photographs produce HOG-PCA statistics indistinguishable
from VOC proposals, which was anticipated in §5, but the measured refusal (0.091) is only
1.8× the in-domain rate, short of the 3× term that was meant to make the bar non-vacuous.

**A distance-to-reference gate is structurally the wrong instrument in this feature
space.** Nothing about `k`, `τ` or `|R|` fixes an anti-correlated score; a gate that would
work has to be sensitive to *degeneracy* (descriptor energy, gradient support) and to
class-conditional structure, not to proximity alone. That is a different family, and
choosing it now — after seeing these numbers — would be exactly the post-hoc selection the
pre-registration forbids. It is recorded here as the next honest experiment, to be
pre-registered on unspent slices before it is fitted.

The gate machinery itself is independently sound and stays: 22 analytic fixtures, clean
under ASan+UBSan+Leak, deterministic and fixed-partition parallel.

## 11b. Remaining results

**Not reached.** No slice in §2 has been touched, so there are no results to report and
every verdict in §8 stands WITHHELD. The only thing this checkpoint establishes is that
the capsule format can carry a frontend asset as one bound object (§3), verified by
`make vision_capsule_asset` (39 checks) and its ASan+UBSan+Leak lane.
