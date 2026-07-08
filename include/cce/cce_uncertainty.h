#ifndef CCE_UNCERTAINTY_H
#define CCE_UNCERTAINTY_H

#include "cce_defs.h"
#include "cce_router.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_specialist_uncertainty_report {
    float activation_entropy;
    float activation_variance;
    float route_entropy;
    float route_variance;
    float combined_uncertainty;
} cce_specialist_uncertainty_report;

/* Estimate uncertainty from a specialist activation/logit vector. Returns a
   normalized [0,1] entropy score: peaked activations are low uncertainty,
   flat activations are high uncertainty. */
cce_result cce_specialist_get_uncertainty(const float* activations,
                                          int activation_count,
                                          float* uncertainty);

/* Extended estimator combining activation entropy with route entropy from a
   primary route plus counterfactual routes. The routes array may be NULL. */
cce_result cce_specialist_get_uncertainty_ex(const float* activations,
                                             int activation_count,
                                             const CounterfactualRoute* routes,
                                             int route_count,
                                             cce_specialist_uncertainty_report* report);

#ifdef __cplusplus
}
#endif

#endif /* CCE_UNCERTAINTY_H */
