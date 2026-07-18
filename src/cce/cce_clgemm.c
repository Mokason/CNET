#include "../../include/cce/cce_clgemm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE cce_dl;
static cce_dl cce_dl_open(const char *n) { return LoadLibraryA(n); }
static void *cce_dl_sym(cce_dl h, const char *n) { return (void *)GetProcAddress(h, n); }
static void cce_dl_close(cce_dl h) { FreeLibrary(h); }
#define CCE_CL_DEFAULT_DLL "OpenCL.dll"
#else
#include <dlfcn.h>
typedef void *cce_dl;
static cce_dl cce_dl_open(const char *n) { return dlopen(n, RTLD_NOW); }
static void *cce_dl_sym(cce_dl h, const char *n) { return dlsym(h, n); }
static void cce_dl_close(cce_dl h) { dlclose(h); }
#define CCE_CL_DEFAULT_DLL "libOpenCL.so.1"
#endif

/* ---- minimal OpenCL declarations (no SDK header dependency) ------------- */

typedef signed int cl_int;
typedef unsigned int cl_uint;
typedef unsigned long long cl_ulong;
typedef cl_ulong cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_command_queue_properties;
typedef cl_uint cl_bool;
typedef cl_uint cl_device_info;
typedef cl_uint cl_program_build_info;

typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_program *cl_program;
typedef struct _cl_kernel *cl_kernel;
typedef struct _cl_mem *cl_mem;

#define CL_SUCCESS 0
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_MEM_READ_ONLY (1 << 2)
#define CL_MEM_READ_WRITE (1 << 0)
#define CL_MEM_COPY_HOST_PTR (1 << 5)
#define CL_TRUE 1
#define CL_FALSE 0
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_HOST_UNIFIED_MEMORY 0x1035
#define CL_PROGRAM_BUILD_LOG 0x1183

/* OpenCL entry points are CL_API_CALL; on x64 the calling convention is
   unified, so plain pointers are correct for this 64-bit-only toolchain. */
typedef cl_int (*p_clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
typedef cl_int (*p_clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint,
                                   cl_device_id *, cl_uint *);
typedef cl_int (*p_clGetDeviceInfo)(cl_device_id, cl_device_info, size_t,
                                    void *, size_t *);
typedef cl_context (*p_clCreateContext)(const void *, cl_uint,
                                        const cl_device_id *, void *, void *,
                                        cl_int *);
typedef cl_command_queue (*p_clCreateCommandQueue)(cl_context, cl_device_id,
                                                   cl_command_queue_properties,
                                                   cl_int *);
typedef cl_program (*p_clCreateProgramWithSource)(cl_context, cl_uint,
                                                  const char **,
                                                  const size_t *, cl_int *);
typedef cl_int (*p_clBuildProgram)(cl_program, cl_uint, const cl_device_id *,
                                   const char *, void *, void *);
typedef cl_int (*p_clGetProgramBuildInfo)(cl_program, cl_device_id,
                                          cl_program_build_info, size_t,
                                          void *, size_t *);
typedef cl_kernel (*p_clCreateKernel)(cl_program, const char *, cl_int *);
typedef cl_mem (*p_clCreateBuffer)(cl_context, cl_mem_flags, size_t, void *,
                                   cl_int *);
typedef cl_int (*p_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool,
                                         size_t, size_t, const void *, cl_uint,
                                         const void *, void *);
typedef cl_int (*p_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool,
                                        size_t, size_t, void *, cl_uint,
                                        const void *, void *);
typedef cl_int (*p_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
typedef cl_int (*p_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel,
                                           cl_uint, const size_t *,
                                           const size_t *, const size_t *,
                                           cl_uint, const void *, void *);
typedef cl_int (*p_clFlush)(cl_command_queue);
typedef cl_int (*p_clFinish)(cl_command_queue);
typedef cl_int (*p_clReleaseMemObject)(cl_mem);
typedef cl_int (*p_clReleaseKernel)(cl_kernel);
typedef cl_int (*p_clReleaseProgram)(cl_program);
typedef cl_int (*p_clReleaseCommandQueue)(cl_command_queue);
typedef cl_int (*p_clReleaseContext)(cl_context);

/* C[t*N+n] = (bias? B[n]:0) + sum_k A[t*K+k]*W[k*N+n].
 *
 * Speed: local-memory tiles of A so N work-items share one A load (decode
 * T=1 is the hot path). Bit-identity: k still walks 0..K-1 ascending with
 * FP_CONTRACT OFF — same as CPU mul+add order.
 * N is a kernel argument so column-split slices stay correct. */
static const char *k_src =
    "#pragma OPENCL FP_CONTRACT OFF\n"
    "#define CCE_ATILE 64\n"
    "__kernel void cnet_gemm(__global const float* A,\n"
    "                        __global const float* W,\n"
    "                        __global const float* B,\n"
    "                        __global float* C,\n"
    "                        const int T, const int K, const int N,\n"
    "                        const int has_bias) {\n"
    "    int n = get_global_id(0);\n"
    "    int t = get_global_id(1);\n"
    "    int lid = get_local_id(0);\n"
    "    __local float As[CCE_ATILE];\n"
    "    float acc = 0.0f;\n"
    "    int valid = (n < N && t < T);\n"
    "    if (valid && has_bias) acc = B[n];\n"
    "    __global const float* arow = A + (size_t)(valid ? t : 0) * (size_t)K;\n"
    "    for (int k0 = 0; k0 < K; k0 += CCE_ATILE) {\n"
    "        int kk = k0 + lid;\n"
    "        if (lid < CCE_ATILE)\n"
    "            As[lid] = (kk < K) ? arow[kk] : 0.0f;\n"
    "        barrier(CLK_LOCAL_MEM_FENCE);\n"
    "        if (valid) {\n"
    "            int lim = K - k0;\n"
    "            if (lim > CCE_ATILE) lim = CCE_ATILE;\n"
    "            for (int i = 0; i < lim; ++i)\n"
    "                acc += As[i] * W[((size_t)k0 + (size_t)i) * (size_t)N + (size_t)n];\n"
    "        }\n"
    "        barrier(CLK_LOCAL_MEM_FENCE);\n"
    "    }\n"
    "    if (valid) C[(size_t)t * (size_t)N + (size_t)n] = acc;\n"
    "}\n"
    "__kernel void cnet_gemm_q8(__global const float* A,\n"
    "                           __global const char* Wq,\n"
    "                           __global const float* S,\n"
    "                           __global const float* B,\n"
    "                           __global float* C,\n"
    "                           const int T, const int K, const int N,\n"
    "                           const int has_bias) {\n"
    "    int n = get_global_id(0);\n"
    "    int t = get_global_id(1);\n"
    "    int lid = get_local_id(0);\n"
    "    __local float As[CCE_ATILE];\n"
    "    float acc = 0.0f;\n"
    "    int valid = (n < N && t < T);\n"
    "    __global const float* arow = A + (size_t)(valid ? t : 0) * (size_t)K;\n"
    "    for (int k0 = 0; k0 < K; k0 += CCE_ATILE) {\n"
    "        int kk = k0 + lid;\n"
    "        if (lid < CCE_ATILE)\n"
    "            As[lid] = (kk < K) ? arow[kk] : 0.0f;\n"
    "        barrier(CLK_LOCAL_MEM_FENCE);\n"
    "        if (valid) {\n"
    "            int lim = K - k0;\n"
    "            if (lim > CCE_ATILE) lim = CCE_ATILE;\n"
    "            for (int i = 0; i < lim; ++i)\n"
    "                acc += As[i] * convert_float(\n"
    "                    Wq[((size_t)k0 + (size_t)i) * (size_t)N + (size_t)n]);\n"
    "        }\n"
    "        barrier(CLK_LOCAL_MEM_FENCE);\n"
    "    }\n"
    "    if (valid) {\n"
    "        float v = (has_bias ? B[n] : 0.0f) + S[n] * acc;\n"
    "        C[(size_t)t * (size_t)N + (size_t)n] = v;\n"
    "    }\n"
    "}\n"
    /* Residency helpers + decode attention (primary device). */
    "__kernel void cnet_rms(__global const float* x,\n"
    "                       __global const float* w,\n"
    "                       __global float* y,\n"
    "                       const int D, const float eps, const int has_w) {\n"
    "    int i = get_global_id(0);\n"
    "    if (i != 0) return;\n"
    "    float ss = 0.0f;\n"
    "    for (int d = 0; d < D; ++d) ss += x[d] * x[d];\n"
    "    ss = rsqrt(ss / (float)D + eps);\n"
    "    for (int d = 0; d < D; ++d)\n"
    "        y[d] = x[d] * ss * (has_w ? w[d] : 1.0f);\n"
    "}\n"
    "__kernel void cnet_add(__global const float* a,\n"
    "                       __global const float* b,\n"
    "                       __global float* y, const int n) {\n"
    "    int i = get_global_id(0);\n"
    "    if (i < n) y[i] = a[i] + b[i];\n"
    "}\n"
    "__kernel void cnet_silu_mul(__global const float* g,\n"
    "                            __global const float* u,\n"
    "                            __global float* y, const int n) {\n"
    "    int i = get_global_id(0);\n"
    "    if (i < n) {\n"
    "        float x = g[i];\n"
    "        float s = 1.0f / (1.0f + exp(-x));\n"
    "        y[i] = (x * s) * u[i];\n"
    "    }\n"
    "}\n"
    /* scores[j] = scale * dot(q, K[j*hd : ])  for j in [0, seq) */
    "__kernel void cnet_ascores(__global const float* q,\n"
    "                           __global const float* K,\n"
    "                           __global float* scores,\n"
    "                           const int seq, const int hd, const float scale) {\n"
    "    int j = get_global_id(0);\n"
    "    if (j >= seq) return;\n"
    "    float acc = 0.0f;\n"
    "    __global const float* kj = K + (size_t)j * (size_t)hd;\n"
    "    for (int d = 0; d < hd; ++d) acc += q[d] * kj[d];\n"
    "    scores[j] = acc * scale;\n"
    "}\n"
    "__kernel void cnet_asoftmax(__global float* scores, const int seq) {\n"
    "    int i = get_global_id(0);\n"
    "    if (i != 0) return;\n"
    "    float m = -1e30f;\n"
    "    for (int j = 0; j < seq; ++j) if (scores[j] > m) m = scores[j];\n"
    "    float s = 0.0f;\n"
    "    for (int j = 0; j < seq; ++j) {\n"
    "        scores[j] = exp(scores[j] - m);\n"
    "        s += scores[j];\n"
    "    }\n"
    "    float inv = 1.0f / s;\n"
    "    for (int j = 0; j < seq; ++j) scores[j] *= inv;\n"
    "}\n"
    "__kernel void cnet_av(__global const float* scores,\n"
    "                      __global const float* V,\n"
    "                      __global float* out,\n"
    "                      const int seq, const int vd) {\n"
    "    int d = get_global_id(0);\n"
    "    if (d >= vd) return;\n"
    "    float acc = 0.0f;\n"
    "    for (int j = 0; j < seq; ++j)\n"
    "        acc += scores[j] * V[(size_t)j * (size_t)vd + (size_t)d];\n"
    "    out[d] = acc;\n"
    "}\n";

#define CLGEMM_MAX_RESIDENT 1024
#define CLGEMM_MAX_DEV 8
#define CLGEMM_MAX_ENUM 16
/* Lower default split so dual discrete cards co-own large layers more often
   (was 32 MiB; 8 MiB covers typical 4k×4k+ FP weights). */
#define CLGEMM_SPLIT_BYTES_DEFAULT (8u * 1024u * 1024u)
#define CLGEMM_MIN_FLOPS_DEFAULT (1u << 20) /* 1M: skip PCIe-bound micro GEMMs */

/* Device-resident placement of one stable host array (weights or bias),
   keyed by host pointer. Either column-SPLIT across every device (device d
   holds packed columns [off[d], off[d]+len[d])) or whole on one OWNER
   (len[owner] = N, all others 0). Placement is decided once at first use
   and reused verbatim on every later call. */
typedef struct {
    const void *host;
    size_t bytes;                 /* total host bytes (split slices sum to this) */
    int split;
    int is_q8;                    /* int8 weights (+ per-column scale buffer) */
    size_t owner;                 /* meaningful when !split */
    cl_mem mem[CLGEMM_MAX_DEV];
    cl_mem scale[CLGEMM_MAX_DEV]; /* q8 only: per-column float scales */
    size_t off[CLGEMM_MAX_DEV];   /* first column on device d */
    size_t len[CLGEMM_MAX_DEV];   /* columns on device d (0 = not involved) */
} ResidentBuf;

typedef struct {
    cl_device_id dev;
    cl_context ctx;
    cl_command_queue q;
    cl_program prog;
    cl_kernel kern;
    cl_kernel kern_q8;
    cl_kernel kern_rms;
    cl_kernel kern_add;
    cl_kernel kern_silu;
    cl_kernel kern_ascores;
    cl_kernel kern_asoft;
    cl_kernel kern_av;
    cl_mem a_buf, c_buf;          /* reusable activation/output scratch */
    size_t a_cap, c_cap;
    cl_mem op_a, op_b, op_c;      /* residency ops + attn scratch */
    size_t op_a_cap, op_b_cap, op_c_cap;
    float *c_host;                /* host landing zone for split reads */
    size_t c_host_cap;
    float *pack_host;             /* host pack for K/V heads */
    size_t pack_cap;
    /* Skip H2D when the same activation rows are reused (q/k/v, gate/up).
       Fingerprint catches in-place rewrites of the same host buffer. */
    const float *a_fp_host;
    size_t a_fp_bytes;
    uint64_t a_fp_tag;
} ClgemmDev;

struct cce_clgemm {
    cce_dl dll;
    ClgemmDev d[CLGEMM_MAX_DEV];
    size_t ndev;
    size_t rr;                    /* round-robin owner for unsplit weights */
    size_t split_bytes;           /* matrices >= this are column-split */
    size_t min_flops;             /* T*K*N below this → refuse (CPU) */
    ResidentBuf resident[CLGEMM_MAX_RESIDENT];
    size_t resident_count;
    /* function pointers */
    p_clCreateBuffer CreateBuffer;
    p_clEnqueueWriteBuffer WriteBuffer;
    p_clEnqueueReadBuffer ReadBuffer;
    p_clSetKernelArg SetArg;
    p_clEnqueueNDRangeKernel Enqueue;
    p_clFlush Flush;
    p_clFinish Finish;
    p_clReleaseMemObject ReleaseMem;
    p_clReleaseKernel ReleaseKernel;
    p_clReleaseProgram ReleaseProgram;
    p_clReleaseCommandQueue ReleaseQueue;
    p_clReleaseContext ReleaseContext;
};

/* Parse a small positive env integer; fallback when unset/garbage. */
static size_t env_size(const char *name, size_t fallback) {
    const char *s = getenv(name);
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return fallback;
    v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') return fallback;
    return (size_t)v;
}

/* solo_index < 0: open the whole selected set (env-configurable). Otherwise
   open ONLY the solo_index-th device of the set that actually OPENS (the
   index space cce_clgemm_device_count reports) — the handle an oracle-pool
   lane pins to, so concurrent lanes never share a queue. */
static cce_clgemm *clgemm_open_internal(const char *dll_name, int solo_index,
                                        char *device_name_out,
                                        size_t device_name_cap) {
    cce_clgemm *h;
    p_clGetPlatformIDs GetPlatformIDs;
    p_clGetDeviceIDs GetDeviceIDs;
    p_clGetDeviceInfo GetDeviceInfo;
    p_clCreateContext CreateContext;
    p_clCreateCommandQueue CreateQueue;
    p_clCreateProgramWithSource CreateProgram;
    p_clBuildProgram BuildProgram;
    p_clCreateKernel CreateKernel;
    cl_platform_id plats[8];
    cl_uint n_plat = 0, p;
    /* enumeration (platform-major order — the order CNET_GPU_DEVICES indexes) */
    cl_device_id all_dev[CLGEMM_MAX_ENUM];
    size_t all_plat[CLGEMM_MAX_ENUM];
    int all_unified[CLGEMM_MAX_ENUM];
    size_t n_all = 0;
    size_t picked[CLGEMM_MAX_DEV];
    size_t n_picked = 0;
    size_t i, j;
    size_t count_cap;
    cl_int err = 0;

    h = (cce_clgemm *)calloc(1, sizeof *h);
    if (!h) return NULL;

    h->dll = cce_dl_open(dll_name ? dll_name : CCE_CL_DEFAULT_DLL);
    if (!h->dll) { free(h); return NULL; }

#define SYM(dst, type, name)                                                \
    do {                                                                    \
        void *_sym = cce_dl_sym(h->dll, name);                              \
        if (!_sym) { cce_dl_close(h->dll); free(h); return NULL; }          \
        /* POSIX requires dlsym results to be usable as function pointers.     \
           memcpy avoids ISO C's forbidden object/function-pointer cast. */  \
        _Static_assert(sizeof(dst) == sizeof(_sym),                          \
                       "dlsym/function pointer size mismatch");              \
        memcpy(&(dst), &_sym, sizeof(dst));                                 \
    } while (0)

    SYM(GetPlatformIDs, p_clGetPlatformIDs, "clGetPlatformIDs");
    SYM(GetDeviceIDs, p_clGetDeviceIDs, "clGetDeviceIDs");
    SYM(GetDeviceInfo, p_clGetDeviceInfo, "clGetDeviceInfo");
    SYM(CreateContext, p_clCreateContext, "clCreateContext");
    SYM(CreateQueue, p_clCreateCommandQueue, "clCreateCommandQueue");
    SYM(CreateProgram, p_clCreateProgramWithSource, "clCreateProgramWithSource");
    SYM(BuildProgram, p_clBuildProgram, "clBuildProgram");
    SYM(CreateKernel, p_clCreateKernel, "clCreateKernel");
    SYM(h->CreateBuffer, p_clCreateBuffer, "clCreateBuffer");
    SYM(h->WriteBuffer, p_clEnqueueWriteBuffer, "clEnqueueWriteBuffer");
    SYM(h->ReadBuffer, p_clEnqueueReadBuffer, "clEnqueueReadBuffer");
    SYM(h->SetArg, p_clSetKernelArg, "clSetKernelArg");
    SYM(h->Enqueue, p_clEnqueueNDRangeKernel, "clEnqueueNDRangeKernel");
    SYM(h->Flush, p_clFlush, "clFlush");
    SYM(h->Finish, p_clFinish, "clFinish");
    SYM(h->ReleaseMem, p_clReleaseMemObject, "clReleaseMemObject");
    SYM(h->ReleaseKernel, p_clReleaseKernel, "clReleaseKernel");
    SYM(h->ReleaseProgram, p_clReleaseProgram, "clReleaseProgram");
    SYM(h->ReleaseQueue, p_clReleaseCommandQueue, "clReleaseCommandQueue");
    SYM(h->ReleaseContext, p_clReleaseContext, "clReleaseContext");
#undef SYM

    if (GetPlatformIDs(8, plats, &n_plat) != CL_SUCCESS || n_plat == 0) {
        cce_dl_close(h->dll);
        free(h);
        return NULL;
    }
    if (n_plat > 8) n_plat = 8;   /* n_plat reports AVAILABLE, not written */

    /* Enumerate every GPU device on every platform. */
    for (p = 0; p < n_plat && n_all < CLGEMM_MAX_ENUM; ++p) {
        cl_device_id devs[CLGEMM_MAX_ENUM];
        cl_uint nd = 0, k;
        if (GetDeviceIDs(plats[p], CL_DEVICE_TYPE_GPU, CLGEMM_MAX_ENUM, devs,
                         &nd) != CL_SUCCESS) {
            continue;
        }
        for (k = 0; k < nd && n_all < CLGEMM_MAX_ENUM; ++k) {
            cl_bool unified = CL_FALSE;
            if (GetDeviceInfo(devs[k], CL_DEVICE_HOST_UNIFIED_MEMORY,
                              sizeof unified, &unified, NULL) != CL_SUCCESS) {
                unified = CL_FALSE;   /* unknown: treat as discrete */
            }
            all_dev[n_all] = devs[k];
            all_plat[n_all] = (size_t)p;
            all_unified[n_all] = (unified != CL_FALSE);
            n_all++;
        }
    }
    if (n_all == 0) { cce_dl_close(h->dll); free(h); return NULL; }

    /* Selection. CNET_GPU_DEVICES=i,j — explicit enumeration indices.
       Default: every DISCRETE device of the first platform that has one
       (the integrated GPU is excluded by the host-unified-memory property,
       not by name); an iGPU-only box keeps its single device as before. */
    {
        const char *spec = getenv("CNET_GPU_DEVICES");
        if (spec && *spec) {
            const char *s = spec;
            while (*s && n_picked < CLGEMM_MAX_DEV) {
                char *end = NULL;
                unsigned long v = strtoul(s, &end, 10);
                if (end == s) break;                 /* not a number: stop */
                if (v < n_all) {
                    int dup = 0;
                    for (j = 0; j < n_picked; ++j)
                        if (picked[j] == (size_t)v) dup = 1;
                    if (!dup) picked[n_picked++] = (size_t)v;
                }
                s = end;
                while (*s == ',' || *s == ' ') ++s;
            }
            /* an EXPLICIT spec that matches nothing must refuse, not
               silently expand to every discrete device the operator was
               deliberately keeping free */
            if (n_picked == 0) {
                cce_dl_close(h->dll);
                free(h);
                return NULL;
            }
        }
        if (n_picked == 0) {
            size_t home_plat = (size_t)-1;
            for (i = 0; i < n_all; ++i) {
                if (all_unified[i]) continue;
                if (home_plat == (size_t)-1) home_plat = all_plat[i];
                if (all_plat[i] == home_plat && n_picked < CLGEMM_MAX_DEV)
                    picked[n_picked++] = i;
            }
            if (n_picked == 0) picked[n_picked++] = 0;
        }
        count_cap = env_size("CNET_GPU_COUNT", 0);
        if (count_cap > 0 && count_cap < n_picked) n_picked = count_cap;
    }

    /* Per-device init: context, queue, program, kernel. A device that fails
       to initialize is skipped (the name string reports what actually
       opened); zero usable devices = NULL = CPU-only, as before. */
    for (i = 0; i < n_picked; ++i) {
        ClgemmDev *D = &h->d[h->ndev];
        memset(D, 0, sizeof *D);
        D->dev = all_dev[picked[i]];
        D->ctx = CreateContext(NULL, 1, &D->dev, NULL, NULL, &err);
        if (!D->ctx || err != CL_SUCCESS) continue;
        D->q = CreateQueue(D->ctx, D->dev, 0, &err);
        if (!D->q || err != CL_SUCCESS) goto dev_fail;
        D->prog = CreateProgram(D->ctx, 1, &k_src, NULL, &err);
        if (!D->prog || err != CL_SUCCESS) goto dev_fail;
        if (BuildProgram(D->prog, 1, &D->dev, "", NULL, NULL) != CL_SUCCESS)
            goto dev_fail;
        D->kern = CreateKernel(D->prog, "cnet_gemm", &err);
        if (!D->kern || err != CL_SUCCESS) goto dev_fail;
        D->kern_q8 = CreateKernel(D->prog, "cnet_gemm_q8", &err);
        if (!D->kern_q8 || err != CL_SUCCESS) goto dev_fail;
        /* Optional ops: soft-fail → attn/residency return -1, GEMM still works */
        D->kern_rms = CreateKernel(D->prog, "cnet_rms", &err);
        D->kern_add = CreateKernel(D->prog, "cnet_add", &err);
        D->kern_silu = CreateKernel(D->prog, "cnet_silu_mul", &err);
        D->kern_ascores = CreateKernel(D->prog, "cnet_ascores", &err);
        D->kern_asoft = CreateKernel(D->prog, "cnet_asoftmax", &err);
        D->kern_av = CreateKernel(D->prog, "cnet_av", &err);
        h->ndev++;
        continue;
    dev_fail:
        if (D->kern_av) h->ReleaseKernel(D->kern_av);
        if (D->kern_asoft) h->ReleaseKernel(D->kern_asoft);
        if (D->kern_ascores) h->ReleaseKernel(D->kern_ascores);
        if (D->kern_silu) h->ReleaseKernel(D->kern_silu);
        if (D->kern_add) h->ReleaseKernel(D->kern_add);
        if (D->kern_rms) h->ReleaseKernel(D->kern_rms);
        if (D->kern_q8) h->ReleaseKernel(D->kern_q8);
        if (D->kern) h->ReleaseKernel(D->kern);
        if (D->prog) h->ReleaseProgram(D->prog);
        if (D->q) h->ReleaseQueue(D->q);
        if (D->ctx) h->ReleaseContext(D->ctx);
        memset(D, 0, sizeof *D);
    }
    if (h->ndev == 0) { cce_dl_close(h->dll); free(h); return NULL; }

    /* solo mode resolves against the devices that actually OPENED — the
       same index space cce_clgemm_device_count reports — so a lane never
       pins a device the probe already proved dead. */
    if (solo_index >= 0) {
        if ((size_t)solo_index >= h->ndev) {
            for (i = 0; i < h->ndev; ++i) {
                ClgemmDev *D = &h->d[i];
                h->ReleaseKernel(D->kern);
                h->ReleaseProgram(D->prog);
                h->ReleaseQueue(D->q);
                h->ReleaseContext(D->ctx);
            }
            cce_dl_close(h->dll);
            free(h);
            return NULL;
        }
        for (i = 0; i < h->ndev; ++i) {
            ClgemmDev *D = &h->d[i];
            if (i == (size_t)solo_index) continue;
            h->ReleaseKernel(D->kern);
            h->ReleaseProgram(D->prog);
            h->ReleaseQueue(D->q);
            h->ReleaseContext(D->ctx);
        }
        if (solo_index != 0) h->d[0] = h->d[solo_index];
        memset(&h->d[1], 0, (CLGEMM_MAX_DEV - 1) * sizeof h->d[0]);
        h->ndev = 1;
    }

    h->split_bytes = env_size("CNET_GPU_SPLIT_MB", 0) * 1024u * 1024u;
    if (h->split_bytes == 0) h->split_bytes = CLGEMM_SPLIT_BYTES_DEFAULT;
    h->min_flops = env_size("CNET_GPU_MIN_FLOPS", CLGEMM_MIN_FLOPS_DEFAULT);
    if (h->min_flops == 0) h->min_flops = CLGEMM_MIN_FLOPS_DEFAULT;

    if (device_name_out && device_name_cap > 0) {
        char name0[128] = {0}, namei[128];
        int all_same = 1;
        GetDeviceInfo(h->d[0].dev, CL_DEVICE_NAME, sizeof name0 - 1, name0,
                      NULL);
        for (i = 1; i < h->ndev; ++i) {
            memset(namei, 0, sizeof namei);
            GetDeviceInfo(h->d[i].dev, CL_DEVICE_NAME, sizeof namei - 1,
                          namei, NULL);
            if (strcmp(name0, namei) != 0) all_same = 0;
        }
        if (h->ndev == 1)
            snprintf(device_name_out, device_name_cap, "%s", name0);
        else if (all_same)
            snprintf(device_name_out, device_name_cap, "%s x%lu", name0,
                     (unsigned long)h->ndev);
        else
            snprintf(device_name_out, device_name_cap, "%s +%lu more", name0,
                     (unsigned long)(h->ndev - 1));
    }
    return h;
}

cce_clgemm *cce_clgemm_open(const char *dll_name,
                            char *device_name_out, size_t device_name_cap) {
    return clgemm_open_internal(dll_name, -1, device_name_out,
                                device_name_cap);
}

cce_clgemm *cce_clgemm_open_device(const char *dll_name, int device_index,
                                   char *device_name_out,
                                   size_t device_name_cap) {
    if (device_index < 0) return NULL;
    return clgemm_open_internal(dll_name, device_index, device_name_out,
                                device_name_cap);
}

void cce_clgemm_close(cce_clgemm *h) {
    size_t i, j;
    if (!h) return;
    for (i = 0; i < h->resident_count; ++i)
        for (j = 0; j < h->ndev; ++j) {
            if (h->resident[i].mem[j]) h->ReleaseMem(h->resident[i].mem[j]);
            if (h->resident[i].scale[j]) h->ReleaseMem(h->resident[i].scale[j]);
        }
    for (j = 0; j < h->ndev; ++j) {
        ClgemmDev *D = &h->d[j];
        if (D->a_buf) h->ReleaseMem(D->a_buf);
        if (D->c_buf) h->ReleaseMem(D->c_buf);
        if (D->op_a) h->ReleaseMem(D->op_a);
        if (D->op_b) h->ReleaseMem(D->op_b);
        if (D->op_c) h->ReleaseMem(D->op_c);
        if (D->kern_av) h->ReleaseKernel(D->kern_av);
        if (D->kern_asoft) h->ReleaseKernel(D->kern_asoft);
        if (D->kern_ascores) h->ReleaseKernel(D->kern_ascores);
        if (D->kern_silu) h->ReleaseKernel(D->kern_silu);
        if (D->kern_add) h->ReleaseKernel(D->kern_add);
        if (D->kern_rms) h->ReleaseKernel(D->kern_rms);
        if (D->kern_q8) h->ReleaseKernel(D->kern_q8);
        if (D->kern) h->ReleaseKernel(D->kern);
        if (D->prog) h->ReleaseProgram(D->prog);
        if (D->q) h->ReleaseQueue(D->q);
        if (D->ctx) h->ReleaseContext(D->ctx);
        free(D->c_host);
        free(D->pack_host);
    }
    if (h->dll) cce_dl_close(h->dll);
    free(h);
}

size_t cce_clgemm_device_count(const cce_clgemm *h) {
    return h ? h->ndev : 0;
}

size_t cce_clgemm_resident_bytes(const cce_clgemm *h) {
    size_t i, total = 0;
    if (!h) return 0;
    for (i = 0; i < h->resident_count; ++i) total += h->resident[i].bytes;
    return total;
}

/* Upload columns [off, off+len) of host[K x N] to device di, packed as
   [K x len]. len == N uploads the host array as-is (no packing copy). */
static cl_mem upload_cols(cce_clgemm *h, size_t di, const float *host,
                          size_t K, size_t N, size_t off, size_t len) {
    cl_int err = 0;
    cl_mem mem;
    if (len == N) {
        mem = h->CreateBuffer(h->d[di].ctx,
                              CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                              K * N * sizeof(float), (void *)host, &err);
        return (mem && err == CL_SUCCESS) ? mem : NULL;
    }
    {
        float *tmp = (float *)malloc(K * len * sizeof(float));
        size_t k;
        if (!tmp) return NULL;
        for (k = 0; k < K; ++k)
            memcpy(tmp + k * len, host + k * N + off, len * sizeof(float));
        mem = h->CreateBuffer(h->d[di].ctx,
                              CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                              K * len * sizeof(float), tmp, &err);
        free(tmp);
        return (mem && err == CL_SUCCESS) ? mem : NULL;
    }
}

/* Upload columns [off, off+len) of q8 host[K x N] to device di, packed as
   [K x len] int8. len == N uploads as-is. */
static cl_mem upload_cols_q8(cce_clgemm *h, size_t di, const int8_t *host,
                             size_t K, size_t N, size_t off, size_t len) {
    cl_int err = 0;
    cl_mem mem;
    if (len == N) {
        mem = h->CreateBuffer(h->d[di].ctx,
                              CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                              K * N, (void *)host, &err);
        return (mem && err == CL_SUCCESS) ? mem : NULL;
    }
    {
        int8_t *tmp = (int8_t *)malloc(K * len);
        size_t k;
        if (!tmp) return NULL;
        for (k = 0; k < K; ++k)
            memcpy(tmp + k * len, host + k * N + off, len);
        mem = h->CreateBuffer(h->d[di].ctx,
                              CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                              K * len, tmp, &err);
        free(tmp);
        return (mem && err == CL_SUCCESS) ? mem : NULL;
    }
}

static ResidentBuf *resident_find(cce_clgemm *h, const void *host,
                                  size_t bytes) {
    size_t i;
    for (i = 0; i < h->resident_count; ++i)
        if (h->resident[i].host == host && h->resident[i].bytes == bytes)
            return &h->resident[i];
    return NULL;
}

/* Create a resident entry for host[K x N] with the given placement.
   On any device failure the partial entry is released and NULL returned
   (caller falls back to CPU, as always). */
static ResidentBuf *resident_create(cce_clgemm *h, const void *host,
                                    size_t K, size_t N, int split,
                                    size_t owner, const size_t *off,
                                    const size_t *len) {
    ResidentBuf *r;
    size_t d;
    if (h->resident_count >= CLGEMM_MAX_RESIDENT) return NULL;
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
        r->mem[d] = upload_cols(h, d, (const float *)host, K, N, off[d],
                                len[d]);
        if (!r->mem[d]) {
            size_t e;
            for (e = 0; e < d; ++e)
                if (r->mem[e]) h->ReleaseMem(r->mem[e]);
            memset(r, 0, sizeof *r);
            return NULL;
        }
    }
    h->resident_count++;
    return r;
}

/* Create a resident entry for int8 host[K x N] + per-column scales with
   the given placement. NULL on any failure (caller falls back to CPU). */
static ResidentBuf *resident_create_q8(cce_clgemm *h, const int8_t *host,
                                       const float *scales, size_t K,
                                       size_t N, int split, size_t owner,
                                       const size_t *off, const size_t *len) {
    ResidentBuf *r;
    size_t d;
    cl_int err = 0;
    if (h->resident_count >= CLGEMM_MAX_RESIDENT) return NULL;
    r = &h->resident[h->resident_count];
    memset(r, 0, sizeof *r);
    r->host = host;
    r->bytes = K * N;                /* int8: one byte per weight */
    r->split = split;
    r->is_q8 = 1;
    r->owner = owner;
    for (d = 0; d < h->ndev; ++d) {
        r->off[d] = off[d];
        r->len[d] = len[d];
        if (len[d] == 0) continue;
        r->mem[d] = upload_cols_q8(h, d, host, K, N, off[d], len[d]);
        if (r->mem[d]) {
            if (len[d] == N) {
                r->scale[d] = h->CreateBuffer(
                    h->d[d].ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                    N * sizeof(float), (void *)scales, &err);
                if (err != CL_SUCCESS) r->scale[d] = NULL;
            } else {
                r->scale[d] = h->CreateBuffer(
                    h->d[d].ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                    len[d] * sizeof(float), (void *)(scales + off[d]), &err);
                if (err != CL_SUCCESS) r->scale[d] = NULL;
            }
        }
        if (!r->mem[d] || !r->scale[d]) {
            size_t e;
            for (e = 0; e <= d; ++e) {
                if (r->mem[e]) h->ReleaseMem(r->mem[e]);
                if (r->scale[e]) h->ReleaseMem(r->scale[e]);
            }
            memset(r, 0, sizeof *r);
            return NULL;
        }
    }
    h->resident_count++;
    return r;
}

/* Decide placement for a [K x N] weight matrix: column-split across all
   devices when big enough to be bandwidth-bound (each device then reads
   only its share), whole on one round-robin device otherwise (a second
   queue round-trip costs more than a small matrix's reads save). */
static void place_weights(cce_clgemm *h, size_t K, size_t N, int *split,
                          size_t *owner, size_t *off, size_t *len) {
    size_t d;
    for (d = 0; d < h->ndev; ++d) { off[d] = 0; len[d] = 0; }
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

static int scratch_ensure(cce_clgemm *h, size_t di, cl_mem *buf, size_t *cap,
                          size_t bytes) {
    cl_int err = 0;
    if (*cap >= bytes && *buf) return 0;
    if (*buf) h->ReleaseMem(*buf);
    *buf = h->CreateBuffer(h->d[di].ctx, CL_MEM_READ_WRITE, bytes, NULL, &err);
    if (!*buf || err != CL_SUCCESS) { *buf = NULL; *cap = 0; return -1; }
    *cap = bytes;
    /* buffer reallocated — cached A no longer lives here */
    h->d[di].a_fp_host = NULL;
    h->d[di].a_fp_bytes = 0;
    h->d[di].a_fp_tag = 0;
    return 0;
}

/* Cheap content tag: size + strided samples (catches layer-buffer reuse). */
static uint64_t a_fingerprint(const float *A, size_t nfloat) {
    uint64_t h = 14695981039346656037ULL ^ (nfloat * 0x9E3779B97F4A7C15ULL);
    size_t i, step;
    if (!A || nfloat == 0) return 0;
    step = nfloat > 64 ? nfloat / 64 : 1;
    for (i = 0; i < nfloat; i += step) {
        uint32_t u;
        memcpy(&u, &A[i], 4);
        h ^= (uint64_t)u + 0x9e3779b9u + (h << 6) + (h >> 2);
    }
    if (nfloat > 1) {
        uint32_t u;
        memcpy(&u, &A[nfloat - 1], 4);
        h ^= (uint64_t)u;
    }
    return h;
}

/* Upload A unless this device already holds the same rows. */
static int ensure_A(cce_clgemm *h, size_t di, const float *A, size_t T,
                    size_t K) {
    ClgemmDev *D = &h->d[di];
    size_t bytes = T * K * sizeof(float);
    uint64_t tag;
    if (scratch_ensure(h, di, &D->a_buf, &D->a_cap, bytes) != 0) return -1;
    tag = a_fingerprint(A, T * K);
    if (D->a_fp_host == A && D->a_fp_bytes == bytes && D->a_fp_tag == tag)
        return 0;
    if (h->WriteBuffer(D->q, D->a_buf, CL_FALSE, 0, bytes, A, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    D->a_fp_host = A;
    D->a_fp_bytes = bytes;
    D->a_fp_tag = tag;
    return 0;
}

/* global[0] rounded up to 64 for local A-tile work-group. */
static int enqueue_2d(cce_clgemm *h, cl_command_queue q, cl_kernel kern,
                      size_t cols, size_t T) {
    size_t local[2], global[2];
    local[0] = 64;
    local[1] = 1;
    global[0] = ((cols + 63) / 64) * 64;
    if (global[0] == 0) global[0] = 64;
    global[1] = T < 1 ? 1 : T;
    return h->Enqueue(q, kern, 2, NULL, global, local, 0, NULL, NULL) ==
                   CL_SUCCESS
               ? 0
               : -1;
}

/* C[T x N] = (bias?) + scale[n] * (A[T x K] . (float)Wq[K x N]) — the
   int8 seam. Same accumulation order as cce_block's int8 matvec and the
   same FP_CONTRACT OFF kernel discipline: BIT-identical to the CPU path,
   so per-matrix CPU fallback (e.g. VRAM pressure) cannot move a decision. */
int cce_clgemm_matmul_q8(cce_clgemm *h, const float *A, size_t T, size_t K,
                         const int8_t *Wq, const float *scales,
                         const float *bias, size_t N, float *C) {
    ResidentBuf *went, *bent = NULL;
    int has_bias, rc = 0;
    int started[CLGEMM_MAX_DEV] = {0};
    size_t d, t;

    if (!h || !A || !Wq || !scales || !C || T == 0 || T > 8 || K == 0 ||
        N == 0) return -1;
    if ((size_t)T * K * N < h->min_flops) return -1;
    if (K > 0x7FFFFFFF || N > 0x7FFFFFFF) return -1;

    went = resident_find(h, Wq, K * N);
    if (!went) {
        int split;
        size_t owner, off[CLGEMM_MAX_DEV], len[CLGEMM_MAX_DEV];
        size_t d2;
        for (d2 = 0; d2 < CLGEMM_MAX_DEV; ++d2) { off[d2] = 0; len[d2] = 0; }
        if (h->ndev > 1 && K * N >= h->split_bytes && N >= h->ndev) {
            size_t base = N / h->ndev, rem = N % h->ndev, at = 0;
            split = 1; owner = 0;
            for (d2 = 0; d2 < h->ndev; ++d2) {
                len[d2] = base + (d2 < rem ? 1 : 0);
                off[d2] = at;
                at += len[d2];
            }
        } else {
            split = 0;
            owner = h->rr++ % h->ndev;
            len[owner] = N;
        }
        went = resident_create_q8(h, Wq, scales, K, N, split, owner, off,
                                  len);
        if (!went) return -1;
    }
    if (bias) {
        bent = resident_find(h, bias, N * sizeof(float));
        if (!bent) {
            bent = resident_create(h, bias, 1, N, went->split, went->owner,
                                   went->off, went->len);
            if (!bent) return -1;
        }
    }
    has_bias = bias ? 1 : 0;

    for (d = 0; d < h->ndev; ++d) {
        ClgemmDev *D = &h->d[d];
        size_t cols = went->len[d];
        int iT, iK, iN;
        cl_mem b_mem;
        float *dst;
        if (cols == 0) continue;
        if (ensure_A(h, d, A, T, K) != 0 ||
            scratch_ensure(h, d, &D->c_buf, &D->c_cap,
                           T * cols * sizeof(float)) != 0) {
            rc = -1;
            break;
        }
        if (went->split) {
            if (D->c_host_cap < T * cols) {
                float *nc = (float *)realloc(D->c_host,
                                             T * cols * sizeof(float));
                if (!nc) { rc = -1; break; }
                D->c_host = nc;
                D->c_host_cap = T * cols;
            }
            dst = D->c_host;
        } else {
            dst = C;
        }
        started[d] = 1;
        b_mem = bent ? bent->mem[d] : went->scale[d];  /* dummy bind, unread */
        iT = (int)T; iK = (int)K; iN = (int)cols;
        if (h->SetArg(D->kern_q8, 0, sizeof(cl_mem), &D->a_buf) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 1, sizeof(cl_mem), &went->mem[d]) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 2, sizeof(cl_mem), &went->scale[d]) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 3, sizeof(cl_mem), &b_mem) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 4, sizeof(cl_mem), &D->c_buf) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 5, sizeof(int), &iT) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 6, sizeof(int), &iK) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 7, sizeof(int), &iN) != CL_SUCCESS ||
            h->SetArg(D->kern_q8, 8, sizeof(int), &has_bias) != CL_SUCCESS) {
            rc = -1;
            break;
        }
        if (enqueue_2d(h, D->q, D->kern_q8, cols, (size_t)T) != 0) {
            rc = -1;
            break;
        }
        if (h->ReadBuffer(D->q, D->c_buf, CL_FALSE, 0,
                          T * cols * sizeof(float), dst, 0, NULL,
                          NULL) != CL_SUCCESS) {
            rc = -1;
            break;
        }
        h->Flush(D->q);
    }

    for (d = 0; d < h->ndev; ++d)
        if (started[d] && h->Finish(h->d[d].q) != CL_SUCCESS) rc = -1;
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
    return 0;
}

int cce_clgemm_matmul(cce_clgemm *h, const float *A, size_t T, size_t K,
                      const float *W, const float *bias, size_t N, float *C) {
    ResidentBuf *went, *bent = NULL;
    int has_bias, rc = 0;
    int started[CLGEMM_MAX_DEV] = {0};
    size_t d, t;

    if (!h || !A || !W || !C || T == 0 || T > 8 || K == 0 || N == 0) return -1;
    if ((size_t)T * K * N < h->min_flops) return -1;
    if (K > 0x7FFFFFFF || N > 0x7FFFFFFF) return -1;

    went = resident_find(h, W, K * N * sizeof(float));
    if (!went) {
        int split;
        size_t owner, off[CLGEMM_MAX_DEV], len[CLGEMM_MAX_DEV];
        place_weights(h, K, N, &split, &owner, off, len);
        went = resident_create(h, W, K, N, split, owner, off, len);
        if (!went) return -1;
    }
    /* the bias rides with its weight matrix: same devices, same columns */
    if (bias) {
        bent = resident_find(h, bias, N * sizeof(float));
        if (!bent) {
            bent = resident_create(h, bias, 1, N, went->split, went->owner,
                                   went->off, went->len);
            if (!bent) return -1;
        }
    }
    has_bias = bias ? 1 : 0;

    for (d = 0; d < h->ndev; ++d) {
        ClgemmDev *D = &h->d[d];
        size_t cols = went->len[d];
        int iT, iK, iN;
        cl_mem b_mem;
        float *dst;
        if (cols == 0) continue;
        if (ensure_A(h, d, A, T, K) != 0 ||
            scratch_ensure(h, d, &D->c_buf, &D->c_cap,
                           T * cols * sizeof(float)) != 0) {
            rc = -1;
            break;
        }
        if (went->split) {
            if (D->c_host_cap < T * cols) {
                float *nc = (float *)realloc(D->c_host,
                                             T * cols * sizeof(float));
                if (!nc) { rc = -1; break; }
                D->c_host = nc;
                D->c_host_cap = T * cols;
            }
            dst = D->c_host;
        } else {
            dst = C;   /* cols == N: the kernel's layout IS the caller's */
        }
        started[d] = 1;
        b_mem = bent ? bent->mem[d] : went->mem[d];   /* dummy bind, unread */
        iT = (int)T; iK = (int)K; iN = (int)cols;
        if (h->SetArg(D->kern, 0, sizeof(cl_mem), &D->a_buf) != CL_SUCCESS ||
            h->SetArg(D->kern, 1, sizeof(cl_mem), &went->mem[d]) != CL_SUCCESS ||
            h->SetArg(D->kern, 2, sizeof(cl_mem), &b_mem) != CL_SUCCESS ||
            h->SetArg(D->kern, 3, sizeof(cl_mem), &D->c_buf) != CL_SUCCESS ||
            h->SetArg(D->kern, 4, sizeof(int), &iT) != CL_SUCCESS ||
            h->SetArg(D->kern, 5, sizeof(int), &iK) != CL_SUCCESS ||
            h->SetArg(D->kern, 6, sizeof(int), &iN) != CL_SUCCESS ||
            h->SetArg(D->kern, 7, sizeof(int), &has_bias) != CL_SUCCESS) {
            rc = -1;
            break;
        }
        if (enqueue_2d(h, D->q, D->kern, cols, (size_t)T) != 0) {
            rc = -1;
            break;
        }
        if (h->ReadBuffer(D->q, D->c_buf, CL_FALSE, 0,
                          T * cols * sizeof(float), dst, 0, NULL,
                          NULL) != CL_SUCCESS) {
            rc = -1;
            break;
        }
        h->Flush(D->q);   /* start this device NOW, then feed the next one */
    }

    /* Drain every started queue even on failure: a stray in-flight read
       must not land in C after the caller has fallen back to the CPU path. */
    for (d = 0; d < h->ndev; ++d)
        if (started[d] && h->Finish(h->d[d].q) != CL_SUCCESS) rc = -1;
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
    return 0;
}

/* ---- Residency ops + attention (device 0) -------------------------------- */

static int op_scratch(cce_clgemm *h, cl_mem *buf, size_t *cap, size_t bytes) {
    cl_int err = 0;
    ClgemmDev *D;
    if (!h || h->ndev < 1) return -1;
    D = &h->d[0];
    if (*cap >= bytes && *buf) return 0;
    if (*buf) h->ReleaseMem(*buf);
    *buf = h->CreateBuffer(D->ctx, CL_MEM_READ_WRITE, bytes, NULL, &err);
    if (!*buf || err != CL_SUCCESS) { *buf = NULL; *cap = 0; return -1; }
    *cap = bytes;
    return 0;
}

int cce_clgemm_rms_norm(cce_clgemm *h, const float *x, const float *w,
                        float *y, int D, float eps) {
    ClgemmDev *Dv;
    int has_w, iD;
    size_t g;
    if (!h || !x || !y || D < 1 || h->ndev < 1) return -1;
    Dv = &h->d[0];
    if (!Dv->kern_rms) return -1;
    if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap, (size_t)D * 4) != 0 ||
        op_scratch(h, &Dv->op_b, &Dv->op_b_cap, (size_t)D * 4) != 0 ||
        op_scratch(h, &Dv->op_c, &Dv->op_c_cap, (size_t)D * 4) != 0)
        return -1;
    has_w = w ? 1 : 0;
    iD = D;
    if (h->WriteBuffer(Dv->q, Dv->op_a, CL_FALSE, 0, (size_t)D * 4, x, 0, NULL,
                       NULL) != CL_SUCCESS)
        return -1;
    if (has_w &&
        h->WriteBuffer(Dv->q, Dv->op_b, CL_FALSE, 0, (size_t)D * 4, w, 0, NULL,
                       NULL) != CL_SUCCESS)
        return -1;
    if (h->SetArg(Dv->kern_rms, 0, sizeof(cl_mem), &Dv->op_a) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms, 1, sizeof(cl_mem), &Dv->op_b) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms, 2, sizeof(cl_mem), &Dv->op_c) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms, 3, sizeof(int), &iD) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms, 4, sizeof(float), &eps) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms, 5, sizeof(int), &has_w) != CL_SUCCESS)
        return -1;
    g = 1;
    if (h->Enqueue(Dv->q, Dv->kern_rms, 1, NULL, &g, NULL, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    if (h->ReadBuffer(Dv->q, Dv->op_c, CL_TRUE, 0, (size_t)D * 4, y, 0, NULL,
                      NULL) != CL_SUCCESS)
        return -1;
    return 0;
}

int cce_clgemm_add(cce_clgemm *h, const float *a, const float *b, float *y,
                   int n) {
    ClgemmDev *Dv;
    int in;
    size_t g;
    if (!h || !a || !b || !y || n < 1 || h->ndev < 1) return -1;
    Dv = &h->d[0];
    if (!Dv->kern_add) return -1;
    if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap, (size_t)n * 4) != 0 ||
        op_scratch(h, &Dv->op_b, &Dv->op_b_cap, (size_t)n * 4) != 0 ||
        op_scratch(h, &Dv->op_c, &Dv->op_c_cap, (size_t)n * 4) != 0)
        return -1;
    in = n;
    if (h->WriteBuffer(Dv->q, Dv->op_a, CL_FALSE, 0, (size_t)n * 4, a, 0, NULL,
                       NULL) != CL_SUCCESS ||
        h->WriteBuffer(Dv->q, Dv->op_b, CL_FALSE, 0, (size_t)n * 4, b, 0, NULL,
                       NULL) != CL_SUCCESS)
        return -1;
    if (h->SetArg(Dv->kern_add, 0, sizeof(cl_mem), &Dv->op_a) != CL_SUCCESS ||
        h->SetArg(Dv->kern_add, 1, sizeof(cl_mem), &Dv->op_b) != CL_SUCCESS ||
        h->SetArg(Dv->kern_add, 2, sizeof(cl_mem), &Dv->op_c) != CL_SUCCESS ||
        h->SetArg(Dv->kern_add, 3, sizeof(int), &in) != CL_SUCCESS)
        return -1;
    g = (size_t)((n + 63) / 64) * 64;
    if (g == 0) g = 64;
    {
        size_t loc = 64;
        if (h->Enqueue(Dv->q, Dv->kern_add, 1, NULL, &g, &loc, 0, NULL, NULL) !=
            CL_SUCCESS)
            return -1;
    }
    if (h->ReadBuffer(Dv->q, Dv->op_c, CL_TRUE, 0, (size_t)n * 4, y, 0, NULL,
                      NULL) != CL_SUCCESS)
        return -1;
    return 0;
}

int cce_clgemm_silu_mul(cce_clgemm *h, const float *gate, const float *up,
                        float *y, int n) {
    ClgemmDev *Dv;
    int in;
    size_t g, loc;
    if (!h || !gate || !up || !y || n < 1 || h->ndev < 1) return -1;
    Dv = &h->d[0];
    if (!Dv->kern_silu) return -1;
    if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap, (size_t)n * 4) != 0 ||
        op_scratch(h, &Dv->op_b, &Dv->op_b_cap, (size_t)n * 4) != 0 ||
        op_scratch(h, &Dv->op_c, &Dv->op_c_cap, (size_t)n * 4) != 0)
        return -1;
    in = n;
    if (h->WriteBuffer(Dv->q, Dv->op_a, CL_FALSE, 0, (size_t)n * 4, gate, 0,
                       NULL, NULL) != CL_SUCCESS ||
        h->WriteBuffer(Dv->q, Dv->op_b, CL_FALSE, 0, (size_t)n * 4, up, 0, NULL,
                       NULL) != CL_SUCCESS)
        return -1;
    if (h->SetArg(Dv->kern_silu, 0, sizeof(cl_mem), &Dv->op_a) != CL_SUCCESS ||
        h->SetArg(Dv->kern_silu, 1, sizeof(cl_mem), &Dv->op_b) != CL_SUCCESS ||
        h->SetArg(Dv->kern_silu, 2, sizeof(cl_mem), &Dv->op_c) != CL_SUCCESS ||
        h->SetArg(Dv->kern_silu, 3, sizeof(int), &in) != CL_SUCCESS)
        return -1;
    g = (size_t)((n + 63) / 64) * 64;
    if (g == 0) g = 64;
    loc = 64;
    if (h->Enqueue(Dv->q, Dv->kern_silu, 1, NULL, &g, &loc, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    if (h->ReadBuffer(Dv->q, Dv->op_c, CL_TRUE, 0, (size_t)n * 4, y, 0, NULL,
                      NULL) != CL_SUCCESS)
        return -1;
    return 0;
}

int cce_clgemm_attn_decode(cce_clgemm *h, const float *q, const float *k_cache,
                           const float *v_cache, size_t k_slot, size_t v_slot,
                           size_t k_off, size_t v_off, int n_q, int n_k,
                           int n_v, int head_dim, int v_head_dim, int jmin,
                           int abs_t, float scale, float *attn_out) {
    ClgemmDev *Dv;
    int seq, hi, j, d;
    size_t need, g, loc;
    int iseq, ihd, ivd;
    if (!h || !q || !k_cache || !v_cache || !attn_out || h->ndev < 1)
        return -1;
    if (n_q < 1 || head_dim < 1 || v_head_dim < 1 || abs_t < jmin) return -1;
    Dv = &h->d[0];
    if (!Dv->kern_ascores || !Dv->kern_asoft || !Dv->kern_av) return -1;
    seq = abs_t - jmin + 1;
    if (seq < 1) return -1;
    need = (size_t)seq * (size_t)(head_dim > v_head_dim ? head_dim : v_head_dim);
    if (Dv->pack_cap < need) {
        float *np = (float *)realloc(Dv->pack_host, need * sizeof(float));
        if (!np) return -1;
        Dv->pack_host = np;
        Dv->pack_cap = need;
    }
    if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap,
                   (size_t)head_dim * 4) != 0 || /* q */
        op_scratch(h, &Dv->op_b, &Dv->op_b_cap,
                   (size_t)seq * (size_t)head_dim * 4) != 0 || /* K pack */
        op_scratch(h, &Dv->op_c, &Dv->op_c_cap,
                   (size_t)seq * 4 > (size_t)v_head_dim * 4
                       ? (size_t)seq * 4
                       : (size_t)v_head_dim * 4) != 0)
        return -1;

    for (hi = 0; hi < n_q; ++hi) {
        int kh = hi / (n_q / n_k);
        int vh = hi / (n_q / n_v);
        const float *qh = q + (size_t)hi * (size_t)head_dim;
        float *oh = attn_out + (size_t)hi * (size_t)v_head_dim;
        /* pack K rows for this head */
        for (j = 0; j < seq; ++j) {
            const float *kr =
                k_cache + (size_t)(jmin + j) * k_slot + k_off +
                (size_t)kh * (size_t)head_dim;
            memcpy(Dv->pack_host + (size_t)j * (size_t)head_dim, kr,
                   (size_t)head_dim * sizeof(float));
        }
        if (h->WriteBuffer(Dv->q, Dv->op_a, CL_FALSE, 0,
                           (size_t)head_dim * 4, qh, 0, NULL, NULL) !=
                CL_SUCCESS ||
            h->WriteBuffer(Dv->q, Dv->op_b, CL_FALSE, 0,
                           (size_t)seq * (size_t)head_dim * 4, Dv->pack_host, 0,
                           NULL, NULL) != CL_SUCCESS)
            return -1;
        iseq = seq;
        ihd = head_dim;
        if (h->SetArg(Dv->kern_ascores, 0, sizeof(cl_mem), &Dv->op_a) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores, 1, sizeof(cl_mem), &Dv->op_b) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores, 2, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores, 3, sizeof(int), &iseq) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores, 4, sizeof(int), &ihd) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores, 5, sizeof(float), &scale) != CL_SUCCESS)
            return -1;
        g = (size_t)((seq + 63) / 64) * 64;
        if (g == 0) g = 64;
        loc = 64;
        if (h->Enqueue(Dv->q, Dv->kern_ascores, 1, NULL, &g, &loc, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        /* softmax on scores in op_c[0:seq] */
        if (h->SetArg(Dv->kern_asoft, 0, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_asoft, 1, sizeof(int), &iseq) != CL_SUCCESS)
            return -1;
        g = 1;
        if (h->Enqueue(Dv->q, Dv->kern_asoft, 1, NULL, &g, NULL, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        /* pack V */
        for (j = 0; j < seq; ++j) {
            const float *vr =
                v_cache + (size_t)(jmin + j) * v_slot + v_off +
                (size_t)vh * (size_t)v_head_dim;
            memcpy(Dv->pack_host + (size_t)j * (size_t)v_head_dim, vr,
                   (size_t)v_head_dim * sizeof(float));
        }
        if (op_scratch(h, &Dv->op_b, &Dv->op_b_cap,
                       (size_t)seq * (size_t)v_head_dim * 4) != 0)
            return -1;
        if (h->WriteBuffer(Dv->q, Dv->op_b, CL_FALSE, 0,
                           (size_t)seq * (size_t)v_head_dim * 4, Dv->pack_host,
                           0, NULL, NULL) != CL_SUCCESS)
            return -1;
        /* out in op_a (reuse) — ensure size */
        if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap,
                       (size_t)v_head_dim * 4) != 0)
            return -1;
        ivd = v_head_dim;
        if (h->SetArg(Dv->kern_av, 0, sizeof(cl_mem), &Dv->op_c) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av, 1, sizeof(cl_mem), &Dv->op_b) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av, 2, sizeof(cl_mem), &Dv->op_a) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av, 3, sizeof(int), &iseq) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av, 4, sizeof(int), &ivd) != CL_SUCCESS)
            return -1;
        g = (size_t)((v_head_dim + 63) / 64) * 64;
        if (g == 0) g = 64;
        loc = 64;
        if (h->Enqueue(Dv->q, Dv->kern_av, 1, NULL, &g, &loc, 0, NULL, NULL) !=
            CL_SUCCESS)
            return -1;
        if (h->ReadBuffer(Dv->q, Dv->op_a, CL_TRUE, 0, (size_t)v_head_dim * 4,
                          oh, 0, NULL, NULL) != CL_SUCCESS)
            return -1;
        (void)d;
    }
    return 0;
}
