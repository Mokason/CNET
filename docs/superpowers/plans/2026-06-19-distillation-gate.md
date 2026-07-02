# Distillation Gate Implementation Plan

> **IMPLEMENTATION NOTE (2026-06-19):** Built through Task 5 + Tasks 7–8. **Task 6 (structural pre-distill dedup) was dropped as redundant** — `library_evolve`'s worth-it guard + chunk-collapse already deduplicate same-goal plans pre-distill, so `structurally_deduped` could never fire. Shipped as `EVIDENCE_CLEAR`-only; the `scan.c` digest infrastructure (Tasks 1–3) still ships. Task 8 was repurposed to benchmark gate overhead + the measured sweep cost (~0.065 ms/call). Dedup-related steps below are superseded. See `memory/distillation-gate-built.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in admission gate to `library_evolve` — a proven plan is distilled and frozen only when its primitives are evidence-trusted (`0.9/16`) and the goal-function is not already minted, using a structural canonical digest to deduplicate *before* paying for distillation.

**Architecture:** Promote the structural Merkle digest + perturbation sweep from the test header into `src/scan.c`. Add an opt-in `LibraryGateConfig` to `library.c` that, before each distillation, (1) checks every plan primitive against the lifecycle `0.9/16` evidence bar (fail → DEFER, skip this pass) and (2) computes the smallest-in-class canonical digest and skips if already admitted this run (SKIP). The distiller (`consolidate.c`) and planner/executor cores are untouched. Gate default-off = byte-identical legacy behavior.

**Tech Stack:** C11, MinGW gcc (`-mno-avx` required), Makefile single-exe test suite. Reuses `btn_reliability`, `lifecycle_promote_provisional`'s criterion, `consolidate_*`, `finalize_chunk`'s existing cert/behavioral-dedup/law-guard.

> **Commits on this repo are user-managed.** Each "Commit (user)" step lists the files to stage; the user runs the actual `git commit`. Do not run `git add`/`git commit` unless the user authorizes it in-session.

---

## File Structure

- **`include/scan.h`** (modify): declare `scan_plan_digest`, `structural_canonical_digest`.
- **`src/scan.c`** (modify): the digest + sweep machinery (moved from the test header, made non-static) + new `structural_canonical_digest`.
- **`tests/structural_pref_common.h`** (modify): delete the local digest/sweep statics; delegate to `scan.h` (single source of truth). Existing structural-pref tests keep passing.
- **`include/library.h`** (modify): `LibraryGateConfig`, `library_gate_config_defaults`, `library_evolve_gated`, and two new `LibraryReport` counters (`deferred`, `structurally_deduped`).
- **`src/library.c`** (modify): evidence check, dedup threading, gated core, `library_evolve_gated`; `library_evolve` becomes a gate-off wrapper.
- **`tests/test_distillation_gate.c`** (create): the gate + dedup + digest TDD tests.
- **`tests/test_all.c`**, **`Makefile`** (modify): wire the new test into `make test`.
- **`tests/distillation_gate_bench.c`** (create): the benchmark.

Standalone compile command used throughout (gate/library tests link the full chain incl. `scan.c`):

```bash
gcc -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx -Wno-unused-function \
  -o test_distillation_gate.exe \
  src/nn.c src/router.c src/plan_table.c src/contract.c src/property.c src/consolidate.c src/scan.c src/library.c \
  tests/test_distillation_gate.c -lm
```

---

## Phase 1 — Promote the structural digest + sweep to `src/scan.c`

### Task 1: Public structural plan digest in `scan.c`

**Files:**
- Modify: `include/scan.h` (add declarations near the other prototypes)
- Modify: `src/scan.c` (add the digest functions)
- Test: `tests/test_distillation_gate.c` (create)

- [ ] **Step 1: Write the failing test**

Create `tests/test_distillation_gate.c` with a digest-stability test. Build two structurally identical single-primitive plans in separately-allocated nodes; assert equal digests; build a different-named plan; assert different digest.

```c
/* tests/test_distillation_gate.c -- TDD for the distillation gate (scan digest,
 * EVIDENCE_CLEAR gate, structural pre-distill dedup).
 * (spec: docs/superpowers/specs/2026-06-19-distillation-gate-design.md) */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/scan.h"
#include "../include/library.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, desc) do { \
    if (cond) printf("  ok   %s\n", (desc)); \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* Build a DagPlan whose root is one DAG_PRIMITIVE producing `out` from one
   source slot of type `in`, named `name`. owned[] holds the root. */
static void make_unit_plan(DagPlan *p, BinaryTransformNetwork *btn,
                           const char *name) {
    DagNode *root = calloc(1, sizeof *root);
    root->kind = DAG_PRIMITIVE;
    root->btn = btn;
    root->name = name;
    root->output_index = 0;
    root->child_count = 1;
    root->children[0] = NULL;     /* source slot, unresolved is fine for digest */
    root->child_ports[0] = 0;
    memset(p, 0, sizeof *p);
    p->root = root;
    p->owned = calloc(1, sizeof(DagNode *));
    p->owned[0] = root;
    p->owned_count = 1;
}
static void free_unit_plan(DagPlan *p) { free(p->owned[0]); free(p->owned); }

static void test_scan_digest(void) {
    BinaryTransformNetwork a, b;
    Port in = { PORT_ONEHOT, 4, 1, "" }, out = { PORT_ONEHOT, 4, 1, "" };
    port_set_tag(&in, "x"); port_set_tag(&out, "g");
    btn_init(&a, 4, 4, 1, 4, 0.5, 1u); btn_set_ports(&a, in, out);
    btn_init(&b, 4, 4, 1, 4, 0.5, 1u); btn_set_ports(&b, in, out);

    DagPlan p1, p2, p3;
    make_unit_plan(&p1, &a, "solo");
    make_unit_plan(&p2, &b, "solo");   /* same name+ports, different BTN ptr */
    make_unit_plan(&p3, &b, "other");  /* different name */

    CHECK(scan_plan_digest(&p1) == scan_plan_digest(&p2),
          "digest is by name/structure, not pointer (same struct -> equal)");
    CHECK(scan_plan_digest(&p1) != scan_plan_digest(&p3),
          "different producer name -> different digest");

    free_unit_plan(&p1); free_unit_plan(&p2); free_unit_plan(&p3);
    btn_free(&a); btn_free(&b);
}

int run_test_distillation_gate(void) {
    failures = 0;
    printf("distillation_gate:\n");
    test_scan_digest();
    if (failures == 0) { printf("DISTILLATION_GATE PASS\n"); return 0; }
    printf("DISTILLATION_GATE FAIL: %d\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) { return run_test_distillation_gate(); }
#endif
```

- [ ] **Step 2: Run to verify it fails (link error: `scan_plan_digest` undefined)**

Run:
```bash
gcc -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx -Wno-unused-function \
  -o test_distillation_gate.exe \
  src/nn.c src/router.c src/plan_table.c src/contract.c src/property.c src/consolidate.c src/scan.c src/library.c \
  tests/test_distillation_gate.c -lm
```
Expected: link/compile error — `undefined reference to scan_plan_digest`.

- [ ] **Step 3: Implement the digest in `scan.c` + declare in `scan.h`**

Move these functions **verbatim** from `tests/structural_pref_common.h` into `src/scan.c` (they currently exist there as `static`, lines ~46–132): `spc_mix_u64`, `spc_mix_str`, `spc_mix_port`, `spc_hash_node`, `spc_plan_digest`, plus the two `#define SPC_FNV_*` constants. Apply this transform:
- Keep `spc_mix_u64`/`spc_mix_str`/`spc_mix_port`/`spc_hash_node` as `static` helpers inside `scan.c` (rename optional; keeping `spc_` is fine internally).
- Rename `spc_plan_digest` → **public** `uint64_t scan_plan_digest(const DagPlan *plan)` (drop `static`).
- Add `#include <stdint.h>` to `scan.c` if not present.

In `include/scan.h`, add near the other prototypes:
```c
#include <stdint.h>

/* Structural Merkle digest of a plan: by name/structure (NOT pointer), so two
   plans with identical topology hash equal even if allocated separately. 0 if
   plan/root is NULL. (Promoted from the structural-preference study.) */
uint64_t scan_plan_digest(const DagPlan *plan);
```

- [ ] **Step 4: Run to verify it passes**

Run the Step 2 command, then `./test_distillation_gate.exe`.
Expected: `ok digest is by name/structure...`, `ok different producer name...`, `DISTILLATION_GATE PASS`.

- [ ] **Step 5: Commit (user)**

Stage: `git add include/scan.h src/scan.c tests/test_distillation_gate.c`
Message: `feat(scan): promote structural plan digest to src (scan_plan_digest)`

---

### Task 2: `structural_canonical_digest` (smallest-in-class) in `scan.c`

**Files:**
- Modify: `include/scan.h`, `src/scan.c`
- Test: `tests/test_distillation_gate.c`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_distillation_gate.c`. Reuse the structural-pref fixtures pattern: an **attractor** (one producer X→G) whose canonical digest equals its only plan digest; a **lure** (two distinct-named equal producers X→G) whose canonical digest is the `min` of the two structures and is **stable** across two calls.

```c
/* one producer X("x")->G(goal_tag), untrained */
static int gate_build_prod(BinaryTransformNetwork *b, const char *goal_tag) {
    Port in = { PORT_ONEHOT, 4, 1, "" }, out = { PORT_ONEHOT, 4, 1, "" };
    port_set_tag(&in, "x"); port_set_tag(&out, goal_tag);
    memset(b, 0, sizeof *b);
    if (btn_init(b, 4, 4, 1, 4, 0.5, 1u) != 0) return -1;
    return btn_set_ports(b, in, out);
}

static void test_canonical_digest(void) {
    BinaryTransformNetwork a, b;
    PrimitiveRegistry reg;
    Port goal = { PORT_ONEHOT, 4, 1, "" }, src_t = { PORT_ONEHOT, 4, 1, "" };
    double sv[4] = {1,0,0,0};
    DagSource src; uint64_t d1, d2;

    gate_build_prod(&a, "gl"); gate_build_prod(&b, "gl");
    port_set_tag(&goal, "gl"); port_set_tag(&src_t, "x");
    src.type = src_t; src.values = sv;

    /* lure: two distinct-named equal producers */
    registry_init(&reg);
    registry_add(&reg, &a, "alt_a");
    registry_add(&reg, &b, "alt_b");
    d1 = structural_canonical_digest(&reg, &src, 1, goal);
    d2 = structural_canonical_digest(&reg, &src, 1, goal);
    CHECK(d1 != 0, "lure: canonical digest is nonzero (a plan exists)");
    CHECK(d1 == d2, "lure: canonical digest is stable across calls (content-addressed)");
    registry_free(&reg);
    btn_free(&a); btn_free(&b);
}
```

- [ ] **Step 2: Run to verify it fails**

Run the Task 1 Step 2 build. Expected: `undefined reference to structural_canonical_digest`. (Add `test_canonical_digest();` call into `run_test_distillation_gate`.)

- [ ] **Step 3: Implement `structural_canonical_digest` in `scan.c`**

Move **verbatim** from `tests/structural_pref_common.h` into `scan.c` as `static`: `spc_permute_entries`, `spc_plan_under_perturbation`, the grid constants (`SPC_PERM_COUNT`, `SPC_BEAMS`, `SPC_BEAM_COUNT`, `SPC_MEMO_COUNT`, `SPC_CELL_COUNT`, baseline defines), `SpcCell`, `SpcSweep`, `spc_count_distinct`, `spc_run_sweep`. (They use `scan_plan_digest`/`spc_hash_node` from Task 1.) Then add the public function:

```c
/* The CANONICAL digest of a goal's plan equivalence class: run the footprint-
   free perturbation sweep, return the SMALLEST structural digest among the
   distinct planned structures. min(digest) is a stable, content-addressed
   identity (unlike reproduction-count rank-0, which is grid-dependent). Returns
   0 if no plan exists. Read-only over reg (plans against permuted copies). */
uint64_t structural_canonical_digest(const PrimitiveRegistry *reg,
                                     const DagSource *sources, size_t n_sources,
                                     Port goal) {
    SpcSweep sweep;
    uint64_t best = 0; int have = 0; size_t i;
    if (reg == NULL) return 0;
    if (spc_run_sweep(reg, sources, n_sources, goal, &sweep) != 0) return 0;
    for (i = 0; i < sweep.cell_count; ++i) {
        if (!sweep.cells[i].planned) continue;
        if (!have || sweep.cells[i].digest < best) { best = sweep.cells[i].digest; have = 1; }
    }
    return have ? best : 0;
}
```

Declare in `include/scan.h`:
```c
/* Smallest structural digest over the goal's plan equivalence class -- a stable,
   content-addressed dedup key. Runs a perturbation sweep (~20 ms; admission-time
   only, never the live planner). 0 if no plan exists. Footprint-free. */
uint64_t structural_canonical_digest(const PrimitiveRegistry *reg,
                                     const DagSource *sources, size_t n_sources,
                                     Port goal);
```

- [ ] **Step 4: Run to verify it passes**

Run build + `./test_distillation_gate.exe`. Expected: the two new `ok` lines + `DISTILLATION_GATE PASS`.

- [ ] **Step 5: Commit (user)**

Stage: `git add include/scan.h src/scan.c tests/test_distillation_gate.c`
Message: `feat(scan): structural_canonical_digest (smallest-in-class dedup key)`

---

### Task 3: Re-point the test header to `scan.c` (single source of truth)

**Files:**
- Modify: `tests/structural_pref_common.h`
- Test: existing structural-pref suite (regression)

- [ ] **Step 1: Replace the local statics with delegation**

In `tests/structural_pref_common.h`, delete the local definitions of `spc_plan_digest` and (optionally) the sweep helpers now living in `scan.c`, add `#include "../include/scan.h"`, and provide a thin shim so existing call sites compile unchanged:
```c
static uint64_t spc_plan_digest(const DagPlan *plan) { return scan_plan_digest(plan); }
```
Leave `spc_hash_node` only if still referenced directly by a test; otherwise remove. Keep `spc_run_sweep`/`SpcRanked`/`structural_pref_rank` in the header (they are study/anchor helpers) but have `spc_plan_digest` delegate. Do NOT change digest semantics — `scan_plan_digest` is the same algorithm moved verbatim, so all digest equality/inequality assertions hold.

- [ ] **Step 2: Run the full structural-pref regression**

```bash
gcc -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx -Wno-unused-function \
  -o test_structural_pref_b0.exe src/nn.c src/router.c src/plan_table.c src/contract.c src/scan.c \
  tests/test_structural_pref_b0.c -lm && ./test_structural_pref_b0.exe
```
Also rebuild/run `test_structural_pref` and `test_structural_pref_adversarial`. Expected: all three still `PASS` (digests unchanged → flips/guards unchanged).

- [ ] **Step 3: Commit (user)**

Stage: `git add tests/structural_pref_common.h`
Message: `refactor(scan): structural-pref test header delegates digest to scan.c`

---

## Phase 2 — The opt-in `EVIDENCE_CLEAR` gate in `library.c`

### Task 4: `LibraryGateConfig` + gated entry point (gate-off == legacy)

**Files:**
- Modify: `include/library.h`, `src/library.c`
- Test: `tests/test_distillation_gate.c`

- [ ] **Step 1: Write the failing test (gate-off parity)**

Add a test: build a tiny registry + a route task that `library_evolve` would distill, run `library_evolve_gated` with a **disabled** gate, assert it mints the same chunk count as plain `library_evolve`.

```c
/* Minimal: assume a helper that builds a 2-step route fixture (provided in the
   test). Here we assert the gated entry with gate disabled == legacy. */
static void test_gate_off_parity(void) {
    LibraryGateConfig g;
    library_gate_config_defaults(&g);   /* enabled = 0 */
    CHECK(g.enabled == 0, "gate config default is DISABLED (legacy)");
    CHECK(g.evidence_threshold == 0.9 && g.min_evidence == 16,
          "gate defaults reuse the lifecycle 0.9/16 bar");
    /* Full mint-parity check is exercised in Task 6's fixture; here we only
       lock the default contract. */
}
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: `LibraryGateConfig` / `library_gate_config_defaults` undefined.

- [ ] **Step 3: Add the config + gated core**

In `include/library.h`:
```c
/* Opt-in distillation gate. enabled = 0 -> byte-identical legacy library_evolve.
   evidence_threshold/min_evidence reuse the lifecycle promotion bar (0.9/16):
   every primitive in a candidate plan must clear them before its composition is
   distilled. dedup_structural toggles the smallest-in-class pre-distill dedup. */
typedef struct {
    int    enabled;
    double evidence_threshold;   /* default 0.9  */
    size_t min_evidence;         /* default 16   */
    int    dedup_structural;     /* default 1 when enabled */
} LibraryGateConfig;

/* enabled 0, evidence_threshold 0.9, min_evidence 16, dedup_structural 1. */
void library_gate_config_defaults(LibraryGateConfig *g);

/* Like library_evolve, but applies the admission gate (NULL gate == legacy). */
int library_evolve_gated(PrimitiveRegistry *reg,
                         const LibraryTask *tasks, size_t n_tasks,
                         const Property *laws, size_t n_laws,
                         const ConsolidateConfig *cfg,
                         const LibraryGateConfig *gate,
                         size_t max_iterations,
                         LibraryReport *report);
```
Add two counters to `LibraryReport`:
```c
    size_t deferred;             /* candidate plans skipped: EVIDENCE_CLEAR failed */
    size_t structurally_deduped; /* candidate plans skipped: canonical digest already admitted */
```

In `src/library.c`, add a per-run gate state and thread it:
```c
typedef struct {
    const LibraryGateConfig *cfg;       /* NULL or cfg->enabled==0 -> off */
    uint64_t seen[LIBRARY_MAX_CHUNKS];  /* admitted canonical digests this run */
    size_t   seen_count;
} GateState;

void library_gate_config_defaults(LibraryGateConfig *g) {
    if (g == NULL) return;
    g->enabled = 0; g->evidence_threshold = 0.9; g->min_evidence = 16;
    g->dedup_structural = 1;
}
static int gate_on(const GateState *gs) { return gs && gs->cfg && gs->cfg->enabled; }
```
Refactor the loop body: change `try_consolidate`, `evolve_route`, `evolve_dag` to take an extra `GateState *gs` (pass `NULL`/off from legacy `library_evolve`). Add a shared core:
```c
static int evolve_run(PrimitiveRegistry *reg, const LibraryTask *tasks, size_t n_tasks,
                      const Property *laws, size_t n_laws, const ConsolidateConfig *cfg,
                      GateState *gs, size_t max_iterations, LibraryReport *report) {
    ConsolidateConfig cfg_local; size_t iter, t;
    if (report == NULL) return 0;
    memset(report, 0, sizeof *report);
    if (cfg != NULL) cfg_local = *cfg; else consolidate_config_defaults(&cfg_local);
    for (iter = 0; iter < max_iterations; ++iter) {
        size_t added = 0; report->iterations_run = iter + 1;
        for (t = 0; t < n_tasks; ++t)
            added += (size_t)try_consolidate(reg, &tasks[t], laws, n_laws, &cfg_local, gs, report);
        if (added == 0) break;
    }
    return 0;
}
int library_evolve(PrimitiveRegistry *reg, const LibraryTask *tasks, size_t n_tasks,
                   const Property *laws, size_t n_laws, const ConsolidateConfig *cfg,
                   size_t max_iterations, LibraryReport *report) {
    return evolve_run(reg, tasks, n_tasks, laws, n_laws, cfg, NULL, max_iterations, report);
}
int library_evolve_gated(PrimitiveRegistry *reg, const LibraryTask *tasks, size_t n_tasks,
                         const Property *laws, size_t n_laws, const ConsolidateConfig *cfg,
                         const LibraryGateConfig *gate, size_t max_iterations,
                         LibraryReport *report) {
    GateState gs; gs.cfg = gate; gs.seen_count = 0;
    return evolve_run(reg, tasks, n_tasks, laws, n_laws, cfg, &gs, max_iterations, report);
}
```
Add `#include "../include/scan.h"` to `src/library.c`. Thread `gs` through `try_consolidate`/`evolve_route`/`evolve_dag` signatures (pass it down; unused for now).

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: the two `ok` config lines pass; the existing `test_library` still builds (legacy `library_evolve` signature unchanged).

- [ ] **Step 5: Commit (user)**

Stage: `git add include/library.h src/library.c tests/test_distillation_gate.c`
Message: `feat(library): opt-in LibraryGateConfig + library_evolve_gated (gate-off == legacy)`

---

### Task 5: `EVIDENCE_CLEAR` check + DEFER routing

**Files:**
- Modify: `src/library.c`
- Test: `tests/test_distillation_gate.c`

- [ ] **Step 1: Write the failing test**

Fixture: a registry whose base primitives form a 2-step route to a goal, with the steps at **0 evidence**. With the gate **enabled**, `library_evolve_gated` mints **0** chunks and `report.deferred >= 1`. Then seed every step to `successes=16, failures=0` (reliability ≈ 0.94 ≥ 0.9, evidence 16 ≥ 16) and assert it now mints the chunk.

```c
static void gate_set_ev(BinaryTransformNetwork *b, unsigned long s, unsigned long f) {
    b->output_successes = s; b->output_failures = f;   /* _Atomic store */
}
/* test_evidence_gate(): build the 2-step route fixture (helper in the test),
   run gated with enabled=1; assert chunk_count==0 && deferred>=1; seed evidence;
   re-run on a fresh registry; assert chunk_count==1. */
```

- [ ] **Step 2: Run to verify it fails**

Build + run. Expected: with the unimplemented check, the gated run still mints (chunk_count==1 when it should DEFER) → `FAIL`.

- [ ] **Step 3: Implement the evidence check**

In `src/library.c`, add:
```c
/* Every primitive carries >= min_evidence outcomes AND reliability >= threshold
   (the lifecycle 0.9/16 promotion bar). Empty plan -> not clear. */
static int btn_evidence_clear(const BinaryTransformNetwork *p,
                              double threshold, size_t min_evidence) {
    unsigned long ev;
    if (p == NULL) return 0;
    ev = (unsigned long)p->output_successes + (unsigned long)p->output_failures;
    return btn_reliability(p) >= threshold && ev >= min_evidence;
}
static int route_evidence_clear(const RoutePlan *plan, const GateState *gs) {
    size_t i;
    if (!gate_on(gs)) return 1;
    for (i = 0; i < plan->length; ++i)
        if (!btn_evidence_clear(plan->steps[i], gs->cfg->evidence_threshold,
                                gs->cfg->min_evidence)) return 0;
    return 1;
}
static int dag_evidence_clear(const DagPlan *plan, const GateState *gs) {
    size_t i;
    if (!gate_on(gs)) return 1;
    if (plan->owned == NULL) return plan->root && plan->root->btn
        ? btn_evidence_clear(plan->root->btn, gs->cfg->evidence_threshold, gs->cfg->min_evidence)
        : 0;
    for (i = 0; i < plan->owned_count; ++i)
        if (plan->owned[i] && plan->owned[i]->kind == DAG_PRIMITIVE &&
            !btn_evidence_clear(plan->owned[i]->btn, gs->cfg->evidence_threshold,
                                gs->cfg->min_evidence)) return 0;
    return 1;
}
```
Wire into `evolve_route` (after `plan.length < 2` guard, before `consolidate_route`):
```c
    if (!route_evidence_clear(&plan, gs)) { report->deferred++; return 0; }
```
Wire into `evolve_dag` (after the `count_dag_primitives < 2` guard, before `consolidate_dag` — the plan is still alive there):
```c
    if (!dag_evidence_clear(&plan, gs)) { report->deferred++; dag_free(&plan); return 0; }
```

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: `chunk_count==0 && deferred>=1` at 0 evidence; `chunk_count==1` after seeding. `PASS`.

- [ ] **Step 5: Commit (user)**

Stage: `git add src/library.c tests/test_distillation_gate.c`
Message: `feat(library): EVIDENCE_CLEAR gate (0.9/16) with DEFER routing`

---

## Phase 3 — Structural pre-distill dedup

### Task 6: Canonical-digest dedup (SKIP before consolidate)

**Files:**
- Modify: `src/library.c`
- Test: `tests/test_distillation_gate.c`

- [ ] **Step 1: Write the failing test**

Fixture: two tasks that reduce to the same goal-function via the SAME or variant structures, both with evidence-clear primitives. With the gate enabled (`dedup_structural=1`), assert `chunk_count == 1` and `report.structurally_deduped >= 1` (the second task hit the dedup pre-filter, before paying `consolidate`). Contrast: with `dedup_structural=0`, the existing behavioral dedup still yields `chunk_count == 1` but `structurally_deduped == 0` (it deduped post-distill).

- [ ] **Step 2: Run to verify it fails**

Build + run. Expected: `structurally_deduped` stays 0 (no pre-distill dedup yet) → `FAIL`.

- [ ] **Step 3: Implement the dedup pre-filter**

In `src/library.c`, add the helper + insert in both `evolve_route` and `evolve_dag` **after** the evidence check and **before** `consolidate_*`:
```c
/* Returns 1 if this goal's canonical digest was already admitted this run
   (SKIP). On a miss, returns 0 and the caller records the digest after a
   successful finalize. dedup off / digest 0 (no plan) -> never a dup. */
static int gate_is_dup(GateState *gs, uint64_t digest) {
    size_t i;
    if (!gate_on(gs) || !gs->cfg->dedup_structural || digest == 0) return 0;
    for (i = 0; i < gs->seen_count; ++i) if (gs->seen[i] == digest) return 1;
    return 0;
}
static void gate_remember(GateState *gs, uint64_t digest) {
    if (!gate_on(gs) || !gs->cfg->dedup_structural || digest == 0) return;
    if (gs->seen_count < LIBRARY_MAX_CHUNKS) gs->seen[gs->seen_count++] = digest;
}
```
In `evolve_route` (sources for a 1-source route: build a `DagSource` of `task->sources[0]` for the digest sweep — the sweep re-plans by type, values unused):
```c
    if (gate_on(gs) && gs->cfg->dedup_structural) {
        DagSource ds = { task->sources[0], NULL };
        uint64_t key = structural_canonical_digest(reg, &ds, 1, task->goal);
        if (gate_is_dup(gs, key)) { report->structurally_deduped++; return 0; }
        /* remember after acceptance below */
    }
```
Because the `key` is needed again after `finalize_chunk`, hoist it: compute `key` once into a local `uint64_t key = 0;` before consolidate, then after `accepted = finalize_chunk(...)`:
```c
    if (accepted) gate_remember(gs, key);
```
Mirror exactly in `evolve_dag` using the already-built `sources[]` array (`structural_canonical_digest(reg, sources, task->n_sources, task->goal)`), computing `key` while the plan/sources are alive (before `dag_free`).

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: gate-on `chunk_count==1 && structurally_deduped>=1`; `dedup_structural=0` → `structurally_deduped==0`. `PASS`.

- [ ] **Step 5: Commit (user)**

Stage: `git add src/library.c tests/test_distillation_gate.c`
Message: `feat(library): structural pre-distill dedup via canonical digest`

---

## Phase 4 — Wire into the suite + benchmark

### Task 7: Wire `test_distillation_gate` into `make test`

**Files:**
- Modify: `tests/test_all.c`, `Makefile`

- [ ] **Step 1: Add the declaration + call in `tests/test_all.c`**

After `int run_test_structural_pref_adversarial_circuit(void);` add:
```c
int run_test_distillation_gate(void);
```
After `total_failures += run_test_structural_pref_adversarial_circuit();` add:
```c
    total_failures += run_test_distillation_gate();
```

- [ ] **Step 2: Add the source to the `test_all` Makefile lists**

In `Makefile`, append `tests/test_distillation_gate.c` to BOTH the dependency list line and the recipe source list line that currently end with `tests/test_structural_pref_adversarial_circuit.c \` (the recipe line is tab-indented).

- [ ] **Step 3: Run the full suite**

Run: `make test`
Expected: `DISTILLATION_GATE PASS` appears and `ALL TESTS PASSED (single exe)`, exit 0.

- [ ] **Step 4: Commit (user)**

Stage: `git add tests/test_all.c Makefile`
Message: `test(library): wire distillation-gate suite into make test`

---

### Task 8: Benchmark — gate overhead + dedup compute saved

**Files:**
- Create: `tests/distillation_gate_bench.c`
- Modify: `Makefile` (add a `dgate_bench` target mirroring an existing bench target)

- [ ] **Step 1: Write the benchmark**

`tests/distillation_gate_bench.c`: build a task set where K tasks reduce to the same goal-function. Time three configs over the same fixture: (1) legacy `library_evolve`; (2) gated with `dedup_structural=0`; (3) gated with `dedup_structural=1`. Report wall-clock, `chunk_count`, `structurally_deduped`, and the count of `consolidate` calls avoided (≈ `structurally_deduped`). Use `clock()` (no `Date.now`). Print a small table.

```c
/* tests/distillation_gate_bench.c -- gate overhead + dedup-avoided-compute. */
#include "../include/library.h"
#include <stdio.h>
#include <time.h>
/* build fixture: N copies of a 2-step route task to the same goal;
   run each config, time with clock(), print rows. */
int main(void) {
    /* ... fixture build ... */
    /* for cfg in {legacy, gated-nodedup, gated-dedup}: time library_evolve[_gated] */
    /* print: config | ms | chunks | deduped */
    return 0;
}
```

- [ ] **Step 2: Add the Makefile target**

Mirror an existing bench target (e.g. `lifecycle_bench`, Makefile ~line 269):
```make
DGATE_BENCH := tests/distillation_gate_bench.c
dgate_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(DGATE_BENCH)
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(DGATE_BENCH) $(LDFLAGS)
	./dgate_bench
```

- [ ] **Step 3: Run the benchmark**

Run: `make dgate_bench`
Expected: a 3-row table; gated-dedup shows `deduped >= 1` and lower wall-clock than gated-nodedup when duplicate tasks are present (consolidate avoided), with gate overhead = the ~20 ms/goal sweep on uniques.

- [ ] **Step 4: Commit (user)**

Stage: `git add tests/distillation_gate_bench.c Makefile`
Message: `bench(library): distillation-gate overhead + dedup-avoided-compute`

---

## Self-Review

**Spec coverage:**
- 2-leg gate (`CERTIFIED ∧ EVIDENCE_CLEAR`): `CERTIFIED` is the pre-existing `consolidate` verification + `finalize_chunk` cert/law-guard (Tasks 4–5 leave it intact); `EVIDENCE_CLEAR` = Task 5. ✓
- DISCARD (cert fail) — unchanged existing behavior (consolidate refusal / law rollback). ✓
- DEFER (evidence fail) — Task 5, `report.deferred`. ✓
- Canonicality = dedup (smallest-in-class) — Tasks 2 + 6. ✓ Priority = out of scope (spec non-goal). ✓
- Module split: mechanism `consolidate.c` untouched; policy `library.c` (Tasks 4–6); promotion `scan.c` (Tasks 1–3). ✓
- Opt-in / regression safety — Task 4 (gate-off == legacy), Task 3 (structural-pref regression), Task 7 (`make test`). ✓
- Benchmark — Task 8 (the user's requested end step). ✓

**Placeholder scan:** Task 5/6 test bodies reference a "2-step route fixture helper" — the executor must write the concrete fixture (a registry of two chainable certified base primitives, e.g. reuse the `route_demo`/`test_library` fixture pattern). This is the one place the plan defers concrete fixture construction to the executor; everything else is full code. Flagged here intentionally rather than hidden.

**Type consistency:** `LibraryGateConfig{enabled,evidence_threshold,min_evidence,dedup_structural}`, `GateState{cfg,seen,seen_count}`, `structural_canonical_digest(reg,sources,n_sources,goal)`, `scan_plan_digest(plan)`, `report.deferred`/`report.structurally_deduped` — names are used identically across Tasks 2–8. ✓

**Open executor note:** the Task 5/6 fixtures need real chainable certified primitives so `route_plan`/`dag_plan` actually return a length-≥2 plan; model them on `tests/test_library.c`'s existing fixture (the same tasks `library_evolve` already distills).
