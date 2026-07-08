#ifndef CCE_COMPRESSION_H
#define CCE_COMPRESSION_H

#include <stddef.h>

#include "cce_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_compression_grads {
    float* values;
    size_t count;
    int preserve_identity_path;
    int preserve_residual_path;
} cce_compression_grads;

/* Initialize a zeroed or previously freed buffer. */
cce_result cce_compression_grads_init(cce_compression_grads* grads,
                                      size_t count);
void cce_compression_grads_free(cce_compression_grads* grads);
void cce_compression_grads_zero(cce_compression_grads* grads);

/* Accumulate the two backward paths compression must not erase:
   identity_grad is the direct path, residual_grad is the residual branch. */
cce_result cce_compression_grads_accumulate(cce_compression_grads* grads,
                                            const float* identity_grad,
                                            const float* residual_grad,
                                            size_t count,
                                            float identity_scale,
                                            float residual_scale);

float cce_compression_grads_l2_norm(const cce_compression_grads* grads);

#ifdef __cplusplus
}
#endif

#endif /* CCE_COMPRESSION_H */
