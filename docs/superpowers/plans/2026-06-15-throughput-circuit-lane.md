# Throughput Circuit Lane — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the fast lane to batched **multi-root circuit** execution — several goals produced by one shared node graph, each node evaluated once across all roots — checked against `dag_execute_circuit`, demonstrated on `split(byte) → {hi nibble, lo nibble}` (one shared split execution, two roots).

**Architecture:** Add `fp_circuit_compile`/`fp_circuit_run`/`fp_circuit_free` to `src/fastpath.c`. A circuit reuses the DAG lane's node graph wholesale: `fp_circuit_compile` walks every root into ONE shared `CompiledDag` (the existing `fp_dag_walk` with a shared `seen[]` dedup table, so a node read by several roots is compiled — and later evaluated — exactly once), records each root's compiled-node index + projected output port, and `fp_circuit_run` runs the post-order node sweep once (a small `fp_dag_eval` extracted from `fp_dag_run`) then projects each root's segment, concatenated in goal order. The `double` core (`router.c`/`nn.c`) stays the oracle, untouched. This is the payoff of the DAG lane's true post-order fix: shared-node-once is already correct, so circuits are a thin extension.

**Tech Stack:** C11, GCC (`-std=c11 -O3 -march=native -mno-avx`), GNU make, libm. No new dependencies.

---

## Scope and the workload

The workload is `circuit_demo.c` Part 1: `dag_plan_circuit` over `split` (registered alone) with two `nibble_value` goals from one `byte_value` source discovers a circuit whose **two roots are the SAME split node** projected on ports 0 and 1 (`cp.roots[0] == cp.roots[1]`, `root_ports == {0,1}`). `dag_execute_circuit` runs it with ONE memo, recording a single outcome per byte. Output is the two nibble segments concatenated (8 values). Over committed `split_weights.txt` — no decimal weights needed.

This is the minimal circuit that exercises the new property: **one shared node feeding multiple roots, evaluated once.** The lane is general over any `CircuitPlan`; multi-primitive circuits (the discovered ripple-carry, Part 3) and `consolidate_circuit` (multi-output chunk distillation) need decimal weights and a richer structure — deferred (the split circuit is already a single shared node, so consolidating it has no structure to collapse).

## What is reused (do NOT reimplement)

From `src/fastpath.c` (the route + DAG lanes), reuse as-is:
- `FpDagNode`, `CompiledDag` (the node graph + scratch + `min_margin`).
- `fp_dag_count` (pre-count unique nodes, pointer-dedup, cap 64) — call per root with a shared `seen[]` to count the union.
- `fp_dag_walk` (true post-order compile, pointer-dedup) — call per root with a shared `seen[]` to compile the union; it already returns each root's index and updates `max_in`/`max_hid`/`max_out`.
- `fp_output_seg_off`, `fp_step_free_buffers`, `pack_step_*`, `step_forward_*`, `fp_dag_free`.
- **`fp_dag_eval`** — the per-node post-order sweep, extracted from `fp_dag_run` in Task 1 and shared by both lanes.

## The oracle's contract (mirror this exactly)

`dag_execute_circuit` (`src/router.c:1743`): one `EvalMemo` across every root, so a node read by several roots forwards once and records one outcome; each root's `root_segment` (its projected `root_ports[g]`) is written to `output` concatenated in goal order; `out_cap` must cover the total. The fast lane mirrors this: shared graph, one node sweep, root segments concatenated.

---

## Task 1: extract `fp_dag_eval` from `fp_dag_run` (refactor)

**Files:**
- Modify: `src/fastpath.c` (extract the node sweep; `fp_dag_run` calls it)

This is a behavior-preserving refactor; the existing DAG tests (`test_dag_f64_matches_oracle`, `test_dag_batched_equals_single_f64`, `test_dag_f32_matches_oracle`, `test_dag_int2_gate_bites`, `test_dag_shared_node_split`) are the regression gate.

- [ ] **Step 1: Add `fp_dag_eval` above `fp_dag_run`**

In `src/fastpath.c`, immediately BEFORE `int fp_dag_run(`, add:

```c
/* Evaluate every node once over n samples (post-order forward sweep): sources
   validate+canonicalize their batch; primitives assemble each slot from its
   child's projected segment, forward (kind-dispatched), and snap every output
   segment, folding the worst margin into cd->min_margin. No root projection.
   Assumes arguments already validated by the caller. Returns 0, or -1 on a
   bad/ambiguous handoff. */
static int fp_dag_eval(CompiledDag *cd, const double *const *src_batches, size_t n) {
    size_t ni, k;
    for (ni = 0; ni < cd->node_count; ++ni) {
        FpDagNode *fn = &cd->nodes[ni];
        if (fn->is_source) {
            const double *sb = src_batches[fn->source_index];
            size_t tot = fn->full_out;
            for (k = 0; k < n; ++k) {
                const double *v = sb + k * tot;
                double *dst = fn->nodebuf + k * tot;
                if (!port_validate(fn->src_type, v) ||
                    port_canonicalize(fn->src_type, v, dst) != 0) return -1;
            }
            continue;
        }
        /* primitive: assemble -> forward -> snap */
        for (k = 0; k < n; ++k) {
            size_t s, off = 0;
            double *asm_row = cd->assembled + k * fn->step.in;
            for (s = 0; s < fn->child_count; ++s) {
                const FpDagNode *ch = &cd->nodes[fn->child_node[s]];
                const double *seg = ch->nodebuf + k * ch->full_out + fn->child_seg_off[s];
                if (!port_validate(fn->slot_port[s], seg) ||
                    port_canonicalize(fn->slot_port[s], seg, asm_row + off) != 0)
                    return -1;
                off += fn->slot_total[s];
            }
        }
        switch (cd->prec.kind) {
            case FP_F32: step_forward_f32(&fn->step, cd->assembled, n, cd->hidv, cd->raw); break;
            case FP_INT: step_forward_int(&fn->step, cd->assembled, n, cd->hidv, cd->raw); break;
            default:     step_forward_f64(&fn->step, cd->assembled, n, cd->hidv, cd->raw); break;
        }
        for (k = 0; k < n; ++k) {
            size_t oj, off = 0;
            double *raw_row = cd->raw + k * fn->step.out;
            double *dst = fn->nodebuf + k * fn->full_out;
            for (oj = 0; oj < fn->out_port_count; ++oj) {
                double mm;
                size_t tot = fn->out_port[oj].field_width * fn->out_port[oj].field_count;
                if (port_margin(fn->out_port[oj], raw_row + off, &mm) == 0 &&
                    mm < cd->min_margin) cd->min_margin = mm;
                if (port_canonicalize(fn->out_port[oj], raw_row + off, dst + off) != 0)
                    return -1;
                off += tot;
            }
        }
    }
    return 0;
}
```

- [ ] **Step 2: Replace `fp_dag_run`'s body with checks + `fp_dag_eval` + projection**

Replace the entire current `fp_dag_run` function (the checks, the inline `for (ni ...)` sweep, and the root-projection block) with:

```c
int fp_dag_run(CompiledDag *cd, const double *const *src_batches, size_t n,
               double *out, size_t out_cap_total) {
    size_t k;
    if (cd == NULL || src_batches == NULL || out == NULL) return -1;
    if (n == 0 || n > cd->batch) return -1;
    if (out_cap_total < n * cd->root_seg_total) return -1;

    if (fp_dag_eval(cd, src_batches, n) != 0) return -1;

    {
        const FpDagNode *r = &cd->nodes[cd->root];
        for (k = 0; k < n; ++k)
            memcpy(out + k * cd->root_seg_total,
                   r->nodebuf + k * r->full_out + cd->root_seg_off,
                   cd->root_seg_total * sizeof(double));
    }
    return 0;
}
```

- [ ] **Step 3: Build + run the DAG tests to verify the refactor is behavior-preserving**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS` (all DAG tests still pass — the sweep is identical, just relocated). Build clean under `-Wall -Wextra -pedantic`.

- [ ] **Step 4: Commit**

```bash
git add src/fastpath.c
git commit -m "refactor: extract fp_dag_eval from fp_dag_run (shared by the circuit lane)"
```

---

## Task 2: `fp_circuit` lane (compile/run/free) + oracle equivalence

**Files:**
- Modify: `include/fastpath.h` (circuit API)
- Modify: `src/fastpath.c` (`CompiledCircuit` + the three functions + accessors)
- Modify: `tests/test_fastpath.c` (oracle-equivalence + shared-eval-once test)

- [ ] **Step 1: Add the circuit API to `include/fastpath.h`**

Insert before the final `#endif`:

```c
/* ---- batched multi-root circuit execution (mirrors dag_execute_circuit) ---- */

typedef struct CompiledCircuit CompiledCircuit;

/* Pack every root's node graph into `prec` ONCE over a SHARED graph (a node read
   by several roots is compiled, and evaluated, exactly once) and size scratch to
   `batch`. `source_types[i]` is the Port for circuit source i. Returns NULL on
   bad arguments or allocation failure. Reports margin; never rejects. */
CompiledCircuit *fp_circuit_compile(const CircuitPlan *plan, const Port *source_types,
                                    size_t n_sources, FastPrecision prec, size_t batch);

/* Run n (<= batch) samples. `src_batches[i]` points to n contiguous source-i
   vectors. Each root's projected segment is written concatenated in goal order to
   out + k*out_total for sample k (out_cap_total >= n*out_total). Returns 0, or -1
   on a bad/ambiguous handoff or argument. */
int fp_circuit_run(CompiledCircuit *cc, const double *const *src_batches, size_t n,
                   double *out, size_t out_cap_total);

double fp_circuit_min_margin(const CompiledCircuit *cc);
void   fp_circuit_reset_margin(CompiledCircuit *cc);
size_t fp_circuit_out_total(const CompiledCircuit *cc);
/* Number of distinct compiled nodes in the shared graph (a node shared by
   several roots counts once) -- lets a test confirm shared-eval-once. */
size_t fp_circuit_node_count(const CompiledCircuit *cc);

void fp_circuit_free(CompiledCircuit *cc);
```

- [ ] **Step 2: Write the failing test** in `tests/test_fastpath.c`

Add this test and call it from `main`. It mirrors `circuit_demo.c` Part 1, asserts the shared graph has exactly 2 nodes (byte source + one split), and compares `fp_circuit_run` to `dag_execute_circuit` over all 256 bytes:

```c
/* Tagged-port helper (circuits use semantic tags; port_validate ignores tags,
   but dag_plan_circuit needs them to match split's contract). */
static Port PT_(PortFamily family, size_t fw, size_t fc, const char *tag) {
    Port p; p.family = family; p.field_width = fw; p.field_count = fc;
    p.tag[0] = '\0';
    if (tag) port_set_tag(&p, tag);
    return p;
}

static void test_circuit_split_matches_oracle(void) {
    BinaryTransformNetwork split;
    PrimitiveRegistry reg;
    DagSource src[1];
    Port goals[2];
    CircuitPlan cp = {0};
    Port byte_t = PT_(PORT_BINARY_MSB, 8, 1, "byte_value");
    Port nib_t  = PT_(PORT_BINARY_MSB, 4, 1, "nibble_value");
    Port src_types[1];
    FastPrecision prec = { FP_F64, 0 };
    CompiledCircuit *cc;
    double seed[8] = {0};
    double *in_b, *fast_out;
    const double *sb[1];
    int b, bit, mism = 0;

    memset(&split, 0, sizeof split);
    if (btn_load(&split, "split_weights.txt") != 0) {
        printf("FAIL: load split (run ./nn_demo)\n"); ++failures; return;
    }
    registry_init(&reg);
    registry_add(&reg, &split, "split");
    src[0].type = byte_t; src[0].values = seed;
    goals[0] = nib_t; goals[1] = nib_t;
    if (dag_plan_circuit(&reg, src, 1, goals, 2, &cp) != 0) {
        printf("FAIL: no split circuit\n"); ++failures;
        registry_free(&reg); btn_free(&split); return;
    }

    src_types[0] = byte_t;
    cc = fp_circuit_compile(&cp, src_types, 1, prec, 256);
    CHECK(cc != NULL, "fp_circuit_compile");
    if (cc == NULL) { circuit_free(&cp); registry_free(&reg); btn_free(&split); return; }
    CHECK(fp_circuit_out_total(cc) == 8, "circuit out_total == 8");
    CHECK(fp_circuit_node_count(cc) == 2, "split shared: graph has 2 nodes (src + split)");

    in_b = calloc(256 * 8, sizeof(double));
    fast_out = calloc(256 * 8, sizeof(double));
    for (b = 0; b < 256; ++b)
        for (bit = 0; bit < 8; ++bit)
            in_b[b * 8 + bit] = (double)((b >> (7 - bit)) & 1);   /* MSB-first */
    sb[0] = in_b;

    CHECK(fp_circuit_run(cc, sb, 256, fast_out, 256 * 8) == 0, "fp_circuit_run f64");

    for (b = 0; b < 256; ++b) {
        double oo[8] = {0};
        src[0].values = in_b + b * 8;
        if (dag_execute_circuit(&cp, src, 1, oo, 8) != 0 ||
            memcmp(oo, fast_out + b * 8, sizeof oo) != 0) ++mism;
    }
    CHECK(mism == 0, "circuit f64 == oracle (256 bytes, split eval'd once)");

    free(in_b); free(fast_out);
    fp_circuit_free(cc);
    circuit_free(&cp);
    registry_free(&reg);
    btn_free(&split);
}
```

Add `test_circuit_split_matches_oracle();` to `main`.

- [ ] **Step 3: Run to verify it fails**

Run: `make test_fastpath`
Expected: link error — `undefined reference to fp_circuit_compile` / `fp_circuit_run` / `fp_circuit_free` / `fp_circuit_out_total` / `fp_circuit_node_count`.

- [ ] **Step 4: Implement the circuit lane** in `src/fastpath.c`

Add AFTER `fp_dag_run` (so `fp_dag_eval`, `fp_dag_walk`, `fp_dag_count`, `fp_output_seg_off`, `fp_dag_free` are all in scope):

```c
/* ===================== batched circuit lane ===================== */

struct CompiledCircuit {
    CompiledDag *g;                       /* the shared node graph (reuses DAG) */
    size_t roots[CIRCUIT_MAX_ROOTS];      /* compiled-node index per root */
    size_t root_off[CIRCUIT_MAX_ROOTS];   /* projected-segment offset per root */
    size_t root_total[CIRCUIT_MAX_ROOTS]; /* projected-segment width per root */
    size_t root_count;
    size_t out_total;                     /* sum of root_total */
};

double fp_circuit_min_margin(const CompiledCircuit *cc) {
    return cc ? cc->g->min_margin : 0.0;
}
void fp_circuit_reset_margin(CompiledCircuit *cc) {
    if (cc) cc->g->min_margin = 0.5;
}
size_t fp_circuit_out_total(const CompiledCircuit *cc) { return cc ? cc->out_total : 0; }
size_t fp_circuit_node_count(const CompiledCircuit *cc) {
    return cc ? cc->g->node_count : 0;
}

void fp_circuit_free(CompiledCircuit *cc) {
    if (cc == NULL) return;
    fp_dag_free(cc->g);   /* frees the graph's nodes + scratch + the CompiledDag */
    free(cc);
}

CompiledCircuit *fp_circuit_compile(const CircuitPlan *plan, const Port *source_types,
                                    size_t n_sources, FastPrecision prec, size_t batch) {
    CompiledCircuit *cc;
    CompiledDag *g;
    const DagNode *count_seen[64];
    const DagNode *seen[64];
    size_t total = 0, max_in = 0, max_hid = 0, max_out = 0, gi;

    if (plan == NULL || plan->root_count == 0 || source_types == NULL || batch == 0)
        return NULL;
    if (plan->root_count > CIRCUIT_MAX_ROOTS) return NULL;
    /* Count the UNION of nodes over all roots (shared dedup) to size the graph. */
    for (gi = 0; gi < plan->root_count; ++gi)
        if (plan->roots[gi] == NULL ||
            fp_dag_count(plan->roots[gi], count_seen, &total) != 0) return NULL;
    if (total == 0) return NULL;

    g = calloc(1, sizeof *g);
    if (g == NULL) return NULL;
    g->prec = prec; g->batch = batch; g->min_margin = 0.5;
    g->nodes = calloc(total, sizeof(FpDagNode));
    if (g->nodes == NULL) { free(g); return NULL; }

    cc = calloc(1, sizeof *cc);
    if (cc == NULL) { fp_dag_free(g); return NULL; }
    cc->g = g;
    cc->root_count = plan->root_count;

    /* Walk every root into the SHARED graph: a node read by several roots is
       compiled once (shared seen[]), so it is later evaluated once too. */
    for (gi = 0; gi < plan->root_count; ++gi) {
        const DagNode *r = plan->roots[gi];
        size_t idx = fp_dag_walk(g, r, seen, source_types, n_sources,
                                 &max_in, &max_hid, &max_out);
        if (idx == (size_t)-1) { fp_dag_free(g); free(cc); return NULL; }
        cc->roots[gi] = idx;
        if (r->kind == DAG_SOURCE) {
            cc->root_off[gi] = 0;
            cc->root_total[gi] = g->nodes[idx].full_out;
        } else {
            cc->root_off[gi] = fp_output_seg_off(r->btn, plan->root_ports[gi]);
            cc->root_total[gi] =
                r->btn->output_ports[plan->root_ports[gi]].field_width *
                r->btn->output_ports[plan->root_ports[gi]].field_count;
        }
        cc->out_total += cc->root_total[gi];
    }

    g->assembled = malloc(batch * (max_in  ? max_in  : 1) * sizeof(double));
    g->hidv      = malloc(batch * (max_hid ? max_hid : 1) * sizeof(double));
    g->raw       = malloc(batch * (max_out ? max_out : 1) * sizeof(double));
    if (!g->assembled || !g->hidv || !g->raw) { fp_dag_free(g); free(cc); return NULL; }
    return cc;
}

int fp_circuit_run(CompiledCircuit *cc, const double *const *src_batches, size_t n,
                   double *out, size_t out_cap_total) {
    size_t gi, k;
    if (cc == NULL || src_batches == NULL || out == NULL) return -1;
    if (n == 0 || n > cc->g->batch) return -1;
    if (out_cap_total < n * cc->out_total) return -1;

    if (fp_dag_eval(cc->g, src_batches, n) != 0) return -1;

    /* Project each root's segment, concatenated in goal order, per sample. */
    for (k = 0; k < n; ++k) {
        size_t concat = 0;
        for (gi = 0; gi < cc->root_count; ++gi) {
            const FpDagNode *r = &cc->g->nodes[cc->roots[gi]];
            memcpy(out + k * cc->out_total + concat,
                   r->nodebuf + k * r->full_out + cc->root_off[gi],
                   cc->root_total[gi] * sizeof(double));
            concat += cc->root_total[gi];
        }
    }
    return 0;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS`, including `split shared: graph has 2 nodes (src + split)` and `circuit f64 == oracle (256 bytes, split eval'd once)`.

- [ ] **Step 6: Run the full suite (route + DAG + circuit unaffected)**

Run: `make test`
Expected: all suites pass, ending `FASTPATH PASS`.

- [ ] **Step 7: Commit**

```bash
git add include/fastpath.h src/fastpath.c tests/test_fastpath.c
git commit -m "feat: fastpath circuit lane -- shared multi-root graph, oracle-checked"
```

---

## Task 3: circuit throughput rows in `throughput_study.c`

**Files:**
- Modify: `tests/throughput_study.c` (add a circuit table for the split byte circuit)

- [ ] **Step 1: Add the circuit study section.** In `tests/throughput_study.c`, after the DAG block and before `registry_free(&reg);`, add:

```c
    /* ===== Circuit lane: split(byte) -> {hi nibble, lo nibble}, one shared node ===== */
    {
        BinaryTransformNetwork csplit = {0};
        PrimitiveRegistry creg2;
        DagSource csrc[1];
        Port cgoals[2];
        CircuitPlan ccp = {0};
        Port byte_t = P(PORT_BINARY_MSB, 8, 1);
        Port nib_t  = P(PORT_BINARY_MSB, 4, 1);
        Port cstypes[1];
        double seed[8] = {0};
        double *cin = malloc((size_t)N * 8 * sizeof(double));
        double *cor = malloc((size_t)N * 8 * sizeof(double));   /* circuit oracle out */
        double *cfo = malloc((size_t)N * 8 * sizeof(double));   /* circuit fast out  */
        double cbase_ns = 0.0;
        size_t kk;

        /* split's circuit needs matching semantic tags. */
        port_set_tag(&byte_t, "byte_value");
        port_set_tag(&nib_t, "nibble_value");

        if (!cin || !cor || !cfo || btn_load(&csplit, "split_weights.txt") != 0) {
            fprintf(stderr, "skip circuit study (split load / OOM)\n");
        } else {
            registry_init(&creg2);
            registry_add(&creg2, &csplit, "split");
            csrc[0].type = byte_t; csrc[0].values = seed;
            cgoals[0] = nib_t; cgoals[1] = nib_t;
            cstypes[0] = byte_t;

            for (kk = 0; kk < (size_t)N; ++kk) {
                size_t bit; unsigned bv = (unsigned)(kk & 0xFF);
                for (bit = 0; bit < 8; ++bit)
                    cin[kk*8 + bit] = (double)((bv >> (7 - bit)) & 1u);
            }

            if (dag_plan_circuit(&creg2, csrc, 1, cgoals, 2, &ccp) != 0) {
                fprintf(stderr, "skip circuit study (no plan)\n");
            } else {
                struct { const char *name; FastPrecision prec; } clevers[] = {
                    { "C1 f64*1", { FP_F64, 0 } },
                    { "C2 f64",   { FP_F64, 0 } },
                    { "C3 f32",   { FP_F32, 0 } },
                    { "C4 int8",  { FP_INT, 8 } },
                };
                size_t L;

                printf("\nCIRCUIT: split(byte) -> {hi,lo} (one shared node), N=%d\n\n", N);
                printf(" lever      | ns/sample | speedup | mism/N  | min margin | certified\n");
                printf(" -----------|-----------|---------|---------|------------|----------\n");

                /* C0 oracle: dag_execute_circuit per sample (fills cor). */
                t0 = now_seconds();
                for (kk = 0; kk < (size_t)N; ++kk) {
                    csrc[0].values = cin + kk*8;
                    dag_execute_circuit(&ccp, csrc, 1, cor + kk*8, 8);
                }
                t1 = now_seconds();
                cbase_ns = (t1 - t0) * 1e9 / N;
                printf(" C0 oracle  | %9.1f | %7s | %7d | %10s | %s\n",
                       cbase_ns, "1.00x", 0, "-", "ref");

                for (L = 0; L < sizeof clevers / sizeof clevers[0]; ++L) {
                    size_t batch = (L == 0) ? 1 : BATCH, off, mism = 0;
                    int run_ok = 1; double ns, mm;
                    CompiledCircuit *cc = fp_circuit_compile(&ccp, cstypes, 1,
                                                             clevers[L].prec, batch);
                    if (cc == NULL) { printf(" %-10s | compile failed\n", clevers[L].name); continue; }
                    fp_circuit_reset_margin(cc);
                    t0 = now_seconds();
                    for (off = 0; off < (size_t)N; off += batch) {
                        size_t cnt = ((size_t)N - off < batch) ? (size_t)N - off : batch;
                        const double *w[1]; w[0] = cin + off*8;
                        if (fp_circuit_run(cc, w, cnt, cfo + off*8, cnt*8) != 0) run_ok = 0;
                    }
                    t1 = now_seconds();
                    if (run_ok) {
                        ns = (t1 - t0) * 1e9 / N;
                        for (kk = 0; kk < (size_t)N * 8; ++kk)
                            if (cfo[kk] != cor[kk]) mism++;
                        mm = fp_circuit_min_margin(cc);
                        printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                               clevers[L].name, ns, cbase_ns / ns, (unsigned long)mism, mm,
                               (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                    } else {
                        printf(" %-10s | run failed\n", clevers[L].name);
                    }
                    fp_circuit_free(cc);
                }
                circuit_free(&ccp);
            }
            registry_free(&creg2);
            btn_free(&csplit);
        }
        free(cin); free(cor); free(cfo);
    }
```

- [ ] **Step 2: Build and run**

Run: `make throughput`
Expected: route, DAG, then the CIRCUIT table (C0 oracle + C1..C4). `C2 f64` must be `mism 0` / certified YES. The circuit oracle does per-sample `dag_execute_circuit` (allocs + one memo per call), so the fast lane should win. Record the numbers.

- [ ] **Step 3: Commit**

```bash
git add tests/throughput_study.c
git commit -m "feat: throughput circuit table -- split(byte)->{hi,lo} shared-node vs oracle"
```

---

## Deferred to a later plan (not in scope here)

- **`consolidate_circuit` (C5).** Distilling a circuit into a multi-output chunk only pays on a multi-primitive circuit (the discovered ripple-carry, `circuit_demo.c` Part 3) — the split circuit is already a single shared node, so there is no structure to collapse. The ripple-carry workload needs the committed decimal weights (`dec_value`, `dec_full_add`) and a 20000-case domain; a separate plan.
- **Phase 2 hardening** (rejection-gate API, streaming with O(window) memory).

---

## Self-review

**Spec coverage:**
- Batched multi-root circuit execution mirroring `dag_execute_circuit` → Tasks 1–2 (`fp_circuit_compile`/`fp_circuit_run`/`fp_circuit_free`). ✓
- Shared node evaluated once across roots → Task 2 (shared `seen[]` in compile; one `fp_dag_eval` sweep; `fp_circuit_node_count(cc) == 2` asserts the split is compiled once). ✓
- Reuses the DAG graph machinery (no reimplementation) → `fp_dag_count`/`fp_dag_walk`/`fp_dag_eval`/`fp_output_seg_off`/`fp_dag_free` reused; only `fp_dag_eval` is newly extracted (Task 1). ✓
- Oracle equivalence over the split byte circuit (256 bytes) → Task 2. ✓
- Circuit throughput table C0–C4 → Task 3. ✓
- Core untouched (only `src/fastpath.c` additions + the `fp_dag_eval` extraction within it) → confirmed; `router.c`/`nn.c` unchanged. ✓
- `consolidate_circuit` + ripple-carry + Phase 2 → deferred with rationale. ✓

**Placeholder scan:** no TBD/TODO; every code step is complete; commands have expected output.

**Type consistency:** `fp_circuit_compile`/`fp_circuit_run`/`fp_circuit_free`/`fp_circuit_min_margin`/`fp_circuit_reset_margin`/`fp_circuit_out_total`/`fp_circuit_node_count` and `CompiledCircuit` are used identically across `fastpath.h`, `fastpath.c`, `test_fastpath.c`, `throughput_study.c`. `fp_circuit_run` takes `const double *const *src_batches`, mirroring `fp_dag_run`. The reused `fp_dag_walk`/`fp_dag_count`/`fp_dag_eval`/`fp_dag_free` signatures are unchanged by Task 1 except `fp_dag_eval` is newly added. `CIRCUIT_MAX_ROOTS` comes from `router.h`.

**One risk flagged for the implementer:** `fp_circuit_compile` reuses `CompiledDag` as the shared graph container and reuses `fp_dag_free` to release it; `fp_circuit_free` calls `fp_dag_free(cc->g)` then `free(cc)`. Do not double-free: on a compile failure path, free `g` via `fp_dag_free(g)` and `cc` via `free(cc)` exactly once each (the code does). `g->root`/`g->root_seg_*` are left zero (unused by circuits — projection uses `cc->roots[]`/`cc->root_off[]`/`cc->root_total[]`), which is fine.
