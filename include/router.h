#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>
#include <stdint.h>

#include "nn.h"
#include "cnet_export.h"
#include "specialist_kind.h"   /* SpecialistKind — durable live identity on RegistryEntry */

#define ROUTE_MAX_STEPS 8

/* forward for v2.2 field in PrimitiveRegistry */
typedef struct CircuitRankArtifact CircuitRankArtifact;

/* Attention planner modes (advisory routing layer over the hard DAG planner).
   OFF: identical to pre-attention behavior (zero change to plans or search).
   SHADOW: compute multi-head telemetry and top-k proposal for every plan,
           but use legacy reliability order + full exhaustive search only.
           No behavior change; pure measurement.
   ORDER_ONLY: reorder the candidate consideration using multi-head advisory
               ranking (primary) + reliability (secondary) + orig index tiebreak,
               but still consider the FULL candidate set (no pruning).
   PRUNE_WITH_FALLBACK: propose a top-K subset via attention_retrieve_top_k
                        (hard type-compat filter on output ports), attempt search
                        using only that restricted proposal; on miss, fall back to
                        a clean full exhaustive search. Sets first_pass / fallback
                        / miss flags for measurement. The final plan is always the
                        exhaustive one on miss (completeness preserved). */
typedef enum {
    CNET_ATTENTION_OFF = 0,
    CNET_ATTENTION_SHADOW = 1,
    CNET_ATTENTION_ORDER_ONLY = 2,
    CNET_ATTENTION_PRUNE_WITH_FALLBACK = 3
} CNETAttentionMode;

/* Streaming callback for live drafting view (narrative passes, executor handoffs).
   text: the fragment to emit (already canonicalized).
   tag: semantic tag or phase like "skeleton", "refine", "reflect", "token".
   is_final: whether this is committed (vs draft that may be revised). */
typedef void (*CNETStreamFn)(const char *text, const char *tag, int is_final);

/* Cost-aware planning policy. DEFAULT == legacy (rank candidates by learned
   reliability only). LOW == among EQUAL-reliability candidates, prefer the
   lower inference cost (MAC estimate); reliability is never traded for cost.
   registry_init zeroes it -> DEFAULT = byte-identical legacy behavior. */
typedef enum {
    CNET_POWER_DEFAULT = 0,
    CNET_POWER_LOW = 1
} PowerMode;

/* Primitive lifecycle state (the spine for self-healing, meltdown, shadow,
   and cost-aware behaviors). Numeric 0 == FUZZY keeps zero-init consistent.
   The only path INTO FROZEN is a passing btn_certify; RESET overrides FROZEN
   for planning eligibility without erasing the historical `certified` fact. */
typedef enum {
    PRIM_FUZZY = 0,        /* registered, uncertified, little/no evidence */
    PRIM_PROVISIONAL = 1,  /* uncertified but accruing positive evidence */
    PRIM_FROZEN = 2,       /* certified / law-proven; maximum trust */
    PRIM_RESET = 3         /* failed a runtime invariant; excluded from planning */
} PrimitiveState;

/* Per-primitive retraining queue (meltdown -> heal). A failing input is parked
   UNLABELED (we have the input + the bad raw output but no verified target).
   Labeling (teacher / external oracle) moves it to LABELED (input + verified
   target). registry_heal retrains on contract exemplars UNION the labeled set.
   Dims (input_count/output_count) are fixed from the primitive on first use. */
typedef struct {
    size_t input_count;
    size_t output_count;
    double *labeled_inputs;    /* labeled_count x input_count */
    double *labeled_targets;   /* labeled_count x output_count */
    size_t labeled_count;
    size_t labeled_cap;
    double *unlabeled_inputs;  /* unlabeled_count x input_count */
    double *unlabeled_raw;     /* unlabeled_count x output_count (the bad output) */
    size_t unlabeled_count;
    size_t unlabeled_cap;
} RetrainQueue;

/* 3C dual-track expansion: the lean teacher sub-plan a minted chunk replaced,
   stored names-only (the wiring is re-derived by the planner). Owns its names
   by value because RegistryEntry.name is borrowed. */
#define EXPAND_MAX_PRIMS 16
typedef struct {
    char   primitives[EXPAND_MAX_PRIMS][64];  /* teacher primitive names, in plan order */
    size_t primitive_count;
} ExpansionRecipe;

typedef struct {
    BinaryTransformNetwork *btn;  /* borrowed; the registry does not own it */
    const char *name;
    /* WHICH backend this node is — durable live identity, set once by
       specialist_admit (never persisted; trust replays separately). Defaults
       to SPECIALIST_KIND_BTN (0) for any entry added outside the specialist
       door, which is the correct kind for a native matrix primitive. */
    SpecialistKind kind;
    int certified;  /* set only by registry_add_certified */
    uint64_t cert_btn_digest; /* contract_btn_digest at certification time;
                                 registry_audit_certified demotes on mismatch */
    PrimitiveState state;  /* lifecycle state; FUZZY on add, FROZEN on certify */
    RetrainQueue *queue;  /* owned; NULL until the first fault is recorded */
    const char *shadow_of;  /* non-NULL -> a shadow candidate of this active name (borrowed) */
    /* 3C cost-aware dual track. The 3A cost label (transient in LibraryReport)
       is persisted here at mint; recipe != NULL iff this is an expandable chunk;
       expand_in_low == !compute_beneficial. All zero/NULL for a plain primitive. */
    size_t teacher_mac;
    size_t student_mac;
    int    compute_beneficial;
    ExpansionRecipe *recipe;  /* owned; NULL unless this is an expandable chunk */
    int    expand_in_low;
    /* Optional rank-r output adapter (cce_lora), borrowed like `btn`: the
       registry NULL-inits it and never owns it — the adapter layer
       (registry_lora.*) attaches, serves, and frees it. NULL => the base is
       served unchanged (zero overhead, default). Forward-declared so core
       registry TUs need no CCE dependency. */
    struct cce_lora *lora;
    /* Certify-before-serve gate: the executor hook applies `lora` only when this
       is nonzero. registry_teach_lora attaches a candidate with it 0;
       registry_certify_lora sets it from a held-out fixes-vs-regressions check.
       Zero-init default => an untested adapter never reaches production. */
    int lora_certified;
} RegistryEntry;

typedef struct {
    RegistryEntry *entries;
    size_t count;
    size_t capacity;
    /* Planning policy: nonzero -> route_plan/dag_plan consider ONLY
       certified entries, so a returned plan is certified end-to-end.
       registry_init zeroes it -- opt in before planning (mirrors the
       strict-execution precedent: zero-init = legacy behavior). */
    int require_certified;
    /* Nonzero disables the DAG planners' reachability pruning (the
       planning "memo"). Pruning only skips branches that provably
       contain no plan, so results are identical either way; the knob
       exists for the honest benchmark. Zero-init = pruning on. */
    int disable_plan_memo;
    /* Optional beam on primitive expansion in DAG/CIRCUIT search.
       0 => use the planner's default (8); otherwise only the top-N
       reliability-ranked candidates are considered per slot. */
    size_t dag_beam_limit;
    /* Attention advisory layer (v0.2+). Default OFF (zero behavior change).
       attention_prune_k: 0 means default (8); used only in PRUNE_WITH_FALLBACK
       to size the first-pass proposal. */
    CNETAttentionMode attention_mode;
    size_t attention_prune_k;

    /* v2.2: frozen rank artifact (ORDER_ONLY traversal bias only, read-only). */
    const CircuitRankArtifact *rank_artifact;
    /* Lifecycle policy: nonzero -> planners exclude PRIM_RESET entries
       (the spine's RESET-skip). registry_init zeroes it -- opt in, mirroring
       require_certified: zero-init = legacy behavior (state ignored). */
    int lifecycle_enabled;
    /* Cost-aware planning policy (see PowerMode). registry_init zeroes it
       -> DEFAULT = legacy. */
    PowerMode power_mode;
    /* 3C dual-track expansion opt-in. Nonzero AND power_mode == CNET_POWER_LOW
       -> the planner hides a compute-heavy expandable chunk whose recipe
       primitives are all present, so it rebuilds the obligation from them.
       registry_init zeroes it -> legacy (no expansion). */
    int expand_in_low_enabled;
    /* CNET-D steering influence (0.0 = legacy, 1.0 = full). 
       In DEFAULT: boosts "keep chunk" score by (1 + influence).
       In LOW: boosts expansion likelihood by (1 - influence) for keep-chunk decisions.
       Default 0.6 for this increment. */
    double cnet_d_influence;
    /* 3F: opt-in for text contract expansion (similar to expand_in_low) */
    int text_contract_expansion_enabled;

    /* Streaming hook for live view of agent generation (8A+ streaming).
       If non-NULL, executors and narrative contracts will call it for tagged outputs.
       Enables "live drafting" instead of only final output. */
    CNETStreamFn streamer;

    /* Name → entry index map (open addressing). values: 0 empty, else index+1.
       Power-of-two name_hash_cap; rebuilt on growth / remove_last.
       Opt out: CNET_REGISTRY_LINEAR=1 forces linear scan (debug). */
    size_t *name_hash;
    size_t name_hash_cap;

    /* Opt-in live-serving of attached cce_lora adapters. Zero-init = off (the
       executors serve the frozen base, byte-identical to legacy). Flipped by
       registry_lora_enable_serving(); the executors consult the hook below. */
    int lora_serving_enabled;
} PrimitiveRegistry;  /* note: rank_artifact defined later in this header */

/* Live-serving adapter hook (implemented in registry_lora.*). NULL => off, with
   zero overhead — the default. The executors call it right after btn_forward to
   add any attached low-rank delta to the primitive's raw output IN PLACE. The
   signature is CCE-free so core router TUs (route.c/dag_full.c) need no CCE
   dependency; only the opt-in adapter layer installs it. */
typedef void (*CnetLoraServeHook)(const BinaryTransformNetwork *btn,
                                  const double *input, double *raw, size_t out_len);
extern CnetLoraServeHook g_cnet_lora_serve_hook;

/* Governed adapter-maintenance hook: the orchestrator (personal_ai_tick) calls
   it once per tick to teach/certify/attach adapters from units' fault queues.
   NULL => off (default). Installed by registry_lora_install_orchestrator. CCE-
   free so personal_ai.c and the core stay free of any CCE dependency. */
typedef void (*CnetLoraTickHook)(PrimitiveRegistry *reg);
extern CnetLoraTickHook g_cnet_lora_tick_hook;

typedef struct {
    const BinaryTransformNetwork *steps[ROUTE_MAX_STEPS];
    const char *names[ROUTE_MAX_STEPS];
    size_t length;
    Port goal;  /* the goal port the plan was built for; set by route_plan */
    /* Execution policy: nonzero -> an out-of-domain RAW output aborts the
       run (-1) instead of being snapped; reliability evidence is recorded
       either way. route_plan resets this to 0 -- opt in after planning. */
    int strict;
} RoutePlan;

/* Legacy/test registry constructor: accepts unchecked fixture entries.
   Production authorities must use registry_init_production(). */
CNET_INTERNAL void registry_init(PrimitiveRegistry *reg);
CNET_API void registry_init_production(PrimitiveRegistry *reg);
CNET_API void registry_set_dag_beam_limit(PrimitiveRegistry *reg, size_t dag_beam_limit);

/* Low-level unchecked append for legacy/test fixtures and the certification
   implementation. It is deliberately outside the stable CNET_API surface;
   production admission goes through specialist_admit(). Borrows btn. */
CNET_INTERNAL int registry_add(PrimitiveRegistry *reg, BinaryTransformNetwork *btn, const char *name);
/* Persist every registered primitive as <name>.btn, <name>.contract, and
   <name>.stats inside dir. Returns 0 on success, -1 on failure. */
int registry_save(const PrimitiveRegistry *reg, const char *dir);

/* Frees the entries array only; the registered BinaryTransformNetworks are
   owned by the caller and are left untouched. */
void registry_free(PrimitiveRegistry *reg);

/* Drop the last registered entry (the borrowed btn and name are left to the
   caller). Used to roll back a just-appended primitive. Returns 0, or -1 if
   the registry is empty. */
int registry_remove_last(PrimitiveRegistry *reg);

/* Set the lifecycle state of the primitive named `name`. Returns 0, or -1 if
   reg is NULL or no entry has that name (first strcmp match wins). */
int registry_set_state(PrimitiveRegistry *reg, const char *name,
                       PrimitiveState state);

/* 3C: attach the cost truth + expansion recipe to the chunk named `name`.
   `prim_names`/`n` is the teacher sub-plan (names-only); n in [1,EXPAND_MAX_PRIMS]
   allocates a recipe, otherwise only the cost fields are set (no recipe).
   expand_in_low is set to !compute_beneficial. Replaces any prior recipe.
   Returns 0, or -1 if reg/name is NULL, the entry is absent, or allocation fails. */
int registry_set_expansion(PrimitiveRegistry *reg, const char *name,
                           const char *const *prim_names, size_t n,
                           size_t teacher_mac, size_t student_mac,
                           int compute_beneficial);

/* 3C: restore the recipe + cost truth for `name` from <dir>/<name>.expansion.
   Returns 0 when attached OR when the sidecar is absent (a no-op); -1 if
   reg/name/dir is NULL, the entry is absent, or the sidecar is malformed. */
int registry_load_expansion(PrimitiveRegistry *reg, const char *name,
                            const char *dir);

/* Restore every persisted runtime-only registry field from `dir`: global
   policy first, then each registered entry's optional expansion sidecar
   and CNET_STATS reliability counters (<name>.stats). Absent sidecars are
   a no-op; malformed state fails closed. */
int registry_restore_runtime_state(PrimitiveRegistry *reg, const char *dir);

/* Persist runtime-only registry state to `dir` without rewriting sealed CNU
   units: per-unit .stats (successes/failures) + registry.meta globals +
   expansion sidecars when present. Creates dir if needed. Returns 0/-1. */
int registry_persist_runtime_state(const PrimitiveRegistry *reg, const char *dir);

/* Promote every PRIM_FUZZY entry whose learned reliability >= promote_threshold
   AND whose recorded evidence (successes + failures) >= min_evidence to
   PRIM_PROVISIONAL. PROVISIONAL never reaches FROZEN here (that needs a proof);
   FROZEN and RESET entries are left untouched. */
void lifecycle_promote_provisional(PrimitiveRegistry *reg,
                                   double promote_threshold,
                                   size_t min_evidence);

/* Record a runtime fault: park (input, raw_output) in the named primitive's
   UNLABELED queue (lazily allocating it) and set the primitive PRIM_RESET.
   input has the primitive's input_count values; raw_output its output_count.
   Returns 0, or -1 (reg/name NULL, unknown name, or OOM). */
int registry_record_fault(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *raw_output);

/* Number of UNLABELED (awaiting-a-target) failures parked for `name`. 0 if no
   queue / unknown name. */
size_t registry_pending_labels(const PrimitiveRegistry *reg, const char *name);

/* External-oracle hook (target source C): supply a verified target for a parked
   UNLABELED failure whose input matches `input` (exact match per value). Moves
   it to the LABELED set. Returns 0, or -1 (no queue / no matching input / OOM).
   The label is TRUSTED by the caller -- registry_heal will retrain on it. */
int registry_supply_label(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *target);

/* Teacher labeling (target source B): for each UNLABELED failure of `name`, if
   any OTHER registry primitive with matching dims/ports produces an in-domain
   (cleanly canonicalizing) output for that input, adopt it as the target and
   move the failure to LABELED. Returns the number labeled (0 if none/unknown). */
size_t registry_label_via_teacher(PrimitiveRegistry *reg, const char *name);

/* Mark `name` as a shadow candidate of the active primitive `active_name` (both
   must exist). A shadow is excluded from planning when lifecycle_enabled (it
   never drives production) but accrues evidence via registry_run_shadows.
   active_name == NULL clears `name`'s shadow status. Returns 0, or -1. */
int registry_set_shadow(PrimitiveRegistry *reg, const char *name,
                        const char *active_name);

/* Run every shadow of `active_name` on `input` (in_len values), validating each
   RAW output against its output port and recording the reliability outcome --
   the same evidence the executors record, but for shadows on live inputs and
   WITHOUT affecting any production output. Skips shadows whose input arity /
   port shape does not match. Returns the number of shadows run. */
size_t registry_run_shadows(PrimitiveRegistry *reg, const char *active_name,
                            const double *input, size_t in_len);

/* Rough inference cost of a primitive: MAC estimate input_count*hidden_count +
   hidden_count*output_count. 0 for a NULL or empty net. Used by cost-aware
   planning (PowerMode). */
size_t btn_cost(const BinaryTransformNetwork *btn);

/* Find the chain of registered primitives (single-input, single-output)
   transforming a value of type input_port into one compatible with
   goal_port that MAXIMIZES the product of step reliabilities, within
   ROUTE_MAX_STEPS (exact hop-capped DP over port types). Reliabilities are
   strictly < 1, so with uniform evidence fewer hops always win (a fresh
   registry plans shortest-then-registry-order); strong evidence can justify
   a longer chain over a flaky shortcut. Ties keep the shorter chain, then
   registry order. Returns 0 with *out filled (length 0 if input already
   satisfies goal), or -1 if no chain exists. Reads only contracts and
   reliability stats -- no data, no training. */
CNET_API int route_plan(
    const PrimitiveRegistry *reg,
    Port input_port,
    Port goal_port,
    RoutePlan *out
);

/* Run a plan on an input vector. Every handoff -- including the external
   input -- is validated against the consuming port's domain and then
   canonicalized before the forward pass; each step's output is canonicalized
   against its output port. Each step also records a reliability outcome:
   whether its RAW output was in-domain before the snap (recording never
   changes the run's result). in_len must equal the first step's input total;
   out_cap must be >= the last step's output total. A length-0 plan validates
   and canonicalizes the input against the plan's goal port. Returns 0 on
   success, -1 on failure (including an ambiguous/out-of-domain handoff). */
CNET_API int route_execute(
    const RoutePlan *plan,
    const double *input,
    size_t in_len,
    double *output,
    size_t out_cap
);

/* A strict-execution fault: which registered primitive produced an out-of-domain
   RAW output, and at which step. Filled by route_execute_ex only on a strict
   abort; primitive == NULL means "no reroutable fault" (bad args / allocation
   failure / canonicalization error). */
typedef struct {
    const BinaryTransformNetwork *primitive;
    const char *name;       /* borrowed; matches a registry entry's name */
    size_t step_index;
} ExecFault;

/* Like route_execute, but on a strict abort reports the faulting primitive via
   *fault (fault may be NULL). route_execute is route_execute_ex(..., NULL). */
int route_execute_ex(
    const RoutePlan *plan,
    const double *input,
    size_t in_len,
    double *output,
    size_t out_cap,
    ExecFault *fault
);

/* Self-healing linear execution: plan input_port->goal_port, run strict; on a
   strict out-of-domain fault, mark the faulting primitive PRIM_RESET and re-plan
   around it, up to max_reroutes times. Requires reg->lifecycle_enabled for the
   RESET-skip to take effect on re-plan. Returns 0 on a clean run (possibly after
   rerouting), -1 if no route exists, the fault is not reroutable, or reroutes are
   exhausted. Mutates lifecycle state (sets RESET on faulting primitives). */
int route_execute_healing(
    PrimitiveRegistry *reg,
    Port input_port,
    Port goal_port,
    const double *input,
    size_t in_len,
    double *output,
    size_t out_cap,
    size_t max_reroutes
);

/* ---- DAG composition for multi-input primitives ---- */

#define DAG_MAX_SLOTS 8
#define DAG_MAX_DEPTH 8

/* A typed external input available to the planner. */
typedef struct {
    Port type;
    const double *values;
} DagSource;

typedef enum { DAG_SOURCE, DAG_PRIMITIVE } DagNodeKind;

typedef struct DagNode {
    DagNodeKind kind;
    int source_index;                  /* DAG_SOURCE: index into sources[] */
    const BinaryTransformNetwork *btn;  /* DAG_PRIMITIVE */
    const char *name;
    int output_index;  /* DAG_PRIMITIVE: which output port feeds the consumer */
    struct DagNode *children[DAG_MAX_SLOTS];  /* one per input slot */
    /* Which output port of children[k] feeds slot k. 0 means "use the
       child's output_index" -- the planner sets edges consistently with
       that rule, and zero-initialized hand-built trees keep their old
       meaning. Only meaningful for DAG_PRIMITIVE children. */
    int child_ports[DAG_MAX_SLOTS];
    size_t child_count;
} DagNode;

typedef struct {
    DagNode *root;
    /* Execution policy: nonzero -> an out-of-domain RAW output (ANY segment
       of a multi-output primitive) aborts the run instead of being snapped;
       reliability evidence is recorded either way. dag_plan resets this to
       0 -- opt in after planning. */
    int strict;
    /* Node ownership for PLANNER-built plans: dag_free releases this flat
       table, so a node shared by several consumers is freed exactly once.
       NULL for hand-built plans, whose nodes the caller owns (dag_free
       then falls back to the recursive tree free). */
    DagNode **owned;
    size_t owned_count;

    /* ---- Attention telemetry (advisory only; never affects plan validity) ----
       Populated on every dag_plan when registry.attention_mode >= SHADOW.
       All fields zeroed on entry. chosen_* describe the final root primitive
       of the returned plan (looked up by name against the attention proposal).
       For PRUNE_WITH_FALLBACK the first_pass / fallback / miss flags capture
       whether the restricted top-K proposal alone would have succeeded. */
    int attention_computed;
    size_t full_candidate_count;      /* candidates considered in full order */
    size_t attention_top_k_count;     /* size of the attention proposal (min(k, usable)) */
    int chosen_was_in_top_k;          /* 1 if the final chosen root primitive was inside the attention top-K proposal */
    int chosen_registry_idx;          /* registry index of chosen root, or -1 */
    int chosen_attention_rank;        /* 0-based rank within the top-K proposal (or -1) */

    /* PRUNE_WITH_FALLBACK specific (and forced-miss measurement) */
    int attention_pruned;             /* 1 if a restricted first-pass was attempted */
    size_t top_k_limit;               /* the k supplied for this call (from reg or default) */
    size_t pruned_candidate_count;    /* actual number fed to first-pass search */
    int fallback_used;                /* 1 if first pass produced no plan and we did full exhaustive */
    int first_pass_found_plan;        /* 1 if the attention-restricted pass succeeded */
    int exhaustive_fallback_found_plan; /* 1 if the full pass (fallback or only) succeeded */
    int attention_miss_would_have_failed_without_fallback; /* 1 if first==0 and exh==1 (true miss recovered) */

    /* Study-only comparison to OFF baseline (set by caller after running OFF+mode pair) */
    int same_structure_as_off;
    int same_validity_as_off;
} DagPlan;

#define CIRCUIT_MAX_ROOTS BTN_MAX_OUTPUT_PORTS

/* v0.7: Circuit attention telemetry (shadow-only for v0.7; observation, no influence on search, ordering, pruning or topology).
   Per-root rows capture the chosen producer (and its projection port) vs the attention proposal for that specific goal.
   This is visibility only; circuit search and sharing rules are untouched. */
typedef struct {
    size_t root_index;
    Port goal;
    size_t full_candidate_count;
    size_t attention_top_k_count;
    int chosen_primitive_registry_idx;   /* -1 for source roots or unresolved */
    int chosen_attention_rank;           /* 0-based in the goal's top-k proposal, or -1 */
    int chosen_was_in_attention_top_k;
    int projected_output_port;           /* the root_ports[g] value for this root */

    /* GRPO-style group-relative rank tuning (SHADOW_ONLY first).
       Reward source = strict verified execution only (from blackboard post-execute).
       advantage recorded for telemetry/study; never fed into ranking, pruning or plan selection in this slice. */
    size_t grpo_group_size;
    double grpo_verified_reward;   /* 1.0 if the chosen root producer contributed to successful strict exec */
    double grpo_group_mean_reward;
    double grpo_group_std;
    double grpo_advantage;         /* (r - mean) / (std + eps) within the considered group for this root */
} CircuitRootAttentionRow;

typedef struct {
    int attention_computed;
    size_t root_goal_count;
    size_t total_full_candidate_count;
    size_t total_attention_top_k_count;
    size_t chosen_roots_with_attention_rank;
    size_t chosen_roots_in_top_k;
    double chosen_in_top_k_rate;
    int circuit_attention_shadow_only;   /* 1 when non-OFF modes were treated as SHADOW (v0.7 policy) */

    /* v1.0 pair pruning telemetry */
    size_t candidate_pair_count_full;
    size_t candidate_pair_count_pruned;
    size_t pruned_pair_top_k;
    int chosen_pair_in_pruned_set;
    int fallback_used;
    int fallback_reason; /* 0=none, 1=topk_miss, 2=other */

    /* GRPO group-relative tuning (SHADOW_ONLY; derived from verified execution groups) */
    int grpo_computed;
    size_t grpo_roots_with_group;
    double avg_grpo_advantage_for_chosen;

    /* v2.3: Rank Artifact Evaluation / Search-Efficiency Closure
       Measure whether frozen rank prior improves ordering without changing behavior.
       All values row-derived from actual planning runs. */
    size_t nodes_expanded;
    size_t candidate_pairs_examined;
    int chosen_pair_rank;            /* 0-based rank of the final chosen (prim,port) pair(s) */
    int rank_improved;               /* 1 if artifact moved a verified pair to better rank */
    int artifact_influenced_ordering;
    int same_plan_as_off;
    int same_exec_as_off;
    int same_blackboard_as_off;
    int same_reliability_as_off;

    CircuitRootAttentionRow per_root[CIRCUIT_MAX_ROOTS];
} CircuitAttentionTelemetry;

/* v1.1: Circuit blackboard — typed execution trace only.
   Populated during dag_execute_circuit (if blackboard != NULL).
   Records what was actually executed and materialized, after validation + canonicalization.
   Does not affect planning, does not persist, does not relax contracts. */
typedef struct {
    int node_id;                    /* stable id for this execution (e.g. index in owned) */
    const char *primitive_name;     /* or "SOURCE" for source nodes */
    int output_port;                /* which output port of the node */
    Port port;                      /* the port signature for this segment */
    double *canonical;              /* owned copy of the *full* canonical output of the node (all ports) */
    size_t canonical_len;
    int consumer_count;             /* how many times this node was read as input by other nodes */
    int is_root;                    /* 1 if this node (or one of its ports) is a root of the circuit */
} CircuitBlackboardEntry;

typedef struct {
    CircuitBlackboardEntry *entries;
    size_t count;
    size_t capacity;
} CircuitBlackboard;

/* A multi-root plan: one coordinated structure satisfying several goals at
   once, sharing nodes where output ports allow. Sharing rule (port-disjoint
   fan-out): a node may have several consumers only if each reads a DISTINCT
   output port -- single-output primitives are never shared, so every
   single-goal plan over them is exactly what dag_plan always produced.
   v0.7: attention field is populated in SHADOW (and ORDER/PRUNE treated as shadow);
   plans themselves are identical to OFF. */
typedef struct {
    DagNode *roots[CIRCUIT_MAX_ROOTS];
    int root_ports[CIRCUIT_MAX_ROOTS]; /* projected output port per root */
    size_t root_count;
    DagNode **owned;                   /* always set by dag_plan_circuit */
    size_t owned_count;
    /* Same strict policy as DagPlan; dag_plan_circuit resets it to 0. */
    int strict;

    /* v0.7 attention telemetry (populated when reg.attention_mode >= SHADOW; does not affect planning) */
    CircuitAttentionTelemetry attention;
} CircuitPlan;

/* Build the AND-OR DAG producing goal_port from the registry and the given
   consumable sources (each source used at most once) that MAXIMIZES the
   product of primitive reliabilities over the whole plan (a primitive used
   twice counts twice; sources contribute 1.0). Complete branch-and-bound
   enumeration within DAG_MAX_DEPTH over source-to-slot assignment and
   primitive/output-port choice. Primitive expansion is beam-limited to the
   top-N reliability-ranked candidates per step, where N comes from
   reg->dag_beam_limit (0 means default 8). On small
   registries this preserves exactness, and larger depths scale better.
   Returns 0 with *out filled, or -1 if no plan exists. */
int dag_plan(
    const PrimitiveRegistry *reg,
    const DagSource *sources,
    size_t n_sources,
    Port goal_port,
    DagPlan *out
);

/* Free a plan's nodes (does not touch sources or registered BTNs). Planner
   plans free via the owned table (sharing-safe); hand-built plans without
   one free recursively as before. */
void dag_free(DagPlan *plan);

/* ---- Iterative scan builder (for sequential/recurrent domains) ---- */

/* Describes how a step primitive's slots map to state (recurrent) and data
   (consumed per step) ports. Used by dag_build_iterative_scan. */
typedef struct {
    size_t n_state_slots;                    /* how many input slots carry state */
    size_t state_in_slots[DAG_MAX_SLOTS];    /* which input slot indices are state */
    int    state_out_ports[DAG_MAX_SLOTS];   /* which output port of the step carries the next state */
    size_t n_data_slots;                     /* how many input slots consume items */
    size_t data_in_slots[DAG_MAX_SLOTS];     /* which input slot indices consume one item each step */
} StepWiring;

/* Build an iterative scan DagPlan for n_items steps.
   step_nodes_buf must hold at least n_items DagNode slots (caller manages).
   Returns 0 on success, -1 on invalid arguments.  The plan is hand-built
   (plan->owned == NULL); the caller owns all nodes including state_nodes and
   item_nodes. */
int dag_build_iterative_scan(
    BinaryTransformNetwork *step,
    const char *step_name,
    const StepWiring *wiring,
    DagNode *state_nodes,   /* n_state source nodes */
    size_t n_state,
    DagNode *item_nodes,    /* n_items source nodes */
    size_t n_items,
    DagNode *step_nodes_buf,/* output: n_items DagNode primitives */
    DagPlan *out            /* output: plan rooted at last step node */
);

/* Planner-driven wrapper: like dag_build_iterative_scan but selects the step
   primitive from the registry rather than taking it as a parameter. Returns 0
   on success, -1 if no suitable step is found or arguments are invalid. */
int dag_plan_iterative_scan(
    const PrimitiveRegistry *reg,
    DagNode *state_nodes,
    size_t n_state,
    DagNode *item_nodes,
    size_t n_items,
    const StepWiring *wiring,
    DagPlan *out,
    DagNode *step_nodes_buf
);

/* v0.6 advisory helper for studies/telemetry. Returns source_satisfiability for a
   primitive given the external sources (direct compatibility only for the exposed
   version; internal compute uses reach when available). 1.0 = all inputs directly
   satisfiable, 0.0 = at least one input has no matching source. */
double primitive_source_satisfiability(const BinaryTransformNetwork *p,
                                       const DagSource *sources, size_t n_sources);

/* Multi-root planning over shared structure: find the circuit producing
   every goals[g] (CIRCUIT_MAX_ROOTS at most) that maximizes the product of
   reliabilities over DISTINCT primitive executions -- a shared node counts
   once, which is the planner's reward for sharing. Every source must be
   referenced at least once (arity-truth, matching consolidation; also
   forbids goal-starved degenerate circuits). Roots are planned in goal
   order, so later goals may reuse earlier subtrees' nodes (port-disjoint).
   Uses the same beam-limited DAG expansion policy as dag_plan (controlled by
   reg->dag_beam_limit). Returns 0
   with *out filled, or -1 if no covering circuit exists. */
int dag_plan_circuit(
    const PrimitiveRegistry *reg,
    const DagSource *sources,
    size_t n_sources,
    const Port *goals,
    size_t n_goals,
    CircuitPlan *out
);

void circuit_free(CircuitPlan *plan);

/* Execute all roots in ONE run: every node evaluates once (one forward
   pass, one reliability outcome) no matter how many consumers read it.
   Root segments are written to output concatenated in goal order; out_cap
   must cover the total. If blackboard != NULL the blackboard is reset and
   populated with one CircuitBlackboardEntry per executed (node, output_port)
   after that port's output was validated + canonicalized for the memo.
   Blackboard is a pure typed execution trace/ledger: it does not influence
   planning, is not a memory, does not persist, and never relaxes contracts.
   Returns 0 on success, -1 on failure. */
int dag_execute_circuit(
    const CircuitPlan *plan,
    const DagSource *sources,
    size_t n_sources,
    double *output,
    size_t out_cap,
    CircuitBlackboard *blackboard  /* nullable: execution trace only */
);

/* Blackboard lifecycle (for v1.1 typed ledger use). init zeroes; free
   releases owned canonical buffers (names are borrowed). Safe on NULL. */
void circuit_blackboard_init(CircuitBlackboard *bb);
void circuit_blackboard_free(CircuitBlackboard *bb);

/* Backward-compat aliases for the generic blackboard API used in newer tests. */
typedef CircuitBlackboard CNETBlackboard;
#define blackboard_free  circuit_blackboard_free

/* Initialize a CNETBlackboard with an optional capacity hint (ignored; just zeroes). */
void blackboard_init(CNETBlackboard *bb, size_t capacity_hint);

/* Write a single typed value slot into the blackboard (for typed-ledger use).
   Stores a copy of values[0..field_width*field_count-1] at the next slot.
   Returns 0 on success, -1 on allocation failure. */
int blackboard_write(CNETBlackboard *bb, Port port, const double *values,
                     const char *name, double reliability);

/* Read slot `slot_index` out of the blackboard into out[0..out_len-1].
   Returns 0 on success, -1 if slot_index >= bb->count or out_len too small. */
int blackboard_read(const CNETBlackboard *bb, size_t slot_index,
                    double *out, size_t out_len);

/* Public API: retrieve up to k registry indices compatible with goal_port,
   in reliability order. Returns the number of indices written (0..k). */
size_t attention_top_k(const PrimitiveRegistry *reg, Port goal,
                       const DagSource *sources, size_t n_sources,
                       size_t k, size_t *out_indices);
/* Test-compat alias (public API; the internal static uses a different name) */
#define attention_retrieve_top_k attention_top_k

/* v1.2: Blackboard Debug / Consolidation Telemetry.
   Pure derived view over a filled CircuitBlackboard + the CircuitPlan + the
   final concatenated root outputs from a successful dag_execute_circuit.
   Never reads search telemetry, attention fields, or planner internals.
   summary = f(blackboard, plan, outputs) only. Used for debug, trace
   comparison (OFF vs ORDER vs PRUNE), and future consolidation diagnostics.
   Does not influence any planning. */
typedef struct {
    size_t executed_node_count;           /* distinct nodes that actually executed (unique node_ids in bb) */
    size_t primitive_node_count;          /* among executed, how many were PRIMITIVE (not SOURCE) */
    size_t source_node_count;             /* among executed, how many were SOURCE nodes */
    size_t output_entry_count;            /* total port entries written (bb->count) */
    size_t shared_node_count;             /* nodes with consumer_count > 1 OR >1 output port entry (fanout or internal sharing) */
    size_t root_output_count;             /* number of is_root entries (should == plan->root_count on success) */
    size_t max_consumer_count;            /* highest consumer_count seen across any entry */
    size_t invalid_entry_count;           /* entries that reached bb but were not contract-validated (0 for current ledger) */
    size_t contract_validated_entry_count;/* entries written after validate+canon (== output_entry_count on success) */
    int same_trace_as_off;                /* 1 if this trace's counts match those from an OFF baseline run (set by caller after compare) */
} CircuitTraceSummary;

/* Compute a pure ledger-derived trace summary. bb must have been populated by
   a prior successful dag_execute_circuit(..., bb). outputs + out_len are the
   values returned by that same call (used for root consistency checks if desired).
   On success fills *out and returns 0. Safe to call with NULL bb (zeros the summary).
   The same_trace_as_off field is left as 0; caller compares two summaries to set it. */
int circuit_blackboard_compute_trace_summary(
    const CircuitPlan *plan,
    const CircuitBlackboard *bb,
    const double *outputs,
    size_t out_len,
    CircuitTraceSummary *out
);

/* v1.3: Blackboard-backed Consolidation Report (explanatory only).
   Built from teacher blackboard (full circuit trace) and student (the
   consolidated chunk BTN). Summaries are derived via the v1.2 mechanism.
   compression_ratio, matches etc. are for human/debug/consolidation evidence.
   The safe_to_register flag is purely advisory; real registration still
   requires the existing consolidate verification + contract gate.
   Never influences planning or overrides hard checks. */
typedef struct {
    CircuitTraceSummary teacher_summary;
    CircuitTraceSummary student_summary;

    size_t teacher_node_count;
    size_t teacher_primitive_node_count;
    size_t teacher_output_entry_count;
    size_t student_node_count;

    size_t teacher_mac_estimate;
    size_t student_mac_estimate;

    double compression_ratio;
    int root_coverage_match;
    int output_exact_match;
    int contract_signature_match;
    int summary_valid;
    int consolidation_safe_to_register;

    /* carried from artifact for trend / audit (0 if not present in older artifacts) */
    size_t verified;
    size_t samples;

    /* names for registry matching (v1.7) */
    char teacher_name[64];
    char student_name[64];
} CircuitConsolidationReport;

/* Build a consolidation report from the teacher's executed blackboard
   (and plan/outputs for context) and the student chunk (BTN + optional
   ConsolidateReport from consolidate_circuit for verified info).
   Returns 0 on success (report filled), -1 on bad args.
   If student_bb provided it is used for student_summary; otherwise a
   1-node synthetic summary is derived from the student_btn ports.
   MAC estimates use per-BTN (in*h + h*out) summed for teacher nodes. */
int circuit_consolidation_report(
    const CircuitPlan *teacher_plan,
    const CircuitBlackboard *teacher_bb,
    const double *teacher_outputs,
    size_t teacher_out_len,
    const BinaryTransformNetwork *student_btn,
    size_t student_verified,   /* from ConsolidateReport.verified if available (0 = unknown) */
    size_t student_samples,    /* from ConsolidateReport.samples */
    CircuitConsolidationReport *report
);

/* v1.5: read-only artifact replay / comparison (observability only, zero authority).
   load returns 0 on success, <0 on open/parse error (malformed/missing field).
   comparison never calls any registration, certify, or planner code.
   comparison output cannot cause registration or mutation. */
int circuit_load_consolidation_artifact(const char *path, CircuitConsolidationReport *out);

void circuit_print_consolidation_artifact_comparison(
    const CircuitConsolidationReport *a, const char *name_a,
    const CircuitConsolidationReport *b, const char *name_b
);

/* Helper to write the standard artifact JSON (used by demo and tests for v1.5 replay fixtures).
   Returns 0 on success. */
int circuit_write_consolidation_artifact(
    const char *path,
    const CircuitConsolidationReport *report,
    const char *teacher_name,
    const char *student_name,
    size_t verified,
    size_t samples
);

/* v1.6: Artifact Trend Summary (read-only audit table over N artifacts).
   Expects a NULL-terminated or length-n list of paths already sorted
   deterministically by filename (caller responsibility, e.g. lexical qsort).
   Prints one row per loadable artifact + "same_as_previous" vs the prior row.
   On mismatch, shows deltas for compression/MAC etc. vs previous.
   Malformed artifacts are reported as bad input (not trusted, row skipped or marked).
   No side effects, no planner, no registration. */
void circuit_print_artifact_trend_summary(const char * const *paths, size_t n);

/* v1.7: Consolidation Registry Report (read-only, registry-facing audit).
   Takes the live PrimitiveRegistry (after normal registration/certification)
   and a lexically-sorted list of artifact paths (caller collects + sorts).
   Prints rows for certified primitives, indicating presence of matching
   artifact by student_name == primitive name match (evidence only, not trust).
   Also reports orphan artifacts and bad inputs as observations.
   Never alters registry, never calls certify/register/planner/consolidate.
   "artifact present" is never phrased as "certified". */
void circuit_print_consolidation_registry_report(
    const PrimitiveRegistry *reg,
    const char * const *artifact_paths,
    size_t artifact_n
);

/* v1.8: Registry Audit Snapshot / Diff (read-only evidence persistence + replay).
   Snapshot is purely observational: derived from live reg + sorted artifact list.
   Never read by any gate. Writing is non-fatal and disableable.
   Comparison is pure (no weights, no planner, no registration). */

#define MAX_REGISTRY_REPORT_ROWS 128
#define MAX_REGISTRY_REPORT_ORPHANS 128
#define MAX_REGISTRY_REPORT_BAD 128
#define MAX_REGISTRY_REPORT_DUPLICATES 128

typedef struct {
    char primitive_name[64];
    int registered;
    int certified;

    int artifact_present;
    int has_matching_artifact;
    char artifact_path[256];       /* lex first selected */

    int verified;
    int samples;
    int safe_advisory_only;
    char match_status[32];         /* "name" or "missing" */

    size_t matching_artifact_count;
    int duplicate_matching_artifacts;
    int conflicting_matching_artifacts;
    char evidence_selection[32];   /* "none" | "unique" | "lex_first_advisory_only" */
} CircuitRegistryReportRow;

typedef struct {
    char artifact_path[256];
    char student_name[64];
    int safe_advisory_only;
} CircuitRegistryArtifactObservation;

typedef struct {
    char primitive_name[64];
    char artifact_path[256];
    char selected_artifact_path[256];
    int verified;
    int samples;
    int safe_advisory_only;
    int conflicts_with_selected;
} CircuitRegistryArtifactDuplicate;

typedef struct {
    char version[32]; /* "CNET_REGISTRY_REPORT 1" or "2" */
    size_t row_count;
    CircuitRegistryReportRow rows[MAX_REGISTRY_REPORT_ROWS];

    size_t duplicate_count;
    CircuitRegistryArtifactDuplicate duplicates[MAX_REGISTRY_REPORT_DUPLICATES];

    size_t orphan_count;
    CircuitRegistryArtifactObservation orphans[MAX_REGISTRY_REPORT_ORPHANS];

    size_t bad_input_count;
    char bad_inputs[MAX_REGISTRY_REPORT_BAD][256];
} CircuitRegistryReportSnapshot;

int circuit_build_consolidation_registry_snapshot(
    const PrimitiveRegistry *reg,
    const char * const *artifact_paths,
    size_t artifact_n,
    CircuitRegistryReportSnapshot *out);

int circuit_write_consolidation_registry_snapshot(
    const char *path,
    const CircuitRegistryReportSnapshot *snap);

int circuit_load_consolidation_registry_snapshot(
    const char *path,
    CircuitRegistryReportSnapshot *out);

void circuit_print_consolidation_registry_snapshot_diff(
    const CircuitRegistryReportSnapshot *a,
    const CircuitRegistryReportSnapshot *b);

/* v1.9: Registry Snapshot Trend Summary (read-only audit history over multiple snapshots).
   Pure replay over lex-sorted snapshot paths. Computes counts, same_as_previous,
   and deltas vs previous successful snapshot. Supports v1 and v2 snapshots
   (v1 duplicate/conflict fields default to zero). Malformed snapshots reported
   as bad input and ignored. No authority, no planner, no registration, no mutation. */

#define MAX_REGISTRY_TREND_ROWS 128
#define MAX_REGISTRY_TREND_BAD 128

typedef struct {
    char snapshot_path[256];
    char version[32];

    size_t row_count;
    size_t certified_count;

    size_t artifact_present_count;
    size_t matching_artifact_count;
    size_t missing_artifact_count;

    size_t duplicate_row_count;
    size_t conflicting_row_count;
    size_t duplicate_detail_count;

    size_t orphan_count;
    size_t bad_input_count;

    int same_as_previous;

    int row_delta;
    int matching_artifact_delta;
    int missing_artifact_delta;
    int duplicate_row_delta;
    int conflicting_row_delta;
    int orphan_delta;
    int bad_input_delta;
} CircuitRegistryTrendRow;

typedef struct {
    size_t row_count;
    CircuitRegistryTrendRow rows[MAX_REGISTRY_TREND_ROWS];

    size_t bad_snapshot_count;
    char bad_snapshots[MAX_REGISTRY_TREND_BAD][256];
} CircuitRegistryTrendSummary;

int circuit_build_registry_snapshot_trend(
    const char * const *snapshot_paths,
    size_t snapshot_n,
    CircuitRegistryTrendSummary *out);

void circuit_print_registry_snapshot_trend(
    const CircuitRegistryTrendSummary *trend);

/* v2.0: Circuit Memory Hints (SHADOW_ONLY / advisory telemetry only).
   Hints are persisted typed execution evidence derived strictly from a
   successful strict-mode blackboard ledger after normal planning + execution.
   They are NEVER authority: no registration, no certification, no ordering,
   no pruning, no injection, no reliability mutation, no planner effect in v2.0.
   memory hint = prior observed execution evidence only. */

#define MAX_CIRCUIT_MEMORY_HINTS 128
#define MAX_CIRCUIT_MEMORY_BAD   32
#define MAX_CIRCUIT_MEMORY_SHADOW_ROWS 32

typedef struct CircuitMemoryHint {
    char task_key[192];          /* deterministic typed source+goal signature */

    size_t source_count;
    size_t root_count;
    size_t root_index;

    char primitive_name[64];
    int output_port;

    Port output_sig;             /* observed output port signature (from executed port) */

    char plan_fingerprint[64];   /* deterministic text/FNV or strict-exec tag; not trust */

    size_t observation_count;

    int produced_by_strict_execution;
    int advisory_only;
} CircuitMemoryHint;

typedef struct CircuitMemoryHintStore {
    char version[32];            /* CNET_CIRCUIT_MEMORY_HINTS 1 */

    size_t hint_count;
    CircuitMemoryHint hints[MAX_CIRCUIT_MEMORY_HINTS];

    size_t bad_hint_count;
    char bad_hints[MAX_CIRCUIT_MEMORY_BAD][256];
} CircuitMemoryHintStore;

typedef struct CircuitMemoryShadowRow {
    size_t root_index;
    char task_key[192];

    int hint_present;

    char hinted_primitive[64];
    int hinted_output_port;

    int hint_valid_for_registry;
    int hint_valid_for_projection;
    int hint_valid_for_certified_mode;

    int hinted_pair_in_candidate_set;
    int hinted_pair_matches_final_plan;

    char ignore_reason[64];
    /*
       none
       malformed
       no_hint
       not_registered
       uncertified
       primitive_not_in_candidates
       output_port_missing
       port_mismatch
       tag_mismatch
       source_goal_mismatch
    */

    int influence_on_planner;    /* always 0 in v2.0 */
} CircuitMemoryShadowRow;

typedef struct CircuitMemoryShadowReport {
    size_t row_count;
    CircuitMemoryShadowRow rows[MAX_CIRCUIT_MEMORY_SHADOW_ROWS];

    size_t stale_hint_count;
    size_t malformed_hint_count;

    int influence_on_planner;    /* always 0 in v2.0 */
} CircuitMemoryShadowReport;

/* Public helpers (v2.0): small, side-effect free except for write (which is non-fatal).
   from_blackboard is called only after strict success + bb materialization.
   shadow_report is computed after normal planning/execution for telemetry only.
   write/load are pure JSON roundtrips (forged extra fields ignored). */

int circuit_memory_hints_from_blackboard(
    const CircuitPlan *plan,
    const CircuitBlackboard *bb,
    const Port *sources,
    size_t source_n,
    const Port *goals,
    size_t goal_n,
    CircuitMemoryHintStore *out);

int circuit_memory_write_hints(
    const char *path,
    const CircuitMemoryHintStore *store);

int circuit_memory_load_hints(
    const char *path,
    CircuitMemoryHintStore *out);

int circuit_memory_shadow_report(
    const PrimitiveRegistry *reg,
    const Port *sources,
    size_t source_n,
    const Port *goals,
    size_t goal_n,
    const CircuitPlan *final_plan,
    const CircuitMemoryHintStore *store,
    CircuitMemoryShadowReport *out);

void circuit_memory_print_shadow_report(
    const CircuitMemoryShadowReport *report);

/* Pure-inspection report printer for --memory-hints-report (early exit path). */
void circuit_memory_print_hints_report(
    const char *path,
    const CircuitMemoryHintStore *store);

/* GRPO shadow-only rank tuning helper (SHADOW_ONLY slice).
   Call *after* successful strict dag_execute_circuit with populated blackboard.
   Fills per-root grpo_* fields in plan->attention using verified execution
   as the sole reward source. Does not alter plan, search, or any weights. */
void circuit_grpo_fill_from_blackboard(
    CircuitPlan *plan,
    const CircuitBlackboard *bb,
    const PrimitiveRegistry *reg
);

/* v2.1: Typed Engram Cache — SHADOW_ONLY execution evidence cache.
   Accumulates deterministic, typed, replayable facts from strict verified
   blackboard runs. Pure observability artifact. Never influences planning,
   ranking, pruning, certification, registration, reliability, contracts,
   weights, or search decisions. */

#define MAX_ENGRAM_ENTRIES 256
#define MAX_ENGRAM_LOOKUP_ROWS 32

typedef struct CircuitEngramEntry {
    char task_key[192];
    size_t root_index;
    Port root_goal;                  /* signature of the root goal */

    char producer_name[64];
    int producer_output_port;

    char trace_digest[128];          /* deterministic digest of key bb facts */

    /* trace summary facts (row-derived) */
    size_t executed_node_count;
    size_t output_entry_count;
    size_t shared_node_count;
    size_t root_output_count;

    /* optional GRPO shadow values captured at write time */
    size_t grpo_group_size;
    double grpo_verified_reward;
    double grpo_group_mean_reward;
    double grpo_group_std;
    double grpo_advantage;

    int strict_verified;
    char source[32];                 /* "blackboard" */
    int shadow_only;                 /* always 1 for v2.1 */
} CircuitEngramEntry;

typedef struct CircuitEngramStore {
    char version[32];                /* "CNET_CIRCUIT_ENGRAM 1" */
    size_t entry_count;
    CircuitEngramEntry entries[MAX_ENGRAM_ENTRIES];
} CircuitEngramStore;

typedef struct CircuitEngramLookupRow {
    size_t root_index;
    char task_key[192];

    int engram_present;
    char engram_producer[64];
    int engram_output_port;

    int engram_task_key_match;
    int engram_projection_match;
    int engram_producer_exists;
    int engram_certified_mode_valid;
    int engram_matches_final;
    int engram_trace_digest_match;

    int influence_on_planner;        /* always 0 */

    char ignore_reason[64];          /* none, no_engram, task_mismatch, projection_mismatch, not_registered, uncertified, ... */
} CircuitEngramLookupRow;

typedef struct CircuitEngramLookupReport {
    size_t row_count;
    CircuitEngramLookupRow rows[MAX_ENGRAM_LOOKUP_ROWS];

    size_t missing_count;
    size_t mismatch_count;

    int influence_on_planner;        /* always 0 */
} CircuitEngramLookupReport;

/* Public engram helpers (v2.1, SHADOW_ONLY). */
void circuit_engram_store_init(CircuitEngramStore *store);
void circuit_engram_store_free(CircuitEngramStore *store);

int circuit_engram_from_blackboard(
    const CircuitPlan *plan,
    const CircuitBlackboard *bb,
    const CircuitTraceSummary *summary,
    CircuitEngramStore *out);

int circuit_engram_write_json(const char *path, const CircuitEngramStore *store);
int circuit_engram_load_json(const char *path, CircuitEngramStore *out);

int circuit_engram_lookup_shadow(
    const CircuitEngramStore *store,
    const PrimitiveRegistry *reg,
    const Port *sources,
    size_t source_n,
    const Port *goals,
    size_t goal_n,
    const CircuitPlan *final_plan,
    CircuitEngramLookupReport *out);

void circuit_engram_print_report(const CircuitEngramLookupReport *report);

/* v2.2: Frozen GRPO Rank Artifact, ORDER_ONLY.
   Built from engrams + GRPO values. Loaded as immutable table.
   Used only for ORDER_ONLY full candidate-pair traversal order bias.
   No authority over existence, eligibility, pruning, cert, registry, etc. */

#define MAX_RANK_ARTIFACT_ROWS 256

typedef struct CircuitRankArtifactRow {
    char artifact_version[32];
    char task_key[192];
    Port root_goal;                    /* root goal signature */
    char producer_name[64];
    int producer_output_port;
    Port producer_output_sig;          /* observed port sig */

    size_t seen_count;
    size_t strict_verified_count;
    double reward_sum;
    double advantage_sum;

    double mean_reward;
    double mean_advantage;
    double rank_prior;

    char source_engram_digest[64];
    char source_trace_digest[64];

    int trained_from_strict_verified;
    int order_only;
    int no_prune_authority;
    int no_cert_authority;
    int no_registry_authority;
} CircuitRankArtifactRow;

typedef struct CircuitRankArtifact {
    char artifact_digest[64];
    char created_from_engram_digest[64];
    size_t row_count;
    char feature_schema_version[32];
    double clamp_min;
    double clamp_max;
    double confidence_k;
    int strict_verified_only;
    int frozen;
    int order_only;
    CircuitRankArtifactRow rows[MAX_RANK_ARTIFACT_ROWS];
} CircuitRankArtifact;

typedef struct CircuitRankArtifactLookup {
    int found;
    double rank_prior;
    int order_only;
    int no_prune_authority;
    int no_cert_authority;
    int no_registry_authority;
    char ignore_reason[64];
} CircuitRankArtifactLookup;

typedef struct CircuitRankArtifactReport {
    size_t row_count;
    /* simplified rows for report */
    char artifact_digest[64];
    int order_only;
    int influence_on_planner;  /* 0 for shadow, order_only for order */
    int prune_authority;       /* 0 */
    int registry_authority;    /* 0 */
    int cert_authority;        /* 0 */
} CircuitRankArtifactReport;

/* Public helpers for v2.2 */
void circuit_rank_artifact_init(CircuitRankArtifact *a);
void circuit_rank_artifact_free(CircuitRankArtifact *a);

int circuit_rank_artifact_build_from_engrams(
    const CircuitEngramStore *engrams,
    CircuitRankArtifact *out);

int circuit_rank_artifact_write_json(
    const char *path,
    const CircuitRankArtifact *artifact);

int circuit_rank_artifact_load_json(
    const char *path,
    CircuitRankArtifact *out);

int circuit_rank_artifact_lookup(
    const CircuitRankArtifact *artifact,
    const char *task_key,
    const Port *root_goal,
    const char *producer_name,
    int producer_output_port,
    const Port *producer_output_sig,
    CircuitRankArtifactLookup *out);

void circuit_rank_artifact_print_report(
    const CircuitRankArtifactReport *report);

/* Evaluate a plan: assemble each primitive's multi-slot input from its
   children, validating each handoff against the consuming slot's domain and
   then canonicalizing it (an ambiguous handoff is an error, not rounded
   away); forward; canonicalize the output. A bare source root is likewise
   validated and canonicalized against the source's type. Each primitive
   records a reliability outcome: whether its RAW output was in-domain before
   the snap (recording never changes the run's result). out_cap must be >=
   the root's output total. Returns 0 on success, -1 on failure. */
CNET_API int dag_execute(
    const DagPlan *plan,
    const DagSource *sources,
    size_t n_sources,
    double *output,
    size_t out_cap
);

#endif
