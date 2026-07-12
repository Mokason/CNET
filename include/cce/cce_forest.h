#ifndef CCE_FOREST_H
#define CCE_FOREST_H

#include "cce_cascade.h"
#include "cce_archive.h"
#include "cce_defs.h"
#include "cce_learn.h"  /* for cce_diff_mode_t */

#ifndef CCE_API
#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_API __declspec(dllexport)
#else
#define CCE_API __attribute__((visibility("default")))
#endif
#else
#define CCE_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Tier for branches */
typedef enum {
    CCE_TIER_HOT   = 0,   /* fully in RAM, trainable */
    CCE_TIER_WARM  = 1,   /* memory-mapped via archive, read-only */
    CCE_TIER_COLD  = 2    /* on disk, not mapped */
} cce_tier_t;

/* Specialist intent used by higher-level routers. Default GENERAL preserves
   existing routing behavior for old archives and zero-initialized branches. */
typedef enum {
    CCE_SPECIALIST_GENERAL = 0,
    CCE_SPECIALIST_NARRATIVE = 1
} cce_specialist_type_t;

/* A single branch entry */
typedef struct {
    cce_cascade* cascade;   /* owned when HOT; zero-copy view when is_view */
    cce_specialist_type_t specialist_type;
    size_t       archive_offset;
    size_t       archive_size;
    cce_tier_t   tier;
    int          is_view;       /* 1 = cascade's weights are views into the mmap (read-only) */
    int          persisted;     /* 1 = saved to the archive (offset valid, incl. offset 0) */
    float        centroid[32];  /* simple centroid for recall (dim <=32 for v1) */
    int          centroid_dim;
    char         name[64];

    /* Data merging & structure connections:
       Allows hierarchical sub-branches, refinements, compositions etc.
       inside a single .cce archive (avoids 10k separate files).
       Connections use names for portability across merges. */
    int          num_connections;
    char         conn_names[8][64];
    int          conn_types[8];  /* 0=sub-branch, 1=refines, 2=composes, 3=specializes */

    /* Per-branch diff overrides (for mixed modes in one forest, e.g. EXACT only on amb sub-branches) */
    cce_diff_mode_t diff_mode; /* -1 inherit */
    int          exact_tail_length; /* -1 inherit from forest default */

    /* Residency (tiered runtime): only branches a provider can restore are
       ever evicted; last_use drives LRU. Zero-init = never evicted. */
    int          evictable;
    int          last_use;
} cce_branch;

/* Forest = collection of branches + archive + recall
   (tagged so light headers can forward-declare `struct cce_forest`) */
typedef struct cce_forest {
    cce_archive* archive;
    cce_branch*  branches;
    int          num_branches;
    int          max_branches;

    /* Simple in-RAM index for recall (can be replaced by SSMax later) */
    float*       centroids;   /* [max_branches * centroid_dim] */
    int          centroid_dim;

    int          sealed;      /* 1 = no more appends -> mmap stable -> views safe */

    cce_diff_mode_t diff_mode; /* default differentiation mode for this forest (LOCAL by default).
                                  Used by ABI adapt and can be per-branch via sub-forests. */
    int          default_exact_tail_length; /* default 2 for HYBRID mode */

    /* Residency management (all zero = disabled, behavior unchanged).
       With a provider registered, cce_forest_get_resident rehydrates evicted
       evictable branches on demand and keeps the number of resident evictable
       cascades within hot_cap (LRU by last_use). */
    int hot_cap;
    int use_tick;
    int resident_high_water;
    cce_cascade* (*residency_provider)(void* ctx, const char* branch_name);
    void* residency_ctx;
} cce_forest;

/* Open/create forest backed by archive file */
CCE_API cce_result cce_forest_open(cce_forest** forest, const char* archive_path, int max_branches);

/* Close and release all resources */
CCE_API void cce_forest_close(cce_forest* forest);

/* ---- residency (tiered runtime) ----
 * Lookup-with-rehydration: identical to a plain name lookup when no provider
 * is registered. With a provider: an evicted branch is restored through the
 * provider (the forest ADOPTS the returned heap cascade), and the LRU
 * evictable cascade is dropped whenever more than hot_cap are resident.
 * hot_cap is clamped to >= 8 (a transformer layer's working set) so cascades
 * in use by the current layer are never evicted mid-computation. */
CCE_API void cce_forest_set_residency(cce_forest* f, int hot_cap,
                                      cce_cascade* (*provider)(void*, const char*),
                                      void* ctx);
CCE_API cce_cascade* cce_forest_get_resident(cce_forest* f, const char* branch_name);
CCE_API cce_result cce_forest_evict_branch(cce_forest* f, int idx); /* evictable HOT non-view only */
CCE_API int cce_forest_resident_count(const cce_forest* f);          /* resident evictable branches */
CCE_API int cce_forest_resident_high_water(const cce_forest* f);

/* Add a trained cascade as a new branch (will be HOT, can later be contracted).
   On success, moves block ownership into the forest and clears the source cascade. */
cce_result cce_forest_add_branch(cce_forest* forest, cce_cascade* cas, const char* name);

/* Add a host-built cascade as a branch and move its block ownership into the forest. */
CCE_API cce_result cce_forest_add_cascade_branch(cce_forest* forest,
                                                 cce_cascade* cascade,
                                                 const char* name,
                                                 int* out_branch_idx);

/* Build and add a simple named linear specialist branch:
   input -> hidden sigmoid linear -> output linear head (raw logits). */
CCE_API cce_result cce_forest_add_linear_branch(cce_forest* forest,
                                                const char* name,
                                                int input_dim,
                                                int hidden_dim,
                                                int output_dim,
                                                float init_scale,
                                                int* out_branch_idx);

/* Build and add a patch specialist branch:
   flattened patch -> patch contract -> hidden sigmoid linear -> output linear head. */
CCE_API cce_result cce_forest_add_patch_branch(cce_forest* forest,
                                               const char* name,
                                               int patch_size,
                                               int stride,
                                               int channels,
                                               int hidden_dim,
                                               int output_dim,
                                               float init_scale,
                                               int* out_branch_idx);

/* Recall: find best branch by simple centroid distance (placeholder for SSMax) */
cce_result cce_forest_recall(cce_forest* forest, const float* input, int dim, int* branch_idx);

/* Load a branch into HOT tier (mmap -> RAM copy if needed) */
cce_result cce_forest_promote_to_hot(cce_forest* forest, int branch_idx);

/* Contract / offload a branch to archive (freeze + store view) */
cce_result cce_forest_contract_branch(cce_forest* forest, int branch_idx);

/* Forward through recalled branch (loads if necessary) */
cce_result cce_forest_infer(cce_forest* forest, const float* input, int dim,
                            int* out_class, float* confidence);

/* Seal the forest: forbid further cce_forest_add_branch (an append remaps the
   archive and would invalidate live views). After sealing, branches materialize
   as ZERO-COPY views into the mmap on demand. Returns 0, or -1 on bad arg. */
CCE_API cce_result cce_forest_seal(cce_forest* forest);

/* Forward `input` through branch `branch_idx`, materializing it on demand:
   a zero-copy VIEW if the forest is sealed (read-only, reuses the mmap pointer,
   no reload), otherwise an owned copy. Output is written to `output` (overwritten;
   free it with cce_tensor_free). Returns 0, or -1 on bad arg / load failure. */
CCE_API cce_result cce_forest_forward(cce_forest* forest, int branch_idx,
                              const cce_tensor* input, cce_tensor* output);

/* === Data merging and structure connections ===
   These enable composing leaves/branches/sub-branches into larger connected
   structures inside ONE archive file (the reason the model uses this hierarchy
   instead of 10k loose files). */

/* Connect two branches with a typed relation. Names or indices.
   Types: 0=sub, 1=refines, 2=composes, 3=specializes */
CCE_API cce_result cce_forest_connect(cce_forest* forest, int from_idx, int to_idx, int conn_type);

/* Merge src into dst. Branches from src are added (with optional prefix for names).
   Connections are copied/adjusted. Returns number of branches merged or <0 on error.
   This is the key for data merging without file explosion. */
CCE_API cce_result cce_forest_merge(cce_forest* dst, cce_forest* src, const char* name_prefix);

/* Get connections for a branch (names + types). Fills up to max_conn. */
CCE_API cce_result cce_forest_get_connections(cce_forest* forest, int branch_idx,
                                      char names[][64], int types[], int* num, int max_conn);

/* Get block metadata for a branch (block types + input/output dims). Fills up to max_blocks. */
CCE_API cce_result cce_forest_get_branch_blocks(cce_forest* forest, int branch_idx,
                                                int types[], int input_dims[], int output_dims[],
                                                int* num, int max_blocks);

CCE_API cce_result cce_forest_set_branch_specialist_type(cce_forest* forest,
                                                          int branch_idx,
                                                          cce_specialist_type_t specialist_type);
CCE_API cce_specialist_type_t cce_forest_get_branch_specialist_type(const cce_forest* forest,
                                                                     int branch_idx);

/* Set default diff mode for the forest (propagates to new branches and ABI adapt).
   Existing branches keep their training behavior unless re-adapted with new learner. */
CCE_API cce_result cce_forest_set_diff_mode(cce_forest* forest, cce_diff_mode_t mode);

/* Set default exact tail length for HYBRID (used when branch/cascade has -1) */
void cce_forest_set_default_exact_tail_length(cce_forest* forest, int length);

/* Per-branch overrides (for mixed modes in one forest) */
CCE_API cce_result cce_forest_set_branch_diff_mode(cce_forest* forest, int branch_idx, cce_diff_mode_t mode);
CCE_API cce_result cce_forest_set_branch_exact_tail_length(cce_forest* forest, int branch_idx, int length);

/* Number of branches currently in the forest. */
CCE_API int cce_forest_branch_count(const cce_forest* forest);

/* Copy branch `idx`'s name into buf (NUL-terminated, truncated to len). */
CCE_API cce_result cce_forest_branch_name(const cce_forest* forest, int idx, char* buf, int len);

/* Import weights for a previously added linear (or linear-head) branch.
   The branch must have been created with matching in/out dims.
   weights is row-major [out_dim x in_dim] or appropriate, bias [out_dim].
   This enables "pre-train tiny head outside (PyTorch/NumPy) and drop in". */
CCE_API cce_result cce_forest_set_branch_linear_weights(cce_forest* forest, int branch_idx,
                                                         const float* weights, int w_in_dim, int w_out_dim,
                                                         const float* bias, int b_dim);

#ifdef __cplusplus
}
#endif

#endif /* CCE_FOREST_H */
