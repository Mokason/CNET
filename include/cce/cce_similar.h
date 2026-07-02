#ifndef CCE_SIMILAR_H
#define CCE_SIMILAR_H

/* SIMILAR_TO + evidence-gated merge: extend "smaller" beyond byte-exact.
 *
 * Exact sharing (the weight store) needs identical bytes. Fine-tunes often
 * carry layers that differ by epsilon while behaving identically for every
 * practical purpose. This module finds those pairs and merges them — but
 * only behind evidence, never on a hash or a hunch:
 *
 *  1. candidates: cheap structural filter over two specialist graphs
 *     (dims match, digests differ, coarse signature distance <= tau) —
 *     the SIMILAR_TO edge, codebase-memory-mcp style.
 *  2. verify: an adversarial probe battery (K seeded inputs through BOTH
 *     cascades pulled from the store); the pair is `equivalent` only if the
 *     max relative output deviation stays within epsilon across the battery.
 *     The measured deviation is recorded on the pair — evidence, not vibes.
 *  3. merge: rewrite a model's MANIFEST so the near-duplicate row references
 *     the canonical digest. cce_similar_merge REFUSES pairs that were not
 *     verified equivalent. The payload bytes are untouched (no weight
 *     surgery); un-merging is editing a text file back.
 *
 * An epsilon-merge changes the merged model's outputs by (at most) about the
 * measured deviation — that is the contract the caller accepts by choosing
 * epsilon. The test gate measures end-to-end logit drift and, for a model
 * whose ONLY difference was the merged matrix, checks the merged restore is
 * bit-identical to the canonical model.
 */

#include "cce_defs.h"
#include "cce_specgraph.h"
#include "cce_weight_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     name_a[96], name_b[96];
    uint64_t digest_a, digest_b;
    float    sig_dist;     /* coarse signature distance (candidate score) */
    float    max_rel_dev;  /* measured over the verify battery */
    int      probes;       /* battery size actually run */
    int      equivalent;   /* 1 = within epsilon across the whole battery */
} cce_similar_pair;

/* Structural candidate scan between two graphs (SIMILAR_TO edges).
 * Returns the number of pairs written (up to cap). */
int cce_similar_candidates(const cce_spec_graph* ga, const cce_spec_graph* gb,
                           float tau, cce_similar_pair* out, int cap);

/* Adversarial verification: K fresh seeded probes through both cascades
 * (fetched from the store by digest). Fills max_rel_dev/probes/equivalent. */
cce_result cce_similar_verify(cce_weight_store* s, cce_similar_pair* p,
                              int probes, float epsilon);

/* Evidence-gated merge: rewrite manifest rows referencing digest_b to
 * digest_a. REFUSES unless p->equivalent (verified) and digest_a exists in
 * the store. Returns CCE_OK and the rewritten row count via out. */
cce_result cce_similar_merge(cce_weight_store* s, const cce_similar_pair* p,
                             const char* manifest_in, const char* manifest_out,
                             int* rows_rewritten);

#ifdef __cplusplus
}
#endif

#endif /* CCE_SIMILAR_H */
