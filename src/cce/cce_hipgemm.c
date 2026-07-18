/* Pure-C hipBLAS multi-GPU linear seam for CNET (dlopen ROCm). */
#include "../../include/cce/cce_hipgemm.h"

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

/* Minimal HIP / hipBLAS declarations — no SDK required at compile time. */
typedef int hipError_t;
typedef int hipblasStatus_t;
typedef int hipDeviceAttribute_t;
typedef enum {
    hipMemcpyHostToDevice = 1,
    hipMemcpyDeviceToHost = 2
} hipMemcpyKind;
typedef enum {
    HIPBLAS_OP_N = 111,
    HIPBLAS_OP_T = 112
} hipblasOperation_t;
typedef enum {
    HIPBLAS_STATUS_SUCCESS = 0
} hipblasStatus_e;
#define HIP_SUCCESS 0
#define hipDeviceAttributeIntegrated 16 /* hipDeviceAttribute_t: after HostNativeAtomic */

typedef struct hipblasContext *hipblasHandle_t;

typedef hipError_t (*p_hipGetDeviceCount)(int *);
typedef hipError_t (*p_hipSetDevice)(int);
typedef hipError_t (*p_hipDeviceGetAttribute)(int *, hipDeviceAttribute_t, int);
typedef hipError_t (*p_hipMalloc)(void **, size_t);
typedef hipError_t (*p_hipFree)(void *);
typedef hipError_t (*p_hipMemcpy)(void *, const void *, size_t, hipMemcpyKind);
typedef hipError_t (*p_hipDeviceSynchronize)(void);
typedef hipError_t (*p_hipGetDeviceProperties_v)(void *, int); /* unused */

typedef hipblasStatus_t (*p_hipblasCreate)(hipblasHandle_t *);
typedef hipblasStatus_t (*p_hipblasDestroy)(hipblasHandle_t);
typedef hipblasStatus_t (*p_hipblasSgemm)(hipblasHandle_t, hipblasOperation_t,
                                          hipblasOperation_t, int, int, int,
                                          const float *, const float *, int,
                                          const float *, int, const float *,
                                          float *, int);

#define HIPGEMM_MAX_DEV 8
#define HIPGEMM_MAX_RES 1024
#define HIPGEMM_SPLIT_MB_DEFAULT 8u
#define HIPGEMM_MIN_FLOPS_DEFAULT (1u << 20) /* 1M mul-adds */

typedef struct {
    const void *host;
    size_t bytes;
    int split;
    size_t owner;
    void *mem[HIPGEMM_MAX_DEV];
    size_t off[HIPGEMM_MAX_DEV];
    size_t len[HIPGEMM_MAX_DEV];
} HipRes;

typedef struct {
    int dev_id;
    hipblasHandle_t blas;
    void *a_dev, *c_dev;
    size_t a_cap, c_cap;
    float *c_host;
    size_t c_host_cap;
    const float *a_fp_host;
    size_t a_fp_bytes;
    uint64_t a_fp_tag;
} HipDev;

struct cce_hipgemm {
    cce_dl hip_dll;
    cce_dl blas_dll;
    HipDev d[HIPGEMM_MAX_DEV];
    size_t ndev;
    size_t rr;
    size_t split_bytes;
    size_t min_flops;
    HipRes resident[HIPGEMM_MAX_RES];
    size_t resident_count;
    p_hipSetDevice SetDevice;
    p_hipMalloc Malloc;
    p_hipFree Free;
    p_hipMemcpy Memcpy;
    p_hipDeviceSynchronize Sync;
    p_hipblasSgemm Sgemm;
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

static int load_syms(cce_hipgemm *h) {
    void *sym;
#define L(dll, dst, name)                                                      \
    do {                                                                       \
        sym = cce_dl_sym(dll, name);                                           \
        if (!sym) return -1;                                                   \
        memcpy(&(dst), &sym, sizeof(dst));                                     \
    } while (0)

    /* Prefer sonames used by ROCm 6/7 on Linux */
    h->hip_dll = cce_dl_open("libamdhip64.so");
    if (!h->hip_dll) h->hip_dll = cce_dl_open("libamdhip64.so.6");
    if (!h->hip_dll) h->hip_dll = cce_dl_open("/opt/rocm/lib/libamdhip64.so");
    if (!h->hip_dll) return -1;

    h->blas_dll = cce_dl_open("libhipblas.so");
    if (!h->blas_dll) h->blas_dll = cce_dl_open("libhipblas.so.2");
    if (!h->blas_dll) h->blas_dll = cce_dl_open("/opt/rocm/lib/libhipblas.so");
    if (!h->blas_dll) {
        cce_dl_close(h->hip_dll);
        h->hip_dll = NULL;
        return -1;
    }

    L(h->hip_dll, h->SetDevice, "hipSetDevice");
    L(h->hip_dll, h->Malloc, "hipMalloc");
    L(h->hip_dll, h->Free, "hipFree");
    L(h->hip_dll, h->Memcpy, "hipMemcpy");
    L(h->hip_dll, h->Sync, "hipDeviceSynchronize");
    L(h->blas_dll, h->Sgemm, "hipblasSgemm");
#undef L
    return 0;
}

static cce_hipgemm *hipgemm_open_internal(int solo_index, char *name_out,
                                          size_t name_cap) {
    cce_hipgemm *h;
    p_hipGetDeviceCount GetCount;
    p_hipDeviceGetAttribute GetAttr;
    p_hipblasCreate BlasCreate;
    void *sym;
    int n_all = 0, i, integrated;
    int all_ids[HIPGEMM_MAX_DEV * 2];
    int n_disc = 0;
    int picked[HIPGEMM_MAX_DEV];
    int n_picked = 0;
    size_t count_cap;

    h = (cce_hipgemm *)calloc(1, sizeof *h);
    if (!h) return NULL;
    if (load_syms(h) != 0) {
        free(h);
        return NULL;
    }

    sym = cce_dl_sym(h->hip_dll, "hipGetDeviceCount");
    if (!sym) goto fail;
    memcpy(&GetCount, &sym, sizeof GetCount);
    sym = cce_dl_sym(h->hip_dll, "hipDeviceGetAttribute");
    if (!sym) goto fail;
    memcpy(&GetAttr, &sym, sizeof GetAttr);
    sym = cce_dl_sym(h->blas_dll, "hipblasCreate");
    if (!sym) goto fail;
    memcpy(&BlasCreate, &sym, sizeof BlasCreate);

    if (GetCount(&n_all) != HIP_SUCCESS || n_all < 1) goto fail;
    if (n_all > (int)(sizeof all_ids / sizeof all_ids[0]))
        n_all = (int)(sizeof all_ids / sizeof all_ids[0]);

    for (i = 0; i < n_all; ++i) {
        integrated = 0;
        if (GetAttr(&integrated, hipDeviceAttributeIntegrated, i) !=
            HIP_SUCCESS)
            integrated = 0;
        if (integrated) continue; /* leave out iGPU */
        all_ids[n_disc++] = i;
    }
    if (n_disc == 0) goto fail;

    {
        const char *spec = getenv("CNET_GPU_DEVICES");
        if (spec && *spec) {
            const char *s = spec;
            while (*s && n_picked < HIPGEMM_MAX_DEV) {
                char *end = NULL;
                unsigned long v = strtoul(s, &end, 10);
                if (end == s) break;
                /* index into discrete enumeration (same as OpenCL after filter) */
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
            for (i = 0; i < n_disc && n_picked < HIPGEMM_MAX_DEV; ++i)
                picked[n_picked++] = all_ids[i];
        }
        count_cap = env_size("CNET_GPU_COUNT", 0);
        if (count_cap > 0 && (int)count_cap < n_picked)
            n_picked = (int)count_cap;
    }

    for (i = 0; i < n_picked; ++i) {
        HipDev *D = &h->d[h->ndev];
        memset(D, 0, sizeof *D);
        D->dev_id = picked[i];
        if (h->SetDevice(D->dev_id) != HIP_SUCCESS) continue;
        if (BlasCreate(&D->blas) != HIPBLAS_STATUS_SUCCESS) continue;
        h->ndev++;
    }
    if (h->ndev == 0) goto fail;

    if (solo_index >= 0) {
        if ((size_t)solo_index >= h->ndev) goto fail;
        /* keep only solo_index */
        {
            size_t j;
            p_hipblasDestroy BlasDestroy;
            sym = cce_dl_sym(h->blas_dll, "hipblasDestroy");
            memcpy(&BlasDestroy, &sym, sizeof BlasDestroy);
            for (j = 0; j < h->ndev; ++j) {
                if (j == (size_t)solo_index) continue;
                if (h->d[j].blas && BlasDestroy)
                    BlasDestroy(h->d[j].blas);
            }
            if (solo_index != 0) h->d[0] = h->d[solo_index];
            memset(&h->d[1], 0, (HIPGEMM_MAX_DEV - 1) * sizeof h->d[0]);
            h->ndev = 1;
        }
    }

    h->split_bytes =
        env_size("CNET_GPU_SPLIT_MB", HIPGEMM_SPLIT_MB_DEFAULT) * 1024u *
        1024u;
    if (h->split_bytes == 0)
        h->split_bytes = HIPGEMM_SPLIT_MB_DEFAULT * 1024u * 1024u;
    h->min_flops = env_size("CNET_GPU_MIN_FLOPS", HIPGEMM_MIN_FLOPS_DEFAULT);
    if (h->min_flops == 0) h->min_flops = HIPGEMM_MIN_FLOPS_DEFAULT;

    if (name_out && name_cap > 0) {
        if (h->ndev == 1)
            snprintf(name_out, name_cap, "hipBLAS dev%d", h->d[0].dev_id);
        else
            snprintf(name_out, name_cap, "hipBLAS x%zu (dev%d…)", h->ndev,
                     h->d[0].dev_id);
    }
    return h;

fail:
    cce_hipgemm_close(h);
    return NULL;
}

cce_hipgemm *cce_hipgemm_open(char *device_name_out, size_t device_name_cap) {
    return hipgemm_open_internal(-1, device_name_out, device_name_cap);
}

cce_hipgemm *cce_hipgemm_open_device(int device_index, char *device_name_out,
                                     size_t device_name_cap) {
    if (device_index < 0) return NULL;
    return hipgemm_open_internal(device_index, device_name_out, device_name_cap);
}

void cce_hipgemm_close(cce_hipgemm *h) {
    size_t i, j;
    p_hipblasDestroy BlasDestroy = NULL;
    void *sym;
    if (!h) return;
    if (h->blas_dll) {
        sym = cce_dl_sym(h->blas_dll, "hipblasDestroy");
        if (sym) memcpy(&BlasDestroy, &sym, sizeof BlasDestroy);
    }
    for (i = 0; i < h->resident_count; ++i)
        for (j = 0; j < h->ndev; ++j)
            if (h->resident[i].mem[j]) {
                h->SetDevice(h->d[j].dev_id);
                h->Free(h->resident[i].mem[j]);
            }
    for (j = 0; j < h->ndev; ++j) {
        HipDev *D = &h->d[j];
        h->SetDevice(D->dev_id);
        if (D->a_dev) h->Free(D->a_dev);
        if (D->c_dev) h->Free(D->c_dev);
        if (D->blas && BlasDestroy) BlasDestroy(D->blas);
        free(D->c_host);
    }
    if (h->blas_dll) cce_dl_close(h->blas_dll);
    if (h->hip_dll) cce_dl_close(h->hip_dll);
    free(h);
}

size_t cce_hipgemm_device_count(const cce_hipgemm *h) {
    return h ? h->ndev : 0;
}

size_t cce_hipgemm_resident_bytes(const cce_hipgemm *h) {
    size_t i, t = 0;
    if (!h) return 0;
    for (i = 0; i < h->resident_count; ++i) t += h->resident[i].bytes;
    return t;
}

static HipRes *res_find(cce_hipgemm *h, const void *host, size_t bytes) {
    size_t i;
    for (i = 0; i < h->resident_count; ++i)
        if (h->resident[i].host == host && h->resident[i].bytes == bytes)
            return &h->resident[i];
    return NULL;
}

static int upload_cols(cce_hipgemm *h, size_t di, const float *host, size_t K,
                       size_t N, size_t off, size_t len, void **out) {
    size_t bytes = K * len * sizeof(float);
    void *dev = NULL;
    h->SetDevice(h->d[di].dev_id);
    if (h->Malloc(&dev, bytes) != HIP_SUCCESS) return -1;
    if (len == N) {
        if (h->Memcpy(dev, host, bytes, hipMemcpyHostToDevice) != HIP_SUCCESS) {
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
        if (h->Memcpy(dev, tmp, bytes, hipMemcpyHostToDevice) != HIP_SUCCESS) {
            free(tmp);
            h->Free(dev);
            return -1;
        }
        free(tmp);
    }
    *out = dev;
    return 0;
}

static HipRes *res_create(cce_hipgemm *h, const float *host, size_t K, size_t N,
                          int split, size_t owner, const size_t *off,
                          const size_t *len) {
    HipRes *r;
    size_t d;
    if (h->resident_count >= HIPGEMM_MAX_RES) return NULL;
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

static void place(cce_hipgemm *h, size_t K, size_t N, int *split, size_t *owner,
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

static int scratch(cce_hipgemm *h, size_t di, void **buf, size_t *cap,
                   size_t bytes) {
    if (*cap >= bytes && *buf) return 0;
    h->SetDevice(h->d[di].dev_id);
    if (*buf) h->Free(*buf);
    *buf = NULL;
    *cap = 0;
    if (h->Malloc(buf, bytes) != HIP_SUCCESS) return -1;
    *cap = bytes;
    h->d[di].a_fp_host = NULL;
    h->d[di].a_fp_bytes = 0;
    h->d[di].a_fp_tag = 0;
    return 0;
}

static uint64_t hip_a_fp(const float *A, size_t nfloat) {
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

static int hip_ensure_A(cce_hipgemm *h, size_t di, const float *A, size_t T,
                        size_t K) {
    HipDev *D = &h->d[di];
    size_t bytes = T * K * sizeof(float);
    uint64_t tag;
    h->SetDevice(D->dev_id);
    if (scratch(h, di, &D->a_dev, &D->a_cap, bytes) != 0) return -1;
    tag = hip_a_fp(A, T * K);
    if (D->a_fp_host == A && D->a_fp_bytes == bytes && D->a_fp_tag == tag)
        return 0;
    if (h->Memcpy(D->a_dev, A, bytes, hipMemcpyHostToDevice) != HIP_SUCCESS)
        return -1;
    D->a_fp_host = A;
    D->a_fp_bytes = bytes;
    D->a_fp_tag = tag;
    return 0;
}

int cce_hipgemm_matmul(cce_hipgemm *h, const float *A, size_t T, size_t K,
                       const float *W, const float *bias, size_t N, float *C) {
    HipRes *went, *bent = NULL;
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
        size_t owner, off[HIPGEMM_MAX_DEV], len[HIPGEMM_MAX_DEV];
        place(h, K, N, &split, &owner, off, len);
        went = res_create(h, W, K, N, split, owner, off, len);
        if (!went) return -1;
    }
    if (bias) {
        bent = res_find(h, bias, N * sizeof(float));
        if (!bent) {
            bent = res_create(h, bias, 1, N, went->split, went->owner, went->off,
                              went->len);
            if (!bent) return -1;
        }
    }

    /* Phase 1: upload A + launch sgemm on every device (no D2H yet) so dual
       GPUs overlap compute. Phase 2: sync + download. */
    for (d = 0; d < h->ndev; ++d) {
        HipDev *D = &h->d[d];
        size_t cols = went->len[d];
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        if (hip_ensure_A(h, d, A, T, K) != 0 ||
            scratch(h, d, &D->c_dev, &D->c_cap, T * cols * sizeof(float)) !=
                0) {
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
           sgemm(N,N, n=N, m=T, k=K, W lda=N, A ldb=K, C ldc=N) */
        if (h->Sgemm(D->blas, HIPBLAS_OP_N, HIPBLAS_OP_N, (int)cols, (int)T,
                     (int)K, &alpha, (const float *)went->mem[d], (int)cols,
                     (const float *)D->a_dev, (int)K, &beta,
                     (float *)D->c_dev, (int)cols) != HIPBLAS_STATUS_SUCCESS) {
            rc = -1;
            break;
        }
        /* leave sgemm in flight; next device setup overlaps */
    }
    if (rc != 0) return -1;
    for (d = 0; d < h->ndev; ++d) {
        HipDev *D = &h->d[d];
        size_t cols = went->len[d];
        float *dst;
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        if (h->Sync() != HIP_SUCCESS) {
            rc = -1;
            break;
        }
        dst = went->split ? D->c_host : C;
        if (h->Memcpy(dst, D->c_dev, T * cols * sizeof(float),
                      hipMemcpyDeviceToHost) != HIP_SUCCESS) {
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
                memcpy(C + t * N + went->off[d],
                       h->d[d].c_host + t * cols, cols * sizeof(float));
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

#define HIP_Q8_MAX 256
#define HIP_Q8_MB_DEFAULT 8192u

typedef struct {
    const int8_t *wq;
    size_t K, N;
    size_t bytes; /* K*N*4 total across devices (approx) */
    uint64_t stamp;
    void *mem[HIPGEMM_MAX_DEV];
    size_t off[HIPGEMM_MAX_DEV];
    size_t len[HIPGEMM_MAX_DEV];
    int split;
    size_t owner;
    int live;
} HipQ8;

/* stash on handle via unused tail of resident array — keep separate static
 * list on the handle by growing struct... use file-static map keyed by h. */
typedef struct {
    cce_hipgemm *h;
    HipQ8 slot[HIP_Q8_MAX];
    size_t n;
    size_t budget;
    size_t used;
    uint64_t tick;
} HipQ8Cache;

static HipQ8Cache g_q8_caches[8];
static int g_q8_ncache;

static HipQ8Cache *q8_cache_for(cce_hipgemm *h) {
    int i;
    size_t mb;
    for (i = 0; i < g_q8_ncache; i++)
        if (g_q8_caches[i].h == h) return &g_q8_caches[i];
    if (g_q8_ncache >= 8) return NULL;
    memset(&g_q8_caches[g_q8_ncache], 0, sizeof g_q8_caches[0]);
    g_q8_caches[g_q8_ncache].h = h;
    mb = 0;
    {
        const char *e = getenv("CNET_HIP_Q8_MB");
        if (e && *e) mb = (size_t)strtoul(e, NULL, 10);
    }
    if (mb == 0) mb = HIP_Q8_MB_DEFAULT;
    g_q8_caches[g_q8_ncache].budget = mb * 1024u * 1024u;
    return &g_q8_caches[g_q8_ncache++];
}

static void q8_evict_one(HipQ8Cache *c) {
    size_t i, victim = (size_t)-1;
    uint64_t oldest = ~(uint64_t)0;
    size_t d;
    cce_hipgemm *h = c->h;
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

static HipQ8 *q8_get(cce_hipgemm *h, const int8_t *Wq, const float *scales,
                     size_t K, size_t N) {
    HipQ8Cache *c = q8_cache_for(h);
    HipQ8 *s;
    size_t i, d, need;
    int split;
    size_t owner, off[HIPGEMM_MAX_DEV], len[HIPGEMM_MAX_DEV];
    float *tmp = NULL;
    if (!c) return NULL;
    for (i = 0; i < c->n; i++) {
        if (c->slot[i].live && c->slot[i].wq == Wq && c->slot[i].K == K &&
            c->slot[i].N == N) {
            c->slot[i].stamp = ++c->tick;
            return &c->slot[i];
        }
    }
    /* expand */
    need = K * N * sizeof(float);
    while (c->used + need > c->budget && c->used > 0) q8_evict_one(c);
    if (c->used + need > c->budget) return NULL;
    /* find free slot */
    s = NULL;
    for (i = 0; i < c->n; i++)
        if (!c->slot[i].live) {
            s = &c->slot[i];
            break;
        }
    if (!s) {
        if (c->n >= HIP_Q8_MAX) {
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

int cce_hipgemm_matmul_q8(cce_hipgemm *h, const float *A, size_t T, size_t K,
                          const int8_t *Wq, const float *scales,
                          const float *bias, size_t N, float *C) {
    HipQ8 *wq;
    HipRes *bent = NULL;
    size_t d, t;
    float alpha = 1.0f, beta = 0.0f;
    int rc = 0;
    if (!h || !A || !Wq || !scales || !C || T == 0 || T > 8 || K == 0 || N == 0)
        return -1;
    if ((size_t)T * K * N < h->min_flops) return -1;

    wq = q8_get(h, Wq, scales, K, N);
    if (!wq) return -1;
    if (bias) {
        bent = res_find(h, bias, N * sizeof(float));
        if (!bent) {
            bent = res_create(h, bias, 1, N, wq->split, wq->owner, wq->off,
                              wq->len);
            if (!bent) return -1;
        }
    }
    for (d = 0; d < h->ndev; ++d) {
        HipDev *D = &h->d[d];
        size_t cols = wq->len[d];
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        if (hip_ensure_A(h, d, A, T, K) != 0 ||
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
        if (h->Sgemm(D->blas, HIPBLAS_OP_N, HIPBLAS_OP_N, (int)cols, (int)T,
                     (int)K, &alpha, (const float *)wq->mem[d], (int)cols,
                     (const float *)D->a_dev, (int)K, &beta, (float *)D->c_dev,
                     (int)cols) != HIPBLAS_STATUS_SUCCESS) {
            rc = -1;
            break;
        }
    }
    if (rc != 0) return -1;
    for (d = 0; d < h->ndev; ++d) {
        HipDev *D = &h->d[d];
        size_t cols = wq->len[d];
        float *dst;
        if (cols == 0) continue;
        h->SetDevice(D->dev_id);
        h->Sync();
        dst = wq->split ? D->c_host : C;
        if (h->Memcpy(dst, D->c_dev, T * cols * sizeof(float),
                      hipMemcpyDeviceToHost) != HIP_SUCCESS)
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
