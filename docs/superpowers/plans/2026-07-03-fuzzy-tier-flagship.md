# Fuzzy-Tier Flagship Implementation Plan

> Executed inline by the spec's author immediately. Spec:
> docs/superpowers/specs/2026-07-03-fuzzy-tier-flagship-design.md.
> NO GIT PUSH (local commits fine). TDD; `make flagship` stays the gate
> (sections extend tests/test_flagship.c); real-run CLI gains task modes.

**Goal:** measure extraction across certification tiers: PAIR task (sampled +
Wilson + conformal reject, report-only) and TOPK ranked-preference task
(exact, thin margins) through the existing drain/base/governor stack.

**Tag families (Damerau-safe vs existing `wa<t>q<t>`):** Track A `pr<t>q<t>`
(input tag `w_pair`), Track B `tk<t>q<t>` (input `w_cur`, shared with
campaign 1 — exact re-mint is idempotent).

- [ ] Task 1: acquire.c report enrichment — `AcquireReport.last_bound`
      (Wilson when SAMPLED, 1.0 when PROVEN) + `last_min_margin` (from
      ExhaustiveReport); set in both attempt paths; existing gates stay green.
- [ ] Task 2: flagship task shapes — `FlagshipTask` enum (ARGMAX/PAIR/TOPK) +
      `topk` in config; sweep builds ports/names/tags per shape; report gains
      proof/sampled tallies + bounds[] and margins[] arrays with
      min/median/max printing; gate sections [6] (PAIR sampled, small
      mine_budget, sample_count 128) and [8] (TOPK proof) + [9] (TOPK
      capacity-starved → certify_failed, DEFER-total).
- [ ] Task 3: conformal probe (report-only) — flagship-side: for PAIR units
      that close SAMPLED, re-mine disjoint calib(256)/test(256) strides from
      the oracle, `conformal_calibrate_btn` on a fresh `cnb_get_unit` copy,
      measure answered/abstained/empirical-risk; aggregate in report; gate
      section [7]; link $(CONFORMAL) into flagship targets.
- [ ] Task 4: CLI task modes — flagship_run arg 8 = argmax|pair|topk; PAIR
      oracle = 3-token context argmax; TOPK oracle = ordered top-3 one-hot
      fields; determinism checks per mode; real smoke (few units each mode)
      against the gemma model; armed full-run commands documented.
- [ ] Task 5: full `make test` regression + warning sweep + local commit +
      memory update; spec status flip.
