#ifndef LIBRARY_H
#define LIBRARY_H

#include <stddef.h>

#include "nn.h"
#include "router.h"
#include "consolidate.h"
#include "contract/contract.h"
#include "property.h"

/* DreamCoder-style library learning: re-plan a declared task list each pass,
   distill every proven multi-step plan into a certified chunk, stop at a
   fixed point. In-process only -- the registry starts empty each run. The
   planner/executor/consolidate cores are untouched; this driver only calls
   their public APIs. */

#define LIBRARY_MAX_SOURCES BTN_MAX_INPUT_PORTS  /* 8 */
#define LIBRARY_MAX_CHUNKS  64                    /* cap on chunks per call */

typedef struct {
    const char *name;        /* the invented chunk's name (atom [A-Za-z0-9_], <=63) */
    Port sources[LIBRARY_MAX_SOURCES];
    size_t n_sources;        /* valid range [1, LIBRARY_MAX_SOURCES]; 1 -> route task, >=2 -> dag task */
    Port goal;               /* single goal in v1 */
    /* Multi-output circuit goals. n_goals >= 2 selects the circuit path
       (evolve_circuit); n_goals 0 or 1 use the single `goal` field above
       (route/dag). Kept ALONGSIDE `goal` so every existing single-goal caller
       stays byte-identical (n_goals == 0). The dispatcher reads n_goals to know
       the topological intent of the task. */
    Port   goals[CIRCUIT_MAX_ROOTS];
    size_t n_goals;
} LibraryTask;

typedef struct {
    char names[LIBRARY_MAX_CHUNKS][CONTRACT_NAME_MAX];  /* invented, in order */
    BinaryTransformNetwork *chunks[LIBRARY_MAX_CHUNKS]; /* OWNED by the report */
    size_t chunk_count;
    size_t iterations_run;   /* passes actually executed */
    size_t rolled_back;      /* chunks rejected by the law guard */
    size_t deferred;         /* candidate plans skipped: EVIDENCE_CLEAR failed */
    /* 3A cost label, per minted chunk (same index as names[]/chunks[]).
       compression_ratio = teacher/student (>1 = chunk smaller than the sub-plan
       = beneficial). Advisory; no decision reads these. */
    size_t teacher_mac_estimate[LIBRARY_MAX_CHUNKS];
    size_t student_mac_estimate[LIBRARY_MAX_CHUNKS];
    double compression_ratio[LIBRARY_MAX_CHUNKS];
    int    compute_beneficial[LIBRARY_MAX_CHUNKS];
} LibraryReport;

/* Opt-in distillation gate. enabled = 0 -> byte-identical legacy library_evolve.
   evidence_threshold/min_evidence reuse the lifecycle promotion bar (0.9/16):
   every primitive in a candidate plan must clear them before its composition is
   distilled. */
typedef struct {
    int    enabled;
    double evidence_threshold;   /* default 0.9  */
    size_t min_evidence;         /* default 16   */
} LibraryGateConfig;

/* enabled 0, evidence_threshold 0.9, min_evidence 16. */
void library_gate_config_defaults(LibraryGateConfig *g);

/* Like library_evolve, but applies the admission gate (NULL gate == legacy). */
int library_evolve_gated(PrimitiveRegistry *reg,
                         const LibraryTask *tasks, size_t n_tasks,
                         const Property *laws, size_t n_laws,
                         const ConsolidateConfig *cfg,
                         const LibraryGateConfig *gate,
                         size_t max_iterations,
                         LibraryReport *report);

/* Evolve the library over `tasks`. `laws` (may be NULL when n_laws==0) are a
   post-consolidation regression guard: after a chunk registers, every law is
   re-checked and the chunk is rolled back if any law is violated. `cfg` NULL
   = consolidate_config_defaults. The loop runs at most max_iterations passes
   and stops early when a full pass invents nothing. The registry BORROWS each
   invented chunk; the report OWNS them (each chunks[i] must be
   heap-allocated with malloc/calloc so library_report_free can free it after
   btn_free). Call library_report_free AFTER you are done using reg.
   Returns 0. */
int library_evolve(PrimitiveRegistry *reg,
                   const LibraryTask *tasks, size_t n_tasks,
                   const Property *laws, size_t n_laws,
                   const ConsolidateConfig *cfg,
                   size_t max_iterations,
                   LibraryReport *report);

/* Free the invented chunk BTNs the report owns. Safe to call once, after the
   registry that borrowed them is no longer in use. */
void library_report_free(LibraryReport *report);

#endif

