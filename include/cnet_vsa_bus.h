#ifndef CNET_VSA_BUS_H
#define CNET_VSA_BUS_H

#include "cnet_vsa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_MAX_SPECIALISTS 64

typedef enum {
    CNET_VSA_STATUS_SUCCESS = 0,
    CNET_VSA_STATUS_ABSTAIN = 1,     /* Outside certified metric coverage */
    CNET_VSA_STATUS_MARGIN_FAIL = 2,  /* Failed certified margin floor */
    CNET_VSA_STATUS_EXEC_ERR = -1
} CnetVsaStatus;

typedef int (*CnetVsaSpecialistFn)(const float *in, float *delta_out, int dim, void *ctx);

typedef struct {
    char name[CNET_VSA_NAME_MAX];
    CnetVsaMetricContract contract;
    CnetVsaSpecialistFn forward;
    void *user_ctx;
    size_t invocations;
} CnetVsaSpecialist;

typedef struct {
    int dim;
    size_t count;
    CnetVsaSpecialist specialists[CNET_VSA_MAX_SPECIALISTS];
} CnetVsaBus;

/* Initialize universal residual bus */
int  cnet_vsa_bus_init(CnetVsaBus *bus, int dim);
void cnet_vsa_bus_free(CnetVsaBus *bus);

/* Register a certified specialist on the bus */
int  cnet_vsa_bus_register(CnetVsaBus *bus, const char *name,
                           const float *centroid, float radius_epsilon,
                           float margin_floor, CnetVsaSpecialistFn fn,
                           void *user_ctx);

/* Route and execute a single step: finds the best matching certified specialist.
   If admitted, applies additive delta to io_vector: io_vector = normalize(io_vector + delta).
   If no specialist passes metric contract, returns CNET_VSA_STATUS_ABSTAIN without mutating io_vector. */
CnetVsaStatus cnet_vsa_bus_route_step(CnetVsaBus *bus, float *io_vector,
                                      char *out_specialist_name,
                                      size_t name_cap, float *out_margin);

/* Execute a multi-hop chained plan over specified specialists, enforcing the metric contract
   at every single hop. If any intermediate vector falls outside the next specialist's certified
   radius, execution stops immediately and returns CNET_VSA_STATUS_ABSTAIN. */
CnetVsaStatus cnet_vsa_bus_execute_chain(CnetVsaBus *bus,
                                         const char *const *specialist_names,
                                         size_t hop_count, float *io_vector,
                                         size_t *hops_executed);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_BUS_H */
