#ifndef CNET_HARNESS_ORACLE_H
#define CNET_HARNESS_ORACLE_H

/* Admit a generation/harness-backed capability as an ordinary Oracle
 * specialist through the single door (specialist_admit / oracle adapter).
 *
 * This is the Phase-3 control-plane seam: the .NET harness / llama path is
 * not a second planner authority. It becomes a callback oracle with an
 * explicit identity; certification and registry admission stay CNET's.
 * Hermetic tests use a fake callback — no GGUF required.
 *
 * Gate: exercised by make self_improve (harness oracle slice).
 */

#include <stddef.h>
#include <stdint.h>

#include "acquire.h"
#include "cnet_export.h"
#include "nn.h"
#include "router.h"
#include "specialist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    OracleEntry entry;                 /* filled by bind; owned by caller storage */
    BinaryTransformNetwork btn;        /* adapter BTN; free with btn_free */
    Specialist specialist;             /* view over the adapter */
    int bound;
} HarnessOracleSlot;

/* Bind a named harness-style oracle into *slot. fn/ctx implement the port
   contract; identity must be non-NULL with nonzero artifact_digest.
   Returns 0, or <0. */
CNET_API int harness_oracle_bind(
    HarnessOracleSlot *slot,
    const char *name,
    Port input_port,
    Port output_port,
    CnetOracleFn fn,
    void *ctx,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest);

/* Admit the bound slot through specialist_admit into *reg. Requires a
   matching certified Contract (caller-owned). Returns 0, or <0. */
CNET_API int harness_oracle_admit(
    HarnessOracleSlot *slot,
    PrimitiveRegistry *reg,
    const Contract *contract);

CNET_API void harness_oracle_unbind(HarnessOracleSlot *slot);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HARNESS_ORACLE_H */
