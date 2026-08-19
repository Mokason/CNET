/* FORWARD ONLY.
 *
 * This backend implements the inference GEMM and nothing else: no backward
 * pass, no straight-through estimator, no gradient path. cce_clgemm.c (OpenCL)
 * is the ONLY backend that carries those, so every training and QAT campaign
 * runs there or not at all -- this file cannot stand in for it.
 *
 * The three *gemm.c files sit side by side and read as interchangeable. They
 * are not, and the difference is load-bearing when scheduling a campaign
 * against a machine. Checkable directly:
 *     grep -coiE 'backward|_bwd|ste_|grad' src/cce/cce_*gemm.c
 */
/* Pure-C cuBLAS multi-GPU linear seam for CNET (dlopen CUDA).
 * Peer of cce_hipgemm.c — same structure, same matmul semantics, same env
 * knobs. No CUDA toolkit / headers / nvcc required to build: all CUDA and
 * cuBLAS entry points are resolved at runtime via dlopen. When the CUDA runtime
 * or driver is absent, open() returns NULL and the caller uses the CPU path.
 *
 * FORWARD ONLY. This backend implements the inference GEMM and nothing else:
 * there is no backward pass, no straight-through estimator, no gradient path.
 * cce_clgemm.c (OpenCL) is the ONLY backend that carries those, so every
 * training and QAT campaign runs there or not at all -- this file cannot serve
 * as a fallback for one. The three *gemm.c files sit side by side and look
 * interchangeable; they are not. Checkable with:
 *     grep -coiE 'backward|_bwd|ste_|grad' src/cce/cce_*gemm.c
 */
#include "../../include/cce/cce_cudagemm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE cce_dl;
static cce_dl cce_dl_open(const char *n) { return LoadLibraryA(n); }
static void *cce_dl_sym(cce_dl h, const char *n) {
    return (void *)GetProcAddress(h, n);
}
static void cce_dl_close(cce_dl h) { if (h) FreeLibrary(h); }
#else
#include <dlfcn.h>
typedef void *cce_dl;
static cce_dl cce_dl_open(const char *n) { return dlopen(n, RTLD_NOW); }
static void *cce_dl_sym(cce_dl h, const char *n) { return dlsym(h, n); }
static void cce_dl_close(cce_dl h) { if (h) dlclose(h); }
#endif

/* Minimal CUDA / cuBLAS declarations — no SDK required at compile time. */
typedef int cudaError_t;                 /* cudaSuccess == 0 */
typedef int cublasStatus_t;              /* CUBLAS_STATUS_SUCCESS == 0 */
typedef int cudaDeviceAttr;
typedef enum {
    cudaMemcpyHostToDevice = 1,
    cudaMemcpyDeviceToHost = 2
} cudaMemcpyKind;
typedef enum {
    CUBLAS_OP_N = 0,
    CUBLAS_OP_T = 1
} cublasOperation_t;
#define CUDA_SUCCESS_OK 0
#define CUBLAS_STATUS_SUCCESS_OK 0
/* cudaDevAttrIntegrated is 18 in the CUDA runtime attribute enum. */
#define cudaDevAttrIntegrated 18

typedef struct cublasContext *cublasHandle_t;

typedef cudaError_t (*p_cudaGetDeviceCount)(int *);
typedef cudaError_t (*p_cudaSetDevice)(int);
typedef cudaError_t (*p_cudaDeviceGetAttribute)(int *, cudaDeviceAttr, int);
typedef cudaError_t (*p_cudaMalloc)(void **, size_t);
typedef cudaError_t (*p_cudaFree)(void *);
typedef cudaError_t (*p_cudaMemcpy)(void *, const void *, size_t, cudaMemcpyKind);
typedef cudaError_t (*p_cudaDeviceSynchronize)(void);

typedef cublasStatus_t (*p_cublasCreate)(cublasHandle_t *);
typedef cublasStatus_t (*p_cublasDestroy)(cublasHandle_t);
typedef cublasStatus_t (*p_cublasSgemm)(cublasHandle_t, cublasOperation_t,
                                        cublasOperation_t, int, int, int,
                                        const float *, const float *, int,
                                        const float *, int, const float *,
                                        float *, int);

#define CUDAGEMM_MAX_DEV 8
#define CUDAGEMM_MAX_RES 1024
#define CUDAGEMM_SPLIT_MB_DEFAULT 8u
#define CUDAGEMM_MIN_FLOPS_DEFAULT (1u << 20) /* 1M mul-adds */

typedef struct {
    const void *host;
    size_t bytes;
    int split;
    size_t owner;
    void *mem[CUDAGEMM_MAX_DEV];
    size_t off[CUDAGEMM_MAX_DEV];
    size_t len[CUDAGEMM_MAX_DEV];
} CudaRes;

typedef struct {
    int dev_id;
    cublasHandle_t blas;
    void *a_dev, *c_dev;
    size_t a_cap, c_cap;
    float *c_host;
    size_t c_host_cap;
    const float *a_fp_host;
    size_t a_fp_bytes;
    uint64_t a_fp_tag;
} CudaDev;

struct cce_cudagemm {
    cce_dl rt_dll;
    cce_dl blas_dll;
    CudaDev d[CUDAGEMM_MAX_DEV];
    size_t ndev;
    size_t rr;
    size_t split_bytes;
    size_t min_flops;
    CudaRes resident[CUDAGEMM_MAX_RES];
    size_t resident_count;
    p_cudaSetDevice SetDevice;
    p_cudaMalloc Malloc;
    p_cudaFree Free;
    p_cudaMemcpy Memcpy;
    p_cudaDeviceSynchronize Sync;
    p_cublasSgemm Sgemm;
};

static size_t env_size(const char *name, size_t fallback) {
    const char *s = getenv(name);
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return fallback;
    v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') return fallback;
    return (size_t)v;
}

static cce_dl open_first(const char *const *names) {
    int i;
    for (i = 0; names[i]; ++i) {
        cce_dl h = cce_dl_open(names[i]);
        if (h) return h;
    }
    return NULL;
}

static int load_syms(cce_cudagemm *h) {
    void *sym;
#define L(dll, dst, name)                                                      \
    do {                                                                       \
        sym = cce_dl_sym(dll, name);                                           \
        if (!sym) return -1;                                                   \
        memcpy(&(dst), &sym, sizeof(dst));                                     \
    } while (0)

#ifdef _WIN32
    static const char *const rt_names[] = {
        "cudart64_12.dll", "cudart64_120.dll", "cudart64_110.dll",
        "cudart64_11.dll", "cudart64_101.dll", NULL};
    static const char *const blas_names[] = {
        "cublas64_12.dll", "cublas64_120.dll", "cublas64_11.dll",
        "cublas64_110.dll", NULL};
#else
    static const char *const rt_names[] = {
        "libcudart.so", "libcudart.so.12", "libcudart.so.11.0",
        "libcudart.so.11", "/usr/local/cuda/lib64/libcudart.so", NULL};
    static const char *const blas_names[] = {
        "libcublas.so", "libcublas.so.12", "libcublas.so.11",
        "/usr/local/cuda/lib64/libcublas.so", NULL};
#endif

    h->rt_dll = open_first(rt_names);
    if (!h->rt_dll) return -1;
    h->blas_dll = open_first(blas_names);
    if (!h->blas_dll) {
        cce_dl_close(h->rt_dll);
        h->rt_dll = NULL;
        return -1;
    }

    L(h->rt_dll, h->SetDevice, "cudaSetDevice");
    L(h->rt_dll, h->Malloc, "cudaMalloc");
    L(h->rt_dll, h->Free, "cudaFree");
    L(h->rt_dll, h->Memcpy, "cudaMemcpy");
    L(h->rt_dll, h->Sync, "cudaDeviceSynchronize");
    /* cuBLAS v2 API entry points. */
    L(h->blas_dll, h->Sgemm, "cublasSgemm_v2");
#undef L
    return 0;
}

static cce_cudagemm *cudagemm_open_internal(int solo_index, char *name_out,
                                            size_t name_cap) {
    cce_cudagemm *h;
    p_cudaGetDeviceCount GetCount;
    p_cudaDeviceGetAttribute GetAttr;
    p_cublasCreate BlasCreate;
    void *sym;
    int n_all = 0, i, integrated;
    int all_ids[CUDAGEMM_MAX_DEV * 2];
    int n_disc = 0;
    int picked[CUDAGEMM_MAX_DEV];
    int n_picked = 0;
    size_t count_cap;

    h = (cce_cudagemm *)calloc(1, sizeof *h);
    if (!h) return NULL;
    if (load_syms(h) != 0) {
        free(h);
        return NULL;
    }

    sym = cce_dl_sym(h->rt_dll, "cudaGetDeviceCount");
    if (!sym) goto fail;
    memcpy(&GetCount, &sym, sizeof GetCount);
    sym = cce_dl_sym(h->rt_dll, "cudaDeviceGetAttribute");
    if (!sym) goto fail;
    memcpy(&GetAttr, &sym, sizeof GetAttr);
    sym = cce_dl_sym(h->blas_dll, "cublasCreate_v2");
    if (!sym) goto fail;
    memcpy(&BlasCreate, &sym, sizeof BlasCreate);

    if (GetCount(&n_all) != CUDA_SUCCESS_OK || n_all < 1) goto fail;
    if (n_all > (int)(sizeof all_ids / sizeof all_ids[0]))
        n_all = (int)(sizeof all_ids / sizeof all_ids[0]);

    for (i = 0; i < n_all; ++i) {
        integrated = 0;
        if (GetAttr(&integrated, cudaDevAttrIntegrated, i) != CUDA_SUCCESS_OK)
            integrated = 0;
        if (integrated) continue; /* leave out iGPU / Tegra-style parts */
        all_ids[n_disc++] = i;
    }
    if (n_disc == 0) goto fail;

    {
        const char *spec = getenv("CNET_GPU_DEVICES");
        if (spec && *spec) {
            const char *s = spec;
            while (*s && n_picked < CUDAGEMM_MAX_DEV) {
                char *end = NULL;
                unsigned long v = strtoul(s, &end, 10);
                if (end == s) break;
                if ((int)v < n_disc) {
                    int dup = 0, j;
                    for (j = 0; j < n_picked; ++j)
                        if (picked[j] == all_ids[v]) dup = 1;
                    if (!dup) picked[n_picked++] = all_ids[v];
                }
                s = end;
                while (*s == ',' || *s == ' ') ++s;
            }
            if (n_picked == 0) goto fail;
        }
        if (n_picked == 0) {
            for (i = 0; i < n_disc && n_picked < CUDAGEMM_MAX_DEV; ++i)
                picked[n_picked++] = all_ids[i];
        }
        count_cap = env_size("CNET_GPU_COUNT", 0);
        if (count_cap > 0 && (int)count_cap < n_picked)
            n_picked = (int)count_cap;
    }

    for (i = 0; i < n_picked; ++i) {
        CudaDev *D = &h->d[h->ndev];
        memset(D, 0, sizeof *D);
        D->dev_id = picked[i];
        if (h->SetDevice(D->dev_id) != CUDA_SUCCESS_OK) continue;
        if (BlasCreate(&D->blas) != CUBLAS_STATUS_SUCCESS_OK) continue;
        h->ndev++;
    }
    if (h->ndev == 0) goto fail;

    if (solo_index >= 0) {
        if ((size_t)solo_index >= h->ndev) goto fail;
        {
            size_t j;
            p_cublasDestroy BlasDestroy = NULL;
            sym = cce_dl_sym(h->blas_dll, "cublasDestroy_v2");
            if (sym) memcpy(&BlasDestroy, &sym, sizeof BlasDestroy);
            for (j = 0; j < h->ndev; ++j) {
                if (j == (size_t)solo_index) continue;
                if (h->d[j].blas && BlasDestroy) BlasDestroy(h->d[j].blas);
            }
            if (solo_index != 0) h->d[0] = h->d[solo_index];
            memset(&h->d[1], 0, (CUDAGEMM_MAX_DEV - 1) * sizeof h->d[0]);
            h->ndev = 1;
        }
    }

    h->split_bytes =
        env_size("CNET_GPU_SPLIT_MB", CUDAGEMM_SPLIT_MB_DEFAULT) * 1024u * 1024u;
    if (h->split_bytes == 0)
        h->split_bytes = CUDAGEMM_SPLIT_MB_DEFAULT * 1024u * 1024u;
    h->min_flops = env_size("CNET_GPU_MIN_FLOPS", CUDAGEMM_MIN_FLOPS_DEFAULT);
    if (h->min_flops == 0) h->min_flops = CUDAGEMM_MIN_FLOPS_DEFAULT;

    if (name_out && name_cap > 0) {
        if (h->ndev == 1)
            snprintf(name_out, name_cap, "cuBLAS dev%d", h->d[0].dev_id);
        else
            snprintf(name_out, name_cap, "cuBLAS x%zu (dev%d...)", h->ndev,
                     h->d[0].dev_id);
    }
    return h;

fail:
    cce_cudagemm_close(h);
    return NULL;
}

cce_cudagemm *cce_cudagemm_open(char *device_name_out, size_t device_name_cap) {
    return cudagemm_open_internal(-1, device_name_out, device_name_cap);
}

cce_cudagemm *cce_cudagemm_open_device(int device_index, char *device_name_out,
                                       size_t device_name_cap) {
    if (device_index < 0) return NULL;
    return cudagemm_open_internal(device_index, device_name_out,
                                  device_name_cap);
}

void cce_cudagemm_close(cce_cudagemm *h) {
    size_t i, j;
    p_cublasDestroy BlasDestroy = NULL;
    void *sym;
    if (!h) return;
    if (h->blas_dll) {
        sym = cce_dl_sym(h->blas_dll, "cublasDestroy_v2");
        if (sym) memcpy(&BlasDestroy, &sym, sizeof BlasDestroy);
    }
    for (i = 0; i < h->resident_count; ++i)
        for (j = 0; j < h->ndev; ++j)
            if (h->resident[i].mem[j]) {
                h->SetDevice(h->d[j].dev_id);
                h->Free(h->resident[i].mem[j]);
            }
    for (j = 0; j < h->ndev; ++j) {
        CudaDev *D = &h->d[j];
        h->SetDevice(D->dev_id);
        if (D->a_dev) h->Free(D->a_dev);
        if (D->c_dev) h->Free(D->c_dev);
        if (D->blas && BlasDestroy) BlasDestroy(D->blas);
        free(D->c_host);
    }
    if (h->blas_dll) cce_dl_close(h->blas_dll);
    if (h->rt_dll) cce_dl_close(h->rt_dll);
    free(h);
}

size_t cce_cudagemm_device_count(const cce_cudagemm *h) {
    return h ? h->ndev : 0;
}

size_t cce_cudagemm_resident_bytes(const cce_cudagemm *h) {
    size_t i, t = 0;
    if (!h) return 0;
    for (i = 0; i < h->resident_count; ++i) t += h->resident[i].bytes;
    return t;
}

static CudaRes *res_find(cce_cudagemm *h, const void *host, size_t bytes) {
    size_t i;
    for (i = 0; i < h->resident_count; ++i)
        if (h->resident[i].host == host && h->resident[i].bytes == bytes)
            return &h->resident[i];
    return NULL;
}

static int upload_cols(cce_cudagemm *h, size_t di, const float *host, size_t K,
                       size_t N, size_t off, size_t len, void **out) {
    size_t bytes = K * len * sizeof(float);
    void *dev = NULL;
    h->SetDevice(h->d[di].dev_id);
    if (h->Malloc(&dev, bytes) != CUDA_SUCCESS_OK) return -1;
    if (len == N) {
        if (h->Memcpy(dev, host, bytes, cudaMemcpyHostToDevice) !=
            CUDA_SUCCESS_OK) {
            h->Free(dev);
            return -1;
        }
    } else {
        float *tmp = (float *)malloc(bytes);
        size_t k;
        if (!tmp) {
            h->Free(dev);
            return -1;
        }
        for (k = 0; k < K; ++k)
            memcpy(tmp + k * len, host + k * N + off, len * sizeof(float));
        if (h->Memcpy(dev, tmp, bytes, cudaMemcpyHostToDevice) !=
            CUDA_SUCCESS_OK) {
            free(tmp);
            h->Free(dev);
            return -1;
        }
        free(tmp);
    }
    *out = dev;
    return 0;
}

static CudaRes *res_create(cce_cudagemm *h, const float *host, size_t K,
                           size_t N, int split, size_t owner, const size_t *off,
                           const size_t *len) {
    CudaRes *r;
    size_t d;
    if (h->resident_count >= CUDAGEMM_MAX_RES) return NULL;
    r = &h->resident[h->resident_count];
    memset(r, 0, sizeof *r);
    r->host = host;
    r->bytes = K * N * sizeof(float);
    r->split = split;
    r->owner = owner;
    for (d = 0; d < h->ndev; ++d) {
        r->off[d] = off[d];
        r->len[d] = len[d];
        if (len[d] == 0) continue;
        if (upload_cols(h, d, host, K, N, off[d], len[d], &r->mem[d]) != 0) {
            size_t e;
            for (e = 0; e < d; ++e)
                if (r->mem[e]) {
                    h->SetDevice(h->d[e].dev_id);
                    h->Free(r->mem[e]);
                }
            memset(r, 0, sizeof *r);
            return NULL;
        }
    }
    h->resident_count++;
    return r;
}

static void place(cce_cudagemm *h, size_t K, size_t N, int *split, size_t *owner,
                  size_t *off, size_t *len) {
    size_t d;
    for (d = 0; d < h->ndev; ++d) {
        off[d] = 0;
        len[d] = 0;
    }
    if (h->ndev > 1 && K * N * sizeof(float) >= h->split_bytes &&
        N >= h->ndev) {
        size_t base = N / h->ndev, rem = N % h->ndev, at = 0;
        *split = 1;
        *owner = 0;
        for (d = 0; d < h->ndev; ++d) {
            len[d] = base + (d < rem ? 1 : 0);
            off[d] = at;
            at += len[d];
        }
    } else {
        *split = 0;
        *owner = h->rr++ % h->ndev;
        len[*owner] = N;
    }
}

static int scratch(cce_cudagemm *h, size_t di, void **buf, size_t *cap,
                   size_t bytes) {
    if (*cap >= bytes && *buf) return 0;
    h->SetDevice(h->d[di].dev_id);
    if (*buf) h->Free(*buf);
    *buf = NULL;
    *cap = 0;
    if (h->Malloc(buf, bytes) != CUDA_SUCCESS_OK) return -1;
    *cap = bytes;
    h->d[di].a_fp_host = NULL;
    h->d[di].a_fp_bytes = 0;
    h->d[di].a_fp_tag = 0;
    return 0;
}

static uint64_t cuda_a_fp(const float *A, size_t nfloat) {
    uint64_t h = 14695981039346656037ULL ^ (nfloat * 0x9E3779B97F4A7C15ULL);
    size_t i, step;
    if (!A || nfloat == 0) return 0;
    step = nfloat > 64 ? nfloat / 64 : 1;
    for (i = 0; i < nfloat; i += step) {
        uint32_t u;
        memcpy(&u, &A[i], 4);
        h ^= (uint64_t)u + 0x9e3779b9u + (h << 6) + (h >> 2);
    }
    return h;
}

static int cuda_ensure_A(cce_cudagemm *h, size_t di, const float *A, size_t T,
                         size_t K) {
    CudaDev *D = &h->d[di];
    size_t bytes = T * K * sizeof(float);
    uint64_t tag;
    h->SetDevice(D->dev_id);
    if (scratch(h, di, &D->a_dev, &D->a_cap, bytes) != 0) return -1;
    tag = cuda_a_fp(A, T * K);
    if (D->a_fp_host == A && D->a_fp_bytes == bytes && D->a_fp_tag == tag)
        return 0;
    if (h->Memcpy(D->a_dev, A, bytes, cudaMemcpyHostToDevice) != CUDA_SUCCESS_OK)
        return -1;
    D->a_fp_host = A;
    D->a_fp_bytes = bytes;
    D->a_fp_tag = tag;
    return 0;
}

int cce_cudagemm_matmul(cce_cudagemm *h, const float *A, size_t T, size_t K,
                        const float *W, const float *bias, size_t N, float *C) {
    CudaRes *went;
    size_t d, t;
    float alpha = 1.0f, beta = 0.0f;
    int rc = 0;

    if (!h || !A || !W || !C || T == 0 || T > 8 || K == 0 || N == 0) return -1;
    /* Size floor: tiny GEMMs lose to PCIe — leave them on CPU. */
    if ((size_t)T * K * N < h->min_flops) return -1;
    if (K > 0x7FFFFFFF || N > 0x7FFFFFFF || T > 0x7FFFFFFF) return -1;

    went = res_find(h, W, K * N * sizeof(float));
    if (!went) {
        int split;
        size_t owner, off[CUDAGEMM_MAX_DEV], len[CUDAGEMM_MAX_DEV];
        place(h, K, N, &split, &owner, off, len);
        went = res_create(h, W, K, N, split, owner, off, len);
        if (!went) return -1;
    }

    /* Phase 1: upload A + launch sgemm on every device (no D2H yet) so multiple
       GPUs overlap. Phase 2: sync + download. */
    for (d = 0; d < h->ndev; ++d) {
        CudaDev *D = &h->d[d];
        size_t cols = went->len[d];
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        if (cuda_ensure_A(h, d, A, T, K) != 0 ||
            scratch(h, d, &D->c_dev, &D->c_cap, T * cols * sizeof(float)) != 0) {
            rc = -1;
            break;
        }
        if (went->split) {
            if (D->c_host_cap < T * cols) {
                float *nc =
                    (float *)realloc(D->c_host, T * cols * sizeof(float));
                if (!nc) {
                    rc = -1;
                    break;
                }
                D->c_host = nc;
                D->c_host_cap = T * cols;
            }
        }
        /* Row-major C[T,N]=A[T,K]*W[K,N] via col-major sgemm:
           sgemm(N,N, m=cols, n=T, k=K, W lda=cols, A ldb=K, C ldc=cols) */
        if (h->Sgemm(D->blas, CUBLAS_OP_N, CUBLAS_OP_N, (int)cols, (int)T,
                     (int)K, &alpha, (const float *)went->mem[d], (int)cols,
                     (const float *)D->a_dev, (int)K, &beta, (float *)D->c_dev,
                     (int)cols) != CUBLAS_STATUS_SUCCESS_OK) {
            rc = -1;
            break;
        }
    }
    if (rc != 0) return -1;
    for (d = 0; d < h->ndev; ++d) {
        CudaDev *D = &h->d[d];
        size_t cols = went->len[d];
        float *dst;
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        if (h->Sync() != CUDA_SUCCESS_OK) {
            rc = -1;
            break;
        }
        dst = went->split ? D->c_host : C;
        if (h->Memcpy(dst, D->c_dev, T * cols * sizeof(float),
                      cudaMemcpyDeviceToHost) != CUDA_SUCCESS_OK) {
            rc = -1;
            break;
        }
    }
    if (rc != 0) return -1;

    if (went->split) {
        for (d = 0; d < h->ndev; ++d) {
            size_t cols = went->len[d];
            if (cols == 0) continue;
            for (t = 0; t < T; ++t)
                memcpy(C + t * N + went->off[d], h->d[d].c_host + t * cols,
                       cols * sizeof(float));
        }
    }

    /* bias on host (T small) */
    if (bias) {
        for (t = 0; t < T; ++t) {
            size_t n;
            float *row = C + t * N;
            for (n = 0; n < N; ++n) row[n] += bias[n];
        }
    }
    return 0;
}

/* ---- int8 expand-to-float (LRU) + sgemm ---------------------------------- */

#define CUDA_Q8_MAX 256
#define CUDA_Q8_MB_DEFAULT 8192u

typedef struct {
    const int8_t *wq;
    size_t K, N;
    size_t bytes;
    uint64_t stamp;
    void *mem[CUDAGEMM_MAX_DEV];
    size_t off[CUDAGEMM_MAX_DEV];
    size_t len[CUDAGEMM_MAX_DEV];
    int split;
    size_t owner;
    int live;
} CudaQ8;

typedef struct {
    cce_cudagemm *h;
    CudaQ8 slot[CUDA_Q8_MAX];
    size_t n;
    size_t budget;
    size_t used;
    uint64_t tick;
} CudaQ8Cache;

static CudaQ8Cache g_q8_caches[8];
static int g_q8_ncache;

static CudaQ8Cache *q8_cache_for(cce_cudagemm *h) {
    int i;
    size_t mb;
    for (i = 0; i < g_q8_ncache; i++)
        if (g_q8_caches[i].h == h) return &g_q8_caches[i];
    if (g_q8_ncache >= 8) return NULL;
    memset(&g_q8_caches[g_q8_ncache], 0, sizeof g_q8_caches[0]);
    g_q8_caches[g_q8_ncache].h = h;
    mb = env_size("CNET_CUDA_Q8_MB", 0);
    if (mb == 0) mb = CUDA_Q8_MB_DEFAULT;
    g_q8_caches[g_q8_ncache].budget = mb * 1024u * 1024u;
    return &g_q8_caches[g_q8_ncache++];
}

static void q8_evict_one(CudaQ8Cache *c) {
    size_t i, victim = (size_t)-1;
    uint64_t oldest = ~(uint64_t)0;
    size_t d;
    cce_cudagemm *h = c->h;
    for (i = 0; i < c->n; i++) {
        if (!c->slot[i].live) continue;
        if (c->slot[i].stamp < oldest) {
            oldest = c->slot[i].stamp;
            victim = i;
        }
    }
    if (victim == (size_t)-1) return;
    for (d = 0; d < h->ndev; d++) {
        if (c->slot[victim].mem[d]) {
            h->SetDevice(h->d[d].dev_id);
            h->Free(c->slot[victim].mem[d]);
        }
    }
    c->used -= c->slot[victim].bytes;
    memset(&c->slot[victim], 0, sizeof c->slot[0]);
}

static CudaQ8 *q8_get(cce_cudagemm *h, const int8_t *Wq, const float *scales,
                      size_t K, size_t N) {
    CudaQ8Cache *c = q8_cache_for(h);
    CudaQ8 *s;
    size_t i, d, need;
    int split;
    size_t owner, off[CUDAGEMM_MAX_DEV], len[CUDAGEMM_MAX_DEV];
    float *tmp = NULL;
    if (!c) return NULL;
    for (i = 0; i < c->n; i++) {
        if (c->slot[i].live && c->slot[i].wq == Wq && c->slot[i].K == K &&
            c->slot[i].N == N) {
            c->slot[i].stamp = ++c->tick;
            return &c->slot[i];
        }
    }
    need = K * N * sizeof(float);
    while (c->used + need > c->budget && c->used > 0) q8_evict_one(c);
    if (c->used + need > c->budget) return NULL;
    s = NULL;
    for (i = 0; i < c->n; i++)
        if (!c->slot[i].live) {
            s = &c->slot[i];
            break;
        }
    if (!s) {
        if (c->n >= CUDA_Q8_MAX) {
            q8_evict_one(c);
            for (i = 0; i < c->n; i++)
                if (!c->slot[i].live) {
                    s = &c->slot[i];
                    break;
                }
        } else {
            s = &c->slot[c->n++];
        }
    }
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    place(h, K, N, &split, &owner, off, len);
    tmp = (float *)malloc(need);
    if (!tmp) return NULL;
    /* scales are per-column (n); Wq layout is k*N+n. */
    for (i = 0; i < K; i++)
        for (d = 0; d < N; d++)
            tmp[i * N + d] = scales[d] * (float)Wq[i * N + d];

    s->wq = Wq;
    s->K = K;
    s->N = N;
    s->split = split;
    s->owner = owner;
    s->bytes = 0;
    for (d = 0; d < h->ndev; d++) {
        s->off[d] = off[d];
        s->len[d] = len[d];
        if (len[d] == 0) continue;
        if (upload_cols(h, d, tmp, K, N, off[d], len[d], &s->mem[d]) != 0) {
            size_t e;
            for (e = 0; e < d; e++)
                if (s->mem[e]) {
                    h->SetDevice(h->d[e].dev_id);
                    h->Free(s->mem[e]);
                }
            free(tmp);
            memset(s, 0, sizeof *s);
            return NULL;
        }
        s->bytes += K * len[d] * sizeof(float);
    }
    free(tmp);
    s->live = 1;
    s->stamp = ++c->tick;
    c->used += s->bytes;
    return s;
}

int cce_cudagemm_matmul_q8(cce_cudagemm *h, const float *A, size_t T, size_t K,
                           const int8_t *Wq, const float *scales,
                           const float *bias, size_t N, float *C) {
    CudaQ8 *wq;
    size_t d, t;
    float alpha = 1.0f, beta = 0.0f;
    int rc = 0;
    if (!h || !A || !Wq || !scales || !C || T == 0 || T > 8 || K == 0 || N == 0)
        return -1;
    if ((size_t)T * K * N < h->min_flops) return -1;

    wq = q8_get(h, Wq, scales, K, N);
    if (!wq) return -1;
    for (d = 0; d < h->ndev; ++d) {
        CudaDev *D = &h->d[d];
        size_t cols = wq->len[d];
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        if (cuda_ensure_A(h, d, A, T, K) != 0 ||
            scratch(h, d, &D->c_dev, &D->c_cap, T * cols * sizeof(float)) != 0) {
            rc = -1;
            break;
        }
        if (wq->split) {
            if (D->c_host_cap < T * cols) {
                float *nc =
                    (float *)realloc(D->c_host, T * cols * sizeof(float));
                if (!nc) {
                    rc = -1;
                    break;
                }
                D->c_host = nc;
                D->c_host_cap = T * cols;
            }
        }
        if (h->Sgemm(D->blas, CUBLAS_OP_N, CUBLAS_OP_N, (int)cols, (int)T,
                     (int)K, &alpha, (const float *)wq->mem[d], (int)cols,
                     (const float *)D->a_dev, (int)K, &beta, (float *)D->c_dev,
                     (int)cols) != CUBLAS_STATUS_SUCCESS_OK) {
            rc = -1;
            break;
        }
    }
    if (rc != 0) return -1;
    for (d = 0; d < h->ndev; ++d) {
        CudaDev *D = &h->d[d];
        size_t cols = wq->len[d];
        float *dst;
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        h->Sync();
        dst = wq->split ? D->c_host : C;
        if (h->Memcpy(dst, D->c_dev, T * cols * sizeof(float),
                      cudaMemcpyDeviceToHost) != CUDA_SUCCESS_OK)
            return -1;
    }
    if (wq->split) {
        for (d = 0; d < h->ndev; ++d) {
            size_t cols = wq->len[d];
            if (cols == 0) continue;
            for (t = 0; t < T; ++t)
                memcpy(C + t * N + wq->off[d], h->d[d].c_host + t * cols,
                       cols * sizeof(float));
        }
    }
    if (bias) {
        for (t = 0; t < T; ++t) {
            size_t n;
            float *row = C + t * N;
            for (n = 0; n < N; ++n) row[n] += bias[n];
        }
    }
    return 0;
}
