#include "../../include/cce/cce_model.h"
#include "../../include/cce/cce_forest.h"
#include "../../include/cce/cce_learn.h"
#include "../../include/cce/cce_gpu.h"
#include "../../include/cce/cce_dataset.h"
#include "../../include/cce/cce_router.h"
#include "../../include/cce/cce_tensor.h"
#include "../../include/cce/cce_cascade.h"
#include "../../include/cce/cce_autograd.h"
#include "cce_model_internal.h"
#include "cce_model_io.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

cce_result cce_model_create(cce_model** model, const char* name) {
    if (!model) return CCE_ERR_INVALID_ARG;
    cce_model* m = (cce_model*)calloc(1, sizeof(cce_model));
    if (!m) return CCE_ERR_OOM;
    strncpy(m->name, name ? name : "unnamed_model", sizeof(m->name)-1);
    m->diff_mode = CCE_DIFF_LOCAL;
    cce_router_init(&m->router, 1.0f, 2);
    m->classify = 0;
    m->goodness_threshold = 0.7f;
    m->dfa_strength = 0.0f;
    m->grad_clip = 0.0f;
    m->owns_forests = 0;
    m->owns_scheduler = 0;
    m->gpu = NULL;
    m->owns_gpu = 0;
    m->use_autograd = 0;
    m->ag = NULL;
    *model = m;
    return CCE_OK;
}

void cce_model_destroy(cce_model* model) {
    if (!model) return;
    if (model->owns_forests) {
        for (int i = 0; i < model->num_forests; ++i) {
            if (model->forests[i]) cce_forest_close(model->forests[i]);
        }
    }
    if (model->owns_scheduler && model->scheduler) {
        free(model->scheduler);
    }
    if (model->owns_gpu && model->gpu) {
        cce_gpu_destroy(model->gpu);
    }
    if (model->ag) {
        cce_ag_destroy(model->ag);
    }
    free(model);
}

cce_result cce_model_add_forest(cce_model* model, cce_forest* forest, const char* name) {
    if (!model || !forest || model->num_forests >= 16) return CCE_ERR_INVALID_ARG;
    int idx = model->num_forests;
    model->forests[idx] = forest;
    strncpy(model->forest_names[idx], name ? name : "forest", sizeof(model->forest_names[idx])-1);
    model->num_forests++;
    return CCE_OK;
}

cce_result cce_model_set_scheduler(cce_model* model, cce_scheduler* scheduler) {
    if (!model) return CCE_ERR_INVALID_ARG;
    model->scheduler = scheduler;
    return CCE_OK;
}

cce_result cce_model_set_diff_mode(cce_model* model, cce_diff_mode_t mode) {
    if (!model) return CCE_ERR_INVALID_ARG;
    model->diff_mode = mode;
    return CCE_OK;
}

cce_result cce_model_set_loss(cce_model* model, int classify) {
    if (!model) return CCE_ERR_INVALID_ARG;
    model->classify = classify ? 1 : 0;
    return CCE_OK;
}

cce_result cce_model_set_learn_params(cce_model* model, float goodness_threshold,
                                      float dfa_strength, float grad_clip) {
    if (!model) return CCE_ERR_INVALID_ARG;
    if (goodness_threshold > 0.0f) model->goodness_threshold = goodness_threshold;
    if (dfa_strength > 0.0f)       model->dfa_strength = dfa_strength;
    if (grad_clip > 0.0f)          model->grad_clip = grad_clip;
    return CCE_OK;
}

cce_result cce_model_set_gpu(cce_model* model, struct cce_gpu_ctx* gpu_ctx) {
    if (!model) return CCE_ERR_INVALID_ARG;
    if (model->owns_gpu && model->gpu && model->gpu != gpu_ctx) {
        /* free previous owned */
        /* we don't have direct destroy here, but caller is responsible or use owned setter */
    }
    model->gpu = gpu_ctx;
    model->owns_gpu = 0;
    return CCE_OK;
}

cce_result cce_model_set_gpu_owned(cce_model* model, struct cce_gpu_ctx* gpu_ctx) {
    if (!model) return CCE_ERR_INVALID_ARG;
    if (model->owns_gpu && model->gpu) {
        cce_gpu_destroy(model->gpu);
    }
    model->gpu = gpu_ctx;
    model->owns_gpu = (gpu_ctx != NULL) ? 1 : 0;
    return CCE_OK;
}

cce_result cce_model_set_use_autograd(cce_model* model, int use) {
    if (!model) return CCE_ERR_INVALID_ARG;
    model->use_autograd = use ? 1 : 0;
    if (model->use_autograd) {
        if (!model->ag) {
            /* Larger tape for scaling up */
            if (cce_ag_create(&model->ag, 4096) != CCE_OK) {
                model->use_autograd = 0;
                return CCE_ERR_OOM;
            }
        }
    } else {
        if (model->ag) {
            cce_ag_destroy(model->ag);
            model->ag = NULL;
        }
    }
    return CCE_OK;
}

/* Internal: pick best forest for this input using simple centroid match (proxy for "router at model level").
   Falls back to 0. Uses names only for debug/logging. */
static int pick_forest_for_sample(cce_model* m, const float* x, size_t dim) {
    if (!m || m->num_forests <= 1) return 0;
    int best = 0;
    float best_d = 1e30f;
    for (int fi = 0; fi < m->num_forests; ++fi) {
        cce_forest* f = m->forests[fi];
        if (!f || f->num_branches == 0 || f->centroid_dim <= 0) continue;
        int cdim = f->centroid_dim;
        float d = 0.0f;
        int use_d = (cdim < (int)dim) ? cdim : (int)dim;
        /* proxy: distance to forest's branch-0 centroid (specialist proxy) */
        for (int dd = 0; dd < use_d; ++dd) {
            float df = x[dd] - f->centroids[0 * cdim + dd];
            d += df * df;
        }
        if (d < best_d) { best_d = d; best = fi; }
    }
    return best;
}

static double cce_compute_loss(int classify, const float* pred, const float* tgt, int n) {
    if (classify) {
        float maxv = pred[0];
        for (int i = 1; i < n; i++) if (pred[i] > maxv) maxv = pred[i];
        double sum = 0.0;
        for (int i = 0; i < n; i++) sum += exp((double)pred[i] - maxv);
        double ce = 0.0;
        for (int i = 0; i < n; i++) {
            double p = exp((double)pred[i] - maxv) / (sum + 1e-12);
            if (tgt[i] > 0.0f) ce -= (double)tgt[i] * log(p + 1e-12);
        }
        return ce;
    }
    double mse = 0.0;
    for (int i = 0; i < n; i++) { double d = (double)pred[i] - (double)tgt[i]; mse += d * d; }
    return 0.5 * mse;
}

/* Internal: dispatch a single sample through model: pick forest (by centroid match), route inside via SSMax router
   to branch, respect per-branch + model diff_mode + scheduler LR, call adapt. Returns per-sample loss proxy. */
static double dispatch_train_sample(cce_model* m, const float* xin, const float* ytgt, size_t in_dim, size_t out_dim, size_t epoch, double last_loss) {
    if (m->num_forests == 0) return 1.0;

    int fi = pick_forest_for_sample(m, xin, in_dim);
    cce_forest* f = m->forests[fi];

    /* route inside the forest to the right branch using router */
    int bi = 0;
    float score = 0.0f;
    cce_router_route(&m->router, f, xin, (int)in_dim, &bi, &score);
    if (bi < 0 || bi >= f->num_branches) bi = 0;

    cce_branch* br = &f->branches[bi];
    if (!br->cascade || br->cascade->num_blocks == 0) {
        /* promote if needed */
        if (cce_forest_promote_to_hot(f, bi) != CCE_OK || !br->cascade) return 1.0;
    }

    /* determine effective diff_mode: per-branch wins over forest/model */
    cce_diff_mode_t dm = (br->diff_mode >= 0) ? br->diff_mode :
                         (f->diff_mode >= 0 ? f->diff_mode : m->diff_mode);

    float lr = m->scheduler ?
               cce_scheduler_get_lr(m->scheduler, (int)epoch, (float)last_loss) : 0.01f;

    cce_learner learner;
    cce_learner_init(&learner, m->goodness_threshold);
    learner.diff_mode = dm;
    learner.classify = m->classify;
    if (m->dfa_strength > 0.0f) learner.dfa_strength = m->dfa_strength;
    if (m->grad_clip   > 0.0f) learner.grad_clip   = m->grad_clip;
    learner.gpu = m->gpu;  /* enable CUDA/OpenCL acceleration when available */

    /* build tensors (owned) */
    int xsh[1] = {(int)in_dim};
    int ysh[1] = {(int)out_dim};
    cce_tensor x, y;
    if (cce_tensor_alloc(&x, xsh, 1) != CCE_OK) return 1.0;
    if (cce_tensor_alloc(&y, ysh, 1) != CCE_OK) { cce_tensor_free(&x); return 1.0; }
    memcpy(x.data, xin, in_dim * sizeof(float));
    memcpy(y.data, ytgt, out_dim * sizeof(float));

    /* also sync cascade for the eff logic in learn */
    if (br->cascade->diff_mode < 0) br->cascade->diff_mode = dm;

    double sample_loss;
    cce_tensor pred; memset(&pred, 0, sizeof(pred));
    if (cce_cascade_forward(br->cascade, &x, &pred) == CCE_OK && pred.numel >= (size_t)out_dim) {
        sample_loss = cce_compute_loss(m->classify, pred.data, y.data, (int)out_dim);
    } else {
        sample_loss = 0.05 + 0.5 * (1.0 - br->cascade->goodness);  /* fallback proxy */
    }
    cce_tensor_free(&pred);

    cce_result rc = CCE_OK;

    if (m->use_autograd && m->ag) {
        /* Reuse persistent ag ctx for scaling (no per-sample create/destroy).
           Use autograd tape ONLY for the exact tail/head (frozen CCE cascade output as features -> ag head) */
        cce_ag_ctx* ag = m->ag;
        cce_ag_reset(ag);  /* full reset for independent per-sample use */
        cce_ag_tensor *ap = NULL, *at = NULL, *al = NULL;
        cce_ag_tensor_from_array(ag, pred.data, 1, (int)out_dim, 0, &ap);
        cce_ag_tensor_from_array(ag, y.data, 1, (int)out_dim, 0, &at);
        if (m->classify) {
            cce_ag_softmax_cross_entropy(ag, ap, at, &al);
        } else {
            cce_ag_mse_loss(ag, ap, at, &al);
        }
        cce_ag_backward(ag, al);
        // In full wiring, sgd would update a separate head; here we exercise the tape for the exact tail
        // The cascade forward was "frozen features"
        // do not call learner_adapt when using autograd for exact tail
    } else {
        rc = cce_learner_adapt(&learner, br->cascade, &x, &y, lr);
    }
    if (rc != CCE_OK) sample_loss = 0.8;

    cce_tensor_free(&x);
    cce_tensor_free(&y);
    return sample_loss;
}

double cce_model_train(cce_model* model, cce_dataset* dataset, size_t max_epochs, float target_loss) {
    if (!model || !dataset) return -1.0;

    double last_loss = 1e9;
    for (size_t ep = 0; ep < max_epochs; ++ep) {
        cce_batch batch;
        double epoch_loss = 0.0;
        size_t batches = 0;
        size_t samples = 0;

        while (cce_dataset_next_batch(dataset, &batch) == CCE_OK) {
            if (model->num_forests == 0) break;

            double b_loss = 0.0;
            for (size_t s = 0; s < batch.batch_size; ++s) {
                const float* xin = batch.inputs + s * batch.in_dim;
                const float* yt  = batch.targets + s * batch.out_dim;
                double sl = dispatch_train_sample(model, xin, yt, batch.in_dim, batch.out_dim, ep, last_loss);
                b_loss += sl;
                samples++;
            }
            if (batch.batch_size > 0) {
                epoch_loss += b_loss / batch.batch_size;
            }
            batches++;
        }

        cce_dataset_reset(dataset);

        if (batches > 0) {
            last_loss = epoch_loss / batches;
        }

        if (last_loss < target_loss) break;

        if (getenv("CNET_CCE_VERBOSE") || getenv("CCE_VERBOSE")) {
            float lr = model->scheduler ? model->scheduler->current_lr : 0.01f;
            fprintf(stderr, "[cce_model] epoch %zu loss=%.4f lr=%.4f forests=%d samples=%zu\n",
                    ep, last_loss, lr, model->num_forests, samples);
        }
    }

    return last_loss;
}

double cce_model_train_batch(cce_model* model, cce_batch* batch) {
    if (!model || !batch || batch->batch_size == 0 || model->num_forests == 0) return -1.0;
    double loss = 0.0;
    for (size_t s = 0; s < batch->batch_size; ++s) {
        const float* xin = batch->inputs + s * batch->in_dim;
        const float* yt  = batch->targets + s * batch->out_dim;
        loss += dispatch_train_sample(model, xin, yt, batch->in_dim, batch->out_dim, 0, 0.5);
    }
    return loss / batch->batch_size;
}

cce_result cce_model_infer(cce_model* model, const float* input, int dim, int* out_label, float* conf) {
    if (!model || model->num_forests == 0) return CCE_ERR_INVALID_ARG;
    /* Route to first forest (or enhance with same pick + forest infer) */
    return cce_forest_infer(model->forests[0], input, dim, out_label, conf);
}

cce_result cce_model_infer_batch(cce_model* model, const cce_batch* batch,
                                 int* out_labels, float* confs) {
    if (!model || !batch || model->num_forests == 0 || !out_labels || !confs) return CCE_ERR_INVALID_ARG;
    if (batch->batch_size == 0) return CCE_OK;

    for (size_t s = 0; s < batch->batch_size; ++s) {
        const float* xin = batch->inputs + s * batch->in_dim;
        /* pick forest + delegate to forest level infer (forest handles its internal router) */
        int fi = pick_forest_for_sample(model, xin, batch->in_dim);
        cce_result rc = cce_forest_infer(model->forests[fi], xin, (int)batch->in_dim,
                                         &out_labels[s], &confs[s]);
        if (rc != CCE_OK) {
            out_labels[s] = -1;
            confs[s] = 0.0f;
        }
    }
    return CCE_OK;
}

static cce_result model_forward_sample(cce_model* model, const float* input, int in_dim,
                                       float* output, int output_cap, int* out_dim) {
    if (!model || !input || !output || output_cap <= 0 || model->num_forests == 0)
        return CCE_ERR_INVALID_ARG;

    int fi = pick_forest_for_sample(model, input, (size_t)in_dim);
    cce_forest* f = model->forests[fi];
    if (!f || f->num_branches == 0) return CCE_ERR_INVALID_ARG;

    int bi = 0;
    float score = 0.0f;
    if (cce_router_route(&model->router, f, input, in_dim, &bi, &score) != CCE_OK ||
        bi < 0 || bi >= f->num_branches) {
        if (cce_forest_recall(f, input, in_dim, &bi) != CCE_OK) return CCE_ERR_INVALID_ARG;
    }

    int xsh[1] = {in_dim};
    cce_tensor x;
    if (cce_tensor_alloc(&x, xsh, 1) != CCE_OK) return CCE_ERR_OOM;
    memcpy(x.data, input, (size_t)in_dim * sizeof(float));

    cce_tensor pred;
    memset(&pred, 0, sizeof(pred));
    cce_result rc = cce_forest_forward(f, bi, &x, &pred);
    cce_tensor_free(&x);
    if (rc != CCE_OK) return rc;

    int actual = (int)pred.numel;
    if (out_dim) *out_dim = actual;
    if (actual > output_cap) {
        cce_tensor_free(&pred);
        return CCE_ERR_INVALID_ARG;
    }

    memcpy(output, pred.data, (size_t)actual * sizeof(float));
    cce_tensor_free(&pred);
    return CCE_OK;
}

cce_result cce_model_forward(cce_model* model, const float* input, int in_dim,
                             float* output, int output_cap, int* out_dim) {
    if (in_dim <= 0) return CCE_ERR_INVALID_ARG;
    return model_forward_sample(model, input, in_dim, output, output_cap, out_dim);
}

cce_result cce_model_forward_batch(cce_model* model, const cce_batch* batch,
                                   float* outputs, int output_cap_per_sample, int* out_dim) {
    if (!model || !batch || !outputs || batch->batch_size == 0 || output_cap_per_sample <= 0)
        return CCE_ERR_INVALID_ARG;

    int first_dim = -1;
    for (size_t s = 0; s < batch->batch_size; ++s) {
        const float* xin = batch->inputs + s * batch->in_dim;
        float* out = outputs + s * (size_t)output_cap_per_sample;
        int dim = 0;
        cce_result rc = model_forward_sample(model, xin, (int)batch->in_dim,
                                             out, output_cap_per_sample, &dim);
        if (rc != CCE_OK) return rc;
        if (first_dim < 0) first_dim = dim;
        if (dim != first_dim) return CCE_ERR_INVALID_ARG;
    }
    if (out_dim) *out_dim = first_dim;
    return CCE_OK;
}

cce_result cce_model_save(cce_model* model, const char* path) {
    return cce_model_io_save(model, path);
}
cce_result cce_model_load(cce_model* model, const char* path) {
    return cce_model_io_load(model, path);
}
