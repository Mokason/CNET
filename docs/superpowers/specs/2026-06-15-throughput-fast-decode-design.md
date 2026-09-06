# Throughput: Bulk Decode at Speed, Faithful After the Snap — Design

**Date:** 2026-06-15
**Status:** Complete (Phase 1) — route lane L0–L5, DAG lane D0–D5, AND multi-root circuit
lane C0–C4 built, TDD, reviewed; findings below. Phase 2 (rejection-gate API, streaming) and
`consolidate_circuit` (multi-primitive ripple-carry, needs decimal weights) deferred.
**Branch:** `chunk-capacity`
**Backbone:** a separate fast-lane executor (the `double` core stays the oracle), with
consolidation as one measured lever. Core untouched.

## The question

The decode pipeline — `0100000101000010 -> 0100 0001 0100 0010 -> 41 42 -> AB` —
runs today as a chain of frozen `btn_forward` passes with a **validate +
canonicalize** snap at every handoff. That is correct and transparent, but it
was never built for bulk: how many bytes/sec can the frozen fabric actually push,
and which levers buy the speed?

Two facts from the current execute path set the ceiling:

- `route_execute` (`src/router.c:2079-2080`) **mallocs two scratch buffers on
  every call**. One call per chunk means two `malloc`/`free` per chunk — the
  allocation dominates the arithmetic.
- The arithmetic itself is a single-sample, all-`double`, scalar MAC loop
  (`btn_forward`, `src/nn.c:743-774`), which writes its scratch *into the struct*
  (`btn->hidden_output`, `btn->last_output`) — so it is neither reentrant nor
  batch-safe in place. The build is `-mno-avx` (the MinGW AVX struct-copy
  segfault), so there is no autovectorization rescue either.

Planning is already separable from execution — `route_execute` takes a prebuilt
plan — so the expensive search is already amortized across a stream. The work is
in execution, not planning.

## The faithfulness bar

The fast path must be **output-faithful**: it produces the same *decoded* bytes
as the `double` pipeline, not the same raw activations. Lower precision is
allowed **only where the canonicalization margin proves the snap will not flip**,
and each precision drop is gated through the existing margin-floor certification.

This is the margin study's payoff cashed in: `port_margin` (`include/nn.h:325`)
already computes the distance of a raw output to the nearest snap boundary. A
precision level is **certified for a plan** iff, over the whole workload,

```
mismatches_vs_oracle == 0   AND   min_margin_over_all_ports_and_samples > floor
```

where `floor` is the margin-floor certification threshold (commit `efc38b4`).
The snap is the faithfulness guarantee; the margin floor is the proof the snap
holds. `double -> float32 -> int8` is unlocked, robustness-gated.

## Architecture (A + consolidation-as-a-lever)

A new module — `src/fastpath.c` + `include/fastpath.h` — that reads frozen plans
and BTN weights **read-only**, reuses `Port` / `port_validate` /
`port_canonicalize` / `port_margin`, and is an *independent* fast implementation
checked against the trusted slow path. The only permitted touch to existing code
is exposing the weight-index helpers (`btn_input_hidden_index` /
`btn_hidden_output_index`) if they are `static`; `nn.c` and `router.c` semantics
are otherwise unchanged. The slow `route_execute` / `btn_forward` remain the
reference oracle.

The module provides a **compiled plan**:

```c
typedef enum { FP_F64, FP_F32, FP_I8 } FastPrecision;

/* Pack every primitive's weights into `prec` ONCE; size scratch to a window of
   `batch` samples. Returns NULL if `prec` fails margin certification for this
   plan (Phase 2 gate; Phase 1 reports instead of rejecting — see below). */
CompiledPlan *route_compile(const RoutePlan *plan, FastPrecision prec, size_t batch);

/* One GEMM per primitive over n <= batch samples; snap at each handoff.
   No allocation, no per-sample reliability mutation. */
int route_run(CompiledPlan *cp, const double *in, size_t n,
              size_t in_len, double *out, size_t out_cap_per);

void route_compile_free(CompiledPlan *cp);
```

Each primitive's two matmuls (`input -> hidden`, `hidden -> output`) are a GEMV
per sample; over a batch they become a GEMM with the weights resident in cache.
Per-sample reduction order is preserved, so the batched `double` path is
bit-identical to the oracle — batching reorders *across* samples, never *within* a
sample's accumulation. The DAG path (`dag_compile` / `dag_run`) mirrors this.

`int8` extends a precedent already in the tree: `btn_forward` already has a
`ternary_inference` / `ternary_quantize` path (`src/nn.c:747-768`). The fast lane
generalizes "quantize the weights for inference" into a packed, batched kernel
rather than a per-element branch in the hot loop.

## Phase 1 — `throughput_study.exe` (the instrument)

A standalone harness (`tests/throughput_study.c`, `make throughput`) that runs the
README decode chain over a buffer of N chunks and emits a
`capacity_results.txt`-style table, **one row per lever**. In Phase 1 the
precision levers *report* their certification verdict rather than being rejected —
the point is to see the numbers.

| Lever | What changes | Faithfulness |
|---|---|---|
| **L0 baseline** | `route_execute` as-is (malloc/call, `double`, 1 sample) | reference / oracle |
| **L1 scratch-reuse** | hoist the two `malloc`s out of the per-call path; pack weights once | bit-identical |
| **L2 batched GEMM** | N samples/primitive, weights resident, one matmul; per-sample reduction order preserved | bit-identical |
| **L3 float32** | pack weights + activations as `float` | identical after snap (margin-certified) |
| **L4 int8 / fixed** | quantize the packed weights (extends `ternary_inference`) | identical after snap (margin-certified) |
| **L5 consolidated chunk** | distill the decode chain into one primitive via existing consolidation, then batch-run it — fewer snaps/byte | capacity-gated |

Columns: **MB/s in · speedup vs L0 · MACs/byte · mismatches/N (vs oracle) ·
min margin · certified? (Y/N)**.

The workload is the README decode chain (the hex-char decode family) over a
randomized buffer of valid chunks; the decimal-adder family is available as a
second workload to confirm the levers are not specific to one shape. The buffer is
large enough that per-call overhead is visible against the math (target: enough
chunks that L1 alone shows a clear separation from L0). The harness is
deterministic. Unlike the margin/fuzzy/stochastic studies (which thread a chain by
hand and link only `src/nn.c`), this one drives a real plan against the
`route_execute` oracle, so it links the planner/executor (`src/router.c`),
`src/nn.c`, `src/fastpath.c`, and — for L5 — `src/consolidate.c`. It modifies none
of them; "core untouched" means no semantic change to planner, executor, or
consolidation, not an `nn.c`-only link.

### What L5 (consolidation) is and is not

L5 is *one row*, not the architecture. It collapses pipeline depth — replacing K
forward passes + K snaps with one forward pass + one snap — using the existing
consolidation machinery. It is capacity-gated: `capacity_results.txt` already
shows wide lookup tables fail to fit a single net ((2,2) = 20000 patterns falls
apart), and the chunk-vs-flat crossover is real (6656 vs 12800 MACs for the
chunked `dec_add2`). L5 measures where collapsing depth pays for *this* pipeline
and where the capacity wall makes the composed path the faster one anyway.

## Findings (Phase 1 — measured)

**Built:** the route fast-lane and levers L0–L5 (`src/fastpath.c` / `include/fastpath.h`,
`tests/throughput_study.c`, `tests/test_fastpath.c`), TDD, the `double` core untouched
(empty diff over `nn.c`/`router.c`/`consolidate.c`). The concrete workload is the
`hex_value -> increment` route (`ONEHOT16 -> BINARY_MSB5`) over N=200000 — a real 2-hop
chain exercising both port families and two snaps. (The literal `combine`-based
binary→bytes assembly is a DAG, deferred with the DAG lane; the route proves every
kernel.)

`make throughput` (representative; `clock()` is 1 ms here, so ns/speedup quantize to
~5 ns and drift run-to-run — the **ordering** and the margin / mismatch / verdict columns
are stable):

```
 lever      | ns/sample | speedup | mism/N  | min margin | certified
 -----------|-----------|---------|---------|------------|----------
 L0 oracle  |     260.0 |   1.00x |       0 |          - | ref
 L1 f64*1   |     205.0 |   1.27x |       0 |      0.388 | YES
 L2 f64     |     175.0 |   1.49x |       0 |      0.388 | YES
 L3 f32     |     200.0 |   1.30x |       0 |      0.388 | YES
 L4 int8    |     260.0 |   1.00x |       0 |      0.234 | no
 L5 chunk   |     215.0 |   1.21x |       0 |      0.473 | YES
   (chunk 16 hidden, verified 16/16, teacher-aborts 0; route was 2 steps)
```

1. **The win is overhead, not arithmetic.** Removing the per-call `malloc` (L1) buys
   ~1.25×; batching the per-primitive matmul (L2) ~1.5× total — and both are *bit-identical*
   to the oracle (`test_batched_equals_single_f64`). At this scale the cost was allocation
   and dispatch, not MACs.

2. **Lower precision does not pay at toy scale without SIMD.** f32 (~1.3×) is *slower* than
   batched f64 (~1.5×), and int8 buys nothing (~1.0×). Under `-mno-avx` on tiny matrices a
   narrower dtype gives no bandwidth win, only per-element cast/quant overhead. Precision
   may pay at larger widths or with SIMD — untested here.

3. **The margin gate works and bites for real reasons.** f32 stays faithful (margin 0.388
   ≥ 0.25 floor → certified). int8 is *output-correct* (0 mismatches) but margin 0.234 <
   0.25 → **refused** — quantization noise, traced to `increment`'s ~28.9 weight outlier
   dominating the per-matrix int8 scale (per-row scaling would likely recover it). int2
   bites hard (16/16 mismatches). So on this route **f32 is the lowest certified lane** —
   refining the pre-measurement assumption (the `int8 → snap OK` mockup) that int8 would
   certify.

4. **Consolidation collapses depth but doesn't beat batching here.** L5 (one chunk, one
   snap) beats the oracle (~1.2×) but not batched f64 (~1.5×): the wider single net's MACs
   offset the saved snap — the capacity-vs-depth crossover, as predicted. Notably the
   chunk's margin (0.473) *exceeds* the composed route's (0.388): a single trained net can
   be more robust than the chain it replaces, echoing the margin study's
   correctness–margin orthogonality.

**Implication for Phase 2:** harden scratch-reuse + batching (the real, faithful wins).
Precision-narrowing and consolidation are not throughput wins at this scale and should be
revisited only with SIMD, larger widths, or depth-dominated plans. The margin-certification
gate is validated end to end (certifies f32 / f64 / L5, refuses int8 / int2). A finer timer
(`QueryPerformanceCounter`) would let the absolute speedups be quoted past two significant
figures.

## DAG lane findings (the literal binary→bytes assembly)

The deferred DAG/decode lane is now built (`fp_dag_compile`/`fp_dag_run`/`fp_dag_free`; plan
[2026-06-15-throughput-dag-decode-lane.md](../../MAINTENANCE.md#recover-an-original)): batched execution of
`combine(hex_value, hex_value) → byte` — the literal two-digits-to-a-byte step — mirroring
`dag_execute`/`eval_node` over N samples, reusing the route lane's pack + forward kernels and
adding multi-slot input gather with output-port projection and per-node caching. The walk is
**true post-order** (children indexed before parents), so a forward sweep is correct for any
acyclic DAG, shared nodes included; a hand-built `combine(split(b)[0], split(b)[1])` test (one
`split` node feeding both `combine` slots) verifies the shared-node path against `dag_execute`.

```
 lever      | ns/sample | speedup | mism/N  | min margin | certified
 -----------|-----------|---------|---------|------------|----------
 D0 oracle  |    1360.0 |   1.00x |       0 |          - | ref
 D1 f64*1   |     925.0 |   1.47x |       0 |      0.391 | YES
 D2 f64     |     845.0 |   1.61x |       0 |      0.391 | YES
 D3 f32     |     945.0 |   1.44x |       0 |      0.391 | YES
 D4 int8    |    1305.0 |   1.04x |       0 |      0.344 | YES
 D5 chunk   |     660.0 |   2.06x |       0 |      0.492 | YES
   (chunk 32 hidden, verified 256/256, teacher-aborts 0)
```

What the DAG adds to the route findings:

1. **Overhead-removal scales with structure.** `dag_execute` callocs per node per sample
   (assembled + full + memo), so D0 is 1360 ns/sample vs the route oracle's 260. Scratch-reuse
   (D1) therefore buys MORE here — ~1.47× vs the route's ~1.25× — and batching (D2) reaches
   ~1.61×. The headline holds and sharpens: the win is the overhead the *structure* imposes,
   not the arithmetic.
2. **Precision still doesn't pay** (f32 < batched f64; int8 ~1.0×) — same as the route, same
   cause (no SIMD, tiny matrices).
3. **int8 CERTIFIES on the DAG** (margin 0.344 ≥ floor) where it was *refused* on the route
   (0.234). Certifiability is **per-plan**, set by the primitives' weight distributions —
   `combine`'s per-matrix int8 scale doesn't erode the margin below the band. The gate is
   plan-specific, exactly as designed.
4. **Consolidation pays in proportion to the structure it collapses.** This inverts the route's
   L5: there, collapsing two cheap steps into one wider chunk did NOT beat batching
   (~1.2× < ~1.45×). Here, collapsing a 3-primitive, 3-snap, multi-slot DAG into one chunk + one
   snap reaches **~2.06× — beating even batched f64** — and the chunk's margin (0.492) is the
   highest of any lane. Depth/breadth-collapse is worth it when the composed structure has real
   overhead to eliminate; it isn't when the structure is already cheap.

## Circuit lane findings (shared multi-root graph)

The multi-root circuit lane is now built (`fp_circuit_compile`/`fp_circuit_run`/`fp_circuit_free`;
plan [2026-06-15-throughput-circuit-lane.md](../../MAINTENANCE.md#recover-an-original)): several goals over ONE shared node graph, each node
evaluated once across all roots, mirroring `dag_execute_circuit`. It reuses the DAG graph machinery
wholesale — `fp_dag_eval` (extracted from `fp_dag_run`), `fp_dag_walk`/`fp_dag_count` over a shared
`seen[]` across roots — so the true-post-order fix that made shared nodes correct is *exactly* what
makes circuits a thin extension. Demonstrated on `split(byte) → {hi nibble, lo nibble}`:
`dag_plan_circuit` discovers a circuit whose two roots are the SAME split node (ports 0/1), and the
fast lane compiles it to a 2-node graph (`fp_circuit_node_count == 2`) — proving the split is
evaluated once, not per root. Checked against `dag_execute_circuit` over all 256 bytes.

```
 lever      | ns/sample | speedup | mism/N  | min margin | certified
 -----------|-----------|---------|---------|------------|----------
 C0 oracle  |     800.0 |   1.00x |       0 |          - | ref
 C1 f64*1   |     695.0 |   1.15x |       0 |      0.400 | YES
 C2 f64     |     650.0 |   1.23x |       0 |      0.400 | YES
 C3 f32     |     735.0 |   1.09x |       0 |      0.400 | YES
 C4 int8    |    1000.0 |   0.80x |       0 |      0.364 | YES
```

The speedups are the most modest of the three lanes (~1.23×), and that is itself the finding: the
split circuit is a *single shared node*, so there is almost no composed structure to amortize —
only the per-call allocation the oracle pays. This completes the pattern across all three lanes:
**the fast lane's win tracks the overhead of the structure it replaces** — largest on the
alloc-heavy multi-node DAG (1.6×, 2.06× consolidated), middling on the route (1.5×), smallest on
the single-node circuit (1.23×). The mechanism — batched, snap-faithful, margin-certified, and
shared-eval-once — holds identically across route, DAG, and circuit; only the headroom differs.

## Phase 2 — harden the winners

Only the levers Phase 1 justifies become real API. Phase 2 adds:

- **The compiled-plan handle** (`route_compile` / `route_run` /
  `route_compile_free` above) as the supported bulk path: plan once, pack once,
  reuse scratch across the stream.
- **Streaming.** The caller feeds fixed-size windows; the `CompiledPlan`'s scratch
  is window-sized, so memory is **O(window), not O(file)**. A windowed run must
  equal a one-shot run.
- **The certification gate bites.** In Phase 2 `route_compile` *rejects* a
  precision that fails the margin floor for the given plan (returns NULL), rather
  than reporting and running. Phase 1's measurement becomes Phase 2's guard.
- **No per-sample reliability mutation.** The fast lane is for proven / certified
  plans; the success/failure counters that `route_execute` maintains
  (`src/router.c:2111-2122`) stay on the slow path. Recording O(N) outcomes per
  stream is both wrong (skews evidence by traffic volume) and a needless write.

## Correctness / regression gates

- **Oracle equivalence.** Every certified precision level's decoded output equals
  `route_execute`'s output over the workload — `mismatches == 0`.
- **Bit-identical.** L1 and L2 (`double`) raw activations equal the oracle
  bit-for-bit. Batching must not change a single bit.
- **The gate bites.** A deliberately over-aggressive quantization (e.g. int4)
  must be **rejected** — `min_margin < floor` and/or `mismatches > 0` — mirroring
  the property-perturbation tests that must be *caught* (`+10` breaks the snap;
  the perturbation must clear the canonicalization margin). A fast path that never
  rejects anything is not certifying anything.
- **Streaming equals one-shot.** Windowed `route_run` output equals the
  single-window output for the same buffer.
- **Mechanism.** The hot loop performs no allocation (asserted), and weights are
  packed exactly once per compiled plan.

## Out of scope (YAGNI)

- GPU.
- Multithreading. Chunks are embarrassingly parallel and the fast lane is
  reentrant by design (external scratch) — noted as the obvious next lever, but
  Phase 1/2 are single-threaded. The point is to measure the serial fabric first.
- Domains beyond the decode chain and the decimal-adder family as study workloads.
- Persisting study results (the table is regenerated, like `capacity_results.txt`).

## Reproduce

```sh
make throughput   # the lever table (L0-L5) over the hex_value->increment route
make test         # includes test_fastpath: oracle-equivalence (f64/f32),
                  # batched==single bit-identical (f64), int8/int2 gate-bites
```

`make throughput` loads the committed frozen `hex_value`/`increment` weights and
regenerates the table; `make test` certifies the fast lane against the `double` oracle
and confirms an over-aggressive precision is rejected. The fast lane is a new module;
the planner, executor, and consolidation core keep their current semantics (empty diff).
Streaming + the rejection gate as API are Phase 2 (deferred).
