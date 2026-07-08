#include "../../include/cce/cce_compression.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

cce_result cce_compression_grads_init(cce_compression_grads* grads,
                                      size_t count) {
    if (!grads || count == 0) return CCE_ERR_INVALID_ARG;
    memset(grads, 0, sizeof(*grads));
    grads->values = (float*)calloc(count, sizeof(float));
    if (!grads->values) return CCE_ERR_OOM;
    grads->count = count;
    grads->preserve_identity_path = 1;
    grads->preserve_residual_path = 1;
    return CCE_OK;
}

void cce_compression_grads_free(cce_compression_grads* grads) {
    if (!grads) return;
    free(grads->values);
    memset(grads, 0, sizeof(*grads));
}

void cce_compression_grads_zero(cce_compression_grads* grads) {
    if (!grads || !grads->values) return;
    memset(grads->values, 0, grads->count * sizeof(float));
}

cce_result cce_compression_grads_accumulate(cce_compression_grads* grads,
                                            const float* identity_grad,
                                            const float* residual_grad,
                                            size_t count,
                                            float identity_scale,
                                            float residual_scale) {
    if (!grads || !grads->values || grads->count != count || count == 0)
        return CCE_ERR_INVALID_ARG;
    if (!identity_grad && !residual_grad) return CCE_ERR_INVALID_ARG;

    for (size_t i = 0; i < count; ++i) {
        float v = 0.0f;
        if (identity_grad && grads->preserve_identity_path) {
            v += identity_scale * identity_grad[i];
        }
        if (residual_grad && grads->preserve_residual_path) {
            v += residual_scale * residual_grad[i];
        }
        grads->values[i] += v;
    }
    return CCE_OK;
}

float cce_compression_grads_l2_norm(const cce_compression_grads* grads) {
    if (!grads || !grads->values || grads->count == 0) return 0.0f;
    double sum = 0.0;
    for (size_t i = 0; i < grads->count; ++i) {
        sum += (double)grads->values[i] * grads->values[i];
    }
    return (float)sqrt(sum);
}
