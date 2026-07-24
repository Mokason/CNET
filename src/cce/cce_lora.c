/* cce_lora — rank-r low-rank adapter for a frozen linear map. See cce_lora.h.

   Prototype goal: capture a per-skill output correction as (alpha/rank)*(xA)B
   with rank*(in+out) trained params instead of a dense in*out update, and let
   it pack/stream through the existing weight-store expert path. Math is explicit
   here so the delta is an exact bilinear map (the cce_block LINEAR forward would
   otherwise apply a sigmoid). */

#include "../../include/cce/cce_lora.h"
#include "../../include/cce/cce_block.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- deterministic gaussian (LCG + Box-Muller), no libc rand dependency ---- */
static uint32_t lcg_next(uint32_t* s) { *s = *s * 1664525u + 1013904223u; return *s; }
static float randn(uint32_t* s) {
    float u1 = (lcg_next(s) >> 8) * (1.0f / 16777216.0f); /* (0,1) */
    float u2 = (lcg_next(s) >> 8) * (1.0f / 16777216.0f);
    if (u1 < 1e-7f) u1 = 1e-7f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static inline float lora_scale(const cce_lora* lo) {
    return (lo->rank > 0) ? (lo->alpha / (float)lo->rank) : 0.0f;
}

cce_result cce_lora_init(cce_lora* lo, int in_dim, int out_dim, int rank,
                         float alpha, uint32_t seed) {
    if (!lo || in_dim <= 0 || out_dim <= 0 || rank <= 0) return CCE_ERR_INVALID_ARG;
    memset(lo, 0, sizeof(*lo));
    lo->in_dim = in_dim; lo->out_dim = out_dim; lo->rank = rank; lo->alpha = alpha;

    int ash[2] = { in_dim, rank };
    int bsh[2] = { rank, out_dim };
    cce_result rc = cce_tensor_alloc(&lo->A, ash, 2);
    if (rc != CCE_OK) return rc;
    rc = cce_tensor_alloc(&lo->B, bsh, 2);
    if (rc != CCE_OK) { cce_tensor_free(&lo->A); return rc; }

    /* A ~ N(0, sigma^2) with sigma = 1/sqrt(in); B = 0 (delta starts at exactly 0). */
    uint32_t s = seed ? seed : 0x9E3779B9u;
    float sigma = 1.0f / sqrtf((float)in_dim);
    for (size_t i = 0; i < lo->A.numel; i++) lo->A.data[i] = randn(&s) * sigma;
    memset(lo->B.data, 0, lo->B.numel * sizeof(float));
    return CCE_OK;
}

void cce_lora_free(cce_lora* lo) {
    if (!lo) return;
    cce_tensor_free(&lo->A);
    cce_tensor_free(&lo->B);
    memset(lo, 0, sizeof(*lo));
}

size_t cce_lora_param_count(const cce_lora* lo) {
    return lo ? (size_t)lo->rank * (lo->in_dim + lo->out_dim) : 0;
}
size_t cce_lora_dense_param_count(const cce_lora* lo) {
    return lo ? (size_t)lo->in_dim * lo->out_dim : 0;
}

/* y += (alpha/rank) * (x A) B */
cce_result cce_lora_apply(const cce_lora* lo, const cce_tensor* x, cce_tensor* y_accum) {
    if (!lo || !x || !y_accum) return CCE_ERR_INVALID_ARG;
    if (x->numel != (size_t)lo->in_dim || y_accum->numel != (size_t)lo->out_dim)
        return CCE_ERR_INVALID_ARG;
    const int in = lo->in_dim, out = lo->out_dim, r = lo->rank;
    const float sc = lora_scale(lo);

    /* tmp[k] = sum_i x[i] * A[i*r + k] */
    float stackbuf[64];
    float* tmp = (r <= 64) ? stackbuf : (float*)malloc((size_t)r * sizeof(float));
    if (!tmp) return CCE_ERR_OOM;
    for (int k = 0; k < r; k++) tmp[k] = 0.0f;
    for (int i = 0; i < in; i++) {
        const float xi = x->data[i];
        if (xi == 0.0f) continue;
        const float* Ai = &lo->A.data[(size_t)i * r];
        for (int k = 0; k < r; k++) tmp[k] += xi * Ai[k];
    }
    /* y[o] += sc * sum_k tmp[k] * B[k*out + o] */
    for (int k = 0; k < r; k++) {
        const float t = sc * tmp[k];
        if (t == 0.0f) continue;
        const float* Bk = &lo->B.data[(size_t)k * out];
        for (int o = 0; o < out; o++) y_accum->data[o] += t * Bk[o];
    }
    if (tmp != stackbuf) free(tmp);
    return CCE_OK;
}

cce_result cce_lora_merge(const cce_lora* lo, cce_tensor* W_inout) {
    if (!lo || !W_inout) return CCE_ERR_INVALID_ARG;
    if (W_inout->numel != (size_t)lo->in_dim * lo->out_dim) return CCE_ERR_INVALID_ARG;
    const int in = lo->in_dim, out = lo->out_dim, r = lo->rank;
    const float sc = lora_scale(lo);
    for (int i = 0; i < in; i++) {
        const float* Ai = &lo->A.data[(size_t)i * r];
        float* Wi = &W_inout->data[(size_t)i * out];
        for (int k = 0; k < r; k++) {
            const float a = sc * Ai[k];
            if (a == 0.0f) continue;
            const float* Bk = &lo->B.data[(size_t)k * out];
            for (int o = 0; o < out; o++) Wi[o] += a * Bk[o];
        }
    }
    return CCE_OK;
}

/* ---- training -------------------------------------------------------------- */
cce_lora_train_opts cce_lora_train_defaults(void) {
    cce_lora_train_opts o;
    o.epochs = 300; o.lr = 0.01f; o.use_adam = 1;
    o.weight_decay = 0.0f; o.target_loss = 0.0f; o.log_every = 0;
    return o;
}

double cce_lora_eval_mse(const cce_lora* lo, const float* inputs,
                         const float* residuals, size_t n) {
    if (!lo || !inputs || !residuals || n == 0) return -1.0;
    const int in = lo->in_dim, out = lo->out_dim;
    cce_tensor x, y;
    int xs[1] = { in }, ys[1] = { out };
    if (cce_tensor_alloc(&x, xs, 1) != CCE_OK) return -1.0;
    if (cce_tensor_alloc(&y, ys, 1) != CCE_OK) { cce_tensor_free(&x); return -1.0; }
    double se = 0.0;
    for (size_t s = 0; s < n; s++) {
        memcpy(x.data, inputs + s * in, (size_t)in * sizeof(float));
        memset(y.data, 0, (size_t)out * sizeof(float));       /* base out = 0 in the residual frame */
        cce_lora_apply(lo, &x, &y);
        const float* rref = residuals + s * out;
        for (int o = 0; o < out; o++) { double e = y.data[o] - rref[o]; se += e * e; }
    }
    cce_tensor_free(&x); cce_tensor_free(&y);
    return se / (double)(n * out);
}

/* Full-batch Adam over A,B minimising 0.5*mean(pred-resid)^2. Base W frozen. */
double cce_lora_train(cce_lora* lo, const float* inputs, const float* residuals,
                      size_t n, const cce_lora_train_opts* opt) {
    if (!lo || !inputs || !residuals || n == 0) return -1.0;
    cce_lora_train_opts o = opt ? *opt : cce_lora_train_defaults();
    const int in = lo->in_dim, out = lo->out_dim, r = lo->rank;
    const float sc = lora_scale(lo);
    const size_t na = (size_t)in * r, nb = (size_t)r * out;

    float *gA = calloc(na, sizeof(float)), *gB = calloc(nb, sizeof(float));
    float *mA = calloc(na, sizeof(float)), *vA = calloc(na, sizeof(float));
    float *mB = calloc(nb, sizeof(float)), *vB = calloc(nb, sizeof(float));
    float *tmp = malloc((size_t)r * sizeof(float));
    float *e   = malloc((size_t)out * sizeof(float));
    if (!gA || !gB || !mA || !vA || !mB || !vB || !tmp || !e) {
        free(gA); free(gB); free(mA); free(vA); free(mB); free(vB); free(tmp); free(e);
        return -1.0;
    }
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    double last = -1.0;

    for (int ep = 1; ep <= o.epochs; ep++) {
        memset(gA, 0, na * sizeof(float));
        memset(gB, 0, nb * sizeof(float));
        double se = 0.0;
        for (size_t s = 0; s < n; s++) {
            const float* x = inputs + s * in;
            const float* rref = residuals + s * out;
            /* forward: tmp = xA; pred = sc*tmp*B */
            for (int k = 0; k < r; k++) tmp[k] = 0.0f;
            for (int i = 0; i < in; i++) {
                const float xi = x[i]; if (xi == 0.0f) continue;
                const float* Ai = &lo->A.data[(size_t)i * r];
                for (int k = 0; k < r; k++) tmp[k] += xi * Ai[k];
            }
            for (int oo = 0; oo < out; oo++) e[oo] = -rref[oo];       /* pred starts 0 */
            for (int k = 0; k < r; k++) {
                const float t = sc * tmp[k];
                const float* Bk = &lo->B.data[(size_t)k * out];
                for (int oo = 0; oo < out; oo++) e[oo] += t * Bk[oo];
            }
            for (int oo = 0; oo < out; oo++) se += (double)e[oo] * e[oo];
            /* grads: dB[k,o] = sc*tmp[k]*e[o]; dA[i,k] = sc*x[i]*sum_o e[o]*B[k,o] */
            for (int k = 0; k < r; k++) {
                const float* Bk = &lo->B.data[(size_t)k * out];
                float* gBk = &gB[(size_t)k * out];
                float back = 0.0f;                      /* sum_o e[o]*B[k,o] */
                const float sct = sc * tmp[k];
                for (int oo = 0; oo < out; oo++) { gBk[oo] += sct * e[oo]; back += e[oo] * Bk[oo]; }
                const float sb = sc * back;
                for (int i = 0; i < in; i++) {
                    const float xi = x[i]; if (xi == 0.0f) continue;
                    gA[(size_t)i * r + k] += xi * sb;
                }
            }
        }
        const float invn = 1.0f / (float)n;
        /* Adam / SGD step */
        for (size_t i = 0; i < na; i++) {
            float g = gA[i] * invn + o.weight_decay * lo->A.data[i];
            if (o.use_adam) {
                mA[i] = b1 * mA[i] + (1 - b1) * g;
                vA[i] = b2 * vA[i] + (1 - b2) * g * g;
                float mh = mA[i] / (1 - powf(b1, (float)ep));
                float vh = vA[i] / (1 - powf(b2, (float)ep));
                lo->A.data[i] -= o.lr * mh / (sqrtf(vh) + eps);
            } else lo->A.data[i] -= o.lr * g;
        }
        for (size_t i = 0; i < nb; i++) {
            float g = gB[i] * invn + o.weight_decay * lo->B.data[i];
            if (o.use_adam) {
                mB[i] = b1 * mB[i] + (1 - b1) * g;
                vB[i] = b2 * vB[i] + (1 - b2) * g * g;
                float mh = mB[i] / (1 - powf(b1, (float)ep));
                float vh = vB[i] / (1 - powf(b2, (float)ep));
                lo->B.data[i] -= o.lr * mh / (sqrtf(vh) + eps);
            } else lo->B.data[i] -= o.lr * g;
        }
        last = se / (double)(n * out);
        if (o.log_every && (ep % o.log_every == 0 || ep == 1))
            printf("  [lora] epoch %d  mse=%.6g\n", ep, last);
        if (o.target_loss > 0.0f && last <= o.target_loss) break;
    }
    free(gA); free(gB); free(mA); free(vA); free(mB); free(vB); free(tmp); free(e);
    return last;
}

/* ---- persistence ----------------------------------------------------------- */
#define CLORA_MAGIC "CLORA1\n"
cce_result cce_lora_save(const cce_lora* lo, const char* path) {
    if (!lo || !path) return CCE_ERR_INVALID_ARG;
    FILE* f = fopen(path, "wb");
    if (!f) return CCE_ERR_IO;
    int32_t hdr[3] = { lo->in_dim, lo->out_dim, lo->rank };
    int ok = 1;
    ok &= (fwrite(CLORA_MAGIC, 1, 7, f) == 7);
    ok &= (fwrite(hdr, sizeof(int32_t), 3, f) == 3);
    ok &= (fwrite(&lo->alpha, sizeof(float), 1, f) == 1);
    ok &= (fwrite(&lo->base_digest, sizeof(uint64_t), 1, f) == 1);
    ok &= (fwrite(lo->A.data, sizeof(float), lo->A.numel, f) == lo->A.numel);
    ok &= (fwrite(lo->B.data, sizeof(float), lo->B.numel, f) == lo->B.numel);
    fclose(f);
    return ok ? CCE_OK : CCE_ERR_IO;
}

cce_result cce_lora_load(cce_lora* lo, const char* path) {
    if (!lo || !path) return CCE_ERR_INVALID_ARG;
    FILE* f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    char magic[7];
    int32_t hdr[3]; float alpha; uint64_t dig;
    if (fread(magic, 1, 7, f) != 7 || memcmp(magic, CLORA_MAGIC, 7) != 0 ||
        fread(hdr, sizeof(int32_t), 3, f) != 3 ||
        fread(&alpha, sizeof(float), 1, f) != 1 ||
        fread(&dig, sizeof(uint64_t), 1, f) != 1) { fclose(f); return CCE_ERR_IO; }
    cce_result rc = cce_lora_init(lo, hdr[0], hdr[1], hdr[2], alpha, 1);
    if (rc != CCE_OK) { fclose(f); return rc; }
    lo->base_digest = dig;
    if (fread(lo->A.data, sizeof(float), lo->A.numel, f) != lo->A.numel ||
        fread(lo->B.data, sizeof(float), lo->B.numel, f) != lo->B.numel) {
        cce_lora_free(lo); fclose(f); return CCE_ERR_IO;
    }
    fclose(f);
    return CCE_OK;
}

/* ---- cascade bridge (weight-store / streaming path) ------------------------ */
cce_result cce_lora_to_cascade(const cce_lora* lo, cce_cascade** out) {
    if (!lo || !out) return CCE_ERR_INVALID_ARG;
    cce_cascade* cas = NULL;
    cce_result rc = cce_cascade_create(&cas, 2);
    if (rc != CCE_OK) return rc;
    rc = cce_cascade_add_linear_head(cas, lo->in_dim, lo->rank, 0.0f);
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }
    rc = cce_cascade_add_linear_head(cas, lo->rank, lo->out_dim, 0.0f);
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }
    /* block0.weights = A ; block1.weights = (alpha/rank) * B (fold scale in). */
    const float sc = lora_scale(lo);
    cce_block* b0 = &cas->blocks[0];
    cce_block* b1 = &cas->blocks[1];
    memcpy(b0->weights.data, lo->A.data, lo->A.numel * sizeof(float));
    memset(b0->bias.data, 0, (size_t)lo->rank * sizeof(float));
    for (size_t i = 0; i < lo->B.numel; i++) b1->weights.data[i] = sc * lo->B.data[i];
    memset(b1->bias.data, 0, (size_t)lo->out_dim * sizeof(float));
    *out = cas;
    return CCE_OK;
}

cce_result cce_lora_from_cascade(const cce_cascade* cas, int in_dim, int out_dim,
                                 int rank, cce_lora* out) {
    if (!cas || !out || cas->num_blocks != 2) return CCE_ERR_INVALID_ARG;
    /* Reconstruct with alpha = rank so scale = 1 and B already carries the
       folded scale — the DELTA is preserved exactly (the A/B/alpha split is
       not, by design; only the bilinear map matters). */
    cce_result rc = cce_lora_init(out, in_dim, out_dim, rank, (float)rank, 1);
    if (rc != CCE_OK) return rc;
    if (cas->blocks[0].weights.numel != out->A.numel ||
        cas->blocks[1].weights.numel != out->B.numel) { cce_lora_free(out); return CCE_ERR_INVALID_ARG; }
    memcpy(out->A.data, cas->blocks[0].weights.data, out->A.numel * sizeof(float));
    memcpy(out->B.data, cas->blocks[1].weights.data, out->B.numel * sizeof(float));
    return CCE_OK;
}

/* ---- integration (M4) ------------------------------------------------------ */
cce_result cce_lora_head_forward(const cce_block* head, const cce_lora* lo,
                                 const cce_tensor* in, cce_tensor* out) {
    if (!head || !in || !out) return CCE_ERR_INVALID_ARG;
    if (lo && head->type != CCE_BLOCK_LINEAR_HEAD) return CCE_ERR_UNSUPPORTED;
    cce_result rc = cce_block_forward(head, in, out);   /* frozen base */
    if (rc != CCE_OK || !lo) return rc;
    return cce_lora_apply(lo, in, out);                 /* + delta */
}

double cce_lora_fit_residual(cce_lora* lo, const float* inputs,
                             const float* base_out, const float* teacher_out,
                             size_t n, const cce_lora_train_opts* opt) {
    if (!lo || !inputs || !base_out || !teacher_out || n == 0) return -1.0;
    const int out = lo->out_dim;
    float* resid = malloc(n * (size_t)out * sizeof(float));
    if (!resid) return -1.0;
    for (size_t i = 0; i < n * (size_t)out; i++) resid[i] = teacher_out[i] - base_out[i];
    double mse = cce_lora_train(lo, inputs, resid, n, opt);
    free(resid);
    return mse;
}
