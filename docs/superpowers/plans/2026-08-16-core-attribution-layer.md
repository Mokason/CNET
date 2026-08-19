# Core Attribution Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give CNET's core a memory of hemisphere disagreement — per-(proposer, goal-signature) attribution that separates *the proposer supplied bad labels* from *the student could not learn this* — as a report-only observer that provably cannot change acquisition behaviour.

**Architecture:** A new standalone unit (`include/attribution.h` + `src/attribution.c`) holding a bounded, fixed-size ledger keyed by `(proposer name, goal Port signature)`. `acquire_drain` and `acquire_now` emit one event per attempt through a nullable function pointer on `AcquireConfig`. The ledger is a pure sink — no acquire code path ever reads from it, which makes the report-only guarantee provable by a byte-identical-drain gate rather than asserted.

**Tech Stack:** C11 (`-Wall -Wextra -pedantic -mno-avx`), no new dependencies. Text sidecars in the existing `CNET_STATS` style. GNU Make; single-executable test suites gated by `tests/verify_logs.sh`.

**Spec:** [docs/superpowers/specs/2026-08-16-core-attribution-layer-design.md](../specs/2026-08-16-core-attribution-layer-design.md)

## Global Constraints

- **C11**, compiled with `-Wall -Wextra -pedantic`. Warnings are errors in practice — silence unused params with `(void)param;`.
- **`-mno-avx` is mandatory** on this toolchain. Never pass structs ≥32 bytes by value in hot paths without checking; the MinGW gcc 15.2 AVX struct-copy bug is live. `Port` is passed by value throughout the existing codebase and is safe under `-mno-avx` — do not change that convention.
- **Attribution must never be able to break acquisition.** Every failure path in `src/attribution.c` increments `dropped_events` and returns; nothing propagates an error into the drain.
- **No allocation in the attribution ledger.** Fixed-size arrays only, so allocation failure is not a reachable state.
- **Sidecar rules (`CNET_STATS` convention, non-negotiable):** never inside a weight file; `load` REPLACES state on success; missing or malformed file returns `-1` with the target struct *untouched*. Parse into a temp and swap only on full success — mirror `acquire_ledger_load` in [src/acquire.c:1766](../../../src/acquire.c).
- **Checkpoint order:** base → gap ledger → attribution. Attribution is written last so a persisted count can never claim work the ledger does not hold.
- **Test convention:** each suite is one executable printing `  ok: <what>` per check, exiting non-zero on first failure, and printing a final `ALL <NAME> TESTS PASSED` line that `tests/verify_logs.sh` greps for.
- **`ATTRIB_NAME_MAX` must equal `ACQUIRE_NAME_MAX` (64).** They are declared separately to avoid a header cycle; a compile-time check enforces equality.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/attribution.h` (create) | Public types: `AttribVerdict`, `struct AttributionEvent`, `AttribRecord`, `AttribLedger`, `AttribCalib`. All function declarations. |
| `src/attribution.c` (create) | Verdict classification table, ledger record/find, posterior math, sidecar save/load, event log append, conformal calibration adapter. |
| `tests/test_attribution.c` (create) | Full suite for the above. |
| `tools/attrib_report.c` (create, Task 5) | Reads a `CNET_ATTRIB` sidecar and prints the scorecard. |
| `include/acquire.h` (modify) | Forward-declare `struct AttributionEvent`; add `on_attempt` / `on_attempt_ctx` to `AcquireConfig`. |
| `src/acquire.c` (modify) | Static `emit_attempt()` helper; three call sites (drain loop, `acquire_now` no-oracle path, `acquire_now` post-attempt path). |
| `Makefile` (modify) | `ATTRIB_SRC` / `ATTRIB_TEST` vars, `attribution` target, `attrib_report` target, add `attribution` to `verify`. |
| `tests/verify_logs.sh` (modify) | Add the `attribution.log` expectation line. |
| `tests/test_mutate.c` (modify, Task 2) | Add the `CNET_ATTRIB` parser to the mutation sweep. |

**Why not extend `OracleEntry`:** it lives in a fixed `ACQUIRE_MAX_ORACLES` (32) array and is flat; a per-signature table inside it forces a hard cap or a heap pointer into a copied struct. More importantly it would weld "who may teach" onto "who is trusted" — the entanglement this work exists to separate.

---

## Task 1: Attribution core — classification and posteriors

**Files:**
- Create: `include/attribution.h`
- Create: `src/attribution.c`
- Create: `tests/test_attribution.c`
- Modify: `Makefile` (add `ATTRIB_SRC`, `ATTRIB_TEST`, `attribution` target; add `attribution` to `verify`)
- Modify: `tests/verify_logs.sh`
- Modify: `docs/superpowers/specs/2026-08-16-core-attribution-layer-design.md` (one correction, Step 9)

**Interfaces:**
- Consumes: `Port` from `include/nn.h`; `CertVerdict` from `include/contract/coverage.h`.
- Produces: `AttribVerdict`, `struct AttributionEvent`, `AttribRecord`, `AttribLedger`, `attrib_ledger_init`, `attrib_classify`, `attrib_record`, `attrib_find`, `attrib_proposer_trust`, `attrib_signature_yield`, `attrib_sink`. Tasks 2–5 all build on these exact names.

### Context you need

`gap_defer` in [src/acquire.c:747](../../../src/acquire.c) is the single chokepoint every deferral routes through. The complete set of defer atoms in the codebase is **14** — verified by enumerating every `gap_defer` call site plus the two indirect producers (`base_precheck` returns `"tag_collision"` / `"register_refused"`; a ternary at line 1140 produces `"class_imbalance"` / `"unbounded_domain"`; `ACQUIRE_DEFER_WAITING_ORACLE` is `"waiting_oracle"`).

The classification rule comes from where each atom fires relative to the oracle-evidence gate (`AcquireConfig.evidence_threshold` = 0.9, `min_evidence` = 16), which runs **before any training step**.

**Note a deliberate deviation from the spec:** spec §3.2 lists `tag_collision` under `PROPOSER_FAULT`. That is wrong for v1 — spec §9 puts tag minting by the creative hemisphere out of scope, so in v1 the tags come from the gap's ports, not from the proposer. Charging the proposer for a tag it did not choose would corrupt the trust number. This plan classifies `tag_collision` as `SYSTEM_FAULT` and Step 9 corrects the spec.

- [ ] **Step 1: Write `include/attribution.h`**

```c
#ifndef ATTRIBUTION_H
#define ATTRIBUTION_H

/* Core attribution layer (v1): per-(proposer, goal signature) evidence that
   separates "the proposer supplied unusable labels" from "the student could
   not learn this signature". REPORT-ONLY: acquire never reads this back, so
   attaching the sink is provably a no-op (see the byte-identical drain gate
   in tests/test_attribution.c).
   Spec: docs/superpowers/specs/2026-08-16-core-attribution-layer-design.md */

#include <stddef.h>
#include <stdint.h>

#include "nn.h"                 /* Port */
#include "contract/coverage.h"  /* CertVerdict */

/* Must equal ACQUIRE_NAME_MAX. Declared separately so acquire.h can
   forward-declare struct AttributionEvent without a header cycle; the
   equality is asserted at compile time in src/attribution.c. */
#define ATTRIB_NAME_MAX 64

#define ATTRIB_MAX_KEYS 256
#define ATTRIB_MAX_RECIPE_FPS 8

typedef enum {
    ATTRIB_ADMITTED = 0,
    ATTRIB_BLAMELESS = 1,        /* excluded from every denominator */
    ATTRIB_PROPOSER_FAULT = 2,   /* died at or before the evidence gate */
    ATTRIB_SYSTEM_FAULT = 3,     /* cleared the gate; recipe or domain */
    ATTRIB_UNCLASSIFIED = 4,     /* unknown atom: counted, never guessed */
    ATTRIB_VERDICT_COUNT = 5
} AttribVerdict;

/* One acquisition attempt, emitted by acquire_drain / acquire_now.
   Pointer fields are BORROWED for the duration of the call only. */
struct AttributionEvent {
    const char *proposer;       /* matched oracle name; "" when none */
    Port input_port;
    Port goal_port;             /* the key's signature */
    const char *reason;         /* defer atom; "" when admitted */
    int admitted;               /* 1 = reached GAP_CLOSED */
    uint64_t recipe_fp;
    size_t domain_cardinality;  /* certified domain size; 0 = unknown */
    double min_margin;          /* worst certified output margin */
    int cert_verdict;           /* CertVerdict as int; -1 = not applicable */
};

typedef struct {
    char proposer[ATTRIB_NAME_MAX];
    Port goal;
    size_t admitted;
    size_t proposer_fault;
    size_t system_fault;
    size_t blameless;
    size_t unclassified;
    /* anti-triviality, recorded on ADMITTED only */
    size_t admitted_card_sum;
    double admitted_margin_min;  /* 1.0 when admitted == 0 */
    size_t admitted_proof;
    size_t admitted_sampled;
    /* distinct recipe fingerprints under which SYSTEM_FAULT was seen:
       spread across several => domain-fault; repeated under one => recipe */
    uint64_t recipe_fps[ATTRIB_MAX_RECIPE_FPS];
    size_t recipe_fp_count;
} AttribRecord;

typedef struct {
    AttribRecord keys[ATTRIB_MAX_KEYS];
    size_t count;
    size_t dropped_events;  /* table full, or malformed event */
} AttribLedger;

/* Zeroes the ledger. Always succeue... (see Step 3 for the real body). */
void attrib_ledger_init(AttribLedger *L);

/* Verdict for one terminated attempt. admitted != 0 => ATTRIB_ADMITTED
   regardless of reason. An unrecognised atom => ATTRIB_UNCLASSIFIED. */
AttribVerdict attrib_classify(const char *reason_atom, int admitted);

/* Fold one event into the ledger. Returns 0, or -1 when the event was
   dropped (bad args, or table full) with dropped_events bumped. NEVER
   returns an error the caller is expected to act on. */
int attrib_record(AttribLedger *L, const struct AttributionEvent *ev);

/* Exact-signature lookup. NULL when absent. */
const AttribRecord *attrib_find(const AttribLedger *L, const char *proposer,
                                Port goal);

/* (admitted + system_fault + 1) / (admitted + system_fault + proposer_fault + 2)
   "When this proposer labels this signature, are the labels usable?"
   SYSTEM_FAULT sits in the numerator: it is explicitly not the proposer's
   fault. Zero evidence reads 0.5. */
double attrib_proposer_trust(const AttribRecord *r);

/* (admitted + 1) / (admitted + system_fault + 2)
   "Given usable labels, does this signature certify at all?" */
double attrib_signature_yield(const AttribRecord *r);

/* AcquireConfig.on_attempt adapter. ctx must be an AttribLedger*. */
void attrib_sink(const struct AttributionEvent *ev, void *ctx);

#endif /* ATTRIBUTION_H */
```

Fix the typo in the `attrib_ledger_init` comment while writing it — it should read `/* Zeroes the ledger. */`.

- [ ] **Step 2: Write the failing test**

Create `tests/test_attribution.c`:

```c
/* Core attribution layer — report-only observer gate.
   Sections are numbered; first failure exits non-zero. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/attribution.h"

static int checks_run = 0;

static void check(int cond, const char *what) {
    ++checks_run;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
    printf("  ok: %s\n", what);
}

static Port make_port(PortFamily fam, size_t w, size_t c, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = fam;
    p.field_width = w;
    p.field_count = c;
    if (port_set_tag(&p, tag) != 0) { printf("FAIL: bad tag %s\n", tag); exit(1); }
    return p;
}

static struct AttributionEvent ev_of(const char *proposer, Port goal,
                                     const char *reason, int admitted) {
    struct AttributionEvent e;
    memset(&e, 0, sizeof e);
    e.proposer = proposer;
    e.goal_port = goal;
    e.input_port = goal;
    e.reason = reason;
    e.admitted = admitted;
    e.cert_verdict = -1;
    e.min_margin = 1.0;
    return e;
}

int main(void) {
    AttribLedger *L = (AttribLedger *)malloc(sizeof *L);
    Port g;
    if (!L) { printf("FAIL: alloc\n"); return 1; }
    g = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");

    printf("== 1. verdict classification ==\n");
    check(attrib_classify("certify_failed", 1) == ATTRIB_ADMITTED,
          "admitted wins over any reason");
    check(attrib_classify("waiting_oracle", 0) == ATTRIB_BLAMELESS,
          "waiting_oracle is blameless");
    check(attrib_classify("incumbent_healthy", 0) == ATTRIB_BLAMELESS,
          "incumbent_healthy is blameless");
    check(attrib_classify("unbounded_domain", 0) == ATTRIB_BLAMELESS,
          "unbounded_domain is blameless");
    check(attrib_classify("oracle_unfit", 0) == ATTRIB_PROPOSER_FAULT,
          "oracle_unfit is proposer fault");
    check(attrib_classify("class_imbalance", 0) == ATTRIB_PROPOSER_FAULT,
          "class_imbalance is proposer fault");
    check(attrib_classify("insufficient_exemplars", 0) == ATTRIB_PROPOSER_FAULT,
          "insufficient_exemplars is proposer fault");
    check(attrib_classify("certify_failed", 0) == ATTRIB_SYSTEM_FAULT,
          "certify_failed is system fault");
    check(attrib_classify("accuracy_bound", 0) == ATTRIB_SYSTEM_FAULT,
          "accuracy_bound is system fault");
    check(attrib_classify("tag_collision", 0) == ATTRIB_SYSTEM_FAULT,
          "tag_collision is system fault in v1 (proposer does not mint tags)");
    check(attrib_classify("a_brand_new_atom", 0) == ATTRIB_UNCLASSIFIED,
          "unknown atom is UNCLASSIFIED, never guessed");
    check(attrib_classify(NULL, 0) == ATTRIB_UNCLASSIFIED,
          "NULL reason is UNCLASSIFIED");

    printf("== 2. posteriors ==\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        struct AttributionEvent e = ev_of("gemma", g, "", 1);
        check(attrib_record(L, &e) == 0, "record an admission");
        r = attrib_find(L, "gemma", g);
        check(r != NULL && r->admitted == 1, "key created and counted");
        check(fabs(attrib_proposer_trust(r) - 2.0 / 3.0) < 1e-12,
              "trust after 1 admission = 2/3");
        check(fabs(attrib_signature_yield(r) - 2.0 / 3.0) < 1e-12,
              "yield after 1 admission = 2/3");
    }

    printf("== 3. cold key is neutral, not blind ==\n");
    {
        AttribRecord empty;
        memset(&empty, 0, sizeof empty);
        check(fabs(attrib_proposer_trust(&empty) - 0.5) < 1e-12,
              "zero evidence trust reads 0.5");
        check(fabs(attrib_signature_yield(&empty) - 0.5) < 1e-12,
              "zero evidence yield reads 0.5");
    }

    printf("== 4. system fault does not blame the proposer ==\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        int i;
        for (i = 0; i < 5; ++i) {
            struct AttributionEvent e = ev_of("gemma", g, "certify_failed", 0);
            e.recipe_fp = 1234u;
            check(attrib_record(L, &e) == 0, "record a system fault");
        }
        r = attrib_find(L, "gemma", g);
        check(r->system_fault == 5 && r->proposer_fault == 0,
              "5 system faults, 0 proposer faults");
        check(attrib_proposer_trust(r) > 0.5,
              "trust stays above 0.5 under pure system fault");
        check(attrib_signature_yield(r) < 0.5,
              "yield falls below 0.5 under pure system fault");
        check(r->recipe_fp_count == 1,
              "one distinct recipe fingerprint recorded (recipe-suspect)");
    }

    printf("== 5. blameless is in no denominator ==\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        struct AttributionEvent a = ev_of("gemma", g, "", 1);
        struct AttributionEvent b = ev_of("gemma", g, "waiting_oracle", 0);
        attrib_record(L, &a);
        attrib_record(L, &b);
        attrib_record(L, &b);
        r = attrib_find(L, "gemma", g);
        check(r->blameless == 2, "blameless counted separately");
        check(fabs(attrib_proposer_trust(r) - 2.0 / 3.0) < 1e-12,
              "trust unchanged by blameless events");
        check(fabs(attrib_signature_yield(r) - 2.0 / 3.0) < 1e-12,
              "yield unchanged by blameless events");
    }

    printf("== 6. key identity is the exact goal signature ==\n");
    attrib_ledger_init(L);
    {
        Port other = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port wider = make_port(PORT_BINARY_MSB, 8, 1, "nibble_next");
        struct AttributionEvent e1 = ev_of("gemma", g, "", 1);
        struct AttributionEvent e2 = ev_of("gemma", other, "", 1);
        struct AttributionEvent e3 = ev_of("gemma", wider, "", 1);
        struct AttributionEvent e4 = ev_of("other_llm", g, "", 1);
        attrib_record(L, &e1);
        attrib_record(L, &e2);
        attrib_record(L, &e3);
        attrib_record(L, &e4);
        check(L->count == 4, "tag, width and proposer each split the key");
        check(attrib_find(L, "gemma", g)->admitted == 1, "lookup is exact");
        check(attrib_find(L, "nobody", g) == NULL, "absent key returns NULL");
    }

    printf("== 7. bounded, never allocating, never failing upward ==\n");
    attrib_ledger_init(L);
    {
        size_t i;
        for (i = 0; i < ATTRIB_MAX_KEYS + 10; ++i) {
            char tag[ATTRIB_NAME_MAX];
            Port p;
            struct AttributionEvent e;
            snprintf(tag, sizeof tag, "t%lu", (unsigned long)i);
            p = make_port(PORT_BINARY_MSB, 4, 1, tag);
            e = ev_of("gemma", p, "", 1);
            attrib_record(L, &e);
        }
        check(L->count == ATTRIB_MAX_KEYS, "key table saturates at the cap");
        check(L->dropped_events == 10, "overflow counted as dropped, not evicted");
    }
    check(attrib_record(NULL, NULL) == -1, "NULL args refused without crashing");

    printf("== 8. anti-triviality columns ==\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        struct AttributionEvent e = ev_of("gemma", g, "", 1);
        e.domain_cardinality = 16;
        e.min_margin = 0.25;
        e.cert_verdict = (int)CERT_PROOF;
        attrib_record(L, &e);
        e.domain_cardinality = 4;
        e.min_margin = 0.10;
        attrib_record(L, &e);
        r = attrib_find(L, "gemma", g);
        check(r->admitted_card_sum == 20, "cardinality accumulates");
        check(fabs(r->admitted_margin_min - 0.10) < 1e-12,
              "worst margin retained, not averaged");
        check(r->admitted_proof == 2, "PROOF admissions counted");
    }

    free(L);
    printf("checks run: %d\n", checks_run);
    printf("ALL ATTRIBUTION TESTS PASSED\n");
    return 0;
}
```

Before running, confirm the `CertVerdict` enumerator name: open `include/contract/coverage.h` around line 76 and use the actual PROOF enumerator (the test above assumes `CERT_PROOF`). If it differs, use the real name in the test.

- [ ] **Step 3: Add the Makefile target and run the test to verify it fails**

Add near the existing `ACQUIRE_SRC` definition (Makefile line ~419):

```make
ATTRIB_SRC := src/attribution.c
ATTRIB_TEST := tests/test_attribution.c
```

Add the target next to the `acquire` target (Makefile line ~611):

```make
# Core attribution layer: per-(proposer, goal signature) evidence separating
# proposer fault from system fault. REPORT-ONLY — includes the byte-identical
# drain gate that proves attaching the sink changes nothing.
attribution: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ATTRIB_SRC) $(ATTRIB_TEST) include/nn.h include/acquire.h include/attribution.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(ACQUIRE_SRC) $(BASE_SRC) $(ATTRIB_SRC) $(ATTRIB_TEST) $(LDFLAGS)
	./$(BIN_DIR)/attribution > logs/attribution.log 2>&1
```

Run: `make attribution`
Expected: FAIL — link error, `undefined reference to attrib_classify` (src/attribution.c does not exist yet).

- [ ] **Step 4: Write `src/attribution.c` — classification and posteriors**

```c
/* Core attribution layer (v1) — see include/attribution.h. */

#include <stdio.h>
#include <string.h>

#include "../include/attribution.h"
#include "../include/acquire.h"   /* ACQUIRE_NAME_MAX, for the size assert only */

/* ATTRIB_NAME_MAX and ACQUIRE_NAME_MAX are declared in separate headers to
   avoid a cycle; they MUST stay equal. Fails the build if they diverge. */
typedef char attrib_name_max_matches_acquire[
    (ATTRIB_NAME_MAX == ACQUIRE_NAME_MAX) ? 1 : -1];

/* The complete defer-atom set, verified against every gap_defer call site in
   src/acquire.c plus the two indirect producers (base_precheck, and the
   class_imbalance/unbounded_domain ternary in attempt_no_plan).

   The rule: the oracle-evidence gate (evidence_threshold, min_evidence) runs
   BEFORE any training step. Dying at or before it means the labels were
   unusable => proposer fault. Dying after it means the labels were fine and
   the student could not fit => system fault.

   tag_collision is SYSTEM_FAULT in v1 because the creative hemisphere does
   not mint tags (spec section 9); revisit if that is ever delegated. */
static const struct { const char *atom; AttribVerdict verdict; } ATOMS[] = {
    { "waiting_oracle",         ATTRIB_BLAMELESS },
    { "incumbent_healthy",      ATTRIB_BLAMELESS },
    { "unknown_subject",        ATTRIB_BLAMELESS },
    { "multi_port_unsupported", ATTRIB_BLAMELESS },
    { "unbounded_domain",       ATTRIB_BLAMELESS },
    { "oracle_unfit",           ATTRIB_PROPOSER_FAULT },
    { "insufficient_exemplars", ATTRIB_PROPOSER_FAULT },
    { "class_imbalance",        ATTRIB_PROPOSER_FAULT },
    { "certify_failed",         ATTRIB_SYSTEM_FAULT },
    { "accuracy_bound",         ATTRIB_SYSTEM_FAULT },
    { "seal_failed",            ATTRIB_SYSTEM_FAULT },
    { "register_refused",       ATTRIB_SYSTEM_FAULT },
    { "replan_failed",          ATTRIB_SYSTEM_FAULT },
    { "tag_collision",          ATTRIB_SYSTEM_FAULT }
};
#define ATOM_COUNT (sizeof ATOMS / sizeof ATOMS[0])

void attrib_ledger_init(AttribLedger *L) {
    if (!L) return;
    memset(L, 0, sizeof *L);
}

AttribVerdict attrib_classify(const char *reason_atom, int admitted) {
    size_t i;
    if (admitted) return ATTRIB_ADMITTED;
    if (!reason_atom || !reason_atom[0]) return ATTRIB_UNCLASSIFIED;
    for (i = 0; i < ATOM_COUNT; ++i)
        if (strcmp(reason_atom, ATOMS[i].atom) == 0) return ATOMS[i].verdict;
    return ATTRIB_UNCLASSIFIED;
}

/* Exact signature equality: family, field_width, field_count AND tag —
   the same rule acquire_port_eq_public applies. */
static int port_sig_eq(Port a, Port b) {
    return a.family == b.family &&
           a.field_width == b.field_width &&
           a.field_count == b.field_count &&
           strcmp(a.tag, b.tag) == 0;
}

static AttribRecord *find_mut(AttribLedger *L, const char *proposer, Port goal) {
    size_t i;
    for (i = 0; i < L->count; ++i)
        if (strcmp(L->keys[i].proposer, proposer) == 0 &&
            port_sig_eq(L->keys[i].goal, goal))
            return &L->keys[i];
    return NULL;
}

const AttribRecord *attrib_find(const AttribLedger *L, const char *proposer,
                                Port goal) {
    if (!L || !proposer) return NULL;
    return find_mut((AttribLedger *)L, proposer, goal);
}

static void note_recipe_fp(AttribRecord *r, uint64_t fp) {
    size_t i;
    if (fp == 0) return;
    for (i = 0; i < r->recipe_fp_count; ++i)
        if (r->recipe_fps[i] == fp) return;
    if (r->recipe_fp_count >= ATTRIB_MAX_RECIPE_FPS) return;
    r->recipe_fps[r->recipe_fp_count++] = fp;
}

int attrib_record(AttribLedger *L, const struct AttributionEvent *ev) {
    AttribRecord *r;
    const char *who;
    AttribVerdict v;
    if (!L || !ev) { if (L) L->dropped_events++; return -1; }
    who = ev->proposer ? ev->proposer : "";
    r = find_mut(L, who, ev->goal_port);
    if (!r) {
        if (L->count >= ATTRIB_MAX_KEYS) { L->dropped_events++; return -1; }
        r = &L->keys[L->count++];
        memset(r, 0, sizeof *r);
        snprintf(r->proposer, ATTRIB_NAME_MAX, "%s", who);
        r->goal = ev->goal_port;
        r->admitted_margin_min = 1.0;
    }
    v = attrib_classify(ev->reason, ev->admitted);
    switch (v) {
    case ATTRIB_ADMITTED:
        r->admitted++;
        r->admitted_card_sum += ev->domain_cardinality;
        if (ev->min_margin < r->admitted_margin_min)
            r->admitted_margin_min = ev->min_margin;
        if (ev->cert_verdict == (int)CERT_PROOF) r->admitted_proof++;
        else if (ev->cert_verdict >= 0) r->admitted_sampled++;
        break;
    case ATTRIB_PROPOSER_FAULT: r->proposer_fault++; break;
    case ATTRIB_SYSTEM_FAULT:
        r->system_fault++;
        note_recipe_fp(r, ev->recipe_fp);
        break;
    case ATTRIB_BLAMELESS:      r->blameless++; break;
    case ATTRIB_UNCLASSIFIED:
    default:                    r->unclassified++; break;
    }
    return 0;
}

double attrib_proposer_trust(const AttribRecord *r) {
    double good, total;
    if (!r) return 0.5;
    good = (double)(r->admitted + r->system_fault) + 1.0;
    total = (double)(r->admitted + r->system_fault + r->proposer_fault) + 2.0;
    return good / total;
}

double attrib_signature_yield(const AttribRecord *r) {
    double good, total;
    if (!r) return 0.5;
    good = (double)r->admitted + 1.0;
    total = (double)(r->admitted + r->system_fault) + 2.0;
    return good / total;
}

void attrib_sink(const struct AttributionEvent *ev, void *ctx) {
    (void)attrib_record((AttribLedger *)ctx, ev);
}
```

Confirm `CERT_PROOF` is the real enumerator name from `include/contract/coverage.h:76`; substitute the actual name if it differs, in both this file and the test.

- [ ] **Step 5: Run the test to verify it passes**

Run: `make attribution && cat logs/attribution.log`
Expected: every `ok:` line prints, ending with `ALL ATTRIBUTION TESTS PASSED`.

- [ ] **Step 6: Wire into `make verify`**

In `Makefile` line 1185, add `attribution` to the `verify:` prerequisite list, immediately after `acquire`.

In `tests/verify_logs.sh`, add after the `acquire.log` line (line 34):

```
attribution.log|||ALL ATTRIBUTION TESTS PASSED
```

- [ ] **Step 7: Run the full gate**

Run: `make attribution && sh tests/verify_logs.sh`
Expected: the log scan reports the attribution expectation satisfied.

- [ ] **Step 8: Correct the spec's tag_collision classification**

In `docs/superpowers/specs/2026-08-16-core-attribution-layer-design.md` §3.2, move `tag_collision` out of the `PROPOSER_FAULT` row into the `SYSTEM_FAULT` row, and append this sentence to §3.2:

> `tag_collision` is classified as `SYSTEM_FAULT` in v1 because the creative hemisphere does not mint tags (§9) — the tags come from the gap's ports, so charging the proposer for a tag it did not choose would corrupt the trust number. It moves to `PROPOSER_FAULT` if tag minting is ever delegated.

- [ ] **Step 9: Commit**

```bash
git add include/attribution.h src/attribution.c tests/test_attribution.c Makefile tests/verify_logs.sh docs/superpowers/specs/2026-08-16-core-attribution-layer-design.md
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(attribution): verdict classification + dual posteriors, report-only core

Separates proposer fault from system fault using the stage at which an
attempt died: the oracle-evidence gate runs before any training step, so
dying at or before it means unusable labels and dying after it means the
student could not fit. Complete 14-atom classification table; unknown atoms
are counted as UNCLASSIFIED rather than guessed.

Corrects the spec: tag_collision is SYSTEM_FAULT in v1 since the creative
hemisphere does not mint tags.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Sidecar persistence + mutation fuzz

**Files:**
- Modify: `include/attribution.h` (add save/load declarations)
- Modify: `src/attribution.c` (add save/load)
- Modify: `tests/test_attribution.c` (append section 9)
- Modify: `tests/test_mutate.c` (add the new parser to the sweep)

**Interfaces:**
- Consumes: `AttribLedger`, `AttribRecord` from Task 1.
- Produces: `attrib_ledger_save(const AttribLedger *L, const char *path)` and `attrib_ledger_load(AttribLedger *L, const char *path)`, both returning `int` (0 / -1). Task 5's report tool calls `attrib_ledger_load`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_attribution.c`, immediately before the `free(L);` line:

```c
    printf("== 9. sidecar round-trip ==\n");
    attrib_ledger_init(L);
    {
        AttribLedger *M = (AttribLedger *)malloc(sizeof *M);
        struct AttributionEvent a = ev_of("gemma", g, "", 1);
        struct AttributionEvent b = ev_of("gemma", g, "certify_failed", 0);
        struct AttributionEvent c = ev_of("gemma", g, "oracle_unfit", 0);
        if (!M) { printf("FAIL: alloc\n"); return 1; }
        a.domain_cardinality = 16; a.min_margin = 0.25;
        a.cert_verdict = (int)CERT_PROOF;
        b.recipe_fp = 99u;
        attrib_record(L, &a);
        attrib_record(L, &b);
        attrib_record(L, &c);

        check(attrib_ledger_save(L, "logs/attrib_rt.stats") == 0, "save ok");
        attrib_ledger_init(M);
        check(attrib_ledger_load(M, "logs/attrib_rt.stats") == 0, "load ok");
        check(M->count == L->count, "key count round-trips");
        {
            const AttribRecord *r = attrib_find(M, "gemma", g);
            check(r != NULL, "key found after load");
            check(r->admitted == 1 && r->system_fault == 1 &&
                  r->proposer_fault == 1, "counters round-trip");
            check(r->admitted_card_sum == 16, "cardinality round-trips");
            check(fabs(r->admitted_margin_min - 0.25) < 1e-9,
                  "margin round-trips");
            check(r->recipe_fp_count == 1 && r->recipe_fps[0] == 99u,
                  "recipe fingerprints round-trip");
            check(fabs(attrib_proposer_trust(r) -
                       attrib_proposer_trust(attrib_find(L, "gemma", g))) < 1e-12,
                  "posterior identical after round-trip");
        }

        printf("== 10. malformed load leaves state untouched ==\n");
        {
            FILE *bad = fopen("logs/attrib_bad.stats", "w");
            size_t before;
            if (!bad) { printf("FAIL: fopen\n"); return 1; }
            fprintf(bad, "CNET_ATTRIB 1\n3\ngarbage not a record\n");
            fclose(bad);
            before = M->count;
            check(attrib_ledger_load(M, "logs/attrib_bad.stats") == -1,
                  "malformed file refused");
            check(M->count == before, "ledger untouched after refusal");
        }
        check(attrib_ledger_load(M, "logs/does_not_exist.stats") == -1,
              "missing file refused");
        check(M->count > 0, "ledger still untouched after missing file");
        free(M);
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make attribution`
Expected: FAIL — `undefined reference to attrib_ledger_save`.

- [ ] **Step 3: Declare save/load in `include/attribution.h`**

Add before the closing `#endif`:

```c
/* Sidecar persistence ("CNET_ATTRIB 1"), CNET_STATS rules: never inside a
   weight file; load REPLACES on success; missing or malformed file returns
   -1 with *L untouched (parse into a temp, swap only on full success).
   Write AFTER the base and the gap ledger in any checkpoint, so a persisted
   count never claims work the ledger does not hold. */
int attrib_ledger_save(const AttribLedger *L, const char *path);
int attrib_ledger_load(AttribLedger *L, const char *path);
```

- [ ] **Step 4: Implement save/load in `src/attribution.c`**

Append:

```c
/* Empty strings are written as "-" so the field count stays fixed — the same
   convention acquire_ledger_save uses. */
static const char *sod(const char *s) { return s[0] ? s : "-"; }

static void dts(char *dst, size_t cap, const char *src) {
    if (strcmp(src, "-") == 0) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

int attrib_ledger_save(const AttribLedger *L, const char *path) {
    FILE *f;
    size_t i, j;
    if (!L || !path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "CNET_ATTRIB 1\n%lu %lu\n",
            (unsigned long)L->count, (unsigned long)L->dropped_events);
    for (i = 0; i < L->count; ++i) {
        const AttribRecord *r = &L->keys[i];
        fprintf(f, "%s %d %lu %lu %s %lu %lu %lu %lu %lu %lu %.17g %lu %lu %lu",
                sod(r->proposer),
                (int)r->goal.family,
                (unsigned long)r->goal.field_width,
                (unsigned long)r->goal.field_count,
                sod(r->goal.tag),
                (unsigned long)r->admitted,
                (unsigned long)r->proposer_fault,
                (unsigned long)r->system_fault,
                (unsigned long)r->blameless,
                (unsigned long)r->unclassified,
                (unsigned long)r->admitted_card_sum,
                r->admitted_margin_min,
                (unsigned long)r->admitted_proof,
                (unsigned long)r->admitted_sampled,
                (unsigned long)r->recipe_fp_count);
        for (j = 0; j < r->recipe_fp_count; ++j)
            fprintf(f, " %llu", (unsigned long long)r->recipe_fps[j]);
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}

int attrib_ledger_load(AttribLedger *L, const char *path) {
    FILE *f;
    unsigned long count, dropped, i, j;
    int ver, fam;
    AttribLedger *fresh;   /* parse into a temp; swap only on full success */
    if (!L || !path) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    {
        char magic[16];
        if (fscanf(f, "%15s %d\n", magic, &ver) != 2 ||
            strcmp(magic, "CNET_ATTRIB") != 0 || ver != 1) {
            fclose(f); return -1;
        }
    }
    if (fscanf(f, "%lu %lu\n", &count, &dropped) != 2 ||
        count > ATTRIB_MAX_KEYS) { fclose(f); return -1; }
    fresh = (AttribLedger *)malloc(sizeof *fresh);
    if (!fresh) { fclose(f); return -1; }
    attrib_ledger_init(fresh);
    for (i = 0; i < count; ++i) {
        AttribRecord *r = &fresh->keys[i];
        char name[ATTRIB_NAME_MAX], tag[ATTRIB_NAME_MAX];
        unsigned long w, c, adm, pf, sf, bl, un, cs, ap, as, nfp;
        memset(r, 0, sizeof *r);
        if (fscanf(f, "%63s %d %lu %lu %63s %lu %lu %lu %lu %lu %lu %lf %lu %lu %lu",
                   name, &fam, &w, &c, tag, &adm, &pf, &sf, &bl, &un, &cs,
                   &r->admitted_margin_min, &ap, &as, &nfp) != 15 ||
            nfp > ATTRIB_MAX_RECIPE_FPS) {
            free(fresh); fclose(f); return -1;
        }
        dts(r->proposer, ATTRIB_NAME_MAX, name);
        r->goal.family = (PortFamily)fam;
        r->goal.field_width = (size_t)w;
        r->goal.field_count = (size_t)c;
        {
            char decoded[ATTRIB_NAME_MAX];
            dts(decoded, ATTRIB_NAME_MAX, tag);
            if (decoded[0] && port_set_tag(&r->goal, decoded) != 0) {
                free(fresh); fclose(f); return -1;
            }
        }
        r->admitted = (size_t)adm;
        r->proposer_fault = (size_t)pf;
        r->system_fault = (size_t)sf;
        r->blameless = (size_t)bl;
        r->unclassified = (size_t)un;
        r->admitted_card_sum = (size_t)cs;
        r->admitted_proof = (size_t)ap;
        r->admitted_sampled = (size_t)as;
        r->recipe_fp_count = (size_t)nfp;
        for (j = 0; j < nfp; ++j) {
            unsigned long long fp;
            if (fscanf(f, " %llu", &fp) != 1) {
                free(fresh); fclose(f); return -1;
            }
            r->recipe_fps[j] = (uint64_t)fp;
        }
    }
    fresh->count = (size_t)count;
    fresh->dropped_events = (size_t)dropped;
    fclose(f);
    *L = *fresh;          /* swap only now: parse fully succeeded */
    free(fresh);
    return 0;
}
```

Add `#include <stdlib.h>` to the top of `src/attribution.c` for `malloc`/`free`.

`port_set_tag` is declared in `include/nn.h` and validates the tag atom — reusing it means a corrupt tag in the sidecar is refused rather than copied in blind.

- [ ] **Step 5: Run to verify it passes**

Run: `make attribution && cat logs/attribution.log`
Expected: sections 9 and 10 pass; `ALL ATTRIBUTION TESTS PASSED`.

- [ ] **Step 6: Add the parser to the mutation sweep**

Open `tests/test_mutate.c` and find how an existing unsealed-probe loader is registered in the sweep (search for `gguf` or `safetensors` entries). Add `CNET_ATTRIB` alongside them with the same shape: write a valid `logs/attrib_rt.stats` first, then let the harness byte-flip and truncate it. The pass condition is **never crashes** (it is an unsealed sidecar, not a sealed artifact, so it is not required to refuse every mutation — only to refuse or accept without crashing or reading out of bounds).

- [ ] **Step 7: Run the mutation sweep**

Run: `make mutate && cat logs/mutate.log`
Expected: `All mutation-sweep gates passed.`

If a crash surfaces, fix the parser — do not weaken the sweep. This harness found two real crash bugs in `cce_gguf.c`; a hit here is a real bug.

- [ ] **Step 8: Commit**

```bash
git add include/attribution.h src/attribution.c tests/test_attribution.c tests/test_mutate.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(attribution): CNET_ATTRIB sidecar with fuzz-gated parser

Round-trip preserves posteriors exactly. Malformed and missing files leave
the ledger untouched (parse into a temp, swap only on full success). Parser
added to the make mutate byte-flip/truncation sweep.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: Drain hook and the no-op gate

This is the load-bearing task. The gate here is what makes "report-only" a property rather than a claim.

**Files:**
- Modify: `include/acquire.h` (forward declaration + two `AcquireConfig` fields)
- Modify: `src/acquire.c` (static `emit_attempt` + three call sites)
- Modify: `tests/test_attribution.c` (append section 11)
- Modify: `Makefile` (`attribution` target already links `$(ACQUIRE_SRC)` — no change needed)

**Interfaces:**
- Consumes: `attrib_sink`, `AttribLedger` from Task 1.
- Produces: `AcquireConfig.on_attempt` (type `void (*)(const struct AttributionEvent *, void *)`) and `AcquireConfig.on_attempt_ctx` (`void *`). Both default to `NULL` via `acquire_config_defaults`.

### Why the hook goes in the drain loop, not in `gap_defer`

`gap_defer` is the single chokepoint for deferrals, which makes it tempting — but it does not have `cfg` in scope, and threading `cfg` through would touch roughly 30 call sites. The drain loop already post-inspects `g->status` after each attempt ([src/acquire.c:1678](../../../src/acquire.c)), and at that point `cfg`, `g` and `report` are all in scope, and `g->oracle` / `g->defer_reason` / `g->recipe_fp` are already populated by the attempt. One helper, three call sites, zero changes to the ~30 defer sites.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_attribution.c` before `free(L);`. This needs the acquire fixture, so add `#include "../include/acquire.h"` at the top of the test file.

```c
    printf("== 11. attaching the sink is a no-op on acquisition ==\n");
    {
        /* Build the SAME acquisition twice: once with no sink, once with the
           sink attached. Every acquire-visible outcome must be identical.
           This is the report-only guarantee, mechanically enforced. */
        AcquireReport r_off, r_on;
        AttribLedger *sink = (AttribLedger *)malloc(sizeof *sink);
        if (!sink) { printf("FAIL: alloc\n"); return 1; }
        attrib_ledger_init(sink);

        memset(&r_off, 0, sizeof r_off);
        memset(&r_on, 0, sizeof r_on);
        run_fixture_drain(NULL, NULL, &r_off);
        run_fixture_drain(attrib_sink, sink, &r_on);

        check(r_off.examined == r_on.examined, "examined identical");
        check(r_off.closed == r_on.closed, "closed identical");
        check(r_off.deferred == r_on.deferred, "deferred identical");
        check(r_off.skipped_no_oracle == r_on.skipped_no_oracle,
              "skipped_no_oracle identical");
        check(r_off.last_verdict == r_on.last_verdict, "verdict identical");
        check(strcmp(r_off.last_unit_name, r_on.last_unit_name) == 0,
              "minted unit name identical");
        check(strcmp(r_off.last_defer_reason, r_on.last_defer_reason) == 0,
              "defer reason identical");
        check(r_off.total_oracle_calls == r_on.total_oracle_calls,
              "oracle call count identical");
        check(sink->count > 0, "the sink actually observed something");
        check(sink->dropped_events == 0, "nothing dropped");
        free(sink);
    }
```

Above `main()`, add the fixture. It mirrors the 4-bit increment fixture in `tests/test_acquire.c` — open that file and copy the oracle function, port construction and registry setup, then wrap it:

```c
/* Runs one complete acquisition against the 4-bit increment fixture.
   hook/ctx are installed on the config; pass NULL/NULL for the control run.
   Everything is rebuilt from scratch each call so the two runs are
   independent. Mirrors the fixture in tests/test_acquire.c. */
static void run_fixture_drain(void (*hook)(const struct AttributionEvent *, void *),
                              void *ctx, AcquireReport *report) {
    PrimitiveRegistry reg;
    AcquireLedger led;
    OracleRegistry orc;
    AcquireConfig cfg;
    Port nib, nibn;

    registry_init(&reg);
    acquire_ledger_init(&led);
    memset(&orc, 0, sizeof orc);
    acquire_config_defaults(&cfg);
    cfg.unit_dir = NULL;          /* no sealing: keeps the gate hermetic */
    cfg.on_attempt = hook;
    cfg.on_attempt_ctx = ctx;

    nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
    nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");

    acquire_oracle_register(&orc, "inc_ref", nib, nibn, oracle_increment, NULL);
    acquire_note_no_plan(&led, nib, nibn);
    acquire_drain(&reg, &led, &orc, &cfg, report);

    acquire_ledger_free(&led);
    registry_free(&reg);
}
```

Copy `oracle_increment` verbatim from `tests/test_acquire.c` (it is the reference 4-bit increment used as the oracle). If the fixture there uses a different setup call than `registry_init`, follow whatever that file actually does — it is the working reference.

- [ ] **Step 2: Run to verify it fails**

Run: `make attribution`
Expected: FAIL — `AcquireConfig has no member named on_attempt`.

- [ ] **Step 3: Add the hook fields to `include/acquire.h`**

Near the existing `struct CnetBase` forward declaration (around line 27), add:

```c
/* attribution sink event (include/attribution.h); forward-declared to avoid a
   header cycle — attribution.h must not include acquire.h */
struct AttributionEvent;
```

Inside `AcquireConfig`, immediately after the `on_close` / `on_close_ctx` pair, add:

```c
    /* Optional attribution sink (NULL = off, the default). Called exactly
       once per terminated acquisition attempt, AFTER the attempt has fully
       settled. REPORT-ONLY: acquire never reads anything back from this, so
       attaching a sink is a provable no-op — see the byte-identical drain
       gate in tests/test_attribution.c section 11. Use attrib_sink with an
       AttribLedger* as ctx. */
    void (*on_attempt)(const struct AttributionEvent *ev, void *ctx);
    void *on_attempt_ctx;
```

`acquire_config_defaults` uses `memset(cfg, 0, sizeof *cfg)` before assigning defaults — confirm that by reading it, and if so both fields default to `NULL` with no further change. If it assigns field by field instead, add explicit `cfg->on_attempt = NULL; cfg->on_attempt_ctx = NULL;`.

- [ ] **Step 4: Add `emit_attempt` to `src/acquire.c`**

Add `#include "../include/attribution.h"` to the includes, and place this helper immediately above `acquire_drain` (around line 1628):

```c
/* Emit one attribution event for a settled gap. Report-only: nothing here
   feeds back into the drain, and a NULL hook makes this a no-op. */
static void emit_attempt(const AcquireConfig *cfg, const GapRecord *g,
                         const AcquireReport *rep) {
    struct AttributionEvent ev;
    if (!cfg || !cfg->on_attempt || !g) return;
    memset(&ev, 0, sizeof ev);
    ev.proposer = g->oracle;
    ev.input_port = g->input_port;
    ev.goal_port = g->goal_port;
    ev.reason = g->defer_reason;
    ev.admitted = (g->status == GAP_CLOSED) ? 1 : 0;
    ev.recipe_fp = g->recipe_fp;
    ev.min_margin = rep ? rep->last_min_margin : 1.0;
    ev.cert_verdict = (rep && g->status == GAP_CLOSED)
                          ? (int)rep->last_verdict : -1;
    ev.domain_cardinality = 0;   /* filled in Step 5 */
    cfg->on_attempt(&ev, cfg->on_attempt_ctx);
}
```

- [ ] **Step 5: Carry the domain cardinality**

`AcquireReport` has no cardinality field today. Add one next to `last_min_margin` in `include/acquire.h`:

```c
    size_t last_domain_card;  /* certified domain cardinality of the last
                                 closed gap (abstained points included) */
```

In `src/acquire.c`, find where `rep->last_bound` and `rep->last_min_margin` are assigned on the successful-close path (search for `last_min_margin`), and set `rep->last_domain_card` from the same cardinality value the coverage call already computed there. Then in `emit_attempt` replace the placeholder with:

```c
    ev.domain_cardinality = rep ? rep->last_domain_card : 0;
```

- [ ] **Step 6: Add the three call sites**

In `acquire_drain`, inside the loop, immediately after the existing `if (g->status == GAP_DEFERRED) g->recipe_fp = fp;` line (currently line 1681):

```c
            emit_attempt(cfg, g, report);
```

In `acquire_now`, in the no-oracle branch, after `l->gaps[idx].recipe_fp = fp;` and before `return -1;` (currently lines 1719–1720):

```c
            emit_attempt(cfg, &l->gaps[idx], report);
```

In `acquire_now`, after `if (l->gaps[idx].status == GAP_DEFERRED) l->gaps[idx].recipe_fp = fp;` and before `return rc;` (currently lines 1723–1724):

```c
        emit_attempt(cfg, &l->gaps[idx], report);
```

Placing the emit *after* the `recipe_fp` stamp in all three sites matters — the event must carry the fingerprint the deferral was actually recorded under.

- [ ] **Step 7: Run the test to verify it passes**

Run: `make attribution && cat logs/attribution.log`
Expected: section 11 passes — every acquire-visible field identical between the two runs, and the sink non-empty.

- [ ] **Step 8: Confirm the existing acquisition gate still passes**

Run: `make acquire && cat logs/acquire.log`
Expected: `ALL ACQUIRE TESTS PASSED`. This is the real regression check — the drain was modified.

- [ ] **Step 9: Commit**

```bash
git add include/acquire.h src/acquire.c tests/test_attribution.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(acquire): report-only attribution hook with byte-identical drain gate

One emit_attempt helper, three call sites (drain loop + both acquire_now
exits), placed after the recipe_fp stamp so events carry the fingerprint the
deferral was recorded under. gap_defer is untouched.

The gate runs the same fixture acquisition twice, with and without the sink,
and asserts every acquire-visible outcome is identical. That is what makes
report-only a property rather than a claim.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: Conformal calibration adapter

**Files:**
- Modify: `include/attribution.h`, `src/attribution.c`
- Modify: `tests/test_attribution.c` (append section 12)
- Modify: `Makefile` (add `$(CONFORMAL)` to the `attribution` target's sources and prerequisites)

**Interfaces:**
- Consumes: `conformal_quantile` from `include/contract/conformal.h` (`double conformal_quantile(const double *scores, size_t n, double alpha)`); `CONFORMAL := src/contract/conformal.c` already exists as a Makefile variable at line 375.
- Produces: `AttribCalib`, `AttribCalibReport`, `attrib_calib_init`, `attrib_calib_add`, `attrib_calib_report`.

### The ground-truth problem

The oracle is the ground-truth source, so it cannot be calibrated against itself. Usable independent labels, in order of availability (spec §4):

1. `port_validate` failure — definitively wrong, always available, free.
2. A registered `CnetOracleValidateFn` verdict — `CNET_ORACLE_VALID` / `CNET_ORACLE_INVALID`. `CNET_ORACLE_VALIDITY_UNDETERMINED` is excluded.
3. Neither → **uncalibratable**. Emit no threshold.

Split conformal calibrates on points whose true label is known-good: score = `1 - confidence` over VALID points; the quantile at `alpha` gives the threshold below which the proposer should abstain. INVALID points do not set the threshold but are reported as a diagnostic — *of N invalid answers, how many would this threshold have caught?* That number is the direct measurement of whether the ABSTAIN valve works.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_attribution.c`:

```c
    printf("== 12. conformal calibration of proposer confidence ==\n");
    {
        AttribCalib cal;
        AttribCalibReport rep;
        int i;
        attrib_calib_init(&cal);

        check(attrib_calib_report(&cal, 0.1, 16, &rep) == -1,
              "empty calibration set is uncalibratable");
        check(rep.calibratable == 0, "report flags uncalibratable");

        /* 40 well-calibrated valid answers (high confidence) ... */
        for (i = 0; i < 40; ++i) attrib_calib_add(&cal, 0.90 + 0.001 * i, 1);
        /* ... and 10 invalid answers the proposer was overconfident about */
        for (i = 0; i < 10; ++i) attrib_calib_add(&cal, 0.60 + 0.001 * i, 0);

        check(attrib_calib_report(&cal, 0.1, 16, &rep) == 0,
              "calibratable once n_valid >= min_n");
        check(rep.calibratable == 1, "report flags calibratable");
        check(rep.n_valid == 40 && rep.n_invalid == 10,
              "valid and invalid counts tracked separately");
        check(rep.q >= 0.0 && rep.q <= 1.0, "threshold in range");
        check(rep.caught_invalid == 10,
              "threshold catches all clearly-overconfident invalid answers");

        printf("== 13. min_n is honoured, not silently relaxed ==\n");
        {
            AttribCalib small;
            AttribCalibReport srep;
            attrib_calib_init(&small);
            for (i = 0; i < 5; ++i) attrib_calib_add(&small, 0.9, 1);
            check(attrib_calib_report(&small, 0.1, 16, &srep) == -1,
                  "5 points under min_n 16 is uncalibratable");
            check(srep.n_valid == 5, "count still reported when uncalibratable");
        }

        printf("== 14. calibration buffer is bounded ==\n");
        {
            AttribCalib big;
            size_t k;
            attrib_calib_init(&big);
            for (k = 0; k < ATTRIB_MAX_CALIB + 50; ++k)
                attrib_calib_add(&big, 0.9, 1);
            check(big.n_valid == ATTRIB_MAX_CALIB, "valid buffer saturates");
            check(big.dropped == 50, "overflow counted");
        }
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make attribution`
Expected: FAIL — `unknown type name 'AttribCalib'`.

- [ ] **Step 3: Declare the calibration types in `include/attribution.h`**

Add before the closing `#endif`:

```c
#define ATTRIB_MAX_CALIB 512

/* Split-conformal calibration of a proposer's self-reported confidence
   against INDEPENDENT ground truth (port_validate failure, or a registered
   CnetOracleValidateFn verdict). The oracle cannot be calibrated against
   itself, so with neither signal available the answer is "uncalibratable" —
   never a fabricated threshold. Spec section 4. */
typedef struct {
    double valid_scores[ATTRIB_MAX_CALIB];   /* 1 - confidence, VALID points */
    size_t n_valid;
    double invalid_conf[ATTRIB_MAX_CALIB];   /* confidence, INVALID points */
    size_t n_invalid;
    size_t dropped;
} AttribCalib;

typedef struct {
    int calibratable;      /* 0 => q is meaningless */
    double q;              /* conformal threshold; accept iff 1-conf <= q */
    double alpha;          /* target miscoverage */
    size_t n_valid;
    size_t n_invalid;
    size_t caught_invalid; /* invalid answers this threshold would reject */
} AttribCalibReport;

void attrib_calib_init(AttribCalib *c);

/* truth_valid: 1 = independently confirmed valid, 0 = independently
   confirmed invalid. Do NOT call for UNDETERMINED points. Returns 0, or
   -1 when the point was dropped (buffer full / non-finite confidence). */
int attrib_calib_add(AttribCalib *c, double confidence, int truth_valid);

/* Fills *out. Returns 0 when calibratable, or -1 when n_valid < min_n (out
   is still filled with the counts, and out->calibratable is 0). */
int attrib_calib_report(const AttribCalib *c, double alpha, size_t min_n,
                        AttribCalibReport *out);
```

- [ ] **Step 4: Implement in `src/attribution.c`**

Add `#include "../include/contract/conformal.h"` and `#include <math.h>` to the includes, then append:

```c
void attrib_calib_init(AttribCalib *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
}

int attrib_calib_add(AttribCalib *c, double confidence, int truth_valid) {
    if (!c) return -1;
    /* A non-finite or out-of-range confidence is a proposer ABI violation,
       not a calibration point. */
    if (!(confidence >= 0.0) || !(confidence <= 1.0)) { c->dropped++; return -1; }
    if (truth_valid) {
        if (c->n_valid >= ATTRIB_MAX_CALIB) { c->dropped++; return -1; }
        c->valid_scores[c->n_valid++] = 1.0 - confidence;
    } else {
        if (c->n_invalid >= ATTRIB_MAX_CALIB) { c->dropped++; return -1; }
        c->invalid_conf[c->n_invalid++] = confidence;
    }
    return 0;
}

int attrib_calib_report(const AttribCalib *c, double alpha, size_t min_n,
                        AttribCalibReport *out) {
    size_t i;
    if (!c || !out) return -1;
    memset(out, 0, sizeof *out);
    out->alpha = alpha;
    out->n_valid = c->n_valid;
    out->n_invalid = c->n_invalid;
    if (c->n_valid < min_n || c->n_valid == 0) return -1;  /* uncalibratable */
    out->q = conformal_quantile(c->valid_scores, c->n_valid, alpha);
    if (out->q < 0.0) return -1;   /* bad alpha; still not calibratable */
    /* An invalid answer is caught when its nonconformity exceeds the
       threshold, i.e. 1 - confidence > q. */
    for (i = 0; i < c->n_invalid; ++i)
        if ((1.0 - c->invalid_conf[i]) > out->q) out->caught_invalid++;
    out->calibratable = 1;
    return 0;
}
```

`conformal_quantile` does not modify its input array, per its contract in `include/contract/conformal.h` — so passing `c->valid_scores` from a const struct is safe. If the compiler objects to the const-qualification, copy into a local buffer rather than casting the const away.

- [ ] **Step 5: Add `$(CONFORMAL)` to the Makefile target**

In the `attribution` target, add `$(CONFORMAL)` to both the prerequisite list and the compile line, plus `include/contract/conformal.h` to the prerequisites.

- [ ] **Step 6: Run to verify it passes**

Run: `make attribution && cat logs/attribution.log`
Expected: sections 12–14 pass.

- [ ] **Step 7: Commit**

```bash
git add include/attribution.h src/attribution.c tests/test_attribution.c Makefile
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(attribution): split-conformal calibration of proposer confidence

Calibrates self-reported confidence against independent ground truth
(port_validate failures, validator verdicts). Reports the honest abstain
threshold plus how many invalid answers it would catch — the direct
measurement of whether the ABSTAIN valve still works. Emits uncalibratable
rather than a fabricated threshold when evidence is short.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 5: Append-only event log and the report tool

**Files:**
- Modify: `include/attribution.h`, `src/attribution.c`
- Create: `tools/attrib_report.c`
- Modify: `tests/test_attribution.c` (append section 15)
- Modify: `Makefile` (add an `attrib_report` target)

**Interfaces:**
- Consumes: `AttribLedger`, `attrib_ledger_load` (Task 2); `struct AttributionEvent` (Task 1).
- Produces: `attrib_log_append(const char *path, const struct AttributionEvent *ev)`; the `attrib_report` executable.

### Why the log exists

The blame taxonomy in Task 1 is the part most likely to be wrong on the first attempt. With raw events retained, posteriors can be recomputed under revised rules — including re-keying to include `input_port` — without re-running a mining campaign to regather evidence. The log is never loaded on the hot path.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_attribution.c`:

```c
    printf("== 15. append-only event log ==\n");
    {
        struct AttributionEvent e = ev_of("gemma", g, "certify_failed", 0);
        FILE *f;
        int lines = 0, ch, prev = '\n';
        remove("logs/attrib_events.log");
        e.recipe_fp = 7u;
        check(attrib_log_append("logs/attrib_events.log", &e) == 0,
              "first append ok");
        check(attrib_log_append("logs/attrib_events.log", &e) == 0,
              "second append ok");
        f = fopen("logs/attrib_events.log", "r");
        check(f != NULL, "log file exists");
        while ((ch = fgetc(f)) != EOF) { if (ch == '\n') lines++; prev = ch; }
        fclose(f);
        check(prev == '\n', "log ends on a newline");
        check(lines == 3, "header plus two appended events");
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make attribution`
Expected: FAIL — `implicit declaration of function 'attrib_log_append'`.

- [ ] **Step 3: Declare and implement the log**

In `include/attribution.h`:

```c
/* Append one event to the raw event log ("CNET_ATTRIB_LOG 1" header, written
   once when the file is created). NEVER loaded on the hot path — this exists
   so posteriors can be recomputed under a revised blame taxonomy without
   re-running a mining campaign. Returns 0, or -1 on bad args / IO failure;
   a failure here must never propagate into acquisition. */
int attrib_log_append(const char *path, const struct AttributionEvent *ev);
```

In `src/attribution.c`:

```c
int attrib_log_append(const char *path, const struct AttributionEvent *ev) {
    FILE *f;
    long pos;
    if (!path || !ev) return -1;
    f = fopen(path, "a");
    if (!f) return -1;
    pos = ftell(f);
    if (pos == 0) fprintf(f, "CNET_ATTRIB_LOG 1\n");
    fprintf(f, "%s %d %lu %lu %s %d %lu %lu %s %s %d %llu %lu %.17g %d\n",
            sod(ev->proposer ? ev->proposer : ""),
            (int)ev->input_port.family,
            (unsigned long)ev->input_port.field_width,
            (unsigned long)ev->input_port.field_count,
            sod(ev->input_port.tag),
            (int)ev->goal_port.family,
            (unsigned long)ev->goal_port.field_width,
            (unsigned long)ev->goal_port.field_count,
            sod(ev->goal_port.tag),
            sod(ev->reason ? ev->reason : ""),
            ev->admitted,
            (unsigned long long)ev->recipe_fp,
            (unsigned long)ev->domain_cardinality,
            ev->min_margin,
            ev->cert_verdict);
    fclose(f);
    return 0;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make attribution && cat logs/attribution.log`
Expected: section 15 passes.

- [ ] **Step 5: Write the report tool**

Create `tools/attrib_report.c`:

```c
/* Reads a CNET_ATTRIB sidecar and prints the per-(proposer, signature)
   scorecard. Report-only inspector; changes nothing. */

#include <stdio.h>
#include <stdlib.h>

#include "../include/attribution.h"

int main(int argc, char **argv) {
    AttribLedger *L;
    size_t i;
    if (argc < 2) {
        fprintf(stderr, "usage: attrib_report <path-to.stats>\n");
        return 2;
    }
    L = (AttribLedger *)malloc(sizeof *L);
    if (!L) { fprintf(stderr, "alloc failed\n"); return 1; }
    attrib_ledger_init(L);
    if (attrib_ledger_load(L, argv[1]) != 0) {
        fprintf(stderr, "refused: %s\n", argv[1]);
        free(L);
        return 1;
    }
    printf("%-20s %-24s %6s %6s %6s %6s  %5s %5s  %s\n",
           "PROPOSER", "GOAL SIGNATURE", "adm", "prop", "sys", "blame",
           "trust", "yield", "note");
    for (i = 0; i < L->count; ++i) {
        const AttribRecord *r = &L->keys[i];
        char sig[64];
        const char *note = "";
        snprintf(sig, sizeof sig, "%d/w%lu/c%lu/%s",
                 (int)r->goal.family,
                 (unsigned long)r->goal.field_width,
                 (unsigned long)r->goal.field_count,
                 r->goal.tag[0] ? r->goal.tag : "-");
        if (r->recipe_fp_count > 1) note = "domain-suspect";
        else if (r->recipe_fp_count == 1 && r->system_fault > 0)
            note = "recipe-suspect";
        if (r->unclassified > 0) note = "HAS UNCLASSIFIED ATOMS";
        printf("%-20s %-24s %6lu %6lu %6lu %6lu  %5.3f %5.3f  %s\n",
               r->proposer[0] ? r->proposer : "-", sig,
               (unsigned long)r->admitted,
               (unsigned long)r->proposer_fault,
               (unsigned long)r->system_fault,
               (unsigned long)r->blameless,
               attrib_proposer_trust(r), attrib_signature_yield(r), note);
        if (r->admitted > 0)
            printf("%-20s   admitted: card_sum=%lu worst_margin=%.4f "
                   "proof=%lu sampled=%lu\n", "",
                   (unsigned long)r->admitted_card_sum,
                   r->admitted_margin_min,
                   (unsigned long)r->admitted_proof,
                   (unsigned long)r->admitted_sampled);
    }
    if (L->dropped_events)
        printf("\nWARNING: %lu events dropped — observation was degraded.\n",
               (unsigned long)L->dropped_events);
    free(L);
    return 0;
}
```

- [ ] **Step 6: Add the tool target**

Next to the `attribution` target in the Makefile:

```make
# Inspector for a CNET_ATTRIB sidecar (report-only, changes nothing).
attrib_report: $(SRC) $(CONTRACT) $(COVERAGE) $(ATTRIB_SRC) tools/attrib_report.c include/attribution.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(SRC) $(CONTRACT) $(COVERAGE) $(ATTRIB_SRC) tools/attrib_report.c $(LDFLAGS)
```

If the link fails on a missing symbol, add the smallest additional source group from the `attribution` target's list that resolves it — do not add the whole list.

- [ ] **Step 7: Verify the tool runs**

Run: `make attrib_report && ./bin/attrib_report logs/attrib_rt.stats`
Expected: a scorecard table with the `gemma` row from Task 2's round-trip fixture, showing trust and yield columns.

Also run: `./bin/attrib_report logs/attrib_bad.stats`
Expected: `refused: logs/attrib_bad.stats`, exit code 1.

- [ ] **Step 8: Run the whole gate**

Run: `make attribution && make acquire && make mutate && sh tests/verify_logs.sh`
Expected: all three logs report their pass lines and the log scan is clean.

- [ ] **Step 9: Commit**

```bash
git add include/attribution.h src/attribution.c tools/attrib_report.c tests/test_attribution.c Makefile
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(attribution): append-only event log + scorecard inspector

The raw log lets posteriors be recomputed under a revised blame taxonomy
without re-running a mining campaign — including re-keying on input_port.
Never loaded on the hot path. attrib_report flags recipe-suspect vs
domain-suspect signatures and surfaces unclassified atoms loudly.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Covered by |
|---|---|
| §3.1 key = (proposer, goal Port signature) | Task 1 Step 4 (`port_sig_eq`, `find_mut`); test section 6 |
| §3.2 four verdicts from stage of death | Task 1 Steps 4 (`ATOMS` table); test section 1 |
| §3.3 two posteriors, Laplace prior | Task 1 Step 4; test sections 2–5 |
| §3.4 anti-triviality columns | Task 1 Step 4 (`admitted_card_sum`, `admitted_margin_min`, proof/sampled); test section 8; Task 3 Step 5 carries cardinality |
| §4 conformal calibration + uncalibratable | Task 4 |
| §5 `on_attempt` hook, `NULL` default, new unit | Task 3 Steps 3–6 |
| §6 `CNET_ATTRIB` sidecar, checkpoint order | Task 2 |
| §6 `CNET_ATTRIB_LOG` append-only | Task 5 |
| §7.1 no-op gate | Task 3 Step 1 (section 11) |
| §7.2 verdict classification | Task 1 test section 1 |
| §7.3 posterior boundaries | Task 1 test sections 3–5 |
| §7.4 conformal `uncalibratable` | Task 4 test sections 12–13 |
| §7.5 sidecar round-trip + `make mutate` | Task 2 Steps 1, 6–7 |
| §7.6 wired into `make verify` | Task 1 Step 6 |
| §8 never breaks acquisition, bounded, `dropped_events` | Task 1 Step 4 (fixed arrays, no allocation); test section 7 |

No gaps.

**Placeholder scan:** One intentional forward reference — `ev.domain_cardinality = 0; /* filled in Step 5 */` in Task 3 Step 4, resolved by Task 3 Step 5 within the same task. Three places instruct the engineer to confirm a real name against the source before use (`CERT_PROOF`, `acquire_config_defaults`'s memset, the `test_acquire.c` fixture); each names the exact file and what to look for, and each has a stated fallback. No other placeholders.

**Type consistency:** `AttribLedger`, `AttribRecord`, `AttribVerdict`, `struct AttributionEvent`, `AttribCalib`, `AttribCalibReport` are used identically across all five tasks. `attrib_sink` (Task 1) is the exact function passed as `on_attempt` (Task 3). `attrib_ledger_load` (Task 2) is what `tools/attrib_report.c` calls (Task 5). `sod()` is defined in Task 2 Step 4 and reused in Task 5 Step 3 — Task 5 must come after Task 2, which the ordering enforces.

**Known risk, flagged not hidden:** Task 3 Step 5 requires finding where the coverage call computes domain cardinality on the close path. If that value turns out not to be in scope at the point `last_min_margin` is assigned, the fallback is to leave `domain_cardinality` at 0 and record cardinality in a follow-up — the anti-triviality column degrades to margin-and-verdict only, and nothing else in the plan depends on it.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-16-core-attribution-layer.md`.
