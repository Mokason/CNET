# Throughput DAG / Decode Lane — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the fast lane to batched **DAG** execution — the literal binary→bytes assembly `combine(hex_value, hex_value) → byte` — checked against `dag_execute` (the oracle), reusing the route lane's weight-packing and forward kernels and adding multi-slot input gather.

**Architecture:** Add `fp_dag_compile` / `fp_dag_run` / `fp_dag_free` to `src/fastpath.c`, mirroring `dag_execute`/`eval_node` over N samples. A compiled DAG is a post-order array of nodes (sources + primitives); each primitive node reuses the existing `FpStep` (packed weights) and the existing `step_forward_{f64,f32,int}` kernels (which are port-agnostic), and adds the DAG-specific topology: per-slot child links with output-port projection, and per-node N-row output buffers cached so a shared node evaluates once. The `double` core (`router.c`/`nn.c`) stays the oracle, untouched.

**Tech Stack:** C11, GCC (`-std=c11 -O3 -march=native -mno-avx`), GNU make, libm. No new dependencies.

---

## Scope and the workload

The workload is the README's byte-assembly DAG `combine(hex_value(src0), hex_value(src1)) → byte` (`two ONEHOT16 → BINARY_MSB8`), built with `dag_plan` exactly as `tests/dag_demo.c` does over the committed `hex_value_weights.txt` + `combine_weights.txt`. This is the literal "two hex digits → their byte" assembly the user's original example (`…→ 41 42 → AB`) is built from. It is a genuine 2-input DAG: it exercises multi-slot input gather and a shared-shape primitive reused on two branches — the new machinery this lane adds over the route lane.

The lane is **general** over any `DagPlan` (mirroring how the route lane is general over any `RoutePlan`), demonstrated on this DAG. Multi-output / shared-node circuits (`dag_execute_circuit`) and Phase-2 hardening (streaming, the rejection-gate API) remain deferred — the per-node memo in the design already handles shared nodes, so circuits are a thin future extension.

## What is reused (do NOT reimplement)

From the existing `src/fastpath.c` (the route lane), reuse as-is:
- `FpStep` — per-primitive packed weights (f64/f32/int) + `in`/`out`/`hid` counts.
- `pack_step_f64(FpStep*, const BinaryTransformNetwork*)`, `pack_step_f32(FpStep*)`, `pack_step_int(FpStep*, int bits)` — weight packing.
- `step_forward_f64/f32/int(const FpStep*, const double *src, size_t n, double *hidv, double *dst)` — the batched forward kernels. **These read only `st->in`/`st->hid`/`st->out` and the packed weights — they do NOT use `st->in_port`/`st->out_port`, so they work unchanged for a multi-input DAG primitive.**
- `fp_sigmoid`, `FastKind`, `FastPrecision`, `FP_MARGIN_FLOOR` (the latter lives in `tests/throughput_study.c`).

The DAG primitive's multi-port structure (slots, output segments) lives in the new `FpDagNode`, NOT in `FpStep`.

## The oracle's evaluation contract (mirror this exactly)

`dag_execute` → `eval_node` (`src/router.c:1526`), per node:
- **Source node:** `port_validate` then `port_canonicalize` the source's values against `sources[source_index].type`; the node's "full output" is the canonicalized source vector.
- **Primitive node:** assemble a flat `input_count` vector — for each slot `s`, take child `s`'s full output, and if that child is a primitive, slice the segment for the **edge's output port** (`edge_port(node,s)` → `output_segment`); `port_validate` the segment against the slot port `input_ports[s]`, then `port_canonicalize` it into the assembled vector at the running offset. `btn_forward`, then `port_canonicalize` **every** output segment into the node's full output.
- **Shared nodes evaluate once** (memo by node pointer).
- `root_segment` projects the root's `output_index` segment from the root's full output.

Weight layout, reduction order, and the margin floor are identical to the route lane (see `2026-06-15-throughput-fast-decode-design.md`).

---

## Task 1: `fp_dag` compile + run + free (f64) + oracle equivalence

**Files:**
- Modify: `include/fastpath.h` (add the DAG API)
- Modify: `src/fastpath.c` (add `FpDagNode`, `CompiledDag`, compile/run/free)
- Modify: `tests/test_fastpath.c` (add the f64 oracle-equivalence test)

- [ ] **Step 1: Add the DAG API to `include/fastpath.h`**

Insert before the final `#endif`:

```c
/* ---- batched DAG execution (mirrors dag_execute over N samples) ---- */

typedef struct CompiledDag CompiledDag;

/* Pack every primitive node's weights into `prec` ONCE and size scratch +
   per-node output buffers to `batch` samples. `source_types[i]` is the Port for
   DAG source i (the plan's source nodes carry only an index; the types come from
   the same array passed to dag_plan). Reads the plan's BTNs read-only. Returns
   NULL on bad arguments or allocation failure. Reports margin; never rejects. */
CompiledDag *fp_dag_compile(const DagPlan *plan, const Port *source_types,
                            size_t n_sources, FastPrecision prec, size_t batch);

/* Run n (<= batch) samples. `src_batches[i]` points to n contiguous source-i
   vectors (each source_types[i] total wide). The root's projected segment is
   written to out + k*out_total for sample k (out_cap_total >= n*out_total).
   Every handoff is validated + canonicalized like eval_node; the worst
   primitive-output margin folds into the running minimum. Returns 0, or -1 on a
   bad/ambiguous handoff or argument. */
int fp_dag_run(CompiledDag *cd, const double *const *src_batches, size_t n,
               double *out, size_t out_cap_total);

double fp_dag_min_margin(const CompiledDag *cd);
void   fp_dag_reset_margin(CompiledDag *cd);
size_t fp_dag_out_total(const CompiledDag *cd);

void fp_dag_free(CompiledDag *cd);
```

- [ ] **Step 2: Write the failing test** in `tests/test_fastpath.c`

Add this test and call it from `main` (before the `if (failures == 0)` block). It mirrors `dag_demo.c`'s setup, builds the plan once, and compares `fp_dag_run` to `dag_execute` over all 256 digit pairs:

```c
static void test_dag_f64_matches_oracle(void) {
    BinaryTransformNetwork hexval, combine;
    PrimitiveRegistry reg;
    DagSource sources[2];
    DagPlan plan = {0};
    Port onehot16 = P(PORT_ONEHOT, 16, 1);
    Port src_types[2];
    FastPrecision prec = { FP_F64, 0 };
    CompiledDag *cd;
    double seed_hi[16] = {0}, seed_lo[16] = {0};
    double *hi_batch, *lo_batch, *fast_out;
    const double *src_batches[2];
    int hi, lo, mism = 0;

    memset(&hexval, 0, sizeof hexval);
    memset(&combine, 0, sizeof combine);
    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&combine, "combine_weights.txt") != 0) {
        printf("FAIL: load dag primitives (run ./nn_demo)\n"); ++failures; return;
    }
    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &combine, "combine");

    /* Plan once with seed source values (dag_plan reads types, not values). */
    seed_hi[0] = 1.0; seed_lo[0] = 1.0;
    sources[0].type = onehot16; sources[0].values = seed_hi;
    sources[1].type = onehot16; sources[1].values = seed_lo;
    if (dag_plan(&reg, sources, 2, P(PORT_BINARY_MSB, 8, 1), &plan) != 0) {
        printf("FAIL: no DAG plan\n"); ++failures;
        registry_free(&reg); btn_free(&hexval); btn_free(&combine); return;
    }

    src_types[0] = onehot16; src_types[1] = onehot16;
    cd = fp_dag_compile(&plan, src_types, 2, prec, 256);
    CHECK(cd != NULL, "fp_dag_compile f64");
    CHECK(cd != NULL && fp_dag_out_total(cd) == 8, "dag out_total == 8");
    if (cd == NULL) { dag_free(&plan); registry_free(&reg);
                      btn_free(&hexval); btn_free(&combine); return; }

    /* 256 samples: all (hi,lo) one-hot pairs. */
    hi_batch = calloc(256 * 16, sizeof(double));
    lo_batch = calloc(256 * 16, sizeof(double));
    fast_out = calloc(256 * 8, sizeof(double));
    for (hi = 0; hi < 16; ++hi)
        for (lo = 0; lo < 16; ++lo) {
            size_t k = (size_t)hi * 16 + lo;
            hi_batch[k * 16 + hi] = 1.0;
            lo_batch[k * 16 + lo] = 1.0;
        }
    src_batches[0] = hi_batch; src_batches[1] = lo_batch;

    CHECK(fp_dag_run(cd, src_batches, 256, fast_out, 256 * 8) == 0, "fp_dag_run f64");

    for (hi = 0; hi < 16; ++hi)
        for (lo = 0; lo < 16; ++lo) {
            size_t k = (size_t)hi * 16 + lo;
            double want[8] = {0};
            sources[0].values = hi_batch + k * 16;
            sources[1].values = lo_batch + k * 16;
            if (dag_execute(&plan, sources, 2, want, 8) != 0 ||
                memcmp(want, fast_out + k * 8, sizeof want) != 0) ++mism;
        }
    CHECK(mism == 0, "dag f64 == oracle (all 256 pairs)");

    free(hi_batch); free(lo_batch); free(fast_out);
    fp_dag_free(cd);
    dag_free(&plan);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&combine);
}
```

`tests/test_fastpath.c` already includes `"../include/router.h"` and `<string.h>`/`<stdlib.h>`? It includes `nn.h`, `router.h`, `fastpath.h`, `<stdio.h>`, `<string.h>`. **Add `#include <stdlib.h>`** for `calloc`/`free` (the test uses them now). Add `test_dag_f64_matches_oracle();` to `main`.

- [ ] **Step 3: Run to verify it fails**

Run: `make test_fastpath`
Expected: link error — `undefined reference to fp_dag_compile` / `fp_dag_run` / `fp_dag_free` / `fp_dag_out_total` (not yet implemented).

- [ ] **Step 4: Implement the DAG lane** in `src/fastpath.c`

Add near the top of `src/fastpath.c`, after the existing `#include` lines, this include (for `DAG_MAX_SLOTS` etc. — already available via `router.h` which `fastpath.h` includes, so no new include needed). Then add the structures and functions AFTER the route lane's `fp_route_free` (so the reused statics `FpStep`, `pack_step_*`, `step_forward_*` are already declared above):

```c
/* ===================== batched DAG lane ===================== */

typedef struct {
    int is_source;
    /* source node */
    int source_index;
    Port src_type;
    /* primitive node (is_source == 0) */
    FpStep step;                          /* packed weights; reuses route kernels */
    size_t child_count;
    size_t child_node[DAG_MAX_SLOTS];     /* compiled index feeding slot s */
    size_t child_seg_off[DAG_MAX_SLOTS];  /* offset of the edge's segment in child full */
    Port   slot_port[DAG_MAX_SLOTS];      /* consuming slot's port */
    size_t slot_total[DAG_MAX_SLOTS];
    Port   out_port[BTN_MAX_OUTPUT_PORTS];/* output segments (snap + margin) */
    size_t out_port_count;
    size_t full_out;                      /* node full output total (per sample) */
    double *nodebuf;                      /* batch * full_out (cached node output) */
} FpDagNode;

struct CompiledDag {
    FastPrecision prec;
    size_t batch;
    FpDagNode *nodes;
    size_t node_count;
    size_t root;
    size_t root_seg_off, root_seg_total;
    size_t n_sources;
    /* scratch reused across nodes/samples */
    double *assembled;   /* batch * max_in  */
    double *hidv;        /* batch * max_hid */
    double *raw;         /* batch * max_out */
    double min_margin;
};

double fp_dag_min_margin(const CompiledDag *cd) { return cd ? cd->min_margin : 0.0; }
void   fp_dag_reset_margin(CompiledDag *cd) { if (cd) cd->min_margin = 0.5; }
size_t fp_dag_out_total(const CompiledDag *cd) { return cd ? cd->root_seg_total : 0; }

/* fp_step_free_buffers is defined with the route lane (above fp_route_free),
   which precedes this section, so it is already in scope here. */
void fp_dag_free(CompiledDag *cd) {
    size_t i;
    if (cd == NULL) return;
    if (cd->nodes) {
        for (i = 0; i < cd->node_count; ++i) {
            if (!cd->nodes[i].is_source) fp_step_free_buffers(&cd->nodes[i].step);
            free(cd->nodes[i].nodebuf);
        }
        free(cd->nodes);
    }
    free(cd->assembled); free(cd->hidv); free(cd->raw);
    free(cd);
}

/* Offset of output port `sel` within a btn's flat output (mirrors router.c
   output_segment). */
static size_t fp_output_seg_off(const BinaryTransformNetwork *p, int sel) {
    size_t off = 0; int oj;
    for (oj = 0; oj < sel && (size_t)oj < p->output_port_count; ++oj)
        off += p->output_ports[oj].field_width * p->output_ports[oj].field_count;
    return off;
}

/* Effective output port of children[k] (mirrors router.c edge_port). */
static int fp_edge_port(const DagNode *parent, size_t k) {
    int pp = parent->child_ports[k];
    return pp != 0 ? pp : parent->children[k]->output_index;
}

/* Post-order walk with pointer-dedup (children before parents). `seen[i]` is the
   DagNode* compiled at node index i. Returns the node's index, or (size_t)-1 on
   failure -- partial state is cleaned up by fp_dag_free on the caller's error
   path, since every FpDagNode is calloc-zeroed and all buffers free NULL-safely. */
static size_t fp_dag_walk(CompiledDag *cd, const DagNode *node,
                          const DagNode **seen, const Port *source_types,
                          size_t n_sources, size_t *max_in, size_t *max_hid,
                          size_t *max_out) {
    size_t i, idx;
    FpDagNode *fn;

    if (node == NULL) return (size_t)-1;
    for (i = 0; i < cd->node_count; ++i)
        if (seen[i] == node) return i;          /* shared node already compiled */

    idx = cd->node_count++;
    seen[idx] = node;
    fn = &cd->nodes[idx];                        /* already calloc-zeroed */

    if (node->kind == DAG_SOURCE) {
        if (node->source_index < 0 || (size_t)node->source_index >= n_sources)
            return (size_t)-1;
        fn->is_source = 1;
        fn->source_index = node->source_index;
        fn->src_type = source_types[node->source_index];
        fn->full_out = fn->src_type.field_width * fn->src_type.field_count;
    } else {
        const BinaryTransformNetwork *p = node->btn;
        size_t s;
        if (p->input_port_count > DAG_MAX_SLOTS) return (size_t)-1;
        if (pack_step_f64(&fn->step, p) != 0) return (size_t)-1;
        if (cd->prec.kind == FP_F32 && pack_step_f32(&fn->step) != 0) return (size_t)-1;
        if (cd->prec.kind == FP_INT &&
            pack_step_int(&fn->step, cd->prec.int_bits) != 0) return (size_t)-1;
        fn->step.int_bits = cd->prec.int_bits;
        fn->child_count = node->child_count;
        fn->out_port_count = p->output_port_count;
        for (s = 0; s < p->output_port_count; ++s) fn->out_port[s] = p->output_ports[s];
        fn->full_out = p->output_count;
        if (p->input_count  > *max_in)  *max_in  = p->input_count;
        if (p->hidden_count > *max_hid) *max_hid = p->hidden_count;
        if (p->output_count > *max_out) *max_out = p->output_count;
        for (s = 0; s < node->child_count; ++s) {
            const DagNode *child = node->children[s];
            size_t cidx = fp_dag_walk(cd, child, seen, source_types, n_sources,
                                      max_in, max_hid, max_out);
            if (cidx == (size_t)-1) return (size_t)-1;
            fn = &cd->nodes[idx];               /* nodes array is fixed; ptr stable */
            fn->child_node[s] = cidx;
            fn->slot_port[s] = p->input_ports[s];
            fn->slot_total[s] = p->input_ports[s].field_width *
                                p->input_ports[s].field_count;
            fn->child_seg_off[s] = (child->kind == DAG_PRIMITIVE)
                ? fp_output_seg_off(child->btn, fp_edge_port(node, s)) : 0;
        }
    }

    /* Per-node output buffer, sized now that full_out is known. */
    fn = &cd->nodes[idx];
    fn->nodebuf = malloc(cd->batch * (fn->full_out ? fn->full_out : 1) * sizeof(double));
    if (fn->nodebuf == NULL) return (size_t)-1;
    return idx;
}

/* Pre-count unique nodes (pointer dedup) to size the array; -1 if > 64. */
static int fp_dag_count(const DagNode *node, const DagNode **seen, size_t *cnt) {
    size_t i;
    if (node == NULL) return -1;
    for (i = 0; i < *cnt; ++i) if (seen[i] == node) return 0;
    if (*cnt >= 64) return -1;
    seen[(*cnt)++] = node;
    if (node->kind == DAG_PRIMITIVE) {
        size_t s;
        for (s = 0; s < node->child_count; ++s)
            if (fp_dag_count(node->children[s], seen, cnt) != 0) return -1;
    }
    return 0;
}

CompiledDag *fp_dag_compile(const DagPlan *plan, const Port *source_types,
                            size_t n_sources, FastPrecision prec, size_t batch) {
    CompiledDag *cd;
    const DagNode *count_seen[64];
    const DagNode *seen[64];
    size_t total = 0, max_in = 0, max_hid = 0, max_out = 0;
    const DagNode *r;

    if (plan == NULL || plan->root == NULL || source_types == NULL || batch == 0)
        return NULL;
    if (fp_dag_count(plan->root, count_seen, &total) != 0 || total == 0)
        return NULL;

    cd = calloc(1, sizeof *cd);
    if (cd == NULL) return NULL;
    cd->prec = prec; cd->batch = batch; cd->n_sources = n_sources;
    cd->min_margin = 0.5;
    cd->nodes = calloc(total, sizeof(FpDagNode));   /* fixed array, never realloc'd */
    if (cd->nodes == NULL) { free(cd); return NULL; }

    /* One pass: build every node and allocate its buffer; fp_dag_free cleans up
       any partial state on failure (calloc-zeroed nodes, NULL-safe frees). */
    cd->root = fp_dag_walk(cd, plan->root, seen, source_types, n_sources,
                           &max_in, &max_hid, &max_out);
    if (cd->root == (size_t)-1) { fp_dag_free(cd); return NULL; }

    r = plan->root;
    if (r->kind == DAG_SOURCE) {
        cd->root_seg_off = 0;
        cd->root_seg_total = cd->nodes[cd->root].full_out;
    } else {
        cd->root_seg_off = fp_output_seg_off(r->btn, r->output_index);
        cd->root_seg_total = r->btn->output_ports[r->output_index].field_width *
                             r->btn->output_ports[r->output_index].field_count;
    }

    cd->assembled = malloc(batch * (max_in  ? max_in  : 1) * sizeof(double));
    cd->hidv      = malloc(batch * (max_hid ? max_hid : 1) * sizeof(double));
    cd->raw       = malloc(batch * (max_out ? max_out : 1) * sizeof(double));
    if (!cd->assembled || !cd->hidv || !cd->raw) { fp_dag_free(cd); return NULL; }
    return cd;
}

int fp_dag_run(CompiledDag *cd, const double *const *src_batches, size_t n,
               double *out, size_t out_cap_total) {
    size_t ni, k;
    if (cd == NULL || src_batches == NULL || out == NULL) return -1;
    if (n == 0 || n > cd->batch) return -1;
    if (out_cap_total < n * cd->root_seg_total) return -1;

    /* Nodes are in post-order (children before parents), so a single forward
       sweep evaluates each exactly once. */
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

Two supporting refactors needed for the above to compile:

(a) **Factor the route lane's per-step buffer free into `fp_step_free_buffers`** so both lanes share it. In `src/fastpath.c`, replace the body of the per-step free loop inside `fp_route_free` with a call, and define the helper. Find the loop in `fp_route_free` that does `free(st->w_ih); free(st->w_ho); ...` and replace those frees with `fp_step_free_buffers(st);`, then add above `fp_route_free`:

```c
static void fp_step_free_buffers(FpStep *st) {
    free(st->w_ih); free(st->w_ho); free(st->b_h); free(st->b_o);
    free(st->w_ih_f); free(st->w_ho_f); free(st->b_h_f); free(st->b_o_f);
    free(st->w_ih_q); free(st->w_ho_q);
}
```

(b) **Single cleanup path.** `fp_dag_free` handles both success and walk-failure: `cd->nodes` is `calloc`-zeroed, so a node that failed mid-build has `is_source == 0` with NULL step buffers and a NULL `nodebuf`, and every `free`/`fp_step_free_buffers` is NULL-safe. So `fp_dag_compile` just calls `fp_dag_free(cd)` on any failure — there is no separate partial-free routine and no pointer-tag trick. The `seen[]` dedup table is a plain local array in `fp_dag_compile`, passed into `fp_dag_walk`; the `nodes` array is sized up front by `fp_dag_count` (≤ 64) so `&cd->nodes[idx]` stays valid across the recursive walk.

- [ ] **Step 5: Run to verify it passes**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS` (now including `dag f64 == oracle (all 256 pairs)`).

> If `dag f64 == oracle` fails, the gather/projection is wrong — verify `child_seg_off` uses `fp_edge_port` only for primitive children and the assembled offset advances by `slot_total`.

- [ ] **Step 6: Commit**

```bash
git add include/fastpath.h src/fastpath.c tests/test_fastpath.c
git commit -m "feat: fastpath DAG lane (f64) -- batched eval_node, oracle-checked"
```

---

## Task 2: batched bit-identical (f64) + f32 faithful + int2 gate-bites (DAG)

**Files:**
- Modify: `tests/test_fastpath.c` (three small tests; f32/int come free via the dispatch in `fp_dag_run`)

- [ ] **Step 1: Write the tests** and call all three from `main`.

```c
/* Shared helper: build the committed combine(hex_value,hex_value)->byte plan.
   Returns 0 on success; fills *plan/*reg/the two btns (caller frees). */
static int load_byte_dag(BinaryTransformNetwork *hexval,
                         BinaryTransformNetwork *combine,
                         PrimitiveRegistry *reg, DagPlan *plan,
                         DagSource sources[2], double *seed_hi, double *seed_lo) {
    Port onehot16 = P(PORT_ONEHOT, 16, 1);
    memset(hexval, 0, sizeof *hexval);
    memset(combine, 0, sizeof *combine);
    if (btn_load(hexval, "hex_value_weights.txt") != 0 ||
        btn_load(combine, "combine_weights.txt") != 0) return -1;
    registry_init(reg);
    registry_add(reg, hexval, "hex_value");
    registry_add(reg, combine, "combine");
    seed_hi[0] = 1.0; seed_lo[0] = 1.0;
    sources[0].type = onehot16; sources[0].values = seed_hi;
    sources[1].type = onehot16; sources[1].values = seed_lo;
    return dag_plan(reg, sources, 2, P(PORT_BINARY_MSB, 8, 1), plan);
}

/* Fill 256 (hi,lo) one-hot pairs into hi_batch/lo_batch (each 256*16). */
static void fill_pairs(double *hi_batch, double *lo_batch) {
    int hi, lo;
    for (hi = 0; hi < 16; ++hi)
        for (lo = 0; lo < 16; ++lo) {
            size_t k = (size_t)hi * 16 + lo;
            hi_batch[k * 16 + hi] = 1.0;
            lo_batch[k * 16 + lo] = 1.0;
        }
}

static void test_dag_batched_equals_single_f64(void) {
    BinaryTransformNetwork hexval, combine; PrimitiveRegistry reg; DagPlan plan = {0};
    DagSource sources[2]; double seed_hi[16] = {0}, seed_lo[16] = {0};
    Port st[2]; FastPrecision prec = { FP_F64, 0 }; CompiledDag *cd;
    double *hi_b, *lo_b, *batched, *single; const double *sb[2]; size_t k;

    if (load_byte_dag(&hexval, &combine, &reg, &plan, sources, seed_hi, seed_lo) != 0) {
        printf("FAIL: byte dag setup (batch)\n"); ++failures; return;
    }
    st[0] = P(PORT_ONEHOT,16,1); st[1] = st[0];
    cd = fp_dag_compile(&plan, st, 2, prec, 256);
    CHECK(cd != NULL, "dag compile batch=256");
    if (cd == NULL) { dag_free(&plan); registry_free(&reg);
                      btn_free(&hexval); btn_free(&combine); return; }
    hi_b = calloc(256*16, sizeof(double)); lo_b = calloc(256*16, sizeof(double));
    batched = calloc(256*8, sizeof(double)); single = calloc(256*8, sizeof(double));
    fill_pairs(hi_b, lo_b); sb[0] = hi_b; sb[1] = lo_b;

    CHECK(fp_dag_run(cd, sb, 256, batched, 256*8) == 0, "dag batched run");
    for (k = 0; k < 256; ++k) {
        const double *one[2]; one[0] = hi_b + k*16; one[1] = lo_b + k*16;
        CHECK(fp_dag_run(cd, one, 1, single + k*8, 8) == 0, "dag single run");
    }
    CHECK(memcmp(batched, single, 256*8*sizeof(double)) == 0,
          "dag batched f64 == single f64 (bit-identical)");

    free(hi_b); free(lo_b); free(batched); free(single);
    fp_dag_free(cd); dag_free(&plan); registry_free(&reg);
    btn_free(&hexval); btn_free(&combine);
}

/* mismatches of fp_dag at `prec` vs dag_execute over the 256 pairs; returns min margin. */
static double dag_run_mismatches(DagPlan *plan, DagSource sources[2],
                                 FastPrecision prec, int *out_mism) {
    Port st[2]; CompiledDag *cd; double *hi_b, *lo_b, *fo; const double *sb[2];
    int hi, lo, mism = 0; double mm;
    st[0] = P(PORT_ONEHOT,16,1); st[1] = st[0];
    cd = fp_dag_compile(plan, st, 2, prec, 256);
    if (cd == NULL) { *out_mism = 9999; return 0.0; }
    hi_b = calloc(256*16, sizeof(double)); lo_b = calloc(256*16, sizeof(double));
    fo = calloc(256*8, sizeof(double));
    fill_pairs(hi_b, lo_b); sb[0] = hi_b; sb[1] = lo_b;
    fp_dag_reset_margin(cd);
    fp_dag_run(cd, sb, 256, fo, 256*8);
    for (hi = 0; hi < 16; ++hi) for (lo = 0; lo < 16; ++lo) {
        size_t k = (size_t)hi*16 + lo; double want[8] = {0};
        sources[0].values = hi_b + k*16; sources[1].values = lo_b + k*16;
        if (dag_execute(plan, sources, 2, want, 8) != 0 ||
            memcmp(want, fo + k*8, sizeof want) != 0) ++mism;
    }
    mm = fp_dag_min_margin(cd);
    *out_mism = mism;
    free(hi_b); free(lo_b); free(fo); fp_dag_free(cd);
    return mm;
}

static void test_dag_f32_matches_oracle(void) {
    BinaryTransformNetwork hexval, combine; PrimitiveRegistry reg; DagPlan plan = {0};
    DagSource sources[2]; double seed_hi[16] = {0}, seed_lo[16] = {0};
    FastPrecision prec = { FP_F32, 0 }; int mism = 0; double mm;
    if (load_byte_dag(&hexval, &combine, &reg, &plan, sources, seed_hi, seed_lo) != 0) {
        printf("FAIL: byte dag setup (f32)\n"); ++failures; return;
    }
    mm = dag_run_mismatches(&plan, sources, prec, &mism);
    CHECK(mism == 0, "dag f32 == oracle (256 pairs)");
    CHECK(mm >= 0.25, "dag f32 min margin >= floor");
    dag_free(&plan); registry_free(&reg); btn_free(&hexval); btn_free(&combine);
}

static void test_dag_int2_gate_bites(void) {
    BinaryTransformNetwork hexval, combine; PrimitiveRegistry reg; DagPlan plan = {0};
    DagSource sources[2]; double seed_hi[16] = {0}, seed_lo[16] = {0};
    FastPrecision prec = { FP_INT, 2 }; int mism = 0; double mm;
    if (load_byte_dag(&hexval, &combine, &reg, &plan, sources, seed_hi, seed_lo) != 0) {
        printf("FAIL: byte dag setup (int2)\n"); ++failures; return;
    }
    mm = dag_run_mismatches(&plan, sources, prec, &mism);
    CHECK(mism > 0 || mm < 0.25, "dag int2 is rejected (gate bites)");
    dag_free(&plan); registry_free(&reg); btn_free(&hexval); btn_free(&combine);
}
```

Add `test_dag_batched_equals_single_f64();`, `test_dag_f32_matches_oracle();`, `test_dag_int2_gate_bites();` to `main`.

- [ ] **Step 2: Run to verify it passes**

Run: `make test_fastpath && ./test_fastpath`
Expected: `FASTPATH PASS`. (f32/int8 ride the same `fp_dag_run` dispatch added in Task 1, so no new production code is needed here — only tests.)

- [ ] **Step 3: Commit**

```bash
git add tests/test_fastpath.c
git commit -m "test: dag lane batched-bit-identical, f32 faithful, int2 gate-bites"
```

---

## Task 3: DAG throughput rows in `throughput_study.c`

**Files:**
- Modify: `tests/throughput_study.c` (add a second table for the byte DAG)

- [ ] **Step 1: Add the DAG study section.** In `tests/throughput_study.c`, after the L5 route block and before `registry_free(&reg);`, add a self-contained DAG study (loads `combine`, plans the byte DAG, times the oracle vs the fast lane). It reuses `now_seconds`, `FP_MARGIN_FLOOR`, `N`, `BATCH`.

```c
    /* ===== DAG lane: combine(hex_value, hex_value) -> byte ===== */
    {
        BinaryTransformNetwork combine = {0};
        PrimitiveRegistry dreg;
        DagSource dsrc[2];
        DagPlan dplan = {0};
        Port onehot16 = P(PORT_ONEHOT, 16, 1);
        Port stypes[2];
        double seed_hi[16] = {0}, seed_lo[16] = {0};
        double *hi_b = malloc((size_t)N * 16 * sizeof(double));
        double *lo_b = malloc((size_t)N * 16 * sizeof(double));
        double *dor  = malloc((size_t)N * 8 * sizeof(double));   /* dag oracle out */
        double *dfo  = malloc((size_t)N * 8 * sizeof(double));   /* dag fast out  */
        double dbase_ns = 0.0;
        size_t kk;

        if (!hi_b || !lo_b || !dor || !dfo ||
            btn_load(&combine, "combine_weights.txt") != 0) {
            fprintf(stderr, "skip DAG study (combine load / OOM)\n");
        } else {
            registry_init(&dreg);
            registry_add(&dreg, &hexval, "hex_value");   /* hexval already loaded above */
            registry_add(&dreg, &combine, "combine");
            seed_hi[0] = 1.0; seed_lo[0] = 1.0;
            dsrc[0].type = onehot16; dsrc[0].values = seed_hi;
            dsrc[1].type = onehot16; dsrc[1].values = seed_lo;
            stypes[0] = onehot16; stypes[1] = onehot16;

            for (kk = 0; kk < (size_t)N; ++kk) {
                memset(hi_b + kk*16, 0, 16*sizeof(double));
                memset(lo_b + kk*16, 0, 16*sizeof(double));
                hi_b[kk*16 + (kk % 16)] = 1.0;
                lo_b[kk*16 + ((kk/16) % 16)] = 1.0;
            }

            if (dag_plan(&dreg, dsrc, 2, P(PORT_BINARY_MSB,8,1), &dplan) != 0) {
                fprintf(stderr, "skip DAG study (no plan)\n");
            } else {
                struct { const char *name; FastPrecision prec; } dlevers[] = {
                    { "D1 f64*1", { FP_F64, 0 } },
                    { "D2 f64",   { FP_F64, 0 } },
                    { "D3 f32",   { FP_F32, 0 } },
                    { "D4 int8",  { FP_INT, 8 } },
                };
                size_t L;
                const double *sb[2]; sb[0] = hi_b; sb[1] = lo_b;

                printf("\nDAG: combine(hex_value,hex_value) -> byte, N=%d\n\n", N);
                printf(" lever      | ns/sample | speedup | mism/N  | min margin | certified\n");
                printf(" -----------|-----------|---------|---------|------------|----------\n");

                /* D0 oracle: dag_execute per sample (fills dor). */
                t0 = now_seconds();
                for (kk = 0; kk < (size_t)N; ++kk) {
                    dsrc[0].values = hi_b + kk*16; dsrc[1].values = lo_b + kk*16;
                    dag_execute(&dplan, dsrc, 2, dor + kk*8, 8);
                }
                t1 = now_seconds();
                dbase_ns = (t1 - t0) * 1e9 / N;
                printf(" D0 oracle  | %9.1f | %7s | %7d | %10s | %s\n",
                       dbase_ns, "1.00x", 0, "-", "ref");

                for (L = 0; L < sizeof dlevers / sizeof dlevers[0]; ++L) {
                    size_t batch = (L == 0) ? 1 : BATCH, off, mism = 0;
                    int run_ok = 1; double ns, mm;
                    CompiledDag *cd = fp_dag_compile(&dplan, stypes, 2, dlevers[L].prec, batch);
                    if (cd == NULL) { printf(" %-10s | compile failed\n", dlevers[L].name); continue; }
                    fp_dag_reset_margin(cd);
                    t0 = now_seconds();
                    for (off = 0; off < (size_t)N; off += batch) {
                        size_t cnt = ((size_t)N - off < batch) ? (size_t)N - off : batch;
                        const double *w[2]; w[0] = hi_b + off*16; w[1] = lo_b + off*16;
                        if (fp_dag_run(cd, w, cnt, dfo + off*8, cnt*8) != 0) run_ok = 0;
                    }
                    t1 = now_seconds();
                    if (run_ok) {
                        ns = (t1 - t0) * 1e9 / N;
                        for (kk = 0; kk < (size_t)N * 8; ++kk)
                            if (dfo[kk] != dor[kk]) mism++;
                        mm = fp_dag_min_margin(cd);
                        printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                               dlevers[L].name, ns, dbase_ns / ns, (unsigned long)mism, mm,
                               (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                    } else {
                        printf(" %-10s | run failed\n", dlevers[L].name);
                    }
                    fp_dag_free(cd);
                }
                dag_free(&dplan);
            }
            registry_free(&dreg);
            btn_free(&combine);
        }
        free(hi_b); free(lo_b); free(dor); free(dfo);
    }
```

Note: `hexval` is the `hex_value` BTN already loaded at the top of `main` for the route study; the DAG registry borrows it (registries borrow, they don't own). Do not free `hexval` twice — it's freed once at the end of `main`.

- [ ] **Step 2: Build and run**

Run: `make throughput`
Expected: the route table prints, then the DAG table. The DAG oracle (D0) does per-sample `dag_execute` with several `calloc`s per node per sample, so the fast lane should win **more** here than on the route. `D2 f64` must be `mism 0` / `certified YES`. Record the numbers.

- [ ] **Step 3: Commit**

```bash
git add tests/throughput_study.c
git commit -m "feat: throughput DAG table -- batched combine(hex_value,hex_value)->byte vs oracle"
```

---

## Task 4: L5 — consolidate the byte DAG into one chunk

**Files:**
- Modify: `tests/throughput_study.c` (add the D5 row inside the DAG block)

`consolidate_dag(&dplan, dsrc, 2, NULL, &chunk, &rep)` distills the DAG into one 2-input chunk (input ports = the two sources, output port = the byte). Registering that chunk alone and re-planning the byte goal yields a 1-primitive DAG `chunk(src0, src1)`, run through the same `fp_dag` lane.

- [ ] **Step 1: Add the D5 block** inside the DAG `{ ... }` block, after the `dlevers` loop and before `dag_free(&dplan);`:

```c
                /* D5: consolidate the DAG into ONE 2-input chunk, run it as a
                   1-primitive DAG through the same lane. */
                {
                    BinaryTransformNetwork chunk = {0};
                    ConsolidateReport rep;
                    if (consolidate_dag(&dplan, dsrc, 2, NULL, &chunk, &rep) != 0) {
                        printf(" D5 chunk   | consolidation refused\n");
                    } else {
                        PrimitiveRegistry creg;
                        DagPlan cplan = {0};
                        registry_init(&creg);
                        registry_add(&creg, &chunk, "byte_chunk");
                        if (dag_plan(&creg, dsrc, 2, P(PORT_BINARY_MSB,8,1), &cplan) != 0) {
                            printf(" D5 chunk   | replan failed\n");
                        } else {
                            CompiledDag *cd = fp_dag_compile(&cplan, stypes, 2,
                                                             (FastPrecision){FP_F64,0}, BATCH);
                            if (cd == NULL) { printf(" D5 chunk   | compile failed\n"); }
                            else {
                                size_t off, mism = 0; int run_ok = 1; double ns, mm;
                                fp_dag_reset_margin(cd);
                                t0 = now_seconds();
                                for (off = 0; off < (size_t)N; off += BATCH) {
                                    size_t cnt = ((size_t)N - off < BATCH) ? (size_t)N - off : BATCH;
                                    const double *w[2]; w[0] = hi_b + off*16; w[1] = lo_b + off*16;
                                    if (fp_dag_run(cd, w, cnt, dfo + off*8, cnt*8) != 0) run_ok = 0;
                                }
                                t1 = now_seconds();
                                if (run_ok) {
                                    ns = (t1 - t0) * 1e9 / N;
                                    for (kk = 0; kk < (size_t)N * 8; ++kk)
                                        if (dfo[kk] != dor[kk]) mism++;
                                    mm = fp_dag_min_margin(cd);
                                    printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                                           "D5 chunk", ns, dbase_ns / ns, (unsigned long)mism, mm,
                                           (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                                    printf("   (chunk %lu hidden, verified %lu/%lu, teacher-aborts %lu)\n",
                                           (unsigned long)chunk.hidden_count,
                                           (unsigned long)rep.verified, (unsigned long)rep.samples,
                                           (unsigned long)rep.teacher_aborts);
                                } else {
                                    printf(" D5 chunk   | run failed\n");
                                }
                                fp_dag_free(cd);
                            }
                            dag_free(&cplan);
                        }
                        registry_free(&creg);
                        btn_free(&chunk);
                    }
                }
```

`tests/throughput_study.c` already includes `"../include/consolidate.h"` (added by the route plan's Task 6) and the `throughput_study` Makefile target already links `$(CONSOLIDATE)`. No Makefile change needed.

- [ ] **Step 2: Build and run**

Run: `make throughput`
Expected: the DAG table now has a `D5 chunk` row + the `(chunk N hidden ...)` line, `mism 0`. The chunk maps two onehot16 → byte directly (one primitive, one snap) vs the 2-level DAG (3 primitive evals, 3 snaps), so D5 may beat D2; record it. If `consolidate_dag` refuses, report the `verified/samples` numbers (it should succeed — the domain is 256 pairs, within the default `max_samples` 4096).

- [ ] **Step 3: Commit**

```bash
git add tests/throughput_study.c
git commit -m "feat: throughput D5 -- consolidate the byte DAG into one chunk"
```

---

## Self-review

**Spec coverage** (this plan implements the deferred "DAG/decode fast-lane" from `2026-06-15-throughput-fast-decode-design.md`):
- Batched `fp_dag_compile`/`fp_dag_run`/`fp_dag_free` mirroring `dag_execute`/`eval_node` → Task 1. ✓
- Reuses the route lane's pack + forward kernels (no reimplementation) → Task 1 (`pack_step_*`, `step_forward_*`, factored `fp_step_free_buffers`). ✓
- Multi-slot input assembly with output-port projection + per-node memo for shared nodes → Task 1 (`fp_dag_walk`, post-order eval, `child_seg_off`). ✓
- Faithfulness: f64 oracle-equivalence + bit-identical batched==single; f32 faithful + margin; int gate-bites → Tasks 1, 2. ✓
- Levers D0–D5 over `combine(hex_value,hex_value)->byte`, capacity-style table → Tasks 3, 4. ✓
- Core untouched (only `src/fastpath.c` additions + the `fp_step_free_buffers` refactor within it) → confirmed; `router.c`/`nn.c`/`consolidate.c` unchanged. ✓
- Deferred (not in scope): multi-output circuits (`dag_execute_circuit`), Phase-2 streaming + rejection-gate API. ✓

**Placeholder scan:** no TBD/TODO; every code step is complete; commands have expected output.

**Type consistency:** `fp_dag_compile`/`fp_dag_run`/`fp_dag_free`/`fp_dag_min_margin`/`fp_dag_reset_margin`/`fp_dag_out_total`, `CompiledDag`, `FpDagNode` are used identically across `fastpath.h`, `fastpath.c`, `test_fastpath.c`, `throughput_study.c`. `fp_dag_run` takes `const double *const *src_batches`. The reused `FpStep`/`pack_step_*`/`step_forward_*` signatures match the route lane. `fp_step_free_buffers` is defined once and called from both `fp_route_free` and `fp_dag_free`/`fp_dag_free_partial`.

**Two invariants the implementer must preserve:** (1) `fp_dag_compile` sizes `cd->nodes` once via `fp_dag_count` (pointer-dedup, cap 64) and never reallocs it, so `&cd->nodes[idx]` stays valid across the recursive `fp_dag_walk` — re-fetch `fn = &cd->nodes[idx]` after each child recursion as shown (defensive, since `node_count` grows). (2) Every `FpDagNode` is `calloc`-zeroed and all its buffers free NULL-safely, so a single `fp_dag_free` cleans up both the success and the mid-walk-failure paths — there is no separate partial-free routine and no pointer-tag trick. The `step_forward_*` reduction order is unchanged from the route lane, so the f64 DAG path is bit-identical to the oracle.
