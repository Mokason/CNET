# Cost-Aware Freeze Gate (3A) — Cost Truth Per Minted Chunk — Design

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged)
**Milestone:** Cycle 3A of the Cost-Aware Compounding arc. The gate keeps minting chunks exactly as
today; it stops pretending they are free. Each minted chunk carries the teacher-vs-student MAC
comparison the engine already computes elsewhere — surfaced transiently in `LibraryReport`, advisory
only. **3A reports cost truth; 3C earns persistence when it acts on that truth.**

## Context

Cycle 2 measured that gated compounding is **planning-beneficial but compute-expensive**: the
distilled `dec_full_adder_unit` cut planner search 4.06× and plan length 3× but cost ~10.8× more MACs
per call than the lean primitives it replaced. The gate currently emits no signal of this — a caller
cannot tell a compute-cheap chunk from a compute-heavy one.

The comparison already exists in the engine: `CircuitConsolidationReport`
([include/router.h:638](../../../include/router.h)) carries `teacher_mac_estimate`,
`student_mac_estimate`, and `compression_ratio`, and `btn_mac_estimate` is literally `btn_cost`
([src/router.c:3658](../../../src/router.c)). 3A wires this same comparison into the distillation
gate's output so every minted chunk reports it — no new cost convention, no behavior change.

## The fields (transient, in `LibraryReport`)

Per minted chunk, **index-associated with the existing `names[i]` / `chunks[i]`** (same array index):

```c
size_t teacher_mac_estimate[LIBRARY_MAX_CHUNKS];  /* sum of btn_cost over the distilled plan's primitives */
size_t student_mac_estimate[LIBRARY_MAX_CHUNKS];  /* btn_cost(chunk) */
double compression_ratio[LIBRARY_MAX_CHUNKS];      /* teacher / student -- EXACT CircuitConsolidationReport orientation */
int    compute_beneficial[LIBRARY_MAX_CHUNKS];     /* student_mac_estimate < teacher_mac_estimate (INTEGER compare) */
```

Decisions locked from the brainstorm:
- **`compression_ratio = teacher / student`**, the exact orientation of `CircuitConsolidationReport`
  (router.c:3719): `> 1` means the chunk is *smaller* than the sub-plan (beneficial); `< 1` means it
  is *heavier* (our `dec_full_adder_unit`: `308/3328 = 0.093`). No parallel `execution_cost_ratio`.
- **`compute_beneficial` is derived from the INTEGER MAC estimates** (`student < teacher`), not from
  the floating ratio — avoids rounding ambiguity at the boundary.
- **No `planning_beneficial` field.** The worth-it guard already requires ≥2 primitives collapsing to
  1, so for any *minted* chunk planning is structurally always beneficial — a constant-true field
  would be noise.

## Data flow (advisory; the gate still mints exactly as today)

The teacher MAC is summed from the distilled plan's primitives, which differ per evolve path:
- `evolve_route`: `Σ btn_cost(plan.steps[i])` over `plan.length`.
- `evolve_dag` / `evolve_circuit`: `Σ btn_cost(owned[i]->btn)` over `DAG_PRIMITIVE` nodes in `owned[]`
  (the same flat walk the evidence check uses).

`finalize_chunk` is the single place that records a chunk into the report (`names[idx]`,
`chunks[idx]`, `chunk_count++`) **and it already holds the minted chunk**, so it gains exactly **one**
parameter — `teacher_mac` — computes the student cost itself, and writes all four label fields
atomically at that `idx`:
```c
size_t student_mac = btn_cost(student);            /* finalize already has the chunk */
report->teacher_mac_estimate[idx] = teacher_mac;
report->student_mac_estimate[idx] = student_mac;
if (student_mac > 0)      report->compression_ratio[idx] = (double)teacher_mac / (double)student_mac;
else if (teacher_mac > 0) report->compression_ratio[idx] = INFINITY;  /* free student: maximally beneficial */
else                      report->compression_ratio[idx] = 1.0;       /* both zero: neutral */
report->compute_beneficial[idx]   = (student_mac < teacher_mac) ? 1 : 0;
```
**Zero-cost handling is truthful and self-consistent** (the naive `student_mac ? ratio : 0.0` would
contradict itself — `compute_beneficial=1` while `ratio=0` reads as non-beneficial). Here
`student_mac == 0 < teacher_mac` gives `compute_beneficial = 1` **and** `ratio = INFINITY` — both say
"beneficial." **Invariant:** a minted chunk is trained, so `hidden_count > 0` ⇒ `btn_cost > 0`; the
zero branch is therefore defensive, not load-bearing — but it is defined truthfully rather than left
contradictory.

Each `evolve_*` computes only `teacher_mac` (`Σ btn_cost` over the plan, **before** it frees the
plan) and passes it to `finalize_chunk`; the student cost lives entirely in `finalize_chunk` (one
`btn_cost(student)`, not three). No planner / executor / registry change. **The change is
decision-identical, not byte-identical:** adding public `LibraryReport` fields changes the struct
layout, but every mint/defer/discard decision and all planner/executor behavior are unchanged.

## What consumes it

Nothing in the engine — by design. The **compounding benchmark is the first consumer**: after it
mints the Tier-2 chunk through `library_evolve_gated`, it prints the label so the Cycle-2 result
*self-explains* its planning-vs-compute story:
```
minted chunk: dec_full_adder_unit
teacher_macs=308 student_macs=3328 compression_ratio=0.093 compute_beneficial=false
```
This print is **observational only**: it must NOT assert `compute_beneficial == 0`, the current
ratio, or the exact student cost — a future capacity improvement that makes the chunk
compute-beneficial must keep the benchmark passing. The existing Run A / Run B assertions are
unchanged. This is the honest label Cycle 2 lacked.

## Testing (TDD, in `make test`)

The teacher-cost sum is implemented **separately in each of `evolve_route` / `evolve_dag` /
`evolve_circuit`**, so coverage must walk the *implementation-path* dimension, not just the
gated/ungated *wrapper* dimension. One successful mint through **each path** suffices (no need to run
every topology under both wrappers — the wrapper doesn't touch the cost fields):

| path | fixture |
|---|---|
| **circuit** (gated) | the existing `dec_full_adder_unit` circuit mint in `tests/test_distillation_gate.c` |
| **dag** | the cheapest existing successful-dag-mint fixture (extend it) |
| **route** | the cheapest existing successful-route-mint fixture (extend it — e.g. the dec→inc route already in the gate test) |

**Each repeated mint runs in a FRESH registry/fixture.** An ungated mint after a chunk is already
registered could alter planning, hit behavioral dedup, or pick the existing chunk as the teacher —
isolate them.

For each path's mint, assert the **formula**, not magic numbers (the chunk's exact MACs depend on
training/growth) — at the chunk's report index `i`:
- `report.teacher_mac_estimate[i] > 0` and equals the explicit Σ `btn_cost` over that plan's
  primitives (e.g. circuit: `2*btn_cost(dec_value) + btn_cost(dec_full_add)`).
- `report.student_mac_estimate[i] == btn_cost(report.chunks[i])`.
- `report.compression_ratio[i]` ≈ `(double)teacher / student` (within 1e-6).
- `report.compute_beneficial[i] == (report.student_mac_estimate[i] < report.teacher_mac_estimate[i])`
  — self-consistent regardless of training. (For the circuit chunk it resolves to `0` today; assert
  the *relation*, not the literal `0`, so a future capacity win doesn't break the test.)

## Non-goals (YAGNI)

- **No behavior change.** The gate does not refuse, reorder, or expand based on the label. That is 3C.
- **`RegistryEntry` untouched** — no persistence. 3C, when it has an actual consumer, will define what
  must persist (likely richer provenance — the replaced sub-plan, an expansion recipe, per-power-mode
  cost, versioned cost data — not merely a boolean).
- **No new cost metric / no `execution_cost_ratio`** — reuse `compression_ratio` verbatim.

## Resolved decisions (from review)

1. **`finalize_chunk` gains exactly ONE parameter (`teacher_mac`)** — it already holds the chunk, so
   it computes `student_mac = btn_cost(student)` and the derived fields centrally; the three
   `evolve_*` callers contribute only their teacher-cost sum. No per-caller record duplication, no
   redundant `btn_cost(student)`.
2. **The benchmark print is IN scope** for 3A (observational only — see "What consumes it"). The
   benchmark is the natural first consumer; without it 3A is only unit-test-inspected fields.
3. **Zero-cost student is defined truthfully** (`INFINITY`/`1.0`), never the contradictory `ratio=0`;
   backed by the stated `btn_cost > 0` invariant for a trained chunk.
4. **"Decision-identical," not "byte-identical"** — the struct grows public fields; decisions and
   planner/executor behavior are what stay unchanged.
