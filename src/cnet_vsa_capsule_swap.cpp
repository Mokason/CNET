#include "../include/cnet_vsa_capsule_swap.h"
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

static uint64_t get_time_ns(void) {
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

int cnet_vsa_capsule_mgr_init(CnetVsaCapsuleManager *mgr, size_t max_vram_bytes) {
    if (!mgr) return -1;
    memset(mgr, 0, sizeof(*mgr));
    mgr->max_vram_bytes = max_vram_bytes > 0 ? max_vram_bytes : (size_t)(1024 * 1024 * 1024); /* 1 GB default */
    return 0;
}

void cnet_vsa_capsule_mgr_free(CnetVsaCapsuleManager *mgr) {
    if (!mgr) return;
    for (size_t i = 0; i < mgr->capsule_count; ++i) {
        CnetVsaCapsuleEntry *cap = &mgr->capsules[i];
        if (cap->location == CNET_VSA_CAPSULE_VRAM && cap->device_weights) {
            if (cnet_vsa_device_is_gpu_active()) {
                (void)hipFree(cap->device_weights);
            }
            cap->device_weights = NULL;
        }
        if (cap->host_weights) {
            free(cap->host_weights);
            cap->host_weights = NULL;
        }
    }
    memset(mgr, 0, sizeof(*mgr));
}

int cnet_vsa_capsule_register(CnetVsaCapsuleManager *mgr, const char *name,
                               const char *domain, const float *centroid,
                               const void *weights, size_t weight_bytes) {
    if (!mgr || !name || !domain || !centroid || mgr->capsule_count >= CNET_VSA_MAX_CAPSULES) {
        return -1;
    }

    CnetVsaCapsuleEntry *cap = &mgr->capsules[mgr->capsule_count++];
    cap->capsule_id = (int)mgr->capsule_count;
    snprintf(cap->name, sizeof(cap->name), "%s", name);
    snprintf(cap->domain, sizeof(cap->domain), "%s", domain);
    memcpy(cap->centroid, centroid, CNET_VSA_DEFAULT_DIM * sizeof(float));

    /* Compute 64-byte VSA signature */
    uint64_t seed = 0x12345678ULL;
    for (const char *p = name; *p; ++p) seed = seed * 31 + (uint8_t)*p;
    for (const char *p = domain; *p; ++p) seed = seed * 37 + (uint8_t)*p;
    cnet_vsa_bsc_random(&cap->signature, &seed);

    cap->weight_bytes = weight_bytes;
    if (weights && weight_bytes > 0) {
        cap->host_weights = malloc(weight_bytes);
        memcpy(cap->host_weights, weights, weight_bytes);
    } else {
        cap->host_weights = NULL;
    }

    cap->device_weights = NULL;
    cap->location = CNET_VSA_CAPSULE_HOST_RAM;
    cap->last_accessed_tick = 0;
    cap->access_count = 0;

    return cap->capsule_id;
}

int cnet_vsa_capsule_evict(CnetVsaCapsuleManager *mgr, int capsule_id) {
    if (!mgr || capsule_id <= 0) return -1;

    for (size_t i = 0; i < mgr->capsule_count; ++i) {
        CnetVsaCapsuleEntry *cap = &mgr->capsules[i];
        if (cap->capsule_id == capsule_id) {
            if (cap->location == CNET_VSA_CAPSULE_VRAM) {
                if (cnet_vsa_device_is_gpu_active() && cap->device_weights) {
                    (void)hipFree(cap->device_weights);
                }
                cap->device_weights = NULL;
                cap->location = CNET_VSA_CAPSULE_HOST_RAM;
                if (mgr->current_vram_bytes >= cap->weight_bytes) {
                    mgr->current_vram_bytes -= cap->weight_bytes;
                } else {
                    mgr->current_vram_bytes = 0;
                }
                mgr->evict_count++;
                return 0;
            }
            return 0;
        }
    }
    return -1;
}

int cnet_vsa_capsule_hot_swap(CnetVsaCapsuleManager *mgr, const float *query_vec,
                               CnetVsaCapsuleEntry **out_capsule,
                               void **out_device_weights,
                               double *out_swap_ms) {
    if (!mgr || !query_vec || mgr->capsule_count == 0 || !out_capsule) return -1;

    mgr->current_tick++;

    /* Phase 1: Router matches query against capsule centroids */
    float best_sim = -2.0f;
    CnetVsaCapsuleEntry *best_cap = NULL;

    for (size_t i = 0; i < mgr->capsule_count; ++i) {
        float sim = cnet_vsa_similarity(query_vec, mgr->capsules[i].centroid, CNET_VSA_DEFAULT_DIM);
        if (sim > best_sim) {
            best_sim = sim;
            best_cap = &mgr->capsules[i];
        }
    }

    if (!best_cap) return -1;

    best_cap->last_accessed_tick = mgr->current_tick;
    best_cap->access_count++;
    *out_capsule = best_cap;

    /* Phase 2: Check if capsule is already hot in VRAM */
    if (best_cap->location == CNET_VSA_CAPSULE_VRAM && best_cap->device_weights != NULL) {
        if (out_device_weights) *out_device_weights = best_cap->device_weights;
        if (out_swap_ms) *out_swap_ms = 0.0;
        return 0;
    }

    /* Phase 3: Page into VRAM (evicting LRU if budget exceeded) */
    uint64_t t0 = get_time_ns();

    while (mgr->current_vram_bytes + best_cap->weight_bytes > mgr->max_vram_bytes) {
        /* Find least recently used capsule currently in VRAM (excluding best_cap) */
        CnetVsaCapsuleEntry *lru_cap = NULL;
        uint64_t oldest_tick = UINT64_MAX;

        for (size_t i = 0; i < mgr->capsule_count; ++i) {
            CnetVsaCapsuleEntry *c = &mgr->capsules[i];
            if (c != best_cap && c->location == CNET_VSA_CAPSULE_VRAM) {
                if (c->last_accessed_tick < oldest_tick) {
                    oldest_tick = c->last_accessed_tick;
                    lru_cap = c;
                }
            }
        }

        if (!lru_cap) {
            /* No more capsules can be evicted */
            break;
        }

        cnet_vsa_capsule_evict(mgr, lru_cap->capsule_id);
    }

    /* Allocate on GPU and DMA copy */
    if (cnet_vsa_device_is_gpu_active()) {
        hipError_t err = hipMalloc(&best_cap->device_weights, best_cap->weight_bytes);
        if (err != hipSuccess) {
            fprintf(stderr, "VRAM Allocation error: %s\n", hipGetErrorString(err));
            return -1;
        }
        err = hipMemcpy(best_cap->device_weights, best_cap->host_weights,
                        best_cap->weight_bytes, hipMemcpyHostToDevice);
        if (err != hipSuccess) {
            (void)hipFree(best_cap->device_weights);
            best_cap->device_weights = NULL;
            return -1;
        }
    } else {
        /* CPU Mode: alias host weights */
        best_cap->device_weights = best_cap->host_weights;
    }

    best_cap->location = CNET_VSA_CAPSULE_VRAM;
    mgr->current_vram_bytes += best_cap->weight_bytes;
    if (mgr->current_vram_bytes > mgr->peak_vram_bytes) {
        mgr->peak_vram_bytes = mgr->current_vram_bytes;
    }
    mgr->swap_in_count++;

    uint64_t t1 = get_time_ns();
    double swap_ms = (double)(t1 - t0) / 1000000.0;
    mgr->total_swap_time_ms += swap_ms;

    if (out_device_weights) *out_device_weights = best_cap->device_weights;
    if (out_swap_ms) *out_swap_ms = swap_ms;

    return 0;
}
