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

/* Drop every evictable resident cascade (start cold). */
cce_result cce_tier_evict_all(cce_tier_runtime* rt);

int cce_tier_rehydrations(const cce_tier_runtime* rt); /* store fetches so far */
int cce_tier_resident(const cce_tier_runtime* rt);
int cce_tier_high_water(const cce_tier_runtime* rt);

/* Detach: rehydrate everything, clear the provider, free the runtime.
 * The model is left fully resident and independent of the store. */
cce_result cce_tier_detach(cce_tier_runtime* rt);

#ifdef __cplusplus
}
#endif

#endif /* CCE_TIER_RUNTIME_H */
