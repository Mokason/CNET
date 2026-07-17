#ifndef CNET_PILOT_H
#define CNET_PILOT_H

/* PILOT-style research prefetch (Colibrì-inspired, P5 research).
 *
 * Records a ring of "next specialist / residual slot" hints while the current
 * step runs. A consumer (tier readahead or residual batch) may drain hints.
 * Default OFF: CNET_PILOT=1 enables recording; no semantic change to answers.
 *
 * Gate covered by make colibri_integrate.
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_PILOT_RING 32

typedef struct {
    int enabled; /* 1 when CNET_PILOT=1 */
    int head, tail, count;
    int hints[CNET_PILOT_RING]; /* opaque ids (layer, window slot, map idx) */
    uint64_t recorded;
    uint64_t drained;
    uint64_t hits; /* consumer reported useful */
} CnetPilot;

CNET_API void cnet_pilot_init(CnetPilot *p);
CNET_API void cnet_pilot_from_env(CnetPilot *p);
CNET_API int cnet_pilot_push(CnetPilot *p, int hint_id);
/* Pop oldest; returns 1 and *out=id, or 0 if empty. */
CNET_API int cnet_pilot_pop(CnetPilot *p, int *out);
CNET_API void cnet_pilot_note_hit(CnetPilot *p);

#ifdef __cplusplus
}
#endif

#endif /* CNET_PILOT_H */
