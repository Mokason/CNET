#ifndef CCE_TRIT_LUT_H
#define CCE_TRIT_LUT_H

/* Compile-time decode table for the packed 1.6-bit ternary format
 * (5 base-3 trits per byte, code = trit - 1 in {-1,0,+1}).
 *
 * cce_trit_lut[b][k] == ((b / 3^k) % 3) - 1 for k in 0..4 — exactly what the
 * old per-trit `code = (b % 3) - 1; b /= 3;` chain produced, for ALL 256 byte
 * values including the >= 243 ones only corrupt files contain, so behavior
 * under the mutate fuzz gate is unchanged. Replacing the div/mod chain with a
 * table read keeps the arithmetic (and therefore the bits) identical while
 * removing the serial dependency from the kernels' inner loops.
 *
 * static const: each including TU gets its own 1.25 KB copy (two users).
 */

#include <stdint.h>

/* Rows are padded to 8 bytes (3 trailing zeros) so a decoder can emit one
 * byte's 5 codes with a single 8-byte copy at stride 5 (overlapping stores);
 * consumers that index [0..4] are unaffected. */
#define CCE_TRIT1(b)  { (int8_t)((b) % 3 - 1),      (int8_t)((b) / 3 % 3 - 1),  \
                        (int8_t)((b) / 9 % 3 - 1),  (int8_t)((b) / 27 % 3 - 1), \
                        (int8_t)((b) / 81 % 3 - 1), 0, 0, 0 }
#define CCE_TRIT4(b)  CCE_TRIT1(b), CCE_TRIT1((b)+1), CCE_TRIT1((b)+2), CCE_TRIT1((b)+3)
#define CCE_TRIT16(b) CCE_TRIT4(b), CCE_TRIT4((b)+4), CCE_TRIT4((b)+8), CCE_TRIT4((b)+12)
#define CCE_TRIT64(b) CCE_TRIT16(b), CCE_TRIT16((b)+16), CCE_TRIT16((b)+32), CCE_TRIT16((b)+48)

static const int8_t cce_trit_lut[256][8] = {
    CCE_TRIT64(0), CCE_TRIT64(64), CCE_TRIT64(128), CCE_TRIT64(192)
};

#undef CCE_TRIT1
#undef CCE_TRIT4
#undef CCE_TRIT16
#undef CCE_TRIT64

#endif /* CCE_TRIT_LUT_H */
