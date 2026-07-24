/* registry_lily — host a cce_lily deep-base adapter under registry_lora's
   certify gate (same policy/report), taught by serve-in-the-loop. See header. */

#include "../../include/router/registry_lily.h"
#include "../../include/cce/cce_ds_runtime.h"

#include <stdlib.h>
#include <string.h>

static void fwd_final(cce_ds_host *h, const float *x, float *out) {
    cce_ds_host_reset(h);
    memcpy(h->residual, x, (size_t)h->d_model * sizeof(float));
    cce_ds_host_forward_token(h);
    memcpy(out, h->residual, (size_t)h->d_model * sizeof(float));
}

int registry_lily_certify(cce_lily *ly, struct cce_ds_host *h,
                          const float *inputs, const float *teacher_out, size_t n,
                          const registry_lora_cert_policy *pol,
                          registry_lora_cert_report *rep) {
    if (!ly || !h || !inputs || !teacher_out || !pol || n == 0) return -1;
    const int d = h->d_model;
    float *base = malloc((size_t)d * sizeof(float));
    float *adap = malloc((size_t)d * sizeof(float));
    if (!base || !adap) { free(base); free(adap); return -1; }

    int fixes = 0, regress = 0;
    double bse = 0.0, ase = 0.0;
    for (size_t s = 0; s < n; s++) {
        const float *x = inputs + s * (size_t)d;
        const float *t = teacher_out + s * (size_t)d;
        cce_lily_uninstall_serving();              /* frozen base */
        fwd_final(h, x, base);
        cce_lily_install_serving(ly);              /* base + adapter */
        fwd_final(h, x, adap);
        cce_lily_uninstall_serving();
        double be = 0, ae = 0;
        for (int o = 0; o < d; o++) { double db = base[o] - t[o], da = adap[o] - t[o]; be += db * db; ae += da * da; }
        bse += be; ase += ae;
        if (ae < be - 1e-12) fixes++;              /* adapter lowered this sample's error */
        else if (ae > be + 1e-12) regress++;       /* adapter raised it */
    }
    free(base); free(adap);

    int passed = 1;
    if (pol->max_regressions >= 0 && regress > pol->max_regressions) passed = 0;
    if ((fixes - regress) < pol->min_net_gain) passed = 0;

    if (rep) {
        rep->passed = passed; rep->n = n;
        rep->base_correct = 0; rep->adapter_correct = 0;   /* MSE-mode: no argmax notion */
        rep->fixes = fixes; rep->regressions = regress;
        rep->base_mse = bse / (double)(n * (size_t)d);
        rep->adapter_mse = ase / (double)(n * (size_t)d);
    }
    return passed;
}

int registry_lily_teach_certify(cce_lily *ly, struct cce_ds_host *h,
                                const float *train_inputs, const float *teacher_res, size_t ntr,
                                int serve_iters, const cce_lily_train_opts *inner,
                                const float *val_inputs, const float *val_teacher_out, size_t nval,
                                const registry_lora_cert_policy *pol,
                                registry_lora_cert_report *rep) {
    if (!ly || !h) return -1;
    /* teach: serve-in-the-loop (no autograd through the frozen base) */
    (void)cce_lily_train_serve_loop(h, train_inputs, teacher_res, ntr, ly, serve_iters, inner);
    /* certify: the same gate cce_lora uses, on the served deep forward */
    return registry_lily_certify(ly, h, val_inputs, val_teacher_out, nval, pol, rep);
}

/* Query-token final output: prefill T-1 context tokens (in `prefill` compute if
   given, else the host's current query compute), then forward the query token in
   the host's current (cheap student) compute. The adapter `ly`, if given, serves
   ONLY at the query token — corrected query, which matches where
   cce_lily_collect_served_ctx trains it. ly=NULL is the frozen student. */
static void fwd_final_ctx(cce_ds_host *h, const cce_lily_compute *prefill,
                          const float *seq, int T, const cce_lily *ly, float *out) {
    const int d = h->d_model;
    cce_lily_compute q;
    q.dsa_enable = h->dsa_enable; q.dsa_fraction = h->dsa_fraction; q.n_expert_used = h->map.hp.n_expert_used;
    cce_ds_host_reset(h);
    cce_lily_uninstall_serving();
    if (prefill) { h->dsa_enable = prefill->dsa_enable; h->dsa_fraction = prefill->dsa_fraction; h->map.hp.n_expert_used = prefill->n_expert_used; }
    for (int t = 0; t < T - 1; t++) {
        memcpy(h->residual, seq + (size_t)t * d, (size_t)d * sizeof(float));
        cce_ds_host_forward_token(h);
    }
    if (prefill) { h->dsa_enable = q.dsa_enable; h->dsa_fraction = q.dsa_fraction; h->map.hp.n_expert_used = q.n_expert_used; }
    if (ly) cce_lily_install_serving(ly);
    memcpy(h->residual, seq + (size_t)(T - 1) * d, (size_t)d * sizeof(float));
    cce_ds_host_forward_token(h);
    cce_lily_uninstall_serving();
    memcpy(out, h->residual, (size_t)d * sizeof(float));
}

int registry_lily_certify_ctx(cce_lily *ly, struct cce_ds_host *h,
                              const cce_lily_compute *prefill,
                              const float *seqs, const float *teacher_out,
                              size_t n, int T,
                              const registry_lora_cert_policy *pol,
                              registry_lora_cert_report *rep) {
    if (!ly || !h || !seqs || !teacher_out || !pol || n == 0 || T < 1) return -1;
    const int d = h->d_model;
    float *base = malloc((size_t)d * sizeof(float));
    float *adap = malloc((size_t)d * sizeof(float));
    if (!base || !adap) { free(base); free(adap); return -1; }

    int fixes = 0, regress = 0;
    double bse = 0.0, ase = 0.0;
    for (size_t s = 0; s < n; s++) {
        const float *seq = seqs + s * (size_t)T * d;
        const float *t = teacher_out + s * (size_t)d;
        fwd_final_ctx(h, prefill, seq, T, NULL, base);   /* frozen cheap student */
        fwd_final_ctx(h, prefill, seq, T, ly, adap);     /* prefill + adapter at the query token */
        double be = 0, ae = 0;
        for (int o = 0; o < d; o++) { double db = base[o] - t[o], da = adap[o] - t[o]; be += db * db; ae += da * da; }
        bse += be; ase += ae;
        if (ae < be - 1e-12) fixes++;
        else if (ae > be + 1e-12) regress++;
    }
    free(base); free(adap);

    int passed = 1;
    if (pol->max_regressions >= 0 && regress > pol->max_regressions) passed = 0;
    if ((fixes - regress) < pol->min_net_gain) passed = 0;

    if (rep) {
        rep->passed = passed; rep->n = n;
        rep->base_correct = 0; rep->adapter_correct = 0;
        rep->fixes = fixes; rep->regressions = regress;
        rep->base_mse = bse / (double)(n * (size_t)d);
        rep->adapter_mse = ase / (double)(n * (size_t)d);
    }
    return passed;
}

int registry_lily_teach_certify_ctx(cce_lily *ly, struct cce_ds_host *h,
                                    const cce_lily_compute *prefill,
                                    const float *train_seqs, const float *teacher_res, size_t ntr, int T,
                                    int serve_iters, const cce_lily_train_opts *inner,
                                    const float *val_seqs, const float *val_teacher_out, size_t nval,
                                    const registry_lora_cert_policy *pol,
                                    registry_lora_cert_report *rep) {
    if (!ly || !h) return -1;
    (void)cce_lily_train_serve_loop_ctx(h, prefill, train_seqs, teacher_res, ntr, T, ly, serve_iters, inner);
    return registry_lily_certify_ctx(ly, h, prefill, val_seqs, val_teacher_out, nval, T, pol, rep);
}
