#ifndef CCE_MAPTRAIN_H
#define CCE_MAPTRAIN_H

/*
 * Hashtable-style parallel map SGD: many same-shape pieces W[E,out,in].
 * GPU (float) when above the offload floor; double always CPU (RDNA4 has
 * no useful fp64 WMMA). Not CERT. Structure/hash stays host.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_MAPTRAIN_OK    0
#define CCE_MAPTRAIN_ERR  -1
#define CCE_MAPTRAIN_FLOOR -2
#define CCE_MAPTRAIN_F32   0
#define CCE_MAPTRAIN_F64   1
#define CCE_MAPTRAIN_VERSION "0.1.0"

typedef struct cce_amdmath cce_amdmath;

const char *cce_maptrain_version(void);

/* W[e,out,in] -= lr * (dY[e,n,out]^T @ X[e,n,in]).
 * gpu may be NULL. dtype = CCE_MAPTRAIN_F32 or _F64.
 * F64 never uses HIP. F32 uses GPU iff gpu && not below floor. */
int cce_maptrain_sgd(cce_amdmath *gpu, int dtype, void *W, const void *X,
                     const void *dY, size_t E, size_t N, size_t in_dim,
                     size_t out_dim, double lr);

#ifdef __cplusplus
}
#endif

#endif
