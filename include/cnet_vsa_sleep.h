#ifndef CNET_VSA_SLEEP_H
#define CNET_VSA_SLEEP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "cnet_vsa.h"
#include "cnet_vsa_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_SLEEP_MAX_BUFFER 256
#define CNET_VSA_SLEEP_MAX_CLUSTERS 32
#define CNET_VSA_SLEEP_TEXT_MAX 512

typedef struct {
    int node_id;
    char text[CNET_VSA_SLEEP_TEXT_MAX];
    char label[CNET_VSA_NAME_MAX];
    float vector[CNET_VSA_DEFAULT_DIM];
    uint32_t access_count;
    float confidence;
    bool contract_verified;
    uint64_t timestamp;
} CnetVsaSleepEvent;

typedef struct {
    int cluster_id;
    char prototype_label[CNET_VSA_NAME_MAX];
    float centroid[CNET_VSA_DEFAULT_DIM];
    float coherence;
    size_t member_count;
} CnetVsaConsolidatedPrototype;

typedef struct {
    int dim;
    size_t event_count;
    CnetVsaSleepEvent events[CNET_VSA_SLEEP_MAX_BUFFER];
    size_t cluster_count;
    CnetVsaConsolidatedPrototype prototypes[CNET_VSA_SLEEP_MAX_CLUSTERS];
    uint32_t min_access_threshold;
    float min_coherence_threshold;
    uint64_t total_pruned_count;
    uint64_t total_promoted_count;
} CnetVsaSleepConsolidator;

/* Initialize sleep consolidator */
int  cnet_vsa_sleep_init(CnetVsaSleepConsolidator *sc, int dim, uint32_t min_access, float min_coherence);
void cnet_vsa_sleep_free(CnetVsaSleepConsolidator *sc);

/* Record a session observation/event into the wake buffer */
int  cnet_vsa_sleep_record_event(CnetVsaSleepConsolidator *sc, const char *label, const char *text,
                                 const float *vec, uint32_t access_count,
                                 float confidence, bool contract_verified);

/* Run one full sleep consolidation cycle:
   1. Decay & prune transient / unverified noise
   2. Metric clustering over remaining verified events
   3. Synthesize prototype centroids and calculate coherence
   4. Promote verified prototypes into permanent LTM codebook */
int  cnet_vsa_sleep_consolidate(CnetVsaSleepConsolidator *sc, CnetVsaCodebook *ltm_codebook,
                                size_t *out_pruned, size_t *out_promoted);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_SLEEP_H */
