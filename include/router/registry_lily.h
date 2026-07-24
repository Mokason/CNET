#ifndef CNET_REGISTRY_LILY_H
#define CNET_REGISTRY_LILY_H

/* Host a cce_lily deep-base adapter under registry_lora's certify gate: same
   accept/reject policy + report structs, applied to a Lily adapter served
   through the DS forward, and taught by serve-in-the-loop training. This is the
   "the certify/orchestrator machinery is parametrization-agnostic" claim made
   real — teach (serve-loop) -> certify (regression gate) -> serve. */

#include "registry_lora.h"       /* registry_lora_cert_policy / registry_lora_cert_report */
#include "../cce/cce_lily.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cce_ds_host;

/* Certify a Lily adapter served through the deep forward against held-out
   (input, teacher_out) pairs: for each sample compare the FROZEN base final
   output and the (base+adapter) final output to the teacher, counting per-sample
   error fixes/regressions (MSE-mode), then apply the policy. Returns 1 (pass) /
   0 (fail) / -1 (error). inputs[n*d_model], teacher_out[n*d_model]. `pol` is
   required (the same registry_lora_cert_policy used for cce_lora). */
int registry_lily_certify(cce_lily *ly, struct cce_ds_host *h,
                          const float *inputs, const float *teacher_out, size_t n,
                          const registry_lora_cert_policy *pol,
                          registry_lora_cert_report *rep);

/* Teach via serve-in-the-loop training, then certify — the gate. teacher_res is
   the teacher's per-layer residual stream [ntr*n_layer*d_model] for training;
   val_inputs/val_teacher_out are held out for certification. Returns the certify
   verdict; the caller installs serving only on PASS. */
int registry_lily_teach_certify(cce_lily *ly, struct cce_ds_host *h,
                                const float *train_inputs, const float *teacher_res, size_t ntr,
                                int serve_iters, const cce_lily_train_opts *inner,
                                const float *val_inputs, const float *val_teacher_out, size_t nval,
                                const registry_lora_cert_policy *pol,
                                registry_lora_cert_report *rep);

/* ---- multi-token (compute-quality teacher) variants ----------------------
   Same gate, but the base/adapter final outputs are the QUERY-token residuals
   after prefilling T-1 context tokens in the host's current compute config.
   This is what a compute-quality teacher (dense attention / all experts vs the
   host's cheap student config) requires, since that gap is multi-token. seqs is
   [n*T*d_model]; teacher_out is the query-token teacher final [n*d_model];
   teacher_res is the teacher's per-layer query residuals [ntr*n_layer*d_model]. */
int registry_lily_certify_ctx(cce_lily *ly, struct cce_ds_host *h,
                              const cce_lily_compute *prefill,
                              const float *seqs, const float *teacher_out,
                              size_t n, int T,
                              const registry_lora_cert_policy *pol,
                              registry_lora_cert_report *rep);
int registry_lily_teach_certify_ctx(cce_lily *ly, struct cce_ds_host *h,
                                    const cce_lily_compute *prefill,
                                    const float *train_seqs, const float *teacher_res, size_t ntr, int T,
                                    int serve_iters, const cce_lily_train_opts *inner,
                                    const float *val_seqs, const float *val_teacher_out, size_t nval,
                                    const registry_lora_cert_policy *pol,
                                    registry_lora_cert_report *rep);

#ifdef __cplusplus
}
#endif

#endif /* CNET_REGISTRY_LILY_H */
