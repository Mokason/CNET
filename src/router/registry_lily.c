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
