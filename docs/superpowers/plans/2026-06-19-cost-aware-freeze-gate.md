# Cost-Aware Freeze Gate (3A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use `- [ ]` checkboxes.

**Goal:** Every minted chunk reports its teacher-vs-student MAC truth — add `teacher_mac_estimate`/`student_mac_estimate`/`compression_ratio`/`compute_beneficial` per chunk to `LibraryReport`, computed in the gate, surfaced in the compounding benchmark. Advisory only; decisions unchanged.

**Architecture:** Each `evolve_route`/`evolve_dag`/`evolve_circuit` sums `btn_cost` over its plan's primitives (`teacher_mac`) and passes it to `finalize_chunk` (+1 param); `finalize_chunk` computes `student_mac = btn_cost(chunk)` and writes all four fields at the chunk's report index. `compression_ratio = teacher/student` (exact `CircuitConsolidationReport` orientation, `btn_mac_estimate == btn_cost`).

**Tech Stack:** C11, MinGW gcc (`-mno-avx`), Makefile. `btn_cost` (router.h:200), `INFINITY` (`<math.h>`).

> **Commits user-managed.** "Commit (user)" steps list files to stage; the user commits.

**Spec:** `docs/superpowers/specs/2026-06-19-cost-aware-freeze-gate-design.md`

---

## File Structure
- `include/library.h` — add 4 parallel arrays to `LibraryReport`.
- `src/library.c` — `<math.h>`; `route_teacher_mac` + `owned_teacher_mac` helpers; `finalize_chunk` +1 param + label write; 3 call-site changes.
- `tests/test_distillation_gate.c` — formula asserts on the circuit + route mints.
- `tests/test_library.c` — formula assert on a dag mint.
- `tests/benchmark_compounding_loop.c` — observational label print.

Build commands:
```bash
# gate tests:
gcc -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx -Wno-unused-function -o test_distillation_gate.exe \
  src/nn.c src/router.c src/plan_table.c src/contract.c src/property.c src/consolidate.c src/scan.c src/library.c \
  tests/test_distillation_gate.c -lm
# full suite / benchmark:
make test ; make compounding_bench
```

---

## Task 1: `LibraryReport` fields + `finalize_chunk` +1 param + teacher-cost helpers + call sites + circuit test

**Files:** `include/library.h`, `src/library.c`, `tests/test_distillation_gate.c`

- [ ] **Step 1: Write the failing test (circuit path)**

In `tests/test_distillation_gate.c`, find `test_circuit_evidence_gate`'s Branch C (the `(16,0)`-seeded mint that asserts `chunk_count>=1` and 3-in/2-out). After that mint's `report`, add (replace `rep` with the actual report variable name in that test):
```c
    /* 3A cost label (circuit path), at the minted chunk's index 0 */
    {
        size_t exp_teacher = 2 * btn_cost(&f.dec_value) + btn_cost(&f.dec_full_add); /* the column's 3 prims */
        CHECK(rep.teacher_mac_estimate[0] == exp_teacher,
              "circuit: teacher_mac == sum btn_cost over column primitives");
        CHECK(rep.student_mac_estimate[0] == btn_cost(rep.chunks[0]),
              "circuit: student_mac == btn_cost(chunk)");
        CHECK(rep.compute_beneficial[0] == (rep.student_mac_estimate[0] < rep.teacher_mac_estimate[0]),
              "circuit: compute_beneficial == (student < teacher) [self-consistent]");
        if (rep.student_mac_estimate[0] > 0)
            CHECK(rep.compression_ratio[0] > 0.0 &&
                  rep.compression_ratio[0] ==
                    (double)rep.teacher_mac_estimate[0] / (double)rep.student_mac_estimate[0],
                  "circuit: compression_ratio == teacher/student");
    }
```
(Use the test's real names for the registry-built `dec_value`/`dec_full_add` and the report; if the fixture stores them differently, compute `exp_teacher` from those handles.) Build → FAILS to compile (`no member named 'teacher_mac_estimate'`).

- [ ] **Step 2: Add the report fields**

In `include/library.h`, inside `LibraryReport`, after `size_t deferred;`:
```c
    /* 3A cost-aware label, per minted chunk (same index as names[]/chunks[]).
       compression_ratio = teacher/student (CircuitConsolidationReport orientation:
       >1 = chunk smaller than the sub-plan = beneficial). Advisory; no decision
       reads these. */
    size_t teacher_mac_estimate[LIBRARY_MAX_CHUNKS];
    size_t student_mac_estimate[LIBRARY_MAX_CHUNKS];
    double compression_ratio[LIBRARY_MAX_CHUNKS];
    int    compute_beneficial[LIBRARY_MAX_CHUNKS];
```

- [ ] **Step 3: `finalize_chunk` +1 param + label write**

In `src/library.c`: add `#include <math.h>` near the top includes. Change `finalize_chunk`'s signature to add `size_t teacher_mac` (put it right before `LibraryReport *report`), and write the label where it records the chunk:
```c
static int finalize_chunk(PrimitiveRegistry *reg, BinaryTransformNetwork *student,
                          const char *name, const Contract *c,
                          const Property *laws, size_t n_laws, size_t max_samples,
                          size_t teacher_mac, LibraryReport *report) {
    size_t before, idx, student_mac;
    /* ... unchanged through the law-guard ... */
    idx = report->chunk_count;                             /* record */
    snprintf(report->names[idx], sizeof report->names[idx], "%s", name);
    report->chunks[idx] = student;
    /* 3A cost label (decision-identical: advisory, nothing reads it) */
    student_mac = btn_cost(student);
    report->teacher_mac_estimate[idx] = teacher_mac;
    report->student_mac_estimate[idx] = student_mac;
    if (student_mac > 0)      report->compression_ratio[idx] = (double)teacher_mac / (double)student_mac;
    else if (teacher_mac > 0) report->compression_ratio[idx] = INFINITY;  /* free student: maximally beneficial */
    else                      report->compression_ratio[idx] = 1.0;       /* both zero: neutral */
    report->compute_beneficial[idx] = (student_mac < teacher_mac) ? 1 : 0;
    report->chunk_count++;
    return 1;
}
```
(Keep the early-return branches `if (report->chunk_count >= LIBRARY_MAX_CHUNKS) return 0;`, dedup, `registry_add_certified`, and the law-guard rollback exactly as they are — only the record block grows.)

- [ ] **Step 4: teacher-cost helpers**

Add above `evolve_route` (`route` over `steps[]`; `dag`/`circuit` share the `owned[]` walk):
```c
static size_t route_teacher_mac(const RoutePlan *p) {
    size_t i, sum = 0;
    for (i = 0; i < p->length; ++i) sum += btn_cost(p->steps[i]);
    return sum;
}
static size_t owned_teacher_mac(DagNode *const *owned, size_t owned_count) {
    size_t i, sum = 0;
    if (owned == NULL) return 0;
    for (i = 0; i < owned_count; ++i)
        if (owned[i] != NULL && owned[i]->kind == DAG_PRIMITIVE) sum += btn_cost(owned[i]->btn);
    return sum;
}
```

- [ ] **Step 5: wire the 3 call sites**

`evolve_route` — the `RoutePlan` is alive at `finalize`; pass `route_teacher_mac(&plan)`:
```c
    accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                              cfg->max_samples, route_teacher_mac(&plan), report);
```
`evolve_dag` — compute BEFORE the existing `dag_free(&plan);` (line ~189), then pass it:
```c
    size_t teacher_mac = owned_teacher_mac(plan.owned, plan.owned_count);
    dag_free(&plan);  /* finalize needs only reg, student, contract */
    accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                              cfg->max_samples, teacher_mac, report);
```
`evolve_circuit` — same pattern: compute `owned_teacher_mac(plan.owned, plan.owned_count)` BEFORE the `circuit_free(&plan);` call, then pass it to `finalize_chunk`.

- [ ] **Step 6: build + verify the circuit test passes**

Build the gate test → run. Expected: the four new circuit `ok` lines + `DISTILLATION_GATE PASS`. (For our chunk, `compute_beneficial` resolves to 0 — heavier — and the self-consistent assert still holds.)

- [ ] **Step 7: Commit (user)**

Stage: `git add include/library.h src/library.c tests/test_distillation_gate.c`
Message: `feat(library): per-chunk MAC cost label in LibraryReport (3A, finalize +teacher_mac)`

---

## Task 2: Route + DAG path coverage (formula, fresh registries)

**Files:** `tests/test_distillation_gate.c`, `tests/test_library.c`

- [ ] **Step 1: Route path assert**

In `test_distillation_gate.c`'s `test_evidence_gate` (Branch B mints `chunk_inc4` via the **route** path on a FRESH registry), after the mint that yields `chunk_count>=1`, add the same formula block at the route chunk's index `i` (use that test's report var + the registry's two route primitives for `exp_teacher = sum btn_cost over the route's steps`):
```c
    CHECK(rep.teacher_mac_estimate[i] > 0, "route: teacher_mac populated");
    CHECK(rep.student_mac_estimate[i] == btn_cost(rep.chunks[i]), "route: student_mac == btn_cost(chunk)");
    CHECK(rep.compute_beneficial[i] == (rep.student_mac_estimate[i] < rep.teacher_mac_estimate[i]),
          "route: compute_beneficial self-consistent");
```
(`teacher_mac_estimate[i] > 0` plus `student==btn_cost(chunk)` proves the route call site wired `route_teacher_mac` and finalize wrote the student — the steps[]-vs-owned[] code path distinct from the circuit one.)

- [ ] **Step 2: DAG path assert (fresh registry)**

In `tests/test_library.c`, find the existing **dag** mint (a `LibraryTask` with `n_sources >= 2`, `n_goals == 0` that `library_evolve` distills — the ungated wrapper; the cost fields populate regardless of gate). On its `LibraryReport`, at the dag chunk's index, add:
```c
    CHECK(report.teacher_mac_estimate[idx] > 0, "dag: teacher_mac populated");
    CHECK(report.student_mac_estimate[idx] == btn_cost(report.chunks[idx]), "dag: student_mac == btn_cost(chunk)");
    CHECK(report.compute_beneficial[idx] == (report.student_mac_estimate[idx] < report.teacher_mac_estimate[idx]),
          "dag: compute_beneficial self-consistent");
```
If no dag mint exists in `test_library.c`, the cheapest concrete fixture is a 2-source DAG (e.g. `combine` over two `hex_value` outputs) mirroring the existing route fixture; build it in a fresh registry. Each mint in its OWN fresh registry (don't reuse one where a prior chunk is registered — it could dedup or be picked as teacher).

- [ ] **Step 3: build + verify both**

Build the gate test + run; build `test_library` via `make test` (it is TEST_ALL-only). Expected: route asserts pass; `ALL LIBRARY TESTS PASS`.

- [ ] **Step 4: Commit (user)**

Stage: `git add tests/test_distillation_gate.c tests/test_library.c`
Message: `test(library): cost-label formula coverage across route + dag + circuit paths`

---

## Task 3: Benchmark print (observational) + full regression

**Files:** `tests/benchmark_compounding_loop.c`

- [ ] **Step 1: Print the label after the mint**

In `tests/benchmark_compounding_loop.c`, in the mint block (right after it prints `Tier-2 mint: ... chunk ports: ...`, while `report` is in scope and `minted` is true), add an **observational** print — NO asserts:
```c
        printf("minted chunk: %s\n", report.names[0]);
        printf("  teacher_macs=%zu student_macs=%zu compression_ratio=%.3f compute_beneficial=%s\n",
               report.teacher_mac_estimate[0], report.student_mac_estimate[0],
               report.compression_ratio[0], report.compute_beneficial[0] ? "true" : "false");
```
Do NOT assert `compute_beneficial == 0`, the ratio, or the student cost — a future capacity win must keep this passing. The existing Run A/Run B asserts stay exactly as they are.

- [ ] **Step 2: build + run the benchmark**

Run `make compounding_bench`. Expected: it prints the new `minted chunk: dec_full_adder_unit` + `teacher_macs=... student_macs=... compression_ratio=... compute_beneficial=false` line, the Run A/B table, and `COMPOUNDING_BENCH PASS` (unchanged asserts).

- [ ] **Step 3: full suite regression**

Run `make test`. Expected: `ALL TESTS PASSED (single exe)` (the new fields/asserts are additive; route/dag/circuit/decimal callers unaffected).

- [ ] **Step 4: Commit (user)**

Stage: `git add tests/benchmark_compounding_loop.c`
Message: `bench(compounding): print per-chunk cost label (observational)`

---

## Self-Review

**Spec coverage:**
- 4 `LibraryReport` fields, index-associated → Task 1 Step 2. ✓
- `finalize_chunk` +1 param (`teacher_mac`), centralized student-cost + label write → Task 1 Step 3. ✓
- `compression_ratio = teacher/student`; truthful zero-case (`INFINITY`/`1.0`) → Task 1 Step 3. ✓
- `compute_beneficial` from integer compare → Task 1 Step 3. ✓
- teacher-cost per path (`route_teacher_mac` over `steps[]`; `owned_teacher_mac` over `owned[]`) → Task 1 Steps 4-5. ✓
- 3-path test matrix (circuit gated, route, dag), fresh registries → Tasks 1-2. ✓
- Benchmark print, observational, no new asserts → Task 3. ✓
- Decision-identical (no planner/executor/registry change) → no task touches them; only `LibraryReport` + gate-internal. ✓
- No `planning_beneficial`, no `execution_cost_ratio`, `RegistryEntry` untouched → not added anywhere. ✓

**Placeholder scan:** the dag fixture in Task 2 Step 2 says "find the existing dag mint, else build a 2-source `combine` fixture" — the formula-check CODE is complete; the only implementer judgment is *which* existing fixture is cheapest, with a concrete fallback given. Everything else is full code.

**Type consistency:** `finalize_chunk(..., size_t teacher_mac, LibraryReport *report)` — all 3 call sites updated (Task 1 Step 5). `route_teacher_mac(const RoutePlan*)`, `owned_teacher_mac(DagNode *const *, size_t)`, the four `LibraryReport` array fields, `btn_cost`, `INFINITY` — used identically across tasks. ✓
