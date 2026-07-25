/* Cross-process fault-bus dedupe probe.
 *
 * Emits a fixed batch of labeled faults and exits. Running it repeatedly
 * against the same CNET_FAULT_LOG must not grow the file: the dedupe set is
 * seeded from records already on disk, so a fresh process recognises its own
 * previous output.
 *
 * Before that seeding existed the set was in-process only, so every
 * cnet_cert_learn_tick invocation started empty and re-appended its whole seed
 * batch — logs/cnet_faults.jsonl reached 4385 lines holding 311 distinct
 * (unit, input) pairs. test_metric_honesty.py drives this binary.
 */
#include "../include/cnet_fault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_IN_DIM 18
#define PROBE_OUT_DIM 8
#define PROBE_BATCH 96

int main(void) {
    int i;
    for (i = 0; i < PROBE_BATCH; i++) {
        double in[PROBE_IN_DIM];
        double tgt[PROBE_OUT_DIM];
        int j;
        for (j = 0; j < PROBE_IN_DIM; j++) in[j] = 0.0;
        for (j = 0; j < PROBE_OUT_DIM; j++) tgt[j] = 0.0;
        /* Deterministic one-hot pattern: identical across runs by design. */
        in[i % PROBE_IN_DIM] = 1.0;
        in[(i * 7 + 3) % PROBE_IN_DIM] = 1.0;
        tgt[i % PROBE_OUT_DIM] = 1.0;
        cnet_fault_mirror_labeled("json_toolcall_v2", in, tgt, PROBE_IN_DIM,
                                  PROBE_OUT_DIM, "jtc");
    }
    printf("FAULT_DEDUPE_PROBE_OK batch=%d\n", PROBE_BATCH);
    return 0;
}
