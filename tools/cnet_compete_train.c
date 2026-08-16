#include "cnet_compete_intent.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    CnetCompeteIntentReport report;
    if (argc != 4) {
        fprintf(stderr, "usage: %s MODEL META SEMANTIC_DEVELOPMENT.tsv\n",
                argv[0]);
        return 2;
    }
    memset(&report, 0, sizeof report);
    if (cnet_compete_intent_train(argv[1], argv[2], argv[3], &report) != 0) {
        printf("CNET_7B_INTENT_TRAIN_FAIL\n");
        return 1;
    }
    printf("CNET_7B_INTENT_TRAIN_PASS params=%ld source_examples=%zu "
           "replay_examples=%zu steps=%zu "
           "train_mean_loss=%.9g threshold=%.9f calibration=%zu/%zu ood=%zu/%zu "
           "parity=%zu max_dnll=%.3g bytes=%zu fnv=%llu provenance=%s\n",
           report.parameters, report.source_examples, report.train_examples,
           report.train_steps,
           report.final_mean_loss, report.threshold,
           report.calibration_correct, report.calibration_covered,
           report.calibration_ood_abstained, report.calibration_ood,
           report.packed_parity_mismatches, report.packed_max_nll_delta,
           report.artifact_bytes, report.artifact_fnv, report.provenance);
    return 0;
}
