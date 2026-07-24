/* CNET MoE-of-parts: hard-route goal tag → certified unit (+ optional LoRA).
 * Not a dense MoE trainer — sparse activation across the registry. */
#ifndef CNET_MOE_H
#define CNET_MOE_H

#include "cnet_export.h"
#include "nn.h"
#include "router.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int hit;              /* 1 if served */
    int used_lora;        /* 1 if certified lora applied via forward_with_lora */
    int steps;            /* activated plan steps (1 for hard unit) */
    char unit[96];
} CnetMoeHit;

/* Try to serve goal_port.tag as a direct registry unit (exact name or acq_*).
 * Uses route_execute single-step; if REGISTRY has certified lora and serving
 * enabled, prefers registry_forward_with_lora when available via link.
 * Returns 0 served, 1 no expert, -1 error. */
CNET_API int cnet_moe_try_hard(PrimitiveRegistry *reg, Port input_port,
                               Port goal_port, const double *input, size_t in_len,
                               double *output, size_t out_cap, CnetMoeHit *hit);

#ifdef __cplusplus
}
#endif
#endif
