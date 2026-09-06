#ifndef CONTROLLER_NET_H
#define CONTROLLER_NET_H
#include "fixture.h"
#include "cce/cce_amdmath.h"
#define HIDDEN 64
#define DEPTH 4
#define BATCH 128
#define OUTPUTS 16 /* physical GPU tile; only ACTIONS logits are supervised */
#define JOINED (INPUTS+HIDDEN+1)
typedef struct {
    float w[DEPTH][HIDDEN*JOINED], out[OUTPUTS*(HIDDEN+1)];
    int depth, tied;
} Net;
typedef struct {
    float z[DEPTH*BATCH*JOINED], h[DEPTH*BATCH*HIDDEN];
    float dz[DEPTH*BATCH*HIDDEN], dh[BATCH*HIDDEN];
    float final[BATCH*(HIDDEN+1)], p[BATCH*OUTPUTS], dy[BATCH*OUTPUTS];
    float transpose[HIDDEN*HIDDEN];
} Scratch;
void net_init(Net *net, int depth, int tied, uint32_t seed);
/* All matrices use the requested device, or explicit CPU when gpu=NULL.
 * Non-finite results and GPU errors return -1. lr=0 is read-only prediction.
 * BPTT computes every derivative before any weight update. */
float net_step(Net *net, Scratch *s, const float *x, const float *y,
               int n, float lr, cce_amdmath *gpu);
unsigned long net_parameters(const Net *net);
unsigned long net_forward_flops(const Net *net);
#endif
