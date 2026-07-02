# DAG Source-Assignment Backtracking — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + all demos green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

The DAG planner's source binding was greedy first-fit: when a slot could be
fed by an unconsumed source, the first compatible one was consumed and never
reconsidered. Backtracking existed only over *primitive* choice. Two plans it
provably missed (now the regression tests):

1. **Direct conflict.** Slots `[BINARY_MSB 4, ONEHOT 4]`, sources
   `[RAW 4, BINARY_MSB 4]`. First-fit burns the RAW source (compatible with
   everything of total 4) on the BINARY slot; the ONEHOT slot, which only the
   RAW source fits, starves. The valid assignment is the swap.
2. **Sub-DAG conflict.** A sibling slot's only source is grabbed *inside* a
   sub-primitive's own slot fill. Fixing it requires revisiting choices made
   deep in an already-"successful" sibling subtree — first-fit cannot, and
   neither can per-level retry alone.

This was the documented "full bipartite matching is future" gap in
2026-06-09-dag-composition-design.md.

## Decision: complete AND-OR backtracking via an obligation agenda

Not an explicit bipartite-matching pass (Hopcroft-Karp et al.): matching only
covers source-to-slot edges, while here a slot may alternatively be fed by a
sub-DAG whose own leaves compete for the same sources. A matching layer would
still need backtracking around it. Instead the search itself becomes complete:
depth-first with chronological backtracking over **both** choice kinds, which
subsumes the augmenting-path search of bipartite matching.

`dag_produce` (one goal, returns a node) is replaced by `dag_solve` (a linked
**agenda** of pending obligations, returns all-or-nothing):

```c
typedef struct DagGoal {
    Port type;             /* what this obligation must produce */
    DagNode **dest;        /* where to attach the built node */
    int depth;             /* per-obligation; capped at DAG_MAX_DEPTH */
    const struct DagGoal *next;
} DagGoal;

static int dag_solve(reg, sources, n_sources, consumed, const DagGoal *agenda);
```

- Empty agenda -> success. Head obligation tries alternatives in the same
  order as before (sources in index order, then primitives in registry
  order), so **every plan the old planner found is still found, identically**.
- Source alternative: consume, attach leaf, recurse on `agenda->next`; on
  failure un-consume and try the next alternative.
- Primitive alternative: attach a bare node (children NULL), link its slot
  obligations (stack-allocated `DagGoal[DAG_MAX_SLOTS]`) **in front of the
  remaining agenda**, recurse once. This is the key move: the sub-primitive's
  internal choices and the parent's remaining sibling slots live in one
  search, so a sibling failure backtracks into the sub-DAG naturally.
- Invariant: `dag_solve` returning 0 has undone every consumption and
  attachment it made (each level restores only its own; induction).

`dag_plan` shrinks to: build the root obligation, calloc `consumed`, call
`dag_solve`.

## Costs and limits

- Worst case is exponential in the depth cap (complete search always is);
  registries here are tiny and failing searches were already exponential via
  primitive backtracking. No memoization (YAGNI, would need consumed-set keys).
- Recursion depth is bounded by pending obligations
  (~`DAG_MAX_DEPTH * DAG_MAX_SLOTS` = 64), not by search breadth.

## Tests (TDD)

`tests/test_dag.c`, hermetic, both RED against the greedy planner:
- "source assignment backtracking": scenario 1 above; asserts dag_plan
  succeeds and the slots got the swapped assignment.
- "backtracks into sub-DAG choices": scenario 2 via
  `deep(hex_value(ONEHOT16 src), RAW16 src)` with the RAW source listed
  first; asserts the hex_value leaf is the ONEHOT source and the BINARY-16
  slot got the RAW source.

Existing planner tests (structure, consumption, insufficient-sources,
heterogeneous slots) pin the no-regression guarantee.

## Out of scope (YAGNI)

- Learned scoring / ranking beyond first-found (unchanged next frontier).
- Failed-subgoal memoization.
- Shallowest-plan guarantee (still first-found in alternative order).
