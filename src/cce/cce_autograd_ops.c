#include "../../include/cce/cce_autograd.h"
#include "../../include/cce/cce_tensor.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* This file contains implementations of differentiable ops and their VJPs.
 * Kept separate for clarity. Most heavy logic is called from cce_autograd.c
 */

/* Forward implementations that were in main for simplicity are in autograd.c in this version.
 * Additional specialized kernels can go here (e.g. fused ops).
 */

/* Placeholder for future optimized kernels (tiled matmul with saved activations, etc.) */
void cce_ag_ops_init(void) {
    /* no-op */
}
