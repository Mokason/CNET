#include "../include/cnet_promote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cnet_promote_defaults(CnetPromoteInput *in) {
    if (!in) return;
    memset(in, 0, sizeof *in);
    in->min_net_gain = 1;
    in->max_regressions = -1;
    in->min_eval_delta = 0.0;
}

CnetPromoteDecision cnet_promote_decide(const CnetPromoteInput *in) {
    CnetPromoteDecision d;
    int net;
    d.allowed = 0;
    d.net_gain = 0;
    d.reason = "null";
    if (!in) return d;
    net = in->fixes - in->regressions;
    d.net_gain = net;
    if (in->max_regressions >= 0 && in->regressions > in->max_regressions) {
        d.reason = "too_many_regressions";
        return d;
    }
    if (net < in->min_net_gain) {
        d.reason = "net_gain_below_min";
        return d;
    }
    if (in->require_eval_delta && in->eval_delta < in->min_eval_delta) {
        d.reason = "eval_delta_below_min";
        return d;
    }
    d.allowed = 1;
    d.reason = "ok";
    return d;
}

int cnet_promote_read_eval_delta(const char *path, double *out_delta) {
    FILE *f;
    char buf[128];
    double v;
    if (!path || !out_delta) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (sscanf(buf, "delta %lf", &v) == 1 || sscanf(buf, "%lf", &v) == 1) {
        *out_delta = v;
        return 0;
    }
    return -1;
}
