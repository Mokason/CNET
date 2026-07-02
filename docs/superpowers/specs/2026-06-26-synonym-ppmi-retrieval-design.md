# Synonym Layer via Corpus PPMI (Lever 3) — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** Close the **synonym gap** left by TF-IDF retrieval — a query that shares *zero*
words with the answer still misses. Add a corpus-derived, interpretable **PPMI
co-occurrence** map and use it for bounded, reversible **query-time expansion**. No dense
weights, no external data — the synonym signal grows from the same books you ingest.

---

## 1. Goal & acceptance criteria

1. **Corpus-grown synonymy**: build a sparse `term → top-k related terms` map from the
   ingested tiles via Positive PMI over same-tile co-occurrence. Interpretable and
   inspectable (you can print that `story ↔ narrative ↔ tale`).
2. **Closes the gap (the headline proof)**: a query whose terms share **no** words with the
   answer tile — but which co-occur elsewhere with the answer's terms — now retrieves that
   tile. With expansion off, it misses. This is the explicit deferred piece from the TF-IDF
   spec (§8 there).
3. **Reversible = regression gate**: expansion is **opt-in and off by default** (`alpha = 0`),
   matching the project's house style (`plan.strict`, zero-init-lenient). With expansion off,
   search is **byte-identical** to today's TF-IDF — so every existing caller (`tfidf`,
   `tiermem`, `graduate`) is unaffected *regardless of whether a map exists*. `make tfidf`
   (3/3) stays green unchanged, plus an explicit `alpha=0` equality assertion.
4. **Zero core/router/contract edits.** New `src/corpus/synonyms.c` + a thin integration in
   `tile_memory`. All existing suites stay green: `tfidf` 3/3, `tiermem_test` 18/18,
   `graduate` 9/9, `fontdecode` 20/20, `pdftest` 19/19.

## 2. Why PPMI co-occurrence (grounded in the project philosophy)

The remaining miss is *not* a weighting problem (TF-IDF fixed that) — it's that the query
and answer use **different words** for the same idea. Bridging that needs a notion that two
terms mean similar things. Three sources were weighed:

- **Corpus co-occurrence (PPMI)** — chosen. Interpretable, grows as more books are read
  (compounding is *system-level memory*, not dense weights — consistent with the project
  thesis), no external data, no training. Noisier on tiny corpora; mitigated by floors.
- Reuse `cce_wordlm` dense embeddings — richer but couples retrieval to a trained dense
  model and needs more data to be reliable. Rejected for v1 (philosophy + data sparsity).
- External thesaurus — reliable out of the box but not corpus-grown and won't learn domain
  terms. Rejected (external data dependency).

**Same-tile** co-occurrence (two terms relate if they appear in the same passage/tile) is
used rather than a sliding word-window: it reuses the tiles already built, costs nothing
extra in tokenization, and yields *topical* relatedness — exactly what bridges
`story`/`narrative` in retrieval.

## 3. The PPMI math (and the key reuse)

For same-tile co-occurrence, a term's marginal count **is its `df`** — already maintained in
`m->tdf` (the `TermDF` dictionary), and `m->doc_count` is `N` (tile count). So no new
marginal bookkeeping:

- `c_ij` = # tiles containing both *i* and *j* (sparse per-term adjacency, built in §5).
- `PMI(i,j) = log( c_ij · N / (df_i · df_j) )`
- `PPMI(i,j) = max(PMI, 0)`
- Keep **top-k** neighbors per term *i* by PPMI, subject to `c_ij ≥ min_cooc`.

Two filters bound *both* cost and noise, applied with **final** `df` known (see §5 build
timing):

- **Stopword-fraction cutoff**: drop terms with `df > df_frac · N` from co-occurrence — they
  co-occur with everything (useless neighbors, hub-term cost blow-up). (The tokenizer's
  built-in `is_stop` list already removes function words; this catches high-df *content*
  hubs like a book's pervasive domain term.)
- **Hapax cutoff**: drop terms with `df < min_df` — can't generalize from a single sighting.

Final map size is `O(V · k)`.

## 4. Module boundary

New focused, independently testable module:

- **`include/corpus/synonyms.h`, `src/corpus/synonyms.c`** — owns the co-occurrence
  accumulator, PPMI finalize, the frozen top-k store, neighbor lookup, and save/load. It
  does **not** tokenize. Interface:
  ```c
  Synonyms *syn_new(void);
  void  syn_free(Synonyms*);
  void  syn_observe_tile(Synonyms*, const char *const *terms, int n_distinct); /* one tile */
  /* counts → PPMI → top-k, then freeze. df_of(term) yields the global df; N = tile count.
     The exact df-passing mechanism (callback vs per-term df at observe) is settled in the plan. */
  void  syn_finalize(Synonyms*, unsigned (*df_of)(void *ctx, const char *term), void *ctx,
                     size_t N, int k, int min_cooc, double df_frac, int min_df);
  int   syn_neighbors(const Synonyms*, const char *term,
                      const char **out_terms, float *out_ppmi, int k);         /* top-k for one term */
  int   syn_save(const Synonyms*, const char *path);
  int   syn_load(Synonyms*, const char *path);
  ```
  `df`/`N` are passed in at finalize (the synonyms module stays ignorant of how marginals are
  stored; `tile_memory` supplies them from `m->tdf`/`m->doc_count` via a tiny accessor or by
  passing per-term df at observe time — settled in the plan).

Dependency direction is clean: **`tile_memory` → `synonyms`** (never the reverse). Keeping
this out of `tile_memory.c` also stops that file from growing a second large concern.

## 5. Build flow (over the FULL tile store — this is where compounding lives)

A finalize pass, not an incremental ingest hook, because the eligibility filters (§3) need
the **final** `df` of every term (you can't tell a hub term from a rare one mid-ingest):

```
tilemem_build_synonyms(m):
  syn = syn_new()
  for each tile in HOT ∪ WARM:                 # full persisted corpus → reflects every book
      terms = tokenize_terms(tile.key)         # same tokenizer as TF-IDF → identical vocab
      distinct(terms)
      syn_observe_tile(syn, distinct, n)        # increment c_ij for eligible pairs
  syn_finalize(syn, m->tdf, m->doc_count, k, min_cooc, df_frac, min_df)  # PPMI + top-k, freeze
  syn_save(syn, synonyms_path(m))
```

- Scanning HOT + WARM mirrors how `tilemem_search` already streams WARM each query, so the
  pass is in-idiom. Compounding holds because the build sees **all** tiles, not just the
  session's new ones — read a new book and its terms enter the same shared map.
- **Laziness / dirtiness**: ingest sets a `syn_dirty` flag. The map is (re)built lazily on
  the first search **only when expansion is enabled** (`alpha > 0`) and the map is
  absent/stale — so an `alpha=0` caller (every existing test) never pays the build cost and
  its timing/behavior is untouched. A build may also be forced explicitly via
  `tilemem_build_synonyms(m)`. On open, `syn_load` restores the frozen map with no rescan when
  nothing changed.
- **Cost honesty**: a rebuild is `O(total tiles × avg-distinct-terms²)` for the pair
  accumulation. Bounded by the §3 filters (hub terms excluded) and a per-term adjacency cap;
  the plan sets a measured ceiling. Reported as a benchmark, not hidden.

## 6. Search / expansion (the gap-closer)

Hook is right after the distinct query terms `qt[qidx[]]` and their `qidf[]` are built in
`tilemem_search` ([tile_memory.c:222](../../src/corpus/tile_memory.c)):

```
for each distinct query term qt[qidx[q]]:
    for each neighbor (nbr, ppmi) in syn_neighbors(qt[qidx[q]], k):
        if nbr already a (hard or soft) query term: skip      # no double count
        soft_weight = idf(nbr) · min(1, ppmi / PMI_SCALE) · alpha
        append nbr to the query-term set as a SOFT term with that weight
cap total soft terms per search (max_expand) to bound latency
```

The **existing** TF-IDF overlap scorer then runs unchanged over the combined hard+soft term
set: a tile's score sums `qidf` for matched hard terms plus `soft_weight` for matched soft
terms, length-normalized by `sqrt(nt+1)` exactly as today. A no-shared-word query now scores
on tiles containing its neighbors' words.

- `idf(nbr)` reuses the same `log((N+1)/(df+1))+1` already computed in search.
- `alpha` is the master switch: `alpha = 0` ⇒ no soft terms ⇒ identical scores/ordering to
  current behavior.

## 7. Persistence

- Finalized top-k map saved as a **text** sidecar `store_dir/synonyms.bin` (mirroring the
  human-readable `idf.bin` format: one line per term → its `k` neighbors with PPMI weights).
  Loaded in `tilemem_load`/written in `tilemem_save` alongside `idf.bin`.
- Frozen on load (read-only) — a rebuild replaces it wholesale.

## 8. Components (change list)

- **New** `include/corpus/synonyms.h` + `src/corpus/synonyms.c` (PPMI map + tests' surface).
- **Modify** `src/corpus/tile_memory.c`:
  - hold a `Synonyms *syn` + `int syn_dirty`, `double syn_alpha` (+ knobs) in `TileMemory`;
  - `tilemem_build_synonyms(m)` (scan HOT+WARM, build, save);
  - set `syn_dirty` in `tilemem_ingest`; lazy build in `tilemem_search`; save/free in close;
  - load `synonyms.bin` in `tilemem_load`; expansion block in `tilemem_search`;
  - a setter `tilemem_set_expansion(m, alpha, k, ...)` (defaults preserve `alpha=0`-like
    safety until a corpus is built — see §10).
  - **Modify** `include/corpus/tile_memory.h`: declare the setter + `tilemem_build_synonyms`.
- **New** `tests/test_synonyms.c` + `make synonyms` target.
- **Makefile**: `synonyms.c` joins the corpus objects; new `synonyms` target.

## 9. Testing / proofs (TDD, red→green)

- **PPMI math (exact)**: a hand-built 3–4 tile corpus with known `c_ij`, `df`, `N`; assert
  the computed PPMI equals the hand value; assert top-k ordering.
- **Distributional twins**: synthetic tiles where two terms are perfect context-twins ⇒
  assert they are mutual top-1 neighbors; assert a high-`df` hub term is **excluded**
  (stopword-fraction) and a `df=1` term is **excluded** (hapax).
- **The gap-closer (headline)**: corpus where query Q = {`a b`} shares zero words with answer
  tile A = {`x y`}, but Q's terms co-occur (in *other* tiles) with A's terms ⇒
  `tilemem_search(Q)` with expansion **on** returns A in top-K; with `alpha=0` it does **not**.
  Direct proof the deferred gap is closed.
- **Reversibility**: assert that with `alpha=0` the full search result (ids + scores +
  ordering) is identical to a pre-expansion baseline; `make tfidf` stays 3/3.
- **Real book (qualitative)**: build the map over `aivalueplaybook.pdf` tiles; print the
  learned neighbors for a few content terms and run one synonym-style query, reporting actual
  (possibly noisy) neighbors — no clean-synonymy claim beyond what the data supports.
- **Regression**: `tiermem_test`, `graduate`, `fontdecode`, `pdftest` all green.

## 10. Knobs (safe defaults; `alpha` is the master switch)

| knob | default | role |
|---|---|---|
| `alpha` (expansion strength) | `0` (opt-in) | master switch; `0` ⇒ today's behavior exactly; demos/tests set `0.35` |
| `k` (neighbors/term) | `5` | breadth of expansion + map size |
| `min_cooc` | `2` | a pair must be seen ≥ this to count |
| `min_df` | `2` | drop hapax terms from co-occurrence |
| `df_frac` (stopword cutoff) | `0.5` | drop terms with `df > df_frac·N` |
| `max_expand` (soft terms/search) | `64` | latency bound |
| `PMI_SCALE` | tuned in plan | normalizes ppmi into the soft-weight `min(1, ·)` |

Default posture: expansion is **off** (`alpha=0`) — every existing caller is unaffected with
no reliance on "the map happens to be empty." A caller opts in with
`tilemem_set_expansion(m, 0.35, ...)`; the synonyms test and any new demo do exactly that.
This mirrors the established opt-in pattern (`plan.strict`, zero-init-lenient).

## 11. Honest scope / limits

- **PPMI on a single small book is noisy.** Neighbor quality scales with corpus size; the
  `min_cooc`/`df` floors and conservative `alpha` keep bad neighbors from hurting precision,
  but the real-book test reports *actual* learned neighbors rather than claiming clean
  synonymy.
- **Same-tile (topical) relatedness**, not strict synonymy — it will relate `narrative` to
  `story` but also to topically co-occurring non-synonyms; the down-weighted soft scoring is
  designed to tolerate this (a wrong neighbor rarely outscores a real hard-term hit).
- Rebuild cost is `O(corpus)`; mitigated by laziness + filters, reported as a benchmark.
- PPMI re-ranking of lexical hits and window-based co-occurrence are **deferred** refinements
  (re-ranking can't rescue a zero-overlap query, so it isn't the primary mechanism).

## 12. Project notes

- **No git on CNET.** Spec written, not committed; verify via filesystem.
- New `src/corpus/synonyms.c` module + thin `tile_memory` integration + `tests/test_synonyms.c`
  + `synonyms` target. **Zero core/router/contract edits.**
