#ifndef CNET_CALIBRATED_GOVERNANCE_H
#define CNET_CALIBRATED_GOVERNANCE_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_GOVERNANCE_ROUTE_MAX 64u
#define CNET_CLAIM_MAX 512u
#define CNET_EVIDENCE_REF_MAX 96u
#define CNET_CLAIM_EVIDENCE_MAX 8u

typedef enum CnetGovernanceDecision {
    CNET_GOVERNANCE_ERROR = -1,
    CNET_GOVERNANCE_ABSTAIN = 0,
    CNET_GOVERNANCE_ANSWER = 1
} CnetGovernanceDecision;

typedef struct CnetRoutePolicy {
    char route_id[CNET_GOVERNANCE_ROUTE_MAX];
    double minimum_margin;
    double minimum_reliability;
    size_t minimum_samples;
} CnetRoutePolicy;

typedef struct CnetClaimBinding {
    char claim[CNET_CLAIM_MAX];
    char evidence_refs[CNET_CLAIM_EVIDENCE_MAX][CNET_EVIDENCE_REF_MAX];
    size_t evidence_count;
    uint64_t binding_digest;
} CnetClaimBinding;

CNET_API CnetGovernanceDecision cnet_governance_decide(
    const CnetRoutePolicy *policy,
    double margin,
    double reliability,
    size_t sample_count);
CNET_API int cnet_governance_load_json(const char *path,
                                       CnetRoutePolicy *policies,
                                       size_t policy_capacity,
                                       size_t *out_policy_count);
CNET_API const CnetRoutePolicy *cnet_governance_find_policy(
    const CnetRoutePolicy *policies,
    size_t policy_count,
    const char *route_id);
CNET_API int cnet_claim_bind(const char *claim,
                             const char *const *evidence_refs,
                             size_t evidence_count,
                             CnetClaimBinding *binding);
CNET_API int cnet_claim_binding_json(const CnetClaimBinding *binding,
                                     char *output,
                                     size_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
