#ifndef CCE_NGRAM_H
#define CCE_NGRAM_H

/*
 * CPU n-gram table for CNET and other C hosts.
 * =================================================================
 * Chess-TT style open-addressing hash: context → top-k continuations.
 * NOT CERT. NOT GPU. Hashtable / ROUTE stay on the host (same rule as
 * cb_board). Dense maps still go through cce_amdmath.
 *
 * order=3 means trigrams: P(w_i | w_{i-2}, w_{i-1}) with stupid-backoff
 * to bigram then unigram when the context is missing.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_NGRAM_OK     0
#define CCE_NGRAM_ERR   -1
#define CCE_NGRAM_FULL  -3
#define CCE_NGRAM_MISS  -4

#define CCE_NGRAM_MAX_ORDER 8
#define CCE_NGRAM_TOPK      16
#define CCE_NGRAM_VERSION   "0.1.0"

typedef struct cce_ngram cce_ngram;

typedef struct cce_ngram_stats {
    int order;
    size_t slots;
    size_t used;
    uint64_t tokens_ingested;
    uint64_t adds;
    uint64_t lookups;
    uint64_t hits;
    uint64_t backoff;
} cce_ngram_stats;

cce_ngram *cce_ngram_open(int order, size_t slots);
void       cce_ngram_close(cce_ngram *t);
const char *cce_ngram_version(void);

/* Observe (ctx[0..ctx_n) → next). ctx_n=0 is unigram. */
int cce_ngram_add(cce_ngram *t, const uint32_t *ctx, int ctx_n, uint32_t next,
                  uint32_t count);

/* All 1..order grams in toks[0..len). */
int cce_ngram_ingest(cce_ngram *t, const uint32_t *toks, size_t len);

/* Merge src counts into dst (same order). For sharded parallel ingest. */
int cce_ngram_merge(cce_ngram *dst, const cce_ngram *src);

/* Top-k next tokens. Fills *k_out. Uses stupid backoff (λ=0.4). */
int cce_ngram_predict(cce_ngram *t, const uint32_t *ctx, int ctx_n,
                      uint32_t *out_tok, float *out_p, int k, int *k_out);

/* Raw count of (ctx → next), 0 if missing. */
uint32_t cce_ngram_count(const cce_ngram *t, const uint32_t *ctx, int ctx_n,
                         uint32_t next);

int cce_ngram_get_stats(const cce_ngram *t, cce_ngram_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_NGRAM_H */
