# Fuzzy-Tier Flagship: sampled extraction + conformal abstention + ranked-preference "soul" — Design

**Date:** 2026-07-03
**Status:** Draft (pending user review)
**Topic:** The first extraction campaign on task classes that do NOT surrender
to proofs. Measures the number that decides the thesis's reach: how much of a
model's behavior lands in each certification tier — PROOF / SAMPLED-with-
guarantees / REFUSED — and what calibrated abstention buys at the sampled
tier. Adds the "soul" track: extracting the model's *ranked preference*
(ordered top-k), not just its argmax.

**Design decisions taken (delegated):**
- **Fuzzy logic (Zadeh) evaluated and rejected.** Graded-membership algebra
  offers no calibration guarantee and would weaken the project's claims. The
  fuzzy tier is *calibrated abstention*: Wilson lower bound globally
  (`coverage_accuracy_lower_bound`) + split-conformal reject-option per query
  (`contract/conformal.h`, distribution-free, finite-sample — already built
  and validated). Nothing stronger is on offer in the literature for this
  problem shape.
- **"Soul" = ranked preference, canonically encoded.** PORT_EVIDENCE carries
  continuous weights that CNU1 cannot seal (bit-packed 0/1 exemplars only)
  and exact replay cannot certify. Instead: output port `ONEHOT` with
  `field_count = k` (k=3) — the model's 1st/2nd/3rd choices in order. Exactly
  canonical, bit-packable, certifiable, composable. The preference *order* is
  the distilled disposition; continuous weights are deliberately left behind
  (quantized-confidence fields are a v2 extension if wanted).

---

## 1. Goal & acceptance criteria

1. **Two new task shapes** through the SAME harness/drain/base machinery:
   - **Track A — pair-conditioned argmax (the sampled tier's fight):**
     per conditioning token t: `ONEHOT|V| ×2 fields ("w_prev","w_cur" as one
     2-field input port, tag "w_pair") → ONEHOT|V| ("pr<t>q<t>")`, computing
     `argmax_{v∈V} P(v | [t, w_prev, w_cur])`. Domain = V² (65,536 at V=256)
     ≫ mine budget → the drain's SAMPLED path runs for real: deterministic
     stride sample + captured pairs, holdout split, `CERT_SAMPLED` gated by
     the Wilson floor.
   - **Track B — ranked preference, "the soul":** per conditioning token t:
     `ONEHOT|V| ("w_cur") → ONEHOT|V| ×3 fields ("tk<t>q<t>")` = the model's
     ordered top-3 next tokens. Domain = V (enumerable) → PROOF-eligible, but
     ranking margins are thin (top2/top3 gaps), so this track produces the
     campaign's first honest `certify_failed` DEFERs — that distribution is a
     RESULT, not a failure.
2. **Conformal reject-option wired as the sampled tier's per-query guarantee**
   (report-only in v1): after a Track-A unit certifies SAMPLED, calibrate
   `ConformalCalibrator` on a calibration split and measure on a test split:
   singleton (answer) rate, abstention rate, empirical risk on answered
   queries vs the certified `1 - alpha`. v1 persists NOTHING conformal (the
   calibrator is runtime state; persistence is follow-up) — the guarantee
   numbers go in the report.
3. **The tier table is the deliverable.** Per track: attempted / PROOF /
   SAMPLED (with min-max Wilson floors) / DEFERRED-by-reason. Plus Track A's
   risk-coverage numbers. Printed by the run; summarized by `cnb_audit`
   (verdict tallies extend it).
4. **Everything already proven stays true:** duty-cycled governor, stop file,
   base-as-checkpoint resume, tag governance (families spaced: `pr<t>q<t>` /
   `tk<t>q<t>` doubled ids), DEFER totality, byte-identical saves.
5. **Gated in verify** (`make fuzzy` or extend `make flagship`): synthetic
   oracle with a controllable noise/instability profile forcing all three
   outcomes (PROOF on a small window, SAMPLED via a tiny `mine_budget`,
   DEFER via thin-margin rankings), plus conformal-wrapper counts asserted.
   No CCE/GPU/model in the gate.

## 2. Non-goals (v1)

- No PORT_EVIDENCE units, no continuous weights in contracts (see decision).
- No persisted conformal calibrators (report-only; sidecar design is v2).
- No planner integration of abstention (strict execution already refuses
  out-of-domain; conformal gating of *routing* is its own milestone).
- No second model / merge; no GPU forward.

## 3. Mechanics (deltas only — everything else is the proven stack)

- **`FlagshipConfig` gains a task shape**: `FLAGSHIP_TASK_ARGMAX` (existing),
  `FLAGSHIP_TASK_PAIR` (Track A), `FLAGSHIP_TASK_TOPK` (Track B, `k` field).
  The sweep/name/tag/oracle-registration blocks dispatch on it; the drain is
  untouched (it already handles multi-field ports and the sampled path).
- **Oracle adapters** (flagship_run CLI): PAIR = 3-token context
  `[t, w_prev, w_cur]`, argmax over V. TOPK = 2-token context, top-3 indices
  over V by logit order, emitted as 3 one-hot fields.
- **Sampled-mode knobs for Track A:** `sample_count` default 1024 (mining
  cost ~1 min/unit at measured forward speed), `holdout_fraction` 0.25.
  The holdout doubles as the conformal calibration split; the test split is
  a second stride (disjoint offsets) mined after certification, size 256.
- **Class-balance heuristic** stays as built (≥2 distinct targets in sampled
  mode); expected to fire on degenerate conditioning tokens — counted, fine.
- **Run sizing (the armed command, user fires):** V=256; Track A
  `max_units=64` (~4–6h), Track B `max_units=256` (~2–3h; enumerable domains
  of size 256 with 3-field outputs). Both resumable, both governed.

## 4. Report (what the run prints)

```
tier table (per track):
  attempted N | PROOF n0 | SAMPLED n1 (wilson floor min/median/max)
  | DEFER n2 {certify_failed: a, class_imbalance: b, oracle_unfit: c, ...}
conformal (track A, alpha=0.05):
  answered %, abstained %, empirical risk on answered (target <= alpha),
  per-unit min/median/max singleton rate
soul margins (track B):
  certified min-margin distribution (port_margin over rank fields)
```

## 5. Tests (gated, synthetic — no model)

1. PAIR task on a tiny window with `mine_budget` forced below V² →
   SAMPLED verdict, Wilson floor computed, holdout untouched by training.
2. Conformal wrapper on a synthetic noisy-tail oracle: singleton + abstain
   counts match hand-computed expectations; empirical risk ≤ alpha on the
   deterministic fixture.
3. TOPK task exact path: stable synthetic ranking → PROOF; capacity-starved
   fixture (`max_hidden` clamped below what the mapping needs) →
   `certify_failed` DEFER (the honest-refusal fixture — a deterministic
   oracle always yields consistent exemplars, so the refusal must come from
   the student's side, not fake oracle noise).
4. Resume + tag governance for both new tag families.
5. DEFER totality unchanged (counters/base byte-identical on refusals).

## 6. Open risks, stated

- Track A trains 512-input BTNs on ~768 samples — training may become the
  bottleneck or underfit; `certify_failed` rates are data, but if they hit
  100% the recipe (hidden cap / epochs) gets ONE documented tuning pass
  before the run counts as measurement.
- Ranking stability (Track B) may be dominated by fp-level logit ties in the
  4-layer draft model; if top-3 order is not deterministic call-to-call, the
  determinism spot check catches it and Track B falls back to top-2/top-1+2
  (decided at smoke time, recorded in the run log).
- 65k-domain units certified SAMPLED are claims about the STRIDE+CAPTURED
  distribution, not the full domain — the report labels them as such
  (verdict strings carry SAMPLED explicitly; no PROOF language anywhere).
