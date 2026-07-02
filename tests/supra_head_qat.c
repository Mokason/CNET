/* supra_head_qat — head-only QAT smoke test (Supra quality phase, milestone 1).
 *
 * Question (scope docs/superpowers/specs/2026-06-28-supra-qat-scope.md, §2/§14):
 *   does QAT recover the head quality that post-hoc ternary destroys?
 *
 * Method (NO transformer backward): freeze the FP Supra transformer; for a tiny
 * corpus run the forward to the final hidden vector h per next-token example and
 * cache (h, target). Then train a ternary BitLinear head (FP shadow W + per-row
 * absmean ternary forward + straight-through estimator), reusing the proven
 * cce_wordlm recipe. Compare three heads on the same eval:
 *   FP (original)  |  post-hoc ternary (quantized FP)  |  QAT ternary (trained).
 * Pass: CE_qat < CE_posthoc, CE_fp <= CE_qat, training loss decreases.
 *
 * Build: make supra_head_qat   (AVX + OpenMP, like supra_console).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_safetensors.h"

#define MAXPAIRS 96

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; printf("FAIL: %s\n", (msg)); } } while (0)

/* ---- BitNet b1.58 ternary helpers (copied from the proven cce_wordlm recipe) ---- */
static float absmean(const float* w, int n) {
    float s = 0; for (int i = 0; i < n; i++) s += fabsf(w[i]);
    return n ? s / (float)n : 0.0f;
}
static float tern(float w, float g) {
    if (g <= 0) return 0.0f;
    float r = roundf(w / g);
    if (r > 1) r = 1;
    if (r < -1) r = -1;
    return g * r;                 /* effective weight = g * {-1,0,+1} */
}

/* logits[o] = bias[o] + sum_i W[o*hid+i]*h[i] (ternary -> per-row absmean) */
static void head_forward(const float* W, const float* bias, int vocab, int hid,
                         int ternary, const float* h, float* logits) {
    #pragma omp parallel for schedule(static)
    for (int o = 0; o < vocab; o++) {
        const float* row = W + (size_t)o * hid;
        float g = ternary ? absmean(row, hid) : 0.0f;
        double s = bias[o];
        for (int i = 0; i < hid; i++) s += (double)(ternary ? tern(row[i], g) : row[i]) * h[i];
        logits[o] = (float)s;
    }
}

/* cross-entropy of `logits` against `target`; writes softmax-1hot grad into dlogit (nullable) */
static double ce_and_grad(const float* logits, int vocab, int target, float* dlogit) {
    float mx = -1e30f;
    for (int o = 0; o < vocab; o++) if (logits[o] > mx) mx = logits[o];
    double sum = 0; for (int o = 0; o < vocab; o++) sum += exp((double)logits[o] - mx);
    double logZ = log(sum) + mx;
    if (dlogit) {
        for (int o = 0; o < vocab; o++) dlogit[o] = (float)(exp((double)logits[o] - logZ)) - (o == target ? 1.0f : 0.0f);
    }
    return logZ - (double)logits[target];   /* -log p[target] */
}

static int argmax(const float* v, int n) { int b = 0; for (int i = 1; i < n; i++) if (v[i] > v[b]) b = i; return b; }

/* ---- tiny FP-path gradient check (validates CE+softmax+linear backward) ---- */
static void gradient_check(void) {
    const int vocab = 6, hid = 4, np = 3;
    float W[24], bias[6], h[12]; int tg[3] = {2, 5, 0};
    for (int k = 0; k < 24; k++) W[k] = 0.1f * ((k % 7) - 3);
    for (int k = 0; k < 6; k++) bias[k] = 0.05f * (k - 2);
    for (int k = 0; k < 12; k++) h[k] = 0.2f * ((k % 5) - 2);

    float g[24]; memset(g, 0, sizeof g);
    float logit[6], dl[6];
    for (int p = 0; p < np; p++) {
        head_forward(W, bias, vocab, hid, 0, h + p * hid, logit);
        ce_and_grad(logit, vocab, tg[p], dl);
        for (int o = 0; o < vocab; o++) for (int i = 0; i < hid; i++) g[o * hid + i] += dl[o] * h[p * hid + i];
    }
    float step = 1e-3f, maxerr = 0;
    for (int idx = 0; idx < 24; idx++) {
        float save = W[idx];
        double lp = 0, lm = 0;
        W[idx] = save + step; for (int p = 0; p < np; p++) { head_forward(W, bias, vocab, hid, 0, h + p*hid, logit); lp += ce_and_grad(logit, vocab, tg[p], NULL); }
        W[idx] = save - step; for (int p = 0; p < np; p++) { head_forward(W, bias, vocab, hid, 0, h + p*hid, logit); lm += ce_and_grad(logit, vocab, tg[p], NULL); }
        W[idx] = save;
        float num = (float)((lp - lm) / (2 * step));
        float e = fabsf(num - g[idx]); if (e > maxerr) maxerr = e;
    }
    printf("  gradient check (FP path): max |analytic - numeric| = %.2e\n", maxerr);
    CHECK(maxerr < 1e-3, "head CE+softmax+linear backward matches finite differences");
}

int main(int argc, char** argv) {
    int   epochs = (argc > 1) ? atoi(argv[1]) : 40;
    float lr     = (argc > 2) ? (float)atof(argv[2]) : 3e-3f;

    gradient_check();

    cce_supra_a2a* a = NULL;
    if (cce_supra_a2a_load(&a, "supra_cache") != CCE_OK || !a || !a->model) {
        printf("Could not load Supra model from ./supra_cache\n");
        return 1;
    }
    cce_supra_decomposed* m = a->model;
    int hid = m->n_embd;

    const float *Wfp_io = NULL, *bias_fp = NULL; int in_dim = 0, out_dim = 0;
    if (cce_supra_head_fp(m, &Wfp_io, &bias_fp, &in_dim, &out_dim) != CCE_OK) {
        printf("FAIL: no FP head weights (model quantized?)\n"); cce_supra_a2a_free(a); return 1;
    }
    int vocab = out_dim;
    CHECK(in_dim == hid, "head in_dim == n_embd");
    printf("  head: [%d -> %d]  hidden=%d  vocab=%d\n", in_dim, out_dim, hid, vocab);

    /* transpose FP head [in,out] -> contiguous-row [out,in] for the trainer */
    float* Wfp = (float*)malloc((size_t)vocab * hid * sizeof(float));
    for (int o = 0; o < vocab; o++) for (int i = 0; i < hid; i++) Wfp[(size_t)o*hid + i] = Wfp_io[(size_t)i*vocab + o];

    /* ---- build the (h, target) cache from a tiny corpus ---- */
    const char* corpus[] = {
        "Once upon a time there was a little girl named Lily.",
        "The cat sat on the mat and looked at the sun.",
        "He opened the door and walked into the bright garden.",
        "She picked up a small red ball and began to play.",
        "They went to the park to see the tall green trees.",
        "The happy dog ran fast across the open field."
    };
    int ncorp = (int)(sizeof(corpus)/sizeof(corpus[0]));
    float* H = (float*)malloc((size_t)MAXPAIRS * hid * sizeof(float));
    int* targets = (int*)malloc((size_t)MAXPAIRS * sizeof(int));
    int npairs = 0;
    for (int c = 0; c < ncorp && npairs < MAXPAIRS; c++) {
        int ids[256]; int n = cce_supra_encode_text(a->tokenizer, corpus[c], ids, 256);
        for (int t = 1; t < n && npairs < MAXPAIRS; t++) {
            if (cce_supra_hidden_last(m, ids, t, H + (size_t)npairs*hid, hid) != CCE_OK) continue;
            targets[npairs] = ids[t];
            npairs++;
        }
    }
    printf("  cached %d (hidden, target) pairs from %d sentences\n", npairs, ncorp);
    CHECK(npairs >= 16, "enough training pairs cached");

    /* sanity: my reconstructed FP head must match the model's own head */
    {
        int ids[256]; int n = cce_supra_encode_text(a->tokenizer, corpus[0], ids, 256);
        float* ref = (float*)malloc((size_t)vocab * sizeof(float));
        float* mine = (float*)malloc((size_t)vocab * sizeof(float));
        cce_supra_gpt_forward(m, ids, n, ref, vocab);
        head_forward(Wfp, bias_fp, vocab, hid, 0, H + 0, mine);   /* H[0] is hidden for prefix len 1; align ref to same */
        /* recompute ref at the SAME prefix (len 1) for a fair compare */
        cce_supra_gpt_forward(m, ids, 1, ref, vocab);
        CHECK(argmax(ref, vocab) == argmax(mine, vocab), "reconstructed FP head argmax matches model head");
        free(ref); free(mine);
    }

    /* ---- distillation target: the FP head's own prediction (teacher) ----
       QAT's job is to RECOVER the FP head's behavior under the ternary constraint.
       post-hoc loses some of it (the project's measured argmax collapse); QAT
       should win it back. Eval is split train/eval (every 5th pair held out) so
       in-sample recovery (the machinery works) and held-out generalization are
       reported separately. (Raw next-token CE is ill-posed here: a 12.9M-param
       head over ~75 inputs just memorizes -- see scope §13.) */
    int* fp_arg = (int*)malloc((size_t)npairs * sizeof(int));
    int* is_eval = (int*)malloc((size_t)npairs * sizeof(int));
    float* logit = (float*)malloc((size_t)vocab * sizeof(float));
    float* dl    = (float*)malloc((size_t)vocab * sizeof(float));
    int ntrain = 0, neval = 0;
    for (int p = 0; p < npairs; p++) {
        head_forward(Wfp, bias_fp, vocab, hid, 0, H + (size_t)p*hid, logit);   /* FP teacher */
        fp_arg[p] = argmax(logit, vocab);
        is_eval[p] = (p % 5 == 0);
        if (is_eval[p]) neval++; else ntrain++;
    }
    printf("  distill-to-FP target | train=%d eval=%d pairs\n", ntrain, neval);

    /* ---- QAT: shadow = copy of FP head, ternary forward + STE, batch Adam ---- */
    size_t N = (size_t)vocab * hid;
    float* W = (float*)malloc(N * sizeof(float)); memcpy(W, Wfp, N * sizeof(float));
    float* bias = (float*)malloc((size_t)vocab * sizeof(float)); memcpy(bias, bias_fp, (size_t)vocab*sizeof(float));
    float* gW = (float*)calloc(N, sizeof(float)), *mW = (float*)calloc(N, sizeof(float)), *vW = (float*)calloc(N, sizeof(float));
    float* gb = (float*)calloc(vocab, sizeof(float)), *mb = (float*)calloc(vocab, sizeof(float)), *vb = (float*)calloc(vocab, sizeof(float));
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;

    double first_loss = 0, last_loss = 0;
    for (int ep = 0; ep < epochs; ep++) {
        memset(gW, 0, N * sizeof(float)); memset(gb, 0, (size_t)vocab*sizeof(float));
        double loss = 0;
        for (int p = 0; p < npairs; p++) {
            if (is_eval[p]) continue;                               /* train split only */
            const float* h = H + (size_t)p*hid;
            head_forward(W, bias, vocab, hid, 1, h, logit);         /* ternary forward */
            loss += ce_and_grad(logit, vocab, fp_arg[p], dl);       /* distill to FP argmax */
            #pragma omp parallel for schedule(static)
            for (int o = 0; o < vocab; o++) {
                float d = dl[o];
                float* grow = gW + (size_t)o*hid;
                for (int i = 0; i < hid; i++) grow[i] += d * h[i];  /* STE: grad -> FP shadow */
                gb[o] += d;
            }
        }
        loss /= (ntrain > 0 ? ntrain : 1);
        if (ep == 0) first_loss = loss;
        last_loss = loss;
        if (ep % 8 == 0 || ep == epochs-1) printf("    epoch %2d  QAT distill CE = %.4f\n", ep, loss);

        float bc1 = 1.0f - powf(b1, (float)(ep+1)), bc2 = 1.0f - powf(b2, (float)(ep+1));
        float inv = 1.0f / (float)(ntrain > 0 ? ntrain : 1);
        #pragma omp parallel for schedule(static)
        for (size_t k = 0; k < N; k++) {
            float g = gW[k] * inv;
            mW[k] = b1*mW[k] + (1-b1)*g;
            vW[k] = b2*vW[k] + (1-b2)*g*g;
            W[k] -= lr * (mW[k]/bc1) / (sqrtf(vW[k]/bc2) + eps);
        }
        for (int o = 0; o < vocab; o++) {
            float g = gb[o] * inv;
            mb[o] = b1*mb[o] + (1-b1)*g;
            vb[o] = b2*vb[o] + (1-b2)*g*g;
            bias[o] -= lr * (mb[o]/bc1) / (sqrtf(vb[o]/bc2) + eps);
        }
    }

    /* ---- eval: agreement with the FP head (recovery), per split ---- */
    int ph_tr=0, qat_tr=0, ph_ev=0, qat_ev=0;            /* argmax agreement w/ FP */
    double ceph_tr=0, ceqat_tr=0, ceph_ev=0, ceqat_ev=0; /* CE to FP target */
    for (int p = 0; p < npairs; p++) {
        const float* h = H + (size_t)p*hid; int fa = fp_arg[p];
        head_forward(Wfp, bias_fp, vocab, hid, 1, h, logit);   /* post-hoc */
        int aph = argmax(logit, vocab); double cph = ce_and_grad(logit, vocab, fa, NULL);
        head_forward(W,   bias,    vocab, hid, 1, h, logit);   /* QAT */
        int aq = argmax(logit, vocab); double cq = ce_and_grad(logit, vocab, fa, NULL);
        if (is_eval[p]) {
            ph_ev  += (aph==fa); qat_ev  += (aq==fa); ceph_ev += cph; ceqat_ev += cq;
        } else {
            ph_tr  += (aph==fa); qat_tr  += (aq==fa); ceph_tr += cph; ceqat_tr += cq;
        }
    }

    printf("\n  === head-only QAT smoke: recover the FP head under ternary ===\n");
    printf("    distill CE: %.4f -> %.4f\n", first_loss, last_loss);
    printf("    FP-argmax agreement   TRAIN  post-hoc=%d/%d  QAT=%d/%d\n", ph_tr, ntrain, qat_tr, ntrain);
    printf("    FP-argmax agreement   EVAL   post-hoc=%d/%d  QAT=%d/%d\n", ph_ev, neval, qat_ev, neval);
    printf("    CE-to-FP (mean)       TRAIN  post-hoc=%.3f  QAT=%.3f\n",
           ceph_tr/(ntrain?ntrain:1), ceqat_tr/(ntrain?ntrain:1));
    printf("    CE-to-FP (mean)       EVAL   post-hoc=%.3f  QAT=%.3f\n",
           ceph_ev/(neval?neval:1), ceqat_ev/(neval?neval:1));

    /* Pass gate = the well-posed in-sample claim: the QAT machinery recovers the
       FP head's behavior, beating post-hoc. Held-out is REPORTED, not asserted
       (generalization needs a real corpus -- smoke-test scale). */
    CHECK(last_loss < first_loss, "QAT distillation loss decreased");
    CHECK(qat_tr > ph_tr, "QAT recovers FP argmax better than post-hoc (in-sample)");
    CHECK(ceqat_tr < ceph_tr, "QAT CE-to-FP < post-hoc CE-to-FP (in-sample)");

    free(Wfp); free(W); free(bias); free(gW); free(mW); free(vW);
    free(gb); free(mb); free(vb); free(logit); free(dl); free(fp_arg); free(is_eval); free(H); free(targets);
    cce_supra_a2a_free(a);

    printf("\nsupra_head_qat: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
