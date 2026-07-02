# Compounding Retrieval Memory for the Generative Layer — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** Make the generative (LM) layer *compound* — learning a new book **reuses**
existing memory and costs **O(book)** (flat), instead of retraining on the full corpus
(**O(Σ books)**, which grows). Retrieval-augmented generation (kNN-LM / RAG) over a
growing, persistent memory, with a bridge to certified contracts.

---

## 1. Goal & acceptance criteria

The current pipeline (`pdflearn`) retrains `cce_wordlm` on the **whole accumulated
corpus** every run — cost grows with total corpus (the measured ~56 min on one book,
and rising). This makes "more books" *scale up*, the opposite of the goal.

This design freezes the LM (trained once) and puts all new knowledge into a **growing
retrieval memory** keyed on generation context. Acceptance:

1. **Flat learning cost.** Ingesting a new book is **O(book)** (index it — seconds),
   independent of how much is already stored; the retrain baseline is **O(Σ)** and grows.
   The demo prints both as a table and asserts the retrieval ingest stays flat while the
   retrain baseline rises.
2. **Reuse is measured.** A sequence of corpora shows: **identical** re-ingest → **0 new
   keys** (100% reuse); **related** book → partial reuse (`new_keys < contexts`,
   `reuse_rate > 0`); **unrelated** book → ~all-new (honest: no free lunch).
3. **Generation reuses cross-book memory.** Retrieval-augmented decode produces, for a
   held-out prompt, continuations drawn from previously-ingested books.
4. **Persists** across runs (Phase 2), and runs on the real `aivalueplaybook.pdf`.
5. **Contract bridge.** Near-deterministic, high-count contexts are flagged as
   contract-distillation candidates (the path from fuzzy memory → proven contract).

## 2. The principle (concrete)

Freeze the "compute" (the word-LM, trained once); all new knowledge enters a **retrieval
memory**. At generation, combine them (kNN-LM): `p = λ·p_LM + (1−λ)·p_retrieval`. This is
the retrieval instance of the research-convergent pattern *frozen compute + growing,
sparsely-updated memory + routing + distillation* — see §8. The reduce-not-scale property
comes from **reuse**: a new book mostly **hits existing context keys** (free) and writes
only **new** keys for novel content, so cost ∝ novelty and falls as memory saturates.

## 3. Components

1. **`src/corpus/retrieval.c`** (`include/corpus/retrieval.h`) — the compounding memory.
   - `RetrievalStore`: open-addressing hash map `context-key → continuation counts`, where
     the key is a hash of the last `ctx` word-ids and the value is a short list of
     `(next_word_id, count)`. This *is* the kNN-LM datastore / a growable interpolated
     n-gram memory. Realloc-grown, no caps.
   - `IngestStats retrieval_ingest(store, tokens, n, ctx)` → returns
     `{new_keys, reused_keys, new_pairs, total_contexts}` — the **measured learning signal**.
   - `retrieval_lookup(store, ctx_tokens, ctx, out_words, out_counts, cap)` → the empirical
     next-word distribution for a context (counts; caller normalizes).
   - `retrieval_save/load(store, path)` → persist across runs (Phase 2).
   - `retrieval_keys/pairs(store)` → size stats.
2. **Retrieval-augmented decode** (in the demo).
   - **Phase 1 (lean, zero core changes):** *retrieval-primary with frozen-LM backoff* —
     if the current context has memory entries, pick from the retrieval distribution
     (with a recent-repeat penalty); else fall back to `cce_wordlm`'s prediction. The
     memory supplies known continuations; the frozen LM generalizes for unseen contexts.
   - **Phase 2 (refinement):** true interpolation `λ·p_LM + (1−λ)·p_retrieval`, which needs
     a small `cce_wordlm` top-k distribution helper (a localized addition, called out in
     the plan).
3. **`tests/compound_demo.c`** + `make compound` — the proof (Phase 1) and the usable path
   (Phase 2). Self-checking, returns nonzero on failure.
4. **Contract bridge** (Phase 2 stretch) — scan the store for contexts whose continuation
   is near-deterministic (one `next` with count ≥ T and dominant share) and emit them as
   **contract-distillation candidates** (the `endgate` pattern: a decidable context→next →
   certify with `btn_certify`). This is how fuzzy memory graduates into a proven contract.

## 4. Data flow

```
corpus (pdf_corpus.txt / ingested) ─▶ tokenize (hashed vocab) ─▶ (context,next) pairs
        ─▶ retrieval_ingest ─▶ RetrievalStore (growable, persistent)
generate:  context ─▶ p_retrieval(store)  ⊕  p_LM(frozen cce_wordlm) ─▶ next
compounding measured by the IngestStats returned across a sequence of corpora.
```

Reuses the existing PDF→corpus pipeline (`corpus_split`, `corpus_store`) and the hashed
vocab + tokenizer from `pdflearn`. `agent_memory` is the existing persistent book-KB
precedent; Phase 2 may route real-book ingestion through its `agent_ingest_file`, but the
retrieval index is the new generation-oriented complement (context→next, which
`agent_recall` — a keyword/document index — does not provide).

## 5. Phase 1 — the measured compounding proof (the rigorous core)

Train one **frozen** base LM (small, fixed). Then ingest a sequence of corpora and read
`retrieval_ingest` stats. Assertions (sharp + honest):

- **Identical re-ingest → `new_keys == 0`** (100% reuse) in O(book) time. The cleanest
  "reduce, don't scale": the retrain baseline re-pays O(2·book).
- **Related corpus → `0 < new_keys < total_contexts`** (partial reuse; `reuse_rate > 0`),
  and `reuse_rate` *rises* as more is accumulated.
- **Unrelated corpus → `new_keys ≈ total_contexts`** (≈no reuse — stated plainly).
- **Flat ingest cost:** per-corpus ingest time is ~constant in accumulated size; a
  `retrain-baseline` measurement (retrain the LM on the growing union) is O(Σ) and rises.
  The demo prints both as a small table → the compounding curve.
- **Generation reuse:** a held-out prompt continues using stored continuations.

Controlled corpora (embedded, with *known* overlap) make the assertions exact;
`aivalueplaybook.pdf` (split into halves) demonstrates it on real text.

## 6. Phase 2 — usable persistent multi-book generator

- `retrieval_save/load` (or rebuild from `pdf_corpus.txt`); store survives across runs.
- CLI: ingest several PDFs, then generate/answer from the **accumulated cross-book**
  memory; frozen LM + interpolation decode.
- Real-PDF run on `aivalueplaybook.pdf`; second half measurably reuses the first.
- Contract bridge (§3.4) surfaces certifiable context→next rules.

## 7. Testing

- **Unit (`make compound` self-checks / a test file):** store ingest/lookup/dedup
  (re-ingest adds 0 keys), save/load round-trip, lookup returns the right continuations.
- **Proof assertions (§5):** identical/related/unrelated three-point measurement; flat
  ingest cost; retrieval-augmented generation non-empty and drawn from memory.
- **Real-PDF:** ingest `aivalueplaybook.pdf` halves → second half reuse > 0.
- Run existing `make pdftest` / `make test` after (changes are additive).

## 8. Research grounding

The design is the retrieval instance of a pattern multiple 2024–2026 lines converge on:
- **Memory Layers at Scale** (Meta, arXiv 2412.09764) — frozen compute + key-value memory.
- **Continual Learning via Sparse Memory Finetuning** (arXiv 2510.15103) — frozen base,
  sparse memory updates → minimal forgetting (the reduce-not-scale mechanism).
- **Titans: Learning to Memorize at Test Time** (Google, arXiv 2501.00663).
- **Nested Learning / "Hope"** (Google, NeurIPS 2025, arXiv 2512.24695) — multi-frequency
  continuum memory.
- **DreamCoder** lineage (Trove, REGAL, LiLo, AbstractBeam arXiv 2405.17514) — library
  learning = CNET's contract distillation (the §3.4 bridge).
kNN-LM / RAG is the classic retrieval-augmentation paradigm this implements at toy scale.

## 9. Honest limits / out of scope

- Compounding is **system-level** (the retrieval memory), **not** in the dense LM weights —
  the correct, research-backed reading of "a model that compounds." The frozen LM does not
  itself improve from new books; the memory does.
- **Exact** context-match retrieval at toy scale; **fuzzy/embedding** match (true kNN) is a
  later upgrade. Unrelated content gets no reuse savings.
- Parametric sparse-memory (Memory-Layers style "in the weights") is explicitly deferred
  (the user chose retrieval); the interface leaves room for it later.
- No new model architecture; reuse `cce_wordlm` frozen.

## 10. Project notes

- **No git on this repo.** The `.git` was removed; run no git commands. Spec written to
  disk, not committed; verify via the filesystem.
- New code under `src/corpus/retrieval.c` + `tests/compound_demo.c`; reuses
  `corpus_split`/`corpus_store`/`cce_wordlm`. **Zero core/router/contract/CCE edits** in
  Phase 1 (Phase 2 adds at most a small localized `cce_wordlm` distribution helper).
