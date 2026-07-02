#ifndef CCE_TENSOR_H
#define CCE_TENSOR_H

#include "cce_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Row-major tensor view (zero-copy friendly for mmap) */
typedef struct {
    float*   data;                    /* may point into mmap, not owned */
    int      shape[CCE_MAX_DIMS];
    int      ndim;
    size_t   stride[CCE_MAX_DIMS];    /* in elements */
    size_t   numel;                   /* total elements */
    int      owns_memory;             /* 1 = we allocated, 0 = view */
    size_t   alignment;               /* actual alignment of data */
} cce_tensor;

/* Allocate a new owned tensor (aligned) */
cce_result cce_tensor_alloc(cce_tensor* t, const int* shape, int ndim);

/* Create a view into existing memory (no copy, for partial load) */
cce_result cce_tensor_view(cce_tensor* t,
                           float* base,
                           const int* shape, int ndim,
                           const size_t* stride /* optional, NULL = row major */);

/* Free only if owns_memory */
void cce_tensor_free(cce_tensor* t);

/* Zero the tensor */
void cce_tensor_zero(cce_tensor* t);

/* Basic GEMM: C = A @ B  (row major) */
cce_result cce_tensor_matmul(const cce_tensor* a,
                             const cce_tensor* b,
                             cce_tensor* c);

/* Elementwise add: out = a + b */
cce_result cce_tensor_add(const cce_tensor* a, const cce_tensor* b, cce_tensor* out);

/* Layer norm: out = ln(in) * gamma + beta (over last dim) */
cce_result cce_tensor_layer_norm(const cce_tensor* in, const cce_tensor* gamma, const cce_tensor* beta,
                                 float eps, cce_tensor* out);

/* GELU activation (exact) */
cce_result cce_tensor_gelu(const cce_tensor* in, cce_tensor* out);

/* Softmax over last dimension */
cce_result cce_tensor_softmax(const cce_tensor* in, cce_tensor* out);

/* Embedding lookup: for each index, copy row from table into out (out shape [n, dim]) */
cce_result cce_tensor_embed(const cce_tensor* table, const int* indices, int n_indices, cce_tensor* out);

/* Print shape (debug) */
void cce_tensor_print_info(const cce_tensor* t, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* CCE_TENSOR_H */
