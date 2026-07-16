#include "../../include/cce/cce_oracle_prefix_cache.h"

void cce_oracle_prefix_cache_init(cce_oracle_prefix_cache *cache) {
    if (cache) cache->token = -1;
}

int cce_oracle_prefix_cache_hit(const cce_oracle_prefix_cache *cache,
                                int token) {
    return cache && cache->token >= 0 && cache->token == token;
}

void cce_oracle_prefix_cache_begin_fill(cce_oracle_prefix_cache *cache) {
    cce_oracle_prefix_cache_invalidate(cache);
}

void cce_oracle_prefix_cache_commit(cce_oracle_prefix_cache *cache,
                                    int token) {
    if (cache) cache->token = token;
}

void cce_oracle_prefix_cache_invalidate(cce_oracle_prefix_cache *cache) {
    if (cache) cache->token = -1;
}
