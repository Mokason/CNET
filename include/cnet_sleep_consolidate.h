#ifndef CNET_SLEEP_CONSOLIDATE_H
#define CNET_SLEEP_CONSOLIDATE_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CnetMemoryClass {
    CNET_MEMORY_SEMANTIC = 1,
    CNET_MEMORY_PROCEDURAL = 2
} CnetMemoryClass;

typedef struct CnetSleepEpisode {
    const char *episode_id;
    const char *trigger;
    const char *content;
    const char *source;
    CnetMemoryClass promote_to;
} CnetSleepEpisode;

typedef struct CnetSleepConfig {
    int vector_dim;
    int hot_capacity;
    double dedup_threshold;
    double semantic_merge_threshold;
    int minimum_terms;
    double maximum_document_frequency;
    int maximum_candidates;
} CnetSleepConfig;

typedef struct CnetSleepReport {
    size_t episodes_seen;
    size_t episodes_ingested;
    size_t redundant_pruned;
    size_t semantic_promoted;
    size_t procedural_promoted;
    size_t tiles_before;
    size_t tiles_after;
    size_t merges;
    size_t graduated_units;
    size_t comparisons;
    uint64_t provenance_digest;
} CnetSleepReport;

typedef struct CnetSleepState {
    uint64_t last_activity_tick;
    uint64_t last_sleep_tick;
    uint64_t slept_through_revision;
} CnetSleepState;

enum {
    CNET_SLEEP_RAN = 0,
    CNET_SLEEP_AWAKE = 1,
    CNET_SLEEP_CURRENT = 2
};

CNET_API void cnet_sleep_config_defaults(CnetSleepConfig *config);
CNET_API int cnet_sleep_consolidate(const char *store_directory,
                                    const CnetSleepEpisode *episodes,
                                    size_t episode_count,
                                    const CnetSleepConfig *config,
                                    CnetSleepReport *report);
CNET_API int cnet_sleep_report_json(const CnetSleepReport *report,
                                    char *output,
                                    size_t output_capacity);
CNET_API void cnet_sleep_state_init(CnetSleepState *state,
                                    uint64_t now_tick);
CNET_API void cnet_sleep_note_activity(CnetSleepState *state,
                                       uint64_t now_tick);
CNET_API int cnet_sleep_try_consolidate(CnetSleepState *state,
                                        uint64_t now_tick,
                                        uint64_t minimum_idle_ticks,
                                        uint64_t material_revision,
                                        const char *store_directory,
                                        const CnetSleepEpisode *episodes,
                                        size_t episode_count,
                                        const CnetSleepConfig *config,
                                        CnetSleepReport *report);

#ifdef __cplusplus
}
#endif

#endif
