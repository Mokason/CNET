# CNET-D v5.1 — Below-Beam Recovery (SHADOW_ONLY, detect + report) — Design

**Date:** 2026-06-18
**Branch:** `chunk-capacity`
**Status:** Approved; **recon complete (2026-06-18) — fixture confirmed constructible, see §Recon outcome**
**Milestone:** v5.1 of the CNET-D arc (follows v5.0 Proposal Sidecar; precedes a possible v5.2 act gate)

## Context

CNET-D is the advisory imagination lane ("diffusion proposes; CNET disposes"). v5.0 proved the
no-authority sidecar *contract*; v5.1 is its first real use. It stays **SHADOW_ONLY**: it
**detects and reports**, it does **not act**. Acting on a recovery (substituting a sidecar plan for
the official one) is a separate policy decision (v5.2+) that *requires* the detection stream this
milestone produces — you cannot design the act gate without first knowing how often the beam misses
and what a recovery looks like.

## The hypothesis (falsifiable)

> The beam-limited DAG planner silently misses valid plans whose correct producer is ranked below
> the beam cutoff because its reliability evidence is stale or adversarial.

This is the one named-but-unmeasured residual risk from the planner-scale study. v5.1 is the
measurement that can confirm or **falsify** it.

## Goal

A shadow probe that, for a task, compares the **official** (beam-limited) planner against a
**beam-lifted** re-plan, strict-validates any below-beam candidate through the existing executor,
and reports — via a typed struct — whether a valid, ground-truth-matching plan existed below the
beam. It changes nothing: official plan untouched, registry untouched, Δ-4 counters preserved.

## Non-goals

No act gate. No plan substitution / fallback. No registry or stats mutation. No planner-ranking
influence. No symbol proposals. No stochastic/learned proposer (the v0 proposer is deterministic —
see §Proposer). No deployment-cost model (that is the act gate's concern, v5.2+).

## The measurement — surfacing channel (typed struct, Δ-1 style)

Per the v5.0 precedent (no opaque payloads; a concrete typed report). New focused struct in
`include/proposal_sidecar.h` — **not** a generalization of `CnetPlanProposalReport` (Δ-2: don't
generalize before a second use forces it):

```c
typedef struct {
    /* no-authority invariants (constants; asserted in the gate) */
    int advisory_only;              /* 1 */
    int authority;                  /* 0 */
    int planner_influence;          /* 0 */
    int registry_mutation_allowed;  /* 0 */

    /* the measurement (Δ-1: "valid" is split, never one bit) */
    int official_strict_ok;         /* official beam-limited plan strict-executed clean */
    int official_matched_expected;  /* ...and matched ground truth */
    int sidecar_strict_ok;          /* beam-lifted candidate strict-executed clean */
    int sidecar_matched_expected;   /* ...and matched ground truth */
    int found_valid_below_beam;     /* sidecar_matched_expected && recovered producer ranks >= beam cutoff */
    int recovered_producer_rank;    /* 0-based reliability rank of the recovered root producer; -1 if none */
    size_t official_beam_limit;     /* the cutoff in force for the official run */

    const char *proposer_name;
    const char *task_key;
} CnetBelowBeamReport;

int proposal_sidecar_below_beam_probe(const PrimitiveRegistry *reg,
                                      const DagSource *sources, size_t n_sources,
                                      Port goal, int expected_argmax,
                                      CnetBelowBeamReport *report);
```

`found_valid_below_beam = sidecar_matched_expected && (recovered_producer_rank >= official_beam_limit)`
— exactly the definition fixed in v5.0 (`matched_expected && ranked-below-beam`), not merely "ran clean."

## The proposer — deterministic beam-lifted re-plan (NOT stochastic)

For the measurement, the proposer is the **existing planner with the beam lifted** (`dag_beam_limit`
raised/disabled, optionally `disable_plan_memo`), compared against the official beam-limited plan.
Rationale (mirrors v5.0 Δ-3): a deterministic, byte-identical-reproducible proposer is the right v0;
it *definitively* answers "is there a valid plan the beam excluded?" with no entropy. A
stochastic/diffusion proposer is a later backend (v5.3), not needed to take the measurement.

## Δ-4 carries forward — stated explicitly so it is not optimized away

Detect-and-report **must** still snapshot+restore every borrowed BTN's reliability counters around
its validation pass. The sidecar runs `dag_execute` (strict) to validate candidates; that records
evidence into the borrowed BTNs, and that evidence would feed the *next* official `dag_plan`'s
ranking/beam. The discipline is "the sidecar is invisible to the planner," full stop — **not**
"invisible only when it acts." The gate asserts BTN counters byte-identical before/after. (The v5.0
`proposal_sidecar.c` already carries the gate-enforced INVARIANT comment for this; v5.1's new
strict-executing path must repeat the snapshot/restore.)

## The fixture — INTENT fixed, MECHANISM recon-dependent

Intent: construct a registry + task where the **correct** producer is ranked **below** the beam
cutoff, so the official planner misses it and the beam-lifted sidecar finds it.

Shape (subject to recon): set `dag_beam_limit` small (e.g. 2). Register ≥3 interchangeable
same-contract producers: ≥2 "decoys" with high reliability score but wrong/invalid behavior (so the
official plan fails strict or mismatches), and 1 correct producer with a lower score so it ranks
below the cutoff. The careful part (your point): under Laplace smoothing `(s+1)/(s+f+2)`, a fresh
correct producer scores 0.5 while a decoy at `s=100,f=0` scores ≈0.990 — so "below beam" is a
*score* construction, not a *count* one. The exact stat values are chosen during implementation
once recon confirms how the beam reads stats.

**This fixture's priming mechanism is explicitly deferred to the recon pass (§Recon). The spec
fixes the intent and the measurement; it does NOT assert the priming method works until recon
confirms it.**

## No-authority proof + pass criteria

Carry the v5.0 gate shape: snapshot (registry entry state/certified/queue/shadow_of + every BTN's
`output_successes`/`output_failures`) before the probe, assert byte-identical after.

- `authority == 0`, `planner_influence == 0`, `registry_mutation_allowed == 0`
- official plan + registry + **BTN counters byte-identical** after the probe (Δ-4)
- on the constructed blind-spot fixture: `official_matched_expected == 0` (beam missed it) **and**
  `found_valid_below_beam == 1` with `recovered_producer_rank >= official_beam_limit`
- Δ-1 fields tracked separately and consistent (`matched_expected` ⇒ `strict_ok`)

## Probe overhead (measured 2026-06-19, `make probe_overhead`)

Deployment-shaped denominator — one route+execute cycle (`dag_plan` + `dag_execute`), the layer the
probe would sit beside in production — on a normal no-blind-spot fixture: the below-beam probe costs
**1.90× a bare route+execute** (2815 vs 1485 ns/call over 200k iters; `tests/probe_overhead_bench.c`).
~2× is the expected shape — the probe runs two plans + two strict executes + cheap rank/Δ-4
bookkeeping vs the baseline's one of each. This is the load-bearing number for a *future activation*
decision: the probe is SHADOW-ONLY and opt-in, so the cost is paid only when something calls it
(nothing does in v5.1). At ~2× a route+execute it is a viable recovery primitive to activate
selectively, not a prohibitive one.

**Caveat for any future citation: 1.90× is a baseline, not an upper bound.** It was measured on a
*no-blind-spot* fixture; heavier fixtures (longer plans, deeper re-rank, more strict executes for
validation) could grow the ratio. Future probe-overhead measurements must report the fixture shape
alongside the ratio rather than treating 1.90× as a constant.

## Falsification honesty (the project's standing rule)

If recon shows the blind spot **cannot** be constructed in the current planner (e.g. the beam is not
honored as believed, or stats cannot be made to force a correct producer below the cutoff), that is a
**finding, not a failure** — v5.1 reports "beam blind spot not exhibitable under construction X" and
the claim-history is updated honestly. No synthetic dashboard, no hand-tuned win. The experiment, as
in the residue test, cannot come back empty: it either exhibits the blind spot or documents why it
can't.

## Recon (MANDATORY before any implementer — this is the v5.0 lesson)

The API mapper must answer, against `src/router.c` / `include/router.h`, before code:

1. **Is `dag_beam_limit` honored as "consider only the top-N reliability-ranked producers per slot"?**
   Exact code path. Can it be set to 2 deterministically for a test? (v5.0 recon saw the field;
   confirm the *semantics*.)
2. **What exactly does the beam rank by?** Confirm the `(s+1)/(s+f+2)` Laplace formula and that it
   reads `btn->output_successes/output_failures` (runtime counters) — or a cached/persisted value.
   This decides whether the fixture can prime stats by writing counters directly.
3. **Can synthetic stats be primed by writing the BTN counters directly**, or must evidence be
   accrued through real `dag_execute` runs? (Direct = fast/deterministic fixture; real = slower but
   closer to "observed staleness." Either is acceptable; recon decides which the code allows.)
4. **How is "rank below the beam cutoff" observable** so the gate can assert `recovered_producer_rank
   >= official_beam_limit`? Is there a ranking the probe can read, or must it be reconstructed?
5. **Does beam-lifting reliably surface the below-beam producer** — i.e. `dag_beam_limit = 0`
   (default 8) or a large value, plus `disable_plan_memo`, yields the full candidate set including
   the correct one? (Memo pruning is sound, so lifting the beam should expose it — confirm.)

If (1) or (2) contradicts the assumed model, the fixture shape flexes; the measurement and the
no-authority discipline do not.

## Recon outcome (resolved 2026-06-18)

All five questions answered against source; the assumed model holds:
- **Beam:** `dag_beam_limit` is honored as "top-N reliability-ranked candidates" — the cutoff loop is
  `src/router.c:2164-2167` (`if (beam_limit != 0 && beam_count >= beam_limit) break;`), set via
  `registry_set_dag_beam_limit`, read at `router.c:2353`. `0` = no limit (default 8 only when unset).
- **Ranking:** `btn_reliability` = `(s+1)/(s+f+2)` over the runtime counters (`src/nn.c:1741`);
  `rank_by_reliability` (`router.c:302`) sorts all entries by it. Confirmed.
- **Priming:** writing `btn->output_successes/failures` directly is honored immediately (read
  on-demand at ranking; `registry_add` takes no snapshot). Direct counter write = the fixture method.
- **Rank observability:** reconstruct via `btn_reliability` (count entries with strictly higher
  reliability = 0-based rank); `rank_by_reliability` is static. `DagPlan.chosen_registry_idx` also available.
- **Beam-lifting:** `dag_beam_limit = 0` + `disable_plan_memo = 1` surfaces the full candidate set.
- **Fixture precedent:** `tests/test_dag.c:426-468` already exhibits the blind spot — a high-reliability,
  output-incompatible "decoy" consumes the beam budget so beam=1 misses a valid plan; beam=8 finds it.
  v5.1 generalizes this, using the residue δ as the **verifiable** correct producer (so `matched_expected`
  is checkable, not just "a plan exists").
- **Correction to recon:** `registry_add_certified` **does** exist (`circuit_demo.c:776`); the recon's
  "no such function" was an error. The fixture doesn't need it — it uses plain `registry_add` with
  `require_certified = 0` (uncertified decoys are considered).
- **const-registry note:** the probe receives `const PrimitiveRegistry *`. To lift the beam it makes a
  **shallow copy** of the registry struct (sharing the borrowed `entries`/BTNs), sets
  `dag_beam_limit = 0` + `disable_plan_memo = 1` on the copy, and plans with the copy — the passed
  registry is never mutated. Δ-4 counter snapshot/restore still wraps any `dag_execute` validation.

## Implementation plan (recon-first; for subagent execution, TDD)

1. **Recon (read-only):** answer the five questions above with file:line citations.
2. **Test first:** `tests/test_below_beam_recovery.c` — build the blind-spot fixture, snapshot
   authority-bearing state, run the probe, assert the measurement + no-authority + Δ-4 invariants.
3. **Contract:** add `CnetBelowBeamReport` + `proposal_sidecar_below_beam_probe` to
   `include/proposal_sidecar.h`.
4. **Impl:** extend `src/proposal_sidecar.c` — official plan vs beam-lifted plan, strict-validate,
   rank-check, Δ-4 snapshot/restore (repeat the v5.0 discipline on this new path).
5. **Makefile + `test_all` wiring:** mirror the v5.0 `test_proposal_sidecar` target + dispatch.
6. **Verify:** `make test_below_beam_recovery` then `make test` — green; new files warning-clean.

## Out of scope

The act/don't-act gate (v5.2+, contingent on what this measures). A characterization *study* of
recovery frequency / rank-gap distribution (useful input to the act decision; a budgeted study, not
this gate). Stochastic/diffusion proposer (v5.3). Symbol proposal family (v5.2). Deployment-cost
model.
```
