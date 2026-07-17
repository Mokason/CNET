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
#include "acquire.h"   /* CnetOracleFn / CnetOracleIdentity for the remount resolver */
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

/* Copy the descriptor's full 256-bit artifact hash into out32 (the collision-
   resistant provenance record; artifact_digest is its 64-bit truncation).
   All-zero when the base predates the full hash. Returns 0, or <0 for a bad
   index/host/buffer. */
CNET_API int soul_oracle_artifact_sha256(SoulHost *h, int index,
                                         unsigned char out32[32]);

/* Linked-runtime provenance digest of an Oracle descriptor (the v5
   attestation: FNV over the process's loaded DSOs + glibc version).
   Returns 0 and writes *out on success. *out is the EXACT persisted
   value; 0 is a valid label meaning "linked runtime unattested"
   (pre-v5 base, or a platform without dl introspection) — never a
   refusal. <0 for a bad host, index, or out-pointer. This is a NEW
   tail-extension accessor; it does NOT alter soul_oracle_identity's
   ABI. */
CNET_API int soul_oracle_runtime_libs_digest(SoulHost *h, int index,
                                             uint64_t *out);

/* Direct unit -> descriptor relation: copy the name of the oracle descriptor
   that taught `name` into `out` ("" when the unit has no recorded teacher).
   Returns 0, -2 for an unknown unit, <0 for bad buffer/truncation. */
CNET_API int soul_unit_provenance(SoulHost *h, const char *name,
                                  char *out, int out_cap);

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

/* ---- counterfactual route evidence (REPORT-ONLY, opt-in) ------------------
   Set CNET_COUNTERFACTUAL=1 (any non-empty value other than "0") BEFORE
   soul_open to enable a shadow evidence channel on soul_route: after a route
   is SERVED, the certified same-shape roster is projected as an ephemeral CCE
   recall forest and the native counterfactual API ranks alternative routes
   and computes a conservative consistency score
   (cce_router_sample_counterfactuals + cce_router_consistency_score). The
   report is logged as one stderr telemetry line ("CNET_COUNTERFACTUAL
   REPORT ...") and kept for this accessor, so a host (e.g. the MCP server's
   existing counterfactualRoutes / counterfactualConsistency metadata channel
   on cnet_verify_claim) can attach it as metadata. Certification outranks
   evidence (docs/dispatch.md): the channel may RANK or REPORT, never decide —
   the served answer, every error code, and every refusal path are
   byte-identical with the knob on or off (gate: make counterfactual_serving).

   Copies the report attached to the LAST SERVED soul_route answer into out
   (NUL-terminated). Returns 0 on success, -1 bad args, -2 channel disabled
   (knob unset at soul_open), -3 no report (nothing served yet, or the last
   routed query was refused), -4 out_cap too small. */
CNET_API int soul_counterfactual_last(SoulHost *h, char *out, int out_cap);

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

/* Novel-goal request by EXPLICIT typed signature (families are PortFamily
   values). BOTH tags are required: an untagged input is a wildcard, which
   trivially "already satisfies" any same-shape goal — unservable by
   construction (-1). Serve-or-note semantics:

   - a certified plan exists: with `in` non-NULL, execute it (in_len must
     equal the input total) and write min(out_total, out_cap) doubles;
     returns the true out_total. With `in` NULL this is a capability probe:
     returns 0 (plannable) without executing.
   - no plan: the full signature is appended to the gap inbox
     (CNET_GAP_INBOX; silently skipped when unset) so the gap lane can
     acquire the capability, and -3 is returned.

   This is the serving half of gap_lane_execute: the request either runs
   now or becomes the lane's work — unknown goals no longer need an
   existing unit to be reportable. Other returns: -1 bad args, -4 buffer/
   length mismatch, -5 execution failure. */
CNET_API int soul_request(SoulHost *h,
                          int in_family, int in_width, int in_count,
                          const char *in_tag,
                          int goal_family, int goal_width, int goal_count,
                          const char *goal_tag,
                          const double *in, int in_len,
                          double *out, int out_cap);

/* ---- resolver-based Oracle remount (the reopen execution tracer) ----------
   Oracle callbacks cannot be serialized; the sealed base persists their
   identity, ports, and unit->descriptor provenance. soul_open loads only the
   native units and PROJECTS the oracle descriptors (no runtime trust).
   soul_mount_oracles turns projected descriptors back into LIVE, certified,
   plannable ORACLE specialists by asking the caller's resolver for the live
   callback and, for each descriptor:
     1. verifying the resolver's ASSERTED identity digest against the SEALED
        descriptor digest (a legacy/absent or mismatched identity is refused);
     2. recovering the sealed contract of a native unit whose provenance points
        at this descriptor (the oracle taught that unit, so its own sealed
        exemplars are the certification evidence — no self-certification);
     3. certifying the remounted adapter against that contract and admitting it
        through the one specialist door (specialist_admit), so the live registry
        entry is a certified ORACLE, planned/executed exactly like a native unit.
   Every non-admission is an EXPLICIT skip count in SoulMountReport, never
   silent runtime trust.

   CCE-model remount is NOT implemented: this closes the generic resolver
   mechanism with Oracle first. */

typedef struct {
    CnetOracleFn fn;             /* required; NULL => leave this descriptor unbound */
    void *ctx;                   /* opaque; forwarded to fn on every call */
    CnetOracleIdentity identity; /* the identity the caller asserts for fn */
    int has_identity;            /* must be nonzero: identity is verified, never trusted */
} SoulOracleBinding;

/* Resolve a persisted descriptor (name, kind) to a live binding. Return 0 with
   *out filled to bind; return non-zero (or leave out->fn NULL) to leave the
   descriptor unbound. Called once per descriptor; *out is zeroed before each
   call. Never serialize/return context pointers into the base. */
typedef int (*SoulOracleResolver)(const char *name, const char *kind,
                                  SoulOracleBinding *out, void *rctx);

typedef struct {
    int mounted;             /* admitted as live certified ORACLE specialists */
    int unbound;             /* resolver supplied no callback */
    int identity_mismatch;   /* asserted identity absent/legacy or != sealed digest */
    int missing_provenance;  /* no provenance-linked native unit/contract to certify against */
    int cert_failed;         /* adapter could not reproduce the recovered sealed contract */
} SoulMountReport;

/* Remount persisted oracle descriptors through `resolver` (REQUIRED — a NULL
   resolver returns -1; descriptor-only projection is soul_open's default and
   binds nothing). Already-mounted descriptors are left as-is (idempotent).
   Fills *report when non-NULL. Returns the number NEWLY mounted (>=0), or <0 on
   bad args / allocation failure. Ownership: the host owns and frees every bound
   OracleEntry, adapter BTN, and recovered contract exactly once in soul_close;
   callbacks/contexts are never serialized. */
CNET_API int soul_mount_oracles(SoulHost *h, SoulOracleResolver resolver,
                                void *rctx, SoulMountReport *report);

/* SpecialistKind of a LIVE registry unit (SpecialistKind numeric values:
   0 BTN, 1 CCE, 2 ORACLE). 0 on success with *kind filled (kind may be NULL for
   a pure existence check); <0 if the name is not a live registry unit — e.g. an
   oracle descriptor that has not been remounted. */
CNET_API int soul_unit_kind(SoulHost *h, const char *name, int *kind);

/* Number of oracle descriptors currently mounted as live ORACLE specialists.
   <0 on a bad/unloaded host. */
CNET_API int soul_mounted_oracle_count(SoulHost *h);

/* ---- Live residual (Tier C) + structure mine (P5) ------------------------
   When CNET_RESIDUAL_GGUF is set, soul_request falls back to a local GGUF
   residual after a certified miss (still notes the gap inbox so the learner
   can seal a permanent skill). Hermetic tests may set
   CNET_SOUL_RESIDUAL_HERMETIC=1 for a rot1 stand-in without a model.

   Residual answers are uncertified. soul_health_tick may structure-mine
   residual traces into certified units and seal them into the open CNB.
*/

/* Last soul_request / soul_route serve source. */
#define SOUL_SOURCE_NONE       0
#define SOUL_SOURCE_CERTIFIED  1
#define SOUL_SOURCE_RESIDUAL   2
#define SOUL_SOURCE_PROBE      3

typedef struct {
    uint64_t certified_serves;
    uint64_t residual_serves;
    uint64_t gap_notes;
    uint64_t structure_mines;
    uint64_t structure_seals;
    int residual_bound;   /* 1 if residual ready */
    int residual_window;  /* window n, or 0 */
    int last_source;      /* SOUL_SOURCE_* */
    int units;            /* live registry count */
} SoulServeStats;

/* Copy serve counters. Returns 0, or <0 on bad host. */
CNET_API int soul_serve_stats(SoulHost *h, SoulServeStats *out);

/* Last source for the most recent soul_request (SOUL_SOURCE_*). <0 bad host. */
CNET_API int soul_last_source(SoulHost *h);

/* Explicit structure-mine attempt (also invoked from soul_health_tick).
   Returns 0 if a unit was mined+sealed, 1 if nothing ripe, <0 on error. */
CNET_API int soul_structure_mine(SoulHost *h);

CNET_API void soul_close(SoulHost *h);

#ifdef __cplusplus
}
#endif

#endif /* SOUL_HOST_H */
