# DAG Composition for Multi-Input Primitives — Design

Date: 2026-06-09
Status: Implemented 2026-06-09 (TDD; `make test` + `make dag` green). Planner
auto-builds AND executes combine(hex_value, hex_value): two hex digits -> byte,
6/6. Note: combine first stalled at loss 0.0269 (LSB unlearned) at lr 0.5;
raising to lr 0.8 reached 0.0022 and fixed the DAG -- a frozen primitive being
undertrained broke a downstream DAG, the project's verify-primitives lesson.
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

The router composes linear chains: each primitive has one input. But a primitive
can need several inputs assembled from different sources (e.g. a join that takes
two nibbles). DAG composition generalizes routing from a chain to an AND-OR tree:
to produce a goal type, pick a producer primitive, then satisfy *all* of its
input slots, each from a source or a sub-DAG.

Decisions (brainstorming): **homogeneous field-slots** (reuse the contract's
`field_count`, no format change), a **`combine` (two hex digits -> byte)
integration demo**, and **consumable typed sources**.

## Model

A primitive's input port `{family, field_width, field_count}` exposes
`field_count` input **slots**, each of slot type `{family, field_width, 1}`.
`field_count == 1` is an ordinary linear primitive; `> 1` is multi-input. No
struct or file-format change.

## The combine primitive (new, frozen)

Input `BINARY_MSB 4x2` (two nibble slots), output `BINARY_MSB 8x1` (a byte).
Per-bit identity (a byte is its two nibbles concatenated), so it is an honest
**join node**: the decode work is `hex_value` applied to each digit; `combine`
is the assembly point. Its output type `BINARY_MSB 8` is unique in the registry,
making the branching DAG unambiguous. Trained on all 256 nibble pairs (input
bits == target bits), saved to `combine_weights.txt` by the demo.

## Data structures (router.h)

```c
#define DAG_MAX_SLOTS 8
#define DAG_MAX_DEPTH 8

typedef struct { Port type; const double *values; } DagSource;

typedef enum { DAG_SOURCE, DAG_PRIMITIVE } DagNodeKind;

typedef struct DagNode {
    DagNodeKind kind;
    int source_index;                                  /* DAG_SOURCE */
    const BinaryTransformNetwork *btn;                 /* DAG_PRIMITIVE */
    const char *name;
    struct DagNode *children[DAG_MAX_SLOTS];
    size_t child_count;
} DagNode;

typedef struct { DagNode *root; } DagPlan;
```

## Planner — recursive AND-OR search with consumable sources

```c
int  dag_plan(const PrimitiveRegistry *reg, const DagSource *sources,
              size_t n_sources, Port goal_port, DagPlan *out);   /* 0 / -1 */
void dag_free(DagPlan *plan);
```

Internal `produce(type, consumed[], depth) -> DagNode*` (NULL on failure):
1. **OR (source):** first unconsumed source with `port_compatible(src.type,
   type)` -> mark consumed, return a leaf node.
2. **OR (primitive):** any `p` with `port_compatible(p->output_port, type)`;
   then **AND** over its slots: `produce` each slot type
   `{p->input_port.family, p->input_port.field_width, 1}`. If any slot fails,
   free the built children, restore the consumed-set (backtrack), try the next
   primitive.

Depth-capped by `DAG_MAX_DEPTH` for cycle safety. Sources are tried before
primitives (use data that directly fits). For goal `BINARY_MSB 8` with two
`ONEHOT 16` sources it returns `combine(hex_value(src0), hex_value(src1))`,
each branch consuming a distinct source.

## Execution

```c
int dag_execute(const DagPlan *plan, const DagSource *sources, size_t n_sources,
                double *output, size_t out_cap);   /* 0 / -1 */
```

`evaluate(node) -> malloc'd buffer + length`:
- `DAG_SOURCE`: copy `sources[source_index].values` (length = source type total).
- `DAG_PRIMITIVE`: allocate `btn->input_count`; for each child `s`, evaluate it,
  `port_canonicalize` its output into the slot field at offset
  `s * input_port.field_width` (slot type `{family, field_width, 1}`), free the
  child buffer; then `port_validate(input_port, assembled)`, `btn_forward`,
  `port_canonicalize(output_port, raw, out)`. Buffers freed on unwind; any
  failure returns NULL and frees partials. Final root buffer copied to `output`
  (requires `out_cap >= root output total`).

## Tests (TDD, each gate has teeth)

1. `tests/test_dag.c` — hermetic (synthetic BTNs via `btn_init` + `btn_set_ports`,
   no training; in `make test`):
   - `dag_plan` for `BINARY_MSB 8` with two `ONEHOT 16` sources builds: root
     `combine` (child_count 2); both children `hexlike` primitives; each
     `hexlike`'s child a `DAG_SOURCE`; the two leaves reference **distinct**
     source indices (consumption).
   - returns -1 when only ONE source is supplied for the two-slot goal.
   - a goal directly matching a source returns a bare `DAG_SOURCE` leaf.
   - `dag_free` runs clean (no leak/crash; validated under normal run; ASan
     unavailable on this toolchain).
2. `tests/dag_demo.c` — integration (`make dag`, runs `nn_demo` first): loads real
   `hex_value` + `combine`, two `ONEHOT 16` hex-digit sources, `dag_plan(->
   BINARY_MSB 8)`, prints the tree, executes; asserts the byte equals
   `value(hi)*16 + value(lo)` for several pairs incl. `('4','1')->0x41`,
   `('F','F')->0xFF`, `('0','0')->0`.

## Verification gate

No ASan on this toolchain (MinGW). Gates are `make test` + `make dag` and a clean
`-Wall -Wextra -pedantic` build.

## Out of scope (YAGNI)

- Heterogeneous explicit multi-ports (differently-typed inputs). Homogeneous
  field-slots only.
- Greedy first-fit source assignment: backtracks over primitive choice, NOT over
  which source fills a leaf. Sufficient for type-distinct sources; full bipartite
  matching is future. Documented, not hidden.
- Learned scoring / ranking beyond first-found.
- DAG result caching / shared sub-results (each slot evaluated independently).
```
