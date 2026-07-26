#include "cnet_sleep_consolidate.h"

#include "corpus/tile_memory.h"

#include <stdio.h>
#include <string.h>

static int sleep_class_valid(CnetMemoryClass memory_class) {
    return memory_class == CNET_MEMORY_SEMANTIC ||
           memory_class == CNET_MEMORY_PROCEDURAL;
}

static uint64_t sleep_hash(uint64_t hash, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        hash ^= (uint64_t)*p++;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= 0xffu;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static int sleep_episode_valid(const CnetSleepEpisode *episode) {
    return episode && episode->episode_id && episode->episode_id[0] &&
           episode->trigger && episode->trigger[0] &&
           episode->content && episode->content[0] &&
           episode->source && episode->source[0] &&
           sleep_class_valid(episode->promote_to);
}

void cnet_sleep_config_defaults(CnetSleepConfig *config) {
    if (!config) return;
    config->vector_dim = 256;
    config->hot_capacity = 256;
    config->dedup_threshold = 0.97;
    config->semantic_merge_threshold = 0.72;
    config->minimum_terms = 3;
    config->maximum_document_frequency = 0.25;
    config->maximum_candidates = 32;
}

int cnet_sleep_consolidate(const char *store_directory,
                           const CnetSleepEpisode *episodes,
                           size_t episode_count,
                           const CnetSleepConfig *config,
                           CnetSleepReport *report) {
    CnetSleepConfig defaults;
    ConsolidateReport consolidation;
    TileMemory *memory;
    size_t i;
    uint64_t digest = UINT64_C(1469598103934665603);
    if (!store_directory || !store_directory[0] ||
        strlen(store_directory) >= 240 || !episodes ||
        episode_count == 0 || !report) {
        return -1;
    }
    if (!config) {
        cnet_sleep_config_defaults(&defaults);
        config = &defaults;
    }
    if (config->vector_dim < 8 || config->hot_capacity < 1 ||
        config->dedup_threshold < 0.0 || config->dedup_threshold > 1.0 ||
        config->semantic_merge_threshold < 0.0 ||
        config->semantic_merge_threshold > 1.0 ||
        config->minimum_terms < 1 ||
        config->maximum_document_frequency < 0.0 ||
        config->maximum_document_frequency > 1.0 ||
        config->maximum_candidates < 1) {
        return -1;
    }
    for (i = 0; i < episode_count; i++) {
        if (!sleep_episode_valid(&episodes[i])) return -1;
    }
    memory = tilemem_open(store_directory, config->vector_dim,
                          config->hot_capacity,
                          config->dedup_threshold);
    if (!memory) return -2;
    memset(report, 0, sizeof(*report));
    memset(&consolidation, 0, sizeof(consolidation));
    report->episodes_seen = episode_count;
    for (i = 0; i < episode_count; i++) {
        char provenance[64];
        const char *label = episodes[i].promote_to == CNET_MEMORY_SEMANTIC ?
                            "semantic" : "procedural";
        int result;
        (void)snprintf(provenance, sizeof(provenance), "%.30s#%.30s",
                       episodes[i].source, episodes[i].episode_id);
        result = tilemem_ingest(memory, episodes[i].trigger,
                                episodes[i].content, label, provenance);
        if (result < 0) {
            tilemem_close(memory);
            return -3;
        }
        if (result == 0) {
            report->redundant_pruned++;
        } else {
            report->episodes_ingested++;
            if (episodes[i].promote_to == CNET_MEMORY_SEMANTIC)
                report->semantic_promoted++;
            else
                report->procedural_promoted++;
        }
        digest = sleep_hash(digest, episodes[i].episode_id);
        digest = sleep_hash(digest, episodes[i].source);
        digest = sleep_hash(digest, episodes[i].trigger);
        digest = sleep_hash(digest, episodes[i].content);
        digest = sleep_hash(digest, label);
    }
    (void)tilemem_consolidate(memory,
                              config->semantic_merge_threshold,
                              config->minimum_terms,
                              config->maximum_document_frequency,
                              config->maximum_candidates,
                              &consolidation);
    report->tiles_before = consolidation.tiles_before;
    report->tiles_after = consolidation.tiles_after;
    report->merges = consolidation.merges;
    report->comparisons = consolidation.comparisons;
    report->redundant_pruned += consolidation.merges;
    report->provenance_digest = digest;
    tilemem_close(memory);
    return 0;
}

int cnet_sleep_report_json(const CnetSleepReport *report,
                           char *output,
                           size_t output_capacity) {
    int written;
    if (!report || !output || output_capacity == 0) return -1;
    written = snprintf(
        output, output_capacity,
        "{\"episodes_seen\":%zu,\"episodes_ingested\":%zu,"
        "\"redundant_pruned\":%zu,\"semantic_promoted\":%zu,"
        "\"procedural_promoted\":%zu,\"tiles_before\":%zu,"
        "\"tiles_after\":%zu,\"merges\":%zu,\"graduated_units\":%zu,"
        "\"comparisons\":%zu,"
        "\"provenance_digest\":\"%016llx\"}",
        report->episodes_seen, report->episodes_ingested,
        report->redundant_pruned, report->semantic_promoted,
        report->procedural_promoted, report->tiles_before,
        report->tiles_after, report->merges, report->graduated_units,
        report->comparisons,
        (unsigned long long)report->provenance_digest);
    if (written < 0 || (size_t)written >= output_capacity) return -2;
    return written;
}
