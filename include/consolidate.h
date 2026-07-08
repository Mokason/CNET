#ifndef CONSOLIDATE_H
#define CONSOLIDATE_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* Chunk consolidation: distill a proven multi-step plan into a NEW single
   primitive. The plan's input domain is enumerated (canonical members only),
   each input is labeled by executing the plan as a STRICT teacher, a student
   BTN is trained on the labeled set, and the student is verified against the
   teacher over the whole domain. The verification outcome seeds the chunk's
   reliability counters -- honest evidence, so the existing plan-score
   objective starts preferring the 1-step chunk with no planner changes. The
   chunk is a regular primitive (standard weight file + stats sidecar); the
   planner/executor core is untouched. */

typedef struct {
    /* Student's starting hidden neurons; 0 = auto, min(max_hidden,
       max(input total, output total)). Do not start chunks from 1 neuron:
       the early bottleneck saturates outputs on a low-rank approximation
       that later growth cannot repair (dead sigmoid gradients). */
    size_t initial_hidden;
    size_t max_hidden;       /* dynamic-growth cap */
    double learning_rate;
    size_t max_epochs;
    size_t growth_window;
    double target_loss;
    double min_improvement;
    unsigned int seed;
    /* Verification gate: refuse (and register nothing) when fewer than this
       fraction of the kept samples verify. verified = RAW output in-domain
       (the same bar the executors score) AND canonical output identical to
       the teacher's label. */
    double min_verify_rate;
    /* Enumeration cap: refuse domains larger than this instead of sampling. */
    size_t max_samples;
} ConsolidateConfig;

/* initial_hidden 0 (auto), max_hidden 128, learning_rate 0.8, max_epochs
   160000, growth_window 1000, target_loss 0.0015, min_improvement 0.01,
   seed 131, min_verify_rate 1.0, max_samples 4096. */
void consolidate_config_defaults(ConsolidateConfig *cfg);

typedef struct {
    size_t samples;         /* enumerated inputs the teacher labeled */
    size_t teacher_aborts;  /* inputs the strict teacher refused (excluded) */
    size_t verified;        /* student raw in-domain AND canonical == label */
    size_t missed;          /* kept samples that failed verification */
    double final_loss;      /* btn_train_dynamic result */
} ConsolidateReport;

/* Distill a route plan (length >= 2) into a single-input chunk. The chunk's
   input port is the first step's input port, its output port the last step's
   output port (tags ride along). cfg NULL = defaults; report may be NULL.
   Returns 0 with *out_student trained, ports set and counters seeded (caller
   saves/registers/frees), or -1 on refusal -- length < 2, a RAW or
   over-the-cap input domain, an empty teacher-labeled set, or a student
   below min_verify_rate -- with *out_student untouched. Member reliability
   counters are snapshotted before the teacher pass and restored after it:
   distillation sweeps are not deployment experience. */
int consolidate_route(
    const RoutePlan *plan,
    const ConsolidateConfig *cfg,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
);

/* Distill a DAG plan (a DAG_PRIMITIVE root and >= 2 primitive executions)
   into a multi-input chunk. Input ports are sources[0..n_sources-1].type in
   INDEX ORDER (source values are not read); every source must be consumed
   exactly once so the chunk's arity tells the truth. The output port is the
   root's projected output port. Same contract as consolidate_route
   otherwise. */
int consolidate_dag(
    const DagPlan *plan,
    const DagSource *sources,
    size_t n_sources,
    const ConsolidateConfig *cfg,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
);

/* Distill a multi-root circuit (primitive roots, >= 2 DISTINCT primitive
   executions) into a MULTI-OUTPUT chunk: the student's output ports are
   the roots' projected ports in goal order (tags ride along), and
   verification requires every segment in-domain and exact. Every source
   must be referenced exactly once (counted sharing-aware: a shared node's
   sources count once). Same contract as consolidate_dag otherwise. */
int consolidate_circuit(
    const CircuitPlan *plan,
    const DagSource *sources,
    size_t n_sources,
    const ConsolidateConfig *cfg,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
);

#endif
