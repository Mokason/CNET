# TF-IDF Retrieval (Lever 2) — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** Upgrade the tile-memory retrieval from raw bag-of-words to **idf-weighted
cosine**, so rare distinctive terms decide the match instead of common ones — fixing the
demonstrated "last mile" miss. With benchmarks.

---

## 1. Goal & acceptance criteria

1. **idf-weighted retrieval** in `tile_memory`: maintain document frequency `df[dim]` +
   tile count; score with idf-weighted cosine.
2. **Fixes the demonstrated miss**: the query *"What is the last mile problem in AI
   projects?"* now returns a sentence containing **"last mile"** in its top-K (it was
   absent before — common words `AI`/`problem`/`project` drowned the rare `mile`).
3. **No regression**: existing `make tiermem_test` stays green (fuzzy-dedup uses plain
   cosine on the stored vecs; only *search* changes; positive scores preserved).
4. **Benchmarks**: idf table build + per-query scoring overhead (idf adds O(dim) per tile).

## 2. Why this is the right fix (grounded)

Document frequencies in the book: `AI` 1101, `problem` 147, `project` 123 vs `mile` 16,
`last mile` 12. Raw bag-of-words counts let the common words dominate, so the last-mile
answer (which *contains* the query's rare words) was buried. **idf** = `log((N+1)/(df+1))+1`
makes `mile`≈6, `AI`≈1.8 → the rare decisive term wins. This is a weighting fix, not a
synonym problem (the answer shares words with the query).

## 3. Mechanism

- **df accumulation**: on each *new* tile ingest, for every hashed bucket `i` with
  `vec[i] > 0`, `df[i]++`; `doc_count++`. (Reuse/dedup does not bump df.) Monotonic — not
  decremented on spill/evict (idf is a soft weighting; slight staleness is harmless).
- **idf at score time**: `idf[i] = log((doc_count+1)/(df[i]+1)) + 1`.
- **idf-weighted cosine**: weight the query vector by idf and L2-normalize once; for each
  candidate tile, weight its stored (normalized) vec by idf, L2-normalize, dot with the
  query. `O(dim)` per tile. Replaces the plain `cosine()` inside `tilemem_search` (HOT +
  WARM). **Ingest/dedup keep plain cosine** (df-independent near-duplicate detection).
- **Persistence**: `df` + `doc_count` save/load with the store (`store_dir/idf.bin`), so a
  reloaded memory keeps its idf.

## 4. Components

- **Modify `include/corpus/tile_memory.h`, `src/corpus/tile_memory.c`**:
  - add `unsigned *df; size_t doc_count;` to `TileMemory`;
  - allocate `df` in `tilemem_open` (size `dim`), load `idf.bin` if present;
  - update `df`/`doc_count` on a *new* tile in `tilemem_ingest`;
  - replace the score in `tilemem_search` (HOT scan + WARM scan) with an idf-weighted
    cosine helper;
  - save `idf.bin` in `tilemem_close`; free `df`.
- **`tests/test_tfidf.c`** + `make tfidf`: a controlled unit test (idf downweights a
  common term so a rare-term query ranks the right tile first) + the real-book last-mile
  proof + benchmarks. Auto-skips the book if absent.

## 5. Data flow

```
ingest (new tile) ─▶ df[bucket]++ for each set bucket; doc_count++
search(query) ─▶ idf[i]=log((N+1)/(df[i]+1))+1 ─▶ weight+normalize query
            ─▶ per tile: weight+normalize its vec by idf ─▶ dot ─▶ top-K
open/close ─▶ load/save store_dir/idf.bin (df + doc_count)
```

## 6. Testing / proofs

- **Unit (controlled)**: ingest tiles where a common word appears in many and a rare word
  in one; a query with both → idf-weighted search ranks the rare-word tile first (raw
  cosine would not). Assert the ranking flips with idf.
- **Real book**: ingest `aivalueplaybook.pdf` (quality-filtered), search *"last mile
  problem in AI projects"*, assert at least one top-5 hit contains `"last mile"` (was 0
  before). Print the top hits (before/after framing).
- **Regression**: `make tiermem_test` green; `make graduate`/`pdftest`/`fontdecode` green.

## 7. Benchmarks (required output)

```
PHASE              TIME(ms)   METRIC
ingest (df build)     ...     N tiles, dim=D
search (idf cosine)   ...     per-query ms over N tiles
```
Plus the top-5 for the last-mile query (showing a "last mile" sentence present).

## 8. Honest scope / limits

- Still **bag-of-words** (idf-weighted) — no synonym matching. The grounded miss is a
  weighting problem, so this fixes it; true synonym queries (no shared words) need the
  deferred co-occurrence/embedding layer.
- `df` is monotonic (not decremented on evict) — a soft approximation; fine for weighting.
- Hashing-trick collisions remain (multiple terms per bucket) — unchanged from today.

## 9. Project notes

- **No git.** Spec written, not committed; verify via filesystem.
- Enhancement to the existing `tile_memory` module (no new module) + `tests/test_tfidf.c`
  + the `tfidf` target. **Zero core/router/contract edits.**
