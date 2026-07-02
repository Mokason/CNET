# Circuit Gate Extension Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the opt-in distillation gate to multi-output **circuit** plans — `LibraryTask` gains `goals[]`/`n_goals`, `try_consolidate` dispatches `n_goals >= 2` to a new `evolve_circuit` that applies the same `CERTIFIED ∧ EVIDENCE_CLEAR` gate (the evidence leg a flat loop over `CircuitPlan.owned[]`).

**Architecture:** Mirror `evolve_dag` for circuits: `dag_plan_circuit` → worth-it (≥2 distinct primitives in `owned[]`) → `circuit_evidence_clear` (flat loop over the sharing-aware deduplicated `owned[]`, reusing `btn_evidence_clear`) → `consolidate_circuit` → `contract_from_circuit` → `finalize_chunk` (already arity-agnostic). Gate-off / `n_goals == 0` stays byte-identical legacy.

**Tech Stack:** C11, MinGW gcc (`-mno-avx` required), Makefile single-exe suite. Reuses `btn_evidence_clear`, `GateState`/`gate_on`, `finalize_chunk`, `consolidate_circuit`, `contract_from_circuit`, `dag_plan_circuit`, `circuit_free`.

> **Commits on this repo are user-managed.** Each "Commit (user)" step lists files to stage; the user runs the commit. Do not run `git add`/`git commit` unless authorized in-session.

**Spec:** `docs/superpowers/specs/2026-06-19-circuit-gate-extension-design.md`

---

## File Structure

- **`include/library.h`** (modify): add `goals[CIRCUIT_MAX_ROOTS]` + `n_goals` to `LibraryTask` (+ doc).
- **`src/library.c`** (modify): `count_circuit_primitives`, `circuit_evidence_clear`, `evolve_circuit`, the `n_goals >= 2` dispatch branch. Reuses existing helpers.
- **`tests/test_distillation_gate.c`** (modify): the circuit gate test (already wired into `make test`).

Build command (the gate test links the full chain):
```bash
gcc -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx -Wno-unused-function \
  -o test_distillation_gate.exe \
  src/nn.c src/router.c src/plan_table.c src/contract.c src/property.c src/consolidate.c src/scan.c src/library.c \
  tests/test_distillation_gate.c -lm
```

---

## Task 1: Pre-flight — confirm `finalize_chunk` is arity-agnostic (directive, no code)

**Files:** none (verification gate).

- [ ] **Step 1: Confirm the three evidence points**

Read and confirm each:
1. `include/contract.h` — `Contract` has `Port output_ports[BTN_MAX_OUTPUT_PORTS]; size_t output_port_count;` (an ARRAY + count, not a single `output_port`).
2. `src/contract.c` `registry_add_certified` (~line 696) — it calls `btn_certify(btn, c, NULL)` and operates on the whole `btn`/`Contract`; grep its body for any `output_port` (singular, indexed `[0]` or `->output_port`) usage. Expected: none — it never indexes a single output port.
3. Existing multi-output registrations already work: `grep -n 'dec_full_adder_unit' tests/circuit_demo.c tests/test_circuit.c tests/capacity_demo.c` — `dec_full_adder_unit` (a 2-output sum+carry chunk) is registered via `registry_add_certified` in production demos/tests.

- [ ] **Step 2: Record the conclusion**

Expected conclusion: **arity-agnostic — no fix needed.** `finalize_chunk` → `registry_add_certified` → `btn_certify` all operate over the contract's `output_ports[]` array. If (contrary to the evidence) Step 1 finds a single-output assumption — e.g. `registry_add_certified` indexing `c->output_ports[0]` as if it were the only port, or `btn_certify` ignoring ports `[1..]` — STOP and report it: fixing it (loop over `output_port_count`) becomes a prepended task before Task 2.

---

## Task 2: Extend `LibraryTask` + dispatch + stub `evolve_circuit`

**Files:**
- Modify: `include/library.h`, `src/library.c`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_distillation_gate.c` (above `run_test_distillation_gate`):
```c
static void test_circuit_task_shape(void) {
    /* A LibraryTask can carry multiple goals; n_goals defaults to 0 (legacy). */
    LibraryTask t;
    memset(&t, 0, sizeof t);
    CHECK(t.n_goals == 0, "LibraryTask.n_goals zero-inits to 0 (legacy single-goal)");
    t.n_goals = 2;
    CHECK(t.n_goals == 2 && (sizeof t.goals / sizeof t.goals[0]) >= 2,
          "LibraryTask carries goals[] for the circuit path");
}
```
Add `test_circuit_task_shape();` into `run_test_distillation_gate`. Build (full chain) → fails (`no member named 'goals'`/`'n_goals'`).

- [ ] **Step 2: Extend the struct**

In `include/library.h`, inside `typedef struct { ... } LibraryTask;` (after `Port goal;`):
```c
    /* Multi-output circuit goals. n_goals >= 2 selects the circuit path
       (evolve_circuit); n_goals 0 or 1 use the single `goal` field above
       (route/dag). Kept ALONGSIDE `goal` so every existing single-goal caller
       stays byte-identical (n_goals == 0). The dispatcher reads n_goals to know
       the topological intent of the task. */
    Port   goals[CIRCUIT_MAX_ROOTS];
    size_t n_goals;
```
(`CIRCUIT_MAX_ROOTS` is from `router.h`, already included by `library.h`.)

- [ ] **Step 3: Add the dispatch branch + a stub `evolve_circuit`**

In `src/library.c`, add a stub above `try_consolidate`:
```c
/* Multi-output circuit distillation under the gate. Stub in this task; real
   logic in the next. Returns 1 if a chunk was minted, else 0. */
static int evolve_circuit(PrimitiveRegistry *reg, const LibraryTask *task,
                          const Property *laws, size_t n_laws,
                          const ConsolidateConfig *cfg, LibraryReport *report,
                          GateState *gs) {
    (void)reg; (void)task; (void)laws; (void)n_laws; (void)cfg; (void)report; (void)gs;
    return 0;
}
```
In `try_consolidate`, add the branch FIRST (after the malformed-arity guard, before the n_sources checks):
```c
    if (task->n_goals >= 2) {
        return evolve_circuit(reg, task, laws, n_laws, cfg, report, gs);
    }
```

- [ ] **Step 4: Run — shape test passes, regression green**

Build the gate test → `test_circuit_task_shape` passes; all earlier gate tests pass. Then regression:
```bash
gcc -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx -Wno-unused-function -o test_library.exe src/nn.c src/router.c src/plan_table.c src/consolidate.c src/contract.c src/property.c src/library.c tests/test_library.c -lm && ./test_library.exe
```
Expected: `ALL LIBRARY TESTS PASS` (legacy callers with `n_goals==0` unaffected).

- [ ] **Step 5: Commit (user)**

Stage: `git add include/library.h src/library.c tests/test_distillation_gate.c`
Message: `feat(library): LibraryTask multi-goal fields + n_goals>=2 circuit dispatch (stub)`

---

## Task 3: `circuit_evidence_clear` + `count_circuit_primitives` + real `evolve_circuit`

**Files:**
- Modify: `src/library.c`, `tests/test_distillation_gate.c`

**Fixture note (the dec_full_adder circuit = Decimal-Ladder Tier 2):** load the two trained
decimal primitives from their committed weights and register them, then drive a 2-goal circuit
task. Ports (from `decimal_demo.c`): `dec_value`: in `PORT_ONEHOT 10 "dec_symbol"` → out
`PORT_BINARY_MSB 4 "dec_digit"`. `dec_full_add`: in `[BINARY_MSB 4 "dec_digit", BINARY_MSB 4
"dec_digit", BINARY_MSB 1 "dec_carry"]` → out `[BINARY_MSB 4 "dec_sum", BINARY_MSB 1 "dec_carry"]`.
Load via `btn_load(&b, "dec_value_weights.txt")` / `"dec_full_add_weights.txt"` (the v4 weight
format restores ports+tags); if ports are absent, set them with `btn_set_ports` per the above.
Task: `n_sources=3` (a `dec_symbol`, b `dec_symbol`, carry `dec_carry`), `goals=[dec_sum, dec_carry]`,
`n_goals=2`. `dag_plan_circuit` discovers `dec_full_add(dec_value(a), dec_value(b), carry) ->
(sum, carry)`; `dec_full_add` is the **shared node** feeding both goal roots via its two output
ports, so `owned[]` = `{dec_value(a), dec_value(b), dec_full_add}` with `dec_full_add` present once.

- [ ] **Step 1: Write the failing test**

Add `test_circuit_evidence_gate()` to `tests/test_distillation_gate.c`. It builds the fixture above
and asserts:
```c
/* (helpers) load + register dec_value/dec_full_add; build the 2-goal LibraryTask;
   gate_set_ev(b, s, f) sets b->output_successes=s; b->output_failures=f. */

/* Branch A: gate enabled, ALL primitives 0 evidence -> DEFER, no mint. */
/*   library_evolve_gated(reg, &task, 1, NULL, 0, NULL, &g_enabled, 3, &rep)
     CHECK(rep.chunk_count == 0 && rep.deferred >= 1); library_report_free(&rep); */

/* Branch B (shared-node coverage): seed ONLY dec_value to (16,0), leave
   dec_full_add at 0 -> STILL DEFER (proves the SHARED dec_full_add node in
   owned[] is checked). CHECK(rep.chunk_count == 0 && rep.deferred >= 1). */

/* Branch C: seed BOTH dec_value AND dec_full_add to (16,0) -> mints a
   MULTI-OUTPUT chunk. CHECK(rep.chunk_count >= 1). Inspect the minted chunk has
   output_port_count == 2 (sum + carry). */

/* Branch D (gate-off parity): library_evolve (ungated) on the same fresh fixture
   distills the circuit too (or, if you prefer, library_evolve_gated with a
   disabled gate) -> chunk_count >= 1 regardless of evidence. */
```
Add `test_circuit_evidence_gate();` to `run_test_distillation_gate`. Build (full chain) → fails
(the stub `evolve_circuit` mints nothing, so Branch C/D fail).

- [ ] **Step 2: Implement the helpers + real `evolve_circuit`**

In `src/library.c`, replace the stub. Add the worth-it counter + the flat evidence loop:
```c
/* Distinct primitive nodes in a circuit plan (sharing-aware: owned[] holds each
   node once). The worth-it guard + consolidate_circuit's ">= 2 distinct
   primitive executions" precondition. */
static int count_circuit_primitives(const CircuitPlan *p) {
    size_t i; int n = 0;
    if (p->owned == NULL) return 0;
    for (i = 0; i < p->owned_count; ++i)
        if (p->owned[i] != NULL && p->owned[i]->kind == DAG_PRIMITIVE) ++n;
    return n;
}

/* EVIDENCE_CLEAR for circuits: a FLAT loop over owned[] (the sharing-aware,
   deduplicated multi-root closure -- a node shared across roots appears exactly
   once, by the port-disjoint fan-out rule), reusing btn_evidence_clear. Not
   recursive; no sub-graph dedup needed -- owned[] already is the dedup. */
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
Replace the stub `evolve_circuit` body (mirror of `evolve_dag`):
```c
static int evolve_circuit(PrimitiveRegistry *reg, const LibraryTask *task,
                          const Property *laws, size_t n_laws,
                          const ConsolidateConfig *cfg, LibraryReport *report,
                          GateState *gs) {
    DagSource sources[LIBRARY_MAX_SOURCES];
    CircuitPlan plan;
    BinaryTransformNetwork *student;
    Contract c = {0};
    size_t i;
    int accepted;

    for (i = 0; i < task->n_sources; ++i) {
        sources[i].type = task->sources[i];
        sources[i].values = NULL;
    }
    memset(&plan, 0, sizeof plan);
    if (dag_plan_circuit(reg, sources, task->n_sources,
                         task->goals, task->n_goals, &plan) != 0) return 0;
    if (count_circuit_primitives(&plan) < 2) { circuit_free(&plan); return 0; }  /* worth-it */
    if (!circuit_evidence_clear(&plan, gs)) {                                     /* EVIDENCE_CLEAR */
        report->deferred++; circuit_free(&plan); return 0;
    }

    student = calloc(1, sizeof *student);
    if (student == NULL) { circuit_free(&plan); return 0; }
    if (consolidate_circuit(&plan, sources, task->n_sources, cfg, student, NULL) != 0) {
        free(student); circuit_free(&plan); return 0;
    }
    if (contract_from_circuit(&plan, sources, task->n_sources, task->name,
                              cfg->max_samples, &c) != 0) {
        btn_free(student); free(student); circuit_free(&plan); return 0;
    }
    circuit_free(&plan);   /* finalize needs only reg, student, contract */
    accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                              cfg->max_samples, report);                          /* CERTIFIED */
    contract_free(&c);
    if (!accepted) { btn_free(student); free(student); }
    return accepted;
}
```
Confirm `src/library.c` already includes what's needed: `consolidate.h` (has `consolidate_circuit`), `router.h` (`dag_plan_circuit`, `circuit_free`, `CircuitPlan`), `contract.h` (`contract_from_circuit`, `contract_free`). `library.h` pulls all three; add any missing include.

- [ ] **Step 3: Run — circuit gate test passes**

Build the gate test + run. Expected: Branch A DEFER, Branch B DEFER (shared-node covered), Branch C
mints a 2-output chunk, Branch D gate-off distills. `DISTILLATION_GATE PASS`. If `dag_plan_circuit`
does NOT discover the dec_full_adder circuit (Branch C/D never plan), STOP and report — the spec
assumed planner discovery; do not hand-build the plan to work around it.

- [ ] **Step 4: Commit (user)**

Stage: `git add src/library.c tests/test_distillation_gate.c`
Message: `feat(library): evolve_circuit — gated multi-output distillation (flat owned[] evidence)`

---

## Task 4: Full-suite regression

**Files:** none (verification).

- [ ] **Step 1: Run the whole suite**

Run: `make test`
Expected: `DISTILLATION_GATE PASS` (now with the circuit cases) and `ALL TESTS PASSED (single exe)`,
exit 0. Pay attention that `test_library` and `test_circuit` are still green (the dispatch change is
additive — `n_goals == 0` paths untouched).

- [ ] **Step 2: Commit (user)** — only if any wiring changed in Task 4 (normally nothing to commit here).

---

## Self-Review

**Spec coverage:**
- Struct extension (`goals[]`/`n_goals`, backward-compatible) → Task 2. ✓
- 2×2 dispatch (`n_goals >= 2` → circuit) → Task 2 Step 3. ✓
- Flat-loop `circuit_evidence_clear` over `owned[]`, reusing `btn_evidence_clear` → Task 3 Step 2. ✓
- `evolve_circuit` mirror (plan → worth-it → EVIDENCE_CLEAR → consolidate_circuit → contract_from_circuit → finalize) → Task 3 Step 2. ✓
- CERTIFIED leg arity-agnostic (finalize_chunk/registry_add_certified) → Task 1 confirms. ✓
- Shared-node coverage test → Task 3 Branch B (seed dec_value, not dec_full_add → DEFER). ✓
- Gate-off / legacy untouched → Task 2 Step 4 + Task 3 Branch D + Task 4. ✓

**Placeholder scan:** Task 3 Step 1 gives the test as structured assertions + a precise fixture
recipe (exact ports, load source, the discovered topology) rather than full literal C — this is the
one place the implementer writes the concrete fixture, deliberately, because the
primitive-loading/port-setup is mechanical and fully specified by the recipe + `decimal_demo.c`
references. Every `src/library.c` change is complete code.

**Type consistency:** `evolve_circuit(reg, task, laws, n_laws, cfg, report, gs)` matches the
`evolve_route`/`evolve_dag` signature shape and the `try_consolidate` call site; `circuit_evidence_clear`
/ `count_circuit_primitives` take `const CircuitPlan *`; `LibraryTask.goals`/`n_goals`,
`btn_evidence_clear`, `GateState`, `gate_on`, `finalize_chunk` names are used identically across
tasks. ✓
