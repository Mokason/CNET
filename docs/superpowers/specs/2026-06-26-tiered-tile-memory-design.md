# Tiered Tile Memory (AICIMO-adapted) — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** A **production** tiered, fuzzy, decaying tile-memory subsystem — the
generation-layer "memory from past books" that compounds (reuses across books),
stays **bounded in RAM** while growing unbounded on disk, and graduates stable
knowledge into certified contracts. Adapts AICIMO's `TiledMemoryIndex` (hashed-vector
fuzzy retrieval) + `GraphMemoryHead` decay, plus the HOT/WARM/COLD residency model.

> **Not a demo.** This is a first-class library (`src/corpus/tile_memory.c`) with a
> documented API, a real persistent store that survives across runs, a proper test
> suite (`tests/test_tile_memory.c`), and a usable ingest/recall entry point. No
> throwaway self-checking `*_demo.c`.

---

## 1. Goal & acceptance criteria

1. **Production library.** `tile_memory` exposes a clean, documented C API (open / ingest
   / search / decay / persist / stats), self-contained (stdlib + libm), zero core edits.
2. **Compounds (AICIMO fuzzy reuse).** Ingest is **fuzzy-dedup**: a tile whose cosine to
   an existing tile ≥ τ is a **reuse** (bump count/heat), not a new tile. Measured: a
   *related* book reuses materially more than exact 3-gram match did; identical = 100%
   reuse; unrelated ≈ 0%.
3. **Bounded RAM, unbounded disk (the 3-tier point).** HOT is capacity-capped; ingest the
   whole `aivalueplaybook.pdf` and **HOT stays ≤ cap** while total capacity (HOT + WARM on
   disk) grows; recall still hits WARM-resident tiles. This is "not all in memory," proven.
4. **Persists across runs.** A canonical store accumulates: run twice on the same book →
   idempotent (fuzzy-dedup); run on a new book → the store + recall grow. The memory is
   real and durable, not rebuilt each run.
5. **Decay/forgetting.** `tilemem_decay` cools unused tiles (AICIMO decay-by-1); zero-heat
   HOT tiles spill to WARM; hits promote WARM→HOT.
6. **Contract graduation (stretch).** Near-deterministic tiles → `btn_certify` → evicted
   from memory into the contract library.
7. **Real tests.** `tests/test_tile_memory.c` covers the API + the three proofs; runnable
   as `make tiermem_test`, green/red gated.

## 2. The Tile (AICIMO's unit, in C)

```c
typedef struct {
    char     id[64];        /* slug of key/label */
    char    *key;           /* the trigger text (the "task"/context) */
    char    *value;         /* the continuation/answer */
    char     label[32];     /* derived tag (first content word / uppercase label) */
    char     source[64];    /* provenance: which PDF/book */
    int      heat;          /* recency counter (decay/eviction) */
    int      count;         /* observation count */
    float   *vec;           /* D-dim FNV-1a hashed dense vector (normalized) */
} Tile;
```
One tile ≈ a sentence/passage. `vec` is AICIMO's hashed dense vector
(`AccumulateHashedVector` + normalize, ported from C#): tokenize → FNV-1a hash each
token into `[0,D)` → accumulate → L2-normalize. Retrieval is cosine (dot of normalized
vecs).

## 3. The three residency tiers ("not all in memory")

```
INGEST ─▶ HOT (in-RAM, cap-capped)  ── decay + spill (coldest) ──▶  WARM (on-disk file, paged on miss)
                 ▲   promote-on-hit            │ compact
                 └────────────────────────────┘
   stable + near-deterministic tiles ─▶ CONTRACT (btn_certify) ─▶ evicted from memory
```
- **HOT** — bounded RAM array (cap, e.g. 1000 tiles). New + recently-hit tiles. Fuzzy
  search scans HOT first (small, fast).
- **WARM** — on-disk tile records (binary). Receives tiles spilled from HOT (lowest heat).
  Searched by streaming-scan on a HOT miss; a hit is **promoted** back to HOT. Grows
  unbounded on disk; **RAM stays bounded**. (True zero-copy `mmap` is the production
  refinement — CNET's WARM cascade view is the precedent, cited not rebuilt here.)
- **COLD/contract** — stable tiles graduate out (§6); compaction reclaims WARM space.

## 4. AICIMO logic adapted (from `AICIMO_Lib/Memory`)

- **Fuzzy retrieval** (`TiledMemoryIndex`): hashed dense vector + cosine top-K, light
  stemming + stop-word filter. Ported C#→C.
- **Fuzzy-dedup ingest** (the compounding upgrade): cosine ≥ τ to an existing tile →
  reuse; else new. Lifts the ~5% exact prose reuse toward semantic reuse.
- **Decay-eviction** (`GraphMemoryHead`: "decay positive cells by 1 every N"):
  `tilemem_decay` drops heats; zero-heat HOT tiles spill to WARM.

## 5. API (production)

```c
typedef struct TileMemory TileMemory;
typedef struct { const Tile *tile; float score; } TileHit;

/* Open/create the persistent store (HOT snapshot + WARM file). Loads prior state. */
TileMemory *tilemem_open(const char *store_dir, int dim, int hot_cap, double dedup_tau);
void        tilemem_close(TileMemory *m);          /* persists HOT + WARM */

/* Fuzzy-dedup ingest. Returns 1 if a new tile was added, 0 if reused. */
int    tilemem_ingest(TileMemory *m, const char *key, const char *value,
                      const char *label, const char *source);
/* Top-K fuzzy recall across HOT (+ WARM on miss); promotes hits. Returns #written. */
int    tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap);
void   tilemem_decay(TileMemory *m);               /* AICIMO decay-by-1 + spill cold HOT */

size_t tilemem_hot_count(const TileMemory *m);
size_t tilemem_warm_count(const TileMemory *m);
size_t tilemem_total(const TileMemory *m);
/* Near-deterministic tiles eligible for contract distillation. */
size_t tilemem_certifiable(const TileMemory *m, int min_count, double min_share);
```

## 6. Phases (each a measurable milestone)

1. **Tile + fuzzy retrieval + fuzzy-dedup** (in-RAM only). Port AICIMO hashed-vector
   recall; ingest with cosine-dedup. *Proof: related-book fuzzy reuse > exact-match reuse;
   identical = 100%; unrelated ≈ 0%.*
2. **HOT/WARM residency + decay + persistence.** Cap HOT, spill to WARM file, promote on
   hit, decay, load/save the store. *Proof: ingest the whole AI book; HOT ≤ cap while
   total grows; recall hits WARM; re-run is idempotent and durable.*
3. **Contract graduation** (stretch). Near-deterministic tiles → `btn_certify` → evicted.

## 7. Components

- **`include/corpus/tile_memory.h`, `src/corpus/tile_memory.c`** — the library.
- **`tests/test_tile_memory.c`** — real test suite (`make tiermem_test`): API unit tests +
  the three proofs + a real-PDF integration run (auto-skips if the PDF is absent).
- Reuses `corpus_split`/`corpus_store`/`pdf_extract` for real ingestion. The existing
  `retrieval.c` (fine-grained kNN n-gram store) stays; `tile_memory` is the coarse,
  fuzzy, **tiered, persistent** production memory — complementary.

## 8. Data flow

```
PDF ─▶ pdf_extract ─▶ corpus_split ─▶ per sentence: tilemem_ingest(key,value,label,source)
                                       (fuzzy-dedup → reuse or new; HOT, spill to WARM on cap)
recall: query ─▶ tilemem_search ─▶ top-K tiles (HOT + WARM), heat-promoted
maintenance: tilemem_decay each cycle; close persists HOT+WARM.
```

For sentence ingestion each tile is a **passage tile**: `key = value = the sentence`
(it is both the retrieval trigger and the stored content), `label` = its first content
word (skipping stop words), `source` = the PDF/book name. `vec` is built from `key`.
Fuzzy-dedup then merges near-duplicate/paraphrased sentences (the cross-book reuse).
(A `key=sentence, value=next-sentence` continuation variant for generation is a later
refinement, not in scope here.)

## 9. Testing

- **Unit:** tile vectorize/cosine; fuzzy-dedup (near-duplicate → reuse, distinct → new);
  search ranking; decay drops heat + spills; save/load round-trip; persistence across two
  `open/close` cycles is idempotent on identical input.
- **Proofs:** fuzzy-reuse > exact-reuse (Phase 1); HOT ≤ cap while total grows + WARM-paged
  recall hits (Phase 2); near-deterministic count (Phase 3).
- **Real PDF:** ingest `aivalueplaybook.pdf` into a persistent store; report HOT/WARM/total,
  reuse rate, and a sample recall; re-run shows idempotent durability.
- Run existing `make pdftest` / suite after (additive).

## 10. Honest scope / limits

- Hashed-vector cosine (bag-of-tokens), **not** learned embeddings — cheap, dependency-free,
  AICIMO-faithful; semantic reach is limited vs real embeddings.
- WARM = on-disk file streamed/scanned on miss; true `mmap` zero-copy is the refinement.
- `τ` (dedup) and `hot_cap` are tunable; the suite documents chosen defaults and the
  over/under-merge failure modes.
- Largest subsystem so far; Phases 1–2 are the heart, Phase 3 the contract bridge.

## 11. Project notes

- **No git on this repo.** Spec written to disk, not committed; verify via filesystem.
- New code under `src/corpus/tile_memory.c`; reuses corpus/pdf modules + (Phase 3) the
  contract/`btn_certify` machinery. Zero core/router edits.
