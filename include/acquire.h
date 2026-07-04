#ifndef ACQUIRE_H
#define ACQUIRE_H

/* Gap-triggered acquisition loop (v1): the router's capability gaps are
   recorded in a ledger; an oracle (reference implementation) supplies labels;
   the drain trains a candidate BTN and pushes it through the EXISTING
   gate -> certify -> seal -> register chain. A candidate is structurally
   invisible to the planner until it is a sealed, certified, registered unit.
   The ledger is a sidecar (CNET_STATS rules): never inside a weight file,
   load REPLACES, missing/malformed -> -1 with state untouched.
   Spec: docs/superpowers/specs/2026-07-02-gap-triggered-acquisition-loop-design.md */

#include <stddef.h>

#include "nn.h"
#include "router.h"
#include "contract/contract.h"
#include "contract/coverage.h"

#define ACQUIRE_MAX_ORACLES 16
#define ACQUIRE_NAME_MAX 64
#define ACQUIRE_REASON_MAX 64

/* unified base (include/base.h); forward-declared to avoid a header cycle */
struct CnetBase;

/* A label source: an in-process reference implementation. in has the input
   port's total values (canonical); the oracle writes the output port's total
   values into out.
   Returns 0 = labeled (added to the certified domain),
          <0 = refusal/error (counted as a reject),
          >0 = ABSTAIN: a valid measurement, but the teacher itself is
               ambiguous here (its own decision margin is below threshold),
               so the point is EXCLUDED from the certified domain rather
               than forcing the student to memorize a coin-flip. The domain
               cardinality still counts it, so coverage becomes SAMPLED and
               the Wilson bound accounts for the abstained residue. This is
               margin-aware certification: prove what the model is decisive
               about, decline to certify its noise. */
typedef int (*CnetOracleFn)(const double *in, double *out, void *ctx);

typedef struct {
    char name[ACQUIRE_NAME_MAX];
    Port input_port;
    Port output_port;
    CnetOracleFn fn;
    void *ctx;
    size_t calls;
    size_t rejects;   /* refusals + outputs that failed port_validate */
    size_t abstains;  /* margin-aware: teacher-ambiguous points excluded */
    /* 0/1 = serial (default). >1 = the fn is safe to call from this many
       threads concurrently (it dispatches per-thread state internally, e.g.
       one model instance per GPU); mining then fans enumeration out and
       compacts serially in index order — tables/counters stay identical. */
    size_t parallel_width;
} OracleEntry;

typedef struct {
    OracleEntry entries[ACQUIRE_MAX_ORACLES];
    size_t count;
} OracleRegistry;

typedef enum { GAP_NO_PLAN = 0, GAP_LOW_RELIABILITY = 1, GAP_HEALTH = 2 } GapKind;
typedef enum { GAP_OPEN = 0, GAP_DEFERRED = 1, GAP_CLOSED = 2 } GapStatus;

typedef struct {
    GapKind kind;
    GapStatus status;
    Port input_port;   /* zeroed for GAP_HEALTH (drain resolves from subject) */
    Port goal_port;    /* zeroed for GAP_HEALTH */
    char subject[ACQUIRE_NAME_MAX];      /* suspect unit; "" for NO_PLAN */
    char oracle[ACQUIRE_NAME_MAX];       /* matched oracle; "" until matched */
    char defer_reason[ACQUIRE_REASON_MAX]; /* atom; "" unless DEFERRED */
    size_t times_hit;
    size_t attempts;
    /* Exemplars captured by the fallback path (in-memory only, NOT persisted;
       canonical values). cap_inputs: cap_count x input total; cap_targets:
       cap_count x output total. */
    double *cap_inputs;
    double *cap_targets;
    size_t cap_count;
    size_t cap_limit;
} GapRecord;

typedef struct {
    GapRecord *gaps;
    size_t count;
    size_t capacity;
    /* BTNs minted by acquisition. The registry BORROWS them; the ledger OWNS
       them (and their names). Free with acquire_ledger_free AFTER the registry
       that borrowed them is no longer in use. */
    BinaryTransformNetwork **acquired;
    char (*acquired_names)[ACQUIRE_NAME_MAX];
    size_t acquired_count;
    size_t acquired_capacity;
} AcquireLedger;

typedef struct {
    size_t mine_budget;        /* enumerate the domain when card <= this (4096) */
    size_t sample_count;       /* deterministic stride samples otherwise (256) */
    /* Confidence-scheduled acquisition (DSpark-inspired: spend the mining
       budget by estimated survival). In SAMPLED mode a domain-spanning pilot
       of this many points is mined first; a degenerate pilot (all targets
       identical) defers class_imbalance immediately instead of after the
       full sample — measured 16x cheaper refusal of constant slices.
       0 disables (default 16). Exhaustive mode never pilots: a proven
       constant function is legitimate knowledge. */
    size_t pilot_count;
    double holdout_fraction;   /* sampled mode only: fraction excluded from
                                  training but kept in the contract (0.25) */
    double evidence_threshold; /* oracle validity-rate floor (0.9; mirrors
                                  library_gate_config_defaults) */
    size_t min_evidence;       /* min usable exemplars (16; mirrors same) */
    double min_accuracy_bound; /* Wilson lower-bound floor, sampled mode (0.95) */
    double wilson_z;           /* 1.96 = 95% */
    size_t exhaustive_cap;     /* cap for btn_certify_exhaustive (0 = builtin) */
    const char *unit_dir;      /* dir for sealed .cnu files; NULL = skip seal */
    /* Non-NULL -> acquired units seal INTO the unified base instead of loose
       .cnu files (tag governance enforced: a near-miss tag DEFERs the
       acquisition with reason "tag_collision"). unit_dir is ignored for
       sealing when base is set. */
    struct CnetBase *base;
    size_t capture_limit;      /* per-gap captured-exemplar cap (256) */
    /* training recipe (lean-teacher defaults; NEVER 1 hidden neuron) */
    size_t init_hidden;        /* 8 */
    size_t max_hidden;         /* 64 */
    double learning_rate;      /* 0.5 */
    unsigned int seed;         /* 42 */
    size_t max_epochs;         /* 4000 */
    size_t growth_window;      /* 200 */
    double target_loss;        /* 1e-4 */
    double min_improvement;    /* 1e-6 */
} AcquireConfig;

void acquire_config_defaults(AcquireConfig *cfg);

typedef struct {
    size_t examined;
    size_t closed;
    size_t deferred;
    size_t skipped_no_oracle;
    CertVerdict last_verdict;                 /* verdict of the last closed gap */
    double last_bound;       /* Wilson floor when SAMPLED; 1.0 when PROVEN */
    double last_min_margin;  /* worst certified output margin (see port_margin) */
    char last_unit_name[ACQUIRE_NAME_MAX];    /* unit minted by the last close */
    char last_defer_reason[ACQUIRE_REASON_MAX];
} AcquireReport;

void acquire_ledger_init(AcquireLedger *l);
void acquire_ledger_free(AcquireLedger *l);

/* Exact signature equality: family, field_width, field_count AND tag. */
int acquire_port_eq_public(Port a, Port b);

/* Register a named oracle for a (input_port -> output_port) link signature.
   Returns 0, or -1 (full, bad name atom, or duplicate name). */
int acquire_oracle_register(OracleRegistry *o, const char *name,
                            Port input_port, Port output_port,
                            CnetOracleFn fn, void *ctx);

/* Opt-in AFTER registering: declare the named oracle safe for concurrent
   fn calls from up to `width` threads. Only takes effect in OpenMP builds;
   mined exemplar tables and counters are identical to the serial path by
   construction (indexed slots, serial compaction). Returns 0, -1 unknown. */
int acquire_oracle_set_parallel(OracleRegistry *o, const char *name,
                                size_t width);

/* Note a gap. Coalesces: a record with the same (kind, ports, subject) gets
   times_hit incremented instead of a duplicate. DEFERRED records reopen
   (status back to OPEN, reason cleared) so a later drain retries them.
   Returns the record index, or -1 on OOM/bad args. */
int acquire_note_no_plan(AcquireLedger *l, Port input_port, Port goal_port);
int acquire_note_low_reliability(AcquireLedger *l, const char *subject,
                                 double score, double floor_used);
int acquire_note_health(AcquireLedger *l, const char *subject,
                        const char *reason_atom);

/* Plan-or-fallback execution (single process, plain function calls).
   Plan exists -> strict route_execute (normal path). No plan -> note the gap;
   if an oracle matches the task signature exactly, answer via the oracle
   (validate-then-canonicalize both ways) and CAPTURE the pair as a training
   exemplar on the gap record. Returns 0 on an answered task, -1 otherwise. */
int acquire_execute_or_fallback(PrimitiveRegistry *reg, AcquireLedger *l,
                                OracleRegistry *oracles,
                                const AcquireConfig *cfg,
                                Port input_port, Port goal_port,
                                const double *input, size_t in_len,
                                double *output, size_t out_cap);

/* Drain every OPEN gap that has a matching oracle:
   mine (captured + enumerated/stride-sampled) -> oracle-evidence gate ->
   train candidate -> holdout check (sampled mode) -> certify exhaustive
   (PROOF, or SAMPLE + Wilson floor) -> seal .cnu -> register -> replan check.
   Any failure -> status DEFERRED + reason atom, with registry, counters and
   disk byte-identical to before the attempt (DEFER is total). Returns 0. */
int acquire_drain(PrimitiveRegistry *reg, AcquireLedger *l,
                  OracleRegistry *oracles, const AcquireConfig *cfg,
                  AcquireReport *report);

/* Inline mode: the same drain body scoped to ONE task signature, invoked
   synchronously (notes the gap itself if new). Returns 0 iff the gap ended
   CLOSED (a plan now exists). */
int acquire_now(PrimitiveRegistry *reg, AcquireLedger *l,
                OracleRegistry *oracles, const AcquireConfig *cfg,
                Port input_port, Port goal_port, AcquireReport *report);

/* Sidecar persistence ("CNET_GAPS 1"). Statuses + counters + signatures only;
   captured exemplar buffers are runtime-only. Save returns 0/-1. Load REPLACES
   the ledger's gap records on success (acquired-BTN ownership is untouched);
   missing/malformed file -> -1 with *l untouched. */
int acquire_ledger_save(const AcquireLedger *l, const char *path);
int acquire_ledger_load(AcquireLedger *l, const char *path);

#endif /* ACQUIRE_H */
