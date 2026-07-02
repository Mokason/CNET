# Lifecycle Spine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an explicit `PrimitiveState` (FUZZY/PROVISIONAL/FROZEN/RESET) to the primitive registry, with the transitions and the opt-in planner behavior the four lifecycle behaviors will hang off — zero-init reproducing today's planning exactly.

**Architecture:** The enum and two new fields live in `router.h`; transitions are inline edits to the registry's own functions (`registry_init`, `registry_add` in `router.c`; `registry_add_certified` in `contract.c`). The single planner change is one guard in the existing `entry_usable` chokepoint (router.c:291), so every planner (route/dag/circuit) inherits RESET-skip at once. Promotion and a name→state setter are small registry operations added to `router.c`. No new translation unit yet — `src/lifecycle.c` is introduced by the self-healing plan, the first real driver over public APIs.

**Tech Stack:** C11, gcc (`make`), the `test_all` single-exe suite.

**Scope notes / deferred:**
- The spec's "FROZEN tie-break preference" in the planner is **deferred** — no downstream phase depends on it, and it risks the byte-identical-plan regression by touching the DP tie logic. RESET-skip (which self-healing *does* depend on) is the essential planner change and is implemented here.
- Lifecycle state is runtime-only (like reliability counters); no persistence in this plan.

---

## File structure

- `include/router.h` — add `PrimitiveState` enum (before `RegistryEntry`), `state` field on `RegistryEntry`, `lifecycle_enabled` field on `PrimitiveRegistry`, and declarations for `registry_set_state` / `lifecycle_promote_provisional`.
- `src/router.c` — init the new fields (`registry_init`, `registry_add`); add RESET guard to `entry_usable`; implement `registry_set_state` and `lifecycle_promote_provisional`.
- `src/contract.c` — set `state = PRIM_FROZEN` on the two `registry_add_certified` success paths.
- `tests/test_lifecycle.c` — NEW suite (`run_test_lifecycle`), self-contained synthetic BTNs.
- `tests/test_all.c` — declare + call `run_test_lifecycle`.
- `Makefile` — add `tests/test_lifecycle.c` to the `test_all` target; add a `lifecycle_bench` build target + `lbench` run alias (Task 7).
- `tests/lifecycle_bench.c` — NEW benchmark (Task 7): ns/`route_plan` with lifecycle OFF vs ON over a scaled registry. NOT part of `make test`.

---

### Task 1: New suite + FUZZY default on `registry_add`

**Files:**
- Create: `tests/test_lifecycle.c`
- Modify: `tests/test_all.c:26` (add declaration), `tests/test_all.c:47` (add call)
- Modify: `Makefile:304-317` (`test_all` target source list)
- Modify: `include/router.h:34-66` (enum + fields), `src/router.c:4569` (`registry_init`), `src/router.c:4607` (`registry_add`)

- [ ] **Step 1: Write the failing test** — create `tests/test_lifecycle.c`:

```c
/*
 * Hermetic tests for the primitive lifecycle spine: PrimitiveState transitions
 * (FUZZY on add, FROZEN on certify), registry_set_state, evidence-based
 * promotion, and the opt-in RESET-skip in the planner. Synthetic BTNs only --
 * the planner reads contracts/reliability, so no training or weight files.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int make_btn(BinaryTransformNetwork *b, size_t in, size_t out,
                    Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, input_port, output_port);
}

static void test_fuzzy_default(void) {
    BinaryTransformNetwork b = {0};
    PrimitiveRegistry reg;

    printf("lifecycle: FUZZY default on registry_add:\n");
    if (make_btn(&b, 16, 4, P(PORT_ONEHOT, 16, 1), P(PORT_BINARY_MSB, 4, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    CHECK(reg.lifecycle_enabled == 0, "registry_init zeroes lifecycle_enabled");
    CHECK(registry_add(&reg, &b, "p") == 0, "add p");
    CHECK(reg.entries[0].state == PRIM_FUZZY, "registry_add sets state FUZZY");
    registry_free(&reg);
    btn_free(&b);
}

int run_test_lifecycle(void) {
    test_fuzzy_default();

    if (failures == 0) {
        printf("\nAll lifecycle tests passed.\n");
        return 0;
    }
    printf("\n%d lifecycle test(s) FAILED.\n", failures);
    return 1;
}
```

- [ ] **Step 2: Wire the suite into `test_all`** — in `tests/test_all.c`, add the declaration after line 26 (`int run_test_expr(void);`):

```c
int run_test_lifecycle(void);
```

and add the call after line 47 (`total_failures += run_test_expr();`):

```c
    total_failures += run_test_lifecycle();
```

In the `Makefile` `test_all` target, add `tests/test_lifecycle.c` to BOTH the prerequisite list (after `tests/test_expr.c \` near line 308) and the compile line (after `tests/test_expr.c \` near line 316). The two lines become:

```make
          tests/test_library.c tests/test_fastpath.c tests/test_residue.c tests/test_expr.c tests/test_lifecycle.c \
```

(identical text in the prerequisites block and the recipe block).

- [ ] **Step 3: Build to verify it fails**

Run: `make test`
Expected: compile error in `tests/test_lifecycle.c` — `PRIM_FUZZY` undeclared and `RegistryEntry has no member named 'state'` / `PrimitiveRegistry has no member named 'lifecycle_enabled'`.

- [ ] **Step 4: Add the enum and fields** — in `include/router.h`, insert the enum immediately before `RegistryEntry` (before line 34):

```c
/* Primitive lifecycle state (the spine for self-healing, meltdown, shadow,
   and cost-aware behaviors). Numeric 0 == FUZZY keeps zero-init consistent.
   The only path INTO FROZEN is a passing btn_certify; RESET overrides FROZEN
   for planning eligibility without erasing the historical `certified` fact. */
typedef enum {
    PRIM_FUZZY = 0,        /* registered, uncertified, little/no evidence */
    PRIM_PROVISIONAL = 1,  /* uncertified but accruing positive evidence */
    PRIM_FROZEN = 2,       /* certified / law-proven; maximum trust */
    PRIM_RESET = 3         /* failed a runtime invariant; excluded from planning */
} PrimitiveState;
```

Add the `state` field to `RegistryEntry` (after the `certified` line, line 37):

```c
typedef struct {
    BinaryTransformNetwork *btn;  /* borrowed; the registry does not own it */
    const char *name;
    int certified;  /* set only by registry_add_certified */
    PrimitiveState state;  /* lifecycle state; FUZZY on add, FROZEN on certify */
} RegistryEntry;
```

Add the `lifecycle_enabled` field to `PrimitiveRegistry` (after the `rank_artifact` line 65, inside the struct):

```c
    /* Lifecycle policy: nonzero -> planners exclude PRIM_RESET entries
       (the spine's RESET-skip). registry_init zeroes it -- opt in, mirroring
       require_certified: zero-init = legacy behavior (state ignored). */
    int lifecycle_enabled;
```

Add the two API declarations after the `registry_remove_last` declaration (after line 95):

```c
/* Set the lifecycle state of the primitive named `name`. Returns 0, or -1 if
   reg is NULL or no entry has that name (first strcmp match wins). */
int registry_set_state(PrimitiveRegistry *reg, const char *name,
                       PrimitiveState state);

/* Promote every PRIM_FUZZY entry whose learned reliability >= promote_threshold
   AND whose recorded evidence (successes + failures) >= min_evidence to
   PRIM_PROVISIONAL. PROVISIONAL never reaches FROZEN here (that needs a proof);
   FROZEN and RESET entries are left untouched. */
void lifecycle_promote_provisional(PrimitiveRegistry *reg,
                                   double promote_threshold,
                                   size_t min_evidence);
```

In `src/router.c` `registry_init` (after `reg->rank_artifact = NULL;`, line 4580):

```c
    reg->lifecycle_enabled = 0;
```

In `src/router.c` `registry_add`, after `reg->entries[reg->count].certified = 0;` (line 4609):

```c
    reg->entries[reg->count].state = PRIM_FUZZY;
```

- [ ] **Step 5: Build + run to verify it passes**

Run: `make test`
Expected: PASS — output includes `ok   registry_init zeroes lifecycle_enabled`, `ok   registry_add sets state FUZZY`, and `ALL TESTS PASSED (single exe)`.

- [ ] **Step 6: Commit**

```bash
git add include/router.h src/router.c tests/test_lifecycle.c tests/test_all.c Makefile
git commit -m "feat(lifecycle): PrimitiveState spine + FUZZY-on-add

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `registry_set_state`

**Files:**
- Modify: `tests/test_lifecycle.c` (add test + register it)
- Modify: `src/router.c` (implement after `registry_remove_last`, ~line 4630)

- [ ] **Step 1: Write the failing test** — add to `tests/test_lifecycle.c` before `run_test_lifecycle`:

```c
static void test_set_state(void) {
    BinaryTransformNetwork b = {0};
    PrimitiveRegistry reg;

    printf("lifecycle: registry_set_state:\n");
    if (make_btn(&b, 16, 4, P(PORT_ONEHOT, 16, 1), P(PORT_BINARY_MSB, 4, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &b, "p");
    CHECK(registry_set_state(&reg, "p", PRIM_RESET) == 0, "set p -> RESET returns 0");
    CHECK(reg.entries[0].state == PRIM_RESET, "p is now RESET");
    CHECK(registry_set_state(&reg, "missing", PRIM_FROZEN) == -1,
          "unknown name returns -1");
    registry_free(&reg);
    btn_free(&b);
}
```

and call it first in `run_test_lifecycle`:

```c
int run_test_lifecycle(void) {
    test_fuzzy_default();
    test_set_state();
```

- [ ] **Step 2: Build to verify it fails**

Run: `make test`
Expected: link error — `undefined reference to registry_set_state`.

- [ ] **Step 3: Implement** — in `src/router.c`, after `registry_remove_last` (after line 4630):

```c
int registry_set_state(PrimitiveRegistry *reg, const char *name,
                       PrimitiveState state) {
    size_t i;
    if (reg == NULL || name == NULL) return -1;
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0) {
            reg->entries[i].state = state;
            return 0;
        }
    }
    return -1;
}
```

(`string.h` is already included in `router.c` — `strcmp`/`same_port_type` use it.)

- [ ] **Step 4: Build + run to verify it passes**

Run: `make test`
Expected: PASS — `ok   set p -> RESET returns 0`, `ok   p is now RESET`, `ok   unknown name returns -1`.

- [ ] **Step 5: Commit**

```bash
git add src/router.c tests/test_lifecycle.c
git commit -m "feat(lifecycle): registry_set_state

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: FROZEN on `registry_add_certified`

**Files:**
- Modify: `tests/test_lifecycle.c` (add test + register it)
- Modify: `src/contract.c:670` and `src/contract.c:679` (set state on both success paths)

- [ ] **Step 1: Write the failing test** — add to `tests/test_lifecycle.c` before `run_test_lifecycle`. A RAW output port makes the contract trivially certifiable: `port_canonicalize` is identity and `port_validate` always true for RAW, so an exemplar equal to the BTN's own deterministic output certifies exactly.

```c
static void test_frozen_on_certify(void) {
    BinaryTransformNetwork b = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double in[4] = {1.0, 0.0, 1.0, 0.0};   /* canonical BINARY_MSB(4,1) */
    double tgt[5];
    const double *out;

    printf("lifecycle: FROZEN on registry_add_certified:\n");
    /* RAW(5,1) output: certify replays `in`, copies raw output, matches tgt. */
    if (make_btn(&b, 4, 5, P(PORT_BINARY_MSB, 4, 1), P(PORT_RAW, 5, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    out = btn_forward(&b, in);
    memcpy(tgt, out, sizeof tgt);

    CHECK(contract_init_borrowed(&c, "rawid", &b, in, tgt, 1) == 0,
          "contract_init_borrowed ok");
    registry_init(&reg);
    CHECK(registry_add_certified(&reg, &b, "rawid", &c) == 0,
          "registry_add_certified succeeds");
    CHECK(reg.entries[0].certified == 1, "entry is certified");
    CHECK(reg.entries[0].state == PRIM_FROZEN,
          "registry_add_certified sets state FROZEN");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&b);
}
```

and call it in `run_test_lifecycle` after `test_set_state();`:

```c
    test_frozen_on_certify();
```

- [ ] **Step 2: Build to verify it fails**

Run: `make test`
Expected: FAIL — `FAIL registry_add_certified sets state FROZEN` (certified is set, state is still FUZZY).

- [ ] **Step 3: Implement** — in `src/contract.c`, set the state on both success paths of `registry_add_certified`. The replace path (after line 670 `reg->entries[i].certified = 1;`):

```c
                reg->entries[i].btn = btn;
                reg->entries[i].certified = 1;
                reg->entries[i].state = PRIM_FROZEN;
                return 0;
```

and the append path (after line 679 `reg->entries[reg->count - 1].certified = 1;`):

```c
    reg->entries[reg->count - 1].certified = 1;
    reg->entries[reg->count - 1].state = PRIM_FROZEN;
    return 0;
```

- [ ] **Step 4: Build + run to verify it passes**

Run: `make test`
Expected: PASS — `ok   registry_add_certified sets state FROZEN`.

- [ ] **Step 5: Commit**

```bash
git add src/contract.c tests/test_lifecycle.c
git commit -m "feat(lifecycle): FROZEN on registry_add_certified

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: `lifecycle_promote_provisional`

**Files:**
- Modify: `tests/test_lifecycle.c` (add test + register it)
- Modify: `src/router.c` (implement after `registry_set_state`)

- [ ] **Step 1: Write the failing test** — add to `tests/test_lifecycle.c` before `run_test_lifecycle`. Evidence counters are public struct fields, so the test sets them directly.

```c
static void test_promote_provisional(void) {
    BinaryTransformNetwork hot = {0};   /* enough good evidence -> promote */
    BinaryTransformNetwork cold = {0};  /* too little evidence -> stays FUZZY */
    BinaryTransformNetwork froz = {0};  /* FROZEN -> untouched */
    BinaryTransformNetwork rst = {0};   /* RESET -> untouched */
    PrimitiveRegistry reg;

    printf("lifecycle: promote FUZZY -> PROVISIONAL by evidence:\n");
    if (make_btn(&hot, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0 ||
        make_btn(&cold, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0 ||
        make_btn(&froz, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0 ||
        make_btn(&rst, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    hot.output_successes = 20; hot.output_failures = 0;   /* rel ~0.95, ev 20 */
    cold.output_successes = 2; cold.output_failures = 0;  /* rel ~0.75, ev 2 */
    froz.output_successes = 20; froz.output_failures = 0;
    rst.output_successes = 20; rst.output_failures = 0;

    registry_init(&reg);
    registry_add(&reg, &hot, "hot");
    registry_add(&reg, &cold, "cold");
    registry_add(&reg, &froz, "froz");
    registry_add(&reg, &rst, "rst");
    registry_set_state(&reg, "froz", PRIM_FROZEN);
    registry_set_state(&reg, "rst", PRIM_RESET);

    lifecycle_promote_provisional(&reg, 0.9, 16);

    CHECK(reg.entries[0].state == PRIM_PROVISIONAL, "hot promoted to PROVISIONAL");
    CHECK(reg.entries[1].state == PRIM_FUZZY, "cold stays FUZZY (low evidence)");
    CHECK(reg.entries[2].state == PRIM_FROZEN, "froz untouched");
    CHECK(reg.entries[3].state == PRIM_RESET, "rst untouched");

    registry_free(&reg);
    btn_free(&hot); btn_free(&cold); btn_free(&froz); btn_free(&rst);
}
```

and call it in `run_test_lifecycle` after `test_frozen_on_certify();`:

```c
    test_promote_provisional();
```

- [ ] **Step 2: Build to verify it fails**

Run: `make test`
Expected: link error — `undefined reference to lifecycle_promote_provisional`.

- [ ] **Step 3: Implement** — in `src/router.c`, after `registry_set_state`:

```c
void lifecycle_promote_provisional(PrimitiveRegistry *reg,
                                   double promote_threshold,
                                   size_t min_evidence) {
    size_t i;
    if (reg == NULL) return;
    for (i = 0; i < reg->count; ++i) {
        const BinaryTransformNetwork *p = reg->entries[i].btn;
        unsigned long evidence;
        if (p == NULL || reg->entries[i].state != PRIM_FUZZY) {
            continue;
        }
        evidence = (unsigned long)p->output_successes +
                   (unsigned long)p->output_failures;
        if (btn_reliability(p) >= promote_threshold &&
            evidence >= min_evidence) {
            reg->entries[i].state = PRIM_PROVISIONAL;
        }
    }
}
```

- [ ] **Step 4: Build + run to verify it passes**

Run: `make test`
Expected: PASS — all four promote checks `ok`.

- [ ] **Step 5: Commit**

```bash
git add src/router.c tests/test_lifecycle.c
git commit -m "feat(lifecycle): evidence-based FUZZY->PROVISIONAL promotion

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: RESET-skip in the planner (opt-in) + legacy regression

**Files:**
- Modify: `tests/test_lifecycle.c` (add test + register it)
- Modify: `src/router.c:291-293` (`entry_usable`)

- [ ] **Step 1: Write the failing test** — add to `tests/test_lifecycle.c` before `run_test_lifecycle`. Two interchangeable 1-hop primitives; registry order makes `bad` (index 0) the default pick, so RESET-skip must flip the choice to `good` only when lifecycle is enabled.

```c
static void test_reset_skip_opt_in(void) {
    BinaryTransformNetwork bad = {0};
    BinaryTransformNetwork good = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    Port in = P(PORT_ONEHOT, 16, 1);
    Port goal = P(PORT_BINARY_MSB, 5, 1);

    printf("lifecycle: opt-in RESET-skip in planner:\n");
    if (make_btn(&bad, 16, 5, in, goal) != 0 ||
        make_btn(&good, 16, 5, in, goal) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &bad, "bad");    /* index 0: default pick by reg order */
    registry_add(&reg, &good, "good");  /* index 1 */

    /* lifecycle off: registry order picks "bad". */
    CHECK(route_plan(&reg, in, goal, &plan) == 0 && plan.length == 1 &&
          strcmp(plan.names[0], "bad") == 0,
          "lifecycle off -> picks 'bad' (registry order)");

    /* mark bad RESET + enable lifecycle: planner must route to "good". */
    registry_set_state(&reg, "bad", PRIM_RESET);
    reg.lifecycle_enabled = 1;
    CHECK(route_plan(&reg, in, goal, &plan) == 0 && plan.length == 1 &&
          strcmp(plan.names[0], "good") == 0,
          "lifecycle on + bad RESET -> picks 'good'");

    /* lifecycle off again: RESET is ignored (legacy) -> "bad" returns. */
    reg.lifecycle_enabled = 0;
    CHECK(route_plan(&reg, in, goal, &plan) == 0 && plan.length == 1 &&
          strcmp(plan.names[0], "bad") == 0,
          "lifecycle off -> RESET ignored, 'bad' again (legacy)");

    registry_free(&reg);
    btn_free(&bad);
    btn_free(&good);
}
```

and call it in `run_test_lifecycle` after `test_promote_provisional();`:

```c
    test_reset_skip_opt_in();
```

- [ ] **Step 2: Build to verify it fails**

Run: `make test`
Expected: FAIL — `FAIL lifecycle on + bad RESET -> picks 'good'` (without the guard, RESET is ignored and `bad` is still chosen).

- [ ] **Step 3: Implement** — replace `entry_usable` in `src/router.c:291-293`:

```c
static int entry_usable(const PrimitiveRegistry *reg, size_t i) {
    if (reg->lifecycle_enabled && reg->entries[i].state == PRIM_RESET) {
        return 0;
    }
    return !reg->require_certified || reg->entries[i].certified;
}
```

- [ ] **Step 4: Build + run to verify it passes**

Run: `make test`
Expected: PASS — all three RESET-skip checks `ok`, and `ALL TESTS PASSED (single exe)` (the rest of the suite, which never sets `lifecycle_enabled`, is unaffected — RESET-skip is gated).

- [ ] **Step 5: Commit**

```bash
git add src/router.c tests/test_lifecycle.c
git commit -m "feat(lifecycle): opt-in RESET-skip in entry_usable (all planners)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Full-suite regression gate

**Files:** none (verification only)

- [ ] **Step 1: Run the whole suite**

Run: `make test`
Expected: `ALL TESTS PASSED (single exe)`, exit 0. Confirms the spine changes are inert with `lifecycle_enabled == 0` everywhere outside the new suite (the byte-identical-behavior gate for existing planners).

- [ ] **Step 2: Confirm the weight-regen path is untouched**

Run: `make run`
Expected: `nn_demo` builds and runs, regenerating the committed `*_weights.txt` files (the spine added no fields to the BTN or any weight format). `git status` should show no unexpected weight diffs beyond what `make run` normally rewrites.

---

### Task 7: Spine overhead benchmark

**Files:**
- Create: `tests/lifecycle_bench.c`
- Modify: `Makefile` (`.PHONY` line ~58, new target, `clean` list ~360)

This is a benchmark (requested alongside the spine), not a `make test` gate. It confirms the spine's thesis-critical property: planning costs the same with `lifecycle_enabled == 0` as before, and the RESET-skip guard is negligible when enabled. Only unused decoy primitives are RESET, so the discovered route is identical across all three policies — any time delta is pure guard overhead.

- [ ] **Step 1: Create `tests/lifecycle_bench.c`**

```c
/*
 * Lifecycle spine benchmark (NOT part of make test; budgeted study).
 * The spine must cost nothing when lifecycle_enabled == 0, and the RESET-skip
 * guard must be negligible when enabled. Reports ns/route_plan over a registry
 * scaled to N primitives for three policies: OFF, ON (no RESET), ON (half the
 * decoys RESET). The route is identical across policies (only unused decoys are
 * RESET), so any delta is pure guard overhead. Timings are machine-indicative.
 */
#include "../include/nn.h"
#include "../include/router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int make_btn(BinaryTransformNetwork *b, size_t in, size_t out,
                    Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) return -1;
    return btn_set_ports(b, input_port, output_port);
}

/* Average ns per route_plan over `iters`. */
static double bench_plan(const PrimitiveRegistry *reg, Port in, Port goal,
                         size_t iters) {
    RoutePlan plan;
    clock_t t0, t1;
    size_t i;
    volatile long sink = 0;
    t0 = clock();
    for (i = 0; i < iters; ++i) {
        sink += route_plan(reg, in, goal, &plan);
    }
    t1 = clock();
    (void)sink;
    return (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1e9 / (double)iters;
}

int main(void) {
    const size_t sizes[] = {16, 64, 256};
    const size_t n_sizes = sizeof sizes / sizeof sizes[0];
    const size_t ITERS = 200000;
    Port in = P(PORT_ONEHOT, 16, 1);
    Port mid = P(PORT_BINARY_MSB, 4, 1);
    Port goal = P(PORT_BINARY_MSB, 5, 1);
    size_t s;

    printf("=== lifecycle spine benchmark (ns/route_plan, %zu iters) ===\n", ITERS);
    printf("%6s %14s %16s %18s\n", "N", "OFF", "ON(no reset)", "ON(50% reset)");

    for (s = 0; s < n_sizes; ++s) {
        size_t N = sizes[s];
        BinaryTransformNetwork *btns = calloc(N, sizeof *btns);
        char (*names)[16] = malloc(N * sizeof *names);
        PrimitiveRegistry reg;
        RoutePlan plan;
        double off, on0, on50;
        size_t i;
        int ok;

        if (btns == NULL || names == NULL) { printf("OOM\n"); free(btns); free(names); return 1; }

        /* prim 0: ONEHOT16 -> BINARY_MSB4 ; prim 1: BINARY_MSB4 -> BINARY_MSB5.
           prims 2..N-1: decoys ONEHOT8 -> BINARY_MSB3 (one shared output type,
           never on the route) to bulk up the entry count cheaply. */
        make_btn(&btns[0], 16, 4, in, mid);
        make_btn(&btns[1], 4, 5, mid, goal);
        for (i = 2; i < N; ++i) {
            make_btn(&btns[i], 8, 3, P(PORT_ONEHOT, 8, 1), P(PORT_BINARY_MSB, 3, 1));
        }
        registry_init(&reg);
        for (i = 0; i < N; ++i) {
            snprintf(names[i], sizeof names[i], "p%zu", i);
            registry_add(&reg, &btns[i], names[i]);
        }

        ok = (route_plan(&reg, in, goal, &plan) == 0 && plan.length == 2);

        reg.lifecycle_enabled = 0;
        off = bench_plan(&reg, in, goal, ITERS);

        reg.lifecycle_enabled = 1;
        on0 = bench_plan(&reg, in, goal, ITERS);

        for (i = 2; i < N; i += 2) registry_set_state(&reg, names[i], PRIM_RESET);
        on50 = bench_plan(&reg, in, goal, ITERS);

        /* RESET only hit unused decoys -> the route is unchanged. */
        ok = ok && (route_plan(&reg, in, goal, &plan) == 0 && plan.length == 2);

        printf("%6zu %12.1f %16.1f %18.1f   %s\n",
               N, off, on0, on50, ok ? "route ok" : "ROUTE BROKEN");

        registry_free(&reg);
        for (i = 0; i < N; ++i) btn_free(&btns[i]);
        free(btns);
        free(names);
    }
    printf("\nOFF is the legacy baseline; ON(no reset) shows the guard's branch\n");
    printf("cost; deltas should sit within timing noise (route is identical).\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile target**

Append the `.PHONY` list (line ~58) with `lifecycle_bench lbench`. Add a target near the other studies:

```make
LIFECYCLE_BENCH := tests/lifecycle_bench.c
lifecycle_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(LIFECYCLE_BENCH) include/nn.h include/router.h include/plan_table.h include/contract.h include/scan.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(SCAN) $(LIFECYCLE_BENCH) $(LDFLAGS)

lbench: lifecycle_bench
	./lifecycle_bench
```

Add `lifecycle_bench` to the `clean` target's `rm -f` list (alongside the other study binaries near line 363).

- [ ] **Step 3: Build + run the benchmark**

Run: `make lbench`
Expected: builds `lifecycle_bench` cleanly, then prints a 3-row table (N = 16, 64, 256) with `route ok` on every row, e.g.:

```
=== lifecycle spine benchmark (ns/route_plan, 200000 iters) ===
     N            OFF     ON(no reset)      ON(50% reset)
    16          <num>          <num>             <num>   route ok
    64          <num>          <num>             <num>   route ok
   256          <num>          <num>             <num>   route ok
```

Record the actual numbers in the commit body. The OFF and ON(no reset) columns should be within timing noise of each other at every N (the spine adds no work when disabled and one branch per entry when enabled).

- [ ] **Step 4: Commit**

```bash
git add tests/lifecycle_bench.c Makefile
git commit -m "bench(lifecycle): spine planning-overhead benchmark

<paste the measured table here>

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (spine section):**
- Enum + `RegistryEntry.state` + `lifecycle_enabled` → Task 1. ✓
- `registry_add` → FUZZY; `registry_add_certified` → FROZEN → Tasks 1, 3. ✓
- `lifecycle_promote_provisional` (thresholds 0.9 / 16, never reaches FROZEN, FROZEN/RESET untouched) → Task 4. ✓
- `registry_set_state` for the RESET transitions used by later phases → Task 2. ✓
- Planner skips RESET only when `lifecycle_enabled` → Task 5. ✓
- Zero-init = byte-identical → Task 6 + the legacy-path assertion in Task 5. ✓
- FROZEN tie-break preference → **explicitly deferred** (documented above; no downstream dependency).
- Spine overhead benchmark → Task 7 (added per request; a benchmark, not a spec gate or a `make test` member).

**Placeholder scan:** none — every step has complete code and exact commands.

**Type consistency:** `PrimitiveState` and members `PRIM_FUZZY/PROVISIONAL/FROZEN/RESET` used identically across router.h, router.c, contract.c, and the test; `registry_set_state(reg, name, state)` and `lifecycle_promote_provisional(reg, threshold, min_evidence)` signatures match between declaration (Task 1) and definitions (Tasks 2, 4) and call sites. Evidence read via the public `output_successes`/`output_failures` fields ([nn.h:75](../../../include/nn.h:75)).
