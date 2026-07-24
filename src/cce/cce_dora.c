#include "../../include/cce/cce_dora.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

cce_result cce_dora_init(cce_dora *d, int in_dim, int out_dim, int rank,
                         float alpha, uint32_t seed) {
    cce_result r;
    int i;
    if (!d || in_dim <= 0 || out_dim <= 0 || rank <= 0) return CCE_ERR_INVALID_ARG;
    memset(d, 0, sizeof *d);
    r = cce_lora_init(&d->lo, in_dim, out_dim, rank, alpha, seed);
    if (r != CCE_OK) return r;
    d->mag = (float *)malloc((size_t)out_dim * sizeof(float));
    if (!d->mag) {
        cce_lora_free(&d->lo);
        return CCE_ERR_OOM;
    }
    for (i = 0; i < out_dim; i++) d->mag[i] = 1.0f;
    return CCE_OK;
}

void cce_dora_free(cce_dora *d) {
    if (!d) return;
    cce_lora_free(&d->lo);
    free(d->mag);
    d->mag = NULL;
}

cce_result cce_dora_apply(const cce_dora *d, const cce_tensor *x, cce_tensor *y_accum) {
    cce_tensor tmp;
    cce_result r;
    int o, out;
    int ys[1];
    if (!d || !x || !y_accum) return CCE_ERR_INVALID_ARG;
    out = d->lo.out_dim;
    ys[0] = out;
    r = cce_tensor_alloc(&tmp, ys, 1);
    if (r != CCE_OK) return r;
    for (o = 0; o < out; o++) tmp.data[o] = 0.0f;
    r = cce_lora_apply(&d->lo, x, &tmp);
    if (r != CCE_OK) {
        cce_tensor_free(&tmp);
        return r;
    }
    for (o = 0; o < out; o++) y_accum->data[o] += d->mag[o] * tmp.data[o];
    cce_tensor_free(&tmp);
    return CCE_OK;
}

double cce_dora_train(cce_dora *d, const float *inputs, const float *residuals,
                      size_t n, const cce_lora_train_opts *opt) {
    double mse;
    size_t i, o;
    int out;
    if (!d || !inputs || !residuals || n == 0) return (double)CCE_ERR_INVALID_ARG;
    mse = cce_lora_train(&d->lo, inputs, residuals, n, opt);
    if (mse < 0) return mse;
    out = d->lo.out_dim;
    /* scale mag toward residual RMS per output (gentle) */
    for (o = 0; o < (size_t)out; o++) {
        double acc = 0;
        for (i = 0; i < n; i++) {
            float r = residuals[i * (size_t)out + o];
            acc += (double)r * (double)r;
        }
        acc = sqrt(acc / (double)n);
        d->mag[o] = fmaxf(0.25f, fminf(4.0f, 1.0f + 0.1f * (float)acc));
    }
    return mse;
}
