#ifndef CNET_SPECIALIST_ADAPTERS_H
#define CNET_SPECIALIST_ADAPTERS_H

#include <stddef.h>
#include <stdint.h>

#include "acquire.h"
#include "cnet_export.h"
#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Project a registered numeric oracle/tool into the canonical specialist ABI.
   The OracleEntry is borrowed and must outlive the adapter. Calls, refusals,
   abstentions, and output-domain failures update the entry's evidence counters.
   The stable digest must identify the implementation/artifact, never pointers. */
CNET_API int cnet_oracle_init_contract_adapter(
    BinaryTransformNetwork *btn,
    OracleEntry *entry,
    uint64_t behavior_digest,
    size_t cost_hint
);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SPECIALIST_ADAPTERS_H */
