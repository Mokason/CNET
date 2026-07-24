/* DoRA-lite: magnitude vector m + LoRA direction. y += m ⊙ lora_delta(x).
 * mag init 1; B=0 => untrained no-op. */
#ifndef CCE_DORA_H
#define CCE_DORA_H

#include "cce_lora.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_dora {
    cce_lora lo;
    float *mag; /* [out_dim], ones */
} cce_dora;

cce_result cce_dora_init(cce_dora *d, int in_dim, int out_dim, int rank,
                         float alpha, uint32_t seed);
void cce_dora_free(cce_dora *d);
/* y_accum += mag ⊙ lora_delta(x) */
cce_result cce_dora_apply(const cce_dora *d, const cce_tensor *x, cce_tensor *y_accum);
/* Train LoRA on residual then light mag scale from residual mean. */
double cce_dora_train(cce_dora *d, const float *inputs, const float *residuals,
                      size_t n, const cce_lora_train_opts *opt);

#ifdef __cplusplus
}
#endif
#endif
