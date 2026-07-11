#ifndef CNET_SPECIALIST_H
#define CNET_SPECIALIST_H

/* One specialist, one lifecycle.
 *
 * The runtime already unifies execution through the BTN adapter ABI
 * (nn.h: adapter_forward / adapter_release / adapter_context): a native
 * matrix BTN, a CCE model, and an Oracle unit all become planner nodes by
 * being a BinaryTransformNetwork. What was still missing was the TYPE that
 * says so: the three wrap sites were independent constructors, admission was
 * an ad-hoc three-step ceremony (adapter -> contract -> registry), and the
 * lifecycle vocabulary was scattered across PrimitiveState, CCE tiers, model
 * catalog states, and shadow/recipe flags.
 *
 * This header is that type. A Specialist names WHICH backend a planner node
 * is (kind), and factors the scattered state machines into three orthogonal
 * axes read off the existing machinery — views, not new state, so nothing
 * here can drift from the registry truth:
 *
 *   trust     — how much the evidence lets us rely on it
 *               (mirrors PrimitiveState numerically: FUZZY/PROVISIONAL/
 *                FROZEN/RESET -> UNCERTIFIED/EVIDENCED/CERTIFIED/DEMOTED)
 *   residency — where the weights physically live right now
 *               (one enum for what cce_forest tiers, tile_memory, and the
 *                model catalog each reimplement at their own granularity)
 *   role      — what the specialist is FOR in planning
 *               (active / shadow candidate / expandable recipe chunk /
 *                advisory, i.e. CNET-D-style no-authority artifacts)
 *
 * specialist_admit is the single admission door: certification is the only
 * path in, identical for every kind. Acceptance gate: make unified_specialist
 * (tests/test_heterogeneous_plan.c) plans ONE ordinary DAG whose nodes are a
 * native BTN, a real CCE model, and an Oracle unit, certified end-to-end.
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "nn.h"
#include "router.h"
#include "contract/contract.h"
#include "acquire.h"

struct cce_model; /* cce/cce_model.h; kept opaque here */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPECIALIST_KIND_BTN    = 0,  /* native matrix primitive */
    SPECIALIST_KIND_CCE    = 1,  /* CCE cascade/model behind the adapter ABI */
    SPECIALIST_KIND_ORACLE = 2   /* oracle/mined unit behind the adapter ABI */
} SpecialistKind;

/* Trust axis. Values mirror PrimitiveState on purpose (same numbers, same
   transitions); DEMOTED is numerically last but is NOT "most trusted" —
   ordering by reliance is UNCERTIFIED < EVIDENCED < CERTIFIED, DEMOTED
   excluded. PROOF-vs-SAMPLED certification tiers live in the base/tag layer,
   not here: the registry records certified-or-not. */
typedef enum {
    SPECIALIST_TRUST_UNCERTIFIED = 0,  /* PRIM_FUZZY, or uncertified */
    SPECIALIST_TRUST_EVIDENCED   = 1,  /* PRIM_PROVISIONAL */
    SPECIALIST_TRUST_CERTIFIED   = 2,  /* certified && PRIM_FROZEN */
    SPECIALIST_TRUST_DEMOTED     = 3   /* PRIM_RESET (overrides FROZEN) */
} SpecialistTrust;

/* Residency axis: the one vocabulary for cce_tier_t, CnetModelState, and the
   tile HOT/WARM split. Use the from_* mappers below; do not renumber. */
typedef enum {
    SPECIALIST_RES_HOT  = 0,  /* in RAM, immediately executable */
    SPECIALIST_RES_WARM = 1,  /* mapped / loading; cheap to touch */
    SPECIALIST_RES_COLD = 2   /* on disk / failed; must be (re)loaded */
} SpecialistResidency;

/* Role axis. ADVISORY is reserved for no-authority artifacts (CNET-D lane);
   those never enter the registry, so specialist_axes never returns it —
   it exists so callers holding sidecar artifacts share the vocabulary. */
typedef enum {
    SPECIALIST_ROLE_ACTIVE   = 0,  /* plannable production entry */
    SPECIALIST_ROLE_SHADOW   = 1,  /* shadow_of != NULL: evidence only */
    SPECIALIST_ROLE_RECIPE   = 2,  /* expandable chunk carrying teachers */
    SPECIALIST_ROLE_ADVISORY = 3   /* proposes, never decides */
} SpecialistRole;

typedef struct {
    SpecialistKind kind;
    BinaryTransformNetwork *btn;  /* the planner-visible node; borrowed */
    const char *name;             /* borrowed */
    unsigned long long digest;    /* behavior identity; set by admit */
} Specialist;

/* Wrap a native matrix BTN (refuses runtime adapters: their kind is known
   at their own wrap site). Returns 0, or -1 on bad args. */
CNET_API int specialist_wrap_btn(Specialist *s,
                                 BinaryTransformNetwork *btn,
                                 const char *name);

/* Wrap a real CCE model: initializes *adapter via the CCE contract adapter
   (typed ports imposed at this boundary; the model stays a modular payload)
   and stamps the Specialist. behavior_digest must be stable and nonzero.
   Returns 0, or -1 (adapter untouched on failure). */
CNET_API int specialist_wrap_cce_model(Specialist *s,
                                       BinaryTransformNetwork *adapter,
                                       struct cce_model *model,
                                       int owns_model,
                                       const Port *input_ports,
                                       size_t input_port_count,
                                       const Port *output_ports,
                                       size_t output_port_count,
                                       unsigned long long behavior_digest,
                                       size_t cost_hint,
                                       const char *name);

/* Wrap a registered oracle: initializes *adapter via the oracle contract
   adapter. behavior_digest 0 means "use entry->behavior_digest" (which must
   then be nonzero). Returns 0, or -1. */
CNET_API int specialist_wrap_oracle(Specialist *s,
                                    BinaryTransformNetwork *adapter,
                                    OracleEntry *entry,
                                    unsigned long long behavior_digest,
                                    size_t cost_hint,
                                    const char *name);

/* THE admission door, identical for every kind: certify against `c` and
   register (registry_add_certified — refusal on a failed replay, imposter
   tags, or a not-better same-named candidate). On success records the
   content digest on the Specialist. Returns 0, or -1 (nothing admitted). */
CNET_API int specialist_admit(PrimitiveRegistry *reg,
                              Specialist *s,
                              const Contract *c);

/* Read the trust and role axes for a registered name (first strcmp match,
   same rule as registry_set_state). Pure view over RegistryEntry — no new
   state, so it cannot disagree with the planner's own eligibility logic.
   Either out pointer may be NULL. Returns 0, or -1 on unknown name. */
CNET_API int specialist_axes(const PrimitiveRegistry *reg,
                             const char *name,
                             SpecialistTrust *trust,
                             SpecialistRole *role);

/* Residency mappers: fold the existing residency machineries into the one
   axis. Arguments are the native enums passed as int (cce_tier_t /
   CnetModelState) so this header stays light; unknown values map COLD
   (conservative: assume a load is needed). */
CNET_API SpecialistResidency specialist_residency_from_cce_tier(int cce_tier);
CNET_API SpecialistResidency specialist_residency_from_model_state(int model_state);

/* Stable lowercase atoms for logs/telemetry ("btn", "certified", ...). */
CNET_API const char *specialist_kind_name(SpecialistKind k);
CNET_API const char *specialist_trust_name(SpecialistTrust t);
CNET_API const char *specialist_residency_name(SpecialistResidency r);
CNET_API const char *specialist_role_name(SpecialistRole r);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SPECIALIST_H */
