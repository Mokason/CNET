#ifndef CCE_ORACLE_PREFIX_CACHE_H
#define CCE_ORACLE_PREFIX_CACHE_H

/* Small state machine for model-backed oracle prefix reuse.
 *
 * A cache hit is valid only after a successful prefix fill commits its token.
 * Any direct/foreign model forward (golden battery, parity probe, diagnostics)
 * must invalidate the cache before the next oracle request. */
typedef struct {
    int token; /* -1 means no reusable prefix */
} cce_oracle_prefix_cache;

void cce_oracle_prefix_cache_init(cce_oracle_prefix_cache *cache);
int cce_oracle_prefix_cache_hit(const cce_oracle_prefix_cache *cache,
                                int token);
void cce_oracle_prefix_cache_begin_fill(cce_oracle_prefix_cache *cache);
void cce_oracle_prefix_cache_commit(cce_oracle_prefix_cache *cache,
                                    int token);
void cce_oracle_prefix_cache_invalidate(cce_oracle_prefix_cache *cache);

#endif /* CCE_ORACLE_PREFIX_CACHE_H */
