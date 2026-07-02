# Dual-Track Expansion Gate (3C) — Cost Truth That Acts — Design

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** A1 implemented + green (`make test` + `make compounding_bench` pass, adversarially reviewed). A2 DEFERRED/reserved by decision 2026-06-19 — no current-domain payoff (decimal/hex have no competing producer for a chunk's signature, so A1 already pins the teacher de-facto); revisit when a domain gains competing producers.
**Milestone:** Cycle 3C of the Cost-Aware Compounding arc. 3A *reported* cost truth (advisory,
transient in `LibraryReport`). 3C makes it **act and persist**: a minted chunk durably carries the
lean sub-plan it replaced (its "expansion recipe"), and under low-power planning the engine drops
the compute-heavy chunk so the planner rebuilds the obligation from the lean primitives.

## Context

Cycle 2 measured the trade-off; 3A labelled it. The flagship `dec_full_adder_unit` is
**planning-beneficial but compute-expensive** (`compute_beneficial = false`, `compression_ratio =
0.093`, ~10.8× MACs vs the 3 primitives it replaces). 3A's non-goals explicitly handed three things
to 3C: *act* on the label, *persist* it (`RegistryEntry` was untouched), and define the *consumer*.

3C's consumer is the **planner**, gated by the existing `PowerMode`. `CNET_POWER_LOW` already means
"among equal-reliability candidates, prefer the lower inference cost" (see `PowerMode` in `include/router.h`),
but a chunk *out-reliabilities* its sub-plan (0.944 vs 0.841³ = product), so cost never bites today.
3C extends LOW's contract: a compute-heavy chunk with a known lean expansion is set aside in LOW so
the lean primitives win — the cost truth finally changing a plan.

## Decisions locked from the brainstorm

- **Dual-track, plan-time expansion** (not execute-time). The planner is the consumer; the executor
  is untouched.
- **Names-only recipe + re-plan** (variant A), not a full structural splice. The recipe stores the
  ordered teacher primitive *names*; the lean sub-plan's wiring is re-derived by the existing planner.
  Tiny, trivially serializable, reuses the planner, and keeps the "both wins" boundary honest
  (LOW re-incurs the sub-search — that is exactly the cost a later cycle will earn back).
- **Consumer mechanism = A1: exclusion gated by recipe-availability** (not A2's recipe-scoped
  re-plan). In LOW, an expandable chunk whose teacher names are *all present in the registry* becomes
  invisible to the planner, so the existing search rebuilds the obligation from the lean primitives.
  A2 (pin the *exact* teacher set via mid-search graft) is a reserved follow-on, only load-bearing
  once a domain has competing producers for the same port signature.
- **`compute_beneficial` chunks get no recipe** — they are already lean; nothing to expand.
- **DEFAULT mode is byte-identical legacy.** The behavior is doubly gated (`power_mode == LOW` AND a
  new opt-in `expand_in_low_enabled`, zero-init off), mirroring CNET's `require_certified` /
  `lifecycle_enabled` / `strict` opt-in discipline. An existing `CNET_POWER_LOW` user who relies on
  the cost-tiebreak-only contract is unaffected until they set the new flag.

## Persistence (`RegistryEntry` + `.expansion` sidecar)

The 3A label is transient in `LibraryReport`; 3C gives it a durable home on the entry, alongside the
recipe.

```c
#define EXPAND_MAX_PRIMS 16
typedef struct {
    char   primitives[EXPAND_MAX_PRIMS][64]; /* teacher primitive names, in plan order, dupes kept */
    size_t primitive_count;
} ExpansionRecipe;

/* added to RegistryEntry: */
size_t teacher_mac, student_mac;   /* 3A cost truth, persisted */
int    compute_beneficial;
ExpansionRecipe *recipe;           /* owned; non-NULL iff this is an expandable chunk */
int    expand_in_low;              /* policy bit, read directly by the planner. Set at attach
                                      time = !compute_beneficial WHEN a recipe is actually
                                      attached, else 0. A SEPARATE persisted field (not
                                      recomputed on read) so a future cycle can decouple the
                                      expansion *policy* from the *cost fact* without disturbing
                                      compute_beneficial. */
```

- **Name ownership:** `RegistryEntry.name` is a *borrowed* `const char *`, so the recipe owns its
  names **by value** (`char[64]`, the repo's name-width convention). Re-planning looks these up in
  the live registry by `strcmp`.
- **Capacity:** a teacher exceeding `EXPAND_MAX_PRIMS` captures **no** recipe → the chunk is simply
  not expandable (`expand_in_low` stays 0). Graceful, like a too-deep plan; never a hard failure.
- **Save:** `registry_save` writes a tiny text `<name>.expansion` only for entries with a recipe —
  same per-entry loop and `registry_build_path` pattern as `.btn`/`.contract`/`.stats`
  (in `src/router.c`). Format (mirrors the `.stats` text style):
  ```
  expansion v1
  teacher_mac <N>
  student_mac <N>
  compute_beneficial <0|1>
  expand_in_low <0|1>
  primitives <count>
  <name0>
  <name1>
  ...
  ```
- **Load:** there is no monolithic `registry_load` (primitives load individually), so 3C adds a
  symmetric `registry_load_expansion(reg, name, dir)` that reads the sidecar and attaches the recipe
  + cost to the named entry. The shared setter `registry_set_expansion(reg, name, names, n,
  teacher_mac, student_mac, compute_beneficial)` (used by the mint path in `finalize_chunk` and by
  tests) allocates the recipe **only when `1 ≤ n ≤ EXPAND_MAX_PRIMS`** and sets `expand_in_low =
  !compute_beneficial` **only when a recipe was actually attached** (else 0). The loader performs an
  analogous allocation + restore (reading the persisted `expand_in_low` value directly) with the same
  capacity guard, so an over-cap or recipe-less chunk is never flagged expandable and on-disk state
  agrees with in-memory.

## Mint capture (`src/library.c`)

`finalize_chunk` already receives `teacher_mac` (3A) and holds the freshly-registered chunk. 3C
extends the teacher-cost walk to also collect the teacher primitive **names** from the same source:

- `evolve_route`: the `RoutePlan.names[i]` over `plan.length`.
- `evolve_dag` / `evolve_circuit`: `owned[i]->name` over the `DAG_PRIMITIVE` nodes in `owned[]`
  (the same flat walk that sums `teacher_mac`).

The 3A helpers `route_teacher_mac` / `owned_teacher_mac` become `route_teacher_info` /
`owned_teacher_info`, filling a small `TeacherInfo { size_t mac; char names[EXPAND_MAX_PRIMS][64];
size_t count; }`. **The names are copied by value** (not borrowed pointers): for the dag/circuit
paths the planner's `DagNode`s are freed (`dag_free`/`circuit_free`) *before* `finalize_chunk` runs,
so a `const char *` into `owned[i]->name` would dangle — the by-value copy taken at the capture site
is what makes the mint path safe (the over-cap case sets `count = 0`, mac still full). `finalize_chunk`
gains the `TeacherInfo *` (replacing the lone `teacher_mac` param), writes the 3A cost fields exactly
as today, and after the chunk is registered calls `registry_set_expansion(...)` — passing the names for
a compute-**heavy** chunk, or `NULL`/`0` for a compute-**beneficial** one (cost truth persisted, no
recipe). If `count > EXPAND_MAX_PRIMS`, no recipe is attached (graceful cap). No new
`btn_cost(student)` — finalize still computes it once.

## Planner hook — the consumer (`src/router.c`)

The original `entry_usable` (in `src/router.c`) is factored into a `_base`
form (the pre-3C logic, reused without recursion by `recipe_available`) plus one 3C clause. This is
the chokepoint every planner stage (reach-table build, reach lookup, candidate loops, beam admission)
already routes through, so the hook lands in exactly one place:

```c
/* require_certified gate: when the registry demands certification, an
   uncertified entry is invisible to the PLANNERS (executors never read
   the registry). The "base" form excludes the 3C expansion clause so the
   recipe-availability check can use it without self-recursion. */
static int entry_usable_base(const PrimitiveRegistry *reg, size_t i) {
    if (reg->lifecycle_enabled &&
        (reg->entries[i].state == PRIM_RESET || reg->entries[i].shadow_of != NULL)) {
        return 0;
    }
    return !reg->require_certified || reg->entries[i].certified;
}

/* 3C: every distinct recipe primitive resolves to a base-usable entry OTHER
   than `self` (a self-referential name can never satisfy its own expansion, so
   such a recipe is treated as unavailable -> the chunk degrades to "stays
   usable" rather than hiding itself into an unplannable obligation). */
static int recipe_available(const PrimitiveRegistry *reg, const ExpansionRecipe *r,
                            size_t self) {
    size_t k, j;
    if (r == NULL || r->primitive_count == 0) return 0;
    for (k = 0; k < r->primitive_count; ++k) {
        int found = 0;
        for (j = 0; j < reg->count; ++j) {
            if (j == self) continue;
            if (reg->entries[j].name != NULL && reg->entries[j].btn != NULL &&
                strcmp(reg->entries[j].name, r->primitives[k]) == 0 &&
                entry_usable_base(reg, j)) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

static int entry_usable(const PrimitiveRegistry *reg, size_t i) {
    const RegistryEntry *e = &reg->entries[i];
    if (!entry_usable_base(reg, i)) return 0;
    /* 3C dual-track: in LOW power (opt-in), a compute-heavy chunk with an
       available lean recipe is hidden so the planner rebuilds from primitives. */
    if (reg->power_mode == CNET_POWER_LOW && reg->expand_in_low_enabled &&
        e->expand_in_low && e->recipe != NULL && recipe_available(reg, e->recipe, i)) {
        return 0;
    }
    return 1;
}
```

When the gate fires, the chunk is excluded and the planner satisfies the obligation from the lean
primitives via its normal search. Reachability is preserved because the recipe primitives are, by
construction, the teacher that produced the chunk's outputs — so any goal the chunk reached, they
reach.

**Every branch has a defined, graceful outcome — 3C never converts a cost decision into a planning
failure:**

- **(a) DEFAULT, or LOW with the opt-in off** → `entry_usable == entry_usable_base`, byte-identical
  legacy. The chunk is a normal candidate.
- **(b) LOW + opt-in + recipe *available*** → the chunk is hidden; the planner **organically
  composes** the obligation from the recipe's (present) lean primitives. This is the dual-track win.
- **(c) LOW + opt-in + recipe *unavailable*** (a teacher primitive is missing, or the recipe is
  self-referential — `recipe_available(.., self)` skips the chunk's own entry, so a self-reference
  resolves as unavailable) → the chunk **stays usable**; the planner uses the chunk as-is. A
  self-referential recipe is only reachable via direct `registry_set_expansion` misuse or a corrupted
  sidecar — never via the mint path, whose teacher predates the chunk — so this is a robustness floor,
  not a normal path.

The recipe is thus load-bearing in two ways: it **gates** the exclusion (path c keeps an under-equipped
or corrupted chunk plannable), and it carries provenance + portability.

**Known limitation (documented, not hidden):** expansion trades away the chunk's depth-collapse. If
the lean plan would exceed `DAG_MAX_DEPTH` (8) where the chunk fit, LOW-mode planning can fail to
find a plan that DEFAULT found. The compounding benchmark's Run A proves the raw 4-digit plan is
within depth, so the demonstrator is safe; a fall-back-to-chunk-on-expansion-failure is a reserved
refinement (it edges toward "both wins" and is out of A1 scope).

## Property: cross-mode equivalence is accuracy, not a bug

LOW-power expansion runs the **teacher** primitives, whose outputs are the ground truth the chunk was
trained to *approximate*. So a chunk and its expansion are **not** required to agree bit-for-bit, and
switching power mode can change results — toward the teacher, i.e. usually *more* accurate. This is a
deliberate property, not a defect:

- The chunk is a lossy student; the recipe is its lossless teacher. DEFAULT trades accuracy for the
  chunk's planning-cheapness; LOW trades planning-cheapness back for the teacher's accuracy *and*
  compute-cheapness.
- Therefore tests assert **behavior and round-trip** — "the LOW plan contains the recipe primitives,
  not the chunk" — and **never** assert cross-mode output equality. The compounding benchmark's Run C
  is observational only for the same reason.
- A corollary: a future capacity improvement that makes the chunk compute-beneficial flips it to
  recipe-less (no expansion), and the property holds vacuously — DEFAULT and "LOW" then coincide.

## Lifecycle / bookkeeping

- `registry_init` zeroes the new `expand_in_low_enabled` flag (→ legacy).
- `registry_add` zero-inits the new entry fields (`recipe = NULL`, `expand_in_low = 0`, costs `0`).
- `registry_free` and `registry_remove_last` `free()` each entry's owned `recipe`.

## Testing (TDD, in `make test` via a new `tests/test_expansion.c`)

Assert *behavior and round-trip*, never cross-mode output equality (see the "Property: cross-mode
equivalence is accuracy, not a bug" section above):

1. **DEFAULT keeps the chunk.** Register two lean primitives + an expandable chunk (attached via
   `registry_set_expansion`); plan a task the chunk satisfies; assert the plan **contains the chunk**.
2. **LOW expands.** Same registry, `power_mode = LOW`, `expand_in_low_enabled = 1`; assert the plan
   **contains the recipe primitives and NOT the chunk**.
3. **Availability gate.** Remove one teacher primitive; in LOW the chunk stays usable (recipe
   unavailable) → plan **contains the chunk** (graceful, no planning failure).
4. **Persistence round-trip.** `registry_save` → a `<name>.expansion` sidecar exists with the recipe
   + cost; fresh registry + `registry_load_expansion` restores `recipe`/`expand_in_low`/costs; the
   LOW-expand behavior reproduces.
5. **Mint capture (integration, via the gate test or a focused fixture).** After a real mint, the
   chunk's entry has a non-empty recipe whose names equal the teacher primitives and
   `expand_in_low == !compute_beneficial`.

6. **Robustness.** A self-referential recipe → chunk stays usable (no self-hide); an over-cap teacher
   (> `EXPAND_MAX_PRIMS`) → no recipe and `expand_in_low` stays 0.

**Observational, shipped** (3A-style, no asserts on magnitudes): a "Run C (LOW)" probe in
`benchmark_compounding_loop.c` showing macs fall back toward Run A and `plan_length` rise as the
chunk expands — self-documenting the dual track. Measured: Run C == Run A exactly
(`nodes=10471481, len=12, macs=1232, chunk_uses=0`) vs Run B (`nodes=2577387, len=4, macs=13312,
chunk_uses=4`). Gated behind the same opt-in; the existing Run A/B asserts are untouched.

## Non-goals (YAGNI)

- **A2 (recipe-scoped re-plan / exact-teacher pinning) and execute-time "both wins"** — reserved,
  explicitly named, each its own later increment. The structural recipe A2/both-wins would need is
  deliberately *not* built now.
- **No executor change, no `BinaryTransformNetwork` format change.** Recipe + cost live on the
  registry entry and its sidecar, never in `<name>.btn`.
- **No automatic re-derivation of cost on load** — the persisted cost is authoritative; nothing
  recomputes `btn_cost` at load.
- **No fall-back-to-chunk on expansion planning failure** (the depth-limit case) — reserved.

## Resolved decisions (from review)

1. **Doubly-gated opt-in (`power_mode == LOW` AND `expand_in_low_enabled`)** keeps every existing
   path byte-identical and does not surprise existing `CNET_POWER_LOW` (tiebreak-only) users.
2. **Names-only recipe, A1 exclusion** is the minimal consumer that yields the demonstrable dual
   track; A2's pinning is reserved until a domain has competing producers.
3. **Recipe owns names by value** because `RegistryEntry.name` is borrowed; `char[64]` matches the
   repo convention.
4. **Graceful caps everywhere** — over-`EXPAND_MAX_PRIMS` teacher → no recipe; unavailable teacher
   prims → chunk stays usable. 3C never converts a cost decision into a planning failure.
5. **`expand_in_low` is a separate persisted field, not a derived expression.** Its value is fixed at
   attach/mint time (`!compute_beneficial` iff a recipe was attached, else 0) and read directly by the
   planner — O(1), survives save/load, and gives a future cycle one place to decouple the expansion
   policy from the cost fact. (Today the value *does* derive from `compute_beneficial`; the field
   exists so it need not stay that way.)

## Files changed (working tree; no commits — version control is user-managed)

| File | Owns |
|---|---|
| `include/router.h` | `EXPAND_MAX_PRIMS` + `ExpansionRecipe`; 5 new `RegistryEntry` fields (`teacher_mac`, `student_mac`, `compute_beneficial`, `recipe`, `expand_in_low`); `PrimitiveRegistry.expand_in_low_enabled`; `registry_set_expansion`/`registry_load_expansion` decls. |
| `src/router.c` | **The planner hook** — `entry_usable_base` + `recipe_available(.., self)` + the 3C clause in `entry_usable`. **Persistence** — `registry_set_expansion`, `registry_load_expansion`, `registry_write_expansion`, and the `.expansion` write spliced into `registry_save`. **Bookkeeping** — new-field zero-init in `registry_add`, the flag in `registry_init`, recipe `free()` in `registry_free`/`registry_remove_last`. |
| `src/library.c` | **The mint capture** — `TeacherInfo`; `route_teacher_info`/`owned_teacher_info` (replacing the `*_teacher_mac` helpers, names added); `finalize_chunk` takes `const TeacherInfo *` and calls `registry_set_expansion`; the three `evolve_route`/`evolve_dag`/`evolve_circuit` call sites capture names **before** freeing the plan. |
| `tests/test_expansion.c` *(new)* | 23 checks: planner gates, availability, policy, robustness (self-ref + over-cap), persistence round-trip. |
| `tests/test_distillation_gate.c` | 5 mint-capture asserts on the real circuit chunk (Branch C): recipe present, names `{dec_value ×2, dec_full_add ×1}`, `expand_in_low == !compute_beneficial`, entry cost == report. |
| `tests/test_all.c` | declares + calls `run_test_expansion`. |
| `Makefile` | `tests/test_expansion.c` added to the `test_all` deps + compile line. |
| `tests/benchmark_compounding_loop.c` | the restored 3A cost-label print **and** the new Run C (LOW) observational probe. |
| `docs/superpowers/specs/2026-06-19-dual-track-expansion-3c-design.md` | this spec. |

> Note the split that the recap's reconstruction inverted: `finalize_chunk`/recipe-**capture** lives in
> `src/library.c`; `registry_save`/`registry_load_expansion`/the planner **hook** live in
> `src/router.c`. The header owns only declarations + struct shape.
