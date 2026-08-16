/* The reference ternary matmul, extracted VERBATIM from cce_block_forward so
   the bridge's equivalence harness can call it directly.

   Behaviour is unchanged: same 510-wide tiling, same per-output accumulation
   over i ascending, same arithmetic, same OpenMP work gate. The bit-identity
   of this kernel against the int8 ternary path depends on that accumulation
   order, so it must not be reassociated, tree-reduced or reordered.

   Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include "../../include/cce/cce_mojo_kernel.h"
#include "../../include/cce/cce_trit_lut.h"

#include <math.h>
#include <string.h>

/* Identical to the static sigmoid in cce_block.c. Duplicated rather than
   exported: a divergence here would silently change every output, so the
   definition is kept literal and next to its only user. */
static float trit_sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

void cce_trit_matmul_c(const float* input, const uint8_t* w_trit,
                       const float* w_scale, const float* bias,
                       float* output, int in_dim, int out_dim,
                       int w_trit_bpr, int apply_sigmoid) {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) \
            shared(output, input, w_trit, w_scale, bias, in_dim, out_dim, \
                   w_trit_bpr, apply_sigmoid, cce_trit_lut) \
            if((size_t)in_dim * (size_t)out_dim >= (size_t)1 << 21)
#endif
    for (int ob = 0; ob < out_dim; ob += 510) {
        int oe = (ob + 510 < out_dim) ? ob + 510 : out_dim;
        int w = oe - ob;
        int nb = (w + 4) / 5;             /* packed bytes covering the tile */
        int8_t codes[510 + 8];            /* +8: the last decode store overlaps */
        float* out = &output[ob];
        for (int o = 0; o < w; ++o) out[o] = 0.0f;
        for (int i = 0; i < in_dim; ++i) {
            const float a = input[i];
            const uint8_t* p = &w_trit[(size_t)i * w_trit_bpr + (size_t)(ob / 5)];
            for (int b = 0; b < nb; ++b)
                memcpy(&codes[b * 5], cce_trit_lut[p[b]], 8);
            for (int o = 0; o < w; ++o) out[o] += a * (float)codes[o];
        }
        for (int o = 0; o < w; ++o) {
            float v = bias[ob + o] + w_scale[ob + o] * out[o];
            out[o] = apply_sigmoid ? trit_sigmoid(v) : v;
        }
    }
}

uint64_t cce_mojo_anchor(const float* v, size_t n) {
    const unsigned char* b = (const unsigned char*)v;
    size_t total = n * sizeof(float), i;
    uint64_t h = 1469598103934665603ULL;      /* FNV-1a offset basis */
    for (i = 0; i < total; ++i) {
        h ^= (uint64_t)b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
