# Compounding-Loop Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Build `tests/benchmark_compounding_loop.c` — prove compositional transfer on a 4-digit decimal-addition curriculum by measuring the planner-effort/plan-size delta between Run A (raw primitives) and Run B (after a Tier-2 chunk is minted through `library_evolve_gated` and evidenced).

**Architecture:** Three runnable stages in one `main()`: (1) load Tier-1 decimal primitives + probe the 4-digit task in SHADOW = Run A; (2) accrue evidence by executing the Tier-2 single column, gate-distill `dec_full_adder_unit`, micro-batch it to out-reliability the raw column; (3) re-probe = Run B, print the full `CompoundingReport`, then assert. A benchmark, not a unit test — self-checking via `assert`, run by `make compounding_bench`.

**Tech Stack:** C11, MinGW gcc (`-mno-avx`), Makefile. Uses `dag_plan_circuit`, `dag_execute_circuit`, `library_evolve_gated`, `btn_cost`, `btn_load`, `btn_set_ports`. SHADOW mode populates `CircuitPlan.attention.nodes_expanded`.

> **Commits are user-managed.** "Commit (user)" steps list files to stage; the user commits.

**Spec:** `docs/superpowers/specs/2026-06-19-compounding-loop-benchmark-design.md`

---

## File Structure

- **`tests/benchmark_compounding_loop.c`** (new): the whole experiment.
- **`Makefile`** (new target `compounding_bench`, mirroring `dgate_bench`).

Build/run:
```bash
make compounding_bench
```
Direct compile (for iterating):
```bash
gcc -std=c11 -Wall -Wextra -pedantic -O3 -march=native -mno-avx -Wno-unused-function \
  -o compounding_bench.exe \
  src/nn.c src/router.c src/plan_table.c src/contract.c src/property.c src/consolidate.c src/scan.c src/library.c \
  tests/benchmark_compounding_loop.c -lm && ./compounding_bench.exe
```

---

## Task 1: Scaffold — structs, metric helpers, fixture, Tier-3 builder, Run A probe, Makefile target

**Files:**
- Create: `tests/benchmark_compounding_loop.c`
- Modify: `Makefile`

- [ ] **Step 1: Makefile target**

Mirror `dgate_bench` (search `Makefile` for `dgate_bench:` and copy its shape). Add:
```make
CMPND_BENCH := tests/benchmark_compounding_loop.c
compounding_bench: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(CMPND_BENCH)
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(LIBRARY) $(CMPND_BENCH) $(LDFLAGS)
	./compounding_bench
```
Add `compounding_bench` to the `.PHONY` line.

- [ ] **Step 2: File skeleton + structs + metric helpers**

Create `tests/benchmark_compounding_loop.c`:
```c
/* benchmark_compounding_loop.c -- the Decimal Ladder: measure compositional
 * transfer (Run A raw vs Run B after a gated Tier-2 chunk). assert direction,
 * report magnitude. (spec: docs/superpowers/specs/2026-06-19-compounding-loop-benchmark-design.md) */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/library.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define CHUNK_NAME "dec_full_adder_unit"

typedef struct { size_t nodes_expanded, plan_length, macs, chunk_uses; } RunMetrics;
typedef struct {
    RunMetrics raw, compounded;
    double nodes_factor, length_factor, macs_factor;
    int    chunk_minted;
} CompoundingReport;

/* ---- metric walk over a circuit plan's owned[] (sharing-aware closure) ---- */
static RunMetrics metrics_of(const CircuitPlan *cp) {
    RunMetrics m; size_t i;
    memset(&m, 0, sizeof m);
    m.nodes_expanded = cp->attention.nodes_expanded;
    if (cp->owned == NULL) return m;
    for (i = 0; i < cp->owned_count; ++i) {
        const DagNode *n = cp->owned[i];
        if (n == NULL || n->kind != DAG_PRIMITIVE) continue;
        m.plan_length++;
        m.macs += btn_cost(n->btn);
        if (n->name != NULL && strcmp(n->name, CHUNK_NAME) == 0) m.chunk_uses++;
    }
    return m;
}
```
Ports (from `tests/decimal_demo.c`): `dec_value` ONEHOT 10 `"dec_symbol"` -> BINARY_MSB 4 `"dec_digit"`; `dec_full_add` [BINARY_MSB 4 `"dec_digit"`, BINARY_MSB 4 `"dec_digit"`, BINARY_MSB 1 `"dec_carry"`] -> [BINARY_MSB 4 `"dec_sum"`, BINARY_MSB 1 `"dec_carry"`].

- [ ] **Step 3: Fixture loader + Tier-3 builder + Run A**

Add (model the load on `tests/test_distillation_gate.c`'s decimal load — `btn_load` from the `*_weights.txt` in repo root, then `btn_set_ports` with the ports above; `dec_value` and `dec_full_add` must be registered with their names):
```c
/* Build the 4-digit Tier-3 task: 9 sources (a0..a3,b0..b3 dec_symbol; carry
   dec_carry), 5 goals (sum0..sum3 dec_digit/dec_sum tag; cout dec_carry).
   Fills sources[] types (values set per-input by the evidence runner; the PROBE
   needs only types). Returns n_sources via *ns, n_goals via *ng. */
static void build_tier3(DagSource src[9], Port goals[5], size_t *ns, size_t *ng) {
    size_t i;
    Port sym = { PORT_ONEHOT, 10, 1, "" };      port_set_tag(&sym, "dec_symbol");
    Port cin = { PORT_BINARY_MSB, 1, 1, "" };    port_set_tag(&cin, "dec_carry");
    Port sum = { PORT_BINARY_MSB, 4, 1, "" };    port_set_tag(&sum, "dec_sum");
    Port cout= { PORT_BINARY_MSB, 1, 1, "" };    port_set_tag(&cout,"dec_carry");
    for (i = 0; i < 8; ++i) { src[i].type = sym; src[i].values = NULL; }
    src[8].type = cin; src[8].values = NULL;
    goals[0]=goals[1]=goals[2]=goals[3]=sum; goals[4]=cout;
    *ns = 9; *ng = 5;
}

/* Probe the Tier-3 task in SHADOW (plans identically to OFF but populates
   nodes_expanded). Returns RunMetrics; *planned set to 1 iff a plan was found. */
static RunMetrics probe_tier3(PrimitiveRegistry *reg, int *planned) {
    DagSource src[9]; Port goals[5]; size_t ns, ng;
    CircuitPlan cp; RunMetrics m;
    CNETAttentionMode saved = reg->attention_mode;
    build_tier3(src, goals, &ns, &ng);
    reg->attention_mode = CNET_ATTENTION_SHADOW;
    memset(&cp, 0, sizeof cp);
    *planned = (dag_plan_circuit(reg, src, ns, goals, ng, &cp) == 0);
    memset(&m, 0, sizeof m);
    if (*planned) { m = metrics_of(&cp); circuit_free(&cp); }
    reg->attention_mode = saved;
    return m;
}
```
And a `main()` that loads + registers `dec_value`/`dec_full_add`, runs `probe_tier3` = Run A, prints it, and (for now) returns 0:
```c
int main(void) {
    PrimitiveRegistry reg; BinaryTransformNetwork dec_value, dec_full_add;
    int planned_a; RunMetrics raw;
    /* load_decimal_primitives(&dec_value, &dec_full_add) -- btn_load + btn_set_ports
       per the ports above; abort with a message if a weight file is missing */
    registry_init(&reg);
    registry_add(&reg, &dec_value, "dec_value");
    registry_add(&reg, &dec_full_add, "dec_full_add");
    raw = probe_tier3(&reg, &planned_a);
    printf("Run A (raw): planned=%d nodes=%zu len=%zu macs=%zu chunk_uses=%zu\n",
           planned_a, raw.nodes_expanded, raw.plan_length, raw.macs, raw.chunk_uses);
    if (!planned_a) { fprintf(stderr, "STOP: 4-digit add did not plan from raw primitives.\n"); return 2; }
    /* Tasks 2-3 extend here */
    registry_free(&reg); btn_free(&dec_value); btn_free(&dec_full_add);
    return 0;
}
```

- [ ] **Step 4: Build + verify Run A**

Run `make compounding_bench`. Expected: it builds and prints `Run A (raw): planned=1 nodes=<N>0 len=~12 chunk_uses=0`. If `planned=0`, STOP and report (the spec's Open Point #1 — do not hand-build the plan).

- [ ] **Step 5: Commit (user)**

Stage: `git add tests/benchmark_compounding_loop.c Makefile`
Message: `bench(compounding): scaffold + Run A (raw 4-digit probe) + metric walk`

---

## Task 2: Evidence runner + gated Tier-2 mint + micro-batch

**Files:**
- Modify: `tests/benchmark_compounding_loop.c`

- [ ] **Step 1: Tier-2 plan + evidence runner**

Add helpers. The Tier-2 single column = 3 sources (a `dec_symbol`, b `dec_symbol`, carry `dec_carry`) -> 2 goals (`dec_sum`, `dec_carry`). Execute it 16× on valid one-hot inputs to accrue evidence on `dec_value`/`dec_full_add` (each `dag_execute_circuit` run records one outcome per node — see `test_circuit.c:266`):
```c
/* Fill a column's source VALUES for digits a,b and carry c. sym buffers are
   10-wide one-hot; carry is 1-wide. Caller owns the buffers for the call. */
static void set_col_inputs(DagSource s[3], double abuf[10], double bbuf[10],
                           double cbuf[1], int a, int b, int c) {
    int i;
    Port sym = { PORT_ONEHOT, 10, 1, "" }; port_set_tag(&sym, "dec_symbol");
    Port cin = { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cin, "dec_carry");
    for (i = 0; i < 10; ++i) { abuf[i] = (i==a)?1.0:0.0; bbuf[i] = (i==b)?1.0:0.0; }
    cbuf[0] = c ? 1.0 : 0.0;
    s[0].type = sym; s[0].values = abuf;
    s[1].type = sym; s[1].values = bbuf;
    s[2].type = cin; s[2].values = cbuf;
}

/* Plan the Tier-2 column over `reg`, execute it `n` times on deterministic
   valid inputs -> the column's primitives accrue evidence. Returns 0/-1. */
static int accrue_tier2_evidence(PrimitiveRegistry *reg, int n) {
    DagSource s[3]; double ab[10], bb[10], cb[1], out[8];
    Port goals[2]; CircuitPlan cp; int k, rc = 0;
    Port sum = { PORT_BINARY_MSB, 4, 1, "" }; port_set_tag(&sum, "dec_sum");
    Port cout= { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cout, "dec_carry");
    goals[0] = sum; goals[1] = cout;
    for (k = 0; k < n; ++k) {
        set_col_inputs(s, ab, bb, cb, k % 10, (k * 7) % 10, k & 1);
        memset(&cp, 0, sizeof cp);
        if (dag_plan_circuit(reg, s, 3, goals, 2, &cp) != 0) { rc = -1; break; }
        if (dag_execute_circuit(&cp, s, 3, out, 8, NULL) != 0) rc = -1;
        circuit_free(&cp);
        if (rc) break;
    }
    return rc;
}
```

- [ ] **Step 2: Gated Tier-2 mint + identity capture + micro-batch**

Add:
```c
/* Gate-distill the Tier-2 column through the SHIPPED gate. On success the
   chunk CHUNK_NAME is registered (FROZEN, seeded-evidenced). Fills *minted (1/0)
   and, if minted, *out_chunk points at report.chunks[0] (multi-output BTN).
   The LibraryReport OWNS the chunk; caller library_report_free()s it at the end
   (AFTER all probing, since the registry borrows it). */
static int mint_tier2(PrimitiveRegistry *reg, LibraryReport *report, int *minted) {
    LibraryTask task; LibraryGateConfig gate; ConsolidateConfig cfg;
    Port sym = { PORT_ONEHOT, 10, 1, "" }; port_set_tag(&sym, "dec_symbol");
    Port cin = { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cin, "dec_carry");
    Port sum = { PORT_BINARY_MSB, 4, 1, "" }; port_set_tag(&sum, "dec_sum");
    Port cout= { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cout, "dec_carry");

    memset(&task, 0, sizeof task);
    task.name = CHUNK_NAME;
    task.sources[0] = sym; task.sources[1] = sym; task.sources[2] = cin;
    task.n_sources = 3;
    task.goals[0] = sum; task.goals[1] = cout; task.n_goals = 2;

    consolidate_config_defaults(&cfg);
    cfg.min_verify_rate = 0.95;     /* training override: nonlinear mod-10 chunk */
    cfg.max_epochs = 320000;
    library_gate_config_defaults(&gate);
    gate.enabled = 1;

    if (library_evolve_gated(reg, &task, 1, NULL, 0, &cfg, &gate, 3, report) != 0) return -1;
    *minted = (report->chunk_count >= 1);
    return 0;
}

/* Re-plan the Tier-2 column (now chunk-preferred) and execute it n times so the
   new chunk accrues evidence (Pi-reliability safeguard). 0/-1. */
static int trust_chunk(PrimitiveRegistry *reg, int n) { return accrue_tier2_evidence(reg, n); }
```
Extend `main()` after the Run A block (before `registry_free`):
```c
    if (accrue_tier2_evidence(&reg, 16) != 0) { fprintf(stderr, "STOP: evidence batch failed.\n"); return 3; }
    {
        LibraryReport report; int minted = 0;
        if (mint_tier2(&reg, &report, &minted) != 0) { fprintf(stderr, "STOP: mint call failed.\n"); return 4; }
        printf("Tier-2 mint: minted=%d name=%s\n", minted, minted ? report.names[0] : "<none>");
        if (minted) {
            printf("  chunk ports: in=%zu out=%zu\n",
                   report.chunks[0]->input_port_count, report.chunks[0]->output_port_count);
            (void)trust_chunk(&reg, 16);
        }
        /* report kept alive until end of main (registry borrows the chunk);
           library_report_free(&report) at the very end. */
        /* Task 3 uses `report.names[0]` / `report.chunks[0]` for the identity assert. */
    }
```
(Refactor so `report` lives in `main`'s scope through Run B; the simplest is to declare `LibraryReport report; int minted=0;` near the top of `main` and free it at the end.)

- [ ] **Step 3: Build + verify mint**

Run `make compounding_bench`. Expected (after Run A line): `Tier-2 mint: minted=1 name=dec_full_adder_unit` and `chunk ports: in=3 out=2`. If `minted=0`, STOP and report (the gate refused — likely the override needs more epochs / lower verify; report the `ConsolidateReport` if you wire one in).

- [ ] **Step 4: Commit (user)**

Stage: `git add tests/benchmark_compounding_loop.c`
Message: `bench(compounding): evidence batch + gated Tier-2 mint + chunk trust micro-batch`

---

## Task 3: Run B probe + report, print-then-assert

**Files:**
- Modify: `tests/benchmark_compounding_loop.c`

- [ ] **Step 1: Run B + report fill + print + assert**

Extend `main()` after the mint block. Probe Tier-3 again (chunk now registered+evidenced) = Run B; fill the report; **print the full table FIRST; then assert** (mint identity -> causal-story -> direction), per the spec's mandatory ordering:
```c
    {
        int planned_b; RunMetrics comp = probe_tier3(&reg, &planned_b);
        CompoundingReport rep;
        memset(&rep, 0, sizeof rep);
        rep.raw = raw; rep.compounded = comp; rep.chunk_minted = minted;
        rep.nodes_factor  = comp.nodes_expanded ? (double)raw.nodes_expanded / comp.nodes_expanded : 0.0;
        rep.length_factor = comp.plan_length    ? (double)raw.plan_length    / comp.plan_length    : 0.0;
        rep.macs_factor   = comp.macs           ? (double)raw.macs           / comp.macs           : 0.0;

        /* ---- PRINT FIRST (always, before any assert can abort) ---- */
        printf("\n=== Compounding-Loop Benchmark (4-digit decimal add) ===\n");
        printf("                 nodes_expanded   plan_length   macs    chunk_uses\n");
        printf("  Run A (raw)    %14zu  %12zu  %6zu  %10zu\n",
               raw.nodes_expanded, raw.plan_length, raw.macs, raw.chunk_uses);
        printf("  Run B (chunk)  %14zu  %12zu  %6zu  %10zu\n",
               comp.nodes_expanded, comp.plan_length, comp.macs, comp.chunk_uses);
        printf("  reduction x    %14.2f  %12.2f  %6.2f\n",
               rep.nodes_factor, rep.length_factor, rep.macs_factor);
        printf("  chunk_minted=%d  planned_b=%d\n\n", rep.chunk_minted, planned_b);
        fflush(stdout);

        /* ---- THEN assert (order: identity -> causal -> direction) ---- */
        assert(rep.chunk_minted == 1 && "Tier-2 chunk minted through the gate");
        assert(strcmp(report.names[0], CHUNK_NAME) == 0 && "minted chunk is dec_full_adder_unit");
        assert(report.chunks[0]->input_port_count == 3 &&
               report.chunks[0]->output_port_count == 2 && "chunk signature 3-in/2-out");
        assert(planned_b && "Run B planned");
        assert(raw.chunk_uses == 0 && "Run A used no chunk");
        assert(comp.chunk_uses >= 4 && "Run B composed the chunk once per column (>=4)");
        assert(comp.plan_length < raw.plan_length && "compounded plan is shorter");
        assert(comp.nodes_expanded < raw.nodes_expanded && "compounded search is smaller");
        printf("COMPOUNDING_BENCH PASS\n");

        library_report_free(&report);
    }
```
(Ensure `report`/`minted`/`raw` are declared in `main`'s scope so this block sees them; free the report only here, after Run B, since the registry borrowed the chunk.)

- [ ] **Step 2: Build + run the full benchmark**

Run `make compounding_bench`. Expected: the table prints with `Run B` plan_length and nodes_expanded **both smaller** than Run A, `chunk_uses>=4`, then `COMPOUNDING_BENCH PASS`. If an assert fires, the table is already printed — read it: if only `nodes_expanded` failed (didn't shrink), that is the spec's named finding (compounding shrank plan size but not search effort) — report it with the table, do NOT delete the assert.

- [ ] **Step 3: Commit (user)**

Stage: `git add tests/benchmark_compounding_loop.c`
Message: `bench(compounding): Run B + print-then-assert delta (compositional transfer)`

---

## Self-Review

**Spec coverage:**
- Curriculum (Tier-1 raw, Tier-2 gated chunk, Tier-3 4-digit) → Tasks 1–3. ✓
- Run A (SHADOW probe, raw) → Task 1; Evidence batch → Task 2 Step 1; Gate-distill via `library_evolve_gated` + override → Task 2 Step 2; Trust micro-batch → Task 2 Step 2; Run B → Task 3. ✓
- Metrics (nodes_expanded/plan_length/macs/chunk_uses) → `metrics_of` (Task 1). ✓
- Print-before-assert ordering → Task 3 Step 1 (print block precedes asserts, `fflush`). ✓
- Mint identity (name + 3-in/2-out) → Task 3 asserts. ✓
- Causal story (`raw.chunk_uses==0`, `comp.chunk_uses>=4`) → Task 3 asserts. ✓
- Direction asserted / magnitude reported → Task 3 (asserts on `<`, factors printed not asserted). ✓
- Makefile `compounding_bench`, standalone (not in `make test`) → Task 1 Step 1. ✓

**Placeholder scan:** the only deferred-to-implementer piece is `load_decimal_primitives` (the `btn_load` + `btn_set_ports` weight loading), explicitly pointed at `tests/test_distillation_gate.c`'s decimal load + the ports listed in Task 1 Step 2 — mechanical and fully specified. Every other block is complete code.

**Type consistency:** `RunMetrics{nodes_expanded,plan_length,macs,chunk_uses}`, `CompoundingReport`, `metrics_of`, `probe_tier3`, `build_tier3`, `accrue_tier2_evidence`, `mint_tier2`, `trust_chunk`, `CHUNK_NAME`, `report.names[0]`/`report.chunks[0]` — used identically across tasks. `library_evolve_gated(reg, tasks, n_tasks, laws, n_laws, cfg, gate, max_iter, report)` matches the Cycle-1 signature. ✓

**Note for the executor:** declare `LibraryReport report; int minted=0; RunMetrics raw;` in `main`'s top scope (Task 1) so Tasks 2–3 extend the same flow; `library_report_free(&report)` exactly once, at the very end (Task 3), because the registry borrows the chunk through Run B.
