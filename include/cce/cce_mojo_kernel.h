#ifndef CCE_MOJO_KERNEL_H
#define CCE_MOJO_KERNEL_H

/* The C ABI boundary for Mojo kernels.
 *
 * ONLY primitives and pointers cross this line — no cce_block, no cce_tensor,
 * no Mojo type. This header is hand-written C and is the contract both sides
 * compile against; Mojo never generates it.
 *
 * Mojo is never load-bearing: everything here works with the toolchain
 * absent, in which case the C reference below is the only implementation.
 * Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ternary (1.6-bit packed) matvec, the reference C implementation.
 *   out[o] = bias[o] + w_scale[o] * SUM_i input[i] * code(i,o)
 * with code in {-1,0,+1} unpacked from w_trit, then sigmoid when
 * apply_sigmoid != 0.
 *
 * Accumulation is per-output over i ASCENDING and must stay that way: the
 * bit-identity of this kernel against the int8 path depends on it. */
void cce_trit_matmul_c(const float* input, const uint8_t* w_trit,
                       const float* w_scale, const float* bias,
                       float* output, int in_dim, int out_dim,
                       int w_trit_bpr, int apply_sigmoid);

/* FNV-1a over the raw bytes of a float array. Used as the bit-identity
 * ANCHOR: a printed float would compare one value, this covers every output
 * byte. Never used for anything but comparison. */
uint64_t cce_mojo_anchor(const float* v, size_t n);

/* 1 only when the Mojo kernel is BOTH compiled in (CNET_HAVE_MOJO) and
 * enabled at runtime (CNET_MOJO=1). Default is off: building with the
 * toolchain present must not change behaviour until explicitly asked. */
int cce_mojo_available(void);

/* Route one ternary matvec. Returns 0 when the Mojo kernel handled it, or
 * -1 when the caller must run cce_trit_matmul_c instead. A Mojo kernel is
 * allowed to be partial: declining is normal, not an error. */
int cce_mojo_dispatch_trit(const float* input, const uint8_t* w_trit,
                           const float* w_scale, const float* bias,
                           float* output, int in_dim, int out_dim,
                           int w_trit_bpr, int apply_sigmoid);

/* Count of calls where the Mojo kernel was attempted and failed, so a
 * degraded box is visible rather than silently slow. */
unsigned long cce_mojo_fallback_count(void);

#ifdef CNET_HAVE_MOJO
/* Implemented in mojo/trit_matmul.mojo via the C-ABI export.
 * Returns 0 on success, non-zero to decline or on error. */
int cnet_mojo_trit_matmul(const float* input, const uint8_t* w_trit,
                          const float* w_scale, const float* bias,
                          float* output, int in_dim, int out_dim,
                          int w_trit_bpr, int apply_sigmoid);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CCE_MOJO_KERNEL_H */
