#ifndef CNET_REGISTRY_LORA_H
#define CNET_REGISTRY_LORA_H

/* Wire a rank-r cce_lora adapter into a real registry unit's retrain queue.
   Kept in its own translation unit so core registry TUs pull in no CCE
   dependency; only targets that want adapter-teaching link this + the CCE stack.
   Everything here is OFF the default path — it runs only when called. */

#include "../router.h"
#include "../cce/cce_lora.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   rank;                 /* adapter rank */
    float alpha;                /* delta scale (alpha/rank) */
    cce_lora_train_opts train;  /* epochs / lr / adam ... */
} registry_lora_opts;

registry_lora_opts registry_lora_defaults(void);

typedef struct {
    size_t pairs;        /* labeled queue pairs used                */
    double pre_mse;      /* base error vs teacher target (delta=0)  */
    double post_mse;     /* (base + adapter) error vs target        */
    int    in_dim;
    int    out_dim;
    int    rank;
    size_t params;       /* trained params (rank*(in+out))          */
    size_t dense_params; /* dense-equivalent (in*out)               */
} registry_lora_stats;

/* Train a rank-r adapter for `name` from its LABELED retrain queue: residual =
   teacher_target - btn_forward(base). Attaches the adapter to the entry
   (borrowed; free with registry_lora_detach). Returns 0, or -1 (unknown name /
   no labeled pairs / dim mismatch / OOM). Replaces any prior adapter on `name`. */
int registry_teach_lora(PrimitiveRegistry *reg, const char *name,
                        const registry_lora_opts *opt, registry_lora_stats *stats);

/* ---- certify-before-serve gate ------------------------------------------- */
/* Policy for accepting a trained adapter. In argmax_mode a sample is "correct"
   when argmax(output) == argmax(target) (classifiers/one-hot); otherwise
   correctness is per-sample squared error (an adapter that lowers a sample's
   error is a fix, one that raises it a regression). */
typedef struct {
    int argmax_mode;       /* 1 => argmax correctness; 0 => per-sample MSE */
    int max_regressions;   /* reject if right->wrong count exceeds this (<0 => ignore) */
    int min_net_gain;      /* require (fixes - regressions) >= this */
} registry_lora_cert_policy;

registry_lora_cert_policy registry_lora_cert_defaults(void);

typedef struct {
    int    passed;
    size_t n;
    int    base_correct, adapter_correct;   /* argmax_mode counts */
    int    fixes, regressions;
    double base_mse, adapter_mse;           /* always computed */
} registry_lora_cert_report;

/* Validate the adapter attached to `name` on a held-out set and set the entry's
   certified gate accordingly: only a PASS lets the executor hook serve it.
   inputs:[n*in], targets:[n*out]. Returns 1 (passed, gate set), 0 (failed, gate
   cleared), or -1 (no adapter / dim mismatch / error). */
int registry_certify_lora(PrimitiveRegistry *reg, const char *name,
                          const double *inputs, const double *targets, size_t n,
                          const registry_lora_cert_policy *policy,
                          registry_lora_cert_report *report);

/* Whether `name`'s adapter is attached AND certified (i.e. will be served). */
int registry_lora_is_certified(const PrimitiveRegistry *reg, const char *name);

/* Serve `name` on `input`: out = btn_forward(base)(input) + adapter delta.
   `out` holds output_count doubles. With no adapter attached this is exactly the
   base output (zero overhead). Returns 0 or -1. */
int registry_forward_with_lora(PrimitiveRegistry *reg, const char *name,
                               const double *input, double *out);

/* True if `name` currently has an adapter attached. */
int registry_has_lora(const PrimitiveRegistry *reg, const char *name);

/* Detach and free the adapter on `name` (no-op if none). */
void registry_lora_detach(PrimitiveRegistry *reg, const char *name);

/* Live-serving toggle. Enabling sets reg->lora_serving_enabled and installs the
   global executor hook (route.c/dag_full.c), so any primitive with an attached
   adapter is served as base + delta during route/DAG execution; disabling clears
   both, restoring the byte-identical frozen-base path. OFF by default.
   Prototype scope: one active serving registry at a time (last enable wins). */
void registry_lora_enable_serving(PrimitiveRegistry *reg);
void registry_lora_disable_serving(PrimitiveRegistry *reg);

/* ---- governed adapter action (orchestrator tick) ------------------------- */
/* Optional per-unit representative validation sampler. Fill up to `cap`
   (input, oracle-target) pairs for `unit` (dims given) into inputs[cap*in_dim]
   and targets[cap*out_dim]; return the count filled. Because it spans the input
   distribution (base-correct AND base-wrong cases), certifying on it lets the
   in-tick gate enforce a REGRESSION bound — unlike a fault-queue holdout, which
   is all base-wrong and can only measure fixes. Provided by the orchestrator
   (it owns the unit's oracle); registry_lora.c only calls the pointer. */
typedef size_t (*RegistryLoraValidateFn)(const char *unit, int in_dim, int out_dim,
                                         double *inputs, double *targets,
                                         size_t cap, void *ctx);

typedef struct {
    size_t min_faults;                 /* act only on units with >= this many labeled pairs */
    double holdout_frac;               /* fault-queue split used only when validate == NULL */
    registry_lora_opts teach;          /* rank / alpha / training */
    registry_lora_cert_policy cert;    /* accept/reject policy */
    /* Representative validation: when set, teach on the whole fault queue and
       certify on this sampler's output (regression-aware). NULL => certify on a
       fault-queue holdout (fixes only). */
    RegistryLoraValidateFn validate;
    void  *validate_ctx;
    size_t validate_cap;               /* samples to request (0 => 256) */
} registry_lora_tick_opts;
registry_lora_tick_opts registry_lora_tick_defaults(void);

typedef struct { size_t units_seen, taught, certified, rejected; } registry_lora_tick_report;

/* One governed pass over the registry: for each unit with >= min_faults labeled
   pairs, teach a rank-r adapter on a train split and certify on a held-out split
   (the gate). PASS opens the serve gate; FAIL detaches the candidate. Returns 0.
   This is the action the live orchestrator invokes each tick. */
int registry_lora_tick(PrimitiveRegistry *reg, const registry_lora_tick_opts *opt,
                       registry_lora_tick_report *report);

/* Ingest labeled (in,tgt) pairs from the unified fault JSONL into unit queues.
 * path NULL => CNET_FAULT_LOG. unit_filter NULL => all units present in reg.
 * Returns pairs ingested, or -1. */
int registry_lora_ingest_fault_bus(PrimitiveRegistry *reg, const char *path,
                                   const char *unit_filter);

/* Install/uninstall the adapter action in the live orchestrator: enables serving
   AND arms the tick hook (g_cnet_lora_tick_hook), so personal_ai_tick runs
   registry_lora_tick each pass. `opt` NULL => defaults. OFF until installed. */
void registry_lora_install_orchestrator(PrimitiveRegistry *reg, const registry_lora_tick_opts *opt);
void registry_lora_uninstall_orchestrator(PrimitiveRegistry *reg);

#ifdef __cplusplus
}
#endif

#endif /* CNET_REGISTRY_LORA_H */
