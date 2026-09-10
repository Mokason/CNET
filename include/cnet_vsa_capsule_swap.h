#ifndef CNET_VSA_CAPSULE_SWAP_H
#define CNET_VSA_CAPSULE_SWAP_H

#include <stddef.h>
#include <stdint.h>
#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"
#include "cnet_vsa_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_MAX_CAPSULES 32
#define CNET_VSA_CAPSULE_NAME_MAX 64

typedef enum {
    CNET_VSA_CAPSULE_ON_DISK = 0,
    CNET_VSA_CAPSULE_HOST_RAM = 1,
    CNET_VSA_CAPSULE_VRAM = 2
} CnetVsaCapsuleLocation;

typedef struct {
    int capsule_id;
    char name[CNET_VSA_CAPSULE_NAME_MAX];
    char domain[128];
    CnetVsaBsc signature;                 /* 64-byte VSA manifold signature */
    float centroid[CNET_VSA_DEFAULT_DIM]; /* Continuous centroid */
    size_t weight_bytes;                  /* Weight / codebook payload size */
    void *host_weights;                   /* Host RAM copy */
    void *device_weights;                 /* GPU VRAM pointer (NULL if not loaded) */
    CnetVsaCapsuleLocation location;
    uint64_t last_accessed_tick;
    uint64_t access_count;
} CnetVsaCapsuleEntry;

typedef struct {
    size_t max_vram_bytes;                /* Hard VRAM budget limit */
    size_t current_vram_bytes;            /* Currently allocated VRAM */
    size_t peak_vram_bytes;               /* Peak VRAM tracked */
    size_t capsule_count;
    CnetVsaCapsuleEntry capsules[CNET_VSA_MAX_CAPSULES];
    uint64_t current_tick;
    uint64_t swap_in_count;
    uint64_t evict_count;
    double total_swap_time_ms;
} CnetVsaCapsuleManager;

/* Initialize Capsule Swapper with a hard VRAM budget */
int  cnet_vsa_capsule_mgr_init(CnetVsaCapsuleManager *mgr, size_t max_vram_bytes);
void cnet_vsa_capsule_mgr_free(CnetVsaCapsuleManager *mgr);

/* Register a specialist capsule with host weights and VSA manifold signature */
int  cnet_vsa_capsule_register(CnetVsaCapsuleManager *mgr, const char *name,
                               const char *domain, const float *centroid,
                               const void *weights, size_t weight_bytes);

/* Query-Driven Dynamic Hot-Swap:
   Matches query_vec against capsule centroids.
   Finds best capsule, ensures it is paged into GPU VRAM (evicting LRU if needed).
   Returns active device pointer to weights and capsule metadata. */
int  cnet_vsa_capsule_hot_swap(CnetVsaCapsuleManager *mgr, const float *query_vec,
                               CnetVsaCapsuleEntry **out_capsule,
                               void **out_device_weights,
                               double *out_swap_ms);

/* Manually evict specific capsule from VRAM */
int  cnet_vsa_capsule_evict(CnetVsaCapsuleManager *mgr, int capsule_id);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_CAPSULE_SWAP_H */
