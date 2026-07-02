#ifndef CORPUS_TILE_MEMORY_H
#define CORPUS_TILE_MEMORY_H
#include <stddef.h>
#include "synonyms.h"

/* Tiered, fuzzy, decaying tile memory (AICIMO-adapted). A Tile is a passage of
   memory; ingest is fuzzy-deduped (cosine over FNV-1a hashed bag-of-words); a
   capacity-bounded HOT tier spills to a WARM on-disk tier (bounded RAM, unbounded
   disk); the store persists across runs. See docs/superpowers/specs. */

typedef struct {
    char   id[64];
    char  *key;          /* trigger text (owned) */
    char  *value;        /* content/continuation (owned) */
    char   label[32];
    char   source[64];
    int    heat;
    int    count;
    unsigned tid;        /* inverted-index id (ephemeral; not persisted) */
    float *vec;          /* dim floats, L2-normalized (owned) */
} Tile;

typedef struct { char id[64]; char label[32]; char source[64]; const char *value; float score; } TileHit;
typedef struct TileMemory TileMemory;

/* Open/create the persistent store at store_dir (hot.bin + warm.bin). Loads prior state. */
TileMemory *tilemem_open(const char *store_dir, int dim, int hot_cap, double dedup_tau);
void        tilemem_close(TileMemory *m);   /* persists HOT (+ keeps WARM file) */

int    tilemem_ingest(TileMemory *m, const char *key, const char *value,
                      const char *label, const char *source);   /* 1=new, 0=reused */
int    tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap);
/* Reference linear scan (scores every HOT tile + streams all WARM). Identical results to
   tilemem_search; kept as the inverted-index test oracle and for benchmarking. */
int    tilemem_search_linear(TileMemory *m, const char *query, int topK, TileHit *out, int cap);
void   tilemem_decay(TileMemory *m);

size_t tilemem_hot_count(const TileMemory *m);
size_t tilemem_warm_count(const TileMemory *m);
size_t tilemem_total(const TileMemory *m);
size_t tilemem_certifiable(const TileMemory *m, int min_count, double min_share);
/* Evict HOT tiles whose key contains `needle` (graduated knowledge leaves the
   fuzzy memory). Returns the number evicted. */
size_t tilemem_evict_containing(TileMemory *m, const char *needle);

/* ---- Lever 3: opt-in query-time synonym expansion (PPMI co-occurrence) ----
   alpha=0 (default) disables expansion => search is byte-identical to TF-IDF.
   Pass 0/negative for any tuning knob to keep its current default. */
void tilemem_set_expansion(TileMemory *m, double alpha, int k, int min_cooc,
                           int min_df, double df_frac, int max_expand);
/* Toggle support-discounting of PPMI weights (1=on/default, 0=raw PPMI). Rebuilds the map. */
void tilemem_set_discount(TileMemory *m, int on);
/* Rebuild the PPMI synonym map over the full HOT+WARM corpus and persist it. */
void tilemem_build_synonyms(TileMemory *m);
/* Read-only view of the current synonym map (NULL/empty before a build). */
const Synonyms *tilemem_synonyms(const TileMemory *m);

/* ---- Lever 6: opt-in semantic consolidation (merge paraphrase tiles via the PPMI map) ---- */
typedef struct { size_t tiles_before, tiles_after, merges, comparisons; double ms; } ConsolidateReport;
/* Merge paraphrase HOT tiles whose bidirectional synonym-aware soft-coverage >= tau_sem and
   both have >= min_terms distinct terms. Builds the PPMI map if stale; uses the inverted index
   to find candidates. max_df_frac: during candidate GATHERING, skip terms with
   df > max_df_frac*doc_count (<=0 or >=1 disables pruning) — coverage still uses all terms, so
   merge quality is unchanged; this only bounds how many pairs are examined. rep->comparisons
   reports the number of candidate coverage-checks. Returns merges; rep may be NULL. HOT-only.
   max_cand: cap on candidates examined per tile, gathered rarest-term-first (<=0 = unbounded).
   Bounds the pass to O(n*max_cand); coverage/merge logic unchanged. */
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, int max_cand, ConsolidateReport *rep);
#endif
