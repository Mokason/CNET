#include "../include/harness_oracle.h"
#include "../include/specialist.h"

#include <stdio.h>
#include <string.h>

int harness_oracle_bind(
    HarnessOracleSlot *slot,
    const char *name,
    Port input_port,
    Port output_port,
    CnetOracleFn fn,
    void *ctx,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest)
{
    if (!slot || !name || !name[0] || !fn || !identity) return -1;
    if (identity->artifact_digest == 0) return -2;
    memset(slot, 0, sizeof *slot);
    snprintf(slot->entry.name, sizeof slot->entry.name, "%s", name);
    slot->entry.input_port = input_port;
    slot->entry.output_port = output_port;
    slot->entry.fn = fn;
    slot->entry.ctx = ctx;
    slot->entry.identity = *identity;
    slot->entry.behavior_digest = behavior_digest
        ? behavior_digest
        : cnet_oracle_identity_digest(identity);
    if (slot->entry.behavior_digest == 0)
        slot->entry.behavior_digest = identity->artifact_digest;
    if (specialist_wrap_oracle(&slot->specialist, &slot->btn, &slot->entry,
                               slot->entry.behavior_digest, 1, name) != 0)
        return -3;
    slot->bound = 1;
    return 0;
}

int harness_oracle_admit(
    HarnessOracleSlot *slot,
    PrimitiveRegistry *reg,
    const Contract *contract)
{
    if (!slot || !slot->bound || !reg || !contract) return -1;
    if (specialist_admit(reg, &slot->specialist, contract) != 0)
        return -2;
    return 0;
}

void harness_oracle_unbind(HarnessOracleSlot *slot) {
    if (!slot) return;
    if (slot->bound)
        btn_free(&slot->btn);
    memset(slot, 0, sizeof *slot);
}
