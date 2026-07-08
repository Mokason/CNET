/*
 * scan.h -- reusable infrastructure for "certified step primitive + iterative scan"
 * composition. This is the pattern that let residue and expr escape full
 * domain enumeration for long/unbounded inputs: certify a small finite
 * transition table once, then compose via structural DAG + canonicalization.
 *
 * The facilities here (generalized data builder + the DAG builder already in
 * router) make new sequential/recursive domains much cheaper to add, directly
 * attacking the "enumerable interface" and "hand-build every scan" walls.
 */

#ifndef SCAN_H
#define SCAN_H

#include "nn.h"
#include "router.h"   /* for StepWiring and Dag* */
#include "contract/contract.h"

#include <stddef.h>
#include <stdint.h>

/* Shape of a step primitive for training table construction.
   Assumes one-hot (or equivalent code) fields; width = the size of the onehot
   (or binary total for the field). */
typedef struct {
    size_t num_in_ports;
    size_t in_widths[DAG_MAX_SLOTS];   /* code cardinality for each input port */
    size_t num_out_ports;
    size_t out_widths[DAG_MAX_SLOTS];
} StepShape;

/* User-provided ground-truth transition for one step.
   in_codes[0..num_in-1] are the integer codes for the input fields.
   Write the output codes to out_codes[0..num_out-1].
   ctx is the user context (e.g. b,k for residue or other params).
   The builder will one-hot encode everything (soft 0.9/0.1 targets). */
typedef void (*step_transition_fn)(const int *in_codes, size_t num_in,
                                   int *out_codes, size_t num_out,
                                   void *ctx);

/* Build the full cartesian training table for a step given its shape and the
   pure transition semantics. This eliminates the per-domain triple/quad
   nested for-loops.
   Returns the number of rows written (product of all in_widths).
   inputs  is rows * sum(in_widths)   (flat)
   targets is rows * sum(out_widths)  (soft 0.9/0.1) */
size_t build_step_training_data(const StepShape *shape,
                                step_transition_fn transition,
                                void *ctx,
                                double *inputs,
                                double *targets);

/* Convenience: certify a step that was built with the above (wraps
   contract_init_borrowed + btn_certify_robust). Useful for the inductive
   "certify the step, trust the scan" pattern. */
int certify_step(BinaryTransformNetwork *step,
                 const char *name,
                 const double *inputs,
                 const double *targets,
                 size_t samples,
                 double margin_floor,
                 CertifyReport *report);

/* Optional helper: given a certified step contract, produce a "scan contract"
   that layers on it (using the adaptive layering we added). The scan contract
   can use the same ports as the step (for the repeated state) or be extended.
   This is a thin wrapper around contract_set_parent + copy for the common case.
   For full long-N contracts you still usually rely on the step cert + structure. */
int make_layered_scan_contract(const Contract *step_contract,
                               const char *scan_name,
                               Contract *out_scan_contract);

/* Real contract_from_iterative_scan (next hardest after planner).
   Builds a small unrolled scan plan for 'small_n' using the builder,
   then emits a proper Contract for the composed behavior via the existing
   contract_from_dag machinery. Optionally layers on the step_contract
   using the new adaptive layering.
   This gives you a machine-checkable contract for the "long" behavior
   without having to hand-write the full teacher for arbitrary N. */
int contract_from_iterative_scan(
    BinaryTransformNetwork *step,
    const char *step_name,
    const StepWiring *wiring,
    const char *scan_name,
    size_t small_n,                 /* number of steps for the exemplar table */
    const DagSource *initial_states,
    size_t n_state,
    const DagSource *sample_items,  /* at least small_n items */
    const Contract *step_contract,  /* optional, for layering */
    size_t max_samples_for_emit,
    Contract *out
);

/* Easy runner wrapper (push item 3).
   Creates the nodes, sources, builds the plan with the builder, executes it,
   and returns the final state. Minimal boilerplate for new experiments.
   initial_state_values and item_values are the raw flat data (caller manages
   the memory for the duration of the call). */
int scan_run_simple(
    BinaryTransformNetwork *step,
    const StepWiring *wiring,
    const double *initial_state_values, /* concatenated for n_state slots */
    const double * const *item_values,  /* n_steps pointers to per-item data */
    size_t n_steps,
    double *final_out,
    size_t out_cap
);

/* Structural Merkle digest of a plan: by name/structure (NOT pointer), so two
   plans with identical topology hash equal even if allocated separately. 0 if
   plan/root is NULL. (Promoted from the structural-preference study.) */
uint64_t scan_plan_digest(const DagPlan *plan);

/* Smallest structural digest over the goal's plan equivalence class -- a stable,
   content-addressed dedup key. Runs a perturbation sweep (admission-time only,
   never the live planner). 0 if no plan exists. Footprint-free. */
uint64_t structural_canonical_digest(const PrimitiveRegistry *reg,
                                     const DagSource *sources, size_t n_sources,
                                     Port goal);

#endif

