# Circuit Gate Extension — Multi-Output Distillation Admission — Design

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged)
**Milestone:** Cycle 1 of the compounding-loop arc. Extend the distillation admission gate
(`library_evolve_gated`) to **multi-output circuit** plans, so the same `CERTIFIED ∧
EVIDENCE_CLEAR` guarantee protects circuit distillation. Cycle 2 (the Decimal-Ladder compounding
benchmark) depends on this and is out of scope here.

## Context

The shipped gate ([src/library.c](../../../src/library.c)) is **single-goal only**: `LibraryTask`
carries one `Port goal`, and `try_consolidate` dispatches to `evolve_route` (1 source) or
`evolve_dag` (≥2 sources, 1 goal). But the compelling compositional domains — decimal carry-
propagating addition, ripple-carry, split fan-out — are inherently **multi-output**: even one
decimal column emits *sum + carry*. Those plans are **circuits** (`dag_plan_circuit` /
`CircuitPlan`), and `consolidate_circuit` distills them into multi-output chunks — but
`library_evolve` never calls it, so circuit distillation is currently **ungated**. This cycle
closes that: a multi-output chunk should clear the same trust bar as a single-output one before it
is frozen into the registry.

## The struct extension (backward-compatible)

Add to `LibraryTask` (alongside, not replacing, the existing `goal`):

```c
Port   goals[CIRCUIT_MAX_ROOTS];  /* multi-output circuit goals (in root order) */
size_t n_goals;                   /* >= 2 selects the circuit path; 0/1 use `goal` */
```

**Why keep `goal` and add `goals[]` rather than unify on `goals[]`/`n_goals`?** Zero-churn
stability. Every current caller (`test_library`, the decimal/library fixtures, the just-shipped
`test_distillation_gate`) sets `.goal` and leaves `n_goals == 0`. Unifying would re-churn green,
just-committed code to remove one redundant field — a defensive-engineering loss for a cosmetic
gain. The redundancy is bounded and fully explained by the dispatch rule below: **`n_goals >= 2`
means "this is a circuit, read `goals[]`"; otherwise the single `goal` field is authoritative**
(`n_goals` 0 and 1 are equivalent — the route/dag path; a single-goal "circuit" is just a dag under
the port-disjoint sharing rule, so it never needs the circuit path).

## The dispatch matrix

`try_consolidate` gains one branch, checked **first** so multi-output intent wins:

```c
if (task->n_sources == 0 || task->n_sources > LIBRARY_MAX_SOURCES) return 0;  /* malformed */
if (task->n_goals >= 2)   return evolve_circuit(reg, task, laws, n_laws, cfg, report, gs);  /* NEW */
if (task->n_sources == 1) return evolve_route (reg, task, laws, n_laws, cfg, report, gs);
return evolve_dag(reg, task, laws, n_laws, cfg, report, gs);                  /* n_sources >= 2 */
```

This makes the dispatcher a legible 2×2 over the data structure's declared topology:

| | 1 goal | ≥2 goals |
|---|---|---|
| **1 source** | `evolve_route` | `evolve_circuit` |
| **≥2 sources** | `evolve_dag` | `evolve_circuit` |

The `LibraryTask` shape now *signals the topological intent* of the evolution pass — a maintainer
reads `n_goals` and knows which planner/distiller fires.

## The flat-loop evidence check (the key insight to document)

`circuit_evidence_clear` is **not** a recursive multi-root traversal, and it needs **no**
sub-graph deduplication — because `CircuitPlan.owned[]` is *already* the sharing-aware, flattened,
deduplicated node closure. The port-disjoint fan-out rule ([router.h:428](../../../include/router.h))
means a node shared across roots has multiple *consumers* but appears **exactly once** in
`owned[]` (the clone-map builds it that way). So a shared sub-adder is one node with two readers,
not two nodes.

Therefore the circuit evidence check is the **same flat loop** as `dag_evidence_clear`, pointed at
`CircuitPlan.owned[]`, reusing the existing `btn_evidence_clear` helper verbatim:

```c
static int circuit_evidence_clear(const CircuitPlan *plan, const GateState *gs) {
    size_t i;
    if (!gate_on(gs)) return 1;
    for (i = 0; i < plan->owned_count; ++i)
        if (plan->owned[i] && plan->owned[i]->kind == DAG_PRIMITIVE &&
            !btn_evidence_clear(plan->owned[i]->btn, gs->cfg->evidence_threshold,
                                gs->cfg->min_evidence)) return 0;
    return 1;
}
```

Each primitive in the entire multi-root topological closure is checked **exactly once**, regardless
of how many output ports / roots read it. The "don't duplicate work on overlapping sub-graphs"
requirement is satisfied by construction, not by added logic. `owned[i]->btn` borrows the registry
primitive's BTN, so its `output_successes/failures` reflect real runtime accrual (same as the dag
path).

## `evolve_circuit` (mirror of `evolve_dag`)

```
build DagSource[] from task->sources[] (values = NULL; enumeration reads types only)
dag_plan_circuit(reg, sources, n_sources, task->goals, task->n_goals, &cp)   -> else return 0
worth-it guard: count distinct DAG_PRIMITIVE in cp.owned[] >= 2   -> else circuit_free, return 0
circuit_evidence_clear(&cp, gs)   -> else report->deferred++, circuit_free, return 0   [EVIDENCE_CLEAR]
consolidate_circuit(&cp, sources, n_sources, cfg, student, NULL)  -> else free student, circuit_free, return 0
contract_from_circuit(&cp, sources, n_sources, task->name, cfg->max_samples, &c) -> else cleanup
circuit_free(&cp)                 (finalize needs only reg, student, contract)
accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws, cfg->max_samples, report)  [CERTIFIED]
```

Same **Hard-AND** as the dag path: `EVIDENCE_CLEAR` (the new flat check) AND `CERTIFIED` (the
existing `consolidate_circuit` full-domain verification + `finalize_chunk`'s `btn_certify` /
behavioral dedup / law-guard, which are contract- and arity-agnostic). Same **DEFER** routing
(`report.deferred++`, re-eligible next pass). Gate-off (`gate_on` false / `n_goals == 0`) is
byte-identical legacy — circuit tasks simply weren't expressible before, so nothing regresses.

## Reuse (cores untouched)

`btn_evidence_clear`, `finalize_chunk`, `GateState`/`gate_on`, `library_evolve` /
`library_evolve_gated` (the gated entry already threads `gs`; the circuit path threads the same
`gs`), `consolidate_circuit`, `contract_from_circuit`, `dag_plan_circuit`, `circuit_free`. **New
code:** the `LibraryTask` fields, the dispatch branch, `circuit_evidence_clear`, `evolve_circuit`,
and a `count_circuit_primitives(cp)` worth-it helper (distinct `DAG_PRIMITIVE` in `owned[]`). The
planner / executor / consolidate cores are not modified.

## Testing (TDD, in `make test`)

Model the fixture on `tests/test_circuit.c`'s existing, proven-distillable circuits:

- **Shared-node coverage:** a circuit whose `owned[]` contains a node read by ≥2 roots (e.g. the
  split fan-out, `test_circuit.c:356`). Assert the evidence check covers that shared primitive —
  if it is the only un-evidenced node, the whole plan DEFERs; once it clears, the plan proceeds.
  This proves the flat loop visits the shared node and gates on it.
- **EVIDENCE gate:** a ≥2-goal, ≥2-distinct-primitive circuit (e.g. ripple-carry or and/or). At 0
  evidence with the gate enabled → `chunk_count == 0`, `deferred >= 1`. Seed every `owned[]`
  primitive to `(16, 0)` → it mints a **multi-output** chunk (`chunk_count >= 1`).
- **Gate-off parity:** the same circuit task with the gate disabled distills exactly as an ungated
  circuit pass would (and existing single-goal `library_evolve` is untouched — `n_goals == 0`).
- **Regression:** `test_library`, `test_circuit`, and `test_distillation_gate` stay green; full
  `make test` green.

## Non-goals (YAGNI)

- **The compounding benchmark / Decimal Ladder** — that is Cycle 2, a separate spec.
- **Persistent cross-run dedup** — still deferred.
- **`n_goals == 1` circuits** — a single-goal plan is a dag; the circuit path is `n_goals >= 2`
  only.
- **Goals beyond `CIRCUIT_MAX_ROOTS`** — bounded by the existing planner limit; `dag_plan_circuit`
  already rejects `n_goals > CIRCUIT_MAX_ROOTS`.

## Open points for review

1. **`finalize_chunk` multi-output assumption.** It is expected to be arity-agnostic (it does
   `contract_already_known` + `registry_add_certified` + `law_violated`, all over a `Contract` +
   `BinaryTransformNetwork *`, neither of which is single-output-specific). The implementation must
   confirm `registry_add_certified` accepts a multi-output student + circuit contract unchanged; if
   it has a latent single-output assumption, that surfaces as the one real risk in Cycle 1.
2. **Worth-it threshold for circuits.** Defined as "≥2 distinct `DAG_PRIMITIVE` in `owned[]`,"
   matching `consolidate_circuit`'s documented "≥2 distinct primitive executions" precondition. A
   circuit that collapses to a single shared primitive (e.g. pure split fan-out) is correctly *not*
   distilled.
