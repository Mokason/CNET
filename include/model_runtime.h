#ifndef CNET_MODEL_RUNTIME_H
#define CNET_MODEL_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#include "cnet_export.h"

/* ABI version bump whenever struct layouts or enum values change.
 * Consumers must validate abi_version before use. */
#define CNET_MODEL_RUNTIME_ABI_VERSION 1u

#define CNET_MODEL_ID_MAX           128
#define CNET_MODEL_PATH_MAX         512
#define CNET_MODEL_BACKEND_NAME_MAX 64
#define CNET_MODEL_MAX_RESOURCES    64

/* Model class taxonomy — used by the catalog to route models to backends
 * and to describe capability boundaries in the resident manager. */
typedef enum {
    CNET_MODEL_CLASS_OTHER              = 0,
    CNET_MODEL_CLASS_DENSE_TRANSFORMER  = 1,  /* llama/qwen/gpt2-family dense FFN */
    CNET_MODEL_CLASS_MOE                = 2,  /* routed MoE (DeepSeek V4, Mixtral, ...) */
    CNET_MODEL_CLASS_SSM                = 3,  /* Mamba-family state-space models */
    CNET_MODEL_CLASS_EMBEDDING         = 4,  /* bert-style encoders, non-generative */
} CnetModelClass;

/* Capability flags — what the model/backend can do.
 * Used by the lane pool to decide whether a model can serve a request. */
typedef enum {
    CNET_MODEL_CAP_TEXT_GENERATION = (1u << 0),
    CNET_MODEL_CAP_EMBEDDING       = (1u << 1),
    CNET_MODEL_CAP_TOKENIZE        = (1u << 2),
    CNET_MODEL_CAP_SAMPLING        = (1u << 3),
    CNET_MODEL_CAP_KV_CHECKPOINT   = (1u << 4),
    CNET_MODEL_CAP_MULTIMODAL      = (1u << 5),
} CnetModelCapabilities;

/* Physical resource identifiers.  Used in resource masks (bit i = resource i).
 * The exact GPU enumeration is intentionally small; additional resources can be
 * added by extending the enum and raising CNET_MODEL_MAX_RESOURCES. */
typedef enum {
    CNET_MODEL_RESOURCE_CPU  = (1u << 0),
    CNET_MODEL_RESOURCE_GPU0  = (1u << 1),
    CNET_MODEL_RESOURCE_GPU1  = (1u << 2),
    CNET_MODEL_RESOURCE_GPU2  = (1u << 3),
    CNET_MODEL_RESOURCE_GPU3  = (1u << 4),
} CnetModelResource;

/* Resident model lifecycle states. */
typedef enum {
    CNET_MODEL_STATE_COLD     = 0,  /* not loaded, never loaded or evicted */
    CNET_MODEL_STATE_LOADING  = 1,  /* a load is in flight */
    CNET_MODEL_STATE_RESIDENT = 2,  /* loaded, handle live */
    CNET_MODEL_STATE_FAILED   = 3,  /* last load failed; needs reset_failure */
} CnetModelState;

/* Manager return codes.  Non-negative = success. */
typedef enum {
    CNET_MODEL_OK           = 0,
    CNET_MODEL_BUSY         = 1,   /* would require evicting a pinned/leased model */
    CNET_MODEL_LOAD_FAILED  = 2,   /* backend returned an error */
    CNET_MODEL_OVER_BUDGET  = 3,   /* backend usage exceeded the resource budget */
    CNET_MODEL_NOT_FOUND    = -1,
    CNET_MODEL_INVALID      = -2,
    CNET_MODEL_UNSUPPORTED  = -3,
} CnetModelError;

/* A model catalog entry — produced by cce_model_descriptor_probe() and
 * stored in the resident manager's registry. */
typedef struct {
    uint32_t              abi_version;
    uint32_t              struct_size;

    /* Identity */
    char                  model_id[CNET_MODEL_ID_MAX];
    char                  backend_name[CNET_MODEL_BACKEND_NAME_MAX];
    char                  architecture[64];
    char                  format[32];
    char                  quantization[16];

    /* Physical artifact */
    char                  artifact_path[CNET_MODEL_PATH_MAX];
    uint64_t              artifact_bytes;

    /* Classification */
    CnetModelClass        model_class;
    uint32_t              capabilities;

    /* Resource budgeting */
    uint64_t              resident_bytes_per_resource;
    uint64_t              workspace_bytes;
    uint64_t              kv_bytes_per_token;

    /* Placement constraints */
    uint64_t              allowed_resource_mask;
    uint64_t              preferred_resource_mask;
    uint32_t              required_resource_count;

    /* Context limits */
    uint32_t              context_limit;
} CnetModelDescriptor;

/* ---- Backend capability interface (vtable) ---- */

typedef int  (*CnetModelBackendLoadFn)(
    void *context,
    const CnetModelDescriptor *descriptor,
    uint64_t resource_mask,
    void **handle_out,
    uint64_t *resident_bytes_per_resource_out);

typedef void (*CnetModelBackendUnloadFn)(
    void *context,
    void *handle);

typedef struct {
    uint32_t                abi_version;
    uint32_t                struct_size;
    char                    name[CNET_MODEL_BACKEND_NAME_MAX];
    void                   *context;
    CnetModelBackendLoadFn   load;
    CnetModelBackendUnloadFn unload;
} CnetModelBackendSpec;

/* ---- Manager options, leases, and stats ---- */

typedef struct {
    uint64_t resource_mask;  /* CnetModelResource bit(s) */
    uint64_t budget_bytes;   /* max resident bytes on this resource */
} CnetModelBudget;

typedef struct {
    uint32_t               abi_version;
    uint32_t               struct_size;
    uint32_t               max_models;
    uint32_t               max_backends;
    const CnetModelBudget *budgets;
    uint32_t               budget_count;
} CnetModelManagerOptions;

typedef struct {
    char     model_id[CNET_MODEL_ID_MAX];
    void    *handle;
    uint64_t generation;
    uint64_t resource_mask;
} CnetModelLease;

typedef struct {
    CnetModelState state;
    CnetModelClass model_class;
    uint64_t       resident_bytes;
    uint32_t       lease_count;
    uint32_t       pin_count;
    uint32_t       loads;
    uint32_t       cache_hits;
    uint32_t       evictions;
    uint32_t       load_failures;
} CnetModelStats;

typedef struct {
    uint64_t resident_bytes;
    uint64_t reserved_bytes;
    uint64_t budget_bytes;
    uint32_t resident_models;
} CnetModelResourceStats;

/* ---- Manager lifecycle ---- */

typedef struct CnetModelManager CnetModelManager;

#ifdef __cplusplus
extern "C" {
#endif

CNET_API int cnet_model_manager_open(CnetModelManager **manager_out,
                                     const CnetModelManagerOptions *options);
CNET_API int cnet_model_manager_close(CnetModelManager *manager);
CNET_API int cnet_model_manager_model_count(const CnetModelManager *manager);

CNET_API int cnet_model_backend_register(CnetModelManager *manager,
                                         const CnetModelBackendSpec *backend);

CNET_API int cnet_model_catalog_add(CnetModelManager *manager,
                                    const CnetModelDescriptor *descriptor);

CNET_API int cnet_model_acquire(CnetModelManager *manager,
                                const char *model_id,
                                uint64_t preferred_resource_mask,
                                CnetModelLease *lease_out);
CNET_API int cnet_model_release(CnetModelManager *manager,
                                const CnetModelLease *lease);

CNET_API int cnet_model_pin(CnetModelManager *manager, const char *model_id);
CNET_API int cnet_model_unpin(CnetModelManager *manager, const char *model_id);

CNET_API int cnet_model_evict(CnetModelManager *manager, const char *model_id);

CNET_API int cnet_model_reset_failure(CnetModelManager *manager,
                                      const char *model_id);

CNET_API int cnet_model_stats(const CnetModelManager *manager,
                              const char *model_id,
                              CnetModelStats *stats_out);
CNET_API int cnet_model_resource_stats(const CnetModelManager *manager,
                                       uint64_t resource_mask,
                                       CnetModelResourceStats *stats_out);

/* ---- Catalog-only helpers (usable without a manager) ---- */

CNET_API int cnet_descriptor_validate(const CnetModelDescriptor *d);
CNET_API void cnet_descriptor_print(const CnetModelDescriptor *d);
CNET_API const char *cnet_model_class_name(CnetModelClass c);

#ifdef __cplusplus
}
#endif

#endif /* CNET_MODEL_RUNTIME_H */