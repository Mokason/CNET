/* cce_amdmath — AMD GPU math (HIP + rocWMMA + packed VOP).
 *
 * C ABI in include/cce/cce_amdmath.h. Not CERT. Not cce_hipgemm.
 * Build: hipcc --offload-arch=gfx1201 (see mk/amdmath.mk).
 */
#include "cce/cce_amdmath.h"

#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <hip/hip_bfloat16.h>
#include <rocwmma/rocwmma.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <climits>
#include <cmath>

#define WMMA_TILE 16u

struct cce_amdmath {
    int device;
    int gfx12;
    int wave;
    int cu;
    char name[256];
    char arch[64];
    char err[256];
    void *scratch;
    size_t scratch_cap;
};

static void set_err(cce_amdmath *h, const char *msg)
{
    if (!h || !msg)
        return;
    std::snprintf(h->err, sizeof h->err, "%s", msg);
}

static int hip_fail(cce_amdmath *h, hipError_t e, const char *what)
{
    if (e == hipSuccess)
        return 0;
    if (h) {
        std::snprintf(h->err, sizeof h->err, "%s: %s", what, hipGetErrorString(e));
    }
    return -1;
}

static size_t min_flops_env(void)
{
    const char *e = std::getenv("CNET_AMDMATH_MIN_FLOPS");
    if (!e || !e[0])
        return (size_t)1 << 20;
    char *end = NULL;
    unsigned long long v = std::strtoull(e, &end, 10);
    if (end == e)
        return (size_t)1 << 20;
    return (size_t)v;
}

static int ensure_scratch(cce_amdmath *h, size_t bytes)
{
    if (bytes <= h->scratch_cap)
        return 0;
    if (h->scratch) {
        (void)hipFree(h->scratch);
        h->scratch = NULL;
        h->scratch_cap = 0;
    }
    if (hip_fail(h, hipMalloc(&h->scratch, bytes), "hipMalloc scratch"))
        return -1;
    h->scratch_cap = bytes;
    return 0;
}

static int set_dev(cce_amdmath *h)
{
    return hip_fail(h, hipSetDevice(h->device), "hipSetDevice");
}

static int is_integrated(int dev)
{
    int integ = 0;
    if (hipDeviceGetAttribute(&integ, hipDeviceAttributeIntegrated, dev) != hipSuccess)
        return 1;
    return integ;
}

/* Discrete devices in HIP index order. */
static int list_discrete(int *out, int cap)
{
    int ndev = 0;
    if (hipGetDeviceCount(&ndev) != hipSuccess || ndev <= 0)
        return 0;
    int n = 0;
    for (int i = 0; i < ndev && n < cap; i++) {
        if (!is_integrated(i))
            out[n++] = i;
    }
    return n;
}

static cce_amdmath *open_dev(int hip_dev, char *name_out, size_t name_cap)
{
    if (hipSetDevice(hip_dev) != hipSuccess)
        return NULL;
    hipDeviceProp_t prop;
    if (hipGetDeviceProperties(&prop, hip_dev) != hipSuccess)
        return NULL;
    cce_amdmath *h = (cce_amdmath *)std::calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->device = hip_dev;
    h->wave = prop.warpSize;
    h->cu = prop.multiProcessorCount;
    std::snprintf(h->name, sizeof h->name, "%s", prop.name);
    std::snprintf(h->arch, sizeof h->arch, "%s", prop.gcnArchName);
    h->gfx12 = (std::strncmp(prop.gcnArchName, "gfx12", 5) == 0);
    if (name_out && name_cap) {
        std::snprintf(name_out, name_cap, "%s (%s)", h->name, h->arch);
    }
    return h;
}

extern "C" const char *cce_amdmath_version(void)
{
    return CCE_AMDMATH_VERSION;
}

extern "C" const char *cce_amdmath_last_error(const cce_amdmath *h)
{
    return (h && h->err[0]) ? h->err : "";
}

extern "C" int cce_amdmath_device_count(void)
{
    int ids[16];
    return list_discrete(ids, 16);
}

extern "C" cce_amdmath *cce_amdmath_open_device(int discrete_index, char *name_out,
                                                size_t name_cap)
{
    int ids[16];
    int n = list_discrete(ids, 16);
    if (discrete_index < 0 || discrete_index >= n)
        return NULL;
    return open_dev(ids[discrete_index], name_out, name_cap);
}

extern "C" cce_amdmath *cce_amdmath_open(char *name_out, size_t name_cap)
{
    return cce_amdmath_open_device(0, name_out, name_cap);
}

extern "C" void cce_amdmath_close(cce_amdmath *h)
{
    if (!h)
        return;
    if (h->scratch)
        (void)hipFree(h->scratch);
    std::free(h);
}

extern "C" int cce_amdmath_get_caps(const cce_amdmath *h, cce_amdmath_caps *out)
{
    if (!h || !out)
        return CCE_AMDMATH_ERR;
    std::memset(out, 0, sizeof *out);
    out->discrete = 1;
    out->gfx12 = h->gfx12;
    out->wavefront = h->wave;
    out->wmma_f16 = h->gfx12;
    out->wmma_bf16 = h->gfx12;
    out->pk_f16 = 1;
    out->compute_units = h->cu;
    return CCE_AMDMATH_OK;
}

extern "C" size_t cce_amdmath_min_flops(void)
{
    return min_flops_env();
}

extern "C" int cce_amdmath_below_floor(size_t m, size_t n, size_t k)
{
    /* 2*m*n*k FLOPs; also refuse sub-tile WMMA-sized maps that never fill CUs. */
    double flops = 2.0 * (double)m * (double)n * (double)k;
    if (flops < (double)min_flops_env())
        return 1;
    if (m < 16 || n < 16 || k < 16)
        return 1;
    return 0;
}

/* ---------------- kernels ------------------------------------------------ */

__global__ void k_gemm_f32(const float *A, const float *B, float *C, int M, int N,
                           int K)
{
    int n = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    int m = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (m >= M || n >= N)
        return;
    float acc = 0.f;
    const float *a = A + (size_t)m * (size_t)K;
    for (int k = 0; k < K; k++)
        acc = fmaf(a[k], B[(size_t)k * (size_t)N + (size_t)n], acc);
    C[(size_t)m * (size_t)N + (size_t)n] = acc;
}

__global__ void k_linear_f32(const float *X, const float *W, float *Y, int E, int N,
                             int In, int Out)
{
    int o = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    int n = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    int e = (int)blockIdx.z;
    if (e >= E || n >= N || o >= Out)
        return;
    const float *x = X + ((size_t)e * (size_t)N + (size_t)n) * (size_t)In;
    const float *w = W + ((size_t)e * (size_t)Out + (size_t)o) * (size_t)In;
    float acc = 0.f;
    for (int k = 0; k < In; k++)
        acc = fmaf(x[k], w[k], acc);
    Y[((size_t)e * (size_t)N + (size_t)n) * (size_t)Out + (size_t)o] = acc;
}

__global__ void k_sgd_f32(float *W, const float *X, const float *dY, int E, int N,
                          int In, int Out, float lr)
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    int o = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    int e = (int)blockIdx.z;
    if (e >= E || o >= Out || i >= In)
        return;
    float g = 0.f;
    for (int n = 0; n < N; n++) {
        float x = X[((size_t)e * (size_t)N + (size_t)n) * (size_t)In + (size_t)i];
        float dy = dY[((size_t)e * (size_t)N + (size_t)n) * (size_t)Out + (size_t)o];
        g = fmaf(x, dy, g);
    }
    float *w = W + ((size_t)e * (size_t)Out + (size_t)o) * (size_t)In + (size_t)i;
    *w -= lr * g;
}

/* RDNA4 has no fp64 WMMA. Rate-limited scalar FMA — use only if caller
 * insists on double. Hashtable-sized maps should stay CPU. */
__global__ void k_sgd_f64(double *W, const double *X, const double *dY, int E, int N,
                          int In, int Out, double lr)
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    int o = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    int e = (int)blockIdx.z;
    if (e >= E || o >= Out || i >= In)
        return;
    double g = 0.0;
    for (int n = 0; n < N; n++) {
        double x = X[((size_t)e * (size_t)N + (size_t)n) * (size_t)In + (size_t)i];
        double dy = dY[((size_t)e * (size_t)N + (size_t)n) * (size_t)Out + (size_t)o];
        g = fma(x, dy, g);
    }
    double *w = W + ((size_t)e * (size_t)Out + (size_t)o) * (size_t)In + (size_t)i;
    *w -= lr * g;
}

__global__ void k_f32_to_f16(_Float16 *dst, const float *src, int rows, int cols,
                             int dst_ld, int src_ld)
{
    int c = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    int r = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (r >= rows || c >= cols)
        return;
    dst[(size_t)r * (size_t)dst_ld + (size_t)c] = (_Float16)src[(size_t)r * (size_t)src_ld + (size_t)c];
}

__global__ void k_f32_to_bf16(hip_bfloat16 *dst, const float *src, int rows, int cols,
                              int dst_ld, int src_ld)
{
    int c = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    int r = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (r >= rows || c >= cols)
        return;
    dst[(size_t)r * (size_t)dst_ld + (size_t)c] =
        hip_bfloat16(src[(size_t)r * (size_t)src_ld + (size_t)c]);
}

__global__ void k_pk_add(const uint32_t *a, const uint32_t *b, uint32_t *o, size_t n2)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n2)
        return;
    uint32_t r;
    asm volatile("v_pk_add_f16 %0, %1, %2" : "=v"(r) : "v"(a[i]), "v"(b[i]));
    o[i] = r;
}

__global__ void k_pk_mul(const uint32_t *a, const uint32_t *b, uint32_t *o, size_t n2)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n2)
        return;
    uint32_t r;
    asm volatile("v_pk_mul_f16 %0, %1, %2" : "=v"(r) : "v"(a[i]), "v"(b[i]));
    o[i] = r;
}

__global__ void k_pk_fma(const uint32_t *a, const uint32_t *b, const uint32_t *c,
                         uint32_t *o, size_t n2)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n2)
        return;
    uint32_t r;
    asm volatile("v_pk_fma_f16 %0, %1, %2, %3" : "=v"(r) : "v"(a[i]), "v"(b[i]), "v"(c[i]));
    o[i] = r;
}

/* gfx12 WMMA: C[m,n] = A[m,k] @ B[k,n], A/B f16 or bf16, C f32.
 * One warp per 16×16 output tile. M,N,K must be multiples of 16. */
template <typename InT>
__global__ void k_wmma_gemm(const InT *A, const InT *B, float *C, int M, int N, int K)
{
    using namespace rocwmma;
    constexpr uint32_t T = WMMA_TILE;
    const int tiles_n = N / (int)T;
    const int tiles_m = M / (int)T;
    const int warps_per_block = (int)(blockDim.x / 32u);
    const int warp_in_block = (int)(threadIdx.x / 32u);
    const int warp = (int)blockIdx.x * warps_per_block + warp_in_block;
    const int tile_m = warp / tiles_n;
    const int tile_n = warp % tiles_n;
    if (tile_m >= tiles_m)
        return;

    fragment<matrix_a, T, T, T, InT, row_major> a_frag;
    fragment<matrix_b, T, T, T, InT, row_major> b_frag;
    fragment<accumulator, T, T, T, float> c_frag;
    fill_fragment(c_frag, 0.f);

    const InT *a_row = A + (size_t)tile_m * T * (size_t)K;
    const InT *b_col = B + (size_t)tile_n * T;
    for (int k0 = 0; k0 < K; k0 += (int)T) {
        load_matrix_sync(a_frag, a_row + k0, (uint32_t)K);
        load_matrix_sync(b_frag, b_col + (size_t)k0 * (size_t)N, (uint32_t)N);
        mma_sync(c_frag, a_frag, b_frag, c_frag);
    }
    store_matrix_sync(C + (size_t)tile_m * T * (size_t)N + (size_t)tile_n * T, c_frag,
                      (uint32_t)N, mem_row_major);
}

/* Y[e,n,out] = X[e,n,in] @ W[e,out,in]^T  via WMMA (In/N/Out multiples of 16). */
template <typename InT>
__global__ void k_wmma_linear(const InT *X, const InT *W, float *Y, int E, int N, int In,
                              int Out)
{
    using namespace rocwmma;
    constexpr uint32_t T = WMMA_TILE;
    const int tiles_n = N / (int)T;   /* M of GEMM = N_batch */
    const int tiles_o = Out / (int)T; /* N of GEMM = Out */
    const int tiles_eo = tiles_n * tiles_o;
    const int warps_per_block = (int)(blockDim.x / 32u);
    const int warp_in_block = (int)(threadIdx.x / 32u);
    const int warp = (int)blockIdx.x * warps_per_block + warp_in_block;
    const int e = warp / tiles_eo;
    if (e >= E)
        return;
    const int rem = warp % tiles_eo;
    const int tile_n = rem / tiles_o;
    const int tile_o = rem % tiles_o;

    fragment<matrix_a, T, T, T, InT, row_major> a_frag;
    fragment<matrix_b, T, T, T, InT, col_major> b_frag;
    fragment<accumulator, T, T, T, float> c_frag;
    fill_fragment(c_frag, 0.f);

    const InT *Xe = X + (size_t)e * (size_t)N * (size_t)In;
    const InT *We = W + (size_t)e * (size_t)Out * (size_t)In;
    float *Ye = Y + (size_t)e * (size_t)N * (size_t)Out;

    const InT *a_row = Xe + (size_t)tile_n * T * (size_t)In;
    const InT *w_ptr = We + (size_t)tile_o * T * (size_t)In; /* W[out0, 0] */
    for (int k0 = 0; k0 < In; k0 += (int)T) {
        load_matrix_sync(a_frag, a_row + k0, (uint32_t)In);
        load_matrix_sync(b_frag, w_ptr + k0, (uint32_t)In);
        mma_sync(c_frag, a_frag, b_frag, c_frag);
    }
    store_matrix_sync(Ye + (size_t)tile_n * T * (size_t)Out + (size_t)tile_o * T, c_frag,
                      (uint32_t)Out, mem_row_major);
}

/* ---------------- launch helpers ----------------------------------------- */

static dim3 grid2(int x, int y, int bx, int by)
{
    return dim3((unsigned)((x + bx - 1) / bx), (unsigned)((y + by - 1) / by), 1);
}

static int launch_wmma_grid(int tiles)
{
    const int warps_per_block = 4;
    return (tiles + warps_per_block - 1) / warps_per_block;
}

static int gemm_f32_dev(cce_amdmath *h, const float *dA, const float *dB, float *dC, int M,
                        int N, int K)
{
    dim3 block(16, 16);
    dim3 grid = grid2(N, M, 16, 16);
    k_gemm_f32<<<grid, block>>>(dA, dB, dC, M, N, K);
    return hip_fail(h, hipGetLastError(), "k_gemm_f32");
}

extern "C" int cce_amdmath_gemm_f32(cce_amdmath *h, const float *A, const float *B, float *C,
                                    size_t m, size_t n, size_t k)
{
    if (!h || !A || !B || !C || m == 0 || n == 0 || k == 0)
        return CCE_AMDMATH_ERR;
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(m, n, k))
        return CCE_AMDMATH_FLOOR;
    size_t bA = m * k * sizeof(float);
    size_t bB = k * n * sizeof(float);
    size_t bC = m * n * sizeof(float);
    if (ensure_scratch(h, bA + bB + bC))
        return CCE_AMDMATH_ERR;
    float *dA = (float *)h->scratch;
    float *dB = (float *)((char *)h->scratch + bA);
    float *dC = (float *)((char *)h->scratch + bA + bB);
    if (hip_fail(h, hipMemcpy(dA, A, bA, hipMemcpyHostToDevice), "H2D A"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dB, B, bB, hipMemcpyHostToDevice), "H2D B"))
        return CCE_AMDMATH_ERR;
    if (gemm_f32_dev(h, dA, dB, dC, (int)m, (int)n, (int)k))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(C, dC, bC, hipMemcpyDeviceToHost), "D2H C"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

static uint32_t pad16(uint32_t x)
{
    return (x + 15u) & ~15u;
}

static int gemm_wmma_f32host(cce_amdmath *h, const float *A, const float *B, float *C,
                             size_t m, size_t n, size_t k, int use_bf16)
{
    if (!h || !A || !B || !C)
        return CCE_AMDMATH_ERR;
    if (!h->gfx12) {
        set_err(h, "WMMA gfx12 required");
        return CCE_AMDMATH_ERR;
    }
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(m, n, k))
        return CCE_AMDMATH_FLOOR;

    const int Mp = (int)pad16((uint32_t)m);
    const int Np = (int)pad16((uint32_t)n);
    const int Kp = (int)pad16((uint32_t)k);
    const size_t in_sz = (size_t)(use_bf16 ? sizeof(hip_bfloat16) : sizeof(_Float16));
    size_t bA = (size_t)Mp * (size_t)Kp * in_sz;
    size_t bB = (size_t)Kp * (size_t)Np * in_sz;
    size_t bC = (size_t)Mp * (size_t)Np * sizeof(float);
    size_t bAf = (size_t)m * (size_t)k * sizeof(float);
    size_t bBf = (size_t)k * (size_t)n * sizeof(float);
    if (ensure_scratch(h, bAf + bBf + bA + bB + bC))
        return CCE_AMDMATH_ERR;

    char *p = (char *)h->scratch;
    float *dAf = (float *)p;
    p += bAf;
    float *dBf = (float *)p;
    p += bBf;
    void *dA = p;
    p += bA;
    void *dB = p;
    p += bB;
    float *dC = (float *)p;

    if (hip_fail(h, hipMemcpy(dAf, A, bAf, hipMemcpyHostToDevice), "H2D A"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dBf, B, bBf, hipMemcpyHostToDevice), "H2D B"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemset(dA, 0, bA), "zero A"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemset(dB, 0, bB), "zero B"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemset(dC, 0, bC), "zero C"))
        return CCE_AMDMATH_ERR;

    dim3 block(16, 16);
    dim3 gA = grid2((int)k, (int)m, 16, 16);
    dim3 gB = grid2((int)n, (int)k, 16, 16);
    if (use_bf16) {
        k_f32_to_bf16<<<gA, block>>>((hip_bfloat16 *)dA, dAf, (int)m, (int)k, Kp, (int)k);
        k_f32_to_bf16<<<gB, block>>>((hip_bfloat16 *)dB, dBf, (int)k, (int)n, Np, (int)n);
    } else {
        k_f32_to_f16<<<gA, block>>>((_Float16 *)dA, dAf, (int)m, (int)k, Kp, (int)k);
        k_f32_to_f16<<<gB, block>>>((_Float16 *)dB, dBf, (int)k, (int)n, Np, (int)n);
    }
    if (hip_fail(h, hipGetLastError(), "convert"))
        return CCE_AMDMATH_ERR;

    int tiles = (Mp / (int)WMMA_TILE) * (Np / (int)WMMA_TILE);
    dim3 wblock(32 * 4);
    dim3 wgrid((unsigned)launch_wmma_grid(tiles));
    if (use_bf16) {
        k_wmma_gemm<hip_bfloat16>
            <<<wgrid, wblock>>>((hip_bfloat16 *)dA, (hip_bfloat16 *)dB, dC, Mp, Np, Kp);
    } else {
        k_wmma_gemm<_Float16>
            <<<wgrid, wblock>>>((_Float16 *)dA, (_Float16 *)dB, dC, Mp, Np, Kp);
    }
    if (hip_fail(h, hipGetLastError(), "k_wmma_gemm"))
        return CCE_AMDMATH_ERR;

    if (Mp == (int)m && Np == (int)n) {
        if (hip_fail(h, hipMemcpy(C, dC, (size_t)m * n * sizeof(float), hipMemcpyDeviceToHost),
                     "D2H C"))
            return CCE_AMDMATH_ERR;
    } else {
        /* copy rows of width n from padded Mp x Np */
        for (size_t r = 0; r < m; r++) {
            if (hip_fail(h,
                         hipMemcpy(C + r * n, dC + r * (size_t)Np, n * sizeof(float),
                                   hipMemcpyDeviceToHost),
                         "D2H C row"))
                return CCE_AMDMATH_ERR;
        }
    }
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_gemm_f32_wmma(cce_amdmath *h, const float *A, const float *B,
                                         float *C, size_t m, size_t n, size_t k)
{
    return gemm_wmma_f32host(h, A, B, C, m, n, k, /*bf16*/ 1);
}

extern "C" int cce_amdmath_gemm_f16(cce_amdmath *h, const uint16_t *A, const uint16_t *B,
                                    float *C, size_t m, size_t n, size_t k)
{
    if (!h || !A || !B || !C)
        return CCE_AMDMATH_ERR;
    if (!h->gfx12) {
        set_err(h, "WMMA gfx12 required");
        return CCE_AMDMATH_ERR;
    }
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(m, n, k))
        return CCE_AMDMATH_FLOOR;
    if ((m % 16) || (n % 16) || (k % 16)) {
        set_err(h, "gemm_f16 requires m,n,k multiples of 16");
        return CCE_AMDMATH_ERR;
    }
    size_t bA = m * k * sizeof(uint16_t);
    size_t bB = k * n * sizeof(uint16_t);
    size_t bC = m * n * sizeof(float);
    if (ensure_scratch(h, bA + bB + bC))
        return CCE_AMDMATH_ERR;
    _Float16 *dA = (_Float16 *)h->scratch;
    _Float16 *dB = (_Float16 *)((char *)h->scratch + bA);
    float *dC = (float *)((char *)h->scratch + bA + bB);
    if (hip_fail(h, hipMemcpy(dA, A, bA, hipMemcpyHostToDevice), "H2D A"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dB, B, bB, hipMemcpyHostToDevice), "H2D B"))
        return CCE_AMDMATH_ERR;
    int tiles = (int)((m / 16) * (n / 16));
    k_wmma_gemm<_Float16>
        <<<dim3((unsigned)launch_wmma_grid(tiles)), dim3(32 * 4)>>>(dA, dB, dC, (int)m,
                                                                    (int)n, (int)k);
    if (hip_fail(h, hipGetLastError(), "k_wmma_gemm f16"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(C, dC, bC, hipMemcpyDeviceToHost), "D2H C"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_gemm_bf16(cce_amdmath *h, const uint16_t *A, const uint16_t *B,
                                     float *C, size_t m, size_t n, size_t k)
{
    if (!h || !A || !B || !C)
        return CCE_AMDMATH_ERR;
    if (!h->gfx12) {
        set_err(h, "WMMA gfx12 required");
        return CCE_AMDMATH_ERR;
    }
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(m, n, k))
        return CCE_AMDMATH_FLOOR;
    if ((m % 16) || (n % 16) || (k % 16)) {
        set_err(h, "gemm_bf16 requires m,n,k multiples of 16");
        return CCE_AMDMATH_ERR;
    }
    size_t bA = m * k * sizeof(uint16_t);
    size_t bB = k * n * sizeof(uint16_t);
    size_t bC = m * n * sizeof(float);
    if (ensure_scratch(h, bA + bB + bC))
        return CCE_AMDMATH_ERR;
    hip_bfloat16 *dA = (hip_bfloat16 *)h->scratch;
    hip_bfloat16 *dB = (hip_bfloat16 *)((char *)h->scratch + bA);
    float *dC = (float *)((char *)h->scratch + bA + bB);
    if (hip_fail(h, hipMemcpy(dA, A, bA, hipMemcpyHostToDevice), "H2D A"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dB, B, bB, hipMemcpyHostToDevice), "H2D B"))
        return CCE_AMDMATH_ERR;
    int tiles = (int)((m / 16) * (n / 16));
    k_wmma_gemm<hip_bfloat16>
        <<<dim3((unsigned)launch_wmma_grid(tiles)), dim3(32 * 4)>>>(dA, dB, dC, (int)m,
                                                                    (int)n, (int)k);
    if (hip_fail(h, hipGetLastError(), "k_wmma_gemm bf16"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(C, dC, bC, hipMemcpyDeviceToHost), "D2H C"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_linear_f32(cce_amdmath *h, const float *X, const float *W, float *Y,
                                      size_t E, size_t N, size_t in_dim, size_t out_dim)
{
    if (!h || !X || !W || !Y || E == 0 || N == 0 || in_dim == 0 || out_dim == 0)
        return CCE_AMDMATH_ERR;
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(N, out_dim, in_dim))
        return CCE_AMDMATH_FLOOR;
    size_t bX = E * N * in_dim * sizeof(float);
    size_t bW = E * out_dim * in_dim * sizeof(float);
    size_t bY = E * N * out_dim * sizeof(float);
    if (ensure_scratch(h, bX + bW + bY))
        return CCE_AMDMATH_ERR;
    float *dX = (float *)h->scratch;
    float *dW = (float *)((char *)h->scratch + bX);
    float *dY = (float *)((char *)h->scratch + bX + bW);
    if (hip_fail(h, hipMemcpy(dX, X, bX, hipMemcpyHostToDevice), "H2D X"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dW, W, bW, hipMemcpyHostToDevice), "H2D W"))
        return CCE_AMDMATH_ERR;
    dim3 block(16, 16);
    dim3 grid((unsigned)((out_dim + 15) / 16), (unsigned)((N + 15) / 16), (unsigned)E);
    k_linear_f32<<<grid, block>>>(dX, dW, dY, (int)E, (int)N, (int)in_dim, (int)out_dim);
    if (hip_fail(h, hipGetLastError(), "k_linear_f32"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(Y, dY, bY, hipMemcpyDeviceToHost), "D2H Y"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_linear_f32_wmma(cce_amdmath *h, const float *X, const float *W,
                                           float *Y, size_t E, size_t N, size_t in_dim,
                                           size_t out_dim)
{
    if (!h || !X || !W || !Y)
        return CCE_AMDMATH_ERR;
    if (!h->gfx12) {
        set_err(h, "WMMA gfx12 required");
        return CCE_AMDMATH_ERR;
    }
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(N, out_dim, in_dim))
        return CCE_AMDMATH_FLOOR;
    if ((N % 16) || (in_dim % 16) || (out_dim % 16)) {
        set_err(h, "linear_f32_wmma needs N,in,out multiples of 16");
        return CCE_AMDMATH_ERR;
    }
    size_t bXf = E * N * in_dim * sizeof(float);
    size_t bWf = E * out_dim * in_dim * sizeof(float);
    size_t bX = E * N * in_dim * sizeof(hip_bfloat16);
    size_t bW = E * out_dim * in_dim * sizeof(hip_bfloat16);
    size_t bY = E * N * out_dim * sizeof(float);
    if (ensure_scratch(h, bXf + bWf + bX + bW + bY))
        return CCE_AMDMATH_ERR;
    char *p = (char *)h->scratch;
    float *dXf = (float *)p;
    p += bXf;
    float *dWf = (float *)p;
    p += bWf;
    hip_bfloat16 *dX = (hip_bfloat16 *)p;
    p += bX;
    hip_bfloat16 *dW = (hip_bfloat16 *)p;
    p += bW;
    float *dY = (float *)p;
    if (hip_fail(h, hipMemcpy(dXf, X, bXf, hipMemcpyHostToDevice), "H2D X"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dWf, W, bWf, hipMemcpyHostToDevice), "H2D W"))
        return CCE_AMDMATH_ERR;
    dim3 block(16, 16);
    k_f32_to_bf16<<<grid2((int)in_dim, (int)(E * N), 16, 16), block>>>(
        dX, dXf, (int)(E * N), (int)in_dim, (int)in_dim, (int)in_dim);
    k_f32_to_bf16<<<grid2((int)in_dim, (int)(E * out_dim), 16, 16), block>>>(
        dW, dWf, (int)(E * out_dim), (int)in_dim, (int)in_dim, (int)in_dim);
    if (hip_fail(h, hipGetLastError(), "convert linear"))
        return CCE_AMDMATH_ERR;
    int tiles = (int)E * (int)(N / 16) * (int)(out_dim / 16);
    k_wmma_linear<hip_bfloat16><<<dim3((unsigned)launch_wmma_grid(tiles)), dim3(32 * 4)>>>(
        dX, dW, dY, (int)E, (int)N, (int)in_dim, (int)out_dim);
    if (hip_fail(h, hipGetLastError(), "k_wmma_linear"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(Y, dY, bY, hipMemcpyDeviceToHost), "D2H Y"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_sgd_f32(cce_amdmath *h, float *W, const float *X, const float *dY,
                                   size_t E, size_t N, size_t in_dim, size_t out_dim, float lr)
{
    if (!h || !W || !X || !dY)
        return CCE_AMDMATH_ERR;
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(out_dim, in_dim, N))
        return CCE_AMDMATH_FLOOR;
    size_t bW = E * out_dim * in_dim * sizeof(float);
    size_t bX = E * N * in_dim * sizeof(float);
    size_t bY = E * N * out_dim * sizeof(float);
    if (ensure_scratch(h, bW + bX + bY))
        return CCE_AMDMATH_ERR;
    float *dW = (float *)h->scratch;
    float *dX = (float *)((char *)h->scratch + bW);
    float *ddY = (float *)((char *)h->scratch + bW + bX);
    if (hip_fail(h, hipMemcpy(dW, W, bW, hipMemcpyHostToDevice), "H2D W"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dX, X, bX, hipMemcpyHostToDevice), "H2D X"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(ddY, dY, bY, hipMemcpyHostToDevice), "H2D dY"))
        return CCE_AMDMATH_ERR;
    dim3 block(16, 16);
    dim3 grid((unsigned)((in_dim + 15) / 16), (unsigned)((out_dim + 15) / 16), (unsigned)E);
    k_sgd_f32<<<grid, block>>>(dW, dX, ddY, (int)E, (int)N, (int)in_dim, (int)out_dim, lr);
    if (hip_fail(h, hipGetLastError(), "k_sgd_f32"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(W, dW, bW, hipMemcpyDeviceToHost), "D2H W"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_sgd_f64(cce_amdmath *h, double *W, const double *X, const double *dY,
                                   size_t E, size_t N, size_t in_dim, size_t out_dim, double lr)
{
    if (!h || !W || !X || !dY)
        return CCE_AMDMATH_ERR;
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_below_floor(out_dim, in_dim, N))
        return CCE_AMDMATH_FLOOR;
    size_t bW = E * out_dim * in_dim * sizeof(double);
    size_t bX = E * N * in_dim * sizeof(double);
    size_t bY = E * N * out_dim * sizeof(double);
    if (ensure_scratch(h, bW + bX + bY))
        return CCE_AMDMATH_ERR;
    double *dW = (double *)h->scratch;
    double *dX = (double *)((char *)h->scratch + bW);
    double *ddY = (double *)((char *)h->scratch + bW + bX);
    if (hip_fail(h, hipMemcpy(dW, W, bW, hipMemcpyHostToDevice), "H2D W64"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(dX, X, bX, hipMemcpyHostToDevice), "H2D X64"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(ddY, dY, bY, hipMemcpyHostToDevice), "H2D dY64"))
        return CCE_AMDMATH_ERR;
    dim3 block(16, 16);
    dim3 grid((unsigned)((in_dim + 15) / 16), (unsigned)((out_dim + 15) / 16), (unsigned)E);
    k_sgd_f64<<<grid, block>>>(dW, dX, ddY, (int)E, (int)N, (int)in_dim, (int)out_dim, lr);
    if (hip_fail(h, hipGetLastError(), "k_sgd_f64"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipDeviceSynchronize(), "sgd_f64 sync"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(W, dW, bW, hipMemcpyDeviceToHost), "D2H W64"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

static int pk_binop(cce_amdmath *h, const uint16_t *a, const uint16_t *b, const uint16_t *c,
                    uint16_t *out, size_t n, int which)
{
    if (!h || !a || !b || !out || n == 0)
        return CCE_AMDMATH_ERR;
    if (which == 2 && !c)
        return CCE_AMDMATH_ERR;
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    size_t n2 = n / 2;
    size_t bytes = n * sizeof(uint16_t);
    size_t packed = n2 * sizeof(uint32_t);
    size_t need = packed * ((which == 2) ? 4 : 3);
    if (n2 == 0) {
        /* one leftover half — do on host via convert */
        float fa, fb, fc = 0.f;
        std::memcpy(&fa, a, 0); /* silence unused on some paths */
        (void)fa;
        /* scalar leftover below */
    }
    if (ensure_scratch(h, need + 64))
        return CCE_AMDMATH_ERR;
    uint32_t *dA = (uint32_t *)h->scratch;
    uint32_t *dB = dA + n2;
    uint32_t *dC = dB + n2;
    uint32_t *dO = (which == 2) ? (dC + n2) : (dB + n2);
    if (n2) {
        if (hip_fail(h, hipMemcpy(dA, a, packed, hipMemcpyHostToDevice), "H2D a"))
            return CCE_AMDMATH_ERR;
        if (hip_fail(h, hipMemcpy(dB, b, packed, hipMemcpyHostToDevice), "H2D b"))
            return CCE_AMDMATH_ERR;
        if (which == 2) {
            if (hip_fail(h, hipMemcpy(dC, c, packed, hipMemcpyHostToDevice), "H2D c"))
                return CCE_AMDMATH_ERR;
        }
        unsigned threads = 256;
        unsigned blocks = (unsigned)((n2 + threads - 1) / threads);
        if (which == 0)
            k_pk_add<<<blocks, threads>>>(dA, dB, dO, n2);
        else if (which == 1)
            k_pk_mul<<<blocks, threads>>>(dA, dB, dO, n2);
        else
            k_pk_fma<<<blocks, threads>>>(dA, dB, dC, dO, n2);
        if (hip_fail(h, hipGetLastError(), "pk kernel"))
            return CCE_AMDMATH_ERR;
        if (hip_fail(h, hipMemcpy(out, dO, packed, hipMemcpyDeviceToHost), "D2H pk"))
            return CCE_AMDMATH_ERR;
    }
    if (n & 1) {
        /* leftover scalar on host using half math */
        auto h2f = [](uint16_t u) -> float {
            __half h;
            std::memcpy(&h, &u, 2);
            return __half2float(h);
        };
        auto f2h = [](float f) -> uint16_t {
            __half h = __float2half(f);
            uint16_t u;
            std::memcpy(&u, &h, 2);
            return u;
        };
        float fa = h2f(a[n - 1]);
        float fb = h2f(b[n - 1]);
        float r;
        if (which == 0)
            r = fa + fb;
        else if (which == 1)
            r = fa * fb;
        else
            r = fa * fb + h2f(c[n - 1]);
        out[n - 1] = f2h(r);
        (void)bytes;
    }
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_pk_add_f16(cce_amdmath *h, const uint16_t *a, const uint16_t *b,
                                      uint16_t *out, size_t n)
{
    return pk_binop(h, a, b, NULL, out, n, 0);
}

extern "C" int cce_amdmath_pk_mul_f16(cce_amdmath *h, const uint16_t *a, const uint16_t *b,
                                      uint16_t *out, size_t n)
{
    return pk_binop(h, a, b, NULL, out, n, 1);
}

extern "C" int cce_amdmath_pk_fma_f16(cce_amdmath *h, const uint16_t *a, const uint16_t *b,
                                      const uint16_t *c, uint16_t *out, size_t n)
{
    return pk_binop(h, a, b, c, out, n, 2);
}

/* ---------------- device-pointer kernels + 3-tier pool ------------------- */

extern "C" int cce_amdmath_vram_info(cce_amdmath *h, size_t *free_bytes, size_t *total_bytes)
{
    if (!h)
        return CCE_AMDMATH_ERR;
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    size_t fr = 0, tot = 0;
    if (hip_fail(h, hipMemGetInfo(&fr, &tot), "hipMemGetInfo"))
        return CCE_AMDMATH_ERR;
    if (free_bytes)
        *free_bytes = fr;
    if (total_bytes)
        *total_bytes = tot;
    return CCE_AMDMATH_OK;
}

static bool device_shape(cce_amdmath *h, size_t E, size_t N, size_t in_dim, size_t out_dim, bool sgd)
{
    const size_t limit = SIZE_MAX / sizeof(float);
    if (!E || !N || !in_dim || !out_dim || E > 65535 || N > (sgd ? INT_MAX : INT_MAX - 15u) ||
        out_dim > INT_MAX - 15u || in_dim > (sgd ? INT_MAX - 15u : INT_MAX) ||
        (sgd ? out_dim : N) > 65535u * 16u ||
        E > limit / N / in_dim || E > limit / N / out_dim || E > limit / out_dim / in_dim) {
        set_err(h, "invalid/overflowing device matrix shape");
        return false;
    }
    return true;
}

extern "C" int cce_amdmath_linear_f32_stream(cce_amdmath *h, const float *dX, const float *dW,
                                            float *dY, size_t E, size_t N, size_t in_dim,
                                            size_t out_dim, void *stream)
{
    if (!h || !dX || !dW || !dY || !device_shape(h, E, N, in_dim, out_dim, false))
        return CCE_AMDMATH_ERR;
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    dim3 block(16, 16);
    dim3 grid((unsigned)((out_dim + 15) / 16), (unsigned)((N + 15) / 16), (unsigned)E);
    k_linear_f32<<<grid, block, 0, (hipStream_t)stream>>>(dX, dW, dY, (int)E, (int)N, (int)in_dim, (int)out_dim);
    return hip_fail(h, hipGetLastError(), "k_linear_f32_dev") ? CCE_AMDMATH_ERR : CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_sgd_f32_stream(cce_amdmath *h, float *dW, const float *dX,
                                         const float *dY, size_t E, size_t N, size_t in_dim,
                                         size_t out_dim, float lr, void *stream)
{
    if (!h || !dW || !dX || !dY || !device_shape(h, E, N, in_dim, out_dim, true))
        return CCE_AMDMATH_ERR;
    if (!std::isfinite(lr) || lr < 0) {
        set_err(h, "invalid device SGD learning rate");
        return CCE_AMDMATH_ERR;
    }
    if (set_dev(h))
        return CCE_AMDMATH_ERR;
    dim3 block(16, 16);
    dim3 grid((unsigned)((in_dim + 15) / 16), (unsigned)((out_dim + 15) / 16), (unsigned)E);
    k_sgd_f32<<<grid, block, 0, (hipStream_t)stream>>>(dW, dX, dY, (int)E, (int)N, (int)in_dim, (int)out_dim, lr);
    if (hip_fail(h, hipGetLastError(), "k_sgd_f32_dev"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_linear_f32_dev(cce_amdmath *h, const float *dX, const float *dW,
                                        float *dY, size_t E, size_t N, size_t in_dim,
                                        size_t out_dim)
{
    return cce_amdmath_linear_f32_stream(h, dX, dW, dY, E, N, in_dim, out_dim, nullptr);
}

extern "C" int cce_amdmath_sgd_f32_dev(cce_amdmath *h, float *dW, const float *dX,
                                     const float *dY, size_t E, size_t N, size_t in_dim,
                                     size_t out_dim, float lr)
{
    /* Legacy callers may use a finite negative learning rate. The additive
     * stream API has a stricter contract; do not change this existing behavior. */
    if (!h || !dW || !dX || !dY || !device_shape(h, E, N, in_dim, out_dim, true) ||
        !std::isfinite(lr) || set_dev(h))
        return CCE_AMDMATH_ERR;
    dim3 block(16, 16);
    dim3 grid((unsigned)((in_dim + 15) / 16), (unsigned)((out_dim + 15) / 16), (unsigned)E);
    k_sgd_f32<<<grid, block>>>(dW, dX, dY, (int)E, (int)N, (int)in_dim, (int)out_dim, lr);
    if (hip_fail(h, hipGetLastError(), "k_sgd_f32_dev"))
        return CCE_AMDMATH_ERR;
    return hip_fail(h, hipDeviceSynchronize(), "sgd sync") ? CCE_AMDMATH_ERR : CCE_AMDMATH_OK;
}

#define CCE_AMDMATH_MAX_SLABS 1024

struct CceSlab {
    int id;
    int tier;
    int preferred;
    int registered;
    float *host;
    float *dev;
    size_t bytes;
    uint64_t tick;
};

struct cce_amdmath_pool {
    cce_amdmath *h;
    size_t hot_cap;
    size_t warm_cap;
    size_t hot_used;
    size_t warm_used;
    uint64_t tick;
    uint64_t hits_hot, hits_warm, hits_cold, evicts;
    int n;
    CceSlab slabs[CCE_AMDMATH_MAX_SLABS];
    float *stage;
    size_t stage_cap;
};

extern "C" cce_amdmath_pool *cce_amdmath_pool_open(cce_amdmath *h, size_t hot_bytes,
                                                   size_t warm_bytes)
{
    if (!h)
        return NULL;
    cce_amdmath_pool *p = (cce_amdmath_pool *)std::calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->h = h;
    p->hot_cap = hot_bytes;
    p->warm_cap = warm_bytes;
    return p;
}

static void slab_drop_dev(cce_amdmath_pool *p, CceSlab *s)
{
    if (!s->dev)
        return;
    if (p->hot_used >= s->bytes)
        p->hot_used -= s->bytes;
    else
        p->hot_used = 0;
    (void)hipFree(s->dev);
    s->dev = NULL;
    if (s->tier == CCE_AMDMATH_TIER_HOT)
        s->tier = s->registered ? CCE_AMDMATH_TIER_WARM : CCE_AMDMATH_TIER_COLD;
}

extern "C" void cce_amdmath_pool_close(cce_amdmath_pool *p)
{
    if (!p)
        return;
    if (p->h)
        (void)set_dev(p->h);
    for (int i = 0; i < p->n; i++) {
        CceSlab *s = &p->slabs[i];
        if (s->dev && s->host && s->bytes)
            (void)hipMemcpy(s->host, s->dev, s->bytes, hipMemcpyDeviceToHost);
        if (s->dev)
            (void)hipFree(s->dev);
        if (s->registered && s->host)
            (void)hipHostUnregister(s->host);
    }
    if (p->stage)
        (void)hipFree(p->stage);
    std::free(p);
}

extern "C" int cce_amdmath_pool_bind(cce_amdmath_pool *p, int id, float *host, size_t bytes,
                                     int preferred_tier)
{
    if (!p || !host || bytes == 0 || id < 0 || id >= CCE_AMDMATH_MAX_SLABS)
        return CCE_AMDMATH_ERR;
    if (set_dev(p->h))
        return CCE_AMDMATH_ERR;
    if (id >= p->n) {
        for (int i = p->n; i <= id; i++) {
            p->slabs[i].id = i;
            p->slabs[i].tier = 0;
        }
        p->n = id + 1;
    }
    CceSlab *s = &p->slabs[id];
    if (s->dev)
        slab_drop_dev(p, s);
    if (s->registered && s->host) {
        (void)hipHostUnregister(s->host);
        if (p->warm_used >= s->bytes)
            p->warm_used -= s->bytes;
        else
            p->warm_used = 0;
        s->registered = 0;
    }
    s->host = host;
    s->bytes = bytes;
    s->preferred = preferred_tier ? preferred_tier : CCE_AMDMATH_TIER_COLD;
    s->registered = 0;
    s->tier = CCE_AMDMATH_TIER_COLD;
    s->tick = 0;
    if ((s->preferred == CCE_AMDMATH_TIER_WARM || s->preferred == CCE_AMDMATH_TIER_HOT) &&
        p->warm_used + bytes <= p->warm_cap) {
        if (hipHostRegister(host, bytes, hipHostRegisterDefault) == hipSuccess) {
            s->registered = 1;
            s->tier = CCE_AMDMATH_TIER_WARM;
            p->warm_used += bytes;
        }
    }
    return CCE_AMDMATH_OK;
}

static CceSlab *lru_hot(cce_amdmath_pool *p)
{
    CceSlab *best = NULL;
    for (int i = 0; i < p->n; i++) {
        CceSlab *s = &p->slabs[i];
        if (s->tier != CCE_AMDMATH_TIER_HOT || !s->dev)
            continue;
        if (!best || s->tick < best->tick)
            best = s;
    }
    return best;
}

static int ensure_stage(cce_amdmath_pool *p, size_t bytes)
{
    if (bytes <= p->stage_cap)
        return 0;
    if (p->stage) {
        (void)hipFree(p->stage);
        p->stage = NULL;
        p->stage_cap = 0;
    }
    if (hip_fail(p->h, hipMalloc((void **)&p->stage, bytes), "stage malloc"))
        return -1;
    p->stage_cap = bytes;
    return 0;
}

static int evict_one(cce_amdmath_pool *p)
{
    CceSlab *s = lru_hot(p);
    if (!s)
        return -1;
    if (hip_fail(p->h, hipMemcpy(s->host, s->dev, s->bytes, hipMemcpyDeviceToHost),
                 "evict D2H"))
        return -1;
    slab_drop_dev(p, s);
    p->evicts++;
    return 0;
}

extern "C" int cce_amdmath_pool_touch(cce_amdmath_pool *p, int id)
{
    if (!p || id < 0 || id >= p->n)
        return CCE_AMDMATH_ERR;
    if (set_dev(p->h))
        return CCE_AMDMATH_ERR;
    CceSlab *s = &p->slabs[id];
    if (!s->host || s->bytes == 0)
        return CCE_AMDMATH_ERR;
    p->tick++;
    s->tick = p->tick;
    if (s->tier == CCE_AMDMATH_TIER_HOT && s->dev) {
        p->hits_hot++;
        return CCE_AMDMATH_OK;
    }
    if (s->bytes > p->hot_cap) {
        /* never fits — stay warm/cold */
        if (s->tier == CCE_AMDMATH_TIER_WARM)
            p->hits_warm++;
        else
            p->hits_cold++;
        return CCE_AMDMATH_OK;
    }
    while (p->hot_used + s->bytes > p->hot_cap) {
        if (evict_one(p))
            break;
    }
    if (p->hot_used + s->bytes > p->hot_cap) {
        if (s->tier == CCE_AMDMATH_TIER_WARM)
            p->hits_warm++;
        else
            p->hits_cold++;
        return CCE_AMDMATH_OK;
    }
    float *d = NULL;
    if (hip_fail(p->h, hipMalloc((void **)&d, s->bytes), "hot malloc"))
        return CCE_AMDMATH_ERR;
    hipMemcpyKind kind = s->registered ? hipMemcpyHostToDevice : hipMemcpyHostToDevice;
    if (hip_fail(p->h, hipMemcpy(d, s->host, s->bytes, kind), "hot H2D")) {
        (void)hipFree(d);
        return CCE_AMDMATH_ERR;
    }
    s->dev = d;
    s->tier = CCE_AMDMATH_TIER_HOT;
    p->hot_used += s->bytes;
    p->hits_hot++;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_pool_tier(const cce_amdmath_pool *p, int id)
{
    if (!p || id < 0 || id >= p->n)
        return 0;
    return p->slabs[id].tier;
}

extern "C" int cce_amdmath_pool_get_stats(const cce_amdmath_pool *p, cce_amdmath_pool_stats *out)
{
    if (!p || !out)
        return CCE_AMDMATH_ERR;
    std::memset(out, 0, sizeof *out);
    out->hot_used = p->hot_used;
    out->hot_cap = p->hot_cap;
    out->warm_used = p->warm_used;
    out->warm_cap = p->warm_cap;
    out->hits_hot = p->hits_hot;
    out->hits_warm = p->hits_warm;
    out->hits_cold = p->hits_cold;
    out->evicts = p->evicts;
    out->nslabs = p->n;
    size_t fr = 0, tot = 0;
    if (p->h && hipSetDevice(p->h->device) == hipSuccess)
        (void)hipMemGetInfo(&fr, &tot);
    out->vram_free = fr;
    out->vram_total = tot;
    return CCE_AMDMATH_OK;
}

extern "C" int cce_amdmath_pool_sgd_f32(cce_amdmath_pool *p, int w_id, const float *X,
                                        const float *dY, size_t N, size_t in_dim,
                                        size_t out_dim, float lr)
{
    if (!p || !X || !dY)
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_pool_touch(p, w_id))
        return CCE_AMDMATH_ERR;
    CceSlab *s = &p->slabs[w_id];
    size_t bX = N * in_dim * sizeof(float);
    size_t bY = N * out_dim * sizeof(float);
    cce_amdmath *h = p->h;

    if (s->tier == CCE_AMDMATH_TIER_HOT && s->dev) {
        if (ensure_scratch(h, bX + bY))
            return CCE_AMDMATH_ERR;
        float *dX = (float *)h->scratch;
        float *ddY = (float *)((char *)h->scratch + bX);
        if (hip_fail(h, hipMemcpy(dX, X, bX, hipMemcpyHostToDevice), "H2D X"))
            return CCE_AMDMATH_ERR;
        if (hip_fail(h, hipMemcpy(ddY, dY, bY, hipMemcpyHostToDevice), "H2D dY"))
            return CCE_AMDMATH_ERR;
        return cce_amdmath_sgd_f32_dev(h, s->dev, dX, ddY, 1, N, in_dim, out_dim, lr);
    }

    /* stream W through staging slot (WARM = pinned DMA, COLD = pageable) */
    if (ensure_stage(p, s->bytes))
        return CCE_AMDMATH_ERR;
    if (ensure_scratch(h, bX + bY))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(p->stage, s->host, s->bytes, hipMemcpyHostToDevice), "stream W"))
        return CCE_AMDMATH_ERR;
    float *dX = (float *)h->scratch;
    float *ddY = (float *)((char *)h->scratch + bX);
    if (hip_fail(h, hipMemcpy(dX, X, bX, hipMemcpyHostToDevice), "H2D X"))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(ddY, dY, bY, hipMemcpyHostToDevice), "H2D dY"))
        return CCE_AMDMATH_ERR;
    if (cce_amdmath_sgd_f32_dev(h, p->stage, dX, ddY, 1, N, in_dim, out_dim, lr))
        return CCE_AMDMATH_ERR;
    if (hip_fail(h, hipMemcpy(s->host, p->stage, s->bytes, hipMemcpyDeviceToHost), "stream D2H W"))
        return CCE_AMDMATH_ERR;
    return CCE_AMDMATH_OK;
}
