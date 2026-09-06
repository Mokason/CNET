#ifndef CNET_CAPSULE_CORE_H
#define CNET_CAPSULE_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "cnet_core_cell.h"
/* Single-threaded local capsule runtime. No network, teacher or self-training.
 * open verifies ALL immediate capsule subdirectories (max 4096), atomically
 * constructing a fresh registry. Any corrupt/incompatible capsule refuses.
 * Existing CNB/manifest format only; local checksums are not authentication. */
typedef struct CnetCapsuleCore CnetCapsuleCore;
typedef struct {
    int verified;
    unsigned value;
    size_t hops;
    char units[512];
    char reason[160];
} CnetCapsuleCoreReply;
CnetCapsuleCore *cnet_capsule_core_open(const char *root, char *error, size_t cap);
/* Private candidate view for pre-publication evaluation. Does not write root.
 * Replays existing and proposed capsules together; conflicts refuse. */
CnetCapsuleCore *cnet_capsule_core_open_candidate(const char *root,
    const char *candidate, char *error, size_t cap);
void cnet_capsule_core_close(CnetCapsuleCore *core);
/* Mandatory publication check: replay exact guarded joins of sealed contract
 * labels in old AND proposed inventories. Reject changed answers, new path
 * contradictions, or work-budget exhaustion. Does not generate training data.
 * label_obligations counts replayed joins (not unique tasks). */
int cnet_capsule_core_validate_growth(CnetCapsuleCore *before,
    CnetCapsuleCore *candidate, size_t *label_obligations);
/* Checks deterministic consistency AND replays all old/proposed sealed-label
 * joins through the exact opt-in neural serving path, including coverage. */
int cnet_capsule_core_validate_growth_cell(CnetCapsuleCore *before,CnetCapsuleCore *candidate,
    const CnetCoreCell *cell,uint64_t generation,size_t *label_obligations);
/* Exact text grammar: capsule INPUT_TAG OUTPUT_TAG UNSIGNED_INTEGER.
 * Tags bind the complete imported port signature; ambiguity refuses.
 * One-field PORT_BINARY_MSB/LSB (<=16 bits) and ONEHOT (<=64 symbols) only.
 * All refusal paths zero reply.verified and never return a partial answer. */
int cnet_capsule_core_ask(CnetCapsuleCore *core, const char *request,
                         CnetCapsuleCoreReply *reply);
/* Opt-in experimental selector. Builds its graph from this exact verified
 * inventory (<=62 capsules plus start/goal nodes); unsupported views refuse.
 * Every proposed hop uses the same strict audit/coverage/executor as ask().
 * No fallback, no change to deterministic default or 4096-capsule capacity. */
int cnet_capsule_core_ask_cell(CnetCapsuleCore *core,const char *request,
    const CnetCoreCell *cell,uint64_t generation,CnetCapsuleCoreReply *reply);
/* Opt-in, owner-private bounded demand spool. Stores normalized intent only,
 * never an answer. Returns 0 queued, 1 identical pending intent, -1 refused.
 * Call only for uncovered, syntactically valid capsule requests. */
int cnet_capsule_demand_note(const char *root, const char *request);
#endif
