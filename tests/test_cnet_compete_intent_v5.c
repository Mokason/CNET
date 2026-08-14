#include "cnet_compete_intent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REQUIRE(condition, reason)                                           \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_INTENT_V5_RED reason=%s\n", reason);           \
            goto cleanup;                                                     \
        }                                                                     \
    } while (0)

int main(int argc, char **argv) {
    CnetCompeteIntentModel *model = NULL;
    CnetCompeteIntentReport trained, loaded;
    char rejected_directory[] = "/tmp/cnet-intent-v5-reject-XXXXXX";
    char rejected_model[128] = {0}, rejected_meta[128] = {0};
    int rejected_directory_created = 0;
    int rc = 1;

    REQUIRE(argc == 5, "artifact_arguments");
    memset(&trained, 0, sizeof trained);
    memset(&loaded, 0, sizeof loaded);
    REQUIRE(cnet_compete_intent_train_v5(argv[1], argv[2], argv[3], argv[4],
                                         &trained) == 0,
            "train");
    REQUIRE(trained.source_examples ==
                CNET_COMPETE_INTENT_V5_SOURCE_EXAMPLES &&
                trained.train_examples ==
                CNET_COMPETE_INTENT_V5_TRAIN_EXAMPLES &&
                trained.train_steps == CNET_COMPETE_INTENT_V5_TRAIN_STEPS &&
                strcmp(trained.provenance,
                       CNET_COMPETE_INTENT_V5_PROVENANCE) == 0,
            "training_profile");
    REQUIRE(cnet_compete_intent_load(argv[1], argv[2], &model, &loaded) == 0,
            "packed_load");
    REQUIRE(loaded.source_examples == trained.source_examples &&
                loaded.train_examples == trained.train_examples &&
                loaded.train_steps == trained.train_steps &&
                strcmp(loaded.provenance, trained.provenance) == 0,
            "metadata_roundtrip");
    cnet_compete_intent_free(model);
    model = NULL;

    REQUIRE(mkdtemp(rejected_directory) != NULL, "rejection_directory");
    rejected_directory_created = 1;
    REQUIRE(snprintf(rejected_model, sizeof rejected_model, "%s/model",
                     rejected_directory) > 0 &&
                snprintf(rejected_meta, sizeof rejected_meta, "%s/meta",
                         rejected_directory) > 0,
            "rejection_paths");
    REQUIRE(cnet_compete_intent_train_v5(rejected_model, rejected_meta,
                                         argv[3], argv[3], NULL) != 0 &&
                access(rejected_model, F_OK) != 0 &&
                access(rejected_meta, F_OK) != 0,
            "v5_corpus_identity");

    printf("CNET_7B_INTENT_V5_PASS params=%ld source_examples=%zu "
           "replay_examples=%zu steps=%zu threshold=%.9f "
           "calibration=%zu/%zu ood=%zu/%zu bytes=%zu fnv=%llu "
           "provenance=%s\n",
           loaded.parameters, loaded.source_examples, loaded.train_examples,
           loaded.train_steps, loaded.threshold, loaded.calibration_correct,
           loaded.calibration_covered, loaded.calibration_ood_abstained,
           loaded.calibration_ood, loaded.artifact_bytes,
           loaded.artifact_fnv, loaded.provenance);
    rc = 0;
cleanup:
    cnet_compete_intent_free(model);
    if (rejected_model[0] != '\0') (void)unlink(rejected_model);
    if (rejected_meta[0] != '\0') (void)unlink(rejected_meta);
    if (rejected_directory_created)
        (void)rmdir(rejected_directory);
    return rc;
}
