#ifndef CNET_AGENT_ROLE_H
#define CNET_AGENT_ROLE_H

/* Explicit cognitive / policy roles for CNET layer-3 orchestrators.
 *
 * These are NOT SpecialistRole (lifecycle: active/shadow/recipe/advisory).
 * Agent roles answer "who is speaking / what stance / which tools" above the
 * certified planner. See docs/dispatch.md layer 3 and plans/agent_roles.md.
 *
 * Known closed set:
 *   auditor | researcher | coder | critic | memory-witness
 *
 * Free-form role strings remain valid on the harness ABI for back-compat;
 * only this vocabulary gets the policy table (sampling preference, system
 * fragment, capability mask, stable AICIMO name).
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNET_AGENT_ROLE_AUDITOR         = 0,
    CNET_AGENT_ROLE_RESEARCHER      = 1,
    CNET_AGENT_ROLE_CODER           = 2,
    CNET_AGENT_ROLE_CRITIC          = 3,
    CNET_AGENT_ROLE_MEMORY_WITNESS  = 4,
    CNET_AGENT_ROLE_COUNT           = 5
} CnetAgentRole;

/* Preferred sampling stance. Numeric values match CnetHarnessSamplingMode
 * (DETERMINISTIC=1 … EXPLORATORY=4) so harness can cast without a table.
 * AUTO (0) is never a policy preference. */
typedef enum {
    CNET_AGENT_SAMPLE_DETERMINISTIC = 1,
    CNET_AGENT_SAMPLE_FOCUSED       = 2,
    CNET_AGENT_SAMPLE_BALANCED      = 3,
    CNET_AGENT_SAMPLE_EXPLORATORY   = 4
} CnetAgentSamplingPref;

/* Capability bits: policy-only hints for hosts/tools. They do not grant
 * admission or bypass certification. SEAL is reserved and unused by the
 * five roles (none may seal units by policy default). */
#define CNET_AGENT_CAP_INSPECT  (1u << 0)  /* list/inspect units, contracts  */
#define CNET_AGENT_CAP_RECALL   (1u << 1)  /* read memory / KB               */
#define CNET_AGENT_CAP_SEARCH   (1u << 2)  /* external or wiki search        */
#define CNET_AGENT_CAP_CODE     (1u << 3)  /* propose or edit code           */
#define CNET_AGENT_CAP_WRITE    (1u << 4)  /* mutate workspace files         */
#define CNET_AGENT_CAP_CERTIFY  (1u << 5)  /* check certify / evidence       */
#define CNET_AGENT_CAP_SEAL     (1u << 6)  /* mint/seal units — none default */

typedef struct {
    CnetAgentRole role;
    const char *name;                      /* canonical, stable for AICIMO  */
    CnetAgentSamplingPref preferred_sampling;
    uint32_t capabilities;                 /* CNET_AGENT_CAP_* mask         */
    const char *system_fragment;           /* non-NULL, static              */
} CnetAgentRolePolicy;

/* Parse a role string (case-insensitive). Accepts canonical names and a few
 * aliases (memory_witness, witness). Returns 0 and fills *out, or -1. */
CNET_API int cnet_agent_role_parse(const char *s, CnetAgentRole *out);

/* Canonical lowercase name for logs/AICIMO, or "unknown". */
CNET_API const char *cnet_agent_role_name(CnetAgentRole role);

/* Fill *out with the static policy for a known role. Returns 0, or -1. */
CNET_API int cnet_agent_role_policy(CnetAgentRole role, CnetAgentRolePolicy *out);

/* Parse + policy in one step. Returns 0 on known role, -1 otherwise. */
CNET_API int cnet_agent_role_resolve(const char *s, CnetAgentRolePolicy *out);

/* 1 if s parses as a known agent role, else 0. */
CNET_API int cnet_agent_role_is_known(const char *s);

/* Heap-compose system prompt: role fragment, then optional caller system.
 * Caller must free() the result. Returns NULL on OOM or bad policy. */
CNET_API char *cnet_agent_role_compose_system(const CnetAgentRolePolicy *policy,
                                              const char *caller_system);

/* 1 if mask includes only known capability bits. */
CNET_API int cnet_agent_role_caps_valid(uint32_t mask);

#ifdef __cplusplus
}
#endif

#endif /* CNET_AGENT_ROLE_H */
