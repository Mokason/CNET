#ifndef CCE_AICIMO_BRIDGE_H
#define CCE_AICIMO_BRIDGE_H

#include <stddef.h>

int cce_aicimo_expand_context(const float *input, size_t in_len,
                              float *output, size_t out_cap,
                              size_t base_dim);

int cce_aicimo_expand_context_with_uncertainty(const float *input, size_t in_len,
                                               float *output, size_t out_cap,
                                               size_t base_dim,
                                               float *uncertainty);

/* Role-slice routing (Drole vs Dlex) */
int cce_aicimo_route_on_role(const float *input, size_t in_len,
                             float *output, size_t out_cap,
                             size_t base_dim, const char *role,
                             float *uncertainty);

#endif /* CCE_AICIMO_BRIDGE_H */