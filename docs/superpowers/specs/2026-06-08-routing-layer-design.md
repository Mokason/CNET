# Routing Layer (Contract-Graph Planner) — Design

Date: 2026-06-08
Status: Implemented 2026-06-08 (TDD; `make test` + `make route` green). Planner
auto-discovers AND executes hex_value->increment (16/16) with no hand-wiring.
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

We have frozen primitives with machine-checkable interface contracts
(`port_compatible`, `port_validate`; see the interface-contracts spec). They
still have to be wired together by hand. The routing layer makes composition
**automatic and goal-directed**: given an input type and a desired output type,
discover and run a chain of frozen primitives that gets there.

Decisions taken (brainstorming): **contract-graph planner** (deterministic,
not a learned gate) producing **goal-directed multi-hop chains**. A learned MoE
gate is deferred — primitives have heterogeneous input widths and there is no
task distribution to train a gate on yet.

## Architecture

- New module `include/router.h` + `src/router.c`: registry, planner, executor.
  Keeps `nn.c` focused on primitives.
- One new contract-layer helper in `nn.c` / `nn.h`: `port_canonicalize`, needed
  by the executor to turn one primitive's fuzzy output into the next's clean
  input. It belongs with the other `port_*` contract operations.

## port_canonicalize

```c
int port_canonicalize(Port port, const double *raw, double *clean);
```

Discretizes a vector to its port's canonical form:
- `PORT_BINARY_MSB` / `PORT_BINARY_LSB`: `clean[i] = raw[i] >= 0.5 ? 1.0 : 0.0`.
- `PORT_ONEHOT`: per field, argmax → winner 1.0, rest 0.0.
- `PORT_RAW`: copy unchanged.

This is the "discrete interfaces between frozen primitives are robust" insight
in code: the executor re-canonicalizes each inter-primitive signal using the
contract, so saturated-sigmoid outputs (~0.9/0.1) become exact symbols.

## Planner

```c
int route_plan(const PrimitiveRegistry *reg,
               Port input_port, Port goal_port, RoutePlan *out);
```

BFS over **port-types**. A node is a `Port` type `(family, field_width,
field_count)`; an edge is a primitive (`input_port` -> `output_port`). Start at
`input_port`; succeed when the current type is `port_compatible` with
`goal_port` (length-0 chain allowed if already satisfied). A primitive `p` is
usable from type `T` iff `port_compatible(T, p.input_port)`; applying it moves to
`p.output_port`. A visited-type set (canonical key `family:width:count`) prevents
cycles. BFS yields the **shortest** chain. Returns 0 with `out` filled, or -1 if
unreachable within `ROUTE_MAX_STEPS`. Planning reads only contracts — no data,
no training.

## Executor

```c
int route_execute(const RoutePlan *plan, const double *input, size_t in_len,
                  double *output, size_t out_cap);
```

For each step: `port_validate(step.input_port, current)` (runtime eligibility
guard) -> `btn_forward` -> `port_canonicalize(step.output_port, raw, next)` ->
continue. Two scratch buffers sized to the plan's max port total, ping-ponged.
`in_len` must equal the first step's input total; `out_cap` must be >= the last
step's output total. Returns -1 on validation failure, size mismatch, or alloc
failure. A length-0 plan copies the (canonicalized) input to output.

## Data structures

```c
#define ROUTE_MAX_STEPS 8

typedef struct { BinaryTransformNetwork *btn; const char *name; } RegistryEntry;

typedef struct {
    RegistryEntry *entries;
    size_t count;
    size_t capacity;
} PrimitiveRegistry;                 /* borrows BTN pointers; owns only `entries` */

typedef struct {
    const BinaryTransformNetwork *steps[ROUTE_MAX_STEPS];
    const char *names[ROUTE_MAX_STEPS];
    size_t length;
} RoutePlan;

void registry_init(PrimitiveRegistry *reg);
int  registry_add(PrimitiveRegistry *reg, BinaryTransformNetwork *btn, const char *name);
void registry_free(PrimitiveRegistry *reg);  /* frees entries array, NOT the BTNs */
```

## Testing (each gate has teeth)

1. `tests/test_router.c` — hermetic (synthetic BTNs via `btn_init` +
   `btn_set_ports`; no training, no weight files; in `make test`):
   - `port_canonicalize`: binary threshold (`{0.9,0.1,0.6,0.2}` -> `1,0,1,0`);
     one-hot argmax over 2 fields; RAW passthrough.
   - `registry_add` then planner.
   - `route_plan` finds the 2-hop chain for ONEHOT 16 -> BINARY_MSB 5
     (`[hexlike, inclike]`, asserted by name + order).
   - picks the **shortest** path when a direct 1-hop primitive also exists.
   - **ignores a decoy** primitive that doesn't lie on any path.
   - returns -1 for an unreachable goal type.
   - length-0 success when input already compatible with goal.
2. `tests/route_demo.c` — integration (in `make route`, which runs `nn_demo`
   first to regenerate v2 weights): loads the real frozen `hex_value` and
   `increment` primitives, `route_plan(ONEHOT 16 -> BINARY_MSB 5)`, prints the
   discovered chain, then executes it over all 16 hex digits asserting the
   result equals value+1. Demonstrates auto-discovery + execution with no
   hand-wiring; mirrors `make compose` but routed.

## Verification gate

No ASan on this toolchain (MinGW). Gates are the behavioral tests via
`make test` and `make route`, plus a clean `-Wall -Wextra -pedantic` build.

## Out of scope (YAGNI)

- DAG composition / multi-input primitives (a primitive consuming two upstream
  outputs). Linear chains only. This is the next frontier after v1.
- Learned scoring / top-k / MoE gate.
- Ranking chains by anything other than length.
- Auto-loading a registry from a directory of weight files (caller registers
  explicitly for now).
```
