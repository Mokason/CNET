/* registry_lora — train and serve a rank-r cce_lora adapter from a real unit's
   labeled retrain queue. See include/router/registry_lora.h.

   The adapter is an additive low-rank correction on the unit's output:
       serve(x) = btn_forward(base)(x) + (alpha/rank)*(xA)B
   trained on the residual (teacher_target - base) already sitting in the unit's
   RetrainQueue.labeled_* set — the same port-validated pairs registry_heal uses.
   The base BTN is never modified. */

#include "../../include/router/registry_lora.h"
#include "../../include/nn.h"          /* btn_forward, BinaryTransformNetwork */
#include "../../include/cce/cce_tensor.h"

#include <stdlib.h>
#include <string.h>

registry_lora_opts registry_lora_defaults(void) {
    registry_lora_opts o;
    o.rank = 8;
    o.alpha = 16.0f;
    o.train = cce_lora_train_defaults();
    o.train.epochs = 600;
    o.train.lr = 0.02f;
    return o;
}

/* first strcmp match wins, mirroring registry_set_state */
static RegistryEntry *find_entry(PrimitiveRegistry *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->count; i++)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    return NULL;
}

int registry_teach_lora(PrimitiveRegistry *reg, const char *name,
                        const registry_lora_opts *opt, registry_lora_stats *stats) {
    RegistryEntry *e = find_entry(reg, name);
    if (!e || !e->btn || !e->queue) return -1;
    RetrainQueue *q = e->queue;
    const size_t n = q->labeled_count;
    const int in = (int)q->input_count, out = (int)q->output_count;
    if (n == 0 || in <= 0 || out <= 0) return -1;

    registry_lora_opts o = opt ? *opt : registry_lora_defaults();

    /* Build (input, residual) float sets. residual = target - base(input).
       btn_forward reuses an internal buffer, so copy the base out per pair. */
    float *X = malloc(n * (size_t)in * sizeof(float));
    float *R = malloc(n * (size_t)out * sizeof(float));
    if (!X || !R) { free(X); free(R); return -1; }

    double pre = 0.0;
    for (size_t s = 0; s < n; s++) {
        const double *xin = q->labeled_inputs + s * (size_t)in;
        const double *tgt = q->labeled_targets + s * (size_t)out;
        const double *base = btn_forward(e->btn, xin);  /* borrowed buffer */
        if (!base) { free(X); free(R); return -1; }
        for (int i = 0; i < in; i++) X[s * in + i] = (float)xin[i];
        for (int oo = 0; oo < out; oo++) {
            double resid = tgt[oo] - base[oo];
            R[s * out + oo] = (float)resid;
            pre += resid * resid;
        }
    }
    pre /= (double)(n * (size_t)out);

    cce_lora *adp = malloc(sizeof(*adp));
    if (!adp) { free(X); free(R); return -1; }
    if (cce_lora_init(adp, in, out, o.rank, o.alpha, 0x51A17u + (uint32_t)o.rank) != CCE_OK) {
        free(adp); free(X); free(R); return -1;
    }
    double post = cce_lora_train(adp, X, R, n, &o.train);
    if (post < 0.0) { cce_lora_free(adp); free(adp); free(X); free(R); return -1; }

    /* Attach (replace any prior). Borrowed by the registry. */
    if (e->lora) { cce_lora_free(e->lora); free(e->lora); }
    e->lora = adp;

    if (stats) {
        stats->pairs = n;
        stats->pre_mse = pre;
        stats->post_mse = post;
        stats->in_dim = in;
        stats->out_dim = out;
        stats->rank = o.rank;
        stats->params = cce_lora_param_count(adp);
        stats->dense_params = cce_lora_dense_param_count(adp);
    }
    free(X); free(R);
    return 0;
}

int registry_forward_with_lora(PrimitiveRegistry *reg, const char *name,
                               const double *input, double *out) {
    RegistryEntry *e = find_entry(reg, name);
    if (!e || !e->btn || !input || !out) return -1;
    const int outc = (int)e->btn->output_count;
    const int inc = (int)e->btn->input_count;
    const double *base = btn_forward(e->btn, input);
    if (!base) return -1;
    for (int o = 0; o < outc; o++) out[o] = base[o];
    if (!e->lora) return 0;                       /* base unchanged, zero overhead */

    /* out += delta via the float adapter */
    cce_tensor x, y; int xs[1] = { inc }, ys[1] = { outc };
    if (cce_tensor_alloc(&x, xs, 1) != CCE_OK) return -1;
    if (cce_tensor_alloc(&y, ys, 1) != CCE_OK) { cce_tensor_free(&x); return -1; }
    for (int i = 0; i < inc; i++) x.data[i] = (float)input[i];
    for (int o = 0; o < outc; o++) y.data[o] = 0.0f;
    cce_result rc = cce_lora_apply((const cce_lora *)e->lora, &x, &y);
    if (rc == CCE_OK)
        for (int o = 0; o < outc; o++) out[o] += (double)y.data[o];
    cce_tensor_free(&x); cce_tensor_free(&y);
    return rc == CCE_OK ? 0 : -1;
}

int registry_has_lora(const PrimitiveRegistry *reg, const char *name) {
    RegistryEntry *e = find_entry((PrimitiveRegistry *)reg, name);
    return (e && e->lora) ? 1 : 0;
}

void registry_lora_detach(PrimitiveRegistry *reg, const char *name) {
    RegistryEntry *e = find_entry(reg, name);
    if (e && e->lora) { cce_lora_free(e->lora); free(e->lora); e->lora = NULL; }
}
