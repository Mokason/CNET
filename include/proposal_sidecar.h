#ifndef PROPOSAL_SIDECAR_H
#define PROPOSAL_SIDECAR_H

#include "router.h"

/* CNET-D v5.0 -- Proposal Sidecar (SHADOW_ONLY).
 *
 * An advisory "imagination lane" beside the deterministic core: a scripted
 * proposer produces candidate PLANS, the EXISTING strict executor validates
 * them, and the sidecar emits telemetry -- while provably changing nothing
 * about the planner, registry, executor, stats, or library.
 *
 * v5.0 answers ONLY: "Can CNET safely listen to a proposal lane without
 * granting it authority?" It does NOT try to improve anything (that is v5.1).
 *
 * The sidecar has zero authority by construction:
 *   - it receives the registry as a const PrimitiveRegistry * (read-only),
 *   - it never registers, certifies, re-orders, prunes, or re-plans,
 *   - and it snapshots+restores every borrowed BTN's reliability counters
 *     around strict validation so imagination leaves no footprint on the
 *     evidence reality reads (see proposal_sidecar_run_demo, Delta-4).
 */

/* Plan-typed envelope -- NOT a generic opaque payload + validator hook (that
   would create a second validation universe beside CNET). */
typedef enum {
    CNET_PROPOSAL_PLAN_ROUTE   = 1,
    CNET_PROPOSAL_PLAN_DAG     = 2,
    CNET_PROPOSAL_PLAN_CIRCUIT = 3
} CnetProposalPlanKind;

/* Documented growth path for v5.2+ (a proposal FAMILY: plan vs symbol). It is
   NOT a field in the v5.0 report -- a one-value shell adds nothing now and
   earns a home when symbol proposals exist:
     CNET_PROPOSAL_FAMILY_PLAN, CNET_PROPOSAL_FAMILY_SYMBOL (future). */

typedef struct {
    CnetProposalPlanKind kind;

    /* no-authority invariants (constants for v5.0; asserted in the gate) */
    int advisory_only;             /* 1 */
    int authority;                 /* 0 */
    int planner_influence;         /* 0 */
    int registry_mutation_allowed; /* 0 */
    int strict_validator_required; /* 1 */

    /* outcome counters -- "accepted" is never one bit (Delta-1) */
    int proposed_count;
    int strict_executed_ok_count;  /* ran with no out-of-domain handoff (strict mode) */
    int strict_rejected_count;     /* aborted: out-of-domain / type-inconsistent */
    int matched_expected_count;    /* executed AND output == fixture ground truth (when known) */

    const char *proposer_name;
    const char *task_key;
} CnetPlanProposalReport;

/* Runs the scripted v5.0 proposer over `reg` (READ-ONLY), strict-validates each
   candidate through dag_execute, fills *report. Returns 0 on success, -1 on a
   bad argument or setup failure.

   MUST NOT mutate reg, the BTNs' persisted state, OR the BTNs' reliability
   counters: it snapshots output_successes/output_failures for every registry
   BTN before validating the candidate batch and restores them verbatim after
   (Delta-4). Strict-executing candidates therefore leaves the stats reality
   reads bit-for-bit unchanged. */
int proposal_sidecar_run_demo(const PrimitiveRegistry *reg,
                              CnetPlanProposalReport *report);

/* CNET-D v5.1 -- Below-Beam Recovery (SHADOW_ONLY, detect + report).
 *
 * A shadow probe that detects the planner's beam blind spot: a CORRECT producer
 * ranked below the beam cutoff that the official beam-limited planner misses, but
 * a beam-lifted re-plan finds. It DETECTS and REPORTS only -- it does NOT act or
 * substitute. Zero authority; the v5.0 Delta-4 invariant carries forward (the
 * strict-validation pass snapshots+restores every borrowed BTN's counters). */
typedef struct {
    int advisory_only;              /* 1 */
    int authority;                  /* 0 */
    int planner_influence;          /* 0 */
    int registry_mutation_allowed;  /* 0 */
    int official_strict_ok;         /* official beam-limited plan strict-executed clean */
    int official_matched_expected;  /* ...and matched ground truth */
    int sidecar_strict_ok;          /* beam-lifted candidate strict-executed clean */
    int sidecar_matched_expected;   /* ...and matched ground truth */
    int found_valid_below_beam;     /* sidecar_matched_expected && recovered_producer_rank >= official_beam_limit */
    int recovered_producer_rank;    /* 0-based reliability rank of recovered root producer; -1 if none */
    size_t official_beam_limit;     /* the cutoff in force for the official run */
    const char *proposer_name;
    const char *task_key;
} CnetBelowBeamReport;

/* Read-only over reg. Runs the official (beam-limited) planner, then a beam-lifted
   re-plan on a SHALLOW COPY of reg, strict-validates the recovered candidate, and
   fills *report. MUST NOT mutate reg or any BTN's reliability counters (Delta-4:
   snapshot+restore). Returns 0 on success, -1 on bad args. */
int proposal_sidecar_below_beam_probe(const PrimitiveRegistry *reg,
                                      const DagSource *sources, size_t n_sources,
                                      Port goal, int expected_argmax,
                                      CnetBelowBeamReport *report);

#endif
