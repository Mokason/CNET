# Compounding-Loop Benchmark — The Decimal Ladder — Design

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged)
**Milestone:** Cycle 2 of the compounding-loop arc. Put a measured number on the central CNET
thesis — that **safe, gated compounding shortens future work** — by proving compositional transfer
on a decimal-addition curriculum, end-to-end through the shipped distillation gate.

## Context

The mechanism is already proven to *exist*: `capacity_demo.c` shows `dag_plan_circuit` discovering a
multi-column ripple-carry and *reusing* a certified single-column chunk to build a wider adder. What
is missing is a **measured, falsifiable delta** that runs the loop through the real bouncer
(`library_evolve_gated`) and reports the reduction in planner search effort and plan size. This
benchmark formalizes the demo into an A/B experiment with hard assertions.

The load-bearing constraint discovered while grounding (`capacity_demo.c:258` — a replan *kept* the
longer 2-chunk path "because evidence"): the deterministic planner maximizes Π-reliability, so a
shorter path made of an **un-evidenced** chunk loses to a longer path of evidenced primitives. The
benchmark must therefore make the Tier-2 chunk *trusted* before Run B, or the shortcut never
appears (a false negative). This dictates the exact sequence below.

## The curriculum (Decimal Ladder, N = 4)

- **Tier 1 (base):** the raw, trained, certified primitives `dec_value` (`dec_symbol`→`dec_digit`)
  and `dec_full_add` (`dec_digit, dec_digit, dec_carry`→`dec_sum, dec_carry`), loaded from their
  committed weights.
- **Tier 2 (the compounder):** the single-column adder chunk `dec_full_adder_unit`
  (`dec_symbol a, dec_symbol b, dec_carry cin`→`dec_sum, dec_carry cout`) — the exact circuit Cycle 1
  already distilled. Minted **through `library_evolve_gated`** in this benchmark.
- **Tier 3 (the proof):** 4-digit addition — `n_sources = 9` (a0..a3, b0..b3 as `dec_symbol`, plus
  one external `dec_carry`), `n_goals = 5` (sum0..sum3 as `dec_digit`, plus final `dec_carry`).
  Probed directly via `dag_plan_circuit`.

Feasibility is grounded: 5 goals ≤ `CIRCUIT_MAX_ROOTS` (8); `dag_plan_circuit` does not cap
`n_sources`; the ~5-deep carry chain ≤ `DAG_MAX_DEPTH` (8). Run A and Run B solve the **identical**
Tier-3 task (same ports), differing only in which primitives the registry offers.

## The sequence (Run A → Accrual → Gate-Distill → Run B)

1. **Registry setup:** register Tier-1 `dec_value` + `dec_full_add` (certified).
2. **Run A (blank slate):** probe the Tier-3 4-digit task with `attention_mode = SHADOW` (SHADOW
   plans identically to OFF but populates `attention.nodes_expanded`). Record `RunMetrics`:
   `nodes_expanded`, `plan_length` (distinct `DAG_PRIMITIVE` in `owned[]`), `macs` (Σ `btn_cost`
   over those primitives). Expectation: 4 columns × (2 `dec_value` + 1 `dec_full_add`) ≈ 12
   primitives.
3. **Evidence batch:** `dag_execute_circuit` the **Tier-2 single-column** task on ≥16 valid random
   inputs. The executors increment `output_successes` on each in-domain result
   ([router.c:3107](../../../src/router.c)), so `dec_value` and `dec_full_add` accrue **real**
   evidence and clear the `0.9/16` bar.
4. **Gate-distill Tier 2:** build the Tier-2 `LibraryTask` (3 sources, 2 goals), run
   `library_evolve_gated` with `enabled = 1` and a **training override** in the `ConsolidateConfig`
   (`min_verify_rate ≈ 0.95`, raised `max_epochs`) so the nonlinear mod-10/carry chunk converges and
   the gate admits it. **Record** (do not yet assert) `report.chunk_count` into `chunk_minted` and
   capture the minted chunk's `name`/ports for the Step-7 identity check — the `LibraryTask.name` is
   `"dec_full_adder_unit"`. The minted chunk is registered (FROZEN); `consolidate` seeds its
   reliability from the verification pass. (If nothing minted, Steps 5–6 still run with whatever the
   registry holds, and the Step-7 asserts fail *after* the report prints — never silently.)
5. **Trust the chunk (Π-safeguard):** re-plan the **Tier-2 single-column** task (the chunk is now
   registered and, being one fat node vs three, already the planner's pick) and `dag_execute_circuit`
   that plan on ≥16 valid inputs — so the new chunk accrues real executions and its reliability is
   unambiguously ≥ 0.944 (`17/18`), exceeding a raw 3-primitive column's Π (`0.944³ = 0.841`). This
   guarantees the planner *prefers* the chunk in Run B rather than rebuilding the column from raw
   primitives. (A multi-output chunk is executed through the circuit executor, not `route_execute`.)
6. **Run B (compounded):** probe the **same** Tier-3 task with `dec_full_adder_unit` now in the
   registry, again in SHADOW. Record `RunMetrics`. Expectation: 4 columns × 1 chunk ≈ 4 primitives.
7. **Report, then assert the delta.** Fill `CompoundingReport` (including `chunk_uses` for both
   runs, counted by chunk name in `owned[]`), **print the full table first**, then run the asserts in
   the order of the Assertions section (mint identity → causal-story → direction). The print always
   precedes any abort.

## The metrics & report struct

```c
typedef struct {
    size_t nodes_expanded;   /* plan.attention.nodes_expanded (SHADOW telemetry) */
    size_t plan_length;      /* distinct DAG_PRIMITIVE executions in owned[] */
    size_t macs;             /* sum of btn_cost over the plan's primitives */
    size_t chunk_uses;       /* # owned[] nodes that ARE the Tier-2 chunk (by name) */
} RunMetrics;

typedef struct {
    RunMetrics raw;          /* Run A: Tier-1 primitives only (chunk_uses == 0) */
    RunMetrics compounded;   /* Run B: Tier-2 chunk available + evidenced */
    double nodes_factor;     /* raw.nodes_expanded / compounded.nodes_expanded */
    double length_factor;    /* raw.plan_length    / compounded.plan_length    */
    double macs_factor;      /* raw.macs           / compounded.macs           */
    int    chunk_minted;     /* did Tier-2 distill through library_evolve_gated? */
} CompoundingReport;
```

`plan_length` (Σ primitives in `owned[]`) is computed with the same flat walk `evolve_circuit`
already uses; `macs` sums the public `btn_cost(p)` over those primitives; `chunk_uses` counts the
`owned[]` nodes whose `name == "dec_full_adder_unit"` — making Run B's *reason* for shrinking
observable (it actually composed the chunk), not merely inferred from a smaller `plan_length`.

## Assertions (falsifiable; direction asserted, magnitude measured)

**Ordering is mandatory: compute the full `CompoundingReport` → PRINT it in full → THEN assert.**
Because `nodes_expanded` is the risky headline, a failing assert must abort *after* the table is on
screen, so the "real finding" case ships its evidence rather than just a dead process. No `assert`
fires before the print.

**Mint identity** (bind the gate's output to the intended shape, not just a count):
- `chunk_minted == 1` AND `strcmp(report.names[0], "dec_full_adder_unit") == 0`.
- the minted `report.chunks[0]` has the expected signature: `input_port_count == 3`
  (`dec_symbol, dec_symbol, dec_carry`) and `output_port_count == 2` (`dec_sum, dec_carry`) — assert
  the port families/widths/tags match. This blocks a false pass where the gate mints some *other*
  chunk shape.

**Causal-story** (the shortcut happened for the intended reason):
- `raw.chunk_uses == 0` — Run A had no chunk to use.
- `compounded.chunk_uses >= 4` — Run B's 4-digit plan actually composed the Tier-2 chunk once per
  column (4 columns). This is what distinguishes "compounded for the intended reason" from "the
  planner found some unrelated shortcut."

**Direction** (the compounding claims):
- `compounded.plan_length < raw.plan_length` — the **guaranteed** structural shortcut (~12 → ~4);
  deterministic.
- `compounded.nodes_expanded < raw.nodes_expanded` — the headline claim: the chunk collapses the
  search tree.

**Reported, not asserted:** the three `*_factor` magnitudes. We *measure* the reduction; we do not
hard-code "10×". The default beam(8) + reachability pruning cap search breadth, so the
`nodes_factor` may be a few-fold rather than an order of magnitude — the benchmark prints the real
number. (Honest-measurement discipline: assert the sign, report the magnitude.)

**Known risk to surface, not hide:** Run B's registry has one *more* candidate per obligation (the
chunk), which could raise per-obligation breadth even as it shortens the plan. If
`compounded.nodes_expanded` does **not** drop, that is a real finding — compounding reduced *plan
size* but not *search effort* — and the failing assert surfaces it rather than papering over it. The
`plan_length` and `macs` deltas remain valid regardless.

## Structure & integration

- **`tests/benchmark_compounding_loop.c`** (new): the whole experiment in `main()`, printing a small
  table and `assert()`-ing the deltas (so a regression flips it red).
- **`Makefile`** (new target `compounding_bench`, mirroring `dgate_bench`/`lifecycle_bench`): links
  `nn,router,plan_table,contract,property,consolidate,scan,library` + the bench, builds and runs it.
- It is a **standalone benchmark target** (like `dgate_bench`), not part of the unit `make test`
  suite — it trains a nonlinear chunk (seconds), too heavy for the fast suite. It is self-checking
  via `assert`, so `make compounding_bench` is the gate.

## Non-goals (YAGNI)

- **Arithmetic correctness of the distilled chunk.** This benchmark proves *topological* compounding
  (search/plan/compute shrink). The chunk need only be good enough to certify + be preferred; we are
  not chasing 100% mod-10 accuracy (explicitly out of scope, per the cycle's framing).
- **N > 4 / variable-width sweeps**, persistent cross-run dedup, the priority scheduler.
- **Wiring into `make test`** — standalone bench only.

## Open points for review

1. **Run A planning.** A 4-column ripple from raw primitives must be discoverable by
   `dag_plan_circuit` within beam(8) + `DAG_MAX_DEPTH`(8). Grounding says yes (the 2-digit case is
   discovered; depth ~5 < 8), but if Run A fails to plan, the benchmark STOPs and reports — we do
   not hand-build the Run A plan.
2. **The `nodes_expanded` direction** (the risk above) — if it doesn't drop, we report it as the
   finding and keep `plan_length`/`macs` as the compounding evidence.
3. **Micro-batch necessity.** `consolidate` seeds the chunk's reliability from verification
   (~0.945 at 0.95 verify), which already clears the 0.84 raw-column bar; the explicit Step-5
   micro-batch is a determinism safeguard. If it proves redundant in practice, it can be dropped —
   but it is cheap insurance against the Π-reliability false-negative.
