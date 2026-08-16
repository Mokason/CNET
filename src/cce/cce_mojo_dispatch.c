/* The ONLY file that knows whether Mojo exists.
 *
 * Mojo is never load-bearing: this file declines whenever the kernel is not
 * compiled in, not enabled, or fails, and the caller runs the C reference.
 * Deleting mojo/, the MOJO_PROBE stanza and this file's CNET_HAVE_MOJO branch
 * must return the tree to exactly its pre-bridge behaviour.
 *
 * Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include "../../include/cce/cce_mojo_kernel.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned long g_fallbacks = 0;

int cce_mojo_available(void) {
#ifdef CNET_HAVE_MOJO
    {
        const char* e = getenv("CNET_MOJO");
        return (e && e[0] == '1') ? 1 : 0;
    }
#else
    {
        /* Asking for Mojo on a build without it is a misconfigured box, not a
           preference. Say so once rather than being silently slow. */
        static int warned = 0;
        const char* e = getenv("CNET_MOJO");
        if (e && e[0] == '1' && !warned) {
            warned = 1;
            fprintf(stderr, "CNET_MOJO=1 but this build has no Mojo kernel "
                            "(CNET_HAVE_MOJO undefined); using the C path\n");
        }
    }
    return 0;
#endif
}

int cce_mojo_dispatch_trit(const float* input, const uint8_t* w_trit,
                           const float* w_scale, const float* bias,
                           float* output, int in_dim, int out_dim,
                           int w_trit_bpr, int apply_sigmoid) {
    if (!cce_mojo_available()) return -1;
#ifdef CNET_HAVE_MOJO
    {
        /* The Mojo runtime must be initialised before runtime-dependent APIs
           when called from a non-Mojo host: no Mojo main() runs here. The call
           is idempotent, but gate it anyway so the cost is paid once. */
        static int inited = 0;
        if (!inited) { cnet_mojo_init(); inited = 1; }
    }
    if (cnet_mojo_trit_matmul(input, w_trit, w_scale, bias, output,
                              in_dim, out_dim, w_trit_bpr,
                              apply_sigmoid) == 0)
        return 0;
    /* The kernel was enabled and attempted but did not handle the call.
       Counted so a degraded box is visible instead of merely slow. */
    g_fallbacks++;
#else
    (void)input; (void)w_trit; (void)w_scale; (void)bias; (void)output;
    (void)in_dim; (void)out_dim; (void)w_trit_bpr; (void)apply_sigmoid;
#endif
    return -1;
}

unsigned long cce_mojo_fallback_count(void) { return g_fallbacks; }
