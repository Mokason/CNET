# Library Evolve: a DreamCoder-style Library-Learning Loop — Design

**Date:** 2026-06-13
**Status:** Approved

## Goal

Make the library grow by itself. Today, inventing a new chunk is a manual
three-step ritual the demos perform by hand ([decimal_demo.c], [chunk_demo.c]):
distill a proven plan into a student, emit its contract, certify-and-register.
This adds one driver — `library_evolve()` — that runs that ritual automatically
over a declared set of tasks, re-planning each pass so that chunks invented in
one pass shorten the plans of the next, until the library reaches a fixed point.

The wake/sleep framing of DreamCoder maps cleanly onto machinery that already
exists: the **wake** phase is the planners (`route_plan`/`dag_plan` maximizing
Π reliability); the **sleep** phase is `consolidate_*` distilling proven plans
into certified single-primitive abstractions. `library_evolve` is the outer loop
that alternates them.

## What this corrects from the original sketch

The proposed pseudocode was directionally right but had three concrete errors,
all confirmed against source:

1. **Inverted guard.** `property_check` returns `0` *exactly when the law holds*
   and `-1` otherwise ([property.c:342]). The sketch's
   `property_check(...) != 0 && r.violated == 0` fires only on the *refusal*
   path (a law that could not be evaluated, `r.inputs == 0`). "Law holds" is
   `property_check(...) == 0`.
2. **`consolidate_*` does not yield a `Contract`.** It yields a trained student
   BTN ([consolidate.h:63]). The contract is a *separate* call,
   `contract_from_route/dag` ([contract.h:95]). The real sequence is
   `consolidate_* → contract_from_* → registry_add_certified`.
3. **A `Property` cannot supply a plan, nor trigger consolidation.** It holds
   only primitive name-chains ([property.h:22]); a law can be satisfied by
   infinitely many chunks. Properties are a **regression guard**, not a trigger.
   Candidate plans come from the planners, driven by a declared task list.

So `library_evolve` = the working 3-step core, wrapped in three genuinely new
pieces: a **candidate generator** (re-plan the task list), a **dedup gate**
(keyed on the contract), and a **law guard with rollback**.

## Design decisions (locked)

| Decision | Choice |
|---|---|
| Candidate source | Re-plan a declared task/goal list each pass (no plan-log mining) |
| Persistence | In-process only for v1; registry rebuilt from base primitives at startup |
| Stopping rule | `max_iterations` cap **and** early-stop when a pass invents nothing |
| Property guard | Run `property_check` after each new chunk; roll back on a real violation |
| Dedup | Keep contract-based dedup (`btn_certify` against existing entries) |
| Rollback | Add a small `registry_remove_last` to the registry API |
| Multi-goal / circuit tasks | Deferred (route + single-goal dag only in v1) |

## Components

### 1. Module and API (`include/library.h`, `src/library.c`)

A new, self-contained module. It calls only existing public APIs; the planner,
executor, and consolidate cores are not modified (same ethos as
[consolidate.h:17]). It is **not** wired into `btn_train_dynamic`
([nn.c:1113]) — that trains one isolated BTN with no registry or tasks in scope,
the wrong altitude. `library_evolve` is a top-level driver, called between
planning sessions exactly as the demos drive the 3-step pattern today.

```c
#define LIBRARY_MAX_SOURCES BTN_MAX_INPUT_PORTS   /* 8 */
#define LIBRARY_MAX_CHUNKS  64                     /* cap on chunks invented per call */

typedef struct {
    const char *name;        /* name the invented chunk gets (atom [A-Za-z0-9_], <=63) */
    Port        sources[LIBRARY_MAX_SOURCES];
    size_t      n_sources;
    Port        goal;        /* single goal in v1 */
} LibraryTask;

typedef struct {
    char                    names[LIBRARY_MAX_CHUNKS][64];  /* invented, in order */
    BinaryTransformNetwork *chunks[LIBRARY_MAX_CHUNKS];     /* OWNED; freed by report_free */
    size_t                  chunk_count;
    size_t                  iterations_run;
    size_t                  rolled_back;                    /* law-guard rejections */
} LibraryReport;

/* Returns 0 on a clean run (report filled), -1 on an internal error (OOM, etc.).
   A task that yields no chunk is not an error. The registry borrows each invented
   chunk; report owns them. Call library_report_free AFTER you are done using reg. */
int  library_evolve(PrimitiveRegistry *reg,
                    const LibraryTask  *tasks, size_t n_tasks,
                    const Property     *laws,  size_t n_laws,
                    const ConsolidateConfig *cfg,   /* NULL => consolidate_config_defaults */
                    size_t max_iterations,
                    LibraryReport *report);

void library_report_free(LibraryReport *report);   /* frees the owned chunk BTNs */
```

Arity dispatch:
- `n_sources == 1` → `route_plan` → `consolidate_route` → `contract_from_route`
- `n_sources >= 2` → `dag_plan` → `consolidate_dag` → `contract_from_dag`

The driver constructs the `DagSource[]` array the dag planner/consolidator expect
from the task's source `Port`s (consolidation enumerates the canonical domain, so
only the source *types* are needed, not concrete values).

Throughout the pipeline, the enumeration cap for contract emission and law checks
is `cfg->max_samples` (the same `ConsolidateConfig.max_samples`, default 4096,
that bounds distillation) — one knob governs every domain enumeration in a call.

### 2. The evolve loop (cap + fixed point)

```
report.iterations_run = 0
for iter in 0 .. max_iterations-1:
    report.iterations_run++
    added_this_pass = 0
    for each task T in tasks:
        added_this_pass += try_consolidate(reg, T, laws, n_laws, cfg, report)
    if added_this_pass == 0:
        break                      /* fixed point: nothing new to invent */
```

The fixed point is self-enforcing. Once task `T`'s chunk exists, the next pass's
`route_plan(T)` returns a **length-1** plan (the chunk, seeded with its
100%-verification reliability so it dominates the longer chain), which the
worth-it guard skips. Because a new chunk can also shorten *other* tasks' plans,
a task that was not worth consolidating in pass `N` may become so in pass `N+1` —
that compounding is the point of iterating.

### 3. Per-candidate pipeline (`try_consolidate`, returns 0 or 1)

1. **Plan.** `route_plan`/`dag_plan`. No plan → return 0.
2. **Worth-it guard.** Skip unless the plan compresses ≥2 primitives (route:
   `length >= 2`; dag: ≥2 distinct primitive executions). A 1-primitive plan is
   already atomic and `consolidate_*` would refuse it ([consolidate.h:63] rejects
   `length < 2`).
3. **Distill.** `consolidate_route/dag(&plan, cfg, &student, &rep)`. `cfg` defaults
   via `consolidate_config_defaults` — `min_verify_rate = 1.0` (100% exact
   reproduction) and `initial_hidden = 0` (auto). **Never set `initial_hidden = 1`**
   (documented saturation trap). Refusal → return 0.
4. **Contract.** `contract_from_route/dag(&plan, T.name, max_samples, &c)` from the
   *same* plan.
5. **Dedup.** Before registering, scan the registry: if some existing entry's port
   signature matches `c` **and** `btn_certify(existing, &c, NULL) == 0`, the
   abstraction already exists → free the student, free the contract, return 0.
   Keyed on the **contract**, not the property (a law can be satisfied by many
   different chunks; only the contract answers "have I built *this* one").
6. **Register.** Snapshot `before = reg->count`; then
   `registry_add_certified(reg, &student, T.name, &c)`. Deterministic unique task
   names make this always an append, never a `contract_better_if` replacement
   contest. On failure → free student/contract, return 0.
7. **Law guard.** For each supplied `Property`,
   `property_check(law, reg, max_samples, &rep)`. Classify:
   - **violation** = `rc != 0 && rep.violated > 0` → real regression.
   - `rep.inputs == 0` (`rc != 0`) = law could not be evaluated (unresolved name,
     RAW domain, over-cap) → **warning**, not a violation; do *not* roll back.
   On any violation: **rollback** (§4), `report.rolled_back++`, return 0.
   Otherwise record the chunk in `report`, keep the student alive, return 1.

### 4. Ownership, rollback, and `registry_remove_last`

`library_evolve` owns every invented student BTN: each is heap-allocated, its
pointer stored in `LibraryReport.chunks[]`; the registry only **borrows** it
([router.h:11]). The caller frees them via `library_report_free` *after* it is
done using the registry. (Names in registry entries are likewise borrowed; the
report's `names[]` buffers back them.)

Rollback (step 7) needs to undo one append. The registry has no remove today, so
add:

```c
/* Drops the last entry (count check only; borrowed btn/name are left to the
   caller). Returns 0 on success, -1 if the registry is empty. */
int registry_remove_last(PrimitiveRegistry *reg);
```

It is a guarded `reg->count--` — safe because `registry_free` frees only the
entries array and both `btn` and `name` are borrowed ([router.h:56-58]). After
truncating, `library_evolve` frees the just-distilled student BTN it owns. Sound
because unique naming guarantees step 6 was an append, with no incumbent to
restore.

### 5. Reporting

`LibraryReport` (above) records, in order, every invented chunk name and its
owned BTN, plus `iterations_run` and `rolled_back`. This is the audit trail a
caller prints ("invented `dec_carry_add`, `dec_sum2`; converged in 2 passes; 1
rollback") and the handle through which it frees the chunks.

### 6. Testing (TDD, decimal domain)

Built test-first against the **decimal domain** (already in the frozen suite,
deterministic; fixed `seed = 131`). New `tests/library_demo.c`, wired into
`make` as its own target and into `make test`:

- **Invention.** Load base decimal primitives; declare 2–3 tasks whose optimal
  plans are length ≥ 2; run `library_evolve`; assert the expected chunks are
  invented, certified, and registered.
- **Fixed point / dedup.** A second `library_evolve` call (same tasks) invents
  **nothing** and runs exactly one pass before early-stopping
  (`iterations_run == 1`, `chunk_count == 0`).
- **Law guard rollback.** A task constructed to break a supplied law triggers a
  rollback: `report.rolled_back == 1`, registry `count` unchanged from before
  that task, and the surviving registry still satisfies every law
  (`property_check == 0`).
- **Determinism.** Exact counts and invented-chunk weights are reproducible by
  seed, consistent with the project's "byte-identical regen is the regression
  gate" discipline.

## Out of scope for v1 (clean extension points)

- **Cross-run persistence.** `registry_save` already writes `<name>.btn/.contract/
  .stats` ([router.h:52-54]); the missing half is a load/manifest path and a
  base-vs-learned distinction. Deferred.
- **Multi-goal circuit tasks.** `dag_plan_circuit` / `consolidate_circuit` /
  `contract_from_circuit` all exist; adding a `goals[]` task variant is a clean
  follow-on.
- **Runtime plan-log mining** (the hybrid candidate source): record successful
  executions and slice frequent sub-chains. Larger; needs a plan log the stateless
  planners don't keep today.
- **The other three CSV ideas.** Neural-Module-Networks-style learned layout
  scoring already ≈exists (`btn_reliability` + `rank_by_reliability` + the beam in
  `dag_search`); STITCH/e-graph library refactoring is a large greenfield
  subsystem; certified-robustness interval propagation is a separate numerical
  module. None block this loop.

[decimal_demo.c]: tests/decimal_demo.c
[chunk_demo.c]: tests/chunk_demo.c
[property.c:342]: src/property.c
[property.h:22]: include/property.h
[consolidate.h:63]: include/consolidate.h
[consolidate.h:17]: include/consolidate.h
[contract.h:95]: include/contract.h
[nn.c:1113]: src/nn.c
[router.h:11]: include/router.h
[router.h:56-58]: include/router.h
[router.h:52-54]: include/router.h
