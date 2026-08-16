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
#include <stdint.h>

#include "nn.h"
#include "router.h"
#include "contract/contract.h"
#include "contract/coverage.h"

#define ACQUIRE_MAX_ORACLES 32
#define ACQUIRE_NAME_MAX 64
#define ACQUIRE_REASON_MAX 64
#define ACQUIRE_DEFER_WAITING_ORACLE "waiting_oracle"

/* unified base (include/base.h); forward-declared to avoid a header cycle */
struct CnetBase;

/* attribution sink event (include/attribution.h); forward-declared for the
   same reason — attribution.h must NOT include acquire.h */
struct AttributionEvent;

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

/* Optional batched probe: fill `count` output rows for `count` contiguous
   input rows in ONE call (per-point verdicts in rcs: 0 ok, -1 reject,
   +1 abstain). Returns 0, or <0 when the whole call failed — the miner
   then falls back to the serial fn for those points. Semantics must be
   IDENTICAL to `count` serial fn calls; the flagship harness enforces
   this with a startup equivalence gate. */
typedef int (*CnetOracleBatchFn)(const double *in, double *out, int *rcs,
                                 size_t count, void *ctx);

/* Evidence-carrying Oracle ABI v2. The callback still writes into caller-owned
   output storage; the result carries governance metadata, never an owning
   pointer. Version + struct_size make tail extension ABI-safe. */
#define CNET_ORACLE_ABI_VERSION 2u

typedef enum {
    CNET_ORACLE_ANSWER = 0,
    CNET_ORACLE_ABSTAIN_AMBIGUOUS = 1,
    CNET_ORACLE_ABSTAIN_UNDETERMINED = 2,
    CNET_ORACLE_REFUSE_POLICY = 3,
    CNET_ORACLE_FAIL_TRANSIENT = 4,
    CNET_ORACLE_FAIL_PERMANENT = 5,
    CNET_ORACLE_INVALID_OUTPUT = 6,
    CNET_ORACLE_INVALID_INPUT = 7,
    CNET_ORACLE_CANCELLED = 8,
    CNET_ORACLE_DEADLINE_EXCEEDED = 9,
    CNET_ORACLE_STATUS_COUNT = 10
} CnetOracleStatus;

typedef enum {
    CNET_ORACLE_VALIDITY_UNDETERMINED = 0,
    CNET_ORACLE_VALID = 1,
    CNET_ORACLE_INVALID = 2
} CnetOracleValidity;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t artifact_digest;
    uint64_t contract_digest;
    uint64_t config_digest;
    uint64_t retrieval_snapshot_digest;
    uint64_t toolchain_digest;
    /* Full 256-bit artifact hash (tail extension — struct_size/abi_version keep
       it ABI-safe; all-zero = not recorded). artifact_digest is its 64-bit
       truncation and stays the fast behavior-digest input; this is the
       collision-resistant provenance RECORD, so an accidental 64-bit collision
       is still distinguishable by the full hash. Deliberately NOT folded into
       cnet_oracle_identity_digest, so bases written before it still load. */
    unsigned char artifact_sha256[32];
    /* Linked-runtime attestation (tail extension #2, same ABI rules as the
       full hash above): FNV over the process's loaded DSOs — (soname,
       .note.gnu.build-id) pairs in soname-sorted order — plus the glibc
       version. Teaching numerics depend on libm/OpenMP the toolchain digest
       never sees; this records that layer. 0 = linked runtime unattested
       (pre-v5 base, or a platform without dl introspection) — a visible
       label, never a refusal. NOT folded into cnet_oracle_identity_digest. */
    uint64_t runtime_libs_digest;
} CnetOracleIdentity;

/* The current process's linked-runtime digest (see runtime_libs_digest).
   Returns 0 when the platform offers no dl introspection (non-glibc). */
uint64_t cnet_runtime_libs_digest(void);

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    CnetOracleStatus status;
    uint32_t flags;                 /* reserved; must be zero in v2 */
    double confidence;              /* ANSWER only: finite in [0,1] */
    uint64_t evidence_digest;       /* 0 = no attached evidence artifact */
    uint64_t oracle_identity_digest;/* filled authoritatively by invoke */
} CnetOracleResult;

/* Return 0 when `result` was populated. Non-zero means the callback violated
   the transport/ABI contract and is recorded as FAIL_PERMANENT. */
typedef int (*CnetOracleFnV2)(const double *in, size_t in_count,
                              double *out, size_t out_count,
                              CnetOracleResult *result, void *ctx);

/* Semantic validity is separate from output representation validity. A
   verifier may accept one of many correct proofs/plans/programs, reject it,
   or decline to decide. */
typedef CnetOracleValidity (*CnetOracleValidateFn)(
    const double *in, size_t in_count,
    const double *out, size_t out_count,
    const CnetOracleResult *result, void *ctx);

/* v2 batch row ABI: one full CnetOracleResult per row (never collapse to int). */
typedef int (*CnetOracleBatchFnV2)(const double *in, size_t in_stride,
                                   double *out, size_t out_stride,
                                   size_t count,
                                   CnetOracleResult *results,
                                   void *ctx);

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
    CnetOracleBatchFn fn_batch;  /* optional; NULL = serial only */
    size_t batch_hint;           /* preferred points per fn_batch call */
    CnetOracleBatchFnV2 fn_batch_v2; /* optional v2 batch (full per-row results) */
    /* v2 append-only extension. Exactly one of fn/fn_v2 is required. */
    CnetOracleFnV2 fn_v2;
    CnetOracleValidateFn validator;
    CnetOracleIdentity identity;
    uint64_t behavior_digest;
    size_t status_counts[CNET_ORACLE_STATUS_COUNT];
    CnetOracleResult last_result;
    /* ---- Teacher runtime (Tier A/B, 2026-07-21) ---- */
    char family[ACQUIRE_NAME_MAX]; /* optional teacher family atom; "" = none */
    int retired;                   /* 1 = not eligible to teach until rebound */
    char retire_reason[ACQUIRE_REASON_MAX];
    uint64_t lease_gen;            /* 0 = unbound; else active lease id */
    size_t seals_produced;         /* closed units taught by this oracle */
    size_t unfit_closes;           /* drain attempts ending oracle_unfit */
} OracleEntry;

/* Deploy/teach policy for an OracleRegistry (zero-init = hermetic-compatible). */
typedef struct {
    /* When 1, only fully attested identities may teach (non-zero SHA-256 and
       non-zero toolchain_digest). Hermetic tests leave this 0. */
    int require_attested_to_teach;
    /* When 1, acquire_oracle_register (v1) is refused; use v2. */
    int v2_only_new;
    /* When 1, v2 register without a semantic validator is refused for
       non-PROOF/sampled teaching paths (caller opts in for production). */
    int require_validator;
    /* Auto-retire when unfit_closes/calls exceeds rate after min_calls. */
    double retire_unfit_rate;   /* 0 = disabled; e.g. 0.5 */
    size_t retire_min_calls;    /* default 0 → treated as 16 when rate > 0 */
    /* When 1, only leased oracles may teach (after acquire_oracle_bind). */
    int require_lease_to_teach;
} OraclePolicy;

typedef struct {
    OracleEntry entries[ACQUIRE_MAX_ORACLES];
    size_t count;
    OraclePolicy policy;
    uint64_t next_lease_gen; /* monotonic lease issuer; 0 reserved */
} OracleRegistry;

/* Scorecard snapshot (report-only). */
typedef struct {
    char name[ACQUIRE_NAME_MAX];
    char family[ACQUIRE_NAME_MAX];
    size_t calls;
    size_t rejects;
    size_t abstains;
    size_t seals_produced;
    size_t unfit_closes;
    int retired;
    int leased;
    int attested;
    uint64_t behavior_digest;
    double unfit_rate; /* rejects+unfit style; unfit_closes/max(calls,1) */
} OracleScorecard;

typedef enum { GAP_NO_PLAN = 0, GAP_LOW_RELIABILITY = 1, GAP_HEALTH = 2 } GapKind;
typedef enum { GAP_OPEN = 0, GAP_DEFERRED = 1, GAP_CLOSED = 2 } GapStatus;

typedef struct {
    GapKind kind;
    GapStatus status;
    Port input_port;   /* zeroed for GAP_HEALTH (drain resolves from subject) */
    Port goal_port;    /* zeroed for GAP_HEALTH */
    char subject[ACQUIRE_NAME_MAX];      /* suspect unit; "" for NO_PLAN */
    char oracle[ACQUIRE_NAME_MAX];       /* matched oracle; "" until matched */
    char unit[ACQUIRE_NAME_MAX];         /* minted unit; "" until CLOSED */
    char defer_reason[ACQUIRE_REASON_MAX]; /* atom; "" unless DEFERRED */
    size_t times_hit;
    size_t attempts;
    /* Reconciliation state, PERSISTED (ledger v3): the gap lane marks a
       CLOSED record once its teacher descriptor and unit->descriptor
       relation are in the base. The ledger is always saved AFTER the base
       in the same checkpoint, so a persisted 1 never claims work the base
       does not hold; v1/v2 files load as 0 and re-reconcile idempotently. */
    int provenance_done;
    /* Acquisition recipe fingerprint in force when this record was DEFERRED
       for a recipe-dependent reason (PERSISTED, ledger v4; 0 = unknown /
       older ledger / never recipe-deferred). A drain reopens such a record
       when the CURRENT recipe fingerprint differs, so a student budget or
       certification-bar improvement retries old failures instead of leaving
       them stranded by the no-churn re-note policy. */
    uint64_t recipe_fp;
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
    double momentum;           /* heavy-ball SGD coefficient; 0 = plain SGD */
    /* Optional close hook (NULL = off): called once per gap the moment it
       reaches CLOSED, with the record's ledger index. The gap lane feeds
       its provenance-reconcile queue here so per-closure work stays O(1)
       instead of rescanning the ledger. */
    void (*on_close)(size_t gap_index, void *ctx);
    void *on_close_ctx;
    /* Optional attribution sink (NULL = off, the default). Called exactly
       once per terminated acquisition attempt, AFTER the attempt has fully
       settled and the recipe fingerprint has been stamped. REPORT-ONLY:
       acquire never reads anything back from this, so attaching a sink is a
       provable no-op — see the identical-drain gate in
       tests/test_attribution.c section 12. Use attrib_sink with an
       AttribLedger* as ctx. */
    void (*on_attempt)(const struct AttributionEvent *ev, void *ctx);
    void *on_attempt_ctx;
    /* Budgeted self-improve: stop the drain after this many successful
       CLOSES in one acquire_drain call. 0 = unlimited (legacy default).
       Examined/deferred gaps still count toward examined; only closed
       units consume the budget. CNET_LANE_MAX_CLOSURES overlays this. */
    size_t max_closures_per_drain;
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
    size_t last_domain_card; /* certified domain cardinality of the last closed
                                gap (0 when the domain is unbounded). Feeds the
                                attribution layer's anti-triviality column, so
                                a perfect admission rate over tiny or constant
                                domains does not read as excellence. */
    char last_unit_name[ACQUIRE_NAME_MAX];    /* unit minted by the last close */
    char last_defer_reason[ACQUIRE_REASON_MAX];
    size_t recipe_reopened;  /* recipe-stale deferrals reopened this drain */
    /* Efficiency / economics (filled best-effort each drain). */
    size_t total_oracle_calls;
    size_t total_oracle_rejects;
    size_t total_oracle_abstains;
    double train_wall_ms;          /* wall time spent in student train this drain */
    size_t student_bytes;          /* last sealed CNU size, 0 if unknown */
    /* Defer-reason histogram (atoms we care about for ops). */
    size_t defer_waiting_oracle;
    size_t defer_oracle_unfit;
    size_t defer_certify_failed;
    size_t defer_other;
} AcquireReport;

void acquire_ledger_init(AcquireLedger *l);

/* Stable digest over the AcquireConfig knobs that determine whether an
   acquisition can succeed (student capacity/training + mining/certification
   bars). Changes iff a recipe-relevant knob changes, so a DEFERRED record
   stamped with an older fingerprint is known to predate the current recipe.
   Deterministic for a given config; ignores non-recipe fields (base pointer,
   unit_dir, on_close hook, capture_limit). Returns nonzero. */
uint64_t acquire_recipe_fingerprint(const AcquireConfig *cfg);
void acquire_ledger_free(AcquireLedger *l);

/* Exact signature equality: family, field_width, field_count AND tag. */
int acquire_port_eq_public(Port a, Port b);

/* Register a named oracle for a (input_port -> output_port) link signature.
   Returns 0, or -1 (full, bad name atom, or duplicate name). */
int acquire_oracle_register(OracleRegistry *o, const char *name,
                            Port input_port, Port output_port,
                            CnetOracleFn fn, void *ctx);

/* Register an evidence-carrying Oracle. artifact_digest and contract_digest
   are mandatory; the other identity components may be zero when absent. */
int acquire_oracle_register_v2(OracleRegistry *o, const char *name,
                               Port input_port, Port output_port,
                               CnetOracleFnV2 fn,
                               CnetOracleValidateFn validator,
                               const CnetOracleIdentity *identity,
                               void *ctx);

/* Stable FNV-1a digest over every identity component (not pointers/names).
   Returns 0 for malformed identity. */
CNET_API uint64_t cnet_oracle_identity_digest(const CnetOracleIdentity *identity);

/* 1 if identity carries a non-zero full artifact SHA-256 and non-zero
   toolchain_digest (deploy attestation). Digests-only identities return 0. */
CNET_API int cnet_oracle_identity_is_attested(const CnetOracleIdentity *identity);

/* The one governed invocation path. Performs ABI checks, representation
   validation, optional semantic validation, first-class status accounting,
   and legacy aggregate counter updates. Returns the final status. */
CNET_API CnetOracleStatus cnet_oracle_invoke(
    OracleEntry *entry,
    const double *in, size_t in_count,
    double *out, size_t out_count,
    CnetOracleResult *result_out);

/* Governed batch path: fills results[i] for each row. If entry has no
   fn_batch_v2, falls back to serial cnet_oracle_invoke per row (identical
   accounting). Returns 0, or -1 on bad args. */
CNET_API int cnet_oracle_invoke_batch(
    OracleEntry *entry,
    const double *in, size_t in_stride,
    double *out, size_t out_stride,
    size_t count,
    CnetOracleResult *results);

/* Opt-in AFTER registering: declare the named oracle safe for concurrent
   fn calls from up to `width` threads. Only takes effect in OpenMP builds;
   mined exemplar tables and counters are identical to the serial path by
   construction (indexed slots, serial compaction). Returns 0, -1 unknown. */
int acquire_oracle_set_batch(OracleRegistry *o, const char *name,
                             CnetOracleBatchFn fn_batch, size_t batch_hint);
int acquire_oracle_set_batch_v2(OracleRegistry *o, const char *name,
                                CnetOracleBatchFnV2 fn_batch, size_t batch_hint);
int acquire_oracle_set_parallel(OracleRegistry *o, const char *name,
                                size_t width);

/* Teacher runtime policy (zero-init = hermetic: no attestation gate). */
void acquire_oracle_policy_defaults(OraclePolicy *p);
void acquire_oracle_policy_set(OracleRegistry *o, const OraclePolicy *p);

/* Lease a registered oracle for teaching. Returns lease gen (>0), or 0 on
   refusal (unknown/retired/unattested under policy). */
uint64_t acquire_oracle_bind(OracleRegistry *o, const char *name);
int acquire_oracle_unbind(OracleRegistry *o, const char *name, uint64_t lease);
int acquire_oracle_is_teachable(const OracleRegistry *o, const OracleEntry *e);

/* Scorecard fill; returns 0, or -1 unknown. */
int acquire_oracle_scorecard(const OracleRegistry *o, const char *name,
                             OracleScorecard *out);

/* Apply auto-retire policy to all entries; returns number newly retired. */
size_t acquire_oracle_apply_retire_policy(OracleRegistry *o);

/* Record a seal or unfit outcome against the named teacher (drain hooks). */
void acquire_oracle_note_seal(OracleRegistry *o, const char *name);
void acquire_oracle_note_unfit(OracleRegistry *o, const char *name);

/* v2 register with optional teacher family atom ("" ok). Same rules as
   acquire_oracle_register_v2 plus policy gates. */
int acquire_oracle_register_v2_family(OracleRegistry *o, const char *name,
                                      const char *family,
                                      Port input_port, Port output_port,
                                      CnetOracleFnV2 fn,
                                      CnetOracleValidateFn validator,
                                      const CnetOracleIdentity *identity,
                                      void *ctx);

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

/* Sidecar persistence ("CNET_GAPS 4"; older versions load with the missing
   columns defaulted: v1 -> unit "", v1/v2 -> provenance_done 0, v1/v2/v3 ->
   recipe_fp 0). Statuses + counters + signatures + minted unit names +
   reconciliation marks + recipe fingerprints; captured exemplar buffers are
   runtime-only. Save returns 0/-1. Load REPLACES the ledger's gap records on
   success (acquired-BTN ownership is untouched); missing/malformed file ->
   -1 with *l untouched. */
int acquire_ledger_save(const AcquireLedger *l, const char *path);
int acquire_ledger_load(AcquireLedger *l, const char *path);

#endif /* ACQUIRE_H */
