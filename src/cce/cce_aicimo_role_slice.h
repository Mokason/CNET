#ifndef CCE_AICIMO_ROLE_SLICE_H
#define CCE_AICIMO_ROLE_SLICE_H

#include "cce_aicimo.h"

/* Role-slice routing (Drole vs Dlex decoupling) */
int aicimo_route_on_drole(AicimoRouter *r, const float *input, size_t in_len,
                          float *output, size_t out_cap, size_t *used_ops);

int aicimo_route_role_slice(AicimoRouter *r, const float *input, size_t in_len,
                            float *output, size_t out_cap, 
                            const char *role, size_t *used_ops);

#endif /* CCE_AICIMO_ROLE_SLICE_H */