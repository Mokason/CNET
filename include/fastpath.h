#ifndef FASTPATH_H
#define FASTPATH_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* Numeric representation of the fast lane. FP_INT carries a bit-width: 8 is the
   lever; small widths exist to prove the certification gate bites. */
typedef enum { FP_F64, FP_F32, FP_INT } FastKind;

typedef struct {
    FastKind kind;
    int int_bits;   /* used only when kind == FP_INT (e.g. 8) */
} FastPrecision;

typedef struct CompiledRoute CompiledRoute;

/* Pack every step's weights into `prec` ONCE and size scratch to `batch`
   samples. Reads the plan's BTNs read-only; the double core is untouched.
   Returns NULL on bad arguments or allocation failure. Phase 1 never rejects on
   margin -- it REPORTS via fp_route_min_margin. */
CompiledRoute *fp_route_compile(const RoutePlan *plan, FastPrecision prec,
                                size_t batch);

/* Run n (<= batch) inputs through the compiled route. Input i is in_total
   contiguous doubles at in + i*in_total; output i is out_total contiguous
   doubles at out + i*out_total (out_cap_total must be >= n*out_total). Every
   handoff is validated + canonicalized like route_execute; the worst per-handoff
   margin folds into the route's running minimum. Returns 0, or -1 on a
   bad/ambiguous handoff or argument. */
int fp_route_run(CompiledRoute *cr, const double *in, size_t n,
                 double *out, size_t out_cap_total);

/* Worst margin over all handoffs and samples since the last reset (starts at
   0.5, the maximum binary margin). */
double fp_route_min_margin(const CompiledRoute *cr);
void   fp_route_reset_margin(CompiledRoute *cr);

size_t fp_route_in_total(const CompiledRoute *cr);
size_t fp_route_out_total(const CompiledRoute *cr);

void fp_route_free(CompiledRoute *cr);

/* ---- batched DAG execution (mirrors dag_execute over N samples) ---- */

typedef struct CompiledDag CompiledDag;

/* Pack every primitive node's weights into `prec` ONCE and size scratch +
   per-node output buffers to `batch` samples. `source_types[i]` is the Port for
   DAG source i (the plan's source nodes carry only an index; the types come from
   the same array passed to dag_plan). Reads the plan's BTNs read-only. Returns
   NULL on bad arguments or allocation failure. Reports margin; never rejects. */
CompiledDag *fp_dag_compile(const DagPlan *plan, const Port *source_types,
                            size_t n_sources, FastPrecision prec, size_t batch);

/* Run n (<= batch) samples. `src_batches[i]` points to n contiguous source-i
   vectors (each source_types[i] total wide). The root's projected segment is
   written to out + k*out_total for sample k (out_cap_total >= n*out_total).
   Every handoff is validated + canonicalized like eval_node; the worst
   primitive-output margin folds into the running minimum. Returns 0, or -1 on a
   bad/ambiguous handoff or argument. */
int fp_dag_run(CompiledDag *cd, const double *const *src_batches, size_t n,
               double *out, size_t out_cap_total);

double fp_dag_min_margin(const CompiledDag *cd);
void   fp_dag_reset_margin(CompiledDag *cd);
size_t fp_dag_out_total(const CompiledDag *cd);

void fp_dag_free(CompiledDag *cd);

/* ---- batched multi-root circuit execution (mirrors dag_execute_circuit) ---- */

typedef struct CompiledCircuit CompiledCircuit;

/* Pack every root's node graph into `prec` ONCE over a SHARED graph (a node read
   by several roots is compiled, and evaluated, exactly once) and size scratch to
   `batch`. `source_types[i]` is the Port for circuit source i. Returns NULL on
   bad arguments or allocation failure. Reports margin; never rejects. */
CompiledCircuit *fp_circuit_compile(const CircuitPlan *plan, const Port *source_types,
                                    size_t n_sources, FastPrecision prec, size_t batch);

/* Run n (<= batch) samples. `src_batches[i]` points to n contiguous source-i
   vectors. Each root's projected segment is written concatenated in goal order to
   out + k*out_total for sample k (out_cap_total >= n*out_total). Returns 0, or -1
   on a bad/ambiguous handoff or argument. */
int fp_circuit_run(CompiledCircuit *cc, const double *const *src_batches, size_t n,
                   double *out, size_t out_cap_total);

double fp_circuit_min_margin(const CompiledCircuit *cc);
void   fp_circuit_reset_margin(CompiledCircuit *cc);
size_t fp_circuit_out_total(const CompiledCircuit *cc);
/* Number of distinct compiled nodes in the shared graph (a node shared by
   several roots counts once) -- lets a test confirm shared-eval-once. */
size_t fp_circuit_node_count(const CompiledCircuit *cc);

void fp_circuit_free(CompiledCircuit *cc);

#endif
