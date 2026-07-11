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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SoulHost SoulHost;

/* Open a certified base (model_path may be NULL). 0 on success, <0 on error. */
CNET_API int soul_open(const char *base_path, const char *model_path,
                       SoulHost **out);

/* Number of units admitted to the live registry after certification replay.
   This is the canonical roster; sidecars are not consulted. */
CNET_API int soul_unit_count(SoulHost *h);

/* Copy the certified unit name at `index` into `out` (always NUL-terminated on
   success). Returns 0, or <0 for invalid index/buffer/truncation. */
CNET_API int soul_unit_name(SoulHost *h, int index, char *out, int out_cap);

/* Oracle descriptors are persisted provenance/intent, not automatically
   admitted runtime primitives. These functions project the authoritative
   native CNB roster without binding or claiming execution trust. */
CNET_API int soul_oracle_count(SoulHost *h);
CNET_API int soul_oracle_name(SoulHost *h, int index, char *out, int out_cap);
CNET_API int soul_oracle_kind(SoulHost *h, int index, char *out, int out_cap);
CNET_API int soul_oracle_identity(
    SoulHost *h, int index,
    uint64_t *behavior_digest,
    uint64_t *artifact_digest,
    uint64_t *contract_digest,
    uint64_t *config_digest,
    uint64_t *retrieval_snapshot_digest,
    uint64_t *toolchain_digest);

/* Port totals (in doubles) of a named unit, so a host can size its buffers
   BEFORE running. 0 on success (fills both totals when non-NULL), <0 if absent. */
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

/* ---- runtime health (specialist_health_pass over the live registry) ----
   One maintenance pass with the base as the contract source: contracts are
   rematerialized on demand from the sealed unit blobs (cnb_get_unit) and
   cached for the host's lifetime, so heal re-certifies against the SAME
   sealed truth the unit was admitted with. Fix-then-improve, certified
   paths only; a healthy soul is a no-op tick.

   Fills counts[0..SOUL_HEALTH_COUNTS-1] (up to counts_cap):
     0 entries             1 demoted_by_audit    2 labeled_from_contract
     3 labeled_via_teacher 4 heal_attempted      5 healed
     6 promoted_provisional 7 shadows_promoted   8 reset_remaining
     9..12 trust histogram (uncertified/evidenced/certified/demoted)
   Returns the number of counts written (SOUL_HEALTH_COUNTS when counts_cap
   allows), or <0 on error. */
#define SOUL_HEALTH_COUNTS 13
CNET_API int soul_health_tick(SoulHost *h, long long *counts, int counts_cap);

/* Trust/role axes of a named unit on the shared Specialist vocabulary
   (SpecialistTrust / SpecialistRole numeric values). Either out pointer may
   be NULL. 0 on success, <0 if the unit is not in the live registry. */
CNET_API int soul_unit_axes(SoulHost *h, const char *name,
                            int *trust, int *role);

CNET_API void soul_close(SoulHost *h);

#ifdef __cplusplus
}
#endif

#endif /* SOUL_HOST_H */
