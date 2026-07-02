# Library Evolve Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `library_evolve()` — a driver that re-plans a declared task list each pass, distills every proven multi-step plan into a certified chunk, and stops at a fixed point — so the library grows by itself.

**Architecture:** One new self-contained module (`src/library.c` + `include/library.h`) that calls only existing public APIs. Per task it runs the three-step ritual the demos do by hand (`consolidate_* → contract_from_* → registry_add_certified`), wrapped in a candidate generator (the planners), a contract-keyed dedup gate, and a property-law guard that rolls back on violation. The planner/executor/consolidate cores are not modified; the only additive change elsewhere is `registry_remove_last` for rollback. In-process only (the registry starts empty each run, as all demos do today).

**Tech Stack:** C11, gcc (`-std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx`), the project Makefile, the existing `CHECK`-macro test idiom. Tests build tiny synthetic primitives inline (no frozen-weight files), matching `tests/test_certify.c` / `tests/test_consolidate.c`.

**Spec:** [docs/superpowers/specs/2026-06-13-library-evolve-design.md](docs/superpowers/specs/2026-06-13-library-evolve-design.md)

**Two intentional refinements of the spec, both to match codebase convention:**
1. The test harness is `tests/test_library.c` (a `make test` assertion harness, like `test_consolidate`/`test_certify`), not a `library_demo.c`. Demos and tests are separate in this repo; the regression gate is a `test_*`.
2. Test primitives are built inline with `btn_init`+`btn_train_dynamic` over 2–4-bit domains (the established `make test` pattern), not loaded from the committed decimal weight files. This keeps the test self-contained, fast, and deterministic.

**Before you start:** This work is unrelated to the in-progress `chunk-capacity` branch. Execute it on its own branch/worktree off `main` (the execution skill's `using-git-worktrees` step handles this).

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `include/library.h` | Public interface: `LibraryTask`, `LibraryReport`, `library_evolve`, `library_report_free` | Create (Task 1) |
| `src/library.c` | The evolve loop + per-candidate pipeline + helpers | Create (Task 1, filled Tasks 2–4) |
| `include/router.h` | Add `registry_remove_last` declaration | Modify (Task 1) |
| `src/router.c` | Add `registry_remove_last` definition | Modify (Task 1) |
| `tests/test_library.c` | Assertion harness (registry op, route/dag invention, fixed point, rollback) | Create (Task 1, grown Tasks 2–4) |
| `Makefile` | `test_library` target + `library` run target + `test`/`clean` wiring | Modify (Task 1) |

---

## Task 1: Scaffold the module, the test target, and `registry_remove_last`

**Files:**
- Create: `include/library.h`
- Create: `src/library.c`
- Create: `tests/test_library.c`
- Modify: `include/router.h` (add one declaration near `registry_free`)
- Modify: `src/router.c` (add one function)
- Modify: `Makefile`

- [ ] **Step 1: Create the public interface `include/library.h`**

```c
#ifndef LIBRARY_H
#define LIBRARY_H

#include <stddef.h>

#include "nn.h"
#include "router.h"
#include "consolidate.h"
#include "contract.h"
#include "property.h"

/* DreamCoder-style library learning: re-plan a declared task list each pass,
   distill every proven multi-step plan into a certified chunk, stop at a
   fixed point. In-process only -- the registry starts empty each run. The
   planner/executor/consolidate cores are untouched; this driver only calls
   their public APIs. */

#define LIBRARY_MAX_SOURCES BTN_MAX_INPUT_PORTS  /* 8 */
#define LIBRARY_MAX_CHUNKS  64                    /* cap on chunks per call */

typedef struct {
    const char *name;        /* the invented chunk's name (atom [A-Za-z0-9_], <=63) */
    Port sources[LIBRARY_MAX_SOURCES];
    size_t n_sources;        /* 1 -> route task; >=2 -> dag task */
    Port goal;               /* single goal in v1 */
} LibraryTask;

typedef struct {
    char names[LIBRARY_MAX_CHUNKS][CONTRACT_NAME_MAX];  /* invented, in order */
    BinaryTransformNetwork *chunks[LIBRARY_MAX_CHUNKS]; /* OWNED by the report */
    size_t chunk_count;
    size_t iterations_run;   /* passes actually executed */
    size_t rolled_back;      /* chunks rejected by the law guard */
} LibraryReport;

/* Evolve the library over `tasks`. `laws` (may be NULL when n_laws==0) are a
   post-consolidation regression guard: after a chunk registers, every law is
   re-checked and the chunk is rolled back if any law is violated. `cfg` NULL
   = consolidate_config_defaults. The loop runs at most max_iterations passes
   and stops early when a full pass invents nothing. The registry BORROWS each
   invented chunk; the report OWNS them. Call library_report_free AFTER you
   are done using reg. Returns 0. */
int library_evolve(PrimitiveRegistry *reg,
                   const LibraryTask *tasks, size_t n_tasks,
                   const Property *laws, size_t n_laws,
                   const ConsolidateConfig *cfg,
                   size_t max_iterations,
                   LibraryReport *report);

/* Free the invented chunk BTNs the report owns. Safe to call once, after the
   registry that borrowed them is no longer in use. */
void library_report_free(LibraryReport *report);

#endif
```

- [ ] **Step 2: Create `src/library.c` with compiling stubs**

```c
#include "../include/library.h"

#include <stdlib.h>
#include <string.h>

int library_evolve(PrimitiveRegistry *reg,
                   const LibraryTask *tasks, size_t n_tasks,
                   const Property *laws, size_t n_laws,
                   const ConsolidateConfig *cfg,
                   size_t max_iterations,
                   LibraryReport *report) {
    (void)reg; (void)tasks; (void)n_tasks; (void)laws; (void)n_laws;
    (void)cfg; (void)max_iterations;
    memset(report, 0, sizeof *report);
    return 0;
}

void library_report_free(LibraryReport *report) {
    size_t i;
    if (report == NULL) return;
    for (i = 0; i < report->chunk_count; ++i) {
        if (report->chunks[i] != NULL) {
            btn_free(report->chunks[i]);
            free(report->chunks[i]);
            report->chunks[i] = NULL;
        }
    }
    report->chunk_count = 0;
}
```

- [ ] **Step 3: Declare `registry_remove_last` in `include/router.h`**

Add immediately after the `registry_free` declaration (currently `include/router.h:58`):

```c
/* Drop the last registered entry (the borrowed btn and name are left to the
   caller). Used to roll back a just-appended primitive. Returns 0, or -1 if
   the registry is empty. */
int registry_remove_last(PrimitiveRegistry *reg);
```

- [ ] **Step 4: Create `tests/test_library.c` with the harness and the first test**

```c
/*
 * Tests for library_evolve: the DreamCoder-style library-learning loop.
 * Builds tiny synthetic primitives inline (the make-test idiom), so the
 * suite is self-contained, fast, and deterministic.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"
#include "../include/contract.h"
#include "../include/property.h"
#include "../include/library.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                          \
    if (cond) { printf("  ok   %s\n", (desc)); }        \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) port_set_tag(&p, tag);
    return p;
}

static void msb2(int i, double *o) { o[0] = (double)((i >> 1) & 1); o[1] = (double)(i & 1); }
static void msb3(int i, double *o) { o[0] = (double)((i >> 2) & 1); o[1] = (double)((i >> 1) & 1); o[2] = (double)(i & 1); }

static void test_registry_remove_last(void) {
    BinaryTransformNetwork a = {0}, b = {0};
    PrimitiveRegistry reg;
    printf("registry_remove_last:\n");
    if (btn_init(&a, 2, 2, 1, 8, 0.8, 1u) != 0 ||
        btn_init(&b, 2, 2, 1, 8, 0.8, 2u) != 0) {
        printf("  FAIL btn_init\n"); ++failures; return;
    }
    registry_init(&reg);
    registry_add(&reg, &a, "a");
    registry_add(&reg, &b, "b");
    CHECK(reg.count == 2, "two added");
    CHECK(registry_remove_last(&reg) == 0 && reg.count == 1, "remove -> 1");
    CHECK(registry_remove_last(&reg) == 0 && reg.count == 0, "remove -> 0");
    CHECK(registry_remove_last(&reg) == -1, "remove on empty -> -1");
    registry_free(&reg);
    btn_free(&a);
    btn_free(&b);
}

int main(void) {
    test_registry_remove_last();
    if (failures == 0) printf("\nALL LIBRARY TESTS PASS\n");
    else printf("\n%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 5: Wire the Makefile**

After the `CAPACITY_DEMO := tests/capacity_demo.c` line (currently `Makefile:39`), add:

```make
LIBRARY := src/library.c
LIBRARY_TEST := tests/test_library.c
```

In the `.PHONY` line (currently `Makefile:41`), append ` library` to the end of the list.

After the `test_consolidate:` target block (currently ends `Makefile:70`), add:

```make
# library_evolve: re-plan a task list, distill proven plans into certified
# chunks, dedup by contract, law-guard with rollback. Self-contained TDD.
test_library: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(PROPERTY) $(LIBRARY) $(LIBRARY_TEST) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract.h include/property.h include/library.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(PROPERTY) $(LIBRARY) $(LIBRARY_TEST) $(LDFLAGS)

# Builds and runs the library_evolve test directly.
library: test_library
	./test_library
```

In the `test:` target (currently `Makefile:167`), add `test_library` to the dependency list and add `./test_library` as the last run line (after `./test_circuit`):

```make
test: test_nn test_encode_oob test_contract test_router test_dag test_consolidate test_certify test_property test_decimal test_circuit test_library
	./test_nn
	./test_encode_oob
	./test_contract
	./test_router
	./test_dag
	./test_consolidate
	./test_certify
	./test_property
	./test_decimal
	./test_circuit
	./test_library
```

In the `clean:` target, add `test_library` and `test_library.exe` to the removed files (append to the existing `rm -f` lists).

- [ ] **Step 6: Run the test to verify the new harness fails on the missing function**

Run: `make test_library`
Expected: FAIL to link — `undefined reference to 'registry_remove_last'`.

- [ ] **Step 7: Implement `registry_remove_last` in `src/router.c`**

Add next to `registry_free` (search `void registry_free`):

```c
int registry_remove_last(PrimitiveRegistry *reg) {
    if (reg == NULL || reg->count == 0) return -1;
    /* The btn and name are borrowed; nothing to free. The slot is left in
       place and reused on the next append. */
    reg->count--;
    return 0;
}
```

- [ ] **Step 8: Run the test to verify it passes**

Run: `make library`
Expected: PASS — four `ok` lines under `registry_remove_last:` and `ALL LIBRARY TESTS PASS`.

- [ ] **Step 9: Commit**

```bash
git add include/library.h src/library.c tests/test_library.c include/router.h src/router.c Makefile
git commit -m "feat: library_evolve scaffold + registry_remove_last"
```

---

## Task 2: Route invention and the fixed point

Implements the loop and the route branch of the per-candidate pipeline (plan → worth-it guard → distill → contract → dedup → register → record), **without** the law guard (Task 4). Drives it with a forced two-hop route whose optimal plan only the chain produces.

**Files:**
- Modify: `src/library.c` (replace the `library_evolve` stub; add static helpers)
- Modify: `tests/test_library.c` (add base-primitive builders + the test)

- [ ] **Step 1: Add the route-domain primitive builders and the invention test to `tests/test_library.c`**

Add these `make_*` helpers above `main` (they mirror `tests/test_certify.c`):

```c
/* dec: ONEHOT4 -> BINARY_MSB2, i -> i. */
static int make_route_dec(BinaryTransformNetwork *b) {
    double in[4][4] = {{0}}; double tg[4][2]; int i;
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 4, 1, NULL),
                      PT(PORT_BINARY_MSB, 2, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { in[i][i] = 1.0; msb2(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* inc: BINARY_MSB2 -> BINARY_MSB3, i -> i+1 (widens, so the goal port differs
   from the intermediate and only the two-hop chain reaches it). */
static int make_route_inc(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][3]; int i;
    if (btn_init(b, 2, 3, 1, 16, 0.8, 13u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, NULL),
                      PT(PORT_BINARY_MSB, 3, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb3(i + 1, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_route_invention(void) {
    BinaryTransformNetwork dec = {0}, inc = {0};
    PrimitiveRegistry reg;
    LibraryTask task;
    LibraryReport rep1, rep2;
    printf("route invention + fixed point:\n");
    CHECK(make_route_dec(&dec) == 0 && make_route_inc(&inc) == 0,
          "base primitives train");
    registry_init(&reg);
    registry_add(&reg, &dec, "dec");
    registry_add(&reg, &inc, "inc");

    memset(&task, 0, sizeof task);
    task.name = "chunk_inc4";
    task.sources[0] = PT(PORT_ONEHOT, 4, 1, NULL);
    task.n_sources = 1;
    task.goal = PT(PORT_BINARY_MSB, 3, 1, NULL);

    CHECK(library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &rep1) == 0, "evolve runs");
    CHECK(rep1.chunk_count == 1, "one chunk invented");
    CHECK(rep1.rolled_back == 0, "no rollback");
    CHECK(strcmp(rep1.names[0], "chunk_inc4") == 0, "chunk named");
    CHECK(rep1.iterations_run == 2, "converged in 2 passes");
    CHECK(reg.count == 3, "registry grew to 3");

    {   /* the chunk computes index+1 */
        RoutePlan p; double in[4] = {0}; double out[3] = {0}; int got;
        CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, NULL),
                         PT(PORT_BINARY_MSB, 3, 1, NULL), &p) == 0 && p.length == 1,
              "replans to the chunk");
        in[2] = 1.0;
        CHECK(route_execute(&p, in, 4, out, 3) == 0, "chunk executes");
        got = (out[0] > 0.5 ? 4 : 0) + (out[1] > 0.5 ? 2 : 0) + (out[2] > 0.5 ? 1 : 0);
        CHECK(got == 3, "one-hot 2 -> 3");
    }

    CHECK(library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &rep2) == 0, "second evolve runs");
    CHECK(rep2.chunk_count == 0, "no re-invention");
    CHECK(rep2.iterations_run == 1, "fixed point in one pass");

    registry_free(&reg);
    library_report_free(&rep1);
    library_report_free(&rep2);
    btn_free(&dec);
    btn_free(&inc);
}
```

Add `test_route_invention();` to `main`, after `test_registry_remove_last();`.

- [ ] **Step 2: Run to verify it fails against the stub**

Run: `make library`
Expected: FAIL — `one chunk invented` fails (`chunk_count == 0` from the stub), among others.

- [ ] **Step 3: Replace the `library_evolve` stub in `src/library.c` with the route pipeline**

Replace the entire stub `library_evolve` function (keep `library_report_free`) with the helpers and the real driver below:

```c
/* Have we already invented this exact behavior? Keyed on the contract: an
   existing entry whose ports match AND that replays every exemplar exactly. */
static int contract_already_known(const PrimitiveRegistry *reg, const Contract *c) {
    size_t i;
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].btn != NULL &&
            btn_certify(reg->entries[i].btn, c, NULL) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Steps 5-7 of the pipeline, shared by route and dag. Does NOT free student in
   any case -- the caller frees it when this returns 0 (not accepted). The law
   guard is added in Task 4; for now laws are ignored. Returns 1 if the chunk
   was registered and recorded, 0 otherwise. */
static int finalize_chunk(PrimitiveRegistry *reg, BinaryTransformNetwork *student,
                          const char *name, const Contract *c,
                          const Property *laws, size_t n_laws, size_t max_samples,
                          LibraryReport *report) {
    size_t idx;
    (void)laws; (void)n_laws; (void)max_samples;  /* law guard: Task 4 */

    if (report->chunk_count >= LIBRARY_MAX_CHUNKS) return 0;
    if (contract_already_known(reg, c)) return 0;          /* step 5: dedup */
    if (registry_add_certified(reg, student, name, c) != 0) return 0;  /* step 6 */

    idx = report->chunk_count;                              /* step: record */
    snprintf(report->names[idx], sizeof report->names[idx], "%s", name);
    report->chunks[idx] = student;
    report->chunk_count++;
    return 1;
}

/* Route task (single source): plan -> worth-it -> distill -> contract -> finalize. */
static int evolve_route(PrimitiveRegistry *reg, const LibraryTask *task,
                        const Property *laws, size_t n_laws,
                        const ConsolidateConfig *cfg, LibraryReport *report) {
    RoutePlan plan;
    BinaryTransformNetwork *student;
    ConsolidateReport crep;
    Contract c = {0};
    int accepted;

    if (route_plan(reg, task->sources[0], task->goal, &plan) != 0) return 0;
    if (plan.length < 2) return 0;                         /* worth-it guard */

    student = calloc(1, sizeof *student);
    if (student == NULL) return 0;
    if (consolidate_route(&plan, cfg, student, &crep) != 0) {
        free(student);                                     /* refusal: arrays never allocated */
        return 0;
    }
    if (contract_from_route(&plan, task->name, cfg->max_samples, &c) != 0) {
        btn_free(student); free(student);
        return 0;
    }
    accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                              cfg->max_samples, report);
    contract_free(&c);
    if (!accepted) { btn_free(student); free(student); }
    return accepted;
}

/* Dispatch by arity. Dag is wired in Task 3. */
static int try_consolidate(PrimitiveRegistry *reg, const LibraryTask *task,
                           const Property *laws, size_t n_laws,
                           const ConsolidateConfig *cfg, LibraryReport *report) {
    if (task->n_sources == 1) {
        return evolve_route(reg, task, laws, n_laws, cfg, report);
    }
    return 0;  /* dag: Task 3 */
}

int library_evolve(PrimitiveRegistry *reg,
                   const LibraryTask *tasks, size_t n_tasks,
                   const Property *laws, size_t n_laws,
                   const ConsolidateConfig *cfg,
                   size_t max_iterations,
                   LibraryReport *report) {
    ConsolidateConfig cfg_local;
    size_t iter, t;

    memset(report, 0, sizeof *report);
    if (cfg != NULL) cfg_local = *cfg;
    else consolidate_config_defaults(&cfg_local);

    for (iter = 0; iter < max_iterations; ++iter) {
        size_t added = 0;
        report->iterations_run = iter + 1;
        for (t = 0; t < n_tasks; ++t) {
            added += (size_t)try_consolidate(reg, &tasks[t], laws, n_laws,
                                             &cfg_local, report);
        }
        if (added == 0) break;   /* fixed point */
    }
    return 0;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make library`
Expected: PASS — all `route invention + fixed point:` checks `ok`, plus the Task-1 checks.

- [ ] **Step 5: Commit**

```bash
git add src/library.c tests/test_library.c
git commit -m "feat: library_evolve route invention + fixed-point loop"
```

---

## Task 3: DAG invention

Adds the multi-input branch. Driven by a two-source DAG (`comb(dec2(s0), dec2(s1))`) whose plan has ≥2 primitive executions.

**Files:**
- Modify: `src/library.c` (add `count_dag_primitives`, `evolve_dag`; update `try_consolidate`)
- Modify: `tests/test_library.c` (add dag-domain builders + the test)

- [ ] **Step 1: Add the dag-domain primitive builders and the test to `tests/test_library.c`**

Add these helpers above `main` (copied from `tests/test_certify.c`, tagged exactly as there):

```c
/* dec2: ONEHOT2 "bsym" -> BINARY_MSB1 "bit", i -> i. */
static int make_decoder2(BinaryTransformNetwork *b) {
    double in[2][2] = {{1.0, 0.0}, {0.0, 1.0}};
    double tg[2][1] = {{0.0}, {1.0}};
    if (btn_init(b, 2, 1, 1, 8, 0.8, 17u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 2, 1, "bsym"),
                      PT(PORT_BINARY_MSB, 1, 1, "bit")) != 0) return -1;
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 2,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* comb: [BINARY_MSB1 "bit", BINARY_MSB1 "bit"] -> BINARY_MSB2 "pair",
   (hi, lo) -> hi*2 + lo. */
static int make_combiner(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][2]; Port in_ports[2]; int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 19u) != 0) return -1;
    in_ports[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    in_ports[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    if (btn_set_input_ports(b, in_ports, 2,
                            PT(PORT_BINARY_MSB, 2, 1, "pair")) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb2(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_dag_invention(void) {
    BinaryTransformNetwork d2 = {0}, comb = {0};
    PrimitiveRegistry reg;
    LibraryTask task;
    LibraryReport rep;
    printf("dag invention:\n");
    CHECK(make_decoder2(&d2) == 0 && make_combiner(&comb) == 0,
          "base primitives train");
    registry_init(&reg);
    registry_add(&reg, &d2, "dec2");
    registry_add(&reg, &comb, "comb");

    memset(&task, 0, sizeof task);
    task.name = "chunk_pair";
    task.sources[0] = PT(PORT_ONEHOT, 2, 1, "bsym");
    task.sources[1] = PT(PORT_ONEHOT, 2, 1, "bsym");
    task.n_sources = 2;
    task.goal = PT(PORT_BINARY_MSB, 2, 1, "pair");

    CHECK(library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &rep) == 0, "evolve runs");
    CHECK(rep.chunk_count == 1, "one dag chunk invented");
    CHECK(strcmp(rep.names[0], "chunk_pair") == 0, "chunk named");
    CHECK(reg.count == 3, "registry grew to 3");

    registry_free(&reg);
    library_report_free(&rep);
    btn_free(&d2);
    btn_free(&comb);
}
```

Add `test_dag_invention();` to `main`, after `test_route_invention();`.

- [ ] **Step 2: Run to verify it fails (dag dispatch returns 0)**

Run: `make library`
Expected: FAIL — `one dag chunk invented` fails (`chunk_count == 0`; `try_consolidate` returns 0 for `n_sources >= 2`).

- [ ] **Step 3: Add `count_dag_primitives` and `evolve_dag`, and update `try_consolidate` in `src/library.c`**

Add these two functions above `try_consolidate`:

```c
/* Distinct primitive executions in a planner-built plan (sharing-aware via
   the owned table). The worth-it guard for dag tasks. */
static int count_dag_primitives(const DagPlan *p) {
    size_t i; int n = 0;
    if (p->owned != NULL) {
        for (i = 0; i < p->owned_count; ++i) {
            if (p->owned[i] != NULL && p->owned[i]->kind == DAG_PRIMITIVE) ++n;
        }
    } else if (p->root != NULL && p->root->kind == DAG_PRIMITIVE) {
        n = 1;  /* hand-built fallback; planner plans always set owned */
    }
    return n;
}

/* Dag task (>=2 sources): plan -> worth-it -> distill -> contract -> finalize.
   Source values are never read by dag_plan/consolidate_dag/contract_from_dag
   (they enumerate the canonical domain), so .values = NULL is correct. */
static int evolve_dag(PrimitiveRegistry *reg, const LibraryTask *task,
                      const Property *laws, size_t n_laws,
                      const ConsolidateConfig *cfg, LibraryReport *report) {
    DagSource sources[LIBRARY_MAX_SOURCES];
    DagPlan plan = {0};
    BinaryTransformNetwork *student;
    ConsolidateReport crep;
    Contract c = {0};
    size_t i;
    int accepted;

    for (i = 0; i < task->n_sources; ++i) {
        sources[i].type = task->sources[i];
        sources[i].values = NULL;
    }
    if (dag_plan(reg, sources, task->n_sources, task->goal, &plan) != 0) return 0;
    if (count_dag_primitives(&plan) < 2) { dag_free(&plan); return 0; }

    student = calloc(1, sizeof *student);
    if (student == NULL) { dag_free(&plan); return 0; }
    if (consolidate_dag(&plan, sources, task->n_sources, cfg, student, &crep) != 0) {
        free(student); dag_free(&plan);
        return 0;
    }
    if (contract_from_dag(&plan, sources, task->n_sources, task->name,
                          cfg->max_samples, &c) != 0) {
        btn_free(student); free(student); dag_free(&plan);
        return 0;
    }
    dag_free(&plan);  /* finalize needs only reg, student, contract */
    accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                              cfg->max_samples, report);
    contract_free(&c);
    if (!accepted) { btn_free(student); free(student); }
    return accepted;
}
```

Replace the `dag: Task 3` line in `try_consolidate`:

```c
static int try_consolidate(PrimitiveRegistry *reg, const LibraryTask *task,
                           const Property *laws, size_t n_laws,
                           const ConsolidateConfig *cfg, LibraryReport *report) {
    if (task->n_sources == 1) {
        return evolve_route(reg, task, laws, n_laws, cfg, report);
    }
    if (task->n_sources >= 2) {
        return evolve_dag(reg, task, laws, n_laws, cfg, report);
    }
    return 0;  /* n_sources == 0: nothing to do */
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make library`
Expected: PASS — all `dag invention:` checks `ok`, plus Tasks 1–2.

- [ ] **Step 5: Commit**

```bash
git add src/library.c tests/test_library.c
git commit -m "feat: library_evolve dag invention"
```

---

## Task 4: The property-law guard and rollback

Adds step 7. A supplied law that the registry violates must roll the just-added chunk back out, leaving the registry exactly as it was.

**Files:**
- Modify: `src/library.c` (add `law_violated`; extend `finalize_chunk`)
- Modify: `tests/test_library.c` (add same-signature primitives, a false law, the test)

- [ ] **Step 1: Add the rollback test (with a deliberately-false law) to `tests/test_library.c`**

Add these two builders (same signature, different behavior — so a law equating them is violated) above `main`:

```c
/* idp: BINARY_MSB2 -> BINARY_MSB2, identity. */
static int make_idp(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][2]; int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 21u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, NULL),
                      PT(PORT_BINARY_MSB, 2, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb2(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* flp: BINARY_MSB2 -> BINARY_MSB2, i -> (i+1)%4. */
static int make_flp(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][2]; int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 23u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, NULL),
                      PT(PORT_BINARY_MSB, 2, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb2((i + 1) % 4, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_law_guard_rollback(void) {
    BinaryTransformNetwork dec = {0}, inc = {0}, idp = {0}, flp = {0};
    PrimitiveRegistry reg;
    LibraryTask task;
    LibraryReport rep;
    Property law;
    PropertyReport pr;
    size_t i; int present = 0;
    printf("law-guard rollback:\n");
    CHECK(make_route_dec(&dec) == 0 && make_route_inc(&inc) == 0 &&
          make_idp(&idp) == 0 && make_flp(&flp) == 0, "primitives train");
    registry_init(&reg);
    registry_add(&reg, &dec, "dec");
    registry_add(&reg, &inc, "inc");
    registry_add(&reg, &idp, "idp");
    registry_add(&reg, &flp, "flp");

    /* A law the registry does NOT satisfy: idp == flp over BINARY_MSB2. */
    memset(&law, 0, sizeof law);
    snprintf(law.name, sizeof law.name, "%s", "idp_eq_flp");
    law.sources[0] = PT(PORT_BINARY_MSB, 2, 1, NULL);
    law.source_count = 1;
    snprintf(law.lhs[0], sizeof law.lhs[0], "%s", "idp");
    law.lhs_len = 1;
    snprintf(law.rhs[0], sizeof law.rhs[0], "%s", "flp");
    law.rhs_len = 1;
    CHECK(property_check(&law, &reg, 4096, &pr) != 0 && pr.violated > 0,
          "false law is violated as authored");

    memset(&task, 0, sizeof task);
    task.name = "chunk_inc4";
    task.sources[0] = PT(PORT_ONEHOT, 4, 1, NULL);
    task.n_sources = 1;
    task.goal = PT(PORT_BINARY_MSB, 3, 1, NULL);

    CHECK(library_evolve(&reg, &task, 1, &law, 1, NULL, 4, &rep) == 0, "evolve runs");
    CHECK(rep.chunk_count == 0, "chunk rolled back, none kept");
    CHECK(rep.rolled_back == 1, "one rollback recorded");
    CHECK(reg.count == 4, "registry restored to base four");
    for (i = 0; i < reg.count; ++i) {
        if (strcmp(reg.entries[i].name, "chunk_inc4") == 0) present = 1;
    }
    CHECK(!present, "chunk absent from registry");

    registry_free(&reg);
    library_report_free(&rep);
    btn_free(&dec); btn_free(&inc); btn_free(&idp); btn_free(&flp);
}
```

Add `test_law_guard_rollback();` to `main`, after `test_dag_invention();`.

- [ ] **Step 2: Run to verify it fails (no guard yet — the chunk is wrongly kept)**

Run: `make library`
Expected: FAIL — `chunk rolled back, none kept` fails (`chunk_count == 1`), and `one rollback recorded` fails (`rolled_back == 0`).

- [ ] **Step 3: Add `law_violated` and extend `finalize_chunk` in `src/library.c`**

Add above `finalize_chunk`:

```c
/* Does any supplied law have a real violation against the current registry?
   property_check returns 0 iff the law holds. A nonzero return with
   report.violated > 0 is a regression; a nonzero return with report.inputs == 0
   means the law could not be evaluated (unresolved name / RAW / over-cap) --
   a warning, not a violation, so it does not trigger rollback. */
static int law_violated(const Property *laws, size_t n_laws,
                        const PrimitiveRegistry *reg, size_t max_samples) {
    size_t i;
    for (i = 0; i < n_laws; ++i) {
        PropertyReport pr;
        if (property_check(&laws[i], reg, max_samples, &pr) != 0 && pr.violated > 0) {
            return 1;
        }
    }
    return 0;
}
```

Replace `finalize_chunk` in full (drops the `(void)` casts, adds step 7 between register and record):

```c
static int finalize_chunk(PrimitiveRegistry *reg, BinaryTransformNetwork *student,
                          const char *name, const Contract *c,
                          const Property *laws, size_t n_laws, size_t max_samples,
                          LibraryReport *report) {
    size_t before, idx;

    if (report->chunk_count >= LIBRARY_MAX_CHUNKS) return 0;
    if (contract_already_known(reg, c)) return 0;          /* step 5: dedup */

    before = reg->count;
    if (registry_add_certified(reg, student, name, c) != 0) return 0;  /* step 6 */

    if (law_violated(laws, n_laws, reg, max_samples)) {    /* step 7: guard */
        if (reg->count == before + 1) registry_remove_last(reg);  /* rollback the append */
        report->rolled_back++;
        return 0;
    }

    idx = report->chunk_count;                             /* record */
    snprintf(report->names[idx], sizeof report->names[idx], "%s", name);
    report->chunks[idx] = student;
    report->chunk_count++;
    return 1;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make library`
Expected: PASS — all `law-guard rollback:` checks `ok`, plus Tasks 1–3. `ALL LIBRARY TESTS PASS`.

- [ ] **Step 5: Commit**

```bash
git add src/library.c tests/test_library.c
git commit -m "feat: library_evolve property-law guard with rollback"
```

---

## Task 5: Full regression and warning check

**Files:** none changed; verification only.

- [ ] **Step 1: Build the new test with no warnings**

Run: `make test_library`
Expected: compiles clean — no warnings under `-Wall -Wextra -pedantic`.

- [ ] **Step 2: Run the entire frozen suite plus the new test**

Run: `make test`
Expected: every harness prints its `ok` lines and the run ends without a non-zero exit; `test_library` prints `ALL LIBRARY TESTS PASS`. (`make test` rebuilds and runs `test_nn … test_circuit test_library` in order.)

- [ ] **Step 3: Confirm the core was not touched**

Run: `git diff --stat main -- src/router.c src/nn.c src/consolidate.c src/contract.c src/property.c src/plan_table.c`
Expected: only `src/router.c` appears, and only with the small `registry_remove_last` addition. No other core file changed.

- [ ] **Step 4: Commit (if Step 1–3 produced any fixups; otherwise skip)**

```bash
git add -A
git commit -m "test: library_evolve passes the full frozen suite"
```

---

## Self-Review

**Spec coverage:**
- §A module & core untouched → Task 1 (new module), Task 5 Step 3 (verified core untouched). ✓
- §B inputs + arity dispatch → Task 1 (`LibraryTask`), Task 2 (route), Task 3 (dag). ✓
- §C loop (cap + fixed point) → Task 2 `library_evolve`; fixed point asserted in `test_route_invention`. ✓
- §D pipeline steps 1–7 → Task 2 (1–6: plan/worth-it/distill/contract/dedup/register/record), Task 4 (7: law guard). ✓
- §E ownership, rollback, `registry_remove_last` → Task 1 (the function + unit test), Task 2 (student ownership), Task 4 (rollback path). ✓
- §F reporting → Task 1 (`LibraryReport`), populated Tasks 2/4; fields asserted in tests. ✓
- §G testing → `tests/test_library.c` across Tasks 1–4 (deviation from the spec's `library_demo.c`/decimal-weights is documented in the header and is the codebase's own `make test` idiom). ✓
- §H out of scope → not implemented; no task adds persistence, circuits, plan-log mining, or the other CSV ideas. ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code; every run step shows the exact command and expected pass/fail.

**Type consistency:** `finalize_chunk`'s signature is fixed in Task 2 and only its body changes in Task 4 (laws/n_laws/max_samples are real parameters from the start, merely unused until Task 4). `try_consolidate`, `evolve_route`, `evolve_dag` share one signature shape. `LibraryReport.names` is `char[][CONTRACT_NAME_MAX]` and task names are ≤63, consistent with `snprintf` bounds. `DagSource.values = NULL` is justified against the headers' "values are not read" guarantee.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-13-library-evolve.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**
