#ifndef CCE_TIER_RUNTIME_H
#define CCE_TIER_RUNTIME_H

/* Tiered runtime: run a store-backed transformer in bounded RAM.
 *
 * The 3-tier memory pattern (tile_memory: HOT cap + disk spill) applied to
 * weights. After attach, the model's specialists become evictable: the
 * forward pass rehydrates each layer's cascades from the weight store on
 * demand (via cce_forest_get_resident) and the LRU specialist is evicted
 * whenever more than hot_cap are resident. The transformer forward touches
 * layers sequentially, so one forward = one streaming pass with no thrash;
 * repeated forwards re-stream (expected cost of bounded RAM).
 *
 * Gates: logits with a cap are BIT-IDENTICAL to the all-resident model, and
 * resident count never exceeds the cap (high-water tracked in the forest).
 *
 * Only branches covered by the manifest are made evictable: nothing the
 * store cannot restore is ever dropped. attach refuses a manifest that does
 * not cover every forest branch.
 */

#include "cce_defs.h"
#include "cce_gguf.h"
#include "cce_weight_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_tier_runtime cce_tier_runtime;

/* Attach: register the store as the forest's residency provider.
 * hot_cap clamps to >= 8 (one transformer layer's working set). */
cce_result cce_tier_attach(cce_tier_runtime** out, cce_gguf_qwen2* m,
                           cce_weight_store* s, const char* manifest_path,
                           int hot_cap);

/* Drop every evictable resident cascade (start cold). Also flushes any
 * staged readahead payloads (a cold start is COLD). */
cce_result cce_tier_evict_all(cce_tier_runtime* rt);

int cce_tier_rehydrations(const cce_tier_runtime* rt); /* store fetches so far */
int cce_tier_resident(const cce_tier_runtime* rt);
int cce_tier_high_water(const cce_tier_runtime* rt);

/* ---- A3: async readahead + learned hot-pinning (throughput) ----
 *
 * The runtime RECORDS the specialist fetch order of the first cold pass (the
 * access sequence of a dense forward is deterministic). Once a full pass has
 * been observed (seq_learned), readahead prefetches the next `depth`
 * specialists in that order on a background worker (default 1 thread —
 * fetches are memory-bandwidth-heavy and a bigger pool measured slower on
 * real payloads; CNET_TIER_RA_WORKERS=n overrides, max 4) while the forward
 * computes, wrapping around the sequence end (the tail of pass N prefetches
 * the head of pass N+1 — the decode-loop regime). The forward thread adopts
 * staged payloads instead of blocking on disk; the math is unchanged, so
 * logits stay bit-identical. RAM honesty: staged-but-unadopted payloads live
 * outside the forest cap — effective peak is hot_cap + depth (+ pinned).
 * depth 0 disables. On builds without threads this returns
 * CCE_ERR_UNSUPPORTED and streaming stays synchronous (correct, no overlap). */
cce_result cce_tier_set_readahead(cce_tier_runtime* rt, int depth);

/* Pin the n most-fetched specialists resident (learned frequency pinning):
 * they are rehydrated now, leave the LRU pool, and are never evicted until
 * unpin/detach. Effective RAM = hot_cap + pinned. Streaming passes then
 * fetch only the unpinned specialists (rehydrations/pass drops by n). */
cce_result cce_tier_pin_hot(cce_tier_runtime* rt, int n);
cce_result cce_tier_unpin_all(cce_tier_runtime* rt);
int cce_tier_pinned(const cce_tier_runtime* rt);

int cce_tier_seq_learned(const cce_tier_runtime* rt);    /* 1 once a full pass was recorded */
int cce_tier_prefetch_hits(const cce_tier_runtime* rt);  /* adopted with no waiting */
int cce_tier_prefetch_waits(const cce_tier_runtime* rt); /* adopted after waiting on in-flight */
int cce_tier_sync_fetches(const cce_tier_runtime* rt);   /* fell back to a blocking store get */
int cce_tier_staged_high_water(const cce_tier_runtime* rt); /* max staged payloads (RAM bound) */
double cce_tier_stall_seconds(const cce_tier_runtime* rt);  /* forward-thread time blocked on fetches */

/* Detach: rehydrate everything, clear the provider, free the runtime.
 * The model is left fully resident and independent of the store. */
cce_result cce_tier_detach(cce_tier_runtime* rt);

#ifdef __cplusplus
}
#endif

#endif /* CCE_TIER_RUNTIME_H */
