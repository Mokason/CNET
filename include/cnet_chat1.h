#ifndef CNET_CHAT1_H
#define CNET_CHAT1_H

#define CNET_CHAT1_SUITE_ID "CNET-ASI-CHAT-1"
#define CNET_CHAT1_STATE_ROOT "/home/marble/.local/state/cnet/cnet_asi_chat1"
#define CNET_CHAT1_FIXTURE_PATH "benchmarks/cnet_asi_chat1/heldout.tsv"
#define CNET_CHAT1_SYSTEM_PATH "benchmarks/cnet_asi_chat1/baseline_system.txt"
#define CNET_CHAT1_COVERED_ROWS 80u
#define CNET_CHAT1_OOD_ROWS 32u
#define CNET_CHAT1_TOTAL_ROWS (CNET_CHAT1_COVERED_ROWS + CNET_CHAT1_OOD_ROWS)
#define CNET_CHAT1_PROVENANCE "verified_spec_chat1"

#ifndef CNET_CHAT1_FREEZE_COMMIT
#define CNET_CHAT1_FREEZE_COMMIT ""
#endif

#endif
