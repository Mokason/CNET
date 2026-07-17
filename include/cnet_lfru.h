#ifndef CNET_LFRU_H
#define CNET_LFRU_H

/* Colibrì-style LFRU: frequency primary, recency secondary, 25%+ε hysteresis.
 * Header-only; used by forest residency (opt-in) and hybrid medium slots.
 *
 * Adapted from JustVugg/colibri c/tier.h (LFRU + swap hysteresis).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Score: one heat count outweighs up to 255 recency ticks. */
static inline uint64_t cnet_lfru_score(uint32_t heat, uint32_t last,
                                       uint32_t clock) {
    uint32_t age = clock - last;
    uint32_t recent = age < 255u ? 255u - age : 0u;
    return ((uint64_t)heat << 8) | (uint64_t)recent;
}

/* Hysteresis in score units: 25% of cold + 4 heat counts. */
static inline int cnet_lfru_beats(uint64_t hot_score, uint64_t cold_score) {
    return hot_score > cold_score + (cold_score >> 2) + (4ull << 8);
}

/* Pick victim among resident indices [0..nres) given heat/last arrays over
 * the full n items. Returns victim index into resident[] or -1. */
static inline int cnet_lfru_pick_victim(const uint32_t *heat,
                                        const uint32_t *last, uint32_t clock,
                                        const int *resident, int nres) {
    int cold = 0, z;
    if (!heat || !last || !resident || nres < 1) return -1;
    for (z = 1; z < nres; z++) {
        if (cnet_lfru_score(heat[resident[z]], last[resident[z]], clock) <
            cnet_lfru_score(heat[resident[cold]], last[resident[cold]],
                            clock))
            cold = z;
    }
    return cold;
}

/* Decay heat (right-shift). Call periodically to forget stale heat. */
static inline void cnet_lfru_decay(uint32_t *heat, int n) {
    int i;
    if (!heat || n < 1) return;
    for (i = 0; i < n; i++) heat[i] >>= 1;
}

#ifdef __cplusplus
}
#endif

#endif /* CNET_LFRU_H */
