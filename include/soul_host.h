#ifndef SOUL_HOST_H
#define SOUL_HOST_H

/* Small, P/Invoke-friendly C ABI for hosts to reach the CNET certified
 * engine: load a .cnb base (certified units + registry), query a unit's
 * port sizes, run a named unit safely, route to a unit by its REAL typed
 * ports, and read its (real, Laplace-smoothed) reliability.
 *
 * Modeled on tests/soul_query.c + the registry + route_execute. Build into
 * the CNET shared lib: make cnet_dll.
 */

#include "cnet_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SoulHost SoulHost;

/* Open a certified base (model_path may be NULL). 0 on success, <0 on error. */
CNET_API int soul_open(const char *base_path, const char *model_path,
                       SoulHost **out);

/* Port totals (in doubles) of a named unit, so a host can size its buffers
   BEFORE running. 0 on success (fills *in_total/*out_total), <0 if absent. */
CNET_API int soul_unit_dims(SoulHost *h, const char *name,
                            int *in_total, int *out_total);

/* Run a named certified unit. Writes min(out_total, out_cap) doubles into
   `out` (never overflows). Returns the unit's TRUE out_total (>= 0; a value
   greater than out_cap means the caller's buffer truncated it), or <0 on
   error. `in` must be sized to the unit's in_total (see soul_unit_dims). */
CNET_API int soul_run(SoulHost *h, const char *name,
                      const double *in, double *out, int out_cap);

/* Route to the unit that owns `goal_tag`, using its REAL input/output ports
   (route_plan over the loaded registry + route_execute). Returns out_total
   written (>= 0) or <0 (e.g. -3 = no plan). A genuine typed-port route. */
CNET_API int soul_route(SoulHost *h, const char *goal_tag,
                        const double *in, int in_cap, double *out, int out_cap);

/* Real Laplace-smoothed reliability of a named unit from the loaded registry,
   scaled x1000 (920 = 0.920). Fresh units read 500 (0.5) until executed —
   the actual evidence, not a hard-coded constant. <0 if not found. */
CNET_API int soul_unit_reliability_milli(SoulHost *h, const char *name);

CNET_API void soul_close(SoulHost *h);

#ifdef __cplusplus
}
#endif

#endif /* SOUL_HOST_H */
