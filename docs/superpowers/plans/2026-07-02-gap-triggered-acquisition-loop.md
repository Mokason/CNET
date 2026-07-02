# Gap-Triggered Acquisition Loop (v1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **PROJECT RULE — NO GIT:** `G:\AI\CNET` is not a git repository. Run NO git commands. Tasks end at "test passes"; verify via the filesystem. Do not add commit steps.

**Goal:** When the router has no plan (or a unit goes suspect), the system acquires the missing capability itself: mine exemplars from a registered oracle, train a candidate BTN, certify it (PROOF/SAMPLE), seal it to `.cnu`, register it — and the router then plans through it.

**Architecture:** One new module (`src/acquire.c` + `include/acquire.h`) composing only existing public APIs: `route_plan` (−1 = the NO_PLAN gap signal), `contract_domain_cardinality`/`contract_encode_domain_point` (domain enumeration for mining), `btn_train_dynamic` (plastic phase), `btn_certify_exhaustive` + `coverage_accuracy_lower_bound` (PROOF vs SAMPLE), `unit_save` (sealed `.cnu`), `registry_add_certified` (admission), `registry_set_state`/`PRIM_RESET` + `lifecycle_enabled` (rebuild path). Candidates never enter the registry before certification (structural planner_influence = 0). The gap ledger is a text sidecar (`CNET_GAPS 1`) following the CNET_STATS rules: never inside a weight file, load-replaces, malformed → −1 untouched.

**Tech stack:** C11, MinGW gcc (flags fixed in Makefile — note `-mno-avx` is load-bearing), plain `make`. Test: `tests/test_acquire.c` via `make acquire`, wired into the `verify` chain (`make test`).

**Spec:** `docs/superpowers/specs/2026-07-02-gap-triggered-acquisition-loop-design.md`

**Scoping decisions locked here (consistent with the spec's fixture):**
- v1 task signatures are single-input-port → single-goal-port (the `route_plan` shape). Multi-input DAG gaps are follow-up.
- The spec's "EVIDENCE_CLEAR gate" is realized as *oracle-evidence* thresholds carried in `AcquireConfig` (`evidence_threshold` 0.9 / `min_evidence` 16 — the same defaults as `library_gate_config_defaults`), NOT by linking `src/library.c` (that would drag in the whole library-evolve dependency chain for two numbers).
- All behavioral checks happen BEFORE registration; the only post-registration step is a read-only `route_plan` existence check (rollback = `registry_remove_last` + delete the `.cnu`). This is what makes "DEFER is total" and the Δ-4 counter-hygiene assert hold by construction.
- Sampling for enumerable-but-over-budget domains is a deterministic stride (`idx = (k * cardinality) / n`), never `rand()` — byte-identical reruns are the project's regression gate. Non-enumerable (RAW/EVIDENCE/CONCEPT) input domains → `DEFERRED unbounded_domain` in v1.
- Defer reasons are atoms (no spaces) so the sidecar stays line-parseable: `oracle_unfit`, `class_imbalance`, `certify_failed`, `unbounded_domain`, `insufficient_exemplars`, `accuracy_bound`, `seal_failed`, `register_refused`, `replan_failed`, `incumbent_healthy`, `unknown_subject`, `multi_port_unsupported`.

**File structure:**
- Create: `include/acquire.h` — public API: oracle registry, gap ledger, note/coalesce, fallback executor, drain, acquire_now, sidecar save/load, config defaults
- Create: `src/acquire.c` — implementation (only file that changes behavior; core untouched)
- Create: `tests/test_acquire.c` — one binary, numbered sections, exits non-zero on first failure, prints `ALL ACQUIRE TESTS PASSED`
- Modify: `Makefile` — `ACQUIRE_SRC`/`ACQUIRE_TEST` vars, `acquire` target (mirrors the `coverage` target's link set), append `acquire` to the `verify` line (line ~668)

---

### Task 1: Build scaffold (Makefile target + header skeleton + test main)

**Files:**
- Create: `include/acquire.h`
- Create: `src/acquire.c`
- Create: `tests/test_acquire.c`
- Modify: `Makefile` (add vars near line 151; add target after `contract_unit` at ~line 240)

- [ ] **Step 1: Write the header skeleton** — full public API up front so every later task compiles against a stable surface.

`include/acquire.h`:

```c
#ifndef ACQUIRE_H
#define ACQUIRE_H

/* Gap-triggered acquisition loop (v1): the router's capability gaps are
   recorded in a ledger; an oracle (reference implementation) supplies labels;
   the drain trains a candidate BTN and pushes it through the EXISTING
   gate -> certify -> seal -> register chain. A candidate is structurally
   invisible to the planner until it is a sealed, certified, registered unit.
   The ledger is a sidecar (CNET_STATS rules): never inside a weight file,
   load REPLACES, missing/malformed -> -1 with state untouched.
   Spec: docs/superpowers/specs/2026-07-02-gap-triggered-acquisition-loop-design.md */

#include <stddef.h>

#include "nn.h"
#include "router.h"
#include "contract/contract.h"
#include "contract/coverage.h"

#define ACQUIRE_MAX_ORACLES 16
#define ACQUIRE_NAME_MAX 64
#define ACQUIRE_REASON_MAX 64

/* A label source: an in-process reference implementation. in has the input
   port's total values (canonical); the oracle writes the output port's total
   values into out. Returns 0 on success, -1 on refusal (counted as a reject). */
typedef int (*CnetOracleFn)(const double *in, double *out, void *ctx);

typedef struct {
    char name[ACQUIRE_NAME_MAX];
    Port input_port;
    Port output_port;
    CnetOracleFn fn;
    void *ctx;
    size_t calls;
    size_t rejects;   /* refusals + outputs that failed port_validate */
} OracleEntry;

typedef struct {
    OracleEntry entries[ACQUIRE_MAX_ORACLES];
    size_t count;
} OracleRegistry;

typedef enum { GAP_NO_PLAN = 0, GAP_LOW_RELIABILITY = 1, GAP_HEALTH = 2 } GapKind;
typedef enum { GAP_OPEN = 0, GAP_DEFERRED = 1, GAP_CLOSED = 2 } GapStatus;

typedef struct {
    GapKind kind;
    GapStatus status;
    Port input_port;   /* zeroed for GAP_HEALTH (drain resolves from subject) */
    Port goal_port;    /* zeroed for GAP_HEALTH */
    char subject[ACQUIRE_NAME_MAX];      /* suspect unit; "" for NO_PLAN */
    char oracle[ACQUIRE_NAME_MAX];       /* matched oracle; "" until matched */
    char defer_reason[ACQUIRE_REASON_MAX]; /* atom; "" unless DEFERRED */
    size_t times_hit;
    size_t attempts;
    /* Exemplars captured by the fallback path (in-memory only, NOT persisted;
       canonical values). cap_inputs: cap_count x input total; cap_targets:
       cap_count x output total. */
    double *cap_inputs;
    double *cap_targets;
    size_t cap_count;
    size_t cap_limit;
} GapRecord;

typedef struct {
    GapRecord *gaps;
    size_t count;
    size_t capacity;
    /* BTNs minted by acquisition. The registry BORROWS them; the ledger OWNS
       them (and their names). Free with acquire_ledger_free AFTER the registry
       that borrowed them is no longer in use. */
    BinaryTransformNetwork **acquired;
    char (*acquired_names)[ACQUIRE_NAME_MAX];
    size_t acquired_count;
    size_t acquired_capacity;
} AcquireLedger;

typedef struct {
    size_t mine_budget;        /* enumerate the domain when card <= this (4096) */
    size_t sample_count;       /* deterministic stride samples otherwise (256) */
    double holdout_fraction;   /* sampled mode only: fraction excluded from
                                  training but kept in the contract (0.25) */
    double evidence_threshold; /* oracle validity-rate floor (0.9; mirrors
                                  library_gate_config_defaults) */
    size_t min_evidence;       /* min usable exemplars (16; mirrors same) */
    double min_accuracy_bound; /* Wilson lower-bound floor, sampled mode (0.95) */
    double wilson_z;           /* 1.96 = 95% */
    size_t exhaustive_cap;     /* cap for btn_certify_exhaustive (0 = builtin) */
    const char *unit_dir;      /* dir for sealed .cnu files; NULL = skip seal */
    size_t capture_limit;      /* per-gap captured-exemplar cap (256) */
    /* training recipe (lean-teacher defaults; NEVER 1 hidden neuron) */
    size_t init_hidden;        /* 8 */
    size_t max_hidden;         /* 64 */
    double learning_rate;      /* 0.5 */
    unsigned int seed;         /* 42 */
    size_t max_epochs;         /* 4000 */
    size_t growth_window;      /* 200 */
    double target_loss;        /* 1e-4 */
    double min_improvement;    /* 1e-6 */
} AcquireConfig;

void acquire_config_defaults(AcquireConfig *cfg);

typedef struct {
    size_t examined;
    size_t closed;
    size_t deferred;
    size_t skipped_no_oracle;
    CertVerdict last_verdict;                 /* verdict of the last closed gap */
    char last_unit_name[ACQUIRE_NAME_MAX];    /* unit minted by the last close */
    char last_defer_reason[ACQUIRE_REASON_MAX];
} AcquireReport;

void acquire_ledger_init(AcquireLedger *l);
void acquire_ledger_free(AcquireLedger *l);

/* Register a named oracle for a (input_port -> output_port) link signature.
   Returns 0, or -1 (full, bad name atom, or duplicate name). */
int acquire_oracle_register(OracleRegistry *o, const char *name,
                            Port input_port, Port output_port,
                            CnetOracleFn fn, void *ctx);

/* Note a gap. Coalesces: a record with the same (kind, ports, subject) gets
   times_hit incremented instead of a duplicate. DEFERRED records reopen
   (status back to OPEN, reason cleared) so a later drain retries them.
   Returns the record index, or -1 on OOM/bad args. */
int acquire_note_no_plan(AcquireLedger *l, Port input_port, Port goal_port);
int acquire_note_low_reliability(AcquireLedger *l, const char *subject,
                                 double score, double floor_used);
int acquire_note_health(AcquireLedger *l, const char *subject,
                        const char *reason_atom);

/* Plan-or-fallback execution (single process, plain function calls).
   Plan exists -> strict route_execute (normal path). No plan -> note the gap;
   if an oracle matches the task signature exactly, answer via the oracle
   (validate-then-canonicalize both ways) and CAPTURE the pair as a training
   exemplar on the gap record. Returns 0 on an answered task, -1 otherwise. */
int acquire_execute_or_fallback(PrimitiveRegistry *reg, AcquireLedger *l,
                                OracleRegistry *oracles,
                                const AcquireConfig *cfg,
                                Port input_port, Port goal_port,
                                const double *input, size_t in_len,
                                double *output, size_t out_cap);

/* Drain every OPEN gap that has a matching oracle:
   mine (captured + enumerated/stride-sampled) -> oracle-evidence gate ->
   train candidate -> holdout check (sampled mode) -> certify exhaustive
   (PROOF, or SAMPLE + Wilson floor) -> seal .cnu -> register -> replan check.
   Any failure -> status DEFERRED + reason atom, with registry, counters and
   disk byte-identical to before the attempt (DEFER is total). Returns 0. */
int acquire_drain(PrimitiveRegistry *reg, AcquireLedger *l,
                  OracleRegistry *oracles, const AcquireConfig *cfg,
                  AcquireReport *report);

/* Inline mode: the same drain body scoped to ONE task signature, invoked
   synchronously (notes the gap itself if new). Returns 0 iff the gap ended
   CLOSED (a plan now exists). */
int acquire_now(PrimitiveRegistry *reg, AcquireLedger *l,
                OracleRegistry *oracles, const AcquireConfig *cfg,
                Port input_port, Port goal_port, AcquireReport *report);

/* Sidecar persistence ("CNET_GAPS 1"). Statuses + counters + signatures only;
   captured exemplar buffers are runtime-only. Save returns 0/-1. Load REPLACES
   the ledger's gap records on success (acquired-BTN ownership is untouched);
   missing/malformed file -> -1 with *l untouched. */
int acquire_ledger_save(const AcquireLedger *l, const char *path);
int acquire_ledger_load(AcquireLedger *l, const char *path);

#endif /* ACQUIRE_H */
```

- [ ] **Step 2: Write the stub implementation** — every function present, minimal bodies (so the scaffold links), real logic lands in later tasks.

`src/acquire.c`:

```c
#include "../include/acquire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void acquire_config_defaults(AcquireConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->mine_budget = 4096;
    cfg->sample_count = 256;
    cfg->holdout_fraction = 0.25;
    cfg->evidence_threshold = 0.9;   /* mirrors library_gate_config_defaults */
    cfg->min_evidence = 16;          /* mirrors library_gate_config_defaults */
    cfg->min_accuracy_bound = 0.95;
    cfg->wilson_z = 1.96;
    cfg->exhaustive_cap = 0;
    cfg->unit_dir = NULL;
    cfg->capture_limit = 256;
    cfg->init_hidden = 8;
    cfg->max_hidden = 64;
    cfg->learning_rate = 0.5;
    cfg->seed = 42;
    cfg->max_epochs = 4000;
    cfg->growth_window = 200;
    cfg->target_loss = 1e-4;
    cfg->min_improvement = 1e-6;
}

void acquire_ledger_init(AcquireLedger *l) {
    if (!l) return;
    memset(l, 0, sizeof *l);
}

void acquire_ledger_free(AcquireLedger *l) {
    size_t i;
    if (!l) return;
    for (i = 0; i < l->count; ++i) {
        free(l->gaps[i].cap_inputs);
        free(l->gaps[i].cap_targets);
    }
    free(l->gaps);
    for (i = 0; i < l->acquired_count; ++i) {
        btn_free(l->acquired[i]);
        free(l->acquired[i]);
    }
    free(l->acquired);
    free(l->acquired_names);
    memset(l, 0, sizeof *l);
}

int acquire_oracle_register(OracleRegistry *o, const char *name,
                            Port input_port, Port output_port,
                            CnetOracleFn fn, void *ctx) {
    (void)o; (void)name; (void)input_port; (void)output_port; (void)fn; (void)ctx;
    return -1; /* Task 2 */
}

int acquire_note_no_plan(AcquireLedger *l, Port input_port, Port goal_port) {
    (void)l; (void)input_port; (void)goal_port;
    return -1; /* Task 2 */
}

int acquire_note_low_reliability(AcquireLedger *l, const char *subject,
                                 double score, double floor_used) {
    (void)l; (void)subject; (void)score; (void)floor_used;
    return -1; /* Task 2 */
}

int acquire_note_health(AcquireLedger *l, const char *subject,
                        const char *reason_atom) {
    (void)l; (void)subject; (void)reason_atom;
    return -1; /* Task 2 */
}

int acquire_execute_or_fallback(PrimitiveRegistry *reg, AcquireLedger *l,
                                OracleRegistry *oracles,
                                const AcquireConfig *cfg,
                                Port input_port, Port goal_port,
                                const double *input, size_t in_len,
                                double *output, size_t out_cap) {
    (void)reg; (void)l; (void)oracles; (void)cfg; (void)input_port;
    (void)goal_port; (void)input; (void)in_len; (void)output; (void)out_cap;
    return -1; /* Task 4 */
}

int acquire_drain(PrimitiveRegistry *reg, AcquireLedger *l,
                  OracleRegistry *oracles, const AcquireConfig *cfg,
                  AcquireReport *report) {
    (void)reg; (void)l; (void)oracles; (void)cfg; (void)report;
    return -1; /* Task 5 */
}

int acquire_now(PrimitiveRegistry *reg, AcquireLedger *l,
                OracleRegistry *oracles, const AcquireConfig *cfg,
                Port input_port, Port goal_port, AcquireReport *report) {
    (void)reg; (void)l; (void)oracles; (void)cfg; (void)input_port;
    (void)goal_port; (void)report;
    return -1; /* Task 6 */
}

int acquire_ledger_save(const AcquireLedger *l, const char *path) {
    (void)l; (void)path;
    return -1; /* Task 3 */
}

int acquire_ledger_load(AcquireLedger *l, const char *path) {
    (void)l; (void)path;
    return -1; /* Task 3 */
}
```

- [ ] **Step 3: Write the test skeleton with the check harness**

`tests/test_acquire.c`:

```c
/* Gap-triggered acquisition loop (v1) — vertical slice gate.
   Fixture: 4-bit increment (BINARY_MSB w4 "nibble" -> BINARY_MSB w4
   "nibble_next"), withheld from the registry; its reference implementation
   is the oracle. Sections are numbered; first failure exits non-zero. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/acquire.h"
#include "../include/contract/unit.h"

static int checks_run = 0;

static void check(int cond, const char *what) {
    ++checks_run;
    if (!cond) {
        printf("FAIL: %s\n", what);
        exit(1);
    }
    printf("  ok: %s\n", what);
}

/* ---- fixture: ports ---------------------------------------------------- */

static Port make_port(PortFamily fam, size_t w, size_t c, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = fam;
    p.field_width = w;
    p.field_count = c;
    if (port_set_tag(&p, tag) != 0) { printf("FAIL: bad tag %s\n", tag); exit(1); }
    return p;
}

/* nibble value v (0..15) -> 4 MSB-first bits into out[4] */
static void nibble_bits(unsigned v, double *out) {
    out[0] = (v >> 3) & 1u; out[1] = (v >> 2) & 1u;
    out[2] = (v >> 1) & 1u; out[3] = v & 1u;
}

static unsigned bits_nibble(const double *in) {
    return ((in[0] > 0.5) << 3) | ((in[1] > 0.5) << 2) |
           ((in[2] > 0.5) << 1) | (in[3] > 0.5);
}

/* ---- fixture: oracles -------------------------------------------------- */

/* Reference implementation of increment: (v + 1) & 0xF, MSB bits. */
static int oracle_increment(const double *in, double *out, void *ctx) {
    (void)ctx;
    nibble_bits((bits_nibble(in) + 1u) & 0xFu, out);
    return 0;
}

/* Broken oracle: emits ambiguous garbage (fails port_validate). */
static int oracle_broken(const double *in, double *out, void *ctx) {
    (void)in; (void)ctx;
    out[0] = 0.5; out[1] = 0.5; out[2] = 0.5; out[3] = 0.5;
    return 0;
}

int main(void) {
    printf("== acquire: gap-triggered acquisition loop ==\n");

    printf("[1] ledger basics\n");
    /* Task 2 */

    printf("[2] sidecar round-trip\n");
    /* Task 3 */

    printf("[3] oracle fallback + capture\n");
    /* Task 4 */

    printf("[4] drain: NO_PLAN acquisition (headline)\n");
    /* Task 5 */

    printf("[5] acquire_now (inline mode)\n");
    /* Task 6 */

    printf("[6] composition: acquired + frozen unit, certified end-to-end\n");
    /* Task 6 */

    printf("[7] DEFER totality + counter hygiene (broken oracle)\n");
    /* Task 7 */

    printf("[8] rebuild: LOW_RELIABILITY + HEALTH\n");
    /* Task 8 */

    printf("checks run: %d\n", checks_run);
    printf("ALL ACQUIRE TESTS PASSED\n");
    return 0;
}
```

(The `oracle_increment`/`oracle_broken`/`make_port`/`nibble_bits`/`bits_nibble` helpers will be flagged `-Wunused-function` until later tasks use them — silence by referencing them in section stubs is NOT needed; instead mark each with `__attribute__((unused))` is NOT the house style either. Simplest: sections in later tasks use them; for THIS task only, add `(void)oracle_increment; (void)oracle_broken; (void)make_port;` lines at the top of `main` and delete those lines in Task 4/5 when the helpers get real callers.)

- [ ] **Step 4: Add the Makefile wiring** (vars next to `GRADUATE_TEST := tests/test_graduate.c` at line ~151; target after the `contract_unit` rule at line ~240). Do NOT touch the `verify` line yet — that is Task 9, after the whole gate is green.

```make
ACQUIRE_SRC := src/acquire.c
ACQUIRE_TEST := tests/test_acquire.c
```

```make
# Gap-triggered acquisition loop: gap ledger sidecar + oracle mining ->
# train -> certify (PROOF/SAMPLE) -> seal .cnu -> register -> replan.
# Link set mirrors the `coverage` target (+ acquire).
acquire: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(ACQUIRE_TEST) include/nn.h include/router.h include/contract/contract.h include/contract/coverage.h include/contract/unit.h include/acquire.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(ACQUIRE_TEST) $(LDFLAGS)
	./$(BIN_DIR)/acquire > logs/acquire.log 2>&1 || echo "test exited non-zero (see log)"
```

- [ ] **Step 5: Build and run the scaffold**

Run: `make acquire` then `cat logs/acquire.log`
Expected: compiles with no NEW warnings; log ends with `ALL ACQUIRE TESTS PASSED` (all sections empty so far).

---

### Task 2: Ledger core — init/note/coalesce/free + oracle registry + config defaults

**Files:**
- Modify: `src/acquire.c` (replace the Task-1 stubs for oracle_register + the three note functions)
- Modify: `tests/test_acquire.c` (fill section [1])

- [ ] **Step 1: Write the failing tests** — replace the `/* Task 2 */` line in section [1]:

```c
    {
        AcquireLedger led;
        OracleRegistry orc;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        int idx, idx2;

        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);

        check(acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                      oracle_increment, NULL) == 0,
              "oracle registers");
        check(acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                      oracle_increment, NULL) == -1,
              "duplicate oracle name refused");
        check(acquire_oracle_register(&orc, "bad name!", nib, nibn,
                                      oracle_increment, NULL) == -1,
              "non-atom oracle name refused");

        idx = acquire_note_no_plan(&led, nib, nibn);
        check(idx == 0 && led.count == 1, "no_plan gap recorded");
        check(led.gaps[0].kind == GAP_NO_PLAN && led.gaps[0].status == GAP_OPEN,
              "gap is OPEN NO_PLAN");
        check(led.gaps[0].times_hit == 1, "times_hit starts at 1");

        idx2 = acquire_note_no_plan(&led, nib, nibn);
        check(idx2 == 0 && led.count == 1 && led.gaps[0].times_hit == 2,
              "same signature coalesces (times_hit 2, no duplicate)");

        check(acquire_note_low_reliability(&led, "increment", 0.14, 0.5) == 1 &&
              led.count == 2 && led.gaps[1].kind == GAP_LOW_RELIABILITY &&
              strcmp(led.gaps[1].subject, "increment") == 0,
              "low_reliability gap recorded with subject");

        check(acquire_note_health(&led, "increment", "resource_anomaly") == 1 &&
              led.count == 2 && led.gaps[1].times_hit == 2,
              "health on same subject coalesces onto the rebuild record");

        check(acquire_note_health(&led, "hex_value", "resource_anomaly") == 2 &&
              led.count == 3,
              "health on a different subject is a new record");

        acquire_ledger_free(&led);
        check(led.count == 0 && led.gaps == NULL, "free resets the ledger");
    }
```

Note the coalescing rule encoded by the test: LOW_RELIABILITY and HEALTH coalesce with each other **on the same subject** (both mean "rebuild this unit"; the kind field keeps the FIRST trigger). NO_PLAN coalesces on (input_port, goal_port) signature equality including tags.

- [ ] **Step 2: Run to verify failure**

Run: `make acquire` then `cat logs/acquire.log`
Expected: `FAIL: oracle registers` (stubs return -1).

- [ ] **Step 3: Implement** — replace the Task-1 stubs in `src/acquire.c`:

```c
/* ---- small helpers ------------------------------------------------------ */

static int acquire_name_is_atom(const char *s) {
    size_t i;
    if (!s || !s[0]) return 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
    }
    return i < ACQUIRE_NAME_MAX;
}

static int acquire_port_eq(Port a, Port b) {
    return a.family == b.family && a.field_width == b.field_width &&
           a.field_count == b.field_count && strcmp(a.tag, b.tag) == 0;
}

static size_t port_total(Port p) { return p.field_width * p.field_count; }

static GapRecord *ledger_push(AcquireLedger *l) {
    if (l->count == l->capacity) {
        size_t ncap = l->capacity ? l->capacity * 2 : 8;
        GapRecord *ng = realloc(l->gaps, ncap * sizeof *ng);
        if (!ng) return NULL;
        l->gaps = ng;
        l->capacity = ncap;
    }
    memset(&l->gaps[l->count], 0, sizeof l->gaps[l->count]);
    return &l->gaps[l->count++];
}

/* Reopen a DEFERRED record when it is hit again (a later drain retries). */
static void gap_rehit(GapRecord *g) {
    g->times_hit++;
    if (g->status == GAP_DEFERRED) {
        g->status = GAP_OPEN;
        g->defer_reason[0] = '\0';
    }
}

int acquire_oracle_register(OracleRegistry *o, const char *name,
                            Port input_port, Port output_port,
                            CnetOracleFn fn, void *ctx) {
    size_t i;
    if (!o || !fn || !acquire_name_is_atom(name)) return -1;
    if (o->count >= ACQUIRE_MAX_ORACLES) return -1;
    for (i = 0; i < o->count; ++i)
        if (strcmp(o->entries[i].name, name) == 0) return -1;
    memset(&o->entries[o->count], 0, sizeof o->entries[o->count]);
    snprintf(o->entries[o->count].name, ACQUIRE_NAME_MAX, "%s", name);
    o->entries[o->count].input_port = input_port;
    o->entries[o->count].output_port = output_port;
    o->entries[o->count].fn = fn;
    o->entries[o->count].ctx = ctx;
    o->count++;
    return 0;
}

int acquire_note_no_plan(AcquireLedger *l, Port input_port, Port goal_port) {
    size_t i;
    GapRecord *g;
    if (!l) return -1;
    for (i = 0; i < l->count; ++i) {
        g = &l->gaps[i];
        if (g->kind == GAP_NO_PLAN &&
            acquire_port_eq(g->input_port, input_port) &&
            acquire_port_eq(g->goal_port, goal_port)) {
            gap_rehit(g);
            return (int)i;
        }
    }
    g = ledger_push(l);
    if (!g) return -1;
    g->kind = GAP_NO_PLAN;
    g->status = GAP_OPEN;
    g->input_port = input_port;
    g->goal_port = goal_port;
    g->times_hit = 1;
    return (int)(l->count - 1);
}

/* LOW_RELIABILITY and HEALTH both mean "rebuild this unit": they coalesce on
   subject (first kind wins; the distinction is attribution, not action). */
static int note_rebuild(AcquireLedger *l, GapKind kind, const char *subject) {
    size_t i;
    GapRecord *g;
    if (!l || !acquire_name_is_atom(subject)) return -1;
    for (i = 0; i < l->count; ++i) {
        g = &l->gaps[i];
        if ((g->kind == GAP_LOW_RELIABILITY || g->kind == GAP_HEALTH) &&
            strcmp(g->subject, subject) == 0) {
            gap_rehit(g);
            return (int)i;
        }
    }
    g = ledger_push(l);
    if (!g) return -1;
    g->kind = kind;
    g->status = GAP_OPEN;
    snprintf(g->subject, ACQUIRE_NAME_MAX, "%s", subject);
    g->times_hit = 1;
    return (int)(l->count - 1);
}

int acquire_note_low_reliability(AcquireLedger *l, const char *subject,
                                 double score, double floor_used) {
    (void)score; (void)floor_used; /* recorded demand; thresholds live at the caller */
    return note_rebuild(l, GAP_LOW_RELIABILITY, subject);
}

int acquire_note_health(AcquireLedger *l, const char *subject,
                        const char *reason_atom) {
    (void)reason_atom;
    return note_rebuild(l, GAP_HEALTH, subject);
}
```

- [ ] **Step 4: Run to verify pass**

Run: `make acquire` then `cat logs/acquire.log`
Expected: section [1] all `ok:`, ends `ALL ACQUIRE TESTS PASSED`, no new warnings.

---

### Task 3: Ledger sidecar save/load (CNET_GAPS 1)

**Files:**
- Modify: `src/acquire.c` (replace save/load stubs)
- Modify: `tests/test_acquire.c` (fill section [2])

Format (text, one record per line; `-` encodes an empty string; tags/names/reasons are atoms so `%63s` scanning is safe):

```
CNET_GAPS 1
<count>
<kind> <status> <times_hit> <attempts> <in_family> <in_w> <in_c> <in_tag> <goal_family> <goal_w> <goal_c> <goal_tag> <subject> <oracle> <defer_reason>
```

- [ ] **Step 1: Write the failing tests** — replace `/* Task 3 */` in section [2]:

```c
    {
        AcquireLedger led, led2;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        const char *path = "acquire_gaps_test.txt";
        FILE *f;

        acquire_ledger_init(&led);
        acquire_note_no_plan(&led, nib, nibn);
        acquire_note_no_plan(&led, nib, nibn);           /* times_hit 2 */
        acquire_note_health(&led, "increment", "resource_anomaly");
        led.gaps[1].status = GAP_DEFERRED;
        snprintf(led.gaps[1].defer_reason, ACQUIRE_REASON_MAX, "oracle_unfit");

        check(acquire_ledger_save(&led, path) == 0, "ledger saves");

        acquire_ledger_init(&led2);
        check(acquire_ledger_load(&led2, path) == 0, "ledger loads");
        check(led2.count == 2, "record count survives");
        check(led2.gaps[0].kind == GAP_NO_PLAN && led2.gaps[0].times_hit == 2,
              "NO_PLAN record survives with counters");
        check(acquire_port_eq_public(led2.gaps[0].goal_port, nibn),
              "port signature + tag survive");
        check(led2.gaps[1].status == GAP_DEFERRED &&
              strcmp(led2.gaps[1].defer_reason, "oracle_unfit") == 0 &&
              strcmp(led2.gaps[1].subject, "increment") == 0,
              "DEFERRED status + reason + subject survive");

        /* malformed -> -1 with the ledger untouched */
        f = fopen(path, "w");
        fprintf(f, "CNET_GAPS 1\nnot_a_count\n");
        fclose(f);
        check(acquire_ledger_load(&led2, path) == -1 && led2.count == 2,
              "malformed file refused, ledger untouched");

        /* wrong magic -> -1 */
        f = fopen(path, "w");
        fprintf(f, "CNET_STATS 1\n3 4\n");
        fclose(f);
        check(acquire_ledger_load(&led2, path) == -1, "wrong magic refused");

        acquire_ledger_free(&led);
        acquire_ledger_free(&led2);
        remove(path);
    }
```

The test needs port equality; expose the internal helper in `include/acquire.h` (checking signatures is generally useful to callers):

```c
/* Exact signature equality: family, field_width, field_count AND tag. */
int acquire_port_eq_public(Port a, Port b);
```

and in `src/acquire.c` add, next to the static helper:

```c
int acquire_port_eq_public(Port a, Port b) { return acquire_port_eq(a, b); }
```

- [ ] **Step 2: Run to verify failure**

Run: `make acquire` then `cat logs/acquire.log`
Expected: `FAIL: ledger saves`.

- [ ] **Step 3: Implement save/load** in `src/acquire.c`:

```c
static const char *str_or_dash(const char *s) { return s[0] ? s : "-"; }

static void dash_to_str(char *dst, size_t cap, const char *src) {
    if (strcmp(src, "-") == 0) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

int acquire_ledger_save(const AcquireLedger *l, const char *path) {
    FILE *f;
    size_t i;
    if (!l || !path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "CNET_GAPS 1\n%lu\n", (unsigned long)l->count);
    for (i = 0; i < l->count; ++i) {
        const GapRecord *g = &l->gaps[i];
        fprintf(f, "%d %d %lu %lu %d %lu %lu %s %d %lu %lu %s %s %s %s\n",
                (int)g->kind, (int)g->status,
                (unsigned long)g->times_hit, (unsigned long)g->attempts,
                (int)g->input_port.family,
                (unsigned long)g->input_port.field_width,
                (unsigned long)g->input_port.field_count,
                str_or_dash(g->input_port.tag),
                (int)g->goal_port.family,
                (unsigned long)g->goal_port.field_width,
                (unsigned long)g->goal_port.field_count,
                str_or_dash(g->goal_port.tag),
                str_or_dash(g->subject),
                str_or_dash(g->oracle),
                str_or_dash(g->defer_reason));
    }
    fclose(f);
    return 0;
}

int acquire_ledger_load(AcquireLedger *l, const char *path) {
    FILE *f;
    unsigned long count, i;
    AcquireLedger fresh;   /* parse into a temp; swap only on full success */
    if (!l || !path) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    {
        char magic[16]; int ver;
        if (fscanf(f, "%15s %d\n", magic, &ver) != 2 ||
            strcmp(magic, "CNET_GAPS") != 0 || ver != 1) { fclose(f); return -1; }
    }
    if (fscanf(f, "%lu\n", &count) != 1) { fclose(f); return -1; }
    acquire_ledger_init(&fresh);
    for (i = 0; i < count; ++i) {
        int kind, status, in_fam, goal_fam;
        unsigned long hit, att, in_w, in_c, goal_w, goal_c;
        char in_tag[PORT_TAG_MAX], goal_tag[PORT_TAG_MAX];
        char subject[ACQUIRE_NAME_MAX], oracle[ACQUIRE_NAME_MAX];
        char reason[ACQUIRE_REASON_MAX];
        GapRecord *g;
        if (fscanf(f, "%d %d %lu %lu %d %lu %lu %31s %d %lu %lu %31s %63s %63s %63s\n",
                   &kind, &status, &hit, &att,
                   &in_fam, &in_w, &in_c, in_tag,
                   &goal_fam, &goal_w, &goal_c, goal_tag,
                   subject, oracle, reason) != 15 ||
            kind < 0 || kind > 2 || status < 0 || status > 2) {
            acquire_ledger_free(&fresh); fclose(f); return -1;
        }
        g = ledger_push(&fresh);
        if (!g) { acquire_ledger_free(&fresh); fclose(f); return -1; }
        g->kind = (GapKind)kind;
        g->status = (GapStatus)status;
        g->times_hit = hit;
        g->attempts = att;
        g->input_port.family = (PortFamily)in_fam;
        g->input_port.field_width = in_w;
        g->input_port.field_count = in_c;
        dash_to_str(g->input_port.tag, PORT_TAG_MAX, in_tag);
        g->goal_port.family = (PortFamily)goal_fam;
        g->goal_port.field_width = goal_w;
        g->goal_port.field_count = goal_c;
        dash_to_str(g->goal_port.tag, PORT_TAG_MAX, goal_tag);
        dash_to_str(g->subject, ACQUIRE_NAME_MAX, subject);
        dash_to_str(g->oracle, ACQUIRE_NAME_MAX, oracle);
        dash_to_str(g->defer_reason, ACQUIRE_REASON_MAX, reason);
    }
    fclose(f);
    /* success: replace gap records; acquired-BTN ownership is NOT touched */
    {
        size_t j;
        for (j = 0; j < l->count; ++j) {
            free(l->gaps[j].cap_inputs);
            free(l->gaps[j].cap_targets);
        }
        free(l->gaps);
        l->gaps = fresh.gaps;
        l->count = fresh.count;
        l->capacity = fresh.capacity;
    }
    return 0;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `make acquire` then `cat logs/acquire.log`
Expected: sections [1] and [2] green, `ALL ACQUIRE TESTS PASSED`.

---

### Task 4: Oracle fallback executor with exemplar capture

**Files:**
- Modify: `src/acquire.c` (replace the fallback stub)
- Modify: `tests/test_acquire.c` (fill section [3]; remove the `(void)oracle_increment;` silencer from `main`)

- [ ] **Step 1: Write the failing tests** — replace `/* Task 4 */` in section [3]:

```c
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        double in[4], out[4];
        unsigned v;
        int rc;

        registry_init(&reg);              /* EMPTY: no plan can exist */
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);

        /* no plan + no oracle -> -1, gap still recorded */
        nibble_bits(5, in);
        rc = acquire_execute_or_fallback(&reg, &led, &orc, &cfg,
                                         nib, nibn, in, 4, out, 4);
        check(rc == -1 && led.count == 1 && led.gaps[0].kind == GAP_NO_PLAN,
              "no plan + no oracle: error, gap recorded");

        /* with the oracle: answered + captured */
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);
        for (v = 0; v < 6; ++v) {
            nibble_bits(v, in);
            rc = acquire_execute_or_fallback(&reg, &led, &orc, &cfg,
                                             nib, nibn, in, 4, out, 4);
            check(rc == 0, "fallback answers");
            check(bits_nibble(out) == ((v + 1u) & 0xFu), "fallback answer correct");
        }
        check(led.count == 1, "fallback coalesces onto the same gap");
        check(led.gaps[0].cap_count == 6, "six exemplars captured");
        check(strcmp(led.gaps[0].oracle, "increment_ref") == 0,
              "gap remembers which oracle answered");
        /* captured rows are canonical pairs */
        check(bits_nibble(led.gaps[0].cap_inputs + 0 * 4) == 0 &&
              bits_nibble(led.gaps[0].cap_targets + 0 * 4) == 1,
              "captured pair is (input, oracle target)");

        acquire_ledger_free(&led);
        registry_free(&reg);
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `make acquire` then `cat logs/acquire.log`
Expected: `FAIL: no plan + no oracle: error, gap recorded` (stub returns -1 but records no gap — the check on `led.count == 1` fails).

- [ ] **Step 3: Implement** in `src/acquire.c`:

```c
static OracleEntry *find_oracle(OracleRegistry *o, Port in_p, Port goal_p) {
    size_t i;
    if (!o) return NULL;
    for (i = 0; i < o->count; ++i)
        if (acquire_port_eq(o->entries[i].input_port, in_p) &&
            acquire_port_eq(o->entries[i].output_port, goal_p))
            return &o->entries[i];
    return NULL;
}

/* Append one canonical (input, target) pair to a gap's capture buffer. */
static int gap_capture(GapRecord *g, const AcquireConfig *cfg,
                       const double *cin, const double *ctgt,
                       size_t in_total, size_t out_total) {
    if (g->cap_count >= cfg->capture_limit) return 0;   /* bounded: drop */
    if (g->cap_count == g->cap_limit) {
        size_t ncap = g->cap_limit ? g->cap_limit * 2 : 16;
        double *ni, *nt;
        if (ncap > cfg->capture_limit) ncap = cfg->capture_limit;
        ni = realloc(g->cap_inputs, ncap * in_total * sizeof *ni);
        if (!ni) return -1;
        g->cap_inputs = ni;
        nt = realloc(g->cap_targets, ncap * out_total * sizeof *nt);
        if (!nt) return -1;
        g->cap_targets = nt;
        g->cap_limit = ncap;
    }
    memcpy(g->cap_inputs + g->cap_count * in_total, cin,
           in_total * sizeof *cin);
    memcpy(g->cap_targets + g->cap_count * out_total, ctgt,
           out_total * sizeof *ctgt);
    g->cap_count++;
    return 0;
}

int acquire_execute_or_fallback(PrimitiveRegistry *reg, AcquireLedger *l,
                                OracleRegistry *oracles,
                                const AcquireConfig *cfg,
                                Port input_port, Port goal_port,
                                const double *input, size_t in_len,
                                double *output, size_t out_cap) {
    RoutePlan plan;
    size_t in_total = port_total(input_port);
    size_t out_total = port_total(goal_port);
    double cin[64], raw[64], ctgt[64];
    OracleEntry *o;
    int gap_idx;

    if (!reg || !l || !cfg || !input || !output) return -1;
    if (in_len != in_total || out_cap < out_total) return -1;
    if (in_total > 64 || out_total > 64) return -1;  /* v1 stack bound */

    if (route_plan(reg, input_port, goal_port, &plan) == 0) {
        plan.strict = 1;
        return route_execute(&plan, input, in_len, output, out_cap);
    }

    gap_idx = acquire_note_no_plan(l, input_port, goal_port);
    if (gap_idx < 0) return -1;

    o = find_oracle(oracles, input_port, goal_port);
    if (!o) return -1;

    /* validate-then-canonicalize on BOTH sides of the oracle boundary */
    if (!port_validate(input_port, input)) return -1;
    if (port_canonicalize(input_port, input, cin) != 0) return -1;
    o->calls++;
    if (o->fn(cin, raw, o->ctx) != 0) { o->rejects++; return -1; }
    if (!port_validate(goal_port, raw)) { o->rejects++; return -1; }
    if (port_canonicalize(goal_port, raw, ctgt) != 0) return -1;
    memcpy(output, ctgt, out_total * sizeof *ctgt);

    snprintf(l->gaps[gap_idx].oracle, ACQUIRE_NAME_MAX, "%s", o->name);
    gap_capture(&l->gaps[gap_idx], cfg, cin, ctgt, in_total, out_total);
    return 0;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `make acquire` then `cat logs/acquire.log`
Expected: sections [1]–[3] green.

---

### Task 5: The drain — NO_PLAN acquisition (mine → gate → train → certify → seal → register → replan)

**Files:**
- Modify: `src/acquire.c` (replace the drain stub; this is the core of the milestone)
- Modify: `tests/test_acquire.c` (fill section [4])

- [ ] **Step 1: Write the failing headline test** — replace `/* Task 5 */` in section [4]:

```c
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        RoutePlan plan;
        double in[4], out[4];
        unsigned v;
        char cnu_path[256];

        registry_init(&reg);              /* increment WITHHELD */
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        cfg.unit_dir = ".";               /* seal into cwd; cleaned below */
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        check(route_plan(&reg, nib, nibn, &plan) == -1,
              "planner genuinely fails before acquisition");
        acquire_note_no_plan(&led, nib, nibn);

        memset(&rep, 0, sizeof rep);
        check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0, "drain runs");
        check(rep.examined == 1 && rep.closed == 1 && rep.deferred == 0,
              "drain closed the gap");
        check(led.gaps[0].status == GAP_CLOSED, "gap is CLOSED");
        check(rep.last_verdict == CERT_PROVEN,
              "16-point domain fully enumerated -> PROOF");
        check(reg.count == 1 && reg.entries[0].certified == 1 &&
              reg.entries[0].state == PRIM_FROZEN,
              "acquired unit registered certified FROZEN");

        /* the router now plans through it */
        check(route_plan(&reg, nib, nibn, &plan) == 0 && plan.length == 1,
              "replan finds the acquired unit");
        plan.strict = 1;
        for (v = 0; v < 16; ++v) {
            nibble_bits(v, in);
            check(route_execute(&plan, in, 4, out, 4) == 0 &&
                  bits_nibble(out) == ((v + 1u) & 0xFu),
                  "strict execution correct");
        }

        /* sealed unit round-trips (seal verified) */
        snprintf(cnu_path, sizeof cnu_path, "./%s.cnu", rep.last_unit_name);
        {
            BinaryTransformNetwork rbtn;
            Contract rc;
            check(unit_load(&rbtn, &rc, cnu_path) == 0 && rc.seal_verified == 1,
                  ".cnu exists and its seal verifies");
            check(btn_certify(&rbtn, &rc, NULL) == 0,
                  "reloaded unit still certifies its own contract");
            btn_free(&rbtn);
            contract_free(&rc);
        }
        remove(cnu_path);

        acquire_ledger_free(&led);       /* frees the acquired BTN */
        registry_free(&reg);
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `make acquire` then `cat logs/acquire.log`
Expected: `FAIL: drain runs` (stub returns -1).

- [ ] **Step 3: Implement the drain** in `src/acquire.c`. The whole per-gap attempt is one function so the DEFER path has a single cleanup point:

```c
static void gap_defer(GapRecord *g, AcquireReport *rep, const char *reason) {
    g->status = GAP_DEFERRED;
    snprintf(g->defer_reason, ACQUIRE_REASON_MAX, "%s", reason);
    if (rep) {
        rep->deferred++;
        snprintf(rep->last_defer_reason, ACQUIRE_REASON_MAX, "%s", reason);
    }
}

/* Track a minted BTN (and its name) as ledger-owned. */
static int ledger_own_btn(AcquireLedger *l, BinaryTransformNetwork *btn,
                          const char *name) {
    if (l->acquired_count == l->acquired_capacity) {
        size_t ncap = l->acquired_capacity ? l->acquired_capacity * 2 : 4;
        BinaryTransformNetwork **nb =
            realloc(l->acquired, ncap * sizeof *nb);
        char (*nn)[ACQUIRE_NAME_MAX];
        if (!nb) return -1;
        l->acquired = nb;
        nn = realloc(l->acquired_names, ncap * sizeof *nn);
        if (!nn) return -1;
        l->acquired_names = nn;
        l->acquired_capacity = ncap;
    }
    l->acquired[l->acquired_count] = btn;
    snprintf(l->acquired_names[l->acquired_count], ACQUIRE_NAME_MAX, "%s", name);
    l->acquired_count++;
    return 0;
}

/* Mine the (in -> goal) exemplar table from the oracle.
   Enumerable within budget -> full enumeration (exhaustive=1);
   enumerable over budget  -> deterministic stride sample;
   non-enumerable          -> -1 ("unbounded_domain").
   Rows failing the oracle or port_validate are skipped and counted on the
   oracle entry. Returns usable row count via *n_out (tables are malloc'd,
   caller frees), attempts via *attempts_out. */
static int mine_from_oracle(OracleEntry *o, Port in_p, Port goal_p,
                            const AcquireConfig *cfg,
                            double **inputs_out, double **targets_out,
                            size_t *n_out, size_t *attempts_out,
                            int *exhaustive_out) {
    Contract tc;                 /* temp: ports only, for domain enumeration */
    size_t card, n_points, k, usable = 0;
    size_t in_total = port_total(in_p), out_total = port_total(goal_p);
    double *inputs, *targets;
    double raw[64];

    memset(&tc, 0, sizeof tc);
    snprintf(tc.name, CONTRACT_NAME_MAX, "acq_tmp");
    tc.input_ports[0] = in_p;  tc.input_port_count = 1;
    tc.output_ports[0] = goal_p; tc.output_port_count = 1;

    if (!contract_domain_cardinality(&tc, &card)) return -1; /* unbounded */

    *exhaustive_out = (card <= cfg->mine_budget);
    n_points = *exhaustive_out ? card
             : (cfg->sample_count < card ? cfg->sample_count : card);

    inputs  = malloc(n_points * in_total * sizeof *inputs);
    targets = malloc(n_points * out_total * sizeof *targets);
    if (!inputs || !targets) { free(inputs); free(targets); return -1; }

    for (k = 0; k < n_points; ++k) {
        size_t idx = *exhaustive_out ? k : (k * card) / n_points; /* stride */
        double *irow = inputs + usable * in_total;
        double *trow = targets + usable * out_total;
        if (contract_encode_domain_point(&tc, idx, irow) != 0) continue;
        o->calls++;
        if (o->fn(irow, raw, o->ctx) != 0) { o->rejects++; continue; }
        if (!port_validate(goal_p, raw))  { o->rejects++; continue; }
        if (port_canonicalize(goal_p, raw, trow) != 0) continue;
        usable++;
    }
    *inputs_out = inputs;
    *targets_out = targets;
    *n_out = usable;
    *attempts_out = n_points;
    return 0;
}

/* One acquisition attempt for one OPEN gap with a matched oracle.
   All behavioral checks run BEFORE any state change; the only
   post-registration step is a read-only replan (rollback on failure =
   registry_remove_last + remove the sealed file). */
static int attempt_no_plan(PrimitiveRegistry *reg, AcquireLedger *l,
                           OracleEntry *o, GapRecord *g,
                           const AcquireConfig *cfg, AcquireReport *rep) {
    size_t in_total = port_total(g->input_port);
    size_t out_total = port_total(g->goal_port);
    double *inputs = NULL, *targets = NULL;
    size_t usable = 0, attempts = 0, n_train;
    int exhaustive = 0;
    BinaryTransformNetwork *btn = NULL;
    Contract c;
    ExhaustiveReport ex;
    char name[ACQUIRE_NAME_MAX];
    char cnu_path[512];
    int sealed = 0;

    g->attempts++;
    snprintf(g->oracle, ACQUIRE_NAME_MAX, "%s", o->name);

    /* candidate name: acq_<goal tag>, falling back to a counter */
    if (g->goal_port.tag[0])
        snprintf(name, sizeof name, "acq_%s", g->goal_port.tag);
    else
        snprintf(name, sizeof name, "acq_gap%lu",
                 (unsigned long)(g - l->gaps));

    /* 1. mine */
    if (mine_from_oracle(o, g->input_port, g->goal_port, cfg,
                         &inputs, &targets, &usable, &attempts,
                         &exhaustive) != 0) {
        gap_defer(g, rep, "unbounded_domain");
        return -1;
    }

    /* merge captured pairs (sampled mode only; exhaustive already covers them) */
    if (!exhaustive && g->cap_count > 0) {
        size_t i, j;
        double *ni = realloc(inputs, (usable + g->cap_count) * in_total * sizeof *ni);
        double *nt = ni ? realloc(targets, (usable + g->cap_count) * out_total * sizeof *nt) : NULL;
        if (ni) inputs = ni;
        if (nt) targets = nt;
        if (ni && nt) {
            for (i = 0; i < g->cap_count; ++i) {
                int dup = 0;
                const double *cin = g->cap_inputs + i * in_total;
                for (j = 0; j < usable && !dup; ++j)
                    dup = (memcmp(inputs + j * in_total, cin,
                                  in_total * sizeof *cin) == 0);
                if (dup) continue;
                memcpy(inputs + usable * in_total, cin, in_total * sizeof *cin);
                memcpy(targets + usable * out_total,
                       g->cap_targets + i * out_total,
                       out_total * sizeof *targets);
                usable++;
            }
        }
    }

    /* 2. oracle-evidence gate (the acquisition analog of EVIDENCE_CLEAR) */
    if (usable < cfg->min_evidence) {
        free(inputs); free(targets);
        gap_defer(g, rep, "insufficient_exemplars");
        return -1;
    }
    if ((double)usable / (double)(attempts ? attempts : 1) <
        cfg->evidence_threshold) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit");
        return -1;
    }

    /* class balance (sampled mode heuristic): >= 2 distinct target rows */
    if (!exhaustive) {
        size_t i, distinct = 1;
        for (i = 1; i < usable && distinct < 2; ++i)
            if (memcmp(targets, targets + i * out_total,
                       out_total * sizeof *targets) != 0) distinct = 2;
        if (distinct < 2) {
            free(inputs); free(targets);
            gap_defer(g, rep, "class_imbalance");
            return -1;
        }
    }

    /* 3. train candidate (heap: the ledger will own it on success) */
    n_train = exhaustive ? usable
            : usable - (size_t)((double)usable * cfg->holdout_fraction);
    if (n_train == 0) n_train = usable;
    btn = calloc(1, sizeof *btn);
    if (!btn ||
        btn_init(btn, in_total, out_total, cfg->init_hidden, cfg->max_hidden,
                 cfg->learning_rate, cfg->seed) != 0 ||
        btn_set_ports(btn, g->input_port, g->goal_port) != 0) {
        free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed");
        return -1;
    }
    btn_train_dynamic(btn, inputs, targets, n_train, cfg->max_epochs,
                      cfg->growth_window, cfg->target_loss,
                      cfg->min_improvement);

    /* 4. certify: contract over the FULL mined table (holdout rows included:
       they were never trained on, so certification tests them) */
    if (contract_init_borrowed(&c, name, btn, inputs, targets, usable) != 0 ||
        btn_certify_exhaustive(btn, &c, cfg->exhaustive_cap, &ex) < 0 ||
        ex.verdict == CERT_REFUSED) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed");
        return -1;
    }
    if (ex.verdict != CERT_PROVEN) {
        double bound = coverage_accuracy_lower_bound(
            ex.certify.passed, ex.certify.exemplars, cfg->wilson_z);
        if (bound < cfg->min_accuracy_bound) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "accuracy_bound");
            return -1;
        }
    }

    /* 5. seal */
    if (cfg->unit_dir) {
        snprintf(cnu_path, sizeof cnu_path, "%s/%s.cnu", cfg->unit_dir, name);
        if (unit_save(btn, &c, cnu_path) != 0) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "seal_failed");
            return -1;
        }
        sealed = 1;
    }

    /* 6. register (name storage must outlive the registry: ledger-owned) */
    if (ledger_own_btn(l, btn, name) != 0 ||
        registry_add_certified(reg, btn,
                               l->acquired_names[l->acquired_count - 1],
                               &c) != 0) {
        if (sealed) remove(cnu_path);
        /* if ledger_own_btn succeeded the ledger owns btn now; else free it */
        if (l->acquired_count > 0 &&
            l->acquired[l->acquired_count - 1] == btn) {
            l->acquired_count--;
        }
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "register_refused");
        return -1;
    }

    /* 7. read-only replan check; rollback on the (unexpected) miss */
    {
        RoutePlan plan;
        if (route_plan(reg, g->input_port, g->goal_port, &plan) != 0) {
            registry_remove_last(reg);
            if (sealed) remove(cnu_path);
            l->acquired_count--;
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "replan_failed");
            return -1;
        }
    }

    free(inputs); free(targets);   /* contract borrowed them; done with both */
    g->status = GAP_CLOSED;
    if (rep) {
        rep->closed++;
        rep->last_verdict = ex.verdict;
        snprintf(rep->last_unit_name, ACQUIRE_NAME_MAX, "%s", name);
    }
    return 0;
}

int acquire_drain(PrimitiveRegistry *reg, AcquireLedger *l,
                  OracleRegistry *oracles, const AcquireConfig *cfg,
                  AcquireReport *report) {
    size_t i;
    if (!reg || !l || !cfg) return -1;
    for (i = 0; i < l->count; ++i) {
        GapRecord *g = &l->gaps[i];
        OracleEntry *o;
        if (g->status != GAP_OPEN) continue;
        if (report) report->examined++;
        if (g->kind == GAP_NO_PLAN) {
            o = find_oracle(oracles, g->input_port, g->goal_port);
            if (!o) { if (report) report->skipped_no_oracle++; continue; }
            attempt_no_plan(reg, l, o, g, cfg, report);
        } else {
            /* rebuild path lands in Task 8; leave the gap OPEN until then */
            if (report) report->skipped_no_oracle++;
        }
    }
    return 0;
}
```

Also add `#include "../include/contract/unit.h"` at the top of `src/acquire.c`.

**Implementation notes for this step (read before coding):**
- `contract_encode_domain_point` and `contract_domain_cardinality` are pure over the contract's *input ports*; the temp contract has `exemplar_count = 0`. If it turns out either refuses an empty exemplar table (read `src/contract/coverage.c` to confirm), write a local single-port enumerator instead: ONEHOT → index k sets field value `k % field_width` per field; BINARY → k's bits. The test (16-point BINARY_MSB domain, exhaustive PROOF verdict) pins the required behavior either way.
- `btn_train_dynamic` on the 16-exemplar increment table with the config defaults must reach certifiable exactness — the original hex/increment demo trains this exact function. If certification fails in practice, raise `max_epochs` (or `init_hidden`) in `acquire_config_defaults` until the headline test is green; the defaults are then the recipe.
- Fresh reliability counters: `btn_init` zeroes them; nothing else touches them before registration — invariant 5 (no fabricated evidence) holds by construction. Do NOT call executors during the attempt.

- [ ] **Step 4: Run to verify pass**

Run: `make acquire` then `cat logs/acquire.log`
Expected: sections [1]–[4] green, including PROOF verdict, 16/16 strict correctness, and the `.cnu` seal round-trip.

---

### Task 6: Inline acquire_now + composition test (certified end-to-end)

**Files:**
- Modify: `src/acquire.c` (replace the acquire_now stub)
- Modify: `tests/test_acquire.c` (fill sections [5] and [6])

- [ ] **Step 1: Write the failing tests** — replace `/* Task 6 */` in sections [5] and [6]:

Section [5] (inline mode — one synchronous call from no-plan to plan):

```c
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        RoutePlan plan;

        registry_init(&reg);
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);      /* unit_dir NULL: no sealing here */
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        memset(&rep, 0, sizeof rep);
        check(acquire_now(&reg, &led, &orc, &cfg, nib, nibn, &rep) == 0,
              "acquire_now closes the gap in one call");
        check(route_plan(&reg, nib, nibn, &plan) == 0,
              "plan exists after acquire_now");
        check(led.count == 1 && led.gaps[0].status == GAP_CLOSED,
              "acquire_now noted + closed its own gap");

        acquire_ledger_free(&led);
        registry_free(&reg);
    }
```

Section [6] (composition: the acquired unit composes with a pre-existing frozen one, `require_certified` end-to-end). This section needs a trained hex_value fixture — add this helper above `main` (it is the standard 16-exemplar ONEHOT→BINARY fixture used across the test suite):

```c
/* Train the hex_value companion: ONEHOT16 "hex_sym" -> BINARY_MSB4 "nibble".
   Returns a heap BTN certified against a contract built from its own
   exemplar table (caller owns btn; contract is certified then freed). */
static BinaryTransformNetwork *make_hex_value_certified(PrimitiveRegistry *reg) {
    static double inputs[16 * 16];
    static double targets[16 * 4];
    BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
    Contract c;
    Port hex = make_port(PORT_ONEHOT, 16, 1, "hex_sym");
    Port nib = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
    unsigned v;

    memset(inputs, 0, sizeof inputs);
    for (v = 0; v < 16; ++v) {
        inputs[v * 16 + v] = 1.0;
        nibble_bits(v, targets + v * 4);
    }
    if (!btn || btn_init(btn, 16, 4, 8, 64, 0.5, 7) != 0 ||
        btn_set_ports(btn, hex, nib) != 0) { free(btn); return NULL; }
    btn_train_dynamic(btn, inputs, targets, 16, 4000, 200, 1e-4, 1e-6);
    if (contract_init_borrowed(&c, "hex_value", btn, inputs, targets, 16) != 0 ||
        registry_add_certified(reg, btn, "hex_value", &c) != 0) {
        btn_free(btn); free(btn); return NULL;
    }
    return btn;
}
```

then the section body:

```c
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port hex  = make_port(PORT_ONEHOT, 16, 1, "hex_sym");
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        BinaryTransformNetwork *hexv;
        RoutePlan plan;
        double in[16], out[4];
        unsigned v;

        registry_init(&reg);
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        hexv = make_hex_value_certified(&reg);
        check(hexv != NULL, "hex_value companion trains + certifies");

        /* the 2-hop task has NO plan: the nibble->nibble_next link is missing */
        check(route_plan(&reg, hex, nibn, &plan) == -1,
              "2-hop task unplannable before acquisition");

        /* acquire the MISSING LINK (not the whole task) */
        memset(&rep, 0, sizeof rep);
        check(acquire_now(&reg, &led, &orc, &cfg, nib, nibn, &rep) == 0,
              "missing link acquired");

        /* the router composes frozen hex_value with the acquired unit,
           certified end-to-end */
        reg.require_certified = 1;
        check(route_plan(&reg, hex, nibn, &plan) == 0 && plan.length == 2,
              "router discovers the 2-hop certified composition");
        plan.strict = 1;
        for (v = 0; v < 16; ++v) {
            memset(in, 0, sizeof in);
            in[v] = 1.0;
            check(route_execute(&plan, in, 16, out, 4) == 0 &&
                  bits_nibble(out) == ((v + 1u) & 0xFu),
                  "composed strict execution correct");
        }
        reg.require_certified = 0;

        acquire_ledger_free(&led);
        registry_free(&reg);
        btn_free(hexv); free(hexv);
    }
```

- [ ] **Step 2: Run to verify failure**

Run: `make acquire` then `cat logs/acquire.log`
Expected: `FAIL: acquire_now closes the gap in one call` (stub returns -1).

- [ ] **Step 3: Implement acquire_now** in `src/acquire.c` — the same drain body scoped to one signature; no separate training logic:

```c
int acquire_now(PrimitiveRegistry *reg, AcquireLedger *l,
                OracleRegistry *oracles, const AcquireConfig *cfg,
                Port input_port, Port goal_port, AcquireReport *report) {
    int idx;
    OracleEntry *o;
    if (!reg || !l || !cfg) return -1;
    idx = acquire_note_no_plan(l, input_port, goal_port);
    if (idx < 0) return -1;
    if (l->gaps[idx].status != GAP_OPEN) /* already CLOSED by an earlier run */
        return l->gaps[idx].status == GAP_CLOSED ? 0 : -1;
    if (report) report->examined++;
    o = find_oracle(oracles, input_port, goal_port);
    if (!o) { if (report) report->skipped_no_oracle++; return -1; }
    return attempt_no_plan(reg, l, o, &l->gaps[idx], cfg, report);
}
```

- [ ] **Step 4: Run to verify pass**

Run: `make acquire` then `cat logs/acquire.log`
Expected: sections [1]–[6] green. Section [6]'s 2-hop composition is the "router composes acquired + frozen, certified end-to-end" acceptance criterion.

---

### Task 7: DEFER totality + Δ-4 counter hygiene

**Files:**
- Modify: `tests/test_acquire.c` (fill section [7]; no src changes expected — this task PROVES properties the Task-5 structure already has)

- [ ] **Step 1: Write the tests** — replace `/* Task 7 */` in section [7]:

```c
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        BinaryTransformNetwork *hexv;
        unsigned long s_before, f_before;
        size_t count_before;
        FILE *probe;

        registry_init(&reg);
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        cfg.unit_dir = ".";

        /* a bystander certified unit whose counters must not move */
        hexv = make_hex_value_certified(&reg);
        check(hexv != NULL, "bystander unit present");
        hexv->output_successes = 7;   /* nonzero so 'unchanged' is meaningful */
        hexv->output_failures = 3;
        s_before = hexv->output_successes;
        f_before = hexv->output_failures;
        count_before = reg.count;

        acquire_oracle_register(&orc, "broken_ref", nib, nibn,
                                oracle_broken, NULL);
        acquire_note_no_plan(&led, nib, nibn);

        memset(&rep, 0, sizeof rep);
        check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0, "drain runs");
        check(rep.deferred == 1 && rep.closed == 0, "drain deferred");
        check(led.gaps[0].status == GAP_DEFERRED &&
              strcmp(led.gaps[0].defer_reason, "oracle_unfit") == 0,
              "DEFERRED with reason oracle_unfit");

        /* DEFER totality */
        check(reg.count == count_before, "registry count unchanged");
        check(hexv->output_successes == s_before &&
              hexv->output_failures == f_before,
              "bystander reliability counters byte-identical (Delta-4)");
        probe = fopen("./acq_nibble_next.cnu", "r");
        check(probe == NULL, "no .cnu leaked");
        if (probe) fclose(probe);

        /* a re-hit reopens the DEFERRED gap for a future retry */
        acquire_note_no_plan(&led, nib, nibn);
        check(led.gaps[0].status == GAP_OPEN &&
              led.gaps[0].defer_reason[0] == '\0',
              "re-hit reopens a DEFERRED gap");

        acquire_ledger_free(&led);
        registry_free(&reg);
        btn_free(hexv); free(hexv);
    }
```

- [ ] **Step 2: Run — expected to pass immediately** (the Task-5 structure guarantees these properties; this section pins them as regression gates)

Run: `make acquire` then `cat logs/acquire.log`
Expected: all sections green. If `oracle_unfit` is NOT the reason (e.g. `insufficient_exemplars` fires first because zero rows validated), adjust the implementation so the validity-rate check reports `oracle_unfit` whenever `attempts > 0 && usable == 0` — a fully-rejecting oracle is "unfit", not "too few exemplars". Fix in `attempt_no_plan`: check the validity rate BEFORE the min_evidence count when `usable == 0`:

```c
    if (usable == 0 && attempts > 0) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit");
        return -1;
    }
    if (usable < cfg->min_evidence) { /* ... as before ... */ }
```

---

### Task 8: Rebuild path (LOW_RELIABILITY + HEALTH) via lifecycle RESET

**Files:**
- Modify: `src/acquire.c` (replace the rebuild `else` branch in `acquire_drain`)
- Modify: `tests/test_acquire.c` (fill section [8])

Rebuild semantics locked here (avoids the ambiguous same-name-replace path of `registry_add_certified` entirely):
1. Resolve the subject entry by name; missing → `DEFERRED unknown_subject`. Multi-port subject → `DEFERRED multi_port_unsupported` (v1).
2. Task signature = the subject's own ports (`btn->input_ports[0]` → `btn->output_ports[0]`); find an oracle for it; none → skipped (stays OPEN).
3. Mine + gate exactly as NO_PLAN (reuse `mine_from_oracle`).
4. **Incumbent health check:** certify the incumbent against the freshly mined contract. If it CERTIFIES, its behavior is fine (the counters were poisoned/stale, or the health signal was a false alarm) → `DEFERRED incumbent_healthy`, nothing changes. The honest outcome — don't churn a working unit.
5. If the incumbent FAILS the mined contract: train + certify + seal + register a replacement named `<subject>_r<attempts>` (fresh append, never same-name), then `registry_set_state(subject, PRIM_RESET)` and delete the subject's stats sidecar (`<unit_dir>/<subject>.stats`) if present — the retrainer-invalidates rule. Replan check on the subject's signature (with `lifecycle_enabled` the RESET incumbent is excluded, so the plan goes through the replacement). CLOSED.

- [ ] **Step 1: Write the failing tests** — replace `/* Task 8 */` in section [8]:

```c
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        static double inc_inputs[16 * 4], inc_targets[16 * 4];
        BinaryTransformNetwork *inc = calloc(1, sizeof *inc);
        Contract c;
        RoutePlan plan;
        double in[4], out[4];
        unsigned v;

        registry_init(&reg);
        reg.lifecycle_enabled = 1;        /* RESET-skip active */
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        /* a real certified increment */
        for (v = 0; v < 16; ++v) {
            nibble_bits(v, inc_inputs + v * 4);
            nibble_bits((v + 1u) & 0xFu, inc_targets + v * 4);
        }
        check(inc && btn_init(inc, 4, 4, 8, 64, 0.5, 11) == 0 &&
              btn_set_ports(inc, nib, nibn) == 0, "incumbent inits");
        btn_train_dynamic(inc, inc_inputs, inc_targets, 16, 4000, 200,
                          1e-4, 1e-6);
        check(contract_init_borrowed(&c, "increment", inc,
                                     inc_inputs, inc_targets, 16) == 0 &&
              registry_add_certified(&reg, inc, "increment", &c) == 0,
              "incumbent certifies + registers");

        /* Case A: healthy incumbent, poisoned counters -> incumbent_healthy */
        inc->output_successes = 0;
        inc->output_failures = 50;         /* rel ~ 0.02: 'unreliable' */
        acquire_note_low_reliability(&led, "increment", 0.02, 0.5);
        memset(&rep, 0, sizeof rep);
        acquire_drain(&reg, &led, &orc, &cfg, &rep);
        check(led.gaps[0].status == GAP_DEFERRED &&
              strcmp(led.gaps[0].defer_reason, "incumbent_healthy") == 0,
              "healthy incumbent is not churned (DEFER incumbent_healthy)");
        check(reg.entries[0].state == PRIM_FROZEN, "incumbent stays FROZEN");

        /* Case B: actually-broken incumbent (HEALTH trigger) -> rebuild */
        {   /* corrupt: flip the sign of every output-layer weight */
            size_t k, n = inc->hidden_count * 4;
            for (k = 0; k < n; ++k)
                inc->hidden_output_weights[k] = -inc->hidden_output_weights[k];
        }
        acquire_note_health(&led, "increment", "resource_anomaly");
        check(led.gaps[0].status == GAP_OPEN, "re-hit reopened the gap");
        memset(&rep, 0, sizeof rep);
        acquire_drain(&reg, &led, &orc, &cfg, &rep);
        check(rep.closed == 1 && led.gaps[0].status == GAP_CLOSED,
              "broken incumbent rebuilt");
        check(reg.entries[0].state == PRIM_RESET,
              "incumbent demoted to RESET");
        check(reg.count == 2 && reg.entries[1].certified == 1 &&
              strcmp(reg.entries[1].name, "increment_r2") == 0,
              "replacement registered certified under a fresh name");
        check(reg.entries[1].btn->output_successes == 0 &&
              reg.entries[1].btn->output_failures == 0,
              "replacement starts with fresh counters (no fabricated evidence)");

        /* the router routes around the RESET incumbent */
        check(route_plan(&reg, nib, nibn, &plan) == 0 && plan.length == 1,
              "replan succeeds");
        plan.strict = 1;
        for (v = 0; v < 16; ++v) {
            nibble_bits(v, in);
            check(route_execute(&plan, in, 4, out, 4) == 0 &&
                  bits_nibble(out) == ((v + 1u) & 0xFu),
                  "rebuilt behavior correct");
        }

        acquire_ledger_free(&led);
        registry_free(&reg);
        btn_free(inc); free(inc);
    }
```

(Note `increment_r2`: Case A already consumed attempt 1 on this gap record; the Case-B rebuild is attempt 2. If the implementation numbers replacements differently, pin whatever it produces — the assertion that matters is *fresh name, not same-name replace*.)

- [ ] **Step 2: Run to verify failure**

Run: `make acquire` then `cat logs/acquire.log`
Expected: `FAIL: healthy incumbent is not churned...` (the rebuild branch currently just skips).

- [ ] **Step 3: Implement** — replace the `else` branch in `acquire_drain` with a call to this new function:

```c
static RegistryEntry *find_entry(PrimitiveRegistry *reg, const char *name) {
    size_t i;
    for (i = 0; i < reg->count; ++i)
        if (strcmp(reg->entries[i].name, name) == 0) return &reg->entries[i];
    return NULL;
}

static int attempt_rebuild(PrimitiveRegistry *reg, AcquireLedger *l,
                           OracleRegistry *oracles, GapRecord *g,
                           const AcquireConfig *cfg, AcquireReport *rep) {
    RegistryEntry *e = find_entry(reg, g->subject);
    OracleEntry *o;
    Port in_p, goal_p;
    double *inputs = NULL, *targets = NULL;
    size_t usable = 0, attempts = 0;
    int exhaustive = 0;
    Contract mined;
    char name[ACQUIRE_NAME_MAX];
    char cnu_path[512];
    int sealed = 0;
    BinaryTransformNetwork *btn = NULL;
    ExhaustiveReport ex;

    g->attempts++;
    if (!e || !e->btn) { gap_defer(g, rep, "unknown_subject"); return -1; }
    if (e->btn->input_port_count != 1 || e->btn->output_port_count != 1) {
        gap_defer(g, rep, "multi_port_unsupported"); return -1;
    }
    in_p = e->btn->input_ports[0];
    goal_p = e->btn->output_ports[0];

    o = find_oracle(oracles, in_p, goal_p);
    if (!o) { if (rep) rep->skipped_no_oracle++; return -1; } /* stays OPEN */
    snprintf(g->oracle, ACQUIRE_NAME_MAX, "%s", o->name);

    if (mine_from_oracle(o, in_p, goal_p, cfg, &inputs, &targets,
                         &usable, &attempts, &exhaustive) != 0) {
        gap_defer(g, rep, "unbounded_domain"); return -1;
    }
    if (usable == 0 && attempts > 0) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit"); return -1;
    }
    if (usable < cfg->min_evidence) {
        free(inputs); free(targets);
        gap_defer(g, rep, "insufficient_exemplars"); return -1;
    }
    if ((double)usable / (double)attempts < cfg->evidence_threshold) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit"); return -1;
    }

    snprintf(name, sizeof name, "%s_r%lu", g->subject,
             (unsigned long)g->attempts);

    /* incumbent health check against the freshly mined truth */
    if (contract_init_borrowed(&mined, name, e->btn,
                               inputs, targets, usable) == 0 &&
        btn_certify(e->btn, &mined, NULL) == 0) {
        free(inputs); free(targets);
        gap_defer(g, rep, "incumbent_healthy");
        return -1;
    }

    /* train + certify the replacement (same recipe as attempt_no_plan) */
    btn = calloc(1, sizeof *btn);
    if (!btn ||
        btn_init(btn, port_total(in_p), port_total(goal_p),
                 cfg->init_hidden, cfg->max_hidden,
                 cfg->learning_rate, cfg->seed) != 0 ||
        btn_set_ports(btn, in_p, goal_p) != 0) {
        free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed"); return -1;
    }
    btn_train_dynamic(btn, inputs, targets, usable, cfg->max_epochs,
                      cfg->growth_window, cfg->target_loss,
                      cfg->min_improvement);
    if (contract_init_borrowed(&mined, name, btn, inputs, targets,
                               usable) != 0 ||
        btn_certify_exhaustive(btn, &mined, cfg->exhaustive_cap, &ex) < 0 ||
        ex.verdict == CERT_REFUSED) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed"); return -1;
    }
    if (ex.verdict != CERT_PROVEN &&
        coverage_accuracy_lower_bound(ex.certify.passed,
                                      ex.certify.exemplars,
                                      cfg->wilson_z) < cfg->min_accuracy_bound) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "accuracy_bound"); return -1;
    }
    if (cfg->unit_dir) {
        snprintf(cnu_path, sizeof cnu_path, "%s/%s.cnu", cfg->unit_dir, name);
        if (unit_save(btn, &mined, cnu_path) != 0) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "seal_failed"); return -1;
        }
        sealed = 1;
    }
    if (ledger_own_btn(l, btn, name) != 0 ||
        registry_add_certified(reg, btn,
                               l->acquired_names[l->acquired_count - 1],
                               &mined) != 0) {
        if (sealed) remove(cnu_path);
        if (l->acquired_count > 0 &&
            l->acquired[l->acquired_count - 1] == btn)
            l->acquired_count--;
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "register_refused"); return -1;
    }

    /* demote the incumbent + invalidate its stats sidecar */
    registry_set_state(reg, g->subject, PRIM_RESET);
    if (cfg->unit_dir) {
        char stats_path[512];
        snprintf(stats_path, sizeof stats_path, "%s/%s.stats",
                 cfg->unit_dir, g->subject);
        remove(stats_path);   /* absent file is fine */
    }

    {
        RoutePlan plan;
        if (route_plan(reg, in_p, goal_p, &plan) != 0) {
            /* roll back everything, restore the incumbent */
            registry_remove_last(reg);
            registry_set_state(reg, g->subject, PRIM_FROZEN);
            if (sealed) remove(cnu_path);
            l->acquired_count--;
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "replan_failed"); return -1;
        }
    }

    free(inputs); free(targets);
    g->status = GAP_CLOSED;
    if (rep) {
        rep->closed++;
        rep->last_verdict = ex.verdict;
        snprintf(rep->last_unit_name, ACQUIRE_NAME_MAX, "%s", name);
    }
    return 0;
}
```

and in `acquire_drain` replace the placeholder `else` branch:

```c
        } else {
            attempt_rebuild(reg, l, oracles, g, cfg, report);
        }
```

- [ ] **Step 4: Run to verify pass**

Run: `make acquire` then `cat logs/acquire.log`
Expected: all sections [1]–[8] green, `ALL ACQUIRE TESTS PASSED`.

---

### Task 9: Verify-chain wiring + benchmarks + full regression

**Files:**
- Modify: `Makefile` (append `acquire` to the `verify` target, line ~668)
- Modify: `tests/test_acquire.c` (add the per-phase benchmark print before the final PASSED line)

- [ ] **Step 1: Add benchmark reporting** — in section [4] (the headline test), wrap the drain call with `clock()` and print the graduation-slice-style phase report. Add `#include <time.h>` at the top, and around the drain:

```c
        {
            clock_t t0 = clock(), t1;
            check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0, "drain runs");
            t1 = clock();
            printf("  [bench] drain (mine+train+certify+seal+register+replan): %.1f ms, "
                   "verdict=%d, unit=%s\n",
                   1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC,
                   (int)rep.last_verdict, rep.last_unit_name);
        }
```

(Replace the existing bare `check(acquire_drain(...) == 0, "drain runs");` line in section [4] with this block. Benchmarks are printed, not gated.)

- [ ] **Step 2: Wire into verify** — edit the Makefile `verify` line (currently ending `... contract_secure contract_unit`) to end `... contract_secure contract_unit acquire`.

- [ ] **Step 3: Full regression**

Run: `make test`
Expected: the whole verify chain green INCLUDING `acquire`; `logs/acquire.log` ends `ALL ACQUIRE TESTS PASSED`. Known pre-existing failure: `legacy_test` (test_all) segfaults in the dag_plan fallback — that target is NOT in the verify chain and is not a regression from this work.

- [ ] **Step 4: Warning sweep**

Run: `make acquire 2>&1 | grep -i warn` (PowerShell: `make acquire 2>&1 | Select-String -Pattern "warn"`)
Expected: no warnings in `src/acquire.c` / `tests/test_acquire.c`.

---

## Self-review (done at plan-writing time)

**Spec coverage:** §1.1 triggers → Tasks 2 (note functions) + 8 (HEALTH/LOW_RELIABILITY drain paths); §1.2 compose-only-existing-machinery → Tasks 5/8 call only public APIs; §1.3 structural invisibility → candidates registered only in step 6 of `attempt_no_plan`, after gate+certify; §1.4 oracle fallback + capture → Task 4; §1.5 DEFER totality → Task 7; §1.6 verify-chain slice → Tasks 5 (headline) + 9 (wiring). §3.1 ledger/sidecar → Tasks 2–3; §3.6 inline mode → Task 6; §5 invariants → Δ-4 asserted in Task 7, fresh-counters asserted in Task 8, sidecar-only holds (ledger file + stats invalidation only), validate-then-canonicalize in Task 4 + mining; §6 error handling → defer-reason atoms exercised in Tasks 7 (oracle_unfit) and 8 (incumbent_healthy); the rest share the same `gap_defer` mechanism. §7 test list → sections [1]–[8] map 1:1 (test 3 inline mode = section [5], test 7 duplicate coalescing = section [1]).
**Deviations from spec, made explicit:** the gate constants live in `AcquireConfig` instead of linking `library.c` (same 0.9/16 defaults; noted in header comment); low-reliability notes take `(subject, score, floor)` rather than ports (the subject's ports are authoritative at drain time); class-balance check reduced to a ≥2-distinct-targets heuristic in sampled mode (exhaustive mode needs none — full-domain certification subsumes it).
**Type consistency:** `acquire_port_eq_public` declared in Task 3's header addition and used in section [2]; `AcquireReport.last_verdict` is `CertVerdict` (from coverage.h, included by acquire.h); `attempt_no_plan`/`attempt_rebuild` share `mine_from_oracle`, `gap_defer`, `ledger_own_btn`.
**Known risk, mitigations in place:** (1) `contract_encode_domain_point` on a zero-exemplar temp contract — Task 5 notes the local-enumerator fallback if it refuses; (2) `btn_train_dynamic` must reach exactness on the 16-point tables — config knobs are the tuning surface, test pins the requirement; (3) `RoutePlan` for a length-1 plan after acquisition assumes tag-compatible planning over one unit — this is exactly `route_demo`'s proven behavior.
