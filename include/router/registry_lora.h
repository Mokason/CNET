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

/* Serve `name` on `input`: out = btn_forward(base)(input) + adapter delta.
   `out` holds output_count doubles. With no adapter attached this is exactly the
   base output (zero overhead). Returns 0 or -1. */
int registry_forward_with_lora(PrimitiveRegistry *reg, const char *name,
                               const double *input, double *out);

/* True if `name` currently has an adapter attached. */
int registry_has_lora(const PrimitiveRegistry *reg, const char *name);

/* Detach and free the adapter on `name` (no-op if none). */
void registry_lora_detach(PrimitiveRegistry *reg, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* CNET_REGISTRY_LORA_H */
