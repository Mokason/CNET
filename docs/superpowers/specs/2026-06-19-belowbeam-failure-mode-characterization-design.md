# Below-Beam Failure-Mode Characterization — Design (the data that names v5.2)

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged; recon done — see Recon outcome)
**Role:** the measurement gate between v5.1 (detection built) and v5.2 (recovery, not yet named).
It produces data, not a feature. v5.2's name is read off this study's output.

## Context

v5.1's probe detects *any* valid plan whose correct producer sits below the beam — **its detection
is cause-agnostic** (it does not know or care *why* the producer is down there). So "does the probe
detect each cause?" is near-trivially yes and names nothing. The data that names v5.2 is the
*shape*: where the correct producer crosses below the beam, and how large the recovered-producer
**rank-gap** is when it does.

## Goal

A budgeted study that constructs below-beam blind spots, sweeps the two levers that cause them,
reuses the v5.1 probe verbatim, and reports the **reliability-margin boundary** (deployment-
independent) plus the **rank-gap distribution** (the act-gate confidence signal). Output: a printed
table + a CSV artifact, like `residue_study` / `planner_scale_study`. Plus a thin hermetic anchor in
`make test`.

## Non-goals

No recovery action, no act-gate, no α/β/γ/δ implementation — this study *informs* that choice, it
does not make it. No new probe semantics (reuse `proposal_sidecar_below_beam_probe` verbatim). No
real-deployment frequency claim (see Frequency). No drift-vs-attacker attribution (a separate
monitoring concern, out of scope).

## The two causes = the two levers that push a correct producer below the beam

The blind spot is governed by the correct producer's rank = (# producers with higher reliability).
There are exactly two levers, and they are the two causes:

1. **Cold-start (under-ranked-correct):** the *correct* producer's own reliability is low — new, few
   samples, so its score sits near the 0.5 Laplace prior while established (wrong-for-this-task)
   competitors rank above. **Knob:** the correct producer's evidence count, swept `n ∈ {0,1,2,4,8,
   16,32,…}` (successes), watching its reliability climb until it crosses back above the cutoff.
2. **Rank-poisoning (over-ranked-wrong):** a wrong-for-this-task producer's reliability is high
   enough to consume the beam. **Knob:** the competitor reliability (via injected counters) and the
   competitor count vs the beam.

**Why two, not three (decided 2026-06-19):** "stale-evidence" and "adversarial" are merged into
rank-poisoning. (a) *Mechanically identical:* reliability is pure Laplace `(s+1)/(s+f+2)` over
**lifetime** counters with **no decay term** (recon-confirmed), so "stale" cannot be modeled as real
decay — only as set-high counters on a now-wrong producer, which is exactly the adversarial fixture;
they would yield the same rank-gap under the same knob. (b) *Identical for this study's purpose:*
both are "the ranking trusts a producer execution reveals is wrong," with the same recovery
(re-validate, distrust the rank). Their only difference — earned-drift vs malicious-injection — is
*attribution*, a monitoring/security concern, out of scope. Cold-start is the genuinely distinct
case (its signal is the opposite: low claimed reliability, execution-correct).

## What is measured — primary output is a deployment-independent boundary

Each swept point builds the fixture, calls the probe, and records `found_valid_below_beam`,
`recovered_producer_rank`, `official_beam_limit`, `rank_gap = rank − beam`, and the **reliability
margin** = (correct producer's reliability) − (the reliability of the producer ranked at the cutoff,
i.e. the k-th). Report:

- **PRIMARY — the reliability-margin boundary:** the correct producer falls below the beam when its
  reliability deficit vs the k-th-ranked competitor exceeds a threshold, as a function of beam size
  `k`. This is a property of the ranking math, not of any workload — fully transferable.
- **Rank-gap distribution** (min / median / max + raw per-point to CSV) when below beam — the
  act-gate confidence signal: tight near-misses (small, stable gap) → a future gate could trust them;
  scattered (large/variable) → it would need a confidence model.
- **Beam size as a second axis** (`k ∈ {2, 4, 8}`) so boundary/gap are seen against the cutoff.

## Frequency is the deployer's convolution (not ours to assert)

We supply the intrinsic margin-boundary curve; a deployer who knows their own reliability-margin
distribution reads their blind-spot rate off it. We do **not** assert "the blind spot is rare/common"
— there is no real deployment evidence stream (that is what activating v5.1 in production would later
produce). The transferable findings are the **boundary** and the **rank-gap shape**, both
mechanism-intrinsic; the real-world *rate* is the deployer's own convolution against them.

## Reuse / new code

- **Reuse:** `proposal_sidecar_below_beam_probe` (verbatim), the residue δ as the verifiable correct
  producer, the synthetic-decoy pattern + `r_state→r_next` tag device from
  `test_below_beam_recovery.c`, the CSV/print idioms from `residue_study`. Margin is reconstructed by
  sorting `btn_reliability` over the entries (the probe's `psc_rank_of_root` already does the rank).
- **New:** two fixture-cause generators (cold-start, rank-poisoning), the sweep harness, the
  margin-boundary + rank-gap aggregation, and the table/CSV reporting. No core changes.

## No-authority / Δ-4

The study calls the probe many times; the probe already snapshots+restores BTN counters per call
(Δ-4), so the sweep leaves no reliability footprint between points. The study never mutates the
registry's authority-bearing state, and keeps `power_mode == DEFAULT` (recon: a `CNET_POWER_LOW`
cost tiebreak is the only thing that would perturb pure-reliability ranking). The thin anchor asserts
the no-authority invariants once.

## Thin hermetic anchor (the (b) part, in `make test`)

One deterministic fixture **per cause** (cold-start + rank-poisoning → two fixtures), each at a
representative knob value: assert the probe detects the blind spot (`found_valid_below_beam == 1`)
and the no-authority + Δ-4 invariants hold. A regression guard that the two generators build valid
blind spots — not a measurement. Cheap; in `make test`.

## Recon outcome (resolved 2026-06-19)

- **Ranking is deterministic and pure-reliability at defaults:** `rank_by_reliability`
  (`src/router.c:302`) sorts by `btn_reliability` descending; **ties keep registry order** (stable
  insertion sort, comment at `router.c:300`). The only secondary factor is a cost tiebreak gated on
  `power_mode == CNET_POWER_LOW` (`router.c:305,321`); at the default `power_mode` (set by
  `registry_init`) ranking is pure reliability. → keep `power_mode` default; use distinct counter
  values so no ties arise.
- **No decay term** in `btn_reliability` (lifetime Laplace counters) — validates merging "stale" into
  rank-poisoning.
- **Margin/rank reconstructable** by sorting `btn_reliability` over entries; the probe reports
  `recovered_producer_rank` + `official_beam_limit` for the rank-gap.
- **Both fixtures constructible** by reusing the v5.1 device (δ correct producer + injected-counter
  competitors + `r_state→r_next`); cold-start = δ low count, rank-poisoning = δ normal + competitors
  injected high.

## Implementation plan (subagents; TDD)

1. Test first: the thin per-cause hermetic anchor (two fixtures, in `make test`).
2. Two fixture generators (cold-start, rank-poisoning) + the sweep harness + reporting
   (margin-boundary + rank-gap; printed table + CSV under `artifacts/belowbeam_chars/`).
3. Makefile: a budgeted `make belowbeam_chars` target (build+run, **not** in `make test`); wire the
   thin anchor source into `test_all` + its `run_test_*` dispatch.
4. Verify: anchor green in `make test`; the study runs and emits the table + CSV; no new warnings.

## Out of scope

α / β / γ / δ themselves (this study chooses among them). The act-gate. Drift-vs-attacker
attribution (a separate monitoring milestone). Any production deployment. v5.3 diffusion. The symbol
proposal family.
