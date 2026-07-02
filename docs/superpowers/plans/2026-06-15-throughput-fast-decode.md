# Throughput Fast Decode — Implementation Plan (Phase 1: route fast-lane)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a separate fast-lane executor for frozen route plans (pack weights once, reuse scratch, batch the per-primitive matmul, snap each handoff) plus a `throughput_study` instrument that charts each speed lever against the `double` oracle — with precision drops measured for margin certification.

**Architecture:** New module `src/fastpath.c` + `include/fastpath.h`, an *independent* implementation of route execution checked against `route_execute` (the trusted oracle, untouched). It reads `RoutePlan` and `BinaryTransformNetwork` weights read-only, repacks them into the chosen numeric kind (`FP_F64`/`FP_F32`/`FP_INT`), and runs N samples per compiled route with one weight-resident pass per step. Faithfulness bar: identical *snapped* output to the oracle; precision is reported with its worst margin so the study can mark it certified or not. Phase 1 covers the **route** path and levers L0–L5 (L5 = consolidation); the DAG/decode lane and Phase 2 hardening are deferred to a follow-up plan.

**Tech Stack:** C11, GCC (`-std=c11 -O3 -march=native -mno-avx`), GNU make, libm. No new dependencies.

---

## Scope and the workload

Phase 1's measured route is the README's discoverable 2-hop chain `hex_value -> increment` (`ONEHOT16 -> BINARY_MSB5`), built over the committed frozen weights `hex_value_weights.txt` and `increment_weights.txt` exactly as `tests/route_demo.c` does. It exercises both port families (`ONEHOT`, `BINARY_MSB`) and two snaps — enough to prove every kernel and the certification gate. The decimal-adder family and the literal `combine`-based byte assembly (a DAG) are the next plan's workloads; the kernels here are what that plan reuses.

## Weight layout (read-only contract with `nn.c`)

`btn_forward` (`src/nn.c:743`) reads, per step:
- `input_hidden[hidden * input_count + input]` — hidden-major; row length `input_count`.
- `hidden_output_weights[output * max_hidden_count + hidden]` — output-major; **row stride is `max_hidden_count`, not `hidden_count`** (only the first `hidden_count` entries are live).
- `hidden_bias[hidden]`, `output_bias[output]`.
- Forward: `h_out[h] = sigmoid(hidden_bias[h] + Σ_i in[i]*input_hidden[h*in+i])`, then `out[o] = sigmoid(output_bias[o] + Σ_h h_out[h]*hidden_output_weights[o*max_hid+h])`.

The fast lane packs these into dense, `hidden_count`-strided buffers in its chosen kind. The index helpers in `nn.c` are `static`; the fast lane does **not** call them — it repacks via the formulas above, so `nn.c` needs no change.

## Faithfulness, determinism, certification

- **Snapped-output faithful:** for every sample, the fast lane's canonicalized output must equal `route_execute`'s. The study counts `mismatches`.
- **Bit-identical (`FP_F64`):** the batched f64 run must equal the single-sample f64 run bit-for-bit. Achieved by keeping each sample's reduction order identical to `btn_forward` (sum `i` ascending, then `h` ascending); FP contraction is off under `-std=c11` and `-mno-avx`, so no reassociation occurs.
- **Margin reporting:** `fp_route_run` folds each handoff's `port_margin` into a running minimum, read via `fp_route_min_margin`. A precision is **certified** for the route iff `mismatches == 0 && fp_route_min_margin >= floor`. `floor` is a policy parameter (the same role `margin_floor` plays in `btn_certify_robust`, `src/contract.c:508`); the study uses `FP_MARGIN_FLOOR = 0.25` (the `port_validate` unambiguity band). Phase 1 **reports** the verdict; it does not reject (that gate is Phase 2).

---

## Task 1: Module scaffold + f64 fast lane (L0/L1) + Makefile wiring

**Files:**
- Create: `include/fastpath.h`
- Create: `src/fastpath.c`
- Create: `tests/test_fastpath.c`
- Modify: `Makefile` (add `FASTPATH`, `FASTPATH_TEST`, `test_fastpath` target, add to `test` and `clean`)

- [ ] **Step 1: Write the header**

Create `include/fastpath.h`:

```c
#ifndef FASTPATH_H
#define FASTPATH_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* Numeric representation of the fast lane. FP_INT carries a bit-width: 8 is the
   lever; small widths exist to prove the certification gate bites. */
typedef enum { FP_F64, FP_F32, FP_INT } FastKind;

typedef struct {
    FastKind kind;
    int int_bits;   /* used only when kind == FP_INT (e.g. 8) */
} FastPrecision;

typedef struct CompiledRoute CompiledRoute;

/* Pack every step's weights into `prec` ONCE and size scratch to `batch`
   samples. Reads the plan's BTNs read-only; the double core is untouched.
   Returns NULL on bad arguments or allocation failure. Phase 1 never rejects on
   margin -- it REPORTS via fp_route_min_margin. */
CompiledRoute *fp_route_compile(const RoutePlan *plan, FastPrecision prec,
                                size_t batch);

/* Run n (<= batch) inputs through the compiled route. Input i is in_total
   contiguous doubles at in + i*in_total; output i is out_total contiguous
   doubles at out + i*out_total (out_cap_total must be >= n*out_total). Every
   handoff is validated + canonicalized like route_execute; the worst per-handoff
   margin folds into the route's running minimum. Returns 0, or -1 on a
   bad/ambiguous handoff or argument. */
int fp_route_run(CompiledRoute *cr, const double *in, size_t n,
                 double *out, size_t out_cap_total);

/* Worst margin over all handoffs and samples since the last reset (starts at
   0.5, the maximum binary margin). */
double fp_route_min_margin(const CompiledRoute *cr);
void   fp_route_reset_margin(CompiledRoute *cr);

size_t fp_route_in_total(const CompiledRoute *cr);
size_t fp_route_out_total(const CompiledRoute *cr);

void fp_route_free(CompiledRoute *cr);

#endif
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_fastpath.c`:

```c
/*
 * test_fastpath.c -- the fast lane is an independent route executor; this
 * certifies it against route_execute (the double oracle) over the committed
 * hex_value -> increment route. Loads committed frozen weights (like
 * test_decimal); run ./nn_demo first if they are stale or absent.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/fastpath.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
} while (0)

static Port P(PortFamily family, size_t fw, size_t fc) {
    Port p; p.family = family; p.field_width = fw; p.field_count = fc;
    p.tag[0] = '\0'; return p;
}

/* Load the committed route once; returns 0 on success. Caller frees. */
static int load_route(BinaryTransformNetwork *hexval,
                      BinaryTransformNetwork *incr,
                      PrimitiveRegistry *reg, RoutePlan *plan) {
    memset(hexval, 0, sizeof *hexval);
    memset(incr, 0, sizeof *incr);
    if (btn_load(hexval, "hex_value_weights.txt") != 0 ||
        btn_load(incr, "increment_weights.txt") != 0) {
        fprintf(stderr, "could not load frozen primitives (run ./nn_demo).\n");
        return -1;
    }
    registry_init(reg);
    registry_add(reg, hexval, "hex_value");
    registry_add(reg, incr, "increment");
    return route_plan(reg, P(PORT_ONEHOT, 16, 1), P(PORT_BINARY_MSB, 5, 1), plan);
}

static void test_f64_matches_oracle(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision prec = { FP_F64, 0 };
    CompiledRoute *cr;
    int i;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup\n"); ++failures; return;
    }
    cr = fp_route_compile(&plan, prec, 1);
    CHECK(cr != NULL, "fp_route_compile f64");
    if (cr == NULL) { registry_free(&reg); btn_free(&hexval); btn_free(&incr); return; }

    CHECK(fp_route_in_total(cr) == 16, "in_total == 16");
    CHECK(fp_route_out_total(cr) == 5, "out_total == 5");

    for (i = 0; i < 16; ++i) {
        double in[16] = {0};
        double want[5] = {0};
        double got[5] = {0};
        in[i] = 1.0;
        CHECK(route_execute(&plan, in, 16, want, 5) == 0, "oracle run");
        CHECK(fp_route_run(cr, in, 1, got, 5) == 0, "fast f64 run");
        CHECK(memcmp(want, got, sizeof want) == 0, "f64 == oracle (snapped)");
    }

    fp_route_free(cr);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}

int main(void) {
    test_f64_matches_oracle();
    if (failures == 0) {
        printf("FASTPATH PASS\n");
        return 0;
    }
    printf("FASTPATH FAIL: %d checks failed.\n", failures);
    return 1;
}
```

- [ ] **Step 3: Wire the Makefile (so the test can build) and run it to verify it fails**

In `Makefile`, after the `STOCHASTIC_STUDY := ...` line (around line 44) add:

```make
FASTPATH := src/fastpath.c
FASTPATH_TEST := tests/test_fastpath.c
THROUGHPUT_STUDY := tests/throughput_study.c
```

Add `fastpath` and `throughput` to the `.PHONY` line (line 46). After the `test_circuit` target block (around line 122) add:

```make
# The fast lane: an independent route executor (packed weights, reused scratch,
# batched matmul, per-handoff snap) checked against route_execute. Loads the
# committed frozen hex_value/increment weights.
test_fastpath: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(FASTPATH_TEST) include/nn.h include/router.h include/fastpath.h include/plan_table.h include/contract.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(FASTPATH_TEST) $(LDFLAGS)
```

Run: `make test_fastpath`
Expected: FAIL — link error, `undefined reference to fp_route_compile` (and the other `fp_route_*` symbols), because `src/fastpath.c` does not exist yet.

- [ ] **Step 4: Write the minimal f64 implementation**

Create `src/fastpath.c`:

```c
/*
 * fastpath.c -- a separate, batched route executor. It repacks frozen BTN
 * weights into a chosen numeric kind (f64/f32/int) ONCE, reuses scratch across
 * the whole stream, and snaps every handoff exactly like route_execute. The
 * double core (nn.c/router.c) is untouched and remains the oracle this is
 * checked against. Reduction order matches btn_forward so the f64 path is
 * bit-identical to the oracle, per sample.
 */
#include "../include/fastpath.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static double fp_sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }

typedef struct {
    size_t in;     /* input_count */
    size_t out;    /* output_count */
    size_t hid;    /* live hidden_count */
    Port in_port;
    Port out_port;
    /* Dense, hid-strided packs (representation depends on cr->prec.kind). */
    double *w_ih;  /* [hid * in]  row-major by hidden */
    double *w_ho;  /* [out * hid] row-major by output */
    double *b_h;   /* [hid] */
    double *b_o;   /* [out] */
    float  *w_ih_f; float *w_ho_f; float *b_h_f; float *b_o_f;
    int8_t *w_ih_q; int8_t *w_ho_q;
    double w_ih_scale; double w_ho_scale;
    int int_bits;
} FpStep;

struct CompiledRoute {
    FastPrecision prec;
    size_t batch;
    FpStep *steps;
    size_t length;
    size_t in_total;
    size_t out_total;
    /* scratch, reused across samples AND steps */
    double *cur;   /* batch * max_width */
    double *nxt;   /* batch * max_width */
    double *hidv;  /* batch * max_hidden */
    double min_margin;
};

double fp_route_min_margin(const CompiledRoute *cr) {
    return cr ? cr->min_margin : 0.0;
}
void fp_route_reset_margin(CompiledRoute *cr) {
    if (cr) cr->min_margin = 0.5;
}
size_t fp_route_in_total(const CompiledRoute *cr) { return cr ? cr->in_total : 0; }
size_t fp_route_out_total(const CompiledRoute *cr) { return cr ? cr->out_total : 0; }

void fp_route_free(CompiledRoute *cr) {
    size_t s;
    if (cr == NULL) return;
    if (cr->steps) {
        for (s = 0; s < cr->length; ++s) {
            FpStep *st = &cr->steps[s];
            free(st->w_ih); free(st->w_ho); free(st->b_h); free(st->b_o);
            free(st->w_ih_f); free(st->w_ho_f); free(st->b_h_f); free(st->b_o_f);
            free(st->w_ih_q); free(st->w_ho_q);
        }
        free(cr->steps);
    }
    free(cr->cur); free(cr->nxt); free(cr->hidv);
    free(cr);
}

/* Pack one step's weights as dense f64 (hid-strided), reading the canonical
   btn layout described in the plan header. */
static int pack_step_f64(FpStep *st, const BinaryTransformNetwork *btn) {
    size_t i, h, o;
    st->w_ih = malloc(st->hid * st->in * sizeof(double));
    st->w_ho = malloc(st->out * st->hid * sizeof(double));
    st->b_h  = malloc(st->hid * sizeof(double));
    st->b_o  = malloc(st->out * sizeof(double));
    if (!st->w_ih || !st->w_ho || !st->b_h || !st->b_o) return -1;
    for (h = 0; h < st->hid; ++h) {
        st->b_h[h] = btn->hidden_bias[h];
        for (i = 0; i < st->in; ++i)
            st->w_ih[h * st->in + i] = btn->input_hidden[h * btn->input_count + i];
    }
    for (o = 0; o < st->out; ++o) {
        st->b_o[o] = btn->output_bias[o];
        for (h = 0; h < st->hid; ++h)
            st->w_ho[o * st->hid + h] =
                btn->hidden_output_weights[o * btn->max_hidden_count + h];
    }
    return 0;
}

CompiledRoute *fp_route_compile(const RoutePlan *plan, FastPrecision prec,
                                size_t batch) {
    CompiledRoute *cr;
    size_t s, max_width = 0, max_hidden = 0;

    if (plan == NULL || plan->length == 0 || batch == 0) return NULL;

    cr = calloc(1, sizeof *cr);
    if (cr == NULL) return NULL;
    cr->prec = prec;
    cr->batch = batch;
    cr->length = plan->length;
    cr->min_margin = 0.5;
    cr->steps = calloc(plan->length, sizeof(FpStep));
    if (cr->steps == NULL) { fp_route_free(cr); return NULL; }

    for (s = 0; s < plan->length; ++s) {
        const BinaryTransformNetwork *btn = plan->steps[s];
        FpStep *st = &cr->steps[s];
        if (btn->input_port_count != 1 || btn->output_port_count != 1) {
            fp_route_free(cr); return NULL;   /* route lane is single-port */
        }
        st->in = btn->input_count;
        st->out = btn->output_count;
        st->hid = btn->hidden_count;
        st->in_port = btn->input_ports[0];
        st->out_port = btn->output_ports[0];
        st->int_bits = prec.int_bits;
        if (st->in > max_width) max_width = st->in;
        if (st->out > max_width) max_width = st->out;
        if (st->hid > max_hidden) max_hidden = st->hid;
        if (pack_step_f64(st, btn) != 0) { fp_route_free(cr); return NULL; }
        /* f32/int packs are added in later tasks. */
    }
    cr->in_total = cr->steps[0].in;
    cr->out_total = cr->steps[cr->length - 1].out;

    cr->cur  = malloc(batch * max_width * sizeof(double));
    cr->nxt  = malloc(batch * max_width * sizeof(double));
    cr->hidv = malloc(batch * max_hidden * sizeof(double));
    if (!cr->cur || !cr->nxt || !cr->hidv) { fp_route_free(cr); return NULL; }
    return cr;
}

/* Forward one step over n samples in f64. Reads samples from `src` (n x st->in,
   row-major), writes raw outputs to `dst` (n x st->out). Per-sample reduction
   order matches btn_forward. */
static void step_forward_f64(const FpStep *st, const double *src, size_t n,
                             double *hidv, double *dst) {
    size_t k, i, h, o;
    for (k = 0; k < n; ++k) {
        const double *in = src + k * st->in;
        double *hv = hidv + k * st->hid;
        double *ov = dst + k * st->out;
        for (h = 0; h < st->hid; ++h) {
            double sum = st->b_h[h];
            const double *row = st->w_ih + h * st->in;
            for (i = 0; i < st->in; ++i) sum += in[i] * row[i];
            hv[h] = fp_sigmoid(sum);
        }
        for (o = 0; o < st->out; ++o) {
            double sum = st->b_o[o];
            const double *row = st->w_ho + o * st->hid;
            for (h = 0; h < st->hid; ++h) sum += hv[h] * row[h];
            ov[o] = fp_sigmoid(sum);
        }
    }
}

int fp_route_run(CompiledRoute *cr, const double *in, size_t n,
                 double *out, size_t out_cap_total) {
    size_t s, k;
    double *cur, *nxt;

    if (cr == NULL || in == NULL || out == NULL) return -1;
    if (n == 0 || n > cr->batch) return -1;
    if (out_cap_total < n * cr->out_total) return -1;

    cur = cr->cur;
    nxt = cr->nxt;

    /* Load + validate + canonicalize the external input, per sample, against
       the first step's input port (mirrors route_execute's entry snap). */
    for (k = 0; k < n; ++k) {
        const double *src = in + k * cr->in_total;
        double *dstrow = cur + k * cr->steps[0].in;
        if (!port_validate(cr->steps[0].in_port, src)) return -1;
        if (port_canonicalize(cr->steps[0].in_port, src, dstrow) != 0) return -1;
    }

    for (s = 0; s < cr->length; ++s) {
        FpStep *st = &cr->steps[s];
        /* cur holds n x st->in canonical inputs; forward into nxt (raw). */
        step_forward_f64(st, cur, n, cr->hidv, nxt);
        /* Snap each sample's raw output against the output port; fold margin. */
        for (k = 0; k < n; ++k) {
            double *raw = nxt + k * st->out;
            double *dst = cur + k * st->out;   /* next step reads from cur */
            double mm;
            if (port_margin(st->out_port, raw, &mm) == 0 && mm < cr->min_margin)
                cr->min_margin = mm;
            if (port_canonicalize(st->out_port, raw, dst) != 0) return -1;
        }
    }

    for (k = 0; k < n; ++k)
        memcpy(out + k * cr->out_total, cur + k * cr->out_total,
               cr->out_total * sizeof(double));
    return 0;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS`

- [ ] **Step 6: Add to `make test` and `make clean`**

In `Makefile`, append `test_fastpath` to the `test:` prerequisite list and its run block (around lines 210–221):

```make
test: test_nn test_encode_oob test_contract test_router test_dag test_consolidate test_certify test_property test_decimal test_circuit test_library test_fastpath
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
	./test_fastpath
```

Add the binaries to `clean` (both bare and `.exe` forms) by appending `test_fastpath throughput_study` and `test_fastpath.exe throughput_study.exe` to the `clean` rule's file lists.

- [ ] **Step 7: Commit**

```bash
git add include/fastpath.h src/fastpath.c tests/test_fastpath.c Makefile
git commit -m "feat: fastpath route lane (f64) -- independent executor, oracle-checked"
```

---

## Task 2: Batched f64 is bit-identical to single-sample (L2)

**Files:**
- Modify: `tests/test_fastpath.c` (add a test)

The f64 forward already handles `n > 1` (Task 1). This task proves batching changes nothing bit-for-bit.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_fastpath.c`, and call it from `main` before the `if (failures == 0)` block:

```c
static void test_batched_equals_single_f64(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision prec = { FP_F64, 0 };
    CompiledRoute *cr;
    double in[16 * 16] = {0};
    double batched[16 * 5] = {0};
    double single[16 * 5] = {0};
    int i;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (batch)\n"); ++failures; return;
    }
    cr = fp_route_compile(&plan, prec, 16);
    CHECK(cr != NULL, "compile batch=16");
    if (cr == NULL) { registry_free(&reg); btn_free(&hexval); btn_free(&incr); return; }

    for (i = 0; i < 16; ++i) in[i * 16 + i] = 1.0;   /* 16 one-hot inputs */

    CHECK(fp_route_run(cr, in, 16, batched, sizeof batched / sizeof(double)) == 0,
          "batched run");
    for (i = 0; i < 16; ++i)
        CHECK(fp_route_run(cr, in + i * 16, 1, single + i * 5, 5) == 0,
              "single run");

    CHECK(memcmp(batched, single, sizeof batched) == 0,
          "batched f64 == single f64 (bit-identical)");

    fp_route_free(cr);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}
```

Add `test_batched_equals_single_f64();` to `main`.

- [ ] **Step 2: Run to verify it passes**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS` (batching reuses the same per-sample kernel, so the bytes match).

> Note: if this fails, the kernel reordered a per-sample reduction — the inner loops must sum `i` then `h` in ascending order, identical to `btn_forward`. Do not introduce FMA or `-ffast-math`.

- [ ] **Step 3: Commit**

```bash
git add tests/test_fastpath.c
git commit -m "test: fastpath batched f64 is bit-identical to single-sample"
```

---

## Task 3: float32 lane (L3)

**Files:**
- Modify: `src/fastpath.c` (f32 pack + f32 forward; dispatch by `prec.kind`)
- Modify: `tests/test_fastpath.c` (add a test)

- [ ] **Step 1: Write the failing test**

Add to `tests/test_fastpath.c` and call from `main`:

```c
static void test_f32_matches_oracle(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision prec = { FP_F32, 0 };
    CompiledRoute *cr;
    int i, mismatches = 0;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (f32)\n"); ++failures; return;
    }
    cr = fp_route_compile(&plan, prec, 16);
    CHECK(cr != NULL, "compile f32");
    if (cr == NULL) { registry_free(&reg); btn_free(&hexval); btn_free(&incr); return; }

    for (i = 0; i < 16; ++i) {
        double in[16] = {0};
        double want[5] = {0};
        double got[5] = {0};
        in[i] = 1.0;
        route_execute(&plan, in, 16, want, 5);
        CHECK(fp_route_run(cr, in, 1, got, 5) == 0, "f32 run");
        if (memcmp(want, got, sizeof want) != 0) ++mismatches;
    }
    CHECK(mismatches == 0, "f32 == oracle (snapped, all 16)");
    /* The frozen primitives certify well above the band; f32 must not crowd it. */
    CHECK(fp_route_min_margin(cr) >= 0.25, "f32 min margin >= floor (0.25)");

    fp_route_free(cr);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}
```

Add `test_f32_matches_oracle();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test_fastpath && ./test_fastpath`
Expected: FAIL — `compile f32` check fails (the f32 pack/forward is not written, so `fp_route_run` runs the f64 kernel on unpopulated f32 weights, or asserts). The point: f32 is not yet a real path.

- [ ] **Step 3: Implement the f32 pack and forward, and dispatch by kind**

In `src/fastpath.c`, add the f32 pack helper after `pack_step_f64`:

```c
static int pack_step_f32(FpStep *st) {
    size_t i;
    st->w_ih_f = malloc(st->hid * st->in * sizeof(float));
    st->w_ho_f = malloc(st->out * st->hid * sizeof(float));
    st->b_h_f  = malloc(st->hid * sizeof(float));
    st->b_o_f  = malloc(st->out * sizeof(float));
    if (!st->w_ih_f || !st->w_ho_f || !st->b_h_f || !st->b_o_f) return -1;
    for (i = 0; i < st->hid * st->in; ++i)  st->w_ih_f[i] = (float)st->w_ih[i];
    for (i = 0; i < st->out * st->hid; ++i) st->w_ho_f[i] = (float)st->w_ho[i];
    for (i = 0; i < st->hid; ++i) st->b_h_f[i] = (float)st->b_h[i];
    for (i = 0; i < st->out; ++i) st->b_o_f[i] = (float)st->b_o[i];
    return 0;
}
```

In `fp_route_compile`, the per-step loop already calls `pack_step_f64` (the f64 pack is the source the f32/int packs derive from). After it, add:

```c
        if (prec.kind == FP_F32 && pack_step_f32(st) != 0) {
            fp_route_free(cr); return NULL;
        }
```

Add the f32 forward after `step_forward_f64`:

```c
static void step_forward_f32(const FpStep *st, const double *src, size_t n,
                             double *hidv, double *dst) {
    size_t k, i, h, o;
    for (k = 0; k < n; ++k) {
        const double *in = src + k * st->in;
        double *hv = hidv + k * st->hid;
        double *ov = dst + k * st->out;
        for (h = 0; h < st->hid; ++h) {
            float sum = st->b_h_f[h];
            const float *row = st->w_ih_f + h * st->in;
            for (i = 0; i < st->in; ++i) sum += (float)in[i] * row[i];
            hv[h] = fp_sigmoid((double)sum);
        }
        for (o = 0; o < st->out; ++o) {
            float sum = st->b_o_f[o];
            const float *row = st->w_ho_f + o * st->hid;
            for (h = 0; h < st->hid; ++h) sum += (float)hv[h] * row[h];
            ov[o] = fp_sigmoid((double)sum);
        }
    }
}
```

In `fp_route_run`, replace the single `step_forward_f64(...)` call with a dispatch:

```c
        switch (cr->prec.kind) {
            case FP_F32: step_forward_f32(st, cur, n, cr->hidv, nxt); break;
            default:     step_forward_f64(st, cur, n, cr->hidv, nxt); break;
        }
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS`

- [ ] **Step 5: Commit**

```bash
git add src/fastpath.c tests/test_fastpath.c
git commit -m "feat: fastpath float32 lane -- output-faithful, margin reported"
```

---

## Task 4: int8 lane (L4) + the certification gate bites

**Files:**
- Modify: `src/fastpath.c` (int pack + int forward; dispatch)
- Modify: `tests/test_fastpath.c` (int8 certifies; int2 is rejected by the margin gate)

Quantization scheme (symmetric, per-matrix): for a weight matrix `W`, `scale = max(|W|) / qmax` where `qmax = 2^(bits-1) - 1`; store `q = clamp(round(W/scale), -qmax, qmax)`. Activations are in `[0,1]`; quantize per value to `qa = round(a * qmax)` (unsigned-ish, non-negative), so a product dequantizes as `(qw * qa) * scale / qmax`. Accumulate `qw*qa` in `int64`, dequantize once, add the (full-precision) bias, then sigmoid. Biases stay `double` (the `b_h`/`b_o` packs from `pack_step_f64`).

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_fastpath.c` and call both from `main`:

```c
static double run_route_min_margin(RoutePlan *plan, FastPrecision prec,
                                   int *out_mismatches) {
    CompiledRoute *cr = fp_route_compile(plan, prec, 16);
    int i, mism = 0;
    double mm;
    if (cr == NULL) { *out_mismatches = 9999; return 0.0; }
    for (i = 0; i < 16; ++i) {
        double in[16] = {0};
        double want[5] = {0};
        double got[5] = {0};
        in[i] = 1.0;
        route_execute(plan, in, 16, want, 5);
        if (fp_route_run(cr, in, 1, got, 5) != 0 ||
            memcmp(want, got, sizeof want) != 0) ++mism;
    }
    mm = fp_route_min_margin(cr);
    *out_mismatches = mism;
    fp_route_free(cr);
    return mm;
}

static void test_int8_certifies(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision p8 = { FP_INT, 8 };
    int mism8 = 0;
    double mm8;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (int8)\n"); ++failures; return;
    }
    mm8 = run_route_min_margin(&plan, p8, &mism8);
    CHECK(mism8 == 0, "int8 == oracle (snapped, all 16)");
    CHECK(mm8 >= 0.25, "int8 min margin >= floor -> certified");

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}

static void test_int2_gate_bites(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision p2 = { FP_INT, 2 };
    int mism2 = 0;
    double mm2;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (int2)\n"); ++failures; return;
    }
    mm2 = run_route_min_margin(&plan, p2, &mism2);
    /* An over-aggressive precision must NOT certify: either it diverges from the
       oracle, or its margin collapses below the floor. The gate bites. */
    CHECK(mism2 > 0 || mm2 < 0.25, "int2 is rejected (gate bites)");

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}
```

Add `test_int8_certifies();` and `test_int2_gate_bites();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test_fastpath && ./test_fastpath`
Expected: FAIL — `int8 == oracle` fails, because `FP_INT` has no pack/forward yet and falls through to the f64 kernel reading unpopulated int buffers.

- [ ] **Step 3: Implement the int pack and forward**

In `src/fastpath.c`, add after `pack_step_f32`:

```c
/* Symmetric per-matrix quantization of an f64 source into int8 codes. Returns
   the scale (max|w| / qmax); writes codes into q. */
static double quantize_matrix(const double *w, size_t n, int bits, int8_t *q) {
    size_t i;
    double amax = 0.0, scale;
    int qmax = (1 << (bits - 1)) - 1;   /* bits=8 -> 127, bits=2 -> 1 */
    if (qmax < 1) qmax = 1;
    for (i = 0; i < n; ++i) { double a = fabs(w[i]); if (a > amax) amax = a; }
    scale = (amax > 0.0) ? amax / (double)qmax : 1.0;
    for (i = 0; i < n; ++i) {
        long c = lround(w[i] / scale);
        if (c > qmax) c = qmax;
        if (c < -qmax) c = -qmax;
        q[i] = (int8_t)c;
    }
    return scale;
}

static int pack_step_int(FpStep *st, int bits) {
    st->w_ih_q = malloc(st->hid * st->in * sizeof(int8_t));
    st->w_ho_q = malloc(st->out * st->hid * sizeof(int8_t));
    if (!st->w_ih_q || !st->w_ho_q) return -1;
    st->w_ih_scale = quantize_matrix(st->w_ih, st->hid * st->in, bits, st->w_ih_q);
    st->w_ho_scale = quantize_matrix(st->w_ho, st->out * st->hid, bits, st->w_ho_q);
    return 0;
}
```

In `fp_route_compile`, after the f32 pack hook, add:

```c
        if (prec.kind == FP_INT && pack_step_int(st, prec.int_bits) != 0) {
            fp_route_free(cr); return NULL;
        }
```

Add the int forward after `step_forward_f32`:

```c
static void step_forward_int(const FpStep *st, const double *src, size_t n,
                             double *hidv, double *dst) {
    size_t k, i, h, o;
    int qmax = (1 << (st->int_bits - 1)) - 1;
    double aq = (qmax >= 1) ? (double)qmax : 1.0;   /* activation quant levels */
    for (k = 0; k < n; ++k) {
        const double *in = src + k * st->in;
        double *hv = hidv + k * st->hid;
        double *ov = dst + k * st->out;
        for (h = 0; h < st->hid; ++h) {
            long acc = 0;
            const int8_t *row = st->w_ih_q + h * st->in;
            for (i = 0; i < st->in; ++i) {
                long qa = lround(in[i] * aq);   /* in[i] in [0,1] */
                acc += (long)row[i] * qa;
            }
            hv[h] = fp_sigmoid((double)acc * st->w_ih_scale / aq + st->b_h[h]);
        }
        for (o = 0; o < st->out; ++o) {
            long acc = 0;
            const int8_t *row = st->w_ho_q + o * st->hid;
            for (h = 0; h < st->hid; ++h) {
                long qa = lround(hv[h] * aq);   /* hv[h] in (0,1) */
                acc += (long)row[h] * qa;
            }
            ov[o] = fp_sigmoid((double)acc * st->w_ho_scale / aq + st->b_o[o]);
        }
    }
}
```

In `fp_route_run`'s dispatch switch, add the int case:

```c
            case FP_INT: step_forward_int(st, cur, n, cr->hidv, nxt); break;
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS` — int8 certifies (0 mismatches, margin ≥ floor) and int2 is rejected (mismatches or margin collapse).

> If int8 unexpectedly shows mismatches on this route, that is a real finding, not a test bug: it means int8 is *not* faithful here and the gate correctly refuses it. In that case relax the int8 assertion to the gate form (`mism8 > 0 || mm8 < 0.25` is false ⇒ certified) and record the result for the study — but the README route's margins (~0.33) leave ample headroom for int8, so 0 mismatches is expected.

- [ ] **Step 5: Commit**

```bash
git add src/fastpath.c tests/test_fastpath.c
git commit -m "feat: fastpath int8 lane + margin gate bites on over-aggressive precision"
```

---

## Task 5: `throughput_study.exe` — the instrument

**Files:**
- Create: `tests/throughput_study.c`
- Modify: `Makefile` (add the `throughput_study` and `throughput` targets — the `THROUGHPUT_STUDY` var was added in Task 1)

- [ ] **Step 1: Write the study harness**

Create `tests/throughput_study.c`:

```c
/*
 * throughput_study.c -- how fast can the frozen route push bytes, and which
 * lever buys the speed? Runs the README hex_value -> increment route over a
 * large buffer through the double oracle (route_execute) and the fast lane at
 * each precision, and prints a capacity_results-style table:
 *
 *   lever | ns/sample | speedup vs L0 | mismatches/N | min margin | certified?
 *
 * Faithfulness bar: identical SNAPPED output to the oracle. A precision lever is
 * certified iff mismatches == 0 AND min margin >= FP_MARGIN_FLOOR. Phase 1
 * reports the verdict; it does not reject. Standalone study: NOT part of make
 * test. Loads the committed frozen hex_value/increment weights (run ./nn_demo).
 *
 * Timing note: this build is -mno-avx and single-threaded; the numbers are a
 * within-machine comparison of levers, not an absolute bytes/sec claim.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/fastpath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N      200000   /* samples in the buffer */
#define BATCH  1024     /* fast-lane window */
#define FP_MARGIN_FLOOR 0.25

static Port P(PortFamily f, size_t fw, size_t fc) {
    Port p; p.family = f; p.field_width = fw; p.field_count = fc;
    p.tag[0] = '\0'; return p;
}

/* A simple deterministic generator of valid one-hot-16 inputs. */
static void fill_inputs(double *in, size_t n) {
    size_t k;
    for (k = 0; k < n; ++k) {
        memset(in + k * 16, 0, 16 * sizeof(double));
        in[k * 16 + (k % 16)] = 1.0;
    }
}

static double now_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(void) {
    BinaryTransformNetwork hexval = {0}, incr = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    double *in = malloc((size_t)N * 16 * sizeof(double));
    double *oracle_out = malloc((size_t)N * 5 * sizeof(double));
    double *fast_out = malloc((size_t)N * 5 * sizeof(double));
    double t0, t1, base_ns = 0.0;
    size_t k;

    if (!in || !oracle_out || !fast_out) { fprintf(stderr, "OOM\n"); return 1; }
    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&incr, "increment_weights.txt") != 0) {
        fprintf(stderr, "could not load frozen primitives (run ./nn_demo).\n");
        return 1;
    }
    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &incr, "increment");
    if (route_plan(&reg, P(PORT_ONEHOT,16,1), P(PORT_BINARY_MSB,5,1), &plan) != 0) {
        fprintf(stderr, "no route\n"); return 1;
    }
    fill_inputs(in, N);

    printf("throughput study: hex_value -> increment over N=%d (batch=%d)\n\n", N, BATCH);
    printf(" lever      | ns/sample | speedup | mism/N  | min margin | certified\n");
    printf(" -----------|-----------|---------|---------|------------|----------\n");

    /* L0: the oracle (route_execute), per call (malloc/call, double). */
    t0 = now_seconds();
    for (k = 0; k < N; ++k)
        route_execute(&plan, in + k * 16, 16, oracle_out + k * 5, 5);
    t1 = now_seconds();
    base_ns = (t1 - t0) * 1e9 / N;
    printf(" L0 oracle  | %9.1f | %7s | %7d | %10s | %s\n",
           base_ns, "1.00x", 0, "-", "ref");

    /* L1..L4: compile once, run in batches; compare snapped output to oracle. */
    {
        struct { const char *name; FastPrecision prec; } levers[] = {
            { "L1 f64*1",  { FP_F64, 0 } },   /* batch=1: scratch reuse only */
            { "L2 f64",    { FP_F64, 0 } },   /* batched */
            { "L3 f32",    { FP_F32, 0 } },
            { "L4 int8",   { FP_INT, 8 } },
        };
        size_t L;
        for (L = 0; L < sizeof levers / sizeof levers[0]; ++L) {
            size_t batch = (L == 0) ? 1 : BATCH;
            CompiledRoute *cr = fp_route_compile(&plan, levers[L].prec, batch);
            size_t off, mism = 0;
            double ns, mm;
            if (cr == NULL) { printf(" %-10s | compile failed\n", levers[L].name); continue; }
            fp_route_reset_margin(cr);
            t0 = now_seconds();
            for (off = 0; off < (size_t)N; off += batch) {
                size_t cnt = ((size_t)N - off < batch) ? (size_t)N - off : batch;
                fp_route_run(cr, in + off * 16, cnt, fast_out + off * 5, cnt * 5);
            }
            t1 = now_seconds();
            ns = (t1 - t0) * 1e9 / N;
            for (k = 0; k < (size_t)N * 5; ++k)
                if (fast_out[k] != oracle_out[k]) { mism++; }
            mm = fp_route_min_margin(cr);
            printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                   levers[L].name, ns, base_ns / ns, (unsigned long)mism, mm,
                   (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
            fp_route_free(cr);
        }
    }

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
    free(in); free(oracle_out); free(fast_out);
    return 0;
}
```

> The mismatch count is over output *values* (N×5), not samples; that is fine for a faithfulness signal — any nonzero value means the snapped output diverged.

- [ ] **Step 2: Add the Makefile targets**

In `Makefile`, after the `stochastic` target block (around line 185) add:

```make
# Throughput: how fast can the frozen route push samples, and which lever buys
# it? Oracle (route_execute) vs the fast lane at each precision, faithfulness +
# margin reported. Budgeted study, NOT part of make test. Loads committed
# hex_value/increment weights (run ./nn_demo first).
throughput_study: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(THROUGHPUT_STUDY) include/nn.h include/router.h include/fastpath.h include/plan_table.h include/contract.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(THROUGHPUT_STUDY) $(LDFLAGS)

throughput: throughput_study
	./throughput_study
```

Add `throughput_study` (and `.exe`) to the `clean` rule's lists if not already added in Task 1.

- [ ] **Step 3: Build and run the study**

Run: `make throughput`
Expected: a printed table. `L0 oracle` is the reference; `L1 f64*1` should already beat it (no per-call malloc); `L2 f64` should beat `L1` (batched, weights resident) and show `mism/N = 0`, `certified YES`; `L3 f32` and `L4 int8` should show `0` mismatches and `min margin >= 0.25` ⇒ `certified YES`. Exact speedups are the finding — record them.

- [ ] **Step 4: Sanity-check the result, then commit**

Confirm `L2 f64` reports `mism 0` and `certified YES` (the batched double path must be exactly faithful). If any `double` lever shows mismatches, stop — the kernel has a reduction-order bug (see Task 2 note).

```bash
git add tests/throughput_study.c Makefile
git commit -m "feat: throughput_study -- the lever table (L0..L4) for bulk route decode"
```

---

## Task 6: L5 consolidation lever — collapse the route into one chunk

**Files:**
- Modify: `tests/throughput_study.c` (add the `#include`, the L5 block)
- Modify: `Makefile` (add `$(CONSOLIDATE)` + `include/consolidate.h` to the `throughput_study` target)

`consolidate_route` (`include/consolidate.h:63`) distills a length-≥2 route into one chunk primitive whose input port is the first step's and output port is the last step's — verified against the route over the enumerated domain. The chunk is a normal single-input/single-output BTN, so it runs through the **same** fast lane as a hand-built length-1 route: one forward pass, one snap, instead of two. This is the depth-collapse lever; the study measures whether it actually wins given the chunk's (larger) hidden width — the capacity tradeoff from `capacity_results.txt`.

- [ ] **Step 1: Add the consolidate include and the L5 block**

In `tests/throughput_study.c`, add after the existing includes:

```c
#include "../include/consolidate.h"
```

In `main`, immediately before `registry_free(&reg);`, add:

```c
    /* L5: consolidate the 2-step route into ONE chunk, run it as a length-1
       route through the same lane. Fewer snaps/byte; the chunk is wider, so the
       study tells us whether collapsing depth actually pays here. */
    {
        BinaryTransformNetwork chunk = {0};
        ConsolidateReport rep;
        if (consolidate_route(&plan, NULL, &chunk, &rep) != 0) {
            printf(" L5 chunk   | consolidation refused (verify rate below 1.0)\n");
        } else {
            RoutePlan cplan;
            FastPrecision prec = { FP_F64, 0 };
            CompiledRoute *cr;
            memset(&cplan, 0, sizeof cplan);
            cplan.steps[0] = &chunk;
            cplan.names[0] = "chunk";
            cplan.length = 1;
            cplan.goal = chunk.output_ports[0];
            cr = fp_route_compile(&cplan, prec, BATCH);
            if (cr == NULL) {
                printf(" L5 chunk   | compile failed\n");
            } else {
                size_t off, mism = 0;
                double ns, mm;
                fp_route_reset_margin(cr);
                t0 = now_seconds();
                for (off = 0; off < (size_t)N; off += BATCH) {
                    size_t cnt = ((size_t)N - off < BATCH) ? (size_t)N - off : BATCH;
                    fp_route_run(cr, in + off * 16, cnt, fast_out + off * 5, cnt * 5);
                }
                t1 = now_seconds();
                ns = (t1 - t0) * 1e9 / N;
                for (k = 0; k < (size_t)N * 5; ++k)
                    if (fast_out[k] != oracle_out[k]) mism++;
                mm = fp_route_min_margin(cr);
                printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                       "L5 chunk", ns, base_ns / ns, (unsigned long)mism, mm,
                       (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                printf("   (chunk %lu hidden, verified %lu/%lu; route was 2 steps)\n",
                       (unsigned long)chunk.hidden_count,
                       (unsigned long)rep.verified, (unsigned long)rep.samples);
                fp_route_free(cr);
            }
            btn_free(&chunk);
        }
    }
```

- [ ] **Step 2: Update the Makefile target to link consolidate.c**

In `Makefile`, replace the `throughput_study` target (added in Task 5) with this version, which adds `$(CONSOLIDATE)` and its header:

```make
throughput_study: $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(THROUGHPUT_STUDY) include/nn.h include/router.h include/consolidate.h include/fastpath.h include/plan_table.h include/contract.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(CONSOLIDATE) $(PLAN_TABLE) $(CONTRACT) $(FASTPATH) $(THROUGHPUT_STUDY) $(LDFLAGS)
```

- [ ] **Step 3: Build and run**

Run: `make throughput`
Expected: the table now has an `L5 chunk` row. It must show `mism 0` and `certified YES` (consolidation verified the chunk 16/16 against the route, so snapped outputs match the oracle). The speedup may be **above or below 1.0×** vs L0 — a wider single chunk can cost more MACs than two tiny steps even with one fewer snap. Either way is a real finding; the `(chunk N hidden …)` line records the capacity cost.

- [ ] **Step 4: Commit**

```bash
git add tests/throughput_study.c Makefile
git commit -m "feat: throughput L5 -- consolidate route into one chunk, measure depth-collapse"
```

---

## Deferred to the next plan (not in scope here)

- **DAG/decode fast-lane.** A batched `dag_compile`/`dag_run` for multi-input primitives (e.g. `combine(nibble, nibble) -> byte`) — the literal binary→bytes assembly. Reuses the pack + forward kernels here; adds multi-slot input assembly.
- **Phase 2 hardening.** Make `fp_route_compile` *reject* a precision that fails the floor (return NULL), add windowed streaming with O(window) memory and a windowed==one-shot test, and the public bulk API surface.

---

## Self-review

**Spec coverage:**
- Separate fast-lane executor, double core as oracle → Tasks 1–4 (`src/fastpath.c`, checked against `route_execute`). ✓
- Pack weights once, reuse scratch, no per-call malloc → `fp_route_compile` packs; `fp_route_run` reuses `cr->cur/nxt/hidv`. ✓
- Batched GEMM per primitive → `step_forward_*` over `n` samples; Task 2 proves it's bit-identical. ✓
- float32 / int8 unlocked, margin-certified → Tasks 3–4; `fp_route_min_margin` + `FP_MARGIN_FLOOR`. ✓
- Lever table in capacity_results style → Task 5. ✓
- Faithfulness = identical after snap; bit-identical for f64 → Task 1 (vs oracle), Task 2 (batched==single). ✓
- The gate bites (over-aggressive precision rejected) → Task 4 `test_int2_gate_bites`. ✓
- L5 (consolidation lever) → Task 6 (`consolidate_route` + length-1 route through the lane). ✓
- DAG/decode lane + Phase 2 hardening → explicitly deferred with rationale. ✓ (scope decision, surfaced to the user)

**Placeholder scan:** No TBD/TODO; every code step shows complete code; commands have expected output. ✓

**Type consistency:** `fp_route_compile`/`fp_route_run`/`fp_route_free`/`fp_route_min_margin`/`fp_route_reset_margin`/`fp_route_in_total`/`fp_route_out_total` and `FastPrecision{FastKind kind; int int_bits;}` are used identically in `fastpath.h`, `fastpath.c`, `test_fastpath.c`, and `throughput_study.c`. `FpStep`/`CompiledRoute` are internal to `fastpath.c`. Weight layout (`h*input_count+i`, `o*max_hidden_count+h`) matches `nn.c:614,622`. ✓
