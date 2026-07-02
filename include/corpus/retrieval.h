#ifndef CORPUS_RETRIEVAL_H
#define CORPUS_RETRIEVAL_H
#include <stddef.h>
#define RETRIEVAL_MAX_CTX 4

typedef struct { size_t new_keys, reused_keys, new_pairs, total_contexts; } IngestStats;
typedef struct RetrievalStore RetrievalStore;

RetrievalStore *retrieval_create(int ctx);      /* ctx context words, 1..RETRIEVAL_MAX_CTX */
void            retrieval_free(RetrievalStore *r);

/* Index (last-ctx-words -> next) over a token stream. Returns measured stats. */
IngestStats retrieval_ingest(RetrievalStore *r, const int *tokens, size_t n);
/* Continuation counts for a context (ctx tokens). Returns #distinct written (0=unseen). */
int retrieval_lookup(const RetrievalStore *r, const int *ctx_tokens,
                     int *out_words, int *out_counts, int cap);
size_t retrieval_keys(const RetrievalStore *r);
size_t retrieval_pairs(const RetrievalStore *r);
int             retrieval_save(const RetrievalStore *r, const char *path);
RetrievalStore *retrieval_load(const char *path);
/* Count near-deterministic contexts (contract-distillation candidates): top
   continuation has count >= min_count and share >= min_share. */
size_t retrieval_certifiable_contexts(const RetrievalStore *r, int min_count, double min_share);
#endif
