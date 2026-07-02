#include "../../include/cce/cce_dataset.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct cce_dataset {
    float* inputs;      /* owned copy */
    float* targets;
    size_t n_samples;
    size_t in_dim;
    size_t out_dim;
    size_t batch_size;
    size_t current;
    size_t* indices;    /* for shuffling */
    int owns_data;
};

cce_result cce_dataset_from_arrays(cce_dataset** ds_out,
                                   const float* inputs,
                                   const float* targets,
                                   size_t n_samples,
                                   size_t in_dim,
                                   size_t out_dim,
                                   size_t batch_size) {
    if (!ds_out || !inputs || !targets || n_samples == 0 || batch_size == 0) 
        return CCE_ERR_INVALID_ARG;

    cce_dataset* ds = (cce_dataset*)calloc(1, sizeof(cce_dataset));
    if (!ds) return CCE_ERR_OOM;

    ds->n_samples = n_samples;
    ds->in_dim = in_dim;
    ds->out_dim = out_dim;
    ds->batch_size = batch_size;
    ds->current = 0;
    ds->owns_data = 1;

    size_t input_bytes = n_samples * in_dim * sizeof(float);
    size_t target_bytes = n_samples * out_dim * sizeof(float);

    ds->inputs = (float*)malloc(input_bytes);
    ds->targets = (float*)malloc(target_bytes);
    if (!ds->inputs || !ds->targets) {
        free(ds->inputs); free(ds->targets); free(ds);
        return CCE_ERR_OOM;
    }

    memcpy(ds->inputs, inputs, input_bytes);
    memcpy(ds->targets, targets, target_bytes);

    /* identity indices */
    ds->indices = (size_t*)malloc(n_samples * sizeof(size_t));
    for (size_t i = 0; i < n_samples; i++) ds->indices[i] = i;

    *ds_out = ds;
    return CCE_OK;
}

cce_result cce_dataset_wrap_arrays(cce_dataset** ds_out,
                                   const float* inputs,
                                   const float* targets,
                                   size_t n_samples,
                                   size_t in_dim,
                                   size_t out_dim,
                                   size_t batch_size) {
    if (!ds_out || !inputs || !targets || n_samples == 0 || batch_size == 0)
        return CCE_ERR_INVALID_ARG;

    cce_dataset* ds = (cce_dataset*)calloc(1, sizeof(cce_dataset));
    if (!ds) return CCE_ERR_OOM;

    ds->n_samples = n_samples;
    ds->in_dim = in_dim;
    ds->out_dim = out_dim;
    ds->batch_size = batch_size;
    ds->current = 0;
    ds->owns_data = 0;  /* wrap: no free on destroy */

    /* point directly; caller owns lifetime */
    ds->inputs = (float*)inputs;
    ds->targets = (float*)targets;

    ds->indices = (size_t*)malloc(n_samples * sizeof(size_t));
    for (size_t i = 0; i < n_samples; i++) ds->indices[i] = i;

    *ds_out = ds;
    return CCE_OK;
}

cce_result cce_dataset_reset(cce_dataset* ds) {
    if (!ds) return CCE_ERR_INVALID_ARG;
    ds->current = 0;
    return CCE_OK;
}

cce_result cce_dataset_next_batch(cce_dataset* ds, cce_batch* batch) {
    if (!ds || !batch) return CCE_ERR_INVALID_ARG;
    if (ds->current >= ds->n_samples) return CCE_ERR_NOT_FOUND;

    size_t remaining = ds->n_samples - ds->current;
    size_t this_batch = (remaining < ds->batch_size) ? remaining : ds->batch_size;

    batch->inputs = ds->inputs + ds->current * ds->in_dim;
    batch->targets = ds->targets + ds->current * ds->out_dim;
    batch->batch_size = this_batch;
    batch->in_dim = ds->in_dim;
    batch->out_dim = ds->out_dim;
    batch->index = ds->current;

    ds->current += this_batch;
    return CCE_OK;
}

void cce_dataset_destroy(cce_dataset* ds) {
    if (!ds) return;
    if (ds->owns_data) {
        free(ds->inputs);
        free(ds->targets);
    }
    free(ds->indices);
    free(ds);
}

cce_result cce_dataset_from_tile_memory(cce_dataset** ds,
                                        void* tile_memory,
                                        size_t batch_size) {
    (void)tile_memory; (void)batch_size;
    if (!ds) return CCE_ERR_INVALID_ARG;
    /* Optional wiring: extract tile->vec + heat/label into flat arrays, then
       wrap_arrays (zero copy). Perceptual (7seg/glyph/grid via cce_perceptual_leaf)
       and symbolic (tile + corpus retrieval) become first-class for cce_model. */
    return CCE_ERR_UNSUPPORTED;
}

cce_result cce_dataset_shuffle(cce_dataset* ds) {
    if (!ds || !ds->indices) return CCE_ERR_INVALID_ARG;
    for (size_t i = ds->n_samples - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        size_t tmp = ds->indices[i];
        ds->indices[i] = ds->indices[j];
        ds->indices[j] = tmp;
    }
    /* Actually reorder the owned data arrays using the perm so that
       subsequent next_batch slices remain contiguous. Safe even if not owns. */
    if (ds->inputs && ds->targets) {
        float* tmp_in = (float*)malloc(ds->in_dim * sizeof(float));
        float* tmp_tg = (float*)malloc(ds->out_dim * sizeof(float));
        if (tmp_in && tmp_tg) {
            for (size_t i = 0; i < ds->n_samples; ++i) {
                size_t src = ds->indices[i];
                if (src == i) continue;
                /* simple in-place perm via temp (swap with care) */
                memcpy(tmp_in, ds->inputs + i * ds->in_dim, ds->in_dim * sizeof(float));
                memcpy(ds->inputs + i * ds->in_dim, ds->inputs + src * ds->in_dim, ds->in_dim * sizeof(float));
                memcpy(ds->inputs + src * ds->in_dim, tmp_in, ds->in_dim * sizeof(float));

                memcpy(tmp_tg, ds->targets + i * ds->out_dim, ds->out_dim * sizeof(float));
                memcpy(ds->targets + i * ds->out_dim, ds->targets + src * ds->out_dim, ds->out_dim * sizeof(float));
                memcpy(ds->targets + src * ds->out_dim, tmp_tg, ds->out_dim * sizeof(float));
            }
        }
        free(tmp_in); free(tmp_tg);
    }
    /* reset indices to identity after physical reorder */
    for (size_t i = 0; i < ds->n_samples; i++) ds->indices[i] = i;
    return CCE_OK;
}
