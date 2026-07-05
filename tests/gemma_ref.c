/* gemma_ref: an INDEPENDENT, clean-room gemma4 forward in pure C, to serve as
 * ground truth for debugging cce_gguf's forward. Loads weights on demand as
 * F32 (cce_gguf_load_tensor_by_name), holds one layer at a time. No GPU, no
 * int8, no cascades — just the math, written straight from the gemma4 spec.
 *
 * Usage: gemma_ref <model.gguf> <tok0> <tok1> ...   -> prints top-10 next ids.
 * Geometry for gemma4-v2 is read from metadata + tensor shapes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_tensor.h"

static float *T(const cce_gguf *g, const char *fmt, int l, int *numel) {
    char nm[128]; snprintf(nm, sizeof nm, fmt, l);
    cce_tensor t = {0};
    if (cce_gguf_load_tensor_by_name(g, nm, &t) != CCE_OK) { if (numel) *numel = 0; return NULL; }
    float *out = malloc(t.numel * sizeof(float));
    memcpy(out, t.data, t.numel * sizeof(float));
    if (numel) *numel = (int)t.numel;
    cce_tensor_free(&t);
    return out;
}

/* RMSNorm over the last `d` elements: y = x/sqrt(mean(x^2)+eps) * w  (w raw). */
static void rmsnorm(float *y, const float *x, const float *w, int d, float eps) {
    double ss = 0; for (int i = 0; i < d; i++) ss += (double)x[i] * x[i];
    float s = 1.0f / sqrtf((float)(ss / d) + eps);
    for (int i = 0; i < d; i++) y[i] = x[i] * s * w[i];
}

/* out[o] = sum_i W[o*nin+i] * in[i]   (GGUF weight is [nout, nin] row-major). */
static void matvec(float *out, const float *W, const float *in, int nout, int nin) {
    for (int o = 0; o < nout; o++) {
        const float *wr = W + (size_t)o * nin;
        double a = 0; for (int i = 0; i < nin; i++) a += (double)wr[i] * in[i];
        out[o] = (float)a;
    }
}

/* NEOX rope on a head of width hd (rotate i with i+hd/2). */
static void rope(float *v, int hd, int pos, float base) {
    int half = hd / 2;
    for (int i = 0; i < half; i++) {
        float fr = 1.0f / powf(base, (float)(2 * i) / hd);
        float a = pos * fr, c = cosf(a), s = sinf(a);
        float x0 = v[i], x1 = v[i + half];
        v[i] = x0 * c - x1 * s;
        v[i + half] = x0 * s + x1 * c;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <model> <tok..>\n", argv[0]); return 2; }
    cce_gguf *g = NULL;
    if (cce_gguf_load(argv[1], &g) != CCE_OK) { fprintf(stderr, "load fail\n"); return 1; }

    const int D = 3840, NL = 48, NH = 16, NKV = 8, HD = 256, FF = 15360, V = 262144;
    const float eps = 1e-6f, embed_scale = sqrtf((float)D);
    int n = argc - 2;
    int *tok = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) tok[i] = atoi(argv[i + 2]);

    int emb_numel; float *emb = T(g, "token_embd.weight", 0, &emb_numel); /* [V, D] */
    float *onorm = T(g, "output_norm.weight", 0, NULL);
    if (!emb || !onorm) { fprintf(stderr, "no embed/onorm\n"); return 1; }

    /* residual stream x[n][D] */
    float *x = calloc((size_t)n * D, sizeof(float));
    for (int t = 0; t < n; t++)
        for (int i = 0; i < D; i++) x[(size_t)t * D + i] = emb[(size_t)tok[t] * D + i] * embed_scale;

    float *kcache = malloc((size_t)n * NKV * HD * sizeof(float));
    float *vcache = malloc((size_t)n * NKV * HD * sizeof(float));
    const int MAXQ = 8192, MAXKV = 2048;
    float *h = malloc(D * sizeof(float)), *q = malloc(MAXQ * sizeof(float));
    float *kk = malloc(MAXKV * sizeof(float)), *vv = malloc(MAXKV * sizeof(float));
    float *att = malloc(MAXQ * sizeof(float)), *proj = malloc(D * sizeof(float));
    float *sc = malloc(n * sizeof(float));
    float *g1 = malloc(FF * sizeof(float)), *u1 = malloc(FF * sizeof(float));
    free(kcache); free(vcache);
    kcache = malloc((size_t)n * MAXKV * sizeof(float));
    vcache = malloc((size_t)n * MAXKV * sizeof(float));

    for (int l = 0; l < NL; l++) {
        int qnum, knum, vnum, hdn;
        float *anorm = T(g, "blk.%d.attn_norm.weight", l, NULL);
        float *Wq = T(g, "blk.%d.attn_q.weight", l, &qnum);
        float *Wk = T(g, "blk.%d.attn_k.weight", l, &knum);
        float *Wv = T(g, "blk.%d.attn_v.weight", l, &vnum);
        float *Wo = T(g, "blk.%d.attn_output.weight", l, NULL);
        float *qn = T(g, "blk.%d.attn_q_norm.weight", l, &hdn);
        float *kn = T(g, "blk.%d.attn_k_norm.weight", l, NULL);
        float *panorm = T(g, "blk.%d.post_attention_norm.weight", l, NULL);
        float *fnorm = T(g, "blk.%d.ffn_norm.weight", l, NULL);
        float *Wg = T(g, "blk.%d.ffn_gate.weight", l, NULL);
        float *Wu = T(g, "blk.%d.ffn_up.weight", l, NULL);
        float *Wd = T(g, "blk.%d.ffn_down.weight", l, NULL);
        float *pfnorm = T(g, "blk.%d.post_ffw_norm.weight", l, NULL);
        if (!anorm || !Wq || !Wk || !Wo || !fnorm || !Wg || !Wu || !Wd || !qn) {
            fprintf(stderr, "layer %d missing tensor\n", l); return 1;
        }
        /* derive per-layer geometry from tensor shapes */
        int hd = hdn;                 /* q/k head width (= attn_q_norm size): 256 swa / 512 global */
        int qdim = qnum / D, nq = qdim / hd;
        int kdim = knum / D, nk = kdim / hd;
        int tied = (Wv == NULL);      /* global layers ship no V -> tie to raw K */
        int vhd = hd, nv = tied ? nk : (vnum / D) / hd;
        int kvstride = nk * hd;       /* per-token K cache stride */
        int vstride  = nv * vhd;      /* per-token V cache stride */
        float rbase = (hd >= 512) ? 1000000.0f : 10000.0f;   /* global vs swa rope base */
        float scale = 1.0f / sqrtf((float)hd);

        for (int t = 0; t < n; t++) {
            rmsnorm(h, x + (size_t)t * D, anorm, D, eps);
            matvec(q, Wq, h, qdim, D);
            matvec(kk, Wk, h, kdim, D);
            if (tied) memcpy(vv, kk, (size_t)kdim * sizeof(float));  /* V = raw K projection */
            else matvec(vv, Wv, h, nv * vhd, D);
            for (int hh = 0; hh < nq; hh++) {
                float *qh = q + hh * hd;
                { float tmp[512]; rmsnorm(tmp, qh, qn, hd, eps); memcpy(qh, tmp, hd * sizeof(float)); }
                rope(qh, hd, t, rbase);
            }
            for (int hh = 0; hh < nk; hh++) {
                float *khh = kk + hh * hd;
                if (kn) { float tmp[512]; rmsnorm(tmp, khh, kn, hd, eps); memcpy(khh, tmp, hd * sizeof(float)); }
                rope(khh, hd, t, rbase);
            }
            memcpy(kcache + (size_t)t * kvstride, kk, (size_t)kvstride * sizeof(float));
            memcpy(vcache + (size_t)t * vstride,  vv, (size_t)vstride  * sizeof(float));
            for (int hh = 0; hh < nq; hh++) {
                int kvh = hh / (nq / nk);
                float *qh = q + hh * hd;
                float mx = -1e30f;
                for (int j = 0; j <= t; j++) {
                    const float *kj = kcache + (size_t)j * kvstride + kvh * hd;
                    double a = 0; for (int d = 0; d < hd; d++) a += (double)qh[d] * kj[d];
                    sc[j] = (float)a * scale; if (sc[j] > mx) mx = sc[j];
                }
                float sum = 0; for (int j = 0; j <= t; j++) { sc[j] = expf(sc[j] - mx); sum += sc[j]; }
                float *oh = att + hh * vhd; memset(oh, 0, vhd * sizeof(float));
                for (int j = 0; j <= t; j++) {
                    float w = sc[j] / sum;
                    const float *vj = vcache + (size_t)j * vstride + kvh * vhd;
                    for (int d = 0; d < vhd; d++) oh[d] += w * vj[d];
                }
            }
            matvec(proj, Wo, att, D, nq * vhd);
            if (panorm) { float tmp[3840]; rmsnorm(tmp, proj, panorm, D, eps); memcpy(proj, tmp, D * sizeof(float)); }
            for (int i = 0; i < D; i++) x[(size_t)t * D + i] += proj[i];
            /* MLP */
            rmsnorm(h, x + (size_t)t * D, fnorm, D, eps);
            matvec(g1, Wg, h, FF, D);
            matvec(u1, Wu, h, FF, D);
            for (int i = 0; i < FF; i++) { float z = g1[i]; g1[i] = z / (1.0f + expf(-z)) * u1[i]; }
            matvec(proj, Wd, g1, D, FF);
            if (pfnorm) { float tmp[3840]; rmsnorm(tmp, proj, pfnorm, D, eps); memcpy(proj, tmp, D * sizeof(float)); }
            for (int i = 0; i < D; i++) x[(size_t)t * D + i] += proj[i];
        }
        free(anorm); free(Wq); free(Wk); free(Wv); free(Wo); free(qn); free(kn);
        free(panorm); free(fnorm); free(Wg); free(Wu); free(Wd); free(pfnorm);
        fprintf(stderr, "layer %d done, x[last][0:3]=%.3f %.3f %.3f\n", l,
                x[(size_t)(n-1)*D+0], x[(size_t)(n-1)*D+1], x[(size_t)(n-1)*D+2]);
    }
    /* final norm + logits from last token (tied embedding head) */
    rmsnorm(h, x + (size_t)(n - 1) * D, onorm, D, eps);
    float *logits = malloc((size_t)V * sizeof(float));
    matvec(logits, emb, h, V, D);
    printf("TOP");
    for (int r = 0; r < 10; r++) {
        int best = 0; for (int v = 1; v < V; v++) if (logits[v] > logits[best]) best = v;
        printf(" %d", best); logits[best] = -1e30f;
    }
    printf("\n");
    return 0;
}
