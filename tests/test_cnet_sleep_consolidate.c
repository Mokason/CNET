#include "cnet_sleep_consolidate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void remove_store(const char *directory) {
    static const char *files[] = {
        "hot.bin", "warm.bin", "idf.bin", "synonyms.bin"
    };
    char path[512];
    size_t i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        (void)snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        (void)unlink(path);
    }
    (void)rmdir(directory);
}

int main(void) {
    char directory[] = "/tmp/cnet-sleep-XXXXXX";
    char json[768];
    CnetSleepConfig config;
    CnetSleepReport report;
    /* Near-paraphrase multi-term keys (share >=3 discriminative terms) so
       tilemem_consolidate can merge; exact-key duplicate proves pruning and
       recurrence graduation (count>=2). */
    const CnetSleepEpisode episodes[] = {
        {"ep-001",
         "alpha beta gamma water freezes zero celsius standard pressure",
         "At standard pressure water freezes at zero degrees Celsius.",
         "session-a", CNET_MEMORY_SEMANTIC},
        {"ep-002",
         "alpha beta gamma water freezes zero celsius standard pressure",
         "At standard pressure water freezes at zero degrees Celsius.",
         "session-a", CNET_MEMORY_SEMANTIC},
        {"ep-003",
         "alpha beta gamma ice forms zero celsius normal pressure",
         "Ice forms near zero Celsius under normal pressure conditions.",
         "session-b", CNET_MEMORY_SEMANTIC},
        {"ep-004",
         "rotate service credential safely create deploy verify revoke",
         "Create the new credential, deploy it, verify it, then revoke the old one.",
         "runbook-a", CNET_MEMORY_PROCEDURAL},
        {"ep-005",
         "rotate service credential safely create deploy verify revoke",
         "Create the new credential, deploy it, verify it, then revoke the old one.",
         "runbook-a", CNET_MEMORY_PROCEDURAL}
    };

    check(mkdtemp(directory) != NULL, "temporary store is created");
    cnet_sleep_config_defaults(&config);
    config.dedup_threshold = 0.99;
    config.semantic_merge_threshold = 0.55;
    config.minimum_terms = 3;
    config.maximum_document_frequency = 1.0; /* do not prune gather terms */
    config.maximum_candidates = 64;
    check(cnet_sleep_consolidate(directory, episodes,
          sizeof(episodes) / sizeof(episodes[0]), &config, &report) == 0,
          "sleep consolidation succeeds");
    check(report.episodes_seen == 5, "all episodes are considered");
    check(report.episodes_ingested >= 2, "unique episodes are ingested");
    check(report.redundant_pruned >= 1, "duplicate episode is pruned");
    check(report.semantic_promoted >= 1, "semantic fact is promoted");
    check(report.procedural_promoted >= 1, "procedure is promoted");
    check(report.merges >= 1, "near-paraphrase tiles merge");
    check(report.graduated_units >= 1,
          "recurrence-certified tiles graduate at sleep layer");
    check(report.provenance_digest != 0, "provenance digest is bound");
    check(cnet_sleep_report_json(&report, json, sizeof(json)) > 0,
          "evidence report formats as JSON");
    check(strstr(json, "\"provenance_digest\":") != NULL,
          "JSON contains provenance binding");
    check(strstr(json, "\"merges\":") != NULL, "JSON reports merges");
    check(strstr(json, "\"graduated_units\":") != NULL,
          "JSON reports graduation");

    remove_store(directory);
    if (failures) return 1;
    printf("SLEEP_EVIDENCE %s\n", json);
    printf("SLEEP_CONSOLIDATE_PASS metric=1.000 merges=%zu pruned=%zu "
           "graduated=%zu semantic=%zu procedural=%zu provenance=%016llx\n",
           report.merges, report.redundant_pruned,
           report.graduated_units,
           report.semantic_promoted, report.procedural_promoted,
           (unsigned long long)report.provenance_digest);
    return 0;
}
