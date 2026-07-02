#include "../../include/cce/cce_autograd.h"
#include "../../include/cce/cce_tensor.h"
#include "../../include/cce/cce_gpu.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Internal tape structures */

typedef struct {
    cce_ag_op op;
    int out_id;
    int a_id;
    int b_id;
    /* aux data for some ops, e.g. softmax target or saved tensors */
    float* aux;     /* owned if non-null */
    size_t aux_size;
} cce_ag_node;

struct cce_ag_tensor {
    cce_tensor value;
    cce_tensor grad;     /* allocated only if requires_grad */
    int requires_grad;
    int id;              /* index into ctx->tensors */
};

struct cce_ag_ctx {
    cce_ag_tensor* tensors;
    size_t num_tensors;
    size_t max_tensors;

    cce_ag_node* nodes;
    size_t num_nodes;
    size_t max_nodes;

    /* Preallocated working buffers to avoid alloc in backward */
    float* work1;
    float* work2;
    size_t work_size;

    int in_backward;

    /* Optional GPU for acceleration of supported ops */
    struct cce_gpu_ctx* gpu;
};

/* Internal helpers */
static int ag_alloc_tensor(cce_ag_ctx* ctx, int rows, int cols, int requires_grad, cce_ag_tensor** out);
static cce_result ag_record_node(cce_ag_ctx* ctx, cce_ag_op op, int out_id, int a_id, int b_id, const void* aux, size_t aux_elems);
static void ag_ensure_grad(cce_ag_tensor* t);
static cce_result ag_matmul_backward(cce_ag_ctx* ctx, const cce_ag_node* node);
static cce_result ag_add_backward(cce_ag_ctx* ctx, const cce_ag_node* node);
static cce_result ag_relu_backward(cce_ag_ctx* ctx, const cce_ag_node* node);
static cce_result ag_mse_backward(cce_ag_ctx* ctx, const cce_ag_node* node);
static cce_result ag_softmax_ce_backward(cce_ag_ctx* ctx, const cce_ag_node* node);
static cce_result ag_elementwise_backward(cce_ag_ctx* ctx, const cce_ag_node* node, cce_ag_op op);
static cce_result ag_sigmoid_backward(cce_ag_ctx* ctx, const cce_ag_node* node);
static cce_result ag_tanh_backward(cce_ag_ctx* ctx, const cce_ag_node* node);
static cce_result ag_reduce_backward(cce_ag_ctx* ctx, const cce_ag_node* node, int is_mean);
static cce_result ag_reshape_backward(cce_ag_ctx* ctx, const cce_ag_node* node);

cce_result cce_ag_create(cce_ag_ctx** ctx_out, size_t max_nodes) {
    if (!ctx_out || max_nodes == 0) return CCE_ERR_INVALID_ARG;

    cce_ag_ctx* ctx = (cce_ag_ctx*)calloc(1, sizeof(cce_ag_ctx));
    if (!ctx) return CCE_ERR_OOM;

    ctx->max_nodes = max_nodes;
    ctx->max_tensors = max_nodes * 2;  /* generous */

    ctx->tensors = (cce_ag_tensor*)calloc(ctx->max_tensors, sizeof(cce_ag_tensor));
    ctx->nodes = (cce_ag_node*)calloc(max_nodes, sizeof(cce_ag_node));

    if (!ctx->tensors || !ctx->nodes) {
        cce_ag_destroy(ctx);
        return CCE_ERR_OOM;
    }

    /* Preallocate work buffers for matmul etc. Assume reasonable size */
    ctx->work_size = 1024 * 1024; /* 4MB float workspace */
    ctx->work1 = (float*)malloc(ctx->work_size * sizeof(float));
    ctx->work2 = (float*)malloc(ctx->work_size * sizeof(float));
    ctx->gpu = NULL;

    if (!ctx->work1 || !ctx->work2) {
        cce_ag_destroy(ctx);
        return CCE_ERR_OOM;
    }

    *ctx_out = ctx;
    return CCE_OK;
}

void cce_ag_destroy(cce_ag_ctx* ctx) {
    if (!ctx) return;

    for (size_t i = 0; i < ctx->num_tensors; ++i) {
        cce_ag_tensor* t = &ctx->tensors[i];
        cce_tensor_free(&t->value);
        cce_tensor_free(&t->grad);
    }
    for (size_t i = 0; i < ctx->num_nodes; ++i) {
        free(ctx->nodes[i].aux);
    }

    free(ctx->tensors);
    free(ctx->nodes);
    free(ctx->work1);
    free(ctx->work2);
    free(ctx);
}

void cce_ag_zero_grad(cce_ag_ctx* ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->num_tensors; ++i) {
        if (ctx->tensors[i].requires_grad && ctx->tensors[i].grad.data) {
            cce_tensor_zero(&ctx->tensors[i].grad);
        }
    }
    ctx->in_backward = 0;
}

/* Full reset for reuse across independent forwards (e.g. per-sample in scaled training).
   Frees current data, resets counters so next alloc_tensor starts from 0.
   Call instead of or before zero_grad when reusing ctx for new independent computation. */
void cce_ag_reset(cce_ag_ctx* ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->num_tensors; ++i) {
        cce_tensor_free(&ctx->tensors[i].value);
        cce_tensor_free(&ctx->tensors[i].grad);
        memset(&ctx->tensors[i], 0, sizeof(cce_ag_tensor));
    }
    for (size_t i = 0; i < ctx->num_nodes; ++i) {
        if (ctx->nodes[i].aux) {
            free(ctx->nodes[i].aux);
        }
        memset(&ctx->nodes[i], 0, sizeof(cce_ag_node));
    }
    ctx->num_tensors = 0;
    ctx->num_nodes = 0;
    ctx->in_backward = 0;
}

static int ag_alloc_tensor(cce_ag_ctx* ctx, int rows, int cols, int requires_grad, cce_ag_tensor** out) {
    if (ctx->num_tensors >= ctx->max_tensors) return -1;

    int id = (int)ctx->num_tensors;
    cce_ag_tensor* t = &ctx->tensors[id];
    memset(t, 0, sizeof(*t));

    int shape[2] = {rows, cols};
    if (cce_tensor_alloc(&t->value, shape, 2) != CCE_OK) return -1;

    t->requires_grad = requires_grad;
    t->id = id;

    if (requires_grad) {
        if (cce_tensor_alloc(&t->grad, shape, 2) != CCE_OK) {
            cce_tensor_free(&t->value);
            return -1;
        }
        cce_tensor_zero(&t->grad);
    }

    ctx->num_tensors++;
    *out = t;
    return id;
}

cce_result cce_ag_tensor_from_array(cce_ag_ctx* ctx, const float* data,
                                    int rows, int cols, int requires_grad,
                                    cce_ag_tensor** out) {
    if (!ctx || !data || rows <= 0 || cols <= 0 || !out) return CCE_ERR_INVALID_ARG;

    cce_ag_tensor* t = NULL;
    int id = ag_alloc_tensor(ctx, rows, cols, requires_grad, &t);
    if (id < 0) return CCE_ERR_OOM;

    size_t n = (size_t)rows * cols;
    memcpy(t->value.data, data, n * sizeof(float));

    *out = t;
    return CCE_OK;
}

const float* cce_ag_data(const cce_ag_tensor* t) {
    return t ? t->value.data : NULL;
}

const float* cce_ag_grad(const cce_ag_tensor* t) {
    return (t && t->requires_grad) ? t->grad.data : NULL;
}

int cce_ag_tensor_rows(const cce_ag_tensor* t) { return t ? t->value.shape[0] : 0; }
int cce_ag_tensor_cols(const cce_ag_tensor* t) { return t ? t->value.shape[1] : 0; }

static cce_result ag_record_node(cce_ag_ctx* ctx, cce_ag_op op, int out_id, int a_id, int b_id,
                                 const void* aux, size_t aux_elems) {
    if (ctx->num_nodes >= ctx->max_nodes) return CCE_ERR_OOM;

    cce_ag_node* n = &ctx->nodes[ctx->num_nodes];
    n->op = op;
    n->out_id = out_id;
    n->a_id = a_id;
    n->b_id = b_id;
    n->aux = NULL;
    n->aux_size = 0;

    if (aux && aux_elems > 0) {
        n->aux = (float*)malloc(aux_elems * sizeof(float));
        if (!n->aux) return CCE_ERR_OOM;
        memcpy(n->aux, aux, aux_elems * sizeof(float));
        n->aux_size = aux_elems;
    }
    ctx->num_nodes++;
    return CCE_OK;
}

/* ==================== OPS ==================== */

cce_result cce_ag_matmul(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out) {
    if (!ctx || !a || !b || !out) return CCE_ERR_INVALID_ARG;
    if (a->value.shape[1] != b->value.shape[0]) return CCE_ERR_INVALID_ARG;

    int m = a->value.shape[0];
    int n = b->value.shape[1];

    cce_ag_tensor* c = NULL;
    if (ag_alloc_tensor(ctx, m, n, a->requires_grad || b->requires_grad, &c) < 0)
        return CCE_ERR_OOM;

    if (ctx->gpu && cce_gpu_is_cuda(ctx->gpu)) {
        /* GPU path for forward matmul (grads stay host for simplicity in v1) */
        cce_tensor a_dev, b_dev, c_dev;
        if (cce_gpu_upload(&a->value, &a_dev) == CCE_OK &&
            cce_gpu_upload(&b->value, &b_dev) == CCE_OK &&
            cce_gpu_upload(&c->value, &c_dev) == CCE_OK) {
            cce_gpu_matmul(ctx->gpu, &a_dev, &b_dev, &c_dev);
            cce_gpu_download(&c_dev, &c->value);
            cce_gpu_free_device(&a_dev);
            cce_gpu_free_device(&b_dev);
            cce_gpu_free_device(&c_dev);
        } else {
            cce_tensor_matmul(&a->value, &b->value, &c->value);
        }
    } else {
        cce_tensor_matmul(&a->value, &b->value, &c->value);
    }

    cce_result rc = ag_record_node(ctx, CCE_AG_OP_MATMUL, c->id, a->id, b->id, NULL, 0);
    if (rc != CCE_OK) return rc;

    *out = c;
    return CCE_OK;
}

cce_result cce_ag_add_bias(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor* bias, cce_ag_tensor** out) {
    if (!ctx || !x || !bias || !out) return CCE_ERR_INVALID_ARG;
    if (bias->value.shape[0] != 1 && bias->value.shape[1] != x->value.shape[1]) return CCE_ERR_INVALID_ARG;

    int rows = x->value.shape[0];
    int cols = x->value.shape[1];

    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, rows, cols, x->requires_grad || bias->requires_grad, &y) < 0)
        return CCE_ERR_OOM;

    cce_tensor_add(&x->value, &bias->value, &y->value);  /* broadcasting handled in tensor? fall back */

    /* Simple broadcast add for bias */
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            y->value.data[i*cols + j] = x->value.data[i*cols + j] + bias->value.data[j];
        }
    }

    cce_result rc = ag_record_node(ctx, CCE_AG_OP_ADD_BIAS, y->id, x->id, bias->id, NULL, 0);
    if (rc != CCE_OK) return rc;

    *out = y;
    return CCE_OK;
}

cce_result cce_ag_relu(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out) {
    if (!ctx || !x || !out) return CCE_ERR_INVALID_ARG;

    int r = x->value.shape[0], c = x->value.shape[1];
    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, r, c, x->requires_grad, &y) < 0) return CCE_ERR_OOM;

    size_t n = (size_t)r * c;
    for (size_t i = 0; i < n; ++i) {
        float v = x->value.data[i];
        y->value.data[i] = (v > 0.0f) ? v : 0.0f;
    }

    ag_record_node(ctx, CCE_AG_OP_RELU, y->id, x->id, -1, NULL, 0);
    *out = y;
    return CCE_OK;
}

cce_result cce_ag_sigmoid(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out) {
    if (!ctx || !x || !out) return CCE_ERR_INVALID_ARG;
    int r = x->value.shape[0], c = x->value.shape[1];
    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, r, c, x->requires_grad, &y) < 0) return CCE_ERR_OOM;

    size_t n = (size_t)r * c;
    for (size_t i = 0; i < n; ++i) {
        float v = x->value.data[i];
        y->value.data[i] = 1.0f / (1.0f + expf(-v));
    }
    ag_record_node(ctx, CCE_AG_OP_SIGMOID, y->id, x->id, -1, NULL, 0);
    *out = y;
    return CCE_OK;
}

cce_result cce_ag_exp(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out) {
    if (!ctx || !x || !out) return CCE_ERR_INVALID_ARG;
    int r = x->value.shape[0], c = x->value.shape[1];
    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, r, c, x->requires_grad, &y) < 0) return CCE_ERR_OOM;
    size_t n = (size_t)r * c;
    for (size_t i = 0; i < n; ++i) y->value.data[i] = expf(x->value.data[i]);
    ag_record_node(ctx, CCE_AG_OP_NONE, y->id, x->id, -1, NULL, 0);
    *out = y;
    return CCE_OK;
}

cce_result cce_ag_log(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out) {
    if (!ctx || !x || !out) return CCE_ERR_INVALID_ARG;
    int r = x->value.shape[0], c = x->value.shape[1];
    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, r, c, x->requires_grad, &y) < 0) return CCE_ERR_OOM;
    size_t n = (size_t)r * c;
    for (size_t i = 0; i < n; ++i) y->value.data[i] = logf(x->value.data[i] + 1e-12f);
    ag_record_node(ctx, CCE_AG_OP_NONE, y->id, x->id, -1, NULL, 0);
    *out = y;
    return CCE_OK;
}

cce_result cce_ag_tanh(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out) {
    if (!ctx || !x || !out) return CCE_ERR_INVALID_ARG;
    int r = x->value.shape[0], c = x->value.shape[1];
    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, r, c, x->requires_grad, &y) < 0) return CCE_ERR_OOM;

    size_t n = (size_t)r * c;
    for (size_t i = 0; i < n; ++i) {
        float v = x->value.data[i];
        y->value.data[i] = tanhf(v);
    }
    ag_record_node(ctx, CCE_AG_OP_TANH, y->id, x->id, -1, NULL, 0);
    *out = y;
    return CCE_OK;
}

/* Broadcasting helper for 2D: supports same shape, or (1,C) or (R,1) broadcast */
static int can_broadcast(int ra, int ca, int rb, int cb, int* or_, int* oc) {
    if (ra == rb && ca == cb) { *or_ = ra; *oc = ca; return 1; }
    if (ra == 1 && ca == cb) { *or_ = rb; *oc = cb; return 1; }
    if (ca == 1 && ra == rb) { *or_ = ra; *oc = cb; return 1; }
    if (rb == 1 && cb == ca) { *or_ = ra; *oc = ca; return 1; }
    if (cb == 1 && rb == ra) { *or_ = ra; *oc = ca; return 1; }
    return 0;
}

static cce_result ag_elementwise(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out, cce_ag_op op, float (*fwd)(float,float)) {
    if (!ctx || !a || !b || !out) return CCE_ERR_INVALID_ARG;
    int or_, oc;
    if (!can_broadcast(a->value.shape[0], a->value.shape[1], b->value.shape[0], b->value.shape[1], &or_, &oc)) {
        return CCE_ERR_INVALID_ARG;
    }

    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, or_, oc, a->requires_grad || b->requires_grad, &y) < 0) return CCE_ERR_OOM;

    // Simplified broadcast forward (assumes row or col for one)
    for (int i = 0; i < or_; ++i) {
        for (int j = 0; j < oc; ++j) {
            float va = a->value.data[ (a->value.shape[0]==1 ? 0 : i)*a->value.shape[1] + (a->value.shape[1]==1 ? 0 : j) ];
            float vb = b->value.data[ (b->value.shape[0]==1 ? 0 : i)*b->value.shape[1] + (b->value.shape[1]==1 ? 0 : j) ];
            y->value.data[i*oc + j] = fwd(va, vb);
        }
    }
    ag_record_node(ctx, op, y->id, a->id, b->id, NULL, 0);
    *out = y;
    return CCE_OK;
}

static float fwd_mul(float x, float y) { return x * y; }
static float fwd_sub(float x, float y) { return x - y; }
static float fwd_div(float x, float y) { return x / (y + 1e-12f); }

cce_result cce_ag_mul(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out) {
    return ag_elementwise(ctx, a, b, out, CCE_AG_OP_MUL, fwd_mul);
}

cce_result cce_ag_sub(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out) {
    return ag_elementwise(ctx, a, b, out, CCE_AG_OP_SUB, fwd_sub);
}

cce_result cce_ag_div(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out) {
    return ag_elementwise(ctx, a, b, out, CCE_AG_OP_NONE, fwd_div); // reuse
}

/* Reshape (numel must match) - data is copied for simplicity */
cce_result cce_ag_reshape(cce_ag_ctx* ctx, cce_ag_tensor* t, int new_rows, int new_cols, cce_ag_tensor** out) {
    if (!ctx || !t || !out) return CCE_ERR_INVALID_ARG;
    size_t old_n = (size_t)t->value.shape[0] * t->value.shape[1];
    size_t new_n = (size_t)new_rows * new_cols;
    if (old_n != new_n) return CCE_ERR_INVALID_ARG;

    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, new_rows, new_cols, t->requires_grad, &y) < 0) return CCE_ERR_OOM;

    memcpy(y->value.data, t->value.data, old_n * sizeof(float));

    // Record with aux holding old shape for backward reshape
    float aux[2] = { (float)t->value.shape[0], (float)t->value.shape[1] };
    ag_record_node(ctx, CCE_AG_OP_RESHAPE, y->id, t->id, -1, aux, 2);
    *out = y;
    return CCE_OK;
}

/* Sum / mean - support reduce to scalar (dim = -1) or keep dim for now simple scalar */
cce_result cce_ag_sum(cce_ag_ctx* ctx, cce_ag_tensor* t, int dim, cce_ag_tensor** out) {
    if (!ctx || !t || !out) return CCE_ERR_INVALID_ARG;
    // For v1: only full reduce to scalar (dim ignored or -1)
    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, 1, 1, t->requires_grad, &y) < 0) return CCE_ERR_OOM;

    double s = 0.0;
    size_t n = t->value.numel;
    for (size_t i = 0; i < n; ++i) s += t->value.data[i];
    y->value.data[0] = (float)s;

    ag_record_node(ctx, CCE_AG_OP_SUM, y->id, t->id, -1, NULL, 0);
    *out = y;
    return CCE_OK;
}

cce_result cce_ag_mean(cce_ag_ctx* ctx, cce_ag_tensor* t, int dim, cce_ag_tensor** out) {
    cce_result rc = cce_ag_sum(ctx, t, dim, out);
    if (rc != CCE_OK) return rc;
    (*out)->value.data[0] /= (float)t->value.numel;
    // Record mean separately or adjust in backward
    // For simplicity, overwrite last node op
    if (ctx->num_nodes > 0) ctx->nodes[ctx->num_nodes-1].op = CCE_AG_OP_MEAN;
    return CCE_OK;
}

cce_result cce_ag_mse_loss(cce_ag_ctx* ctx, cce_ag_tensor* pred, cce_ag_tensor* target, cce_ag_tensor** loss) {
    if (!ctx || !pred || !target || !loss) return CCE_ERR_INVALID_ARG;
    if (pred->value.numel != target->value.numel) return CCE_ERR_INVALID_ARG;

    cce_ag_tensor* l = NULL;
    if (ag_alloc_tensor(ctx, 1, 1, pred->requires_grad, &l) < 0) return CCE_ERR_OOM;

    double sum = 0.0;
    size_t n = pred->value.numel;
    for (size_t i = 0; i < n; ++i) {
        double d = pred->value.data[i] - target->value.data[i];
        sum += d * d;
    }
    l->value.data[0] = (float)(sum / n);

    /* Save target for backward? For MSE we can recompute or store diff */
    ag_record_node(ctx, CCE_AG_OP_MSE_LOSS, l->id, pred->id, target->id, NULL, 0);
    *loss = l;
    return CCE_OK;
}

cce_result cce_ag_softmax_cross_entropy(cce_ag_ctx* ctx, cce_ag_tensor* logits, cce_ag_tensor* target, cce_ag_tensor** loss) {
    if (!ctx || !logits || !target || !loss) return CCE_ERR_INVALID_ARG;
    if (logits->value.shape[0] != target->value.shape[0] ||
        logits->value.shape[1] != target->value.shape[1]) return CCE_ERR_INVALID_ARG;

    int batch = logits->value.shape[0];
    int classes = logits->value.shape[1];

    cce_ag_tensor* l = NULL;
    if (ag_alloc_tensor(ctx, 1, 1, logits->requires_grad, &l) < 0) return CCE_ERR_OOM;

    /* Compute softmax + ce, save max and sum for stability in backward via aux */
    float* probs = (float*)malloc((size_t)batch * classes * sizeof(float));
    if (!probs) { /* cleanup */ return CCE_ERR_OOM; }

    double total_loss = 0.0;
    for (int b = 0; b < batch; ++b) {
        float* row = logits->value.data + b * classes;
        float m = row[0];
        for (int c = 1; c < classes; ++c) if (row[c] > m) m = row[c];

        double sum = 0.0;
        for (int c = 0; c < classes; ++c) {
            float e = expf(row[c] - m);
            probs[b*classes + c] = e;
            sum += e;
        }
        float invsum = (float)(1.0 / sum);
        for (int c = 0; c < classes; ++c) probs[b*classes + c] *= invsum;

        double row_loss = 0.0;
        for (int c = 0; c < classes; ++c) {
            row_loss -= target->value.data[b*classes + c] * logf(probs[b*classes + c] + 1e-12f);
        }
        total_loss += row_loss;
    }
    l->value.data[0] = (float)(total_loss / batch);

    /* Save probs for backward */
    ag_record_node(ctx, CCE_AG_OP_SOFTMAX_CE, l->id, logits->id, target->id, probs, (size_t)batch * classes);
    free(probs);

    *loss = l;
    return CCE_OK;
}

/* ==================== BACKWARD ==================== */

cce_result cce_ag_backward(cce_ag_ctx* ctx, cce_ag_tensor* loss) {
    if (!ctx || !loss) return CCE_ERR_INVALID_ARG;
    if (!loss->requires_grad) return CCE_OK;

    ctx->in_backward = 1;
    ag_ensure_grad(loss);
    /* Seed gradient of loss = 1.0 */
    if (loss->grad.numel > 0) loss->grad.data[0] = 1.0f;

    /* Walk nodes in reverse order */
    for (int i = (int)ctx->num_nodes - 1; i >= 0; --i) {
        cce_ag_node* node = &ctx->nodes[i];
        cce_result rc = CCE_OK;

        switch (node->op) {
            case CCE_AG_OP_MATMUL:     rc = ag_matmul_backward(ctx, node); break;
            case CCE_AG_OP_ADD_BIAS:   rc = ag_add_backward(ctx, node); break;
            case CCE_AG_OP_RELU:       rc = ag_relu_backward(ctx, node); break;
            case CCE_AG_OP_MSE_LOSS:   rc = ag_mse_backward(ctx, node); break;
            case CCE_AG_OP_SOFTMAX_CE: rc = ag_softmax_ce_backward(ctx, node); break;
            case CCE_AG_OP_TANH:       rc = ag_tanh_backward(ctx, node); break;
            case CCE_AG_OP_SIGMOID:    rc = ag_sigmoid_backward(ctx, node); break;
            case CCE_AG_OP_MUL:
            case CCE_AG_OP_SUB:
            case CCE_AG_OP_DIV:        rc = ag_elementwise_backward(ctx, node, node->op); break;
            case CCE_AG_OP_RESHAPE:    rc = ag_reshape_backward(ctx, node); break;
            case CCE_AG_OP_SUM:        rc = ag_reduce_backward(ctx, node, 0); break;
            case CCE_AG_OP_MEAN:       rc = ag_reduce_backward(ctx, node, 1); break;
            case CCE_AG_OP_CUSTOM:
                // Dispatch: extract fn, user context, and input ids from aux.
                if (node->aux) {
                    const unsigned char* p = (const unsigned char*)node->aux;
                    const size_t aux_bytes = node->aux_size * sizeof(float);
                    const size_t header_bytes =
                        sizeof(cce_ag_custom_backward_fn) + sizeof(void*) + sizeof(int);
                    cce_ag_custom_backward_fn fn;
                    void* uctx;
                    int num_inputs;
                    cce_ag_tensor** inputs = NULL;

                    if (aux_bytes < header_bytes) return CCE_ERR_INVALID_ARG;
                    memcpy(&fn, p, sizeof(fn));
                    p += sizeof(fn);
                    memcpy(&uctx, p, sizeof(uctx));
                    p += sizeof(uctx);
                    memcpy(&num_inputs, p, sizeof(num_inputs));
                    p += sizeof(num_inputs);
                    if (num_inputs < 0) return CCE_ERR_INVALID_ARG;
                    if (aux_bytes < header_bytes + (size_t)num_inputs * sizeof(int)) {
                        return CCE_ERR_INVALID_ARG;
                    }

                    if (num_inputs > 0) {
                        inputs = (cce_ag_tensor**)calloc((size_t)num_inputs, sizeof(*inputs));
                        if (!inputs) return CCE_ERR_OOM;
                        for (int k = 0; k < num_inputs; ++k) {
                            int input_id;
                            memcpy(&input_id, p, sizeof(input_id));
                            p += sizeof(input_id);
                            if (input_id >= 0) {
                                if ((size_t)input_id >= ctx->num_tensors) {
                                    free(inputs);
                                    return CCE_ERR_INVALID_ARG;
                                }
                                inputs[k] = &ctx->tensors[input_id];
                            }
                        }
                    }

                    if (fn) {
                        fn(ctx, &ctx->tensors[node->out_id], inputs, num_inputs, uctx);
                    }
                    free(inputs);
                }
                rc = CCE_OK;
                break;
            default: break;
        }
        if (rc != CCE_OK) {
            ctx->in_backward = 0;
            return rc;
        }
    }
    ctx->in_backward = 0;
    return CCE_OK;
}

static void ag_ensure_grad(cce_ag_tensor* t) {
    if (t->requires_grad && t->grad.numel == 0) {
        int shape[2] = {t->value.shape[0], t->value.shape[1]};
        cce_tensor_alloc(&t->grad, shape, 2);
        cce_tensor_zero(&t->grad);
    }
}

static cce_result ag_matmul_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    cce_ag_tensor* c = &ctx->tensors[node->out_id];
    cce_ag_tensor* a = &ctx->tensors[node->a_id];
    cce_ag_tensor* b = &ctx->tensors[node->b_id];

    if (a->requires_grad) {
        ag_ensure_grad(a);
        /* dL/dA = dL/dC @ B^T */
        /* Simple loop implementation for small sizes */
        int m = a->value.shape[0], k = a->value.shape[1], n = b->value.shape[1];
        for (int i = 0; i < m; ++i) {
            for (int kk = 0; kk < k; ++kk) {
                float s = 0.0f;
                for (int j = 0; j < n; ++j) {
                    s += c->grad.data[i*n + j] * b->value.data[kk*n + j];
                }
                a->grad.data[i*k + kk] += s;
            }
        }
    }
    if (b->requires_grad) {
        ag_ensure_grad(b);
        int m = a->value.shape[0], k = a->value.shape[1], n = b->value.shape[1];
        for (int kk = 0; kk < k; ++kk) {
            for (int j = 0; j < n; ++j) {
                float s = 0.0f;
                for (int i = 0; i < m; ++i) {
                    s += c->grad.data[i*n + j] * a->value.data[i*k + kk];
                }
                b->grad.data[kk*n + j] += s;
            }
        }
    }
    return CCE_OK;
}

static cce_result ag_add_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    cce_ag_tensor* y = &ctx->tensors[node->out_id];
    cce_ag_tensor* x = &ctx->tensors[node->a_id];
    cce_ag_tensor* bias = &ctx->tensors[node->b_id];

    if (x->requires_grad) {
        ag_ensure_grad(x);
        size_t n = x->value.numel;
        for (size_t i = 0; i < n; ++i) x->grad.data[i] += y->grad.data[i];
    }
    if (bias->requires_grad) {
        ag_ensure_grad(bias);
        int cols = x->value.shape[1];
        for (int j = 0; j < cols; ++j) {
            float s = 0.0f;
            for (int i = 0; i < x->value.shape[0]; ++i) {
                s += y->grad.data[i*cols + j];
            }
            bias->grad.data[j] += s;
        }
    }
    return CCE_OK;
}

static cce_result ag_relu_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    cce_ag_tensor* y = &ctx->tensors[node->out_id];
    cce_ag_tensor* x = &ctx->tensors[node->a_id];

    if (x->requires_grad) {
        ag_ensure_grad(x);
        size_t n = x->value.numel;
        for (size_t i = 0; i < n; ++i) {
            x->grad.data[i] += (x->value.data[i] > 0.0f) ? y->grad.data[i] : 0.0f;
        }
    }
    return CCE_OK;
}

static cce_result ag_mse_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    cce_ag_tensor* l = &ctx->tensors[node->out_id];
    cce_ag_tensor* pred = &ctx->tensors[node->a_id];
    cce_ag_tensor* tgt  = &ctx->tensors[node->b_id];

    if (pred->requires_grad) {
        ag_ensure_grad(pred);
        float scale = 2.0f / (float)pred->value.numel;
        size_t n = pred->value.numel;
        for (size_t i = 0; i < n; ++i) {
            float d = pred->value.data[i] - tgt->value.data[i];
            pred->grad.data[i] += scale * d * l->grad.data[0];
        }
    }
    return CCE_OK;
}

static cce_result ag_softmax_ce_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    cce_ag_tensor* l = &ctx->tensors[node->out_id];
    cce_ag_tensor* logits = &ctx->tensors[node->a_id];
    cce_ag_tensor* target = &ctx->tensors[node->b_id];

    if (!node->aux) return CCE_ERR_INVALID_ARG;

    const float* probs = node->aux;
    int batch = logits->value.shape[0];
    int classes = logits->value.shape[1];

    if (logits->requires_grad) {
        ag_ensure_grad(logits);
        float scale = l->grad.data[0] / (float)batch;
        for (int b = 0; b < batch; ++b) {
            for (int c = 0; c < classes; ++c) {
                float p = probs[b*classes + c];
                float t = target->value.data[b*classes + c];
                logits->grad.data[b*classes + c] += scale * (p - t);
            }
        }
    }
    return CCE_OK;
}

/* New backward for elementwise and reduce ops (simplified) */
static cce_result ag_elementwise_backward(cce_ag_ctx* ctx, const cce_ag_node* node, cce_ag_op op) {
    cce_ag_tensor* y = &ctx->tensors[node->out_id];
    cce_ag_tensor* a = &ctx->tensors[node->a_id];
    cce_ag_tensor* b = (node->b_id >= 0) ? &ctx->tensors[node->b_id] : NULL;

    if (a->requires_grad) {
        ag_ensure_grad(a);
        size_t n = a->value.numel;
        for (size_t i = 0; i < n; ++i) {
            float dy = y->grad.data[i];
            float da = 0;
            if (op == CCE_AG_OP_MUL) da = dy * (b ? b->value.data[i] : 1);
            else if (op == CCE_AG_OP_SUB) da = dy;
            else if (op == CCE_AG_OP_NONE) da = dy / (b ? (b->value.data[i] + 1e-12f) : 1); // div
            a->grad.data[i] += da;
        }
    }
    if (b && b->requires_grad) {
        ag_ensure_grad(b);
        size_t n = b->value.numel;
        for (size_t i = 0; i < n; ++i) {
            float dy = y->grad.data[i];
            float db = 0;
            if (op == CCE_AG_OP_MUL) db = dy * a->value.data[i];
            else if (op == CCE_AG_OP_SUB) db = -dy;
            else if (op == CCE_AG_OP_NONE) db = -dy * a->value.data[i] / ((b->value.data[i]+1e-12f)*(b->value.data[i]+1e-12f));
            b->grad.data[i] += db;
        }
    }
    return CCE_OK;
}

static cce_result ag_sigmoid_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    // For sigmoid we didn't record a special op. To make grad check work we need to compute on the fly or fix record.
    // For simplicity in this hardening pass, we implement direct grad using saved value.
    cce_ag_tensor* y = &ctx->tensors[node->out_id];
    cce_ag_tensor* x = &ctx->tensors[node->a_id];
    if (x->requires_grad) {
        ag_ensure_grad(x);
        size_t n = x->value.numel;
        for (size_t i = 0; i < n; ++i) {
            float s = y->value.data[i]; // sigmoid output
            x->grad.data[i] += y->grad.data[i] * s * (1.0f - s);
        }
    }
    return CCE_OK;
}

static cce_result ag_tanh_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    cce_ag_tensor* y = &ctx->tensors[node->out_id];
    cce_ag_tensor* x = &ctx->tensors[node->a_id];
    if (x->requires_grad) {
        ag_ensure_grad(x);
        size_t n = x->value.numel;
        for (size_t i = 0; i < n; ++i) {
            float th = y->value.data[i];
            x->grad.data[i] += y->grad.data[i] * (1.0f - th*th);
        }
    }
    return CCE_OK;
}

static cce_result ag_reduce_backward(cce_ag_ctx* ctx, const cce_ag_node* node, int is_mean) {
    cce_ag_tensor* y = &ctx->tensors[node->out_id]; // scalar grad
    cce_ag_tensor* t = &ctx->tensors[node->a_id];
    if (t->requires_grad) {
        ag_ensure_grad(t);
        float scale = is_mean ? (1.0f / t->value.numel) : 1.0f;
        size_t n = t->value.numel;
        float g = y->grad.data[0] * scale;
        for (size_t i = 0; i < n; ++i) t->grad.data[i] += g;
    }
    return CCE_OK;
}

static cce_result ag_reshape_backward(cce_ag_ctx* ctx, const cce_ag_node* node) {
    cce_ag_tensor* y = &ctx->tensors[node->out_id];
    cce_ag_tensor* orig = &ctx->tensors[node->a_id];
    if (orig->requires_grad) {
        ag_ensure_grad(orig);
        // reshape grad back to original shape
        size_t n = orig->value.numel;
        for (size_t i = 0; i < n; ++i) {
            orig->grad.data[i] += y->grad.data[i];
        }
    }
    return CCE_OK;
}

/* ==================== OPTIMIZER ==================== */

cce_result cce_ag_sgd_step(cce_ag_tensor** parameters, size_t count, float lr) {
    if (!parameters || count == 0) return CCE_ERR_INVALID_ARG;

    for (size_t p = 0; p < count; ++p) {
        cce_ag_tensor* t = parameters[p];
        if (!t || !t->requires_grad || t->grad.numel == 0) continue;

        size_t n = t->value.numel;
        for (size_t i = 0; i < n; ++i) {
            t->value.data[i] -= lr * t->grad.data[i];
        }
    }
    return CCE_OK;
}

/* Custom op support (stub for now - records node and calls backward on demand) */
cce_result cce_ag_custom_op(cce_ag_ctx* ctx,
                            cce_ag_tensor** inputs,
                            int num_inputs,
                            int out_rows,
                            int out_cols,
                            cce_ag_custom_backward_fn backward_fn,
                            void* user_ctx,
                            cce_ag_tensor** out) {
    if (!ctx || !out || num_inputs < 0 || out_rows <= 0 || out_cols <= 0) return CCE_ERR_INVALID_ARG;
    if (num_inputs > 0 && !inputs) return CCE_ERR_INVALID_ARG;

    int requires = 0;
    for (int i = 0; i < num_inputs; i++) if (inputs[i] && inputs[i]->requires_grad) requires = 1;

    cce_ag_tensor* y = NULL;
    if (ag_alloc_tensor(ctx, out_rows, out_cols, requires, &y) < 0) return CCE_ERR_OOM;

    // For custom, the forward is assumed done by caller outside, or we leave value zero.
    // Record fn/user context plus input tensor ids in a byte-addressed payload.
    size_t aux_bytes = sizeof(backward_fn) + sizeof(user_ctx) + sizeof(num_inputs) +
                       (size_t)num_inputs * sizeof(int);
    size_t aux_elems = (aux_bytes + sizeof(float) - 1) / sizeof(float);
    float* aux = (float*)calloc(aux_elems, sizeof(float));
    if (!aux) return CCE_ERR_OOM;

    unsigned char* p = (unsigned char*)aux;
    memcpy(p, &backward_fn, sizeof(backward_fn));
    p += sizeof(backward_fn);
    memcpy(p, &user_ctx, sizeof(user_ctx));
    p += sizeof(user_ctx);
    memcpy(p, &num_inputs, sizeof(num_inputs));
    p += sizeof(num_inputs);
    for (int i = 0; i < num_inputs; ++i) {
        int input_id = inputs[i] ? inputs[i]->id : -1;
        memcpy(p, &input_id, sizeof(input_id));
        p += sizeof(input_id);
    }

    cce_result rc = ag_record_node(ctx, CCE_AG_OP_CUSTOM, y->id, -1, -1, aux, aux_elems);
    free(aux);
    if (rc != CCE_OK) return rc;

    *out = y;
    return CCE_OK;
}

cce_result cce_ag_set_gpu(cce_ag_ctx* ctx, struct cce_gpu_ctx* gpu_ctx) {
    if (!ctx) return CCE_ERR_INVALID_ARG;
    ctx->gpu = gpu_ctx;
    return CCE_OK;
}

/* Simple Adam for ag tensors (host only for v1; uses per-call state or simple).
   For production heads, use the block Adam or extend.
   This is placeholder for breadth. */
cce_result cce_ag_adam_step(cce_ag_tensor** parameters, size_t count, float lr,
                            float beta1, float beta2, float eps, int t) {
    if (!parameters || count == 0) return CCE_ERR_INVALID_ARG;
    // Very simple: no per-param moments stored here (unlike blocks).
    // For demo, fall back to sgd. Full moments would require extra storage.
    // In real, we'd attach moment buffers.
    return cce_ag_sgd_step(parameters, count, lr);
}
