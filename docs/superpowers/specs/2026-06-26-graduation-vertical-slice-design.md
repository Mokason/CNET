# Graduation Vertical Slice (memory → certified units → router composition) — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** Close the loop end-to-end: mine a near-deterministic regularity from the
book memory, distill it into **frozen, certified, positionally-tagged primitives**,
register them, **evict** the corresponding tiles from the fuzzy memory, and have the
**router auto-compose** the certified units into the answer. Proves CNET is
**non-monolithic**: knowledge becomes discrete proven units a router composes — not
weights in one net. With **benchmarks** per phase.

---

## 1. Goal & acceptance criteria

1. **Real graduation operation** `graduate_deterministic(...)` (a library function, not a
   demo): mine → distill → `btn_certify` → `registry_add_certified` → evict tiles.
2. **Router composes the certified units.** With only an input port and a goal port, the
   planner **discovers** a multi-hop plan over the graduated units (like `route_demo`
   discovers `[hex_value, increment]`), and `route_execute` reconstructs the proven
   continuation. **Plan length ≥ 2** (composition, not a monolith); the plan is
   `require_certified` end-to-end.
3. **Knowledge moves soft → hard.** Fuzzy tile-memory `total` **decreases** by the evicted
   tiles; the registry gains certified entries. Printed before/after.
4. **Honest on a real book** (`aivalueplaybook.pdf`) and **guaranteed on a controlled
   corpus** (a deterministic run for the 2-hop proof).
5. **Benchmarks**: per-phase wall time (mine / distill+certify / register / route_plan /
   route_execute) + counts (vocab, exemplars, units graduated, plan length, memory delta).

## 2. The non-monolithic spine

The "model" = **registry of frozen certified primitives + router + fuzzy memory**. This
slice takes one regularity out of the *soft* memory and makes it **two discrete, named,
independently-`btn_certify`-proven units** with **distinct tagged ports**, then lets the
router **auto-compose** them. The capability emerges from composition; nothing is a
monolith; the memory shrinks as knowledge graduates.

## 3. The concrete mechanism

CNET's router composes by **port type + semantic tag**, not by value. So free word-
transitions are made composable by **positional tags**:

- Mine the corpus for **near-deterministic bigrams** `w → dominant(w)` over the *decidable
  core* (count ≥ `min_count`, dominant share ≥ `min_share`) — the endgate/name-gate pattern.
- Scope a **small vocab** `V` to just the words those bigrams touch (BTNs stay tiny).
- Instantiate the SAME mined map as **two positionally-tagged certified units**:
  - `step_ab`: `ONEHOT[V] {tag "w_a"} → ONEHOT[V] {tag "w_b"}`
  - `step_bc`: `ONEHOT[V] {tag "w_b"} → ONEHOT[V] {tag "w_c"}`
  Both certified over the full decidable bigram core; the distinct tags force a single
  composition order.
- Pick a **seed** `X` with a deterministic 2-step run `X → Y → Z` (both `X` and `Y` have a
  dominant next). `route_plan(input = w_a port, goal = w_c port)` discovers `[step_ab,
  step_bc]`; `route_execute(one-hot X)` yields `one-hot Z` — the 2-step continuation
  assembled from two frozen proven units the router chose itself.

(Two tagged units that compute the same proven map but compose into a capability —
`X→Z` — neither provides alone. The mechanism is stated plainly, not hidden.)

## 4. The loop (end to end)

```
corpus ─▶ tile_memory.ingest (fuzzy tiles)          [the soft memory]
corpus ─▶ mine near-deterministic bigrams ─▶ scope vocab V
       ─▶ build step_ab, step_bc (tiny BTNs) ─▶ btn_certify each (proven)
       ─▶ registry_add_certified (require_certified=1)          [hard units]
       ─▶ tilemem_evict_containing(run words)  ─▶ memory total DROPS
route_plan(w_a → w_c) ─▶ discovers [step_ab, step_bc]
route_execute(one-hot X) ─▶ one-hot Z   (== proven 2-step continuation)
```

## 5. Components (a real operation)

- **`include/corpus/graduate.h`, `src/corpus/graduate.c`** — `graduate_deterministic(const
  StrList *corpus, PrimitiveRegistry *reg, TileMemory *mem, int min_count, double
  min_share, GraduateReport *rep)`: mines, builds+certifies the two tagged units, registers
  them, evicts matching tiles; fills a report (vocab, exemplars, seed run, timings).
- **`tilemem_evict_containing(TileMemory *m, const char *needle)`** — new public op on the
  tile memory: removes HOT tiles whose key contains `needle`; returns the count evicted.
  (A real eviction operation — graduated knowledge leaves the fuzzy memory.)
- **`tests/test_graduate.c`** + `make graduate` — runs the full loop + the router
  composition + the benchmark table, on a controlled corpus (guaranteed 2-hop) and the real
  book (honest result; auto-skips if absent). Self-checking, nonzero on failure.

Reuses `nn` (BTN), `contract` (`btn_certify`/`contract_init_borrowed`), `router`
(`registry_*`, `route_plan`, `route_execute`), `tile_memory`, `corpus_split`, `pdf_extract`.

## 6. Benchmarks (required output)

A printed table:
```
PHASE                 TIME(ms)   METRICS
mine bigrams             ...      vocab=V, decidable-core exemplars=E, seed run X->Y->Z
distill + certify        ...      2 units, both CERT (exact), exemplars/unit=E
register                 ...      registry certified entries +2
evict from memory        ...      tiles evicted=K, memory total before>after
route_plan               ...      plan length=2, names=[step_ab, step_bc]
route_execute            ...      X -> Z (== proven), result one-hot ok
```
Plus a one-line summary: units graduated, plan hops, memory delta, all-certified.

## 7. Testing / proofs

- **Controlled corpus** (a small text with a deterministic run, e.g. "alpha beta gamma …"
  repeated): asserts mine finds the run; both units `btn_certify` (return 0); `route_plan`
  length == 2 with the expected names; `route_execute(X)` == `Z`; memory total dropped by
  the evicted tiles; `require_certified` was set.
- **Real book**: graduate from `aivalueplaybook.pdf`; report what it actually mined (the
  top near-deterministic run may be short — reported honestly); the router composition is
  asserted on whatever ≥2-hop the controlled corpus guarantees.
- Run existing `make pdftest` / `make tiermem_test` after (additive).

## 8. Honest scope / limits

- The router composes by **port tag/type**; positional tags encode chain order — stated
  plainly, the slice's central mechanism, not magic.
- Two tagged units compute the **same** mined map; the value is the *composition* (`X→Z`),
  not two different functions. A genuinely heterogeneous 2-stage transform is a follow-up.
- Real prose yields **short** deterministic runs; the controlled corpus carries the 2-hop
  guarantee, the book is reported as-is.
- One slice (two units, one composition). It demonstrates the loop closes; it is **not**
  the full DreamCoder-style library-distillation engine (`library_evolve` is that).

## 9. Project notes

- **No git on this repo.** Spec written, not committed; verify via filesystem.
- New: `src/corpus/graduate.c`, a small `tilemem_evict_containing` addition to
  `tile_memory.c`, `tests/test_graduate.c`, the `graduate` make target. **Zero
  core/router/contract edits** (consumes their public APIs).
