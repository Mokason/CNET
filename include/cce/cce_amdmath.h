#ifndef CCE_AMDMATH_H
#define CCE_AMDMATH_H

/*
 * AMD GPU math library for CNET and other C/C++ hosts.
 * =================================================================
 * HIP + rocWMMA + packed VOP (v_pk_*). No Python, no PyTorch.
 *
 * This is NOT cce_hipgemm (forward-only hipBLAS dlopen) and NOT CERT.
 * Hashtable / ROUTE / CAS stay on the CPU. These kernels are dense maps only.
 *
 * Discrete GPUs only. Pin a handle to one device (open_device). Two GPUs =
 * two persistent handles — do not spawn a process per step (~1.2s tax).
 *
 * Row-major: C[m, n] = A[m, k] * B[k, n]
 * Batched experts (same shape): Y[e, n, out] = X[e, n, in] * W[e, in, out]
 *   where W is stored as W[e, out, in] (y = W x) → caller passes W_T or uses
 *   cce_amdmath_linear which takes W[e, out, in].
 *
 * WMMA path is gfx12 16×16×16 (RDNA4). RDNA3 builtins are not used.
 * hipBLAS MFMA (CDNA) is not used.
 *
 * Returns 0 on success, -1 on error, -2 if below the offload floor
 * (caller should keep that work on CPU).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_AMDMATH_OK     0
#define CCE_AMDMATH_ERR   -1
#define CCE_AMDMATH_FLOOR -2

#define CCE_AMDMATH_VERSION "0.3.0"

/* Partial VRAM exposure — only the working set lives on the GPU.
 * HOT  = device-resident (zero H2D on hit)
 * WARM = pinned host; DMA through a VRAM staging slot
 * COLD = pageable host; memcpy every use
 * Hashtable / CERT stay CPU. Evicting HOT writebacks weights (SGD copies). */
#define CCE_AMDMATH_TIER_HOT  1
#define CCE_AMDMATH_TIER_WARM 2
#define CCE_AMDMATH_TIER_COLD 3

typedef struct cce_amdmath cce_amdmath;

typedef struct cce_amdmath_caps {
    int discrete;
    int gfx12;       /* RDNA4 WMMA v2 (_gfx12 builtins) */
    int wavefront;   /* 32 on R9700 */
    int wmma_f16;
    int wmma_bf16;
    int pk_f16;
    int compute_units;
} cce_amdmath_caps;

/* Open first discrete GPU, or a specific index in the discrete set (0-based).
 * name_out optional. NULL if no discrete GPU / HIP missing. */
cce_amdmath *cce_amdmath_open(char *name_out, size_t name_cap);
cce_amdmath *cce_amdmath_open_device(int discrete_index, char *name_out,
                                     size_t name_cap);
void         cce_amdmath_close(cce_amdmath *h);

const char *cce_amdmath_version(void);
const char *cce_amdmath_last_error(const cce_amdmath *h);
int         cce_amdmath_device_count(void); /* discrete only */
int         cce_amdmath_get_caps(const cce_amdmath *h, cce_amdmath_caps *out);

/* 1 = keep on CPU (tiny maps / capsule 8×8). FLOPs = 2*m*n*k. */
int cce_amdmath_below_floor(size_t m, size_t n, size_t k);
size_t cce_amdmath_min_flops(void); /* default 1<<20; env CNET_AMDMATH_MIN_FLOPS */

/* ---- GEMM ---------------------------------------------------------------- */
int cce_amdmath_gemm_f32(cce_amdmath *h, const float *A, const float *B,
                         float *C, size_t m, size_t n, size_t k);

/* Host f32, device converts to bf16, gfx12 WMMA, f32 accum → host f32.
 * This is the fat-train path (fused MLP / FFN). */
int cce_amdmath_gemm_f32_wmma(cce_amdmath *h, const float *A, const float *B,
                              float *C, size_t m, size_t n, size_t k);

/* Host IEEE f16 bits (uint16), WMMA f16→f32 accum, store f32. */
int cce_amdmath_gemm_f16(cce_amdmath *h, const uint16_t *A, const uint16_t *B,
                         float *C, size_t m, size_t n, size_t k);

int cce_amdmath_gemm_bf16(cce_amdmath *h, const uint16_t *A, const uint16_t *B,
                          float *C, size_t m, size_t n, size_t k);

/* Linear y = x W^T with W[out, in] (row-major). Batched over E same-shape
 * experts: X[E,N,in], W[E,out,in], Y[E,N,out]. One launch. */
int cce_amdmath_linear_f32(cce_amdmath *h, const float *X, const float *W,
                           float *Y, size_t E, size_t N, size_t in_dim,
                           size_t out_dim);

int cce_amdmath_linear_f32_wmma(cce_amdmath *h, const float *X, const float *W,
                                float *Y, size_t E, size_t N, size_t in_dim,
                                size_t out_dim);

/* ---- Packed VOP (SSE analog, 2×f16 per VGPR) ----------------------------- */
int cce_amdmath_pk_add_f16(cce_amdmath *h, const uint16_t *a, const uint16_t *b,
                           uint16_t *out, size_t n);
int cce_amdmath_pk_mul_f16(cce_amdmath *h, const uint16_t *a, const uint16_t *b,
                           uint16_t *out, size_t n);
int cce_amdmath_pk_fma_f16(cce_amdmath *h, const uint16_t *a, const uint16_t *b,
                           const uint16_t *c, uint16_t *out, size_t n);

/* ---- SGD on stacked same-shape maps (copies only — does not CERT) -------- */
/* W[e,out,in] -= lr * (dY[e,n,out]^T @ X[e,n,in]) */
int cce_amdmath_sgd_f32(cce_amdmath *h, float *W, const float *X,
                        const float *dY, size_t E, size_t N, size_t in_dim,
                        size_t out_dim, float lr);

/* Naive HIP fp64 SGD. No WMMA on gfx12. Prefer CPU double unless huge. */
int cce_amdmath_sgd_f64(cce_amdmath *h, double *W, const double *X,
                        const double *dY, size_t E, size_t N, size_t in_dim,
                        size_t out_dim, double lr);

/* Device-pointer variants: no H2D/D2H. Caller owns residency. */
int cce_amdmath_linear_f32_dev(cce_amdmath *h, const float *dX, const float *dW,
                               float *dY, size_t E, size_t N, size_t in_dim,
                               size_t out_dim);
int cce_amdmath_sgd_f32_dev(cce_amdmath *h, float *dW, const float *dX,
                            const float *dY, size_t E, size_t N, size_t in_dim,
                            size_t out_dim, float lr);

/* Enqueue on a borrowed hipStream_t, passed as void* to keep the C ABI HIP-free.
 * NULL selects the default stream. Caller owns same-device pointers/stream and
 * keeps them alive until completion. No copies, allocation or synchronization;
 * success means enqueued, NOT completed. Caller must check stream completion.
 * Dimensions must be nonzero, fit the kernel's int indexing/grid limits, and
 * all three tensor byte counts must fit size_t. SGD requires finite lr >= 0.
 * One caller at a time per handle. Existing _dev synchronization is unchanged. */
int cce_amdmath_linear_f32_stream(cce_amdmath *h, const float *dX, const float *dW,
                                float *dY, size_t E, size_t N, size_t in_dim,
                                size_t out_dim, void *stream);
int cce_amdmath_sgd_f32_stream(cce_amdmath *h, float *dW, const float *dX,
                             const float *dY, size_t E, size_t N, size_t in_dim,
                             size_t out_dim, float lr, void *stream);

int cce_amdmath_vram_info(cce_amdmath *h, size_t *free_bytes, size_t *total_bytes);

/* ---- 3-tier pool -------------------------------------------------------- */
typedef struct cce_amdmath_pool cce_amdmath_pool;

typedef struct cce_amdmath_pool_stats {
    size_t hot_used;
    size_t hot_cap;
    size_t warm_used;
    size_t warm_cap;
    size_t vram_free;
    size_t vram_total;
    uint64_t hits_hot;
    uint64_t hits_warm;
    uint64_t hits_cold;
    uint64_t evicts;
    int nslabs;
} cce_amdmath_pool_stats;

cce_amdmath_pool *cce_amdmath_pool_open(cce_amdmath *h, size_t hot_bytes,
                                        size_t warm_bytes);
void cce_amdmath_pool_close(cce_amdmath_pool *p);

/* Bind host buffer `id` (0..n-1 sequential). preferred_tier is a hint;
 * HOT is still gated by hot_cap on touch. */
int cce_amdmath_pool_bind(cce_amdmath_pool *p, int id, float *host, size_t bytes,
                          int preferred_tier);

/* LRU promote into HOT if it fits (evict oldest HOT with D2H writeback). */
int cce_amdmath_pool_touch(cce_amdmath_pool *p, int id);
int cce_amdmath_pool_tier(const cce_amdmath_pool *p, int id);
int cce_amdmath_pool_get_stats(const cce_amdmath_pool *p, cce_amdmath_pool_stats *out);

/* One-expert train step: touch W, SGD on device if HOT else stream. E=1. */
int cce_amdmath_pool_sgd_f32(cce_amdmath_pool *p, int w_id, const float *X,
                             const float *dY, size_t N, size_t in_dim,
                             size_t out_dim, float lr);

#ifdef __cplusplus
}
#endif

#endif /* CCE_AMDMATH_H */
