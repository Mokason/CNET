#ifndef CORPUS_SYNONYMS_H
#define CORPUS_SYNONYMS_H
#include <stddef.h>

/* Corpus-derived synonym/relatedness map via Positive PMI over same-tile
   co-occurrence. Interpretable, grows with the corpus, no dense weights, no
   external data. See docs/superpowers/specs/2026-06-26-synonym-ppmi-retrieval-design.md */

typedef struct Synonyms Synonyms;

Synonyms *syn_new(void);
void      syn_free(Synonyms *s);

/* Accumulate same-tile co-occurrence for one tile's DISTINCT terms. */
void      syn_observe_tile(Synonyms *s, const char *const *terms, int n_distinct);

/* Raw counts -> PPMI -> keep top-k neighbors per term, then freeze.
   df_of(ctx,term) = # tiles containing term (global doc frequency); N = tile count.
   Eligible terms: pair count >= min_cooc AND df in [min_df, df_frac*N].
   discount: 0 = raw PPMI; 1 = multiply each edge by support (c/(c+1))*(min(df_i,df_j)/(min+1))
   to suppress spurious rare-term neighbors (Pantel-Lin discounting). */
void      syn_finalize(Synonyms *s,
                       unsigned (*df_of)(void *ctx, const char *term), void *ctx,
                       size_t N, int k, int min_cooc, double df_frac, int min_df, int discount);

/* Top neighbors of `term` (post-finalize). Writes up to k (term,ppmi) into out_*,
   returns count. out_terms point into the map (valid until syn_free/rebuild). */
int       syn_neighbors(const Synonyms *s, const char *term,
                        const char **out_terms, float *out_ppmi, int k);

int       syn_save(const Synonyms *s, const char *path, size_t stamp); /* text; 0 ok */
int       syn_load(Synonyms *s, const char *path, size_t *stamp_out);  /* 0 ok; *stamp_out=saved stamp */

size_t    syn_term_count(const Synonyms *s);      /* vocab size */
size_t    syn_neighbor_edges(const Synonyms *s);  /* total top-k list entries */

#endif
