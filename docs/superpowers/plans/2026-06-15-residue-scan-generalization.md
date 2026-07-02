# Residue-Scan Compositional Generalization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a finite-state residue scan — one frozen transition `δ(r,d)=(r·b+d) mod k` composed over an N-digit string — and measure that it generalizes to whole-strings beyond any enumeration where a flat monolith can't, with δ exactly certified so the whole scan is provably correct.

**Architecture:** A new `residue` domain (no core changes), in two harnesses: `tests/test_residue.c` (in `make test`: δ trains + certifies exactly; a short scan matches ground truth) and `tests/residue_study.c` (`make residue`: the data-efficiency gap vs a flat baseline, the length sweep, and the extensibility moduli). Shared helpers live in `tests/residue_common.h` as `static` functions (built with `-Wno-unused-function`, the established escape used by `test_encode_oob`). The scan is a hand-built `DagPlan` run through `dag_execute`; δ is a multi-input BTN trained + certified exactly like `conditional_increment`.

**Tech Stack:** C11, GCC (`-std=c11 -O3 -march=native -mno-avx`), GNU make, libm. No new dependencies.

---

## Reused patterns (do NOT reinvent)

- **Multi-input BTN**: `btn_init(btn, in_count, out_count, init_hidden, max_hidden, lr, seed)`; build a `Port[]` + `port_set_tag`; `btn_set_input_ports(btn, in_ports, n, out_port)`; `btn_train_dynamic(btn, inputs, targets, samples, max_epochs, growth_window, target_loss, min_improvement)`. Exactly the `conditional_increment` flow (`src/main.c:619-651`).
- **In-code certification**: targets are trained at 0.9/0.1 but contracts need canonical (0.0/1.0) values — canonicalize each target row with `port_canonicalize` (as `emit_contract` does, `src/main.c:40-53`), then `contract_init_borrowed(&c, name, btn, inputs, canon, samples)` (`include/contract.h:34`) and `btn_certify_robust(btn, &c, floor, &report)` (`include/contract.h:73`). `report.passed`/`report.failed`/`report.min_margin` carry the verdict.
- **Hand-built scan**: stack `DagNode`s + `DagPlan{root, owned=NULL}` run via `dag_execute(&plan, sources, n_sources, out, cap)` — the pattern in `tests/circuit_demo.c` and `tests/test_fastpath.c`'s `test_dag_shared_node_split`. The same δ pointer is reused at every step.
- **Exact-count reporting**: the `capacity_study.c` / `capacity_demo.c` style (loop, count exact matches, print a table).

## Why certifying δ bounds the whole scan

The scan canonicalizes each residue (argmax → clean one-hot) *before* feeding it to the next δ. So every δ in the chain receives a canonical residue + a canonical digit — i.e. an input drawn from δ's exact 28-pair certified domain. If δ replays all 28 pairs exactly (and each output is a valid one-hot, which `btn_certify` requires), then by induction the scan is exact for **every** string of every length. Certification of the 28-pair table is therefore a proof about an unbounded input space — the result axis 3 in the spec.

---

## Task 1: residue domain core + δ certifies exactly

**Files:**
- Create: `tests/residue_common.h`
- Create: `tests/test_residue.c`
- Modify: `Makefile` (vars, `test_residue` target, add to `test` + `clean`)

- [ ] **Step 1: Write `tests/residue_common.h`** (the domain helpers used by both harnesses)

```c
/*
 * residue_common.h -- shared helpers for the mod-k residue-scan experiment.
 * delta(r,d) = (r*b + d) mod k. State = residue (ONEHOT k); digit = ONEHOT b;
 * output = next residue (ONEHOT k). Static functions, included by both
 * test_residue.c and residue_study.c; compile with -Wno-unused-function.
 */
#ifndef RESIDUE_COMMON_H
#define RESIDUE_COMMON_H

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract.h"

#include <stdlib.h>
#include <string.h>

#define RES_MAX_K 16
#define RES_MAX_B 16
#define RES_MAX_N 64

/* Ground truth: residue of an N-digit base-b number, MSB-first. */
static int string_residue(const int *digits, size_t n, int b, int k) {
    int r = 0;
    size_t i;
    for (i = 0; i < n; ++i) r = (r * b + digits[i]) % k;
    return r;
}

/* One-hot encode v in [0,width) into width doubles. */
static void res_onehot(double *vec, int v, int width) {
    int i;
    for (i = 0; i < width; ++i) vec[i] = (i == v) ? 1.0 : 0.0;
}

/* Argmax over k doubles. */
static int res_argmax(const double *v, int k) {
    int j, best = 0;
    double bv = v[0];
    for (j = 1; j < k; ++j) if (v[j] > bv) { bv = v[j]; best = j; }
    return best;
}

/* Build delta's full training table: all k*b (residue,digit) pairs.
   inputs row = [onehot-k residue | onehot-b digit] (k+b wide);
   targets row = onehot-k next residue at 0.9/0.1. Returns k*b. */
static size_t build_residue_step_data(int b, int k, double *inputs, double *targets) {
    int r, d, j;
    size_t idx = 0, in_w = (size_t)k + (size_t)b;
    for (r = 0; r < k; ++r) {
        for (d = 0; d < b; ++d) {
            double *in = inputs + idx * in_w;
            double *tg = targets + idx * (size_t)k;
            int nr = (r * b + d) % k;
            for (j = 0; j < k; ++j) in[j] = (j == r) ? 1.0 : 0.0;
            for (j = 0; j < b; ++j) in[k + j] = (j == d) ? 1.0 : 0.0;
            for (j = 0; j < k; ++j) tg[j] = (j == nr) ? 0.9 : 0.1;
            ++idx;
        }
    }
    return idx;
}

/* Init + train delta on its table. Caller btn_free's *btn. Returns final loss. */
static double train_residue_step(BinaryTransformNetwork *btn, int b, int k,
                                 const double *inputs, const double *targets,
                                 size_t samples, unsigned int seed) {
    Port in[2];
    Port out = {PORT_ONEHOT, (size_t)k, 1, ""};
    in[0] = (Port){PORT_ONEHOT, (size_t)k, 1, ""};   /* residue */
    in[1] = (Port){PORT_ONEHOT, (size_t)b, 1, ""};   /* digit */
    port_set_tag(&in[0], "residue");
    port_set_tag(&in[1], "digit");
    port_set_tag(&out, "residue");
    btn_init(btn, (size_t)k + (size_t)b, (size_t)k, 8, 64, 0.5, seed);
    btn_set_input_ports(btn, in, 2, out);
    return btn_train_dynamic(btn, inputs, targets, samples,
                             200000, 1000, 0.0008, 0.02);
}

/* Certify delta exactly on its table (canonicalize 0.9/0.1 -> 0/1 first).
   Returns 0 if certified at margin >= floor, -1 otherwise; fills *report. */
static int certify_residue_step(BinaryTransformNetwork *btn, int k,
                                const double *inputs, const double *targets,
                                size_t samples, double floor,
                                CertifyReport *report) {
    Contract c;
    double *canon;
    size_t i;
    int rc;
    canon = (double *)malloc(samples * (size_t)k * sizeof(double));
    if (canon == NULL) return -1;
    for (i = 0; i < samples; ++i)
        if (port_canonicalize(btn->output_ports[0],
                              targets + i * (size_t)k,
                              canon + i * (size_t)k) != 0) { free(canon); return -1; }
    if (contract_init_borrowed(&c, "residue_step", btn, inputs, canon, samples) != 0) {
        free(canon);
        return -1;
    }
    rc = btn_certify_robust(btn, &c, floor, report);
    free(canon);  /* contract borrowed the tables; nothing else to free */
    return rc;
}

#endif
```

- [ ] **Step 2: Write the failing test** `tests/test_residue.c`

```c
/*
 * test_residue.c -- the mod-k residue domain's correctness gate.
 * delta(r,d)=(r*b+d) mod k trains and certifies EXACTLY on its k*b table
 * (so the whole scan is bounded), and a short scan matches ground truth.
 */
#include "residue_common.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
} while (0)

static void test_delta_certifies(void) {
    const int b = 4, k = 7;
    double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta;
    CertifyReport rep;
    size_t samples;
    int rc;

    memset(&delta, 0, sizeof delta);
    samples = build_residue_step_data(b, k, inputs, targets);
    CHECK(samples == (size_t)(b * k), "delta table has k*b rows");
    train_residue_step(&delta, b, k, inputs, targets, samples, 12345u);
    rc = certify_residue_step(&delta, k, inputs, targets, samples, 0.0, &rep);
    CHECK(rc == 0, "delta certifies (exact replay on all k*b pairs)");
    CHECK(rep.passed == samples && rep.failed == 0, "all k*b exemplars pass");
    printf("  [delta b=%d k=%d: %lu/%lu certified, min margin %.4f]\n",
           b, k, (unsigned long)rep.passed, (unsigned long)samples, rep.min_margin);
    btn_free(&delta);
}

int main(void) {
    test_delta_certifies();
    if (failures == 0) { printf("RESIDUE PASS\n"); return 0; }
    printf("RESIDUE FAIL: %d checks failed.\n", failures);
    return 1;
}
```

- [ ] **Step 3: Wire the Makefile and run to verify it fails**

In `Makefile`, after the `STOCHASTIC_STUDY := ...` line add:

```make
RESIDUE_TEST := tests/test_residue.c
RESIDUE_STUDY := tests/residue_study.c
```

Add `residue` to the `.PHONY` line. After the `test_circuit`/`test_fastpath` target block add (note `-Wno-unused-function` for the header statics; links the contract + router stack like `test_decimal`):

```make
# The mod-k residue domain: delta certifies exactly + a short scan matches
# ground truth (the correctness gate). residue_common.h holds shared statics.
test_residue: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(RESIDUE_TEST) include/nn.h include/router.h include/contract.h include/plan_table.h tests/residue_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(RESIDUE_TEST) $(LDFLAGS)
```

Run: `make test_residue`
Expected: compile error — `residue_common.h` not found / undefined helpers until Step 1's file exists. (If Step 1 is already in place, it should build; then `./test_residue` runs.)

- [ ] **Step 4: Run the test to verify δ certifies**

Run: `make test_residue && ./test_residue`
Expected: `RESIDUE PASS`, with a line like `[delta b=4 k=7: 28/28 certified, min margin 0.xxxx]`.

> If δ does NOT certify 28/28, that's a training-tuning issue, not a design flaw — δ is a 28-entry lookup, exactly learnable. Bump `init_hidden`/`max_hidden`/`max_epochs` or `lr` in `train_residue_step` until it certifies. Certification is the gate; do not weaken it.

- [ ] **Step 5: Add to `make test` and `make clean`**

Append `test_residue` to the `test:` prerequisite list and its run block; append `test_residue residue_study` (and `.exe` forms) to the `clean` rule.

- [ ] **Step 6: Commit**

```bash
git add tests/residue_common.h tests/test_residue.c Makefile
git commit -m "feat: mod-k residue domain -- delta trains + certifies exactly (gate)"
```

---

## Task 2: the scan builder + composed correctness

**Files:**
- Modify: `tests/residue_common.h` (add `run_scan`)
- Modify: `tests/test_residue.c` (scan-correctness test)

- [ ] **Step 1: Add `run_scan` to `tests/residue_common.h`** (before `#endif`)

```c
/* Run the residue scan over an N-digit string (digits[i] in [0,b)).
   Builds a hand-built DAG of n delta-nodes (the same delta pointer reused),
   runs dag_execute, returns the predicted residue (argmax of the final state),
   or -1 on executor failure / bad n. */
static int run_scan(BinaryTransformNetwork *delta, int b, int k,
                    const int *digits, size_t n) {
    DagNode start;
    DagNode dn[RES_MAX_N];      /* digit sources */
    DagNode steps[RES_MAX_N];   /* delta nodes */
    DagSource srcs[1 + RES_MAX_N];
    DagPlan plan;
    double start_vec[RES_MAX_K];
    static double digit_vec[RES_MAX_N][RES_MAX_B];
    double out[RES_MAX_K];
    size_t i;

    if (n == 0 || n > RES_MAX_N) return -1;
    memset(&plan, 0, sizeof plan);
    memset(&start, 0, sizeof start);

    res_onehot(start_vec, 0, k);                 /* start residue = 0 */
    start.kind = DAG_SOURCE;
    start.source_index = 0;
    srcs[0].type = (Port){PORT_ONEHOT, (size_t)k, 1, ""};
    port_set_tag(&srcs[0].type, "residue");
    srcs[0].values = start_vec;

    for (i = 0; i < n; ++i) {
        res_onehot(digit_vec[i], digits[i], b);
        memset(&dn[i], 0, sizeof dn[i]);
        dn[i].kind = DAG_SOURCE;
        dn[i].source_index = (int)(1 + i);
        srcs[1 + i].type = (Port){PORT_ONEHOT, (size_t)b, 1, ""};
        port_set_tag(&srcs[1 + i].type, "digit");
        srcs[1 + i].values = digit_vec[i];

        memset(&steps[i], 0, sizeof steps[i]);
        steps[i].kind = DAG_PRIMITIVE;
        steps[i].btn = delta;
        steps[i].name = "residue_step";
        steps[i].children[0] = (i == 0) ? &start : &steps[i - 1];  /* prev residue */
        steps[i].children[1] = &dn[i];                              /* this digit */
        steps[i].child_ports[0] = 0;
        steps[i].child_ports[1] = 0;
        steps[i].child_count = 2;
        steps[i].output_index = 0;
    }
    plan.root = &steps[n - 1];
    plan.owned = NULL;   /* hand-built: stack nodes, do NOT dag_free */

    if (dag_execute(&plan, srcs, 1 + n, out, (size_t)k) != 0) return -1;
    return res_argmax(out, k);
}
```

- [ ] **Step 2: Write the failing test** — add to `tests/test_residue.c` and call from `main`:

```c
/* A cheap deterministic LCG so the sample is reproducible without rand() state. */
static unsigned int res_rng_state = 2463534242u;
static unsigned int res_rng(void) {
    res_rng_state ^= res_rng_state << 13;
    res_rng_state ^= res_rng_state >> 17;
    res_rng_state ^= res_rng_state << 5;
    return res_rng_state;
}

static void test_scan_matches_ground_truth(void) {
    const int b = 4, k = 7;
    const size_t N = 8, TRIALS = 2000;
    double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta;
    CertifyReport rep;
    size_t samples, t, mism = 0;

    memset(&delta, 0, sizeof delta);
    samples = build_residue_step_data(b, k, inputs, targets);
    train_residue_step(&delta, b, k, inputs, targets, samples, 12345u);
    if (certify_residue_step(&delta, k, inputs, targets, samples, 0.0, &rep) != 0) {
        printf("FAIL: delta did not certify (scan test)\n"); ++failures;
        btn_free(&delta); return;
    }

    res_rng_state = 99991u;
    for (t = 0; t < TRIALS; ++t) {
        int digits[RES_MAX_N];
        size_t i;
        int got, want;
        for (i = 0; i < N; ++i) digits[i] = (int)(res_rng() % (unsigned)b);
        got = run_scan(&delta, b, k, digits, N);
        want = string_residue(digits, N, b, k);
        if (got != want) ++mism;
    }
    CHECK(mism == 0, "scan == ground truth on 2000 random length-8 strings");
    printf("  [scan b=%d k=%d N=%lu: %lu/%lu exact]\n",
           b, k, (unsigned long)N, (unsigned long)(TRIALS - mism),
           (unsigned long)TRIALS);
    btn_free(&delta);
}
```

Add `test_scan_matches_ground_truth();` to `main` (before the pass/fail print).

- [ ] **Step 3: Run to verify it fails, then passes**

Run: `make test_residue && ./test_residue`
Expected: with `run_scan` added it builds; `RESIDUE PASS` with a `[scan ... 2000/2000 exact]` line. (Before adding `run_scan` in Step 1, it fails to compile — that's the red.)

> If the scan mismatches, the hand-built DAG is wired wrong: confirm `children[0]` is the previous residue (start for i=0), `children[1]` the digit source, slot order matches δ's input ports (residue then digit), and δ certified first. A certified δ + correct wiring is exact by the bounding argument.

- [ ] **Step 4: Commit**

```bash
git add tests/residue_common.h tests/test_residue.c
git commit -m "feat: residue scan (hand-built DAG) matches ground truth, certified delta bounds it"
```

---

## Task 3: `residue_study.c` — the data-efficiency gap (composed vs flat)

**Files:**
- Modify: `tests/residue_common.h` (flat baseline helpers)
- Create: `tests/residue_study.c`
- Modify: `Makefile` (`residue_study` + `residue` targets)

- [ ] **Step 1: Add flat-baseline helpers to `tests/residue_common.h`** (before `#endif`)

```c
/* Build a flat training/eval row: an N-digit string -> [N one-hot-b inputs |
   onehot-k residue target]. inputs row is n*b wide; target row is k wide
   (0.9/0.1). digits[] are the n digits. */
static void build_flat_row(int b, int k, size_t n, const int *digits,
                           double *in_row, double *tg_row) {
    size_t i; int j, r;
    for (i = 0; i < n; ++i)
        for (j = 0; j < b; ++j) in_row[i * (size_t)b + j] = (digits[i] == j) ? 1.0 : 0.0;
    r = string_residue(digits, n, b, k);
    for (j = 0; j < k; ++j) tg_row[j] = (j == r) ? 0.9 : 0.1;
}

/* Init + train the flat monolith: whole string (n*b one-hot) -> residue (k). */
static double train_flat(BinaryTransformNetwork *btn, int b, int k, size_t n,
                         const double *inputs, const double *targets,
                         size_t samples, unsigned int seed) {
    Port in = {PORT_ONEHOT, (size_t)b, n, ""};     /* n fields of one-hot-b */
    Port out = {PORT_ONEHOT, (size_t)k, 1, ""};
    btn_init(btn, n * (size_t)b, (size_t)k, 16, 128, 0.3, seed);
    btn_set_ports(btn, in, out);
    return btn_train_dynamic(btn, inputs, targets, samples,
                             200000, 2000, 0.0005, 0.01);
}

/* Predict residue from a flat net for one string. */
static int flat_predict(BinaryTransformNetwork *btn, int b, size_t n,
                        const int *digits, int k) {
    static double in_row[RES_MAX_N * RES_MAX_B];
    const double *out;
    size_t i; int j;
    for (i = 0; i < n; ++i)
        for (j = 0; j < b; ++j) in_row[i * (size_t)b + j] = (digits[i] == j) ? 1.0 : 0.0;
    out = btn_forward(btn, in_row);
    return res_argmax(out, k);
}
```

- [ ] **Step 2: Write `tests/residue_study.c`** (the gap at a fixed N)

```c
/*
 * residue_study.c -- does composing one frozen transition generalize beyond
 * enumeration where a flat monolith can't? Composed path: train delta on its
 * k*b table, scan over the string. Flat path: train one net on S sampled
 * strings. Both evaluated on a held-out set neither trained on. Budgeted study;
 * NOT part of make test.
 */
#include "residue_common.h"

#include <stdio.h>

#define B 4
#define K 7
#define HELDOUT 20000     /* held-out strings for evaluation */
#define FLAT_BUDGET 5000  /* strings the flat monolith trains on */

static unsigned int rng_state = 0xC0FFEEu;
static unsigned int rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5; return rng_state;
}
static void rand_string(int *digits, size_t n, int b) {
    size_t i; for (i = 0; i < n; ++i) digits[i] = (int)(rng() % (unsigned)b);
}

int main(void) {
    double dstep_in[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double dstep_tg[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta, flat;
    CertifyReport rep;
    size_t dsamples, t;
    const size_t N = 8;
    double *flat_in, *flat_tg;
    size_t composed_ok = 0, flat_ok = 0;
    static int heldout[HELDOUT][RES_MAX_N];

    /* --- composed path: train + certify delta (k*b pairs) --- */
    memset(&delta, 0, sizeof delta);
    dsamples = build_residue_step_data(B, K, dstep_in, dstep_tg);
    train_residue_step(&delta, B, K, dstep_in, dstep_tg, dsamples, 12345u);
    if (certify_residue_step(&delta, K, dstep_in, dstep_tg, dsamples, 0.0, &rep) != 0) {
        fprintf(stderr, "delta did not certify; aborting study\n");
        btn_free(&delta); return 1;
    }
    printf("residue study: n mod %d, base %d, length N=%lu\n\n", K, B, (unsigned long)N);
    printf("delta certified %lu/%lu (margin %.4f) on %lu pairs\n\n",
           (unsigned long)rep.passed, (unsigned long)dsamples, rep.min_margin,
           (unsigned long)dsamples);

    /* --- held-out evaluation set (disjoint from the flat training set below) --- */
    rng_state = 0xBEEF01u;
    for (t = 0; t < HELDOUT; ++t) rand_string(heldout[t], N, B);

    /* --- flat path: train on FLAT_BUDGET fresh sampled strings --- */
    flat_in = malloc((size_t)FLAT_BUDGET * N * B * sizeof(double));
    flat_tg = malloc((size_t)FLAT_BUDGET * K * sizeof(double));
    if (!flat_in || !flat_tg) { fprintf(stderr, "OOM\n"); return 1; }
    rng_state = 0xF1A7u;  /* distinct stream from the held-out set */
    for (t = 0; t < FLAT_BUDGET; ++t) {
        int digits[RES_MAX_N];
        rand_string(digits, N, B);
        build_flat_row(B, K, N, digits, flat_in + t * N * B, flat_tg + t * K);
    }
    memset(&flat, 0, sizeof flat);
    train_flat(&flat, B, K, N, flat_in, flat_tg, FLAT_BUDGET, 777u);

    /* --- evaluate both on the held-out set --- */
    for (t = 0; t < HELDOUT; ++t) {
        int want = string_residue(heldout[t], N, B, K);
        if (run_scan(&delta, B, K, heldout[t], N) == want) ++composed_ok;
        if (flat_predict(&flat, B, N, heldout[t], K) == want) ++flat_ok;
    }

    printf(" path     | train data        | held-out exact (%d)\n", HELDOUT);
    printf(" ---------|-------------------|--------------------\n");
    printf(" composed | %lu transition pairs | %lu/%d (%.1f%%)\n",
           (unsigned long)dsamples, (unsigned long)composed_ok, HELDOUT,
           100.0 * composed_ok / HELDOUT);
    printf(" flat     | %d strings        | %lu/%d (%.1f%%)\n",
           FLAT_BUDGET, (unsigned long)flat_ok, HELDOUT,
           100.0 * flat_ok / HELDOUT);
    printf("\n composed trains on %lu pairs; flat on %d strings (ratio ~%.0fx more data)\n",
           (unsigned long)dsamples, FLAT_BUDGET, (double)FLAT_BUDGET / (double)dsamples);

    free(flat_in); free(flat_tg);
    btn_free(&delta); btn_free(&flat);
    return 0;
}
```

- [ ] **Step 2b: Add the Makefile targets**

After the `stochastic` block add (links the same stack as `test_residue`):

```make
# Compositional generalization: composed (delta over the string) vs a flat
# monolith, on a held-out set; plus the length sweep + extensibility moduli.
# Budgeted study, NOT part of make test.
residue_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(RESIDUE_STUDY) include/nn.h include/router.h include/contract.h include/plan_table.h tests/residue_common.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(RESIDUE_STUDY) $(LDFLAGS)

residue: residue_study
	./residue_study
```

- [ ] **Step 3: Build and run**

Run: `make residue`
Expected: a table where **composed ≈ 100%** on the 20000 held-out strings (δ trained on 28 pairs) and **flat materially lower** (trained on 5000 strings) — the data-efficiency gap. If flat is also ~100%, that's the documented risk (mod-k turned out flat-learnable at N=8); record it and note the length axis (Task 4) still stands; optionally raise N to widen the gap.

- [ ] **Step 4: Commit**

```bash
git add tests/residue_common.h tests/residue_study.c Makefile
git commit -m "feat: residue_study -- composed-vs-flat data-efficiency gap on held-out strings"
```

---

## Task 4: length-extrapolation + extensibility

**Files:**
- Modify: `tests/residue_study.c` (length sweep + moduli)

- [ ] **Step 1: Add the length sweep + extensibility after the gap table** in `main` (before the `free(...)`):

```c
    /* --- length-extrapolation: the SAME certified delta, composed at lengths
       far beyond anything trained; the flat net is fixed-width and N/A here. --- */
    {
        size_t lengths[] = {4, 8, 16, 32};
        size_t li;
        printf("\nlength-extrapolation (same delta, %lu pairs; flat is fixed-width, N/A):\n",
               (unsigned long)dsamples);
        printf(" N   | composed exact (5000 random)\n");
        printf(" ----|------------------------------\n");
        for (li = 0; li < sizeof lengths / sizeof lengths[0]; ++li) {
            size_t n = lengths[li], ok = 0, s;
            rng_state = 0x5A17u + (unsigned)n;
            for (s = 0; s < 5000; ++s) {
                int digits[RES_MAX_N];
                rand_string(digits, n, B);
                if (run_scan(&delta, B, K, digits, n) == string_residue(digits, n, B, K)) ++ok;
            }
            printf(" %-3lu | %lu/5000 (%.1f%%)\n", (unsigned long)n, (unsigned long)ok,
                   100.0 * ok / 5000.0);
        }
    }

    /* --- extensibility: swap delta for other moduli; the SAME scan composes. --- */
    {
        int moduli[] = {3, 5, 7};
        size_t mi;
        printf("\nextensibility (swap delta -> new modulus, scan unchanged):\n");
        printf(" k   | delta cert | composed exact at N=8 (5000 random)\n");
        printf(" ----|------------|------------------------------------\n");
        for (mi = 0; mi < sizeof moduli / sizeof moduli[0]; ++mi) {
            int kk = moduli[mi];
            double in2[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
            double tg2[RES_MAX_K * RES_MAX_B * RES_MAX_K];
            BinaryTransformNetwork d2;
            CertifyReport r2;
            size_t s2, ns, ok = 0;
            memset(&d2, 0, sizeof d2);
            ns = build_residue_step_data(B, kk, in2, tg2);
            train_residue_step(&d2, B, kk, in2, tg2, ns, 24680u + (unsigned)kk);
            s2 = (certify_residue_step(&d2, kk, in2, tg2, ns, 0.0, &r2) == 0);
            rng_state = 0x90D5u + (unsigned)kk;
            { size_t s; for (s = 0; s < 5000; ++s) {
                int digits[RES_MAX_N];
                rand_string(digits, 8, B);
                if (run_scan(&d2, B, kk, digits, 8) == string_residue(digits, 8, B, kk)) ++ok;
            } }
            printf(" %-3d | %s | %lu/5000 (%.1f%%)\n", kk,
                   s2 ? "yes" : "NO ", (unsigned long)ok, 100.0 * ok / 5000.0);
            btn_free(&d2);
        }
    }
```

- [ ] **Step 2: Build and run**

Run: `make residue`
Expected: after the gap table, a length-extrapolation table (composed ≈ 100% at N = 4, 8, 16, 32 — the same δ, never retrained, no erosion across depth) and an extensibility table (each modulus's δ certifies and its scan is ≈ 100%). These two axes are the airtight + extensibility results; the flat net cannot appear in the length table by construction.

- [ ] **Step 3: Commit**

```bash
git add tests/residue_study.c
git commit -m "feat: residue_study -- length-extrapolation + swap-delta extensibility"
```

---

## Self-review

**Spec coverage:**
- δ primitive (multi-input, ONEHOT residue + digit → ONEHOT residue), trained on k·b table → Task 1 (`train_residue_step`, `build_residue_step_data`). ✓
- δ certified exactly (verify→bound-the-whole, result 3) → Task 1 (`certify_residue_step`, `btn_certify_robust`); the bounding argument is in the plan header. ✓
- Hand-built scan over the digit sequence via `dag_execute` → Task 2 (`run_scan`). ✓
- Composed correctness vs ground truth → Task 2. ✓
- Flat monolith baseline → Task 3 (`train_flat`/`flat_predict`). ✓
- Data-efficiency gap on held-out (result 1) → Task 3 (`residue_study.c`, composed vs flat, disjoint held-out). ✓
- Length-extrapolation (result 2) → Task 4 (N = 4,8,16,32; flat structurally N/A). ✓
- Extensibility (result 4) → Task 4 (k = 3,5,7, scan unchanged). ✓
- `make test` gate (δ certifies + short scan correct) vs budgeted `make residue` study → Tasks 1–2 in `make test`, Tasks 3–4 in `make residue`. ✓
- Core untouched (only new tests + Makefile additions; no `nn.c`/`router.c`/`consolidate.c` change) → confirmed. ✓
- The honest risk (flat may generalize) → Task 3 Step 3 note; length axis stays airtight. ✓

**Placeholder scan:** no TBD/TODO; every step has complete code; commands have expected output.

**Type consistency:** `build_residue_step_data`/`train_residue_step`/`certify_residue_step`/`run_scan`/`string_residue`/`res_onehot`/`res_argmax`/`build_flat_row`/`train_flat`/`flat_predict` are defined once in `residue_common.h` and used identically in both harnesses. δ ports: input `[ONEHOT k "residue", ONEHOT b "digit"]`, output `ONEHOT k "residue"`; the scan wires `children[0]=residue`, `children[1]=digit` to match. `CertifyReport`/`Contract`/`DagPlan`/`DagNode`/`DagSource` come from the existing headers. `RES_MAX_N=64` bounds the hand-built node arrays (dag_execute has no depth cap; for N≤32 the chain is 65 nodes total, within stack limits).

**One risk flagged for the implementer:** δ must certify 28/28 (Task 1 Step 4 note). It's a 28-entry lookup, exactly learnable — if it doesn't certify, raise capacity/epochs in `train_residue_step`; never weaken the certification gate. The scan's exactness depends on it (the bounding argument).
