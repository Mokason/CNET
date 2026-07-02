#include "../../include/cce/cce_tensor.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free _aligned_free
#else
#define aligned_free free
#endif

static int compute_numel_checked(const int* shape, int ndim, size_t* out_numel) {
    if (!shape || !out_numel) return 0;
    size_t n = 1;
    for (int i = 0; i < ndim; ++i) {
        if (shape[i] <= 0) return 0;
        size_t dim = (size_t)shape[i];
        if (n > SIZE_MAX / dim) return 0;
        n *= dim;
    }
    *out_numel = n;
    return 1;
}

static int compute_strides_checked(size_t* stride, const int* shape, int ndim) {
    if (!stride || !shape || ndim <= 0) return 0;
    stride[ndim - 1] = 1;
    for (int i = ndim - 2; i >= 0; --i) {
        if (shape[i + 1] <= 0) return 0;
        if (stride[i + 1] > SIZE_MAX / (size_t)shape[i + 1]) return 0;
        stride[i] = stride[i + 1] * (size_t)shape[i + 1];
    }
    return 1;
}

cce_result cce_tensor_alloc(cce_tensor* t, const int* shape, int ndim) {
    if (!t || ndim <= 0 || ndim > CCE_MAX_DIMS) return CCE_ERR_INVALID_ARG;
    size_t numel = 0;
    if (!compute_numel_checked(shape, ndim, &numel)) return CCE_ERR_INVALID_ARG;
    if (numel > SIZE_MAX / sizeof(float)) return CCE_ERR_INVALID_ARG;

    memset(t, 0, sizeof(*t));
    memcpy(t->shape, shape, sizeof(int) * ndim);
    t->ndim = ndim;
    t->numel = numel;
    if (!compute_strides_checked(t->stride, shape, ndim)) {
        memset(t, 0, sizeof(*t));
        return CCE_ERR_INVALID_ARG;
    }
    t->alignment = CCE_ALIGN;
    t->owns_memory = 1;

    size_t bytes = t->numel * sizeof(float);
    size_t alloc_bytes = bytes;
    size_t rem = alloc_bytes % CCE_ALIGN;
    if (rem != 0) {
        if (alloc_bytes > SIZE_MAX - (CCE_ALIGN - rem)) return CCE_ERR_INVALID_ARG;
        alloc_bytes += CCE_ALIGN - rem;
    }
    t->data = (float*)aligned_alloc(CCE_ALIGN, alloc_bytes);
    if (!t->data) return CCE_ERR_OOM;

    return CCE_OK;
}

cce_result cce_tensor_view(cce_tensor* t,
                           float* base,
                           const int* shape, int ndim,
                           const size_t* stride) {
    if (!t || !base || ndim <= 0 || ndim > CCE_MAX_DIMS) return CCE_ERR_INVALID_ARG;
    size_t numel = 0;
    if (!compute_numel_checked(shape, ndim, &numel)) return CCE_ERR_INVALID_ARG;

    memset(t, 0, sizeof(*t));
    memcpy(t->shape, shape, sizeof(int) * ndim);
    t->ndim = ndim;
    t->numel = numel;
    t->data = base;
    t->owns_memory = 0;
    t->alignment = 1; /* unknown, assume caller aligned */

    if (stride) {
        for (int i = 0; i < ndim; ++i) {
            if (stride[i] == 0) {
                memset(t, 0, sizeof(*t));
                return CCE_ERR_INVALID_ARG;
            }
        }
        memcpy(t->stride, stride, sizeof(size_t) * ndim);
    } else {
        if (!compute_strides_checked(t->stride, shape, ndim)) {
            memset(t, 0, sizeof(*t));
            return CCE_ERR_INVALID_ARG;
        }
    }
    return CCE_OK;
}

void cce_tensor_free(cce_tensor* t) {
    if (!t) return;
    if (t->owns_memory && t->data) {
        aligned_free(t->data);
    }
    memset(t, 0, sizeof(*t));
}

void cce_tensor_zero(cce_tensor* t) {
    if (t && t->data) {
        memset(t->data, 0, t->numel * sizeof(float));
    }
}

cce_result cce_tensor_matmul(const cce_tensor* a,
                             const cce_tensor* b,
                             cce_tensor* c) {
    if (!a || !b || !c) return CCE_ERR_INVALID_ARG;
    if (a->ndim != 2 || b->ndim != 2) return CCE_ERR_UNSUPPORTED;

    int m = a->shape[0];
    int k = a->shape[1];
    int n = b->shape[1];

    if (b->shape[0] != k || c->shape[0] != m || c->shape[1] != n) {
        return CCE_ERR_INVALID_ARG;
    }

    /* Simple tiled GEMM for better cache behavior (tile size 32 is good for small-medium sizes) */
    const int TILE = 32;

    cce_tensor_zero(c);   /* ensure we start from zero */

    for (int i0 = 0; i0 < m; i0 += TILE) {
        for (int j0 = 0; j0 < n; j0 += TILE) {
            for (int p0 = 0; p0 < k; p0 += TILE) {

                int i_max = (i0 + TILE < m) ? i0 + TILE : m;
                int j_max = (j0 + TILE < n) ? j0 + TILE : n;
                int p_max = (p0 + TILE < k) ? p0 + TILE : k;

                for (int i = i0; i < i_max; ++i) {
                    for (int j = j0; j < j_max; ++j) {
                        float sum = c->data[i * c->stride[0] + j * c->stride[1]];
                        for (int p = p0; p < p_max; ++p) {
                            sum += a->data[i * a->stride[0] + p * a->stride[1]] *
                                   b->data[p * b->stride[0] + j * b->stride[1]];
                        }
                        c->data[i * c->stride[0] + j * c->stride[1]] = sum;
                    }
                }
            }
        }
    }
    return CCE_OK;
}

cce_result cce_tensor_add(const cce_tensor* a, const cce_tensor* b, cce_tensor* out) {
    if (!a || !b || !out || a->numel != b->numel || a->numel != out->numel)
        return CCE_ERR_INVALID_ARG;

    for (size_t i = 0; i < a->numel; ++i) {
        out->data[i] = a->data[i] + b->data[i];
    }
    return CCE_OK;
}

cce_result cce_tensor_layer_norm(const cce_tensor* in, const cce_tensor* gamma, const cce_tensor* beta,
                                 float eps, cce_tensor* out) {
    if (!in || !out || in->numel != out->numel || in->ndim < 1) return CCE_ERR_INVALID_ARG;
    int last_dim = in->shape[in->ndim - 1];
    if (gamma && gamma->numel != (size_t)last_dim) return CCE_ERR_INVALID_ARG;
    if (beta && beta->numel != (size_t)last_dim) return CCE_ERR_INVALID_ARG;

    size_t row_size = (size_t)last_dim;
    size_t num_rows = in->numel / row_size;

    for (size_t r = 0; r < num_rows; ++r) {
        const float* row = in->data + r * row_size;
        float* orow = out->data + r * row_size;

        /* mean */
        float mean = 0.f;
        for (size_t i = 0; i < row_size; ++i) mean += row[i];
        mean /= (float)row_size;

        /* var */
        float var = 0.f;
        for (size_t i = 0; i < row_size; ++i) {
            float d = row[i] - mean;
            var += d * d;
        }
        var /= (float)row_size;
        float inv_std = 1.0f / sqrtf(var + eps);

        for (size_t i = 0; i < row_size; ++i) {
            float n = (row[i] - mean) * inv_std;
            float g = gamma ? gamma->data[i] : 1.0f;
            float b = beta ? beta->data[i] : 0.0f;
            orow[i] = n * g + b;
        }
    }
    return CCE_OK;
}

cce_result cce_tensor_gelu(const cce_tensor* in, cce_tensor* out) {
    if (!in || !out || in->numel != out->numel) return CCE_ERR_INVALID_ARG;
    /* exact gelu approx is fine, or use tanh approx for speed */
    for (size_t i = 0; i < in->numel; ++i) {
        float x = in->data[i];
        /* GELU(x) = 0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715 * x^3))) */
        float x3 = x * x * x;
        float inner = 0.79788456f * (x + 0.044715f * x3);
        out->data[i] = 0.5f * x * (1.0f + tanhf(inner));
    }
    return CCE_OK;
}

cce_result cce_tensor_softmax(const cce_tensor* in, cce_tensor* out) {
    if (!in || !out || in->numel != out->numel || in->ndim < 1) return CCE_ERR_INVALID_ARG;
    int last = in->shape[in->ndim-1];
    size_t rows = in->numel / (size_t)last;

    for (size_t r = 0; r < rows; ++r) {
        const float* row_in = in->data + r * last;
        float* row_out = out->data + r * last;

        float maxv = row_in[0];
        for (int i=1; i<last; i++) if (row_in[i] > maxv) maxv = row_in[i];

        float sum = 0.f;
        for (int i=0; i<last; i++) {
            row_out[i] = expf(row_in[i] - maxv);
            sum += row_out[i];
        }
        float inv = 1.0f / sum;
        for (int i=0; i<last; i++) row_out[i] *= inv;
    }
    return CCE_OK;
}

cce_result cce_tensor_embed(const cce_tensor* table, const int* indices, int n_indices, cce_tensor* out) {
    if (!table || !indices || !out || table->ndim != 2 || n_indices <= 0) return CCE_ERR_INVALID_ARG;
    int dim = table->shape[1];
    if (out->ndim != 2 || out->shape[0] != n_indices || out->shape[1] != dim) return CCE_ERR_INVALID_ARG;

    for (int i = 0; i < n_indices; ++i) {
        int idx = indices[i];
        if (idx < 0 || idx >= table->shape[0]) return CCE_ERR_INVALID_ARG;
        const float* src = table->data + (size_t)idx * dim;
        float* dst = out->data + (size_t)i * dim;
        memcpy(dst, src, (size_t)dim * sizeof(float));
    }
    return CCE_OK;
}

void cce_tensor_print_info(const cce_tensor* t, const char* name) {
    if (!t) return;
    printf("%s: [", name ? name : "tensor");
    for (int i = 0; i < t->ndim; ++i) {
        printf("%d%s", t->shape[i], (i + 1 < t->ndim) ? "x" : "");
    }
    printf("] numel=%zu owns=%d\n", t->numel, t->owns_memory);
}
