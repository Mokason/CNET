#include "../../include/cce/cce_clgemm.h"

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
#define CL_DEVICE_NAME 0x102B
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
typedef cl_int (*p_clFinish)(cl_command_queue);
typedef cl_int (*p_clReleaseMemObject)(cl_mem);
typedef cl_int (*p_clReleaseKernel)(cl_kernel);
typedef cl_int (*p_clReleaseProgram)(cl_program);
typedef cl_int (*p_clReleaseCommandQueue)(cl_command_queue);
typedef cl_int (*p_clReleaseContext)(cl_context);

/* C[t*N+n] = (bias? B[n]:0) + sum_k A[t*K+k]*W[k*N+n]; adjacent work-items
   read adjacent W entries (coalesced across the wide N dimension). */
static const char *k_src =
    "__kernel void cnet_gemm(__global const float* A,\n"
    "                        __global const float* W,\n"
    "                        __global const float* B,\n"
    "                        __global float* C,\n"
    "                        const int T, const int K, const int N,\n"
    "                        const int has_bias) {\n"
    "    int n = get_global_id(0);\n"
    "    int t = get_global_id(1);\n"
    "    if (n >= N || t >= T) return;\n"
    "    float acc = has_bias ? B[n] : 0.0f;\n"
    "    __global const float* a = A + (size_t)t * K;\n"
    "    for (int k = 0; k < K; ++k) acc += a[k] * W[(size_t)k * N + n];\n"
    "    C[(size_t)t * N + n] = acc;\n"
    "}\n";

#define CLGEMM_MAX_RESIDENT 128

typedef struct {
    const void *host;
    cl_mem mem;
    size_t bytes;
} ResidentBuf;

struct cce_clgemm {
    cce_dl dll;
    cl_context ctx;
    cl_command_queue q;
    cl_program prog;
    cl_kernel kern;
    cl_device_id dev;
    ResidentBuf resident[CLGEMM_MAX_RESIDENT];
    size_t resident_count;
    cl_mem a_buf, c_buf;         /* reusable activation/output scratch */
    size_t a_cap, c_cap;
    /* function pointers */
    p_clCreateBuffer CreateBuffer;
    p_clEnqueueWriteBuffer WriteBuffer;
    p_clEnqueueReadBuffer ReadBuffer;
    p_clSetKernelArg SetArg;
    p_clEnqueueNDRangeKernel Enqueue;
    p_clFinish Finish;
    p_clReleaseMemObject ReleaseMem;
    p_clReleaseKernel ReleaseKernel;
    p_clReleaseProgram ReleaseProgram;
    p_clReleaseCommandQueue ReleaseQueue;
    p_clReleaseContext ReleaseContext;
};

cce_clgemm *cce_clgemm_open(const char *dll_name,
                            char *device_name_out, size_t device_name_cap) {
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
    cl_uint n_plat = 0, i;
    cl_int err = 0;

    h = (cce_clgemm *)calloc(1, sizeof *h);
    if (!h) return NULL;

    h->dll = cce_dl_open(dll_name ? dll_name : CCE_CL_DEFAULT_DLL);
    if (!h->dll) { free(h); return NULL; }

#define SYM(dst, type, name)                                                \
    do {                                                                    \
        dst = (type)cce_dl_sym(h->dll, name);                               \
        if (!dst) { cce_dl_close(h->dll); free(h); return NULL; }           \
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
    /* first platform that exposes a GPU device wins */
    for (i = 0; i < n_plat; ++i) {
        if (GetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 1, &h->dev, NULL) ==
            CL_SUCCESS) {
            break;
        }
    }
    if (i == n_plat) { cce_dl_close(h->dll); free(h); return NULL; }

    if (device_name_out && device_name_cap > 0) {
        device_name_out[0] = '\0';
        GetDeviceInfo(h->dev, CL_DEVICE_NAME, device_name_cap - 1,
                      device_name_out, NULL);
        device_name_out[device_name_cap - 1] = '\0';
    }

    h->ctx = CreateContext(NULL, 1, &h->dev, NULL, NULL, &err);
    if (!h->ctx || err != CL_SUCCESS) { cce_dl_close(h->dll); free(h); return NULL; }
    h->q = CreateQueue(h->ctx, h->dev, 0, &err);
    if (!h->q || err != CL_SUCCESS) goto fail;
    h->prog = CreateProgram(h->ctx, 1, &k_src, NULL, &err);
    if (!h->prog || err != CL_SUCCESS) goto fail;
    if (BuildProgram(h->prog, 1, &h->dev, "", NULL, NULL) != CL_SUCCESS) {
        goto fail;
    }
    h->kern = CreateKernel(h->prog, "cnet_gemm", &err);
    if (!h->kern || err != CL_SUCCESS) goto fail;
    return h;

fail:
    cce_clgemm_close(h);
    return NULL;
}

void cce_clgemm_close(cce_clgemm *h) {
    size_t i;
    if (!h) return;
    for (i = 0; i < h->resident_count; ++i)
        if (h->resident[i].mem) h->ReleaseMem(h->resident[i].mem);
    if (h->a_buf) h->ReleaseMem(h->a_buf);
    if (h->c_buf) h->ReleaseMem(h->c_buf);
    if (h->kern) h->ReleaseKernel(h->kern);
    if (h->prog) h->ReleaseProgram(h->prog);
    if (h->q) h->ReleaseQueue(h->q);
    if (h->ctx) h->ReleaseContext(h->ctx);
    if (h->dll) cce_dl_close(h->dll);
    free(h);
}

size_t cce_clgemm_resident_bytes(const cce_clgemm *h) {
    size_t i, total = 0;
    if (!h) return 0;
    for (i = 0; i < h->resident_count; ++i) total += h->resident[i].bytes;
    return total;
}

/* Device-resident buffer for a stable host array (weights/bias): uploaded
   once, keyed by host pointer. */
static cl_mem resident_get(cce_clgemm *h, const void *host, size_t bytes) {
    size_t i;
    cl_int err = 0;
    cl_mem mem;
    for (i = 0; i < h->resident_count; ++i)
        if (h->resident[i].host == host && h->resident[i].bytes == bytes)
            return h->resident[i].mem;
    if (h->resident_count >= CLGEMM_MAX_RESIDENT) return NULL;
    mem = h->CreateBuffer(h->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                          bytes, (void *)host, &err);
    if (!mem || err != CL_SUCCESS) return NULL;
    h->resident[h->resident_count].host = host;
    h->resident[h->resident_count].mem = mem;
    h->resident[h->resident_count].bytes = bytes;
    h->resident_count++;
    return mem;
}

static int scratch_ensure(cce_clgemm *h, cl_mem *buf, size_t *cap,
                          size_t bytes) {
    cl_int err = 0;
    if (*cap >= bytes && *buf) return 0;
    if (*buf) h->ReleaseMem(*buf);
    *buf = h->CreateBuffer(h->ctx, CL_MEM_READ_WRITE, bytes, NULL, &err);
    if (!*buf || err != CL_SUCCESS) { *buf = NULL; *cap = 0; return -1; }
    *cap = bytes;
    return 0;
}

int cce_clgemm_matmul(cce_clgemm *h, const float *A, size_t T, size_t K,
                      const float *W, const float *bias, size_t N, float *C) {
    cl_mem w_mem, b_mem;
    int iT, iK, iN, has_bias;
    size_t global[2];

    if (!h || !A || !W || !C || T == 0 || T > 8 || K == 0 || N == 0) return -1;
    if (K > 0x7FFFFFFF || N > 0x7FFFFFFF) return -1;

    w_mem = resident_get(h, W, K * N * sizeof(float));
    if (!w_mem) return -1;
    /* a bias buffer is always bound (kernel signature); reuse W when absent */
    b_mem = bias ? resident_get(h, bias, N * sizeof(float)) : w_mem;
    if (!b_mem) return -1;
    has_bias = bias ? 1 : 0;

    if (scratch_ensure(h, &h->a_buf, &h->a_cap, T * K * sizeof(float)) != 0)
        return -1;
    if (scratch_ensure(h, &h->c_buf, &h->c_cap, T * N * sizeof(float)) != 0)
        return -1;
    if (h->WriteBuffer(h->q, h->a_buf, CL_TRUE, 0, T * K * sizeof(float), A,
                       0, NULL, NULL) != CL_SUCCESS) {
        return -1;
    }

    iT = (int)T; iK = (int)K; iN = (int)N;
    if (h->SetArg(h->kern, 0, sizeof(cl_mem), &h->a_buf) != CL_SUCCESS ||
        h->SetArg(h->kern, 1, sizeof(cl_mem), &w_mem) != CL_SUCCESS ||
        h->SetArg(h->kern, 2, sizeof(cl_mem), &b_mem) != CL_SUCCESS ||
        h->SetArg(h->kern, 3, sizeof(cl_mem), &h->c_buf) != CL_SUCCESS ||
        h->SetArg(h->kern, 4, sizeof(int), &iT) != CL_SUCCESS ||
        h->SetArg(h->kern, 5, sizeof(int), &iK) != CL_SUCCESS ||
        h->SetArg(h->kern, 6, sizeof(int), &iN) != CL_SUCCESS ||
        h->SetArg(h->kern, 7, sizeof(int), &has_bias) != CL_SUCCESS) {
        return -1;
    }
    global[0] = N;
    global[1] = T;
    if (h->Enqueue(h->q, h->kern, 2, NULL, global, NULL, 0, NULL, NULL) !=
        CL_SUCCESS) {
        return -1;
    }
    if (h->ReadBuffer(h->q, h->c_buf, CL_TRUE, 0, T * N * sizeof(float), C,
                      0, NULL, NULL) != CL_SUCCESS) {
        return -1;
    }
    return 0;
}
