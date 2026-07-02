#ifndef CCE_DATASET_H
#define CCE_DATASET_H

/* cce_dataset - simple iterator for training data pipeline.
 * Designed to be called from .NET (P/Invoke) for high-level Training API.
 * Supports flat arrays (for now) and can be extended to corpus/tile_memory.
 *
 * .NET usage example (via P/Invoke):
 *   IntPtr ds;
 *   cce_dataset_from_arrays(..., inputs, targets, n, in_dim, out_dim, &ds);
 *   while (cce_dataset_next_batch(ds, &batch) == 0) { ... train on batch ... }
 *   cce_dataset_destroy(ds);
 */

#include "cce_defs.h"
#include "cce_tensor.h"

#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_API __declspec(dllexport)
#else
#define CCE_API __attribute__((visibility("default")))
#endif
#else
#define CCE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_dataset cce_dataset;

/* Batch view (zero-copy where possible) */
typedef struct {
    const float* inputs;   /* batch_size * in_dim */
    const float* targets;  /* batch_size * out_dim */
    size_t batch_size;
    size_t in_dim;
    size_t out_dim;
    size_t index;          /* current position in dataset */
} cce_batch;

/* Create dataset from raw flat arrays (most common for quick training). */
CCE_API cce_result cce_dataset_from_arrays(cce_dataset** ds,
                                           const float* inputs,
                                           const float* targets,
                                           size_t n_samples,
                                           size_t in_dim,
                                           size_t out_dim,
                                           size_t batch_size);

/* Wrap external (non-owned) arrays. No copy. Caller must keep buffers alive for lifetime of ds.
   Enables zero-copy from perceptual renders, tile vecs, corpus extracted features etc. */
CCE_API cce_result cce_dataset_wrap_arrays(cce_dataset** ds,
                                           const float* inputs,
                                           const float* targets,
                                           size_t n_samples,
                                           size_t in_dim,
                                           size_t out_dim,
                                           size_t batch_size);

/* Reset iterator to start (for next epoch). */
CCE_API cce_result cce_dataset_reset(cce_dataset* ds);

/* Get next batch. Returns CCE_OK or CCE_ERR_NOT_FOUND (end of epoch). */
CCE_API cce_result cce_dataset_next_batch(cce_dataset* ds, cce_batch* batch);

/* Destroy dataset. */
CCE_API void cce_dataset_destroy(cce_dataset* ds);

/* Optional: simple shuffle (Fisher-Yates) for better training. */
CCE_API cce_result cce_dataset_shuffle(cce_dataset* ds);

/* Optional but powerful: wrap tile memory / corpus vectors so perceptual (rendered
   glyphs/grids/7seg from cce_perceptual) + symbolic (tile vecs, retrieval features)
   become first-class sources for cce_model_train without extra copies.
   For tile: pass pre-flattened or use wrap_arrays after extracting vecs from TileMemory.
   Impl may evolve to direct iterator over hot tiles. Returns OK if wired. */
CCE_API cce_result cce_dataset_from_tile_memory(cce_dataset** ds,
                                                void* tile_memory,   /* TileMemory* opaque to avoid dep */
                                                size_t batch_size);

#ifdef __cplusplus
}
#endif

#endif /* CCE_DATASET_H */
