#include "../../include/cce/cce_clgemm.h"
#include "../../include/cce/cce_cl_stream.h"

#include <math.h>
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
typedef cl_int (*p_clEnqueueCopyBuffer)(cl_command_queue, cl_mem, cl_mem,
                                        size_t, size_t, size_t, cl_uint,
                                        const void *, void *);

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
    "}\n"
    /* Device residual stream: rms(x)->ln, x+=y, strided KV attention. */
    "__kernel void cnet_rms_dev(__global const float* x,\n"
    "                           __global const float* w,\n"
    "                           __global float* y,\n"
    "                           const int D, const float eps,\n"
    "                           const int has_w, const int add_one) {\n"
    "    int i = get_global_id(0);\n"
    "    if (i != 0) return;\n"
    "    float ss = 0.0f;\n"
    "    for (int d = 0; d < D; ++d) ss += x[d] * x[d];\n"
    "    ss = rsqrt(ss / (float)D + eps);\n"
    "    for (int d = 0; d < D; ++d) {\n"
    "        float ww = has_w ? (add_one ? (1.0f + w[d]) : w[d]) : 1.0f;\n"
    "        y[d] = x[d] * ss * ww;\n"
    "    }\n"
    "}\n"
    "__kernel void cnet_add_dev(__global float* x,\n"
    "                           __global const float* y, const int n) {\n"
    "    int i = get_global_id(0);\n"
    "    if (i < n) x[i] += y[i];\n"
    "}\n"
    /* scores[j] = scale * q·K[jmin+j] with K row at (jmin+j)*k_slot+k_off+kh*hd */
    "__kernel void cnet_ascores_kv(__global const float* q,\n"
    "                              __global const float* Kc,\n"
    "                              __global float* scores,\n"
    "                              const int seq, const int jmin,\n"
    "                              const int hd, const int kh,\n"
    "                              const int k_slot, const int k_off,\n"
    "                              const float scale) {\n"
    "    int j = get_global_id(0);\n"
    "    if (j >= seq) return;\n"
    "    int pos = jmin + j;\n"
    "    __global const float* kj = Kc + (size_t)pos * (size_t)k_slot +\n"
    "                              (size_t)k_off + (size_t)kh * (size_t)hd;\n"
    "    float acc = 0.0f;\n"
    "    for (int d = 0; d < hd; ++d) acc += q[d] * kj[d];\n"
    "    scores[j] = acc * scale;\n"
    "}\n"
    "__kernel void cnet_av_kv(__global const float* scores,\n"
    "                         __global const float* Vc,\n"
    "                         __global float* out,\n"
    "                         const int seq, const int jmin,\n"
    "                         const int vd, const int vh,\n"
    "                         const int v_slot, const int v_off) {\n"
    "    int d = get_global_id(0);\n"
    "    if (d >= vd) return;\n"
    "    float acc = 0.0f;\n"
    "    for (int j = 0; j < seq; ++j) {\n"
    "        int pos = jmin + j;\n"
    "        __global const float* vj = Vc + (size_t)pos * (size_t)v_slot +\n"
    "                                  (size_t)v_off + (size_t)vh * (size_t)vd;\n"
    "        acc += scores[j] * vj[d];\n"
    "    }\n"
    "    out[d] = acc;\n"
    "}\n"
    /* NEOX RoPE: pair i with i+rope_dim/2 on each head row. */
    "__kernel void cnet_rope_neox(__global float* x,\n"
    "                             const int n_heads, const int head_dim,\n"
    "                             const int rope_dim, const int pos,\n"
    "                             const float base,\n"
    "                             __global const float* factors,\n"
    "                             const int has_f) {\n"
    "    int h = get_global_id(0);\n"
    "    if (h >= n_heads) return;\n"
    "    int half_rd = rope_dim / 2;\n" /* not 'half' — OpenCL type name */
    "    __global float* row = x + (size_t)h * (size_t)head_dim;\n"
    "    for (int i = 0; i < half_rd; ++i) {\n"
    "        float freq = 1.0f / pow(base, (float)(2 * i) / (float)rope_dim);\n"
    "        if (has_f) freq /= factors[i];\n"
    "        float val = (float)pos * freq;\n"
    "        float c = cos(val), s = sin(val);\n"
    "        float a0 = row[i], a1 = row[i + half_rd];\n"
    "        row[i] = a0 * c - a1 * s;\n"
    "        row[i + half_rd] = a0 * s + a1 * c;\n"
    "    }\n"
    "}\n"
    /* YaRN NEOX (ggml rope_yarn + iterative theta). stride=head_dim dense or
       2*head_dim interleaved [q|gate]; only first rope_dim of each head. */
    "__kernel void cnet_rope_yarn(__global float* x,\n"
    "                             const int n_heads, const int head_dim,\n"
    "                             const int rope_dim, const int pos,\n"
    "                             const float base,\n"
    "                             const float freq_scale,\n"
    "                             const float ext_factor,\n"
    "                             const float attn_factor,\n"
    "                             const float corr0, const float corr1,\n"
    "                             const int stride) {\n"
    "    int h = get_global_id(0);\n"
    "    if (h >= n_heads) return;\n"
    "    int half_rd = rope_dim / 2;\n"
    "    __global float* row = x + (size_t)h * (size_t)stride;\n"
    "    float theta = (float)pos;\n"
    "    float theta_scale = pow(base, -2.0f / (float)rope_dim);\n"
    "    for (int i = 0; i < half_rd; ++i) {\n"
    "        float theta_extrap = theta;\n"
    "        float theta_interp = freq_scale * theta_extrap;\n"
    "        float th = theta_interp;\n"
    "        float mscale = attn_factor;\n"
    "        if (ext_factor != 0.0f) {\n"
    "            float y = ((float)i - corr0) / fmax(0.001f, corr1 - corr0);\n"
    "            float ramp = (1.0f - fmin(1.0f, fmax(0.0f, y))) * ext_factor;\n"
    "            th = theta_interp * (1.0f - ramp) + theta_extrap * ramp;\n"
    "            mscale *= 1.0f + 0.1f * log(1.0f / freq_scale);\n"
    "        }\n"
    "        float c = cos(th) * mscale, s = sin(th) * mscale;\n"
    "        float a0 = row[i], a1 = row[i + half_rd];\n"
    "        row[i] = a0 * c - a1 * s;\n"
    "        row[i + half_rd] = a0 * s + a1 * c;\n"
    "        theta *= theta_scale;\n"
    "    }\n"
    "}\n"
    /* Per-head RMSNorm * w[d]. stride = head_dim (dense) or 2*head_dim
       (interleaved [q|gate]: only first head_dim of each head). */
    "__kernel void cnet_head_rms(__global float* x,\n"
    "                            __global const float* w,\n"
    "                            const int n_heads, const int head_dim,\n"
    "                            const int stride, const float eps) {\n"
    "    int h = get_global_id(0);\n"
    "    if (h >= n_heads) return;\n"
    "    __global float* row = x + (size_t)h * (size_t)stride;\n"
    "    float ss = 0.0f;\n"
    "    for (int d = 0; d < head_dim; ++d) ss += row[d] * row[d];\n"
    "    ss = rsqrt(ss / (float)head_dim + eps);\n"
    "    for (int d = 0; d < head_dim; ++d)\n"
    "        row[d] = row[d] * ss * w[d];\n"
    "}\n"
    /* Pack interleaved [q_h|gate_h] → dense Q[n_q * head_dim]. */
    "__kernel void cnet_pack_q_int(__global const float* qg,\n"
    "                              __global float* q,\n"
    "                              const int n_q, const int head_dim) {\n"
    "    int h = get_global_id(0);\n"
    "    if (h >= n_q) return;\n"
    "    __global const float* src = qg + (size_t)h * 2 * (size_t)head_dim;\n"
    "    __global float* dst = q + (size_t)h * (size_t)head_dim;\n"
    "    for (int d = 0; d < head_dim; ++d) dst[d] = src[d];\n"
    "}\n"
    /* AO[h,d] *= sigmoid(gate from interleaved qg). v_hd may equal head_dim. */
    "__kernel void cnet_gate_ao(__global float* ao,\n"
    "                           __global const float* qg,\n"
    "                           const int n_q, const int head_dim,\n"
    "                           const int v_hd) {\n"
    "    int h = get_global_id(0);\n"
    "    if (h >= n_q) return;\n"
    "    __global const float* g =\n"
    "        qg + (size_t)h * 2 * (size_t)head_dim + (size_t)head_dim;\n"
    "    __global float* o = ao + (size_t)h * (size_t)v_hd;\n"
    "    int lim = v_hd < head_dim ? v_hd : head_dim;\n"
    "    for (int d = 0; d < lim; ++d) {\n"
    "        float x = g[d];\n"
    "        float s = 1.0f / (1.0f + exp(-x));\n"
    "        o[d] *= s;\n"
    "    }\n"
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
    cl_kernel kern_rms_dev;
    cl_kernel kern_add_dev;
    cl_kernel kern_ascores_kv;
    cl_kernel kern_av_kv;
    cl_kernel kern_rope;
    cl_kernel kern_rope_yarn;
    cl_kernel kern_head_rms;
    cl_kernel kern_pack_q;
    cl_kernel kern_gate_ao;
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

/* Residual + device KV stream (lives on device 0). */
typedef struct {
    int live;
    int D;
    int max_ctx;
    size_t k_slot, v_slot;
    cl_mem x;       /* residual [D] */
    cl_mem ln;      /* last rms output [D] — matmul A */
    cl_mem tmp;     /* linear / attn scratch */
    cl_mem k_cache; /* [max_ctx * k_slot] */
    cl_mem v_cache;
    cl_mem scores;  /* [max_ctx] */
    cl_mem w_rms;   /* uploaded norm weights */
    cl_mem slot[5]; /* Q K V AO TMP — FFN reuses Q/K/TMP after attn */
    size_t slot_cap[5];
    cl_mem rope_fac; /* optional freq factors */
    size_t rope_fac_cap;
    cl_mem w_head; /* per-head Q/K norm weights */
    size_t w_head_cap;
    float *a_bcast; /* host scratch for multi-GPU A broadcast */
    size_t a_bcast_cap;
    float *c_gather; /* host scratch for multi-GPU C gather → slot */
    size_t c_gather_cap;
    size_t w_rms_cap;
    size_t scores_cap;
    size_t tmp_cap;
} ClStream;

struct cce_clgemm {
    cce_dl dll;
    ClgemmDev d[CLGEMM_MAX_DEV];
    size_t ndev;
    size_t rr;                    /* round-robin owner for unsplit weights */
    size_t split_bytes;           /* matrices >= this are column-split */
    size_t min_flops;             /* T*K*N below this → refuse (CPU) */
    ResidentBuf resident[CLGEMM_MAX_RESIDENT];
    size_t resident_count;
    ClStream stream;
    /* function pointers */
    p_clCreateBuffer CreateBuffer;
    p_clEnqueueWriteBuffer WriteBuffer;
    p_clEnqueueReadBuffer ReadBuffer;
    p_clEnqueueCopyBuffer CopyBuffer;
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
    SYM(h->CopyBuffer, p_clEnqueueCopyBuffer, "clEnqueueCopyBuffer");
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
        if (BuildProgram(D->prog, 1, &D->dev, "", NULL, NULL) != CL_SUCCESS) {
            /* Surface kernel compile failures (silent skip left GPUs dead). */
            size_t logn = 0;
            char *blog = NULL;
            p_clGetProgramBuildInfo GetBuildInfo = NULL;
            void *sym = cce_dl_sym(h->dll, "clGetProgramBuildInfo");
            if (sym) {
                memcpy(&GetBuildInfo, &sym, sizeof GetBuildInfo);
                GetBuildInfo(D->prog, D->dev, CL_PROGRAM_BUILD_LOG, 0, NULL,
                             &logn);
                if (logn > 1 && logn < 1u << 16) {
                    blog = (char *)malloc(logn);
                    if (blog) {
                        GetBuildInfo(D->prog, D->dev, CL_PROGRAM_BUILD_LOG,
                                     logn, blog, NULL);
                        fprintf(stderr, "cce_clgemm: OpenCL build failed:\n%s\n",
                                blog);
                        free(blog);
                    }
                }
            }
            goto dev_fail;
        }
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
        D->kern_rms_dev = CreateKernel(D->prog, "cnet_rms_dev", &err);
        D->kern_add_dev = CreateKernel(D->prog, "cnet_add_dev", &err);
        D->kern_ascores_kv = CreateKernel(D->prog, "cnet_ascores_kv", &err);
        D->kern_av_kv = CreateKernel(D->prog, "cnet_av_kv", &err);
        D->kern_rope = CreateKernel(D->prog, "cnet_rope_neox", &err);
        D->kern_rope_yarn = CreateKernel(D->prog, "cnet_rope_yarn", &err);
        D->kern_head_rms = CreateKernel(D->prog, "cnet_head_rms", &err);
        D->kern_pack_q = CreateKernel(D->prog, "cnet_pack_q_int", &err);
        D->kern_gate_ao = CreateKernel(D->prog, "cnet_gate_ao", &err);
        h->ndev++;
        continue;
    dev_fail:
        if (D->kern_gate_ao) h->ReleaseKernel(D->kern_gate_ao);
        if (D->kern_pack_q) h->ReleaseKernel(D->kern_pack_q);
        if (D->kern_head_rms) h->ReleaseKernel(D->kern_head_rms);
        if (D->kern_rope_yarn) h->ReleaseKernel(D->kern_rope_yarn);
        if (D->kern_rope) h->ReleaseKernel(D->kern_rope);
        if (D->kern_av_kv) h->ReleaseKernel(D->kern_av_kv);
        if (D->kern_ascores_kv) h->ReleaseKernel(D->kern_ascores_kv);
        if (D->kern_add_dev) h->ReleaseKernel(D->kern_add_dev);
        if (D->kern_rms_dev) h->ReleaseKernel(D->kern_rms_dev);
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

static void stream_free(cce_clgemm *h) {
    ClStream *s;
    int i;
    if (!h) return;
    s = &h->stream;
    if (s->x) h->ReleaseMem(s->x);
    if (s->ln) h->ReleaseMem(s->ln);
    if (s->tmp) h->ReleaseMem(s->tmp);
    if (s->k_cache) h->ReleaseMem(s->k_cache);
    if (s->v_cache) h->ReleaseMem(s->v_cache);
    if (s->scores) h->ReleaseMem(s->scores);
    if (s->w_rms) h->ReleaseMem(s->w_rms);
    if (s->rope_fac) h->ReleaseMem(s->rope_fac);
    if (s->w_head) h->ReleaseMem(s->w_head);
    for (i = 0; i < 5; i++)
        if (s->slot[i]) h->ReleaseMem(s->slot[i]);
    free(s->a_bcast);
    free(s->c_gather);
    memset(s, 0, sizeof *s);
}

void cce_clgemm_close(cce_clgemm *h) {
    size_t i, j;
    if (!h) return;
    stream_free(h);
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
        if (D->kern_gate_ao) h->ReleaseKernel(D->kern_gate_ao);
        if (D->kern_pack_q) h->ReleaseKernel(D->kern_pack_q);
        if (D->kern_head_rms) h->ReleaseKernel(D->kern_head_rms);
        if (D->kern_rope_yarn) h->ReleaseKernel(D->kern_rope_yarn);
        if (D->kern_rope) h->ReleaseKernel(D->kern_rope);
        if (D->kern_av_kv) h->ReleaseKernel(D->kern_av_kv);
        if (D->kern_ascores_kv) h->ReleaseKernel(D->kern_ascores_kv);
        if (D->kern_add_dev) h->ReleaseKernel(D->kern_add_dev);
        if (D->kern_rms_dev) h->ReleaseKernel(D->kern_rms_dev);
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

/* ---- Residual stream + device KV (primary device) ------------------------ */

static int stream_enabled(void) {
    const char *e = getenv("CNET_GPU_STREAM");
    if (e && e[0] == '0') return 0;
    return 1;
}

int cce_clgemm_stream_live(const cce_clgemm *h) {
    return h && h->stream.live;
}

void cce_clgemm_stream_reset(cce_clgemm *h) {
    if (h) h->stream.live = 0;
}

int cce_clgemm_stream_bind(cce_clgemm *h, int d_model, int max_ctx,
                           size_t k_slot, size_t v_slot) {
    ClgemmDev *D;
    ClStream *s;
    cl_int err = 0;
    size_t k_bytes, v_bytes;
    if (!h || h->ndev < 1 || d_model < 1 || max_ctx < 1 || k_slot < 1 ||
        v_slot < 1)
        return -1;
    if (!stream_enabled()) return -1;
    D = &h->d[0];
    if (!D->kern_rms_dev || !D->kern_add_dev || !D->kern_ascores_kv ||
        !D->kern_av_kv || !D->kern_rope)
        return -1;
    s = &h->stream;
    if (s->x && s->D == d_model && s->max_ctx == max_ctx &&
        s->k_slot == k_slot && s->v_slot == v_slot) {
        s->live = 1;
        return 0;
    }
    stream_free(h);
    s->D = d_model;
    s->max_ctx = max_ctx;
    s->k_slot = k_slot;
    s->v_slot = v_slot;
    k_bytes = (size_t)max_ctx * k_slot * sizeof(float);
    v_bytes = (size_t)max_ctx * v_slot * sizeof(float);
    s->x = h->CreateBuffer(D->ctx, CL_MEM_READ_WRITE, (size_t)d_model * 4,
                           NULL, &err);
    s->ln = h->CreateBuffer(D->ctx, CL_MEM_READ_WRITE, (size_t)d_model * 4,
                            NULL, &err);
    s->k_cache =
        h->CreateBuffer(D->ctx, CL_MEM_READ_WRITE, k_bytes, NULL, &err);
    s->v_cache =
        h->CreateBuffer(D->ctx, CL_MEM_READ_WRITE, v_bytes, NULL, &err);
    s->scores =
        h->CreateBuffer(D->ctx, CL_MEM_READ_WRITE, (size_t)max_ctx * 4, NULL,
                        &err);
    s->scores_cap = (size_t)max_ctx;
    if (!s->x || !s->ln || !s->k_cache || !s->v_cache || !s->scores) {
        stream_free(h);
        return -1;
    }
    s->live = 1;
    return 0;
}

int cce_clgemm_stream_set_x(cce_clgemm *h, const float *x, int D) {
    ClgemmDev *Dv;
    if (!h || !x || !h->stream.live || D != h->stream.D) return -1;
    Dv = &h->d[0];
    if (h->WriteBuffer(Dv->q, h->stream.x, CL_TRUE, 0, (size_t)D * 4, x, 0,
                       NULL, NULL) != CL_SUCCESS)
        return -1;
    return 0;
}

int cce_clgemm_stream_get_x(cce_clgemm *h, float *x, int D) {
    ClgemmDev *Dv;
    if (!h || !x || !h->stream.live || D != h->stream.D) return -1;
    Dv = &h->d[0];
    if (h->ReadBuffer(Dv->q, h->stream.x, CL_TRUE, 0, (size_t)D * 4, x, 0,
                      NULL, NULL) != CL_SUCCESS)
        return -1;
    return 0;
}

int cce_clgemm_stream_rms_x(cce_clgemm *h, const float *w, int D, float eps,
                            int add_one) {
    ClgemmDev *Dv;
    ClStream *s;
    int iD, has_w, add1;
    size_t g = 1;
    cl_int err = 0;
    if (!h || !h->stream.live || D != h->stream.D) return -1;
    Dv = &h->d[0];
    s = &h->stream;
    has_w = w ? 1 : 0;
    add1 = add_one ? 1 : 0;
    iD = D;
    if (has_w) {
        if (!s->w_rms || s->w_rms_cap < (size_t)D * 4) {
            if (s->w_rms) h->ReleaseMem(s->w_rms);
            s->w_rms = h->CreateBuffer(Dv->ctx, CL_MEM_READ_ONLY,
                                       (size_t)D * 4, NULL, &err);
            s->w_rms_cap = (size_t)D * 4;
            if (!s->w_rms) return -1;
        }
        if (h->WriteBuffer(Dv->q, s->w_rms, CL_FALSE, 0, (size_t)D * 4, w, 0,
                           NULL, NULL) != CL_SUCCESS)
            return -1;
    }
    if (h->SetArg(Dv->kern_rms_dev, 0, sizeof(cl_mem), &s->x) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms_dev, 1, sizeof(cl_mem),
                  has_w ? &s->w_rms : &s->x) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms_dev, 2, sizeof(cl_mem), &s->ln) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms_dev, 3, sizeof(int), &iD) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms_dev, 4, sizeof(float), &eps) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms_dev, 5, sizeof(int), &has_w) != CL_SUCCESS ||
        h->SetArg(Dv->kern_rms_dev, 6, sizeof(int), &add1) != CL_SUCCESS)
        return -1;
    if (h->Enqueue(Dv->q, Dv->kern_rms_dev, 1, NULL, &g, NULL, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Flush(Dv->q);
    /* Mark device A as stream ln for subsequent matmuls on d0 */
    if (scratch_ensure(h, 0, &Dv->a_buf, &Dv->a_cap, (size_t)D * 4) != 0)
        return -1;
    if (h->CopyBuffer(Dv->q, s->ln, Dv->a_buf, 0, 0, (size_t)D * 4, 0, NULL,
                      NULL) != CL_SUCCESS)
        return -1;
    Dv->a_fp_host = NULL;
    Dv->a_fp_bytes = (size_t)D * 4;
    Dv->a_fp_tag = 0xC0FFEE01ULL; /* device-sourced A */
    return 0;
}

int cce_clgemm_stream_add_x_host(cce_clgemm *h, const float *y, int D) {
    ClgemmDev *Dv;
    ClStream *s;
    int in;
    size_t g, loc;
    if (!h || !y || !h->stream.live || D != h->stream.D) return -1;
    Dv = &h->d[0];
    s = &h->stream;
    if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap, (size_t)D * 4) != 0) return -1;
    if (h->WriteBuffer(Dv->q, Dv->op_a, CL_FALSE, 0, (size_t)D * 4, y, 0, NULL,
                       NULL) != CL_SUCCESS)
        return -1;
    in = D;
    if (h->SetArg(Dv->kern_add_dev, 0, sizeof(cl_mem), &s->x) != CL_SUCCESS ||
        h->SetArg(Dv->kern_add_dev, 1, sizeof(cl_mem), &Dv->op_a) !=
            CL_SUCCESS ||
        h->SetArg(Dv->kern_add_dev, 2, sizeof(int), &in) != CL_SUCCESS)
        return -1;
    g = (size_t)((D + 63) / 64) * 64;
    if (g == 0) g = 64;
    loc = 64;
    if (h->Enqueue(Dv->q, Dv->kern_add_dev, 1, NULL, &g, &loc, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Finish(Dv->q);
    return 0;
}

static int stream_bcast_a(cce_clgemm *h, size_t K); /* defined with slot path */

/* Stream matmul: a_buf already holds A. Dual-GPU column-split when resident
 * is split; results gather to C_host (same layout as cce_clgemm_matmul). */
static int stream_matmul_dev0(cce_clgemm *h, size_t K, size_t N, float *C_host,
                              int is_q8, const void *Wkey, size_t Wbytes,
                              const float *bias, const float *scales,
                              const int8_t *Wq, const float *Wfp) {
    ResidentBuf *went = NULL, *bent = NULL;
    int has_bias, iT = 1, iK, started[CLGEMM_MAX_DEV] = {0};
    size_t d;
    int need_split;
    if (!h || !C_host || h->ndev < 1) return -1;
    if ((size_t)1 * K * N < h->min_flops) return -1;
    if (!h->d[0].a_buf || h->d[0].a_cap < K * sizeof(float)) return -1;

    if (is_q8) {
        went = resident_find(h, Wq, K * N);
        if (!went) {
            /* Stream-created weights: whole on d0 (see stream_linear_to_slot). */
            int split = 0;
            size_t owner = 0, off[CLGEMM_MAX_DEV] = {0},
                   len[CLGEMM_MAX_DEV] = {0};
            len[0] = N;
            went = resident_create_q8(h, Wq, scales, K, N, split, owner, off,
                                      len);
            if (!went) return -1;
        }
    } else {
        went = resident_find(h, Wfp, K * N * sizeof(float));
        if (!went) {
            int split = 0;
            size_t owner = 0, off[CLGEMM_MAX_DEV] = {0},
                   len[CLGEMM_MAX_DEV] = {0};
            len[0] = N;
            went = resident_create(h, Wfp, K, N, split, owner, off, len);
            if (!went) return -1;
        }
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
    need_split = (went->len[0] != N);
    if (need_split && stream_bcast_a(h, K) != 0) return -1;

    iK = (int)K;
    for (d = 0; d < h->ndev; ++d) {
        ClgemmDev *D = &h->d[d];
        size_t cols = went->len[d];
        int iN;
        float *dst;
        if (cols == 0) continue;
        if (scratch_ensure(h, d, &D->c_buf, &D->c_cap, cols * 4) != 0)
            return -1;
        if (d != 0 && !need_split && stream_bcast_a(h, K) != 0) return -1;
        if (!D->a_buf || D->a_cap < K * 4) return -1;
        iN = (int)cols;
        if (need_split) {
            if (D->c_host_cap < cols) {
                float *nc =
                    (float *)realloc(D->c_host, cols * sizeof(float));
                if (!nc) return -1;
                D->c_host = nc;
                D->c_host_cap = cols;
            }
            dst = D->c_host;
        } else {
            dst = C_host;
        }
        if (is_q8) {
            cl_mem b_mem = bent ? bent->mem[d] : went->scale[d];
            if (h->SetArg(D->kern_q8, 0, sizeof(cl_mem), &D->a_buf) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 1, sizeof(cl_mem), &went->mem[d]) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 2, sizeof(cl_mem), &went->scale[d]) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 3, sizeof(cl_mem), &b_mem) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 4, sizeof(cl_mem), &D->c_buf) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 5, sizeof(int), &iT) != CL_SUCCESS ||
                h->SetArg(D->kern_q8, 6, sizeof(int), &iK) != CL_SUCCESS ||
                h->SetArg(D->kern_q8, 7, sizeof(int), &iN) != CL_SUCCESS ||
                h->SetArg(D->kern_q8, 8, sizeof(int), &has_bias) != CL_SUCCESS)
                return -1;
            if (enqueue_2d(h, D->q, D->kern_q8, cols, 1) != 0) return -1;
        } else {
            cl_mem b_mem = bent ? bent->mem[d] : went->mem[d];
            if (h->SetArg(D->kern, 0, sizeof(cl_mem), &D->a_buf) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern, 1, sizeof(cl_mem), &went->mem[d]) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern, 2, sizeof(cl_mem), &b_mem) != CL_SUCCESS ||
                h->SetArg(D->kern, 3, sizeof(cl_mem), &D->c_buf) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern, 4, sizeof(int), &iT) != CL_SUCCESS ||
                h->SetArg(D->kern, 5, sizeof(int), &iK) != CL_SUCCESS ||
                h->SetArg(D->kern, 6, sizeof(int), &iN) != CL_SUCCESS ||
                h->SetArg(D->kern, 7, sizeof(int), &has_bias) != CL_SUCCESS)
                return -1;
            if (enqueue_2d(h, D->q, D->kern, cols, 1) != 0) return -1;
        }
        if (h->ReadBuffer(D->q, D->c_buf, CL_FALSE, 0, cols * 4, dst, 0, NULL,
                          NULL) != CL_SUCCESS)
            return -1;
        started[d] = 1;
        h->Flush(D->q);
    }
    for (d = 0; d < h->ndev; ++d)
        if (started[d] && h->Finish(h->d[d].q) != CL_SUCCESS) return -1;
    if (need_split) {
        for (d = 0; d < h->ndev; ++d) {
            size_t cols = went->len[d];
            if (cols == 0) continue;
            memcpy(C_host + went->off[d], h->d[d].c_host, cols * sizeof(float));
        }
    }
    (void)Wkey;
    (void)Wbytes;
    return 0;
}

int cce_clgemm_stream_linear_fp(cce_clgemm *h, const float *W,
                                const float *bias, int K, int N,
                                float *C_host) {
    /* K may be D (from residual ln) or o_in (from AO-as-A). */
    if (!h || !h->stream.live || !W || !C_host || K < 1) return -1;
    return stream_matmul_dev0(h, (size_t)K, (size_t)N, C_host, 0, W,
                              (size_t)K * (size_t)N * 4, bias, NULL, NULL, W);
}

int cce_clgemm_stream_linear_q8(cce_clgemm *h, const int8_t *Wq,
                                const float *scales, const float *bias, int K,
                                int N, float *C_host) {
    if (!h || !h->stream.live || !Wq || !scales || !C_host || K < 1) return -1;
    return stream_matmul_dev0(h, (size_t)K, (size_t)N, C_host, 1, Wq,
                              (size_t)K * (size_t)N, bias, scales, Wq, NULL);
}

int cce_clgemm_stream_kv_write(cce_clgemm *h, int pos, const float *k,
                               const float *v, int k_dim, int v_dim,
                               size_t k_off, size_t v_off) {
    ClgemmDev *Dv;
    ClStream *s;
    size_t ko, vo;
    if (!h || !h->stream.live || !k || !v || pos < 0 || pos >= h->stream.max_ctx)
        return -1;
    Dv = &h->d[0];
    s = &h->stream;
    ko = (size_t)pos * s->k_slot + k_off;
    vo = (size_t)pos * s->v_slot + v_off;
    if (ko + (size_t)k_dim > (size_t)s->max_ctx * s->k_slot ||
        vo + (size_t)v_dim > (size_t)s->max_ctx * s->v_slot)
        return -1;
    if (h->WriteBuffer(Dv->q, s->k_cache, CL_FALSE, ko * 4,
                       (size_t)k_dim * 4, k, 0, NULL, NULL) != CL_SUCCESS ||
        h->WriteBuffer(Dv->q, s->v_cache, CL_FALSE, vo * 4, (size_t)v_dim * 4,
                       v, 0, NULL, NULL) != CL_SUCCESS)
        return -1;
    h->Flush(Dv->q);
    return 0;
}

int cce_clgemm_stream_attn(cce_clgemm *h, const float *q, size_t k_off,
                           size_t v_off, int n_q, int n_k, int n_v,
                           int head_dim, int v_head_dim, int jmin, int abs_t,
                           float scale, float *attn_out) {
    ClgemmDev *Dv;
    ClStream *s;
    int seq, hi, iseq, ijmin, ihd, ivd, ikh, ivh, iks, ivs, iko, ivo;
    size_t g, loc;
    if (!h || !h->stream.live || !q || !attn_out || n_q < 1) return -1;
    if (abs_t < jmin || abs_t >= h->stream.max_ctx) return -1;
    Dv = &h->d[0];
    s = &h->stream;
    seq = abs_t - jmin + 1;
    if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap, (size_t)head_dim * 4) != 0 ||
        op_scratch(h, &Dv->op_c, &Dv->op_c_cap,
                   (size_t)seq * 4 > (size_t)v_head_dim * 4
                       ? (size_t)seq * 4
                       : (size_t)v_head_dim * 4) != 0)
        return -1;
    iseq = seq;
    ijmin = jmin;
    ihd = head_dim;
    ivd = v_head_dim;
    iks = (int)s->k_slot;
    ivs = (int)s->v_slot;
    iko = (int)k_off;
    ivo = (int)v_off;

    for (hi = 0; hi < n_q; ++hi) {
        ikh = hi / (n_q / n_k);
        ivh = hi / (n_q / n_v);
        if (h->WriteBuffer(Dv->q, Dv->op_a, CL_FALSE, 0, (size_t)head_dim * 4,
                           q + (size_t)hi * (size_t)head_dim, 0, NULL,
                           NULL) != CL_SUCCESS)
            return -1;
        if (h->SetArg(Dv->kern_ascores_kv, 0, sizeof(cl_mem), &Dv->op_a) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 1, sizeof(cl_mem), &s->k_cache) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 2, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 3, sizeof(int), &iseq) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 4, sizeof(int), &ijmin) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 5, sizeof(int), &ihd) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 6, sizeof(int), &ikh) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 7, sizeof(int), &iks) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 8, sizeof(int), &iko) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 9, sizeof(float), &scale) !=
                CL_SUCCESS)
            return -1;
        g = (size_t)((seq + 63) / 64) * 64;
        if (g == 0) g = 64;
        loc = 64;
        if (h->Enqueue(Dv->q, Dv->kern_ascores_kv, 1, NULL, &g, &loc, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        if (h->SetArg(Dv->kern_asoft, 0, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_asoft, 1, sizeof(int), &iseq) != CL_SUCCESS)
            return -1;
        g = 1;
        if (h->Enqueue(Dv->q, Dv->kern_asoft, 1, NULL, &g, NULL, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        /* out head into tmp then read */
        if (!s->tmp || s->tmp_cap < (size_t)v_head_dim * 4) {
            cl_int err = 0;
            if (s->tmp) h->ReleaseMem(s->tmp);
            s->tmp = h->CreateBuffer(Dv->ctx, CL_MEM_READ_WRITE,
                                     (size_t)v_head_dim * 4, NULL, &err);
            s->tmp_cap = (size_t)v_head_dim * 4;
            if (!s->tmp) return -1;
        }
        if (h->SetArg(Dv->kern_av_kv, 0, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 1, sizeof(cl_mem), &s->v_cache) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 2, sizeof(cl_mem), &s->tmp) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 3, sizeof(int), &iseq) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 4, sizeof(int), &ijmin) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 5, sizeof(int), &ivd) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 6, sizeof(int), &ivh) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 7, sizeof(int), &ivs) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 8, sizeof(int), &ivo) != CL_SUCCESS)
            return -1;
        g = (size_t)((v_head_dim + 63) / 64) * 64;
        if (g == 0) g = 64;
        loc = 64;
        if (h->Enqueue(Dv->q, Dv->kern_av_kv, 1, NULL, &g, &loc, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        if (h->ReadBuffer(Dv->q, s->tmp, CL_TRUE, 0, (size_t)v_head_dim * 4,
                          attn_out + (size_t)hi * (size_t)v_head_dim, 0, NULL,
                          NULL) != CL_SUCCESS)
            return -1;
    }
    return 0;
}

int cce_clgemm_stream_silu_mul_host(cce_clgemm *h, const float *gate,
                                    const float *up, float *y, int n) {
    return cce_clgemm_silu_mul(h, gate, up, y, n);
}

/* ---- Slot linears, RoPE, device attn ------------------------------------- */

static int stream_slot_ensure(cce_clgemm *h, int slot, size_t bytes) {
    ClgemmDev *D;
    ClStream *s;
    cl_int err = 0;
    if (!h || slot < 0 || slot > 4) return -1;
    D = &h->d[0];
    s = &h->stream;
    if (s->slot[slot] && s->slot_cap[slot] >= bytes) return 0;
    if (s->slot[slot]) h->ReleaseMem(s->slot[slot]);
    s->slot[slot] =
        h->CreateBuffer(D->ctx, CL_MEM_READ_WRITE, bytes, NULL, &err);
    s->slot_cap[slot] = bytes;
    return s->slot[slot] ? 0 : -1;
}

/* Broadcast d0 a_buf[K] to every device (host hop — K is tiny vs GEMM). */
static int stream_bcast_a(cce_clgemm *h, size_t K) {
    ClStream *s;
    ClgemmDev *D0;
    size_t d;
    if (!h || K < 1) return -1;
    s = &h->stream;
    D0 = &h->d[0];
    if (!D0->a_buf || D0->a_cap < K * 4) return -1;
    if (h->ndev <= 1) return 0;
    if (!s->a_bcast || s->a_bcast_cap < K) {
        float *p = (float *)realloc(s->a_bcast, K * sizeof(float));
        if (!p) return -1;
        s->a_bcast = p;
        s->a_bcast_cap = K;
    }
    if (h->ReadBuffer(D0->q, D0->a_buf, CL_TRUE, 0, K * 4, s->a_bcast, 0, NULL,
                      NULL) != CL_SUCCESS)
        return -1;
    for (d = 1; d < h->ndev; ++d) {
        ClgemmDev *D = &h->d[d];
        if (scratch_ensure(h, d, &D->a_buf, &D->a_cap, K * 4) != 0) return -1;
        if (h->WriteBuffer(D->q, D->a_buf, CL_FALSE, 0, K * 4, s->a_bcast, 0,
                           NULL, NULL) != CL_SUCCESS)
            return -1;
        D->a_fp_host = NULL;
        D->a_fp_bytes = K * 4;
        D->a_fp_tag = 0xC0FFEE03ULL;
        h->Flush(D->q);
    }
    return 0;
}

/* Multi-GPU stream linear → d0 slot. Column-split weights run concurrently;
 * C is gathered through host (16–100 KB) then written to the slot. */
static int stream_linear_to_slot(cce_clgemm *h, size_t K, size_t N, int slot,
                                 int is_q8, const float *bias,
                                 const float *scales, const int8_t *Wq,
                                 const float *Wfp) {
    ClgemmDev *D0;
    ClStream *s;
    ResidentBuf *went = NULL, *bent = NULL;
    int has_bias, iT = 1, iK, started[CLGEMM_MAX_DEV] = {0};
    size_t d;
    int need_gather = 0;
    if (!h || !h->stream.live || h->ndev < 1) return -1;
    if (stream_slot_ensure(h, slot, N * sizeof(float)) != 0) return -1;
    D0 = &h->d[0];
    s = &h->stream;
    if ((size_t)1 * K * N < h->min_flops) return -1;
    if (!D0->a_buf || D0->a_cap < K * sizeof(float)) return -1;

    if (is_q8) {
        went = resident_find(h, Wq, K * N);
        if (!went) {
            /* New stream residents stay whole on d0 (A already there). */
            int split = 0;
            size_t owner = 0, off[CLGEMM_MAX_DEV] = {0},
                   len[CLGEMM_MAX_DEV] = {0};
            len[0] = N;
            went = resident_create_q8(h, Wq, scales, K, N, split, owner, off,
                                      len);
            if (!went) return -1;
        }
    } else {
        went = resident_find(h, Wfp, K * N * sizeof(float));
        if (!went) {
            int split = 0;
            size_t owner = 0, off[CLGEMM_MAX_DEV] = {0},
                   len[CLGEMM_MAX_DEV] = {0};
            len[0] = N;
            went = resident_create(h, Wfp, K, N, split, owner, off, len);
            if (!went) return -1;
        }
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
    /* Multi-GPU only when weights were already column-split (e.g. prior
       host matmul). Fresh stream uploads stay on d0 — dual-card gather on
       every T=1 step is slower than single-device. */
    need_gather = (went->len[0] != N);
    if (need_gather && stream_bcast_a(h, K) != 0) return -1;
    if (need_gather) {
        if (!s->c_gather || s->c_gather_cap < N) {
            float *p = (float *)realloc(s->c_gather, N * sizeof(float));
            if (!p) return -1;
            s->c_gather = p;
            s->c_gather_cap = N;
        }
    }

    iK = (int)K;
    for (d = 0; d < h->ndev; ++d) {
        ClgemmDev *D = &h->d[d];
        size_t cols = went->len[d];
        int iN;
        cl_mem out_mem;
        if (cols == 0) continue;
        if (scratch_ensure(h, d, &D->c_buf, &D->c_cap, cols * 4) != 0)
            return -1;
        if (d != 0 && !need_gather) {
            /* unsplit on non-d0: need A there */
            if (stream_bcast_a(h, K) != 0) return -1;
        }
        if (!D->a_buf || D->a_cap < K * 4) return -1;
        iN = (int)cols;
        /* Fast path: whole N on d0 → write slot directly (no host). */
        out_mem = (!need_gather && d == 0 && cols == N) ? s->slot[slot]
                                                        : D->c_buf;
        if (is_q8) {
            cl_mem b_mem = bent ? bent->mem[d] : went->scale[d];
            if (h->SetArg(D->kern_q8, 0, sizeof(cl_mem), &D->a_buf) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 1, sizeof(cl_mem), &went->mem[d]) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 2, sizeof(cl_mem), &went->scale[d]) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 3, sizeof(cl_mem), &b_mem) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 4, sizeof(cl_mem), &out_mem) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern_q8, 5, sizeof(int), &iT) != CL_SUCCESS ||
                h->SetArg(D->kern_q8, 6, sizeof(int), &iK) != CL_SUCCESS ||
                h->SetArg(D->kern_q8, 7, sizeof(int), &iN) != CL_SUCCESS ||
                h->SetArg(D->kern_q8, 8, sizeof(int), &has_bias) != CL_SUCCESS)
                return -1;
            if (enqueue_2d(h, D->q, D->kern_q8, cols, 1) != 0) return -1;
        } else {
            cl_mem b_mem = bent ? bent->mem[d] : went->mem[d];
            if (h->SetArg(D->kern, 0, sizeof(cl_mem), &D->a_buf) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern, 1, sizeof(cl_mem), &went->mem[d]) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern, 2, sizeof(cl_mem), &b_mem) != CL_SUCCESS ||
                h->SetArg(D->kern, 3, sizeof(cl_mem), &out_mem) !=
                    CL_SUCCESS ||
                h->SetArg(D->kern, 4, sizeof(int), &iT) != CL_SUCCESS ||
                h->SetArg(D->kern, 5, sizeof(int), &iK) != CL_SUCCESS ||
                h->SetArg(D->kern, 6, sizeof(int), &iN) != CL_SUCCESS ||
                h->SetArg(D->kern, 7, sizeof(int), &has_bias) != CL_SUCCESS)
                return -1;
            if (enqueue_2d(h, D->q, D->kern, cols, 1) != 0) return -1;
        }
        started[d] = 1;
        if (need_gather) {
            if (h->ReadBuffer(D->q, D->c_buf, CL_FALSE, 0, cols * 4,
                              s->c_gather + went->off[d], 0, NULL,
                              NULL) != CL_SUCCESS)
                return -1;
        }
        h->Flush(D->q);
    }
    for (d = 0; d < h->ndev; ++d)
        if (started[d] && h->Finish(h->d[d].q) != CL_SUCCESS) return -1;
    if (need_gather) {
        if (h->WriteBuffer(D0->q, s->slot[slot], CL_TRUE, 0, N * 4, s->c_gather,
                           0, NULL, NULL) != CL_SUCCESS)
            return -1;
    } else {
        h->Flush(D0->q);
    }
    return 0;
}

int cce_clgemm_stream_linear_fp_slot(cce_clgemm *h, const float *W,
                                     const float *bias, int K, int N,
                                     int slot) {
    if (!W || K < 1) return -1;
    return stream_linear_to_slot(h, (size_t)K, (size_t)N, slot, 0, bias, NULL,
                                 NULL, W);
}

int cce_clgemm_stream_linear_q8_slot(cce_clgemm *h, const int8_t *Wq,
                                     const float *scales, const float *bias,
                                     int K, int N, int slot) {
    if (!Wq || !scales || K < 1) return -1;
    return stream_linear_to_slot(h, (size_t)K, (size_t)N, slot, 1, bias, scales,
                                 Wq, NULL);
}

int cce_clgemm_stream_rope_slot(cce_clgemm *h, int slot, int n_heads,
                                int head_dim, int rope_dim, int pos, float base,
                                const float *freq_factors) {
    ClgemmDev *D;
    ClStream *s;
    int nh, hd, rd, p, has_f;
    size_t g;
    if (!h || !h->stream.live || slot < 0 || slot > 4) return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!D->kern_rope || !s->slot[slot]) return -1;
    if (rope_dim < 2 || rope_dim > head_dim || n_heads < 1) return -1;
    has_f = 0;
    if (freq_factors) {
        size_t half = (size_t)(rope_dim / 2);
        cl_int err = 0;
        if (!s->rope_fac || s->rope_fac_cap < half * 4) {
            if (s->rope_fac) h->ReleaseMem(s->rope_fac);
            s->rope_fac = h->CreateBuffer(D->ctx, CL_MEM_READ_ONLY, half * 4,
                                          NULL, &err);
            s->rope_fac_cap = half * 4;
            if (!s->rope_fac) return -1;
        }
        if (h->WriteBuffer(D->q, s->rope_fac, CL_FALSE, 0, half * 4,
                           freq_factors, 0, NULL, NULL) != CL_SUCCESS)
            return -1;
        has_f = 1;
    }
    nh = n_heads;
    hd = head_dim;
    rd = rope_dim;
    p = pos;
    if (h->SetArg(D->kern_rope, 0, sizeof(cl_mem), &s->slot[slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_rope, 1, sizeof(int), &nh) != CL_SUCCESS ||
        h->SetArg(D->kern_rope, 2, sizeof(int), &hd) != CL_SUCCESS ||
        h->SetArg(D->kern_rope, 3, sizeof(int), &rd) != CL_SUCCESS ||
        h->SetArg(D->kern_rope, 4, sizeof(int), &p) != CL_SUCCESS ||
        h->SetArg(D->kern_rope, 5, sizeof(float), &base) != CL_SUCCESS ||
        h->SetArg(D->kern_rope, 6, sizeof(cl_mem),
                  has_f ? &s->rope_fac : &s->slot[slot]) != CL_SUCCESS ||
        h->SetArg(D->kern_rope, 7, sizeof(int), &has_f) != CL_SUCCESS)
        return -1;
    g = (size_t)n_heads;
    if (h->Enqueue(D->q, D->kern_rope, 1, NULL, &g, NULL, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Flush(D->q);
    return 0;
}

int cce_clgemm_stream_kv_write_slots(cce_clgemm *h, int pos, int k_dim,
                                     int v_dim, size_t k_off, size_t v_off) {
    ClgemmDev *D;
    ClStream *s;
    size_t ko, vo;
    if (!h || !h->stream.live || pos < 0 || pos >= h->stream.max_ctx) return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!s->slot[CCE_CL_SLOT_K] || !s->slot[CCE_CL_SLOT_V]) return -1;
    ko = (size_t)pos * s->k_slot + k_off;
    vo = (size_t)pos * s->v_slot + v_off;
    if (h->CopyBuffer(D->q, s->slot[CCE_CL_SLOT_K], s->k_cache, 0, ko * 4,
                      (size_t)k_dim * 4, 0, NULL, NULL) != CL_SUCCESS ||
        h->CopyBuffer(D->q, s->slot[CCE_CL_SLOT_V], s->v_cache, 0, vo * 4,
                      (size_t)v_dim * 4, 0, NULL, NULL) != CL_SUCCESS)
        return -1;
    h->Flush(D->q);
    return 0;
}

int cce_clgemm_stream_use_slot_as_a(cce_clgemm *h, int slot, int n) {
    ClgemmDev *D;
    ClStream *s;
    if (!h || !h->stream.live || slot < 0 || slot > 4 || n < 1) return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!s->slot[slot]) return -1;
    if (scratch_ensure(h, 0, &D->a_buf, &D->a_cap, (size_t)n * 4) != 0)
        return -1;
    if (h->CopyBuffer(D->q, s->slot[slot], D->a_buf, 0, 0, (size_t)n * 4, 0,
                      NULL, NULL) != CL_SUCCESS)
        return -1;
    D->a_fp_host = NULL;
    D->a_fp_bytes = (size_t)n * 4;
    D->a_fp_tag = 0xC0FFEE02ULL;
    return 0;
}

int cce_clgemm_stream_get_slot(cce_clgemm *h, int slot, float *host, int n) {
    ClgemmDev *D;
    if (!h || !h->stream.live || !host || slot < 0 || slot > 4 || n < 1)
        return -1;
    D = &h->d[0];
    if (!h->stream.slot[slot]) return -1;
    if (h->ReadBuffer(D->q, h->stream.slot[slot], CL_TRUE, 0, (size_t)n * 4,
                      host, 0, NULL, NULL) != CL_SUCCESS)
        return -1;
    return 0;
}

int cce_clgemm_stream_attn_dev(cce_clgemm *h, size_t k_off, size_t v_off,
                               int n_q, int n_k, int n_v, int head_dim,
                               int v_head_dim, int jmin, int abs_t, float scale,
                               float *attn_out_host) {
    ClgemmDev *Dv;
    ClStream *s;
    int seq, hi, iseq, ijmin, ihd, ivd, ikh, ivh, iks, ivs, iko, ivo;
    size_t g, loc;
    if (!h || !h->stream.live || n_q < 1) return -1;
    if (abs_t < jmin || abs_t >= h->stream.max_ctx) return -1;
    if (!h->stream.slot[CCE_CL_SLOT_Q]) return -1;
    Dv = &h->d[0];
    s = &h->stream;
    seq = abs_t - jmin + 1;
    if (stream_slot_ensure(h, CCE_CL_SLOT_AO,
                           (size_t)n_q * (size_t)v_head_dim * 4) != 0)
        return -1;
    if (op_scratch(h, &Dv->op_c, &Dv->op_c_cap, (size_t)seq * 4) != 0)
        return -1;
    iseq = seq;
    ijmin = jmin;
    ihd = head_dim;
    ivd = v_head_dim;
    iks = (int)s->k_slot;
    ivs = (int)s->v_slot;
    iko = (int)k_off;
    ivo = (int)v_off;

    for (hi = 0; hi < n_q; ++hi) {
        size_t q_off = (size_t)hi * (size_t)head_dim * 4;
        size_t ao_off = (size_t)hi * (size_t)v_head_dim * 4;
        cl_mem q_view = s->slot[CCE_CL_SLOT_Q]; /* full buffer; offset via copy */
        /* copy head q into op_a */
        if (op_scratch(h, &Dv->op_a, &Dv->op_a_cap, (size_t)head_dim * 4) != 0)
            return -1;
        if (h->CopyBuffer(Dv->q, q_view, Dv->op_a, q_off, 0,
                          (size_t)head_dim * 4, 0, NULL, NULL) != CL_SUCCESS)
            return -1;
        ikh = hi / (n_q / n_k);
        ivh = hi / (n_q / n_v);
        if (h->SetArg(Dv->kern_ascores_kv, 0, sizeof(cl_mem), &Dv->op_a) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 1, sizeof(cl_mem), &s->k_cache) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 2, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 3, sizeof(int), &iseq) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 4, sizeof(int), &ijmin) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 5, sizeof(int), &ihd) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 6, sizeof(int), &ikh) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 7, sizeof(int), &iks) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 8, sizeof(int), &iko) != CL_SUCCESS ||
            h->SetArg(Dv->kern_ascores_kv, 9, sizeof(float), &scale) !=
                CL_SUCCESS)
            return -1;
        g = (size_t)((seq + 63) / 64) * 64;
        if (g == 0) g = 64;
        loc = 64;
        if (h->Enqueue(Dv->q, Dv->kern_ascores_kv, 1, NULL, &g, &loc, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        if (h->SetArg(Dv->kern_asoft, 0, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_asoft, 1, sizeof(int), &iseq) != CL_SUCCESS)
            return -1;
        g = 1;
        if (h->Enqueue(Dv->q, Dv->kern_asoft, 1, NULL, &g, NULL, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        if (!s->tmp || s->tmp_cap < (size_t)v_head_dim * 4) {
            cl_int err = 0;
            if (s->tmp) h->ReleaseMem(s->tmp);
            s->tmp = h->CreateBuffer(Dv->ctx, CL_MEM_READ_WRITE,
                                     (size_t)v_head_dim * 4, NULL, &err);
            s->tmp_cap = (size_t)v_head_dim * 4;
            if (!s->tmp) return -1;
        }
        if (h->SetArg(Dv->kern_av_kv, 0, sizeof(cl_mem), &Dv->op_c) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 1, sizeof(cl_mem), &s->v_cache) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 2, sizeof(cl_mem), &s->tmp) !=
                CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 3, sizeof(int), &iseq) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 4, sizeof(int), &ijmin) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 5, sizeof(int), &ivd) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 6, sizeof(int), &ivh) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 7, sizeof(int), &ivs) != CL_SUCCESS ||
            h->SetArg(Dv->kern_av_kv, 8, sizeof(int), &ivo) != CL_SUCCESS)
            return -1;
        g = (size_t)((v_head_dim + 63) / 64) * 64;
        if (g == 0) g = 64;
        loc = 64;
        if (h->Enqueue(Dv->q, Dv->kern_av_kv, 1, NULL, &g, &loc, 0, NULL,
                       NULL) != CL_SUCCESS)
            return -1;
        if (h->CopyBuffer(Dv->q, s->tmp, s->slot[CCE_CL_SLOT_AO], 0, ao_off,
                          (size_t)v_head_dim * 4, 0, NULL, NULL) != CL_SUCCESS)
            return -1;
    }
    if (attn_out_host) {
        if (h->ReadBuffer(Dv->q, s->slot[CCE_CL_SLOT_AO], CL_TRUE, 0,
                          (size_t)n_q * (size_t)v_head_dim * 4, attn_out_host,
                          0, NULL, NULL) != CL_SUCCESS)
            return -1;
    } else {
        h->Finish(Dv->q);
    }
    return 0;
}

/* ---- YaRN / QK-norm / FFN device path ------------------------------------ */

int cce_clgemm_stream_head_rms_slot(cce_clgemm *h, int slot, int n_heads,
                                    int head_dim, int stride, const float *w,
                                    float eps) {
    ClgemmDev *D;
    ClStream *s;
    int nh, hd, st;
    size_t g;
    if (!h || !h->stream.live || !w || slot < 0 || slot > 4) return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!D->kern_head_rms || !s->slot[slot]) return -1;
    if (n_heads < 1 || head_dim < 1 || stride < head_dim) return -1;
    {
        cl_int err = 0;
        size_t bytes = (size_t)head_dim * 4;
        if (!s->w_head || s->w_head_cap < bytes) {
            if (s->w_head) h->ReleaseMem(s->w_head);
            s->w_head =
                h->CreateBuffer(D->ctx, CL_MEM_READ_ONLY, bytes, NULL, &err);
            s->w_head_cap = bytes;
            if (!s->w_head) return -1;
        }
        if (h->WriteBuffer(D->q, s->w_head, CL_FALSE, 0, bytes, w, 0, NULL,
                           NULL) != CL_SUCCESS)
            return -1;
    }
    nh = n_heads;
    hd = head_dim;
    st = stride;
    if (h->SetArg(D->kern_head_rms, 0, sizeof(cl_mem), &s->slot[slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_head_rms, 1, sizeof(cl_mem), &s->w_head) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_head_rms, 2, sizeof(int), &nh) != CL_SUCCESS ||
        h->SetArg(D->kern_head_rms, 3, sizeof(int), &hd) != CL_SUCCESS ||
        h->SetArg(D->kern_head_rms, 4, sizeof(int), &st) != CL_SUCCESS ||
        h->SetArg(D->kern_head_rms, 5, sizeof(float), &eps) != CL_SUCCESS)
        return -1;
    g = (size_t)n_heads;
    if (h->Enqueue(D->q, D->kern_head_rms, 1, NULL, &g, NULL, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Flush(D->q);
    return 0;
}

int cce_clgemm_stream_rope_yarn_slot(cce_clgemm *h, int slot, int n_heads,
                                     int head_dim, int rope_dim, int pos,
                                     float base, float freq_scale,
                                     float ext_factor, float attn_factor,
                                     float corr0, float corr1, int stride) {
    ClgemmDev *D;
    ClStream *s;
    int nh, hd, rd, p, st;
    size_t g;
    if (!h || !h->stream.live || slot < 0 || slot > 4) return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!D->kern_rope_yarn || !s->slot[slot]) return -1;
    if (rope_dim < 2 || rope_dim > head_dim || n_heads < 1 || stride < head_dim)
        return -1;
    nh = n_heads;
    hd = head_dim;
    rd = rope_dim;
    p = pos;
    st = stride;
    if (h->SetArg(D->kern_rope_yarn, 0, sizeof(cl_mem), &s->slot[slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 1, sizeof(int), &nh) != CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 2, sizeof(int), &hd) != CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 3, sizeof(int), &rd) != CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 4, sizeof(int), &p) != CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 5, sizeof(float), &base) != CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 6, sizeof(float), &freq_scale) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 7, sizeof(float), &ext_factor) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 8, sizeof(float), &attn_factor) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 9, sizeof(float), &corr0) != CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 10, sizeof(float), &corr1) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_rope_yarn, 11, sizeof(int), &st) != CL_SUCCESS)
        return -1;
    g = (size_t)n_heads;
    if (h->Enqueue(D->q, D->kern_rope_yarn, 1, NULL, &g, NULL, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Flush(D->q);
    return 0;
}

int cce_clgemm_stream_pack_q_interleaved(cce_clgemm *h, int qg_slot, int q_slot,
                                         int n_q, int head_dim) {
    ClgemmDev *D;
    ClStream *s;
    int nq, hd;
    size_t g;
    if (!h || !h->stream.live || qg_slot < 0 || qg_slot > 4 || q_slot < 0 ||
        q_slot > 4 || n_q < 1 || head_dim < 1)
        return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!D->kern_pack_q || !s->slot[qg_slot]) return -1;
    if (stream_slot_ensure(h, q_slot, (size_t)n_q * (size_t)head_dim * 4) != 0)
        return -1;
    nq = n_q;
    hd = head_dim;
    if (h->SetArg(D->kern_pack_q, 0, sizeof(cl_mem), &s->slot[qg_slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_pack_q, 1, sizeof(cl_mem), &s->slot[q_slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_pack_q, 2, sizeof(int), &nq) != CL_SUCCESS ||
        h->SetArg(D->kern_pack_q, 3, sizeof(int), &hd) != CL_SUCCESS)
        return -1;
    g = (size_t)n_q;
    if (h->Enqueue(D->q, D->kern_pack_q, 1, NULL, &g, NULL, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Flush(D->q);
    return 0;
}

int cce_clgemm_stream_gate_ao(cce_clgemm *h, int qg_slot, int n_q, int head_dim,
                              int v_head_dim) {
    ClgemmDev *D;
    ClStream *s;
    int nq, hd, vd;
    size_t g;
    if (!h || !h->stream.live || qg_slot < 0 || qg_slot > 4) return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!D->kern_gate_ao || !s->slot[qg_slot] || !s->slot[CCE_CL_SLOT_AO])
        return -1;
    nq = n_q;
    hd = head_dim;
    vd = v_head_dim;
    if (h->SetArg(D->kern_gate_ao, 0, sizeof(cl_mem), &s->slot[CCE_CL_SLOT_AO]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_gate_ao, 1, sizeof(cl_mem), &s->slot[qg_slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_gate_ao, 2, sizeof(int), &nq) != CL_SUCCESS ||
        h->SetArg(D->kern_gate_ao, 3, sizeof(int), &hd) != CL_SUCCESS ||
        h->SetArg(D->kern_gate_ao, 4, sizeof(int), &vd) != CL_SUCCESS)
        return -1;
    g = (size_t)n_q;
    if (h->Enqueue(D->q, D->kern_gate_ao, 1, NULL, &g, NULL, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Flush(D->q);
    return 0;
}

int cce_clgemm_stream_silu_mul_slots(cce_clgemm *h, int gate_slot, int up_slot,
                                     int out_slot, int n) {
    ClgemmDev *D;
    ClStream *s;
    int in;
    size_t g, loc;
    if (!h || !h->stream.live || n < 1) return -1;
    if (gate_slot < 0 || gate_slot > 4 || up_slot < 0 || up_slot > 4 ||
        out_slot < 0 || out_slot > 4)
        return -1;
    D = &h->d[0];
    s = &h->stream;
    if (!D->kern_silu || !s->slot[gate_slot] || !s->slot[up_slot]) return -1;
    if (stream_slot_ensure(h, out_slot, (size_t)n * 4) != 0) return -1;
    in = n;
    if (h->SetArg(D->kern_silu, 0, sizeof(cl_mem), &s->slot[gate_slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_silu, 1, sizeof(cl_mem), &s->slot[up_slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_silu, 2, sizeof(cl_mem), &s->slot[out_slot]) !=
            CL_SUCCESS ||
        h->SetArg(D->kern_silu, 3, sizeof(int), &in) != CL_SUCCESS)
        return -1;
    g = (size_t)((n + 63) / 64) * 64;
    if (g == 0) g = 64;
    loc = 64;
    if (h->Enqueue(D->q, D->kern_silu, 1, NULL, &g, &loc, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Flush(D->q);
    return 0;
}

int cce_clgemm_stream_add_x_slot(cce_clgemm *h, int slot, int D) {
    ClgemmDev *Dv;
    ClStream *s;
    int n;
    size_t g, loc;
    if (!h || !h->stream.live || slot < 0 || slot > 4 || D < 1) return -1;
    Dv = &h->d[0];
    s = &h->stream;
    if (!Dv->kern_add_dev || !s->x || !s->slot[slot]) return -1;
    n = D;
    if (h->SetArg(Dv->kern_add_dev, 0, sizeof(cl_mem), &s->x) != CL_SUCCESS ||
        h->SetArg(Dv->kern_add_dev, 1, sizeof(cl_mem), &s->slot[slot]) !=
            CL_SUCCESS ||
        h->SetArg(Dv->kern_add_dev, 2, sizeof(int), &n) != CL_SUCCESS)
        return -1;
    g = (size_t)((D + 63) / 64) * 64;
    if (g == 0) g = 64;
    loc = 64;
    if (h->Enqueue(Dv->q, Dv->kern_add_dev, 1, NULL, &g, &loc, 0, NULL, NULL) !=
        CL_SUCCESS)
        return -1;
    h->Finish(Dv->q);
    return 0;
}

/* Concurrent dual-GPU: two independent linears from the same A (d0 a_buf).
 * d0 owns out0_slot; d1 computes out1 and copies via host into out1_slot on d0.
 * Soft-fail → caller runs sequential stream_linear_to_slot. */
int cce_clgemm_stream_linear_pair_slots(
    cce_clgemm *h, const float *W0, const float *bias0, int is_q8_0,
    const int8_t *Wq0, const float *sc0, int N0, int slot0, const float *W1,
    const float *bias1, int is_q8_1, const int8_t *Wq1, const float *sc1,
    int N1, int slot1, int K) {
    ClgemmDev *D0, *D1;
    ResidentBuf *w0 = NULL, *w1 = NULL, *b0 = NULL, *b1 = NULL;
    int has0, has1, iT = 1, iK, iN0, iN1;
    size_t d;
    if (!h || !h->stream.live || K < 1 || N0 < 1 || N1 < 1) return -1;
    if (h->ndev < 2) return -1;
    if ((size_t)1 * K * N0 < h->min_flops ||
        (size_t)1 * K * N1 < h->min_flops)
        return -1;
    if (stream_slot_ensure(h, slot0, (size_t)N0 * 4) != 0 ||
        stream_slot_ensure(h, slot1, (size_t)N1 * 4) != 0)
        return -1;
    D0 = &h->d[0];
    D1 = &h->d[1];
    if (!D0->a_buf || D0->a_cap < (size_t)K * 4) return -1;

    /* Place W0 on d0, W1 on d1 (whole, not split). */
    if (is_q8_0) {
        w0 = resident_find(h, Wq0, (size_t)K * N0);
        if (!w0) {
            size_t off[CLGEMM_MAX_DEV] = {0}, len[CLGEMM_MAX_DEV] = {0};
            len[0] = (size_t)N0;
            w0 = resident_create_q8(h, Wq0, sc0, (size_t)K, (size_t)N0, 0, 0,
                                    off, len);
        }
    } else {
        w0 = resident_find(h, W0, (size_t)K * N0 * 4);
        if (!w0) {
            size_t off[CLGEMM_MAX_DEV] = {0}, len[CLGEMM_MAX_DEV] = {0};
            len[0] = (size_t)N0;
            w0 = resident_create(h, W0, (size_t)K, (size_t)N0, 0, 0, off, len);
        }
    }
    if (is_q8_1) {
        w1 = resident_find(h, Wq1, (size_t)K * N1);
        if (!w1) {
            size_t off[CLGEMM_MAX_DEV] = {0}, len[CLGEMM_MAX_DEV] = {0};
            len[1] = (size_t)N1;
            w1 = resident_create_q8(h, Wq1, sc1, (size_t)K, (size_t)N1, 0, 1,
                                    off, len);
        }
    } else {
        w1 = resident_find(h, W1, (size_t)K * N1 * 4);
        if (!w1) {
            size_t off[CLGEMM_MAX_DEV] = {0}, len[CLGEMM_MAX_DEV] = {0};
            len[1] = (size_t)N1;
            w1 = resident_create(h, W1, (size_t)K, (size_t)N1, 0, 1, off, len);
        }
    }
    if (!w0 || !w1 || w0->len[0] != (size_t)N0 || w1->len[1] != (size_t)N1)
        return -1;
    if (bias0) {
        b0 = resident_find(h, bias0, (size_t)N0 * 4);
        if (!b0)
            b0 = resident_create(h, bias0, 1, (size_t)N0, 0, 0, w0->off,
                                 w0->len);
        if (!b0) return -1;
    }
    if (bias1) {
        b1 = resident_find(h, bias1, (size_t)N1 * 4);
        if (!b1)
            b1 = resident_create(h, bias1, 1, (size_t)N1, 0, 1, w1->off,
                                 w1->len);
        if (!b1) return -1;
    }
    if (stream_bcast_a(h, (size_t)K) != 0) return -1;
    if (scratch_ensure(h, 1, &D1->c_buf, &D1->c_cap, (size_t)N1 * 4) != 0)
        return -1;
    if (!h->stream.c_gather || h->stream.c_gather_cap < (size_t)N1) {
        float *p =
            (float *)realloc(h->stream.c_gather, (size_t)N1 * sizeof(float));
        if (!p) return -1;
        h->stream.c_gather = p;
        h->stream.c_gather_cap = (size_t)N1;
    }
    has0 = bias0 ? 1 : 0;
    has1 = bias1 ? 1 : 0;
    iK = K;
    iN0 = N0;
    iN1 = N1;

    /* Launch d0 → slot0 */
    if (is_q8_0) {
        cl_mem bm = b0 ? b0->mem[0] : w0->scale[0];
        if (h->SetArg(D0->kern_q8, 0, sizeof(cl_mem), &D0->a_buf) !=
                CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 1, sizeof(cl_mem), &w0->mem[0]) !=
                CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 2, sizeof(cl_mem), &w0->scale[0]) !=
                CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 3, sizeof(cl_mem), &bm) != CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 4, sizeof(cl_mem),
                      &h->stream.slot[slot0]) != CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 5, sizeof(int), &iT) != CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 6, sizeof(int), &iK) != CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 7, sizeof(int), &iN0) != CL_SUCCESS ||
            h->SetArg(D0->kern_q8, 8, sizeof(int), &has0) != CL_SUCCESS)
            return -1;
        if (enqueue_2d(h, D0->q, D0->kern_q8, (size_t)N0, 1) != 0) return -1;
    } else {
        cl_mem bm = b0 ? b0->mem[0] : w0->mem[0];
        if (h->SetArg(D0->kern, 0, sizeof(cl_mem), &D0->a_buf) != CL_SUCCESS ||
            h->SetArg(D0->kern, 1, sizeof(cl_mem), &w0->mem[0]) != CL_SUCCESS ||
            h->SetArg(D0->kern, 2, sizeof(cl_mem), &bm) != CL_SUCCESS ||
            h->SetArg(D0->kern, 3, sizeof(cl_mem), &h->stream.slot[slot0]) !=
                CL_SUCCESS ||
            h->SetArg(D0->kern, 4, sizeof(int), &iT) != CL_SUCCESS ||
            h->SetArg(D0->kern, 5, sizeof(int), &iK) != CL_SUCCESS ||
            h->SetArg(D0->kern, 6, sizeof(int), &iN0) != CL_SUCCESS ||
            h->SetArg(D0->kern, 7, sizeof(int), &has0) != CL_SUCCESS)
            return -1;
        if (enqueue_2d(h, D0->q, D0->kern, (size_t)N0, 1) != 0) return -1;
    }
    /* Launch d1 → c_buf */
    if (is_q8_1) {
        cl_mem bm = b1 ? b1->mem[1] : w1->scale[1];
        if (h->SetArg(D1->kern_q8, 0, sizeof(cl_mem), &D1->a_buf) !=
                CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 1, sizeof(cl_mem), &w1->mem[1]) !=
                CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 2, sizeof(cl_mem), &w1->scale[1]) !=
                CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 3, sizeof(cl_mem), &bm) != CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 4, sizeof(cl_mem), &D1->c_buf) !=
                CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 5, sizeof(int), &iT) != CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 6, sizeof(int), &iK) != CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 7, sizeof(int), &iN1) != CL_SUCCESS ||
            h->SetArg(D1->kern_q8, 8, sizeof(int), &has1) != CL_SUCCESS)
            return -1;
        if (enqueue_2d(h, D1->q, D1->kern_q8, (size_t)N1, 1) != 0) return -1;
    } else {
        cl_mem bm = b1 ? b1->mem[1] : w1->mem[1];
        if (h->SetArg(D1->kern, 0, sizeof(cl_mem), &D1->a_buf) != CL_SUCCESS ||
            h->SetArg(D1->kern, 1, sizeof(cl_mem), &w1->mem[1]) != CL_SUCCESS ||
            h->SetArg(D1->kern, 2, sizeof(cl_mem), &bm) != CL_SUCCESS ||
            h->SetArg(D1->kern, 3, sizeof(cl_mem), &D1->c_buf) != CL_SUCCESS ||
            h->SetArg(D1->kern, 4, sizeof(int), &iT) != CL_SUCCESS ||
            h->SetArg(D1->kern, 5, sizeof(int), &iK) != CL_SUCCESS ||
            h->SetArg(D1->kern, 6, sizeof(int), &iN1) != CL_SUCCESS ||
            h->SetArg(D1->kern, 7, sizeof(int), &has1) != CL_SUCCESS)
            return -1;
        if (enqueue_2d(h, D1->q, D1->kern, (size_t)N1, 1) != 0) return -1;
    }
    if (h->ReadBuffer(D1->q, D1->c_buf, CL_FALSE, 0, (size_t)N1 * 4,
                      h->stream.c_gather, 0, NULL, NULL) != CL_SUCCESS)
        return -1;
    h->Flush(D0->q);
    h->Flush(D1->q);
    for (d = 0; d < 2; ++d)
        if (h->Finish(h->d[d].q) != CL_SUCCESS) return -1;
    if (h->WriteBuffer(D0->q, h->stream.slot[slot1], CL_TRUE, 0, (size_t)N1 * 4,
                       h->stream.c_gather, 0, NULL, NULL) != CL_SUCCESS)
        return -1;
    return 0;
}
