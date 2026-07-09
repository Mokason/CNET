/* Exhaustive fp16 decode identity: all 65536 bit patterns through the LIVE
 * cce_gguf_f16_to_f32 vs an independently-written reference decode.
 *
 * Why exhaustive and why the live symbol: the 2026-07-09 subnormal bug
 * (normalize-to-0x200/mask-0x1FF instead of 0x400/0x3FF) decoded every
 * subnormal scale as (512+m)*2^-24 instead of m*2^-24 — up to 2x — so any
 * quant superblock whose f16 `d` landed subnormal dequantized a whole
 * 256-weight block wrong (the gemma4 France->garbage root cause). It was
 * invisible to CNET-vs-CNET identity tests and to sampled cross-checks
 * that never drew a subnormal scale. 65536 cases is cheaper than one
 * mined unit; there is no excuse to sample.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_gguf.h"

/* Independent reference: textbook decode via double math. */
static float ref_f16(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp = (h >> 10) & 0x1F;
    int mant = h & 0x3FF;
    double v;
    if (exp == 0) {
        v = ldexp((double)mant, -24);              /* subnormal: m * 2^-24 */
    } else if (exp == 31) {
        if (mant == 0) return sign ? -INFINITY : INFINITY;
        return NAN;
    } else {
        v = ldexp(1.0 + mant / 1024.0, exp - 15);  /* normal */
    }
    return (float)(sign ? -v : v);
}

int main(void) {
    uint32_t bad = 0, checked = 0, subnormals = 0;
    for (uint32_t b = 0; b <= 0xFFFF; b++) {
        uint16_t h = (uint16_t)b;
        float got = cce_gguf_f16_to_f32(h);
        float want = ref_f16(h);
        checked++;
        if (((h >> 10) & 0x1F) == 0 && (h & 0x3FF) != 0) subnormals++;
        if (isnan(want)) {
            if (!isnan(got)) {
                if (bad++ < 8)
                    fprintf(stderr, "f16 0x%04x: want nan got %.9g\n", h,
                            (double)got);
            }
            continue;
        }
        /* bit-exact for every non-nan value, signed zero included */
        uint32_t gb, wb;
        memcpy(&gb, &got, 4);
        memcpy(&wb, &want, 4);
        if (gb != wb) {
            if (bad++ < 8)
                fprintf(stderr,
                        "f16 0x%04x: want %.9g (0x%08x) got %.9g (0x%08x)\n",
                        h, (double)want, wb, (double)got, gb);
        }
    }
    printf("f16 identity: %u/%u bit-exact (%u subnormals covered)%s\n",
           checked - bad, checked, subnormals, bad ? " FAIL" : "");
    return bad ? 1 : 0;
}
