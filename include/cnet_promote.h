/* Promote gate: adapter/unit may serve only if net gain + optional eval delta. */
#ifndef CNET_PROMOTE_H
#define CNET_PROMOTE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fixes;
    int regressions;
    int min_net_gain;       /* default 1 */
    int max_regressions;    /* default -1 ignore; else reject if over */
    double eval_delta;      /* from ghost-eval or external; 0 if unused */
    int require_eval_delta; /* 1 => eval_delta must be >= min_eval_delta */
    double min_eval_delta;  /* default 0 */
} CnetPromoteInput;

typedef struct {
    int allowed;
    int net_gain;
    const char *reason;
} CnetPromoteDecision;

void cnet_promote_defaults(CnetPromoteInput *in);
CnetPromoteDecision cnet_promote_decide(const CnetPromoteInput *in);

/* Read a one-line file "delta <float>" or bare float; returns 0 ok. */
int cnet_promote_read_eval_delta(const char *path, double *out_delta);

#ifdef __cplusplus
}
#endif
#endif
