#include "../include/cce/cce_oracle_prefix_cache.h"

#include <stdio.h>

static int failures;

static void check(int ok, const char *name) {
    if (ok) printf("  PASS: %s\n", name);
    else { printf("  FAIL: %s\n", name); failures++; }
}

int main(void) {
    cce_oracle_prefix_cache cache;

    cce_oracle_prefix_cache_init(&cache);
    check(!cce_oracle_prefix_cache_hit(&cache, 42),
          "fresh cache never skips prefix replay");

    cce_oracle_prefix_cache_begin_fill(&cache);
    cce_oracle_prefix_cache_commit(&cache, 42);
    check(cce_oracle_prefix_cache_hit(&cache, 42),
          "successful prefix fill caches exactly its token");
    check(!cce_oracle_prefix_cache_hit(&cache, 43),
          "a different unit token cannot reuse the cached prefix");

    cce_oracle_prefix_cache_invalidate(&cache);
    check(!cce_oracle_prefix_cache_hit(&cache, 42),
          "foreign golden forward invalidates the remembered prefix");

    cce_oracle_prefix_cache_begin_fill(&cache);
    check(!cce_oracle_prefix_cache_hit(&cache, 42),
          "failed prefix fill remains invalid until commit");

    if (failures) {
        printf("FLAGSHIP_PREFIX_CACHE_FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("FLAGSHIP_PREFIX_CACHE_PASS (5 checks)\n");
    return 0;
}
