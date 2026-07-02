# Circuit Plans Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Plans become true DAGs — port-disjoint shared nodes, multi-root circuit goals, reachability-pruned search — plus circuit chunks distilled into multi-output primitives.

**Architecture:** The DAG planner gains one alternative class (reuse a COMPLETE multi-output node via an unused output port), per-edge port selection (`child_ports[k]`, 0 falls back to the child's `output_index`), plan-level node ownership (flat owned[] table; recursive free only for hand-built trees), and a precomputed type→min-depth reachability table that prunes provably-empty branches. The executor evaluates each node once per run (full-output per-node memo; consumers slice their edge's segment). Circuit chunks reuse `plan_table` via a new circuit teacher and a multi-output generalization of `consolidate_core`.

**Tech Stack:** C11, MinGW gcc (`-O3 -march=native -mno-avx`), GNU Make. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-06-12-circuit-plans-design.md` (amended by Task 0).

**Invariants that gate every task:** `make test` fully green after each task; existing planner outputs byte-identical for single-output-only registries; no training-path changes for existing primitives (committed weight files stay byte-identical).

---

### Task 0: Branch + spec amendments

**Files:**
- Modify: `docs/superpowers/specs/2026-06-12-circuit-plans-design.md`

- [ ] **Step 0.1:** `git checkout -b circuit-plans`
- [ ] **Step 0.2:** Append an `## Amendments (pre-implementation)` section to the spec recording three refinements discovered while planning:
  1. *Memoization → reachability pruning.* The Pareto-set cache is unsound under cross-subtree sharing (a memoized context-free answer for slot (T, M) hides reuse opportunities — `combine(split(b))`'s second slot must see the sibling's split node). Replaced by a per-planning-call type→min-depth reachability table: a slot whose type needs more depth than remains — and that no existing complete node can serve via an unused port — prunes the branch. Sound by construction (only provably-empty branches are skipped), so memo-on/off plan equivalence is guaranteed, not just tested. Positive Pareto caching recorded as future work if the benchmark shows connected-registry pain.
  2. *Demo part 1.* `combine(split(b))` from one byte source is not demonstrable on the real registry (the byte source itself satisfies any byte goal at score 1.0); it becomes a hermetic test with a distinct-tag synthetic join. The real-primitive demo is the two-root circuit `{hi, lo} = split(b)` — one execution, two roots.
  3. *Sharing-aware traversals.* `plan_dag_count_sources` / member collection double-count under shared nodes; circuit consolidation uses pointer-set walkers.
- [ ] **Step 0.3:** Commit: `git add docs/superpowers/specs/2026-06-12-circuit-plans-design.md && git commit -m "Spec amendments: reachability pruning, demo part 1, sharing-aware walkers"`

---

### Task 1: Representation — edge ports, owned plans, circuit struct

**Files:**
- Modify: `include/router.h`
- Modify: `src/router.c` (dag_free, dag_plan tail, make_source_node, dag_search node creation)
- Test: existing suites only (no behavior change)

- [ ] **Step 1.1:** In `include/router.h`, extend `DagNode` (after `children`):

```c
    struct DagNode *children[DAG_MAX_SLOTS];  /* one per input slot */
    /* Which output port of children[k] feeds slot k. 0 = "use the child's
       output_index" -- planner-built edges are always set consistently and
       zero-initialized hand-built trees keep their old meaning. */
    int child_ports[DAG_MAX_SLOTS];
    size_t child_count;
```

- [ ] **Step 1.2:** Extend `DagPlan` and add `CircuitPlan` + new API to `router.h`:

```c
typedef struct {
    DagNode *root;
    int strict;   /* (existing comment stays) */
    /* Node ownership for PLANNER-built plans: dag_free releases this flat
       table (sharing-safe -- a shared node appears once). NULL for
       hand-built plans, whose nodes the caller owns. */
    DagNode **owned;
    size_t owned_count;
} DagPlan;

#define CIRCUIT_MAX_ROOTS BTN_MAX_OUTPUT_PORTS

/* A multi-root plan: one coordinated structure satisfying several goals,
   sharing nodes where output ports allow (port-disjoint fan-out). */
typedef struct {
    DagNode *roots[CIRCUIT_MAX_ROOTS];
    int root_ports[CIRCUIT_MAX_ROOTS]; /* projected port per root */
    size_t root_count;
    DagNode **owned;
    size_t owned_count;
    int strict;
} CircuitPlan;

/* Multi-root planning over shared structure. Every source must be
   referenced (arity-truth; also forbids goal-starved degenerate circuits).
   Sharing rule: a node may have several consumers only if each reads a
   DISTINCT output port. Returns 0 with *out filled, or -1. */
int dag_plan_circuit(const PrimitiveRegistry *reg, const DagSource *sources,
                     size_t n_sources, const Port *goals, size_t n_goals,
                     CircuitPlan *out);
void circuit_free(CircuitPlan *plan);

/* Executes all roots in ONE run: each node evaluates once (one forward,
   one reliability outcome) no matter how many consumers read it. Root
   segments are written concatenated in goal order. */
int dag_execute_circuit(const CircuitPlan *plan, const DagSource *sources,
                        size_t n_sources, double *output, size_t out_cap);
```

  Also add to `PrimitiveRegistry` (after `require_certified`):

```c
    /* Nonzero disables the planner's reachability pruning (the "memo").
       Zero-init = pruning on. Pruning only skips provably-empty branches,
       so plans are identical either way; the knob exists for the honest
       benchmark. */
    int disable_plan_memo;
```

- [ ] **Step 1.3:** In `src/router.c`: zero `child_ports` in `make_source_node` and at primitive-node creation in `dag_search` (`memset(node->child_ports, 0, sizeof node->child_ports);`). Replace `dag_clone` with an alias-preserving, ownership-collecting clone used by both planners:

```c
typedef struct {
    const DagNode **orig;
    DagNode **copy;
    size_t count;
    size_t cap;
} CloneMap;

/* Alias-preserving deep copy: a node already cloned returns the SAME copy,
   so shared structure stays shared. Every fresh copy is appended to the
   map; the planner hands map->copy[] to the plan as its owned table. */
static DagNode *dag_clone_shared(const DagNode *node, CloneMap *map) {
    DagNode *copy;
    size_t s;

    if (node == NULL) {
        return NULL;
    }
    for (s = 0; s < map->count; ++s) {
        if (map->orig[s] == node) {
            return map->copy[s];
        }
    }
    copy = malloc(sizeof(*copy));
    if (copy == NULL) {
        return NULL;
    }
    *copy = *node;
    if (map->count == map->cap) {
        size_t ncap = map->cap == 0 ? 16 : map->cap * 2;
        const DagNode **norig = realloc((void *)map->orig,
                                        ncap * sizeof(*norig));
        DagNode **ncopy;
        if (norig == NULL) {
            free(copy);
            return NULL;
        }
        map->orig = norig;
        ncopy = realloc(map->copy, ncap * sizeof(*ncopy));
        if (ncopy == NULL) {
            free(copy);
            return NULL;
        }
        map->copy = ncopy;
        map->cap = ncap;
    }
    map->orig[map->count] = node;
    map->copy[map->count] = copy;
    map->count++;
    for (s = 0; s < node->child_count; ++s) {
        copy->children[s] = dag_clone_shared(node->children[s], map);
        if (copy->children[s] == NULL && node->children[s] != NULL) {
            return NULL; /* caller frees via the map */
        }
    }
    return copy;
}
```

  `DagBest` carries a `CloneMap map` instead of a bare root; on a new best, free the old map's copies. `dag_plan` fills `out->owned = best.map.copy; out->owned_count = best.map.count;` (transfer; free the `orig` array). `dag_free`:

```c
void dag_free(DagPlan *plan) {
    if (plan == NULL) {
        return;
    }
    if (plan->owned != NULL) {
        size_t i;
        for (i = 0; i < plan->owned_count; ++i) {
            free(plan->owned[i]);
        }
        free(plan->owned);
        plan->owned = NULL;
        plan->owned_count = 0;
        plan->root = NULL;
        return;
    }
    dag_free_node(plan->root); /* legacy tree path (planless/hand-built) */
    plan->root = NULL;
}
```

  `dag_plan` must also set `out->owned = NULL; out->owned_count = 0;` alongside the existing `out->root = NULL; out->strict = 0;` at entry. Audit every internal `DagPlan` partial init: `src/plan_table.c` (`PlanDagTeacherCtx.teacher`) and `src/consolidate.c` (`ctx.teacher`) assign `.root`/`.strict` field-by-field — add `memset` or explicit zeroing of the new fields there.

- [ ] **Step 1.4:** Build + full suite: `make test` → 9/9 green (pure representation change; planner still produces trees, now owned via the map).
- [ ] **Step 1.5:** Commit: `feat: edge ports, owned plans, circuit structs (no behavior change)`

---

### Task 2: Executor rewrite — evaluate-once with full-output memo

**Files:**
- Modify: `src/router.c` (`dag_evaluate`, `dag_execute`; add `dag_execute_circuit`, `circuit_free`)

- [ ] **Step 2.1:** Replace recursive per-node projection with full-output evaluation + per-run memo. Core shape:

```c
typedef struct {
    const DagNode *node;
    double *full;   /* canonical full output (all segments), memo-owned */
    size_t len;
} EvalEntry;

typedef struct {
    EvalEntry *items;
    size_t count;
    size_t cap;
} EvalMemo;

/* Effective output port of child k (the fallback rule). */
static int edge_port(const DagNode *parent, size_t k) {
    int p = parent->child_ports[k];
    return p != 0 ? p : parent->children[k]->output_index;
}

/* Offset/total of output port `sel` within btn's flat output. */
static int output_segment(const BinaryTransformNetwork *p, size_t sel,
                          size_t *off, size_t *total);

/* Evaluate a node ONCE per run. Returns the memoized FULL canonical
   output (do not free). Sources: validate + canonicalize the external
   value. Primitives: assemble each slot from the child's edge-port
   segment (validate-then-canonicalize, ambiguous = error), forward,
   record ONE reliability outcome over the whole raw output, strict-abort
   on unhealthy, canonicalize every segment into the memo buffer. */
static const double *eval_node(const DagNode *node, const DagSource *sources,
                               size_t n_sources, int strict, EvalMemo *memo,
                               size_t *out_len);
```

  `eval_node` details that must be exact:
  - Memo lookup by pointer FIRST (this is what makes one execution = one forward = one evidence record).
  - Slot assembly: `child_full = eval_node(children[s], ...)`; for a SOURCE child the segment is the whole buffer; for a PRIMITIVE child use `output_segment(child->btn, edge_port(node, s), &off, &tot)` and validate `child_full + off` against `p->input_ports[s]` (same validate-then-canonicalize discipline as today, src/router.c:354-369).
  - Health recording: identical logic to today (whole-output validate, success/failure counters, strict abort) — but now guaranteed once per node per run.
  - The memo owns all buffers; free them all at the end of the run.

  `dag_execute`: build a memo, `eval_node(plan->root, ...)`, slice the ROOT's `output_index` segment (primitive root) or whole buffer (source root), copy out, free memo. `dag_execute_circuit`: ONE memo across roots; for each root g slice `root_ports[g]` and write at the running offset; `out_cap` checked against the concatenated total; any root failure aborts the whole run. `circuit_free` mirrors `dag_free`'s owned path.

- [ ] **Step 2.2:** `make test` → 9/9 green. The decimal ripple sweep (20,000 strict executions over hand-built trees with nonzero `output_index` children) is the regression that proves the fallback rule and the rewritten evaluator are faithful.
- [ ] **Step 2.3:** Commit: `feat: evaluate-once executor with per-edge port slicing`

---

### Task 3: Search rewrite — port-disjoint reuse

**Files:**
- Modify: `src/router.c` (`dag_search`, `dag_plan`)
- Create: `tests/test_circuit.c` (started here; grows in Tasks 4-6)
- Modify: `Makefile` (test_circuit target + `test:` line)

- [ ] **Step 3.1:** Thread a built-node stack through `dag_search` and add the reuse alternative class:

```c
typedef struct {
    DagNode *node;          /* DAG_PRIMITIVE created during this search */
    unsigned ports_used;    /* output ports already read by some consumer */
} BuiltEntry;
/* stack discipline: push at node creation, pop on backtrack */
```

  Search changes, in alternative order (order is the compat contract):
  1. **Sources** (unchanged, tried first).
  2. **Reuse** (new): for each live `BuiltEntry` e in creation order: candidate iff `e.node->btn->output_port_count > 1` (single-output nodes are never shared — this is the provable-compat lever), the node's subtree is COMPLETE (`dag_subtree_complete`: every child attached, recursively — completeness implies it cannot be an ancestor, so cycles are impossible), and some output port `oj` has `port_compatible(out[oj], slot.type)` and `(e.ports_used & (1u << oj)) == 0`. Attach: `*agenda->dest = e.node; parent->child_ports[slot] = (int)oj;` set the bit; recurse with score UNCHANGED (×1.0 — no new execution); undo on backtrack (clear bit, NULL the dest and the child_ports entry).
     - Edge bookkeeping at FIRST consumption: when a primitive node is created for port `oj` (existing alternative 3), also set the creating parent's `child_ports[slot] = (int)oj` and `e.ports_used = 1u << oj`. `node->output_index` keeps its current meaning (first consumer's port) — existing tests that read it stay valid, and the fallback rule makes 0-valued edges exact.
  3. **New primitive** (existing code + push/pop of the BuiltEntry).

  Root goals consume ports too: when the root obligation is satisfied by a primitive (created or reused), the root's port occupies `ports_used` exactly like an interior edge (circuits need this so a root cannot re-read a port feeding another consumer).

- [ ] **Step 3.2:** Wire `test_circuit` into the Makefile (same link line as test_dag: `$(SRC) $(ROUTER)`) and add it to `test:`. First hermetic tests (CHECK style from test_dag.c:13-18):

```c
/* synthetic: splitter (1 in -> 2 out ports, tags "left"/"right"),
   joiner (2 slots tagged "left","right" -> out "joined"),
   hexlike (onehot16 -> bin4, single-output) */

printf("port-disjoint sharing (single goal):\n");
/* sources: ONE bin8 "pair" source; goal "joined".
   Only plan: joiner(splitter(b).0, splitter(b).1) -- requires fan-out. */
CHECK(dag_plan(&reg, sources, 1, P8_joined, &plan) == 0, "join-of-split plans from ONE source");
CHECK(plan.root->btn == &joiner, "root is the joiner");
CHECK(plan.root->children[0] == plan.root->children[1] ||
      (plan.root->children[0]->btn == &splitter &&
       plan.root->children[1] == plan.root->children[0]),
      "both slots reference the SAME splitter node");
CHECK(edge ports of the two slots are 0 and 1 (via child_ports/output_index), "distinct ports consumed");
dag_free(&plan);

printf("sharing refusals:\n");
/* hv-collapse: combine-like joiner with two SAME-tag slots + two onehot
   sources through single-output hexlike: must still consume two distinct
   sources (single-output primitives never shared). Assert the two
   children are DISTINCT nodes consuming DISTINCT sources. */
/* third-consumer conflict: goal needing the same splitter port twice
   (two-slot joiner whose slots BOTH demand tag "left") with one source:
   dag_plan == -1. */
```

  Also: compat sweep — re-assert three representative test_dag shapes (branching combine, hetero cond, multi-output projection) inside test_circuit against a registry that CONTAINS a multi-output primitive but where sharing is not feasible, proving plan shapes are unchanged.

- [ ] **Step 3.3:** `make test` → all green including new test_circuit; existing test_dag untouched and green (the byte-compat gate).
- [ ] **Step 3.4:** Commit: `feat: port-disjoint shared nodes in dag_plan`

---

### Task 4: Multi-root circuits

**Files:**
- Modify: `src/router.c` (`dag_plan_circuit`)
- Modify: `tests/test_circuit.c`

- [ ] **Step 4.1:** `dag_plan_circuit`: seed the agenda with `n_goals` obligations (goal g's dest = `&work_roots[g]`, depth 0, chained in goal order — root 0's subtree completes before root 1 starts, so later roots see earlier subtrees as reuse candidates). At `agenda == NULL`, additionally require every `consumed[i] == 1`; reject candidates that fail coverage and keep enumerating. On success: alias-preserving clone of all roots through ONE CloneMap; record `root_ports[g]` = the projected port of root g (its `output_index` if it was created for this goal, the reused port if attached by reuse, 0 for a bare source). Validate `n_goals <= CIRCUIT_MAX_ROOTS`.
- [ ] **Step 4.2:** Hermetic circuit tests (synthetic adder shape from test_decimal.c plus a `decval` symbol→digit net):

```c
printf("dag_plan_circuit (forced ripple topology):\n");
/* sources: a0,a1,b0,b1 (bin4 dec_digit), cin (bin1 dec_carry);
   goals: sum0 (bin4 dec_sum), sum1 (bin4 dec_sum), cout (bin1 dec_carry) */
CHECK(dag_plan_circuit(&reg, src5, 5, goals3, 3, &cp) == 0, "plans the 3-goal circuit");
CHECK(cp.roots[0] != cp.roots[1], "sum goals come from DISTINCT adder executions (port-disjoint)");
/* exactly one adder consumes the cin source; the other's carry slot is fed
   by the first adder's port 1 (walk the structure and assert) */
CHECK(carry_chain_ok, "carry chains ones->tens; types + the rule force the topology");
CHECK(cp.roots[2] == tens_adder && cp.root_ports[2] == 1, "cout reads the TENS adder's carry (ones carry is taken)");
/* all-sources rule */
CHECK(dag_plan_circuit(&reg, src5, 5, goals_sum0_only, 1, &cp2) == -1,
      "refuses when goals cannot reference every source");
/* split fan-out circuit: goals {left,right} from one pair source ->
   ONE splitter node, two roots, root_ports {0,1} */
```

  Execution tests: `dag_execute_circuit` on the synthetic circuit — assert output concatenation order, and (the sharing payoff) that the shared node's `output_successes` grows by the number of EXECUTIONS, not consumers: run once, assert the ones-adder recorded exactly 1 outcome.
- [ ] **Step 4.3:** `make test` green. Commit: `feat: multi-root circuit planning + single-run circuit execution`

---

### Task 5: Reachability pruning ("planner memo") + benchmark

**Files:**
- Modify: `src/router.c`
- Modify: `tests/test_circuit.c`

- [ ] **Step 5.1:** Precompute, at the top of both planners (skip when `reg->disable_plan_memo`):

```c
/* min_depth[t]: fewest primitive levels needed to produce distinct port
   type t from the available source TYPES (consumption ignored -- a relaxed
   over-approximation, so pruning on it can only skip branches that contain
   NO plan). Computed by fixpoint: a type compatible with some source costs
   0; a primitive output port costs 1 + max over its slots' min_depth.
   Distinct types = every input port, output port, source type and goal,
   deduped by same_port_type. */
```

  Slot-time prune in `dag_search`: if pruning enabled AND `min_depth(slot.type) > DAG_MAX_DEPTH - agenda->depth` AND no live complete BuiltEntry has a compatible unused port (the reuse escape), return 0 for this branch. The escape check keeps the prune sound in the presence of sharing.

  Lookup is over compatibility, not equality: `min_depth(T) = min over table types t with port_compatible(t, T)` — compute the table over producer-side types and scan at query time (table is small; queries are a linear scan like route_plan's type table).

- [ ] **Step 5.2:** Equivalence tests: every hermetic planning case in test_circuit and a re-run of the forced-ripple case execute twice, `disable_plan_memo` 0 and 1, asserting identical plan shapes and scores. (Equivalence is guaranteed by construction; the test pins regressions in the escape check.)
- [ ] **Step 5.3:** Benchmark (lives in circuit_demo, Task 7, but build the registry constructor now in test_circuit as a shared pattern): a layered trap registry — `L` layers of `W` single-output primitives each; layer k's inputs are layer k-1's outputs; plus `T` trap primitives whose input types have NO producer (dead ends discovered only at full depth). Goal = the one type reachable through all layers. Sized so the unpruned search takes ~1s+ (tune W, L, T at implementation; start W=4, L=6, T=12). test_circuit asserts both runs find the SAME plan; the timing claim itself is demo output, not a test assertion (no flaky CI-style timing tests).
- [ ] **Step 5.4:** `make test` green. Commit: `feat: type-reachability pruning with reuse escape (sound planner memo)`

---

### Task 6: Circuit chunks — consolidate_circuit + contract_from_circuit

**Files:**
- Modify: `include/consolidate.h`, `src/consolidate.c`
- Modify: `include/contract.h`, `src/contract.c`
- Modify: `include/plan_table.h`, `src/plan_table.c`
- Modify: `tests/test_circuit.c`

- [ ] **Step 6.1:** `plan_table.c`: add a circuit teacher + sharing-aware walkers:

```c
typedef struct {
    CircuitPlan teacher;      /* borrowed roots, strict = 1 */
    const DagSource *declared;
    size_t n_sources;
    const size_t *offsets;
} PlanCircuitTeacherCtx;
int plan_circuit_teacher(void *ctx, const double *in, size_t in_total,
                         double *out, size_t out_total);
/* Sharing-aware: visit each node ONCE (pointer set), so a shared node's
   members/sources are not double-counted. */
size_t plan_circuit_collect_members(const CircuitPlan *p,
                                    BinaryTransformNetwork **members,
                                    size_t cap);
int plan_circuit_count_sources(const CircuitPlan *p, size_t *counts,
                               size_t n_sources);
```

- [ ] **Step 6.2:** Generalize `consolidate_core` from `Port out_port` to `const Port *out_ports, size_t n_out` (out_total = sum of port totals; verification validates + canonicalizes EVERY segment; `btn_set_io_ports(&student, in_ports, n_in, out_ports, n_out)`). `consolidate_route`/`consolidate_dag` pass `&out_port, 1` — identical behavior (regression: existing chunk weight files regenerate byte-identically, gated in Task 8).
- [ ] **Step 6.3:** `consolidate_circuit(const CircuitPlan*, const DagSource*, size_t, const ConsolidateConfig*, BinaryTransformNetwork*, ConsolidateReport*)`: refusals mirror `consolidate_dag` (every source referenced — circuits guarantee ≥1 by planning; require EXACTLY one reference per source for arity truth, counted sharing-aware; ≥2 distinct member executions; primitive roots only); out_ports[g] = root g's projected port type (tags ride along).
  `contract_from_circuit(const CircuitPlan*, const DagSource*, size_t, const char *name, size_t max_samples, Contract*)`: mirror `contract_from_dag` with the circuit teacher and the multi-port output signature.
- [ ] **Step 6.4:** Tests (hermetic, synthetic 2-output circuit from Task 4's fixtures): consolidation succeeds with 2 output ports; chunk's output ports match root ports in goal order incl. tags; verification seeds evidence; chunk reproduces the teacher on the full domain; `btn_certify` passes against `contract_from_circuit`'s emission; refusals (a source referenced twice via two roots... if constructible — else skip; unconsumed source; single-execution circuit).
- [ ] **Step 6.5:** `make test` green. Commit: `feat: circuit chunks -- multi-output distillation + contract emission`

---

### Task 7: circuit_demo + Makefile + frozen artifacts

**Files:**
- Create: `tests/circuit_demo.c`
- Modify: `Makefile` (`circuit_demo`, phony `circuit`), `tests/test_circuit.c` (frozen half)

- [ ] **Step 7.1:** `circuit_demo.c`, five parts (local helpers copied per repo convention; loads hex + decimal weights, FAIL with "run ./nn_demo and ./decimal_demo first" if missing):
  1. **Split circuit (real primitives):** goals {bin4 nibble_value, bin4 nibble_value} from ONE byte source → `dag_plan_circuit` returns one `split` node, two roots, ports {0,1}; execute all 256 bytes; assert split's evidence grew by 256 (one outcome per run), not 512.
  2. **1-digit adder circuit:** goals {bin4 dec_sum, bin1 dec_carry} from (oh10, oh10, bin1) via dec_value/dec_full_add → one shared adder, two roots; 200/200 exact.
  3. **Ripple discovery:** goals {sum0, sum1, cout} from (a0,a1,b0,b1 digit sources, cin) → assert forced topology; exhaustive 20,000-case verification of the DISCOVERED circuit, strict.
  4. **Circuit chunk:** distill part 2's circuit → `dec_full_adder_unit` (21 in, 5 out, 200 samples — proven shape); save `dec_full_adder_unit_weights.txt`; emit + save `dec_full_adder_unit_contract.txt`; certify; `registry_add_certified` + replan part 2 with `require_certified` → root is the chunk. **Stretch (argv `--stretch` only):** the 2-digit circuit over one-hot sources (41 in, 9 out, 20,000 samples, raised max_samples + generous config); on refusal print the report and continue — the refusal is a documented capacity finding, not a demo failure.
  5. **Pruning benchmark:** build the layered trap registry; time `dag_plan` with `disable_plan_memo` 1 then 0 (clock() around the call); print both times and the speedup; assert identical plan shape.
- [ ] **Step 7.2:** Makefile: `circuit_demo` links `$(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT)`; phony `circuit: circuit_demo` runs it; extend `clean:`. test_circuit frozen half: load `dec_full_adder_unit_weights.txt` + contract, certify, spot-check (skip-with-FAIL-message if missing, like test_decimal).
- [ ] **Step 7.3:** `make circuit` end-to-end, then `./circuit_demo` AGAIN → `dec_full_adder_unit_*.txt` byte-identical (hash compare). `make test` green.
- [ ] **Step 7.4:** Commits: source first (`feat: circuit_demo -- discovery, verification, chunk, benchmark`), then generated `dec_full_adder_unit_*.txt` baseline (`Circuit chunk baseline: ...`).

---

### Task 8: Docs, regression, merge

**Files:**
- Modify: `README.md`; Create: memory entry; merge to master

- [ ] **Step 8.1:** Full regression: `make clean && make test` (now 10 suites); `make run && make decimal` then `git status` → NO modified weight/contract/property files (byte-identical regen proves zero training-path drift).
- [ ] **Step 8.2:** README: new "Circuit Plans" section (sharing rule + why, forced-topology story, evaluate-once executor, reachability pruning with the measured benchmark numbers, circuit chunks); update planner section, Make Targets table (`make circuit`, 10 suites), file formats unchanged.
- [ ] **Step 8.3:** Update the project memory (circuit-plans milestone entry + MEMORY.md line; note what the benchmark measured).
- [ ] **Step 8.4:** Commit docs; merge `circuit-plans` → master (fast-forward), delete branch, re-run `./test_circuit` on master.

---

## Self-review notes

- **Spec coverage:** sharing rule → Task 3; representation/ownership → Task 1; evaluate-once + circuit execute → Task 2; multi-root + all-sources → Task 4; memo (as amended) + benchmark → Tasks 5/7.5; circuit chunks + certification → Task 6; demos/tests/regression → Tasks 7/8. Spec's Pareto memo intentionally amended in Task 0 (unsound under sharing) — recorded in the spec itself.
- **Type consistency:** `CircuitPlan.roots/root_ports/root_count/owned/owned_count/strict` used consistently in Tasks 1, 2, 4, 6, 7; `edge_port` fallback defined once (Task 2) and relied on by Tasks 3-4; `disable_plan_memo` defined in Task 1, used in Tasks 5/7.
- **Known risk order:** Task 3 (search rewrite) is the risk peak — it lands AFTER the executor rewrite is proven green on the 20,000-case strict sweep, so executor and search bugs cannot mask each other.
