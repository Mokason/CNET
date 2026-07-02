/* supra_head_qat_corpus — head-only QAT on a REAL corpus (pdf_corpus.txt), the
 * follow-up to the tiny in-sample smoke. Tests genuine HELD-OUT recovery: does a
 * QAT-trained ternary head reproduce the FP head's behavior on sentences it never
 * trained on, better than post-hoc ternary?
 *
 * Whole sentences are held out (every 8th) so eval pairs come from unseen text.
 * One forward per sentence yields a hidden at every position (cce_supra_hidden_all),
 * so thousands of (hidden, FP-argmax) distillation pairs are cheap to cache.
 *
 * Build: make supra_head_qat_corpus [pairs] [epochs] [lr]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_safetensors.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; printf("FAIL: %s\n", (msg)); } } while (0)

static float absmean(const float* w, int n) {
    float s = 0; for (int i = 0; i < n; i++) s += fabsf(w[i]);
    return n ? s / (float)n : 0.0f;
}
static float tern(float w, float g) {
    if (g <= 0) return 0.0f;
    float r = roundf(w / g);
    if (r > 1) r = 1;
    if (r < -1) r = -1;
    return g * r;
}
/* Quantize the whole weight matrix to its ternary effective weights ONCE
   (Weff[o,i] = tern(W[o,i], absmean(row_o))). The ternary weights change only
   per epoch (after Adam), so doing this once instead of per-example removes the
   non-vectorizable roundf/absmean from the hot loop -- bit-equivalent to the old
   inline ternary forward, ~order-of-magnitude faster. */
static void ternize_into(const float* W, int vocab, int hid, float* Weff) {
    #pragma omp parallel for schedule(static)
    for (int o = 0; o < vocab; o++) {
        const float* row = W + (size_t)o * hid;
        float g = absmean(row, hid);
        float* dst = Weff + (size_t)o * hid;
        for (int i = 0; i < hid; i++) dst[i] = tern(row[i], g);
    }
}
/* Plain dense matvec: logits[o] = bias[o] + sum_i Wm[o*hid+i]*h[i]. With Wm =
   ternized weights this is the ternary forward; with FP weights, the FP forward.
   Float accumulation over hid=256 is AVX-friendly and accurate enough for logits. */
static void head_matvec(const float* Wm, const float* bias, int vocab, int hid,
                        const float* h, float* logits) {
    #pragma omp parallel for schedule(static)
    for (int o = 0; o < vocab; o++) {
        const float* row = Wm + (size_t)o * hid;
        float s = bias[o];
        for (int i = 0; i < hid; i++) s += row[i] * h[i];
        logits[o] = s;
    }
}
static int argmax(const float* v, int n) { int b = 0; for (int i = 1; i < n; i++) if (v[i] > v[b]) b = i; return b; }

/* temperature-scaled softmax: probs = softmax(logits / T) */
static void softmax_T(const float* logits, int vocab, float T, float* probs) {
    float mx = -1e30f; for (int o = 0; o < vocab; o++) { float z = logits[o]/T; if (z > mx) mx = z; }
    double sum = 0; for (int o = 0; o < vocab; o++) { probs[o] = (float)exp((double)logits[o]/T - mx); sum += probs[o]; }
    float inv = (float)(1.0 / (sum > 0 ? sum : 1));
    for (int o = 0; o < vocab; o++) probs[o] *= inv;
}
/* Hinton soft KD at temperature T: loss = CE(tprob, softmax(student/T));
   grad wrt student logit = T*(softmax(student/T) - tprob) (the T^2 scaling folded
   in so gradient magnitude is comparable across T). tprob = softmax(teacher/T). */
static double kd_grad_T(const float* student, const float* tprob, int vocab, float T, float* dlogit) {
    float mx = -1e30f; for (int o = 0; o < vocab; o++) { float z = student[o]/T; if (z > mx) mx = z; }
    double sum = 0; for (int o = 0; o < vocab; o++) sum += exp((double)student[o]/T - mx);
    double logZ = log(sum) + mx, ce = 0;
    for (int o = 0; o < vocab; o++) {
        double q = exp((double)student[o]/T - logZ);
        dlogit[o] = (float)(T * (q - tprob[o]));
        if (tprob[o] > 0) ce -= (double)tprob[o] * log(q > 1e-30 ? q : 1e-30);
    }
    return ce;
}

int main(int argc, char** argv) {
    int   max_pairs = (argc > 1) ? atoi(argv[1]) : 1500;
    int   epochs    = (argc > 2) ? atoi(argv[2]) : 12;
    float lr        = (argc > 3) ? (float)atof(argv[3]) : 3e-3f;
    float T         = (argc > 4) ? (float)atof(argv[4]) : 1.0f;   /* KD temperature */
    if (T < 0.1f) T = 0.1f;
    printf("config: pairs<=%d epochs=%d lr=%g T=%.1f\n", max_pairs, epochs, lr, T);

    cce_supra_a2a* a = NULL;
    if (cce_supra_a2a_load(&a, "supra_cache") != CCE_OK || !a || !a->model) {
        printf("Could not load Supra model from ./supra_cache\n"); return 1;
    }
    cce_supra_decomposed* m = a->model;
    int hid = m->n_embd, bs = m->block_size;

    const float *Wfp_io = NULL, *bias_fp = NULL; int in_dim = 0, vocab = 0;
    if (cce_supra_head_fp(m, &Wfp_io, &bias_fp, &in_dim, &vocab) != CCE_OK) {
        printf("FAIL: no FP head weights\n"); cce_supra_a2a_free(a); return 1;
    }
    float* Wfp = (float*)malloc((size_t)vocab * hid * sizeof(float));
    for (int o = 0; o < vocab; o++) for (int i = 0; i < hid; i++)
        Wfp[(size_t)o*hid + i] = Wfp_io[(size_t)i*vocab + o];

    /* ---- cache (hidden, target) pairs from the real corpus; hold out whole sentences ---- */
    FILE* f = fopen("pdf_corpus.txt", "rb");
    if (!f) { printf("FAIL: pdf_corpus.txt not found (run the PDF pipeline first)\n"); cce_supra_a2a_free(a); return 1; }

    float* H = (float*)malloc((size_t)max_pairs * hid * sizeof(float));
    int*   tgt = (int*)malloc((size_t)max_pairs * sizeof(int));
    int*   is_eval = (int*)malloc((size_t)max_pairs * sizeof(int));
    float* Hbuf = (float*)malloc((size_t)bs * hid * sizeof(float));
    char   line[8192];
    int    npairs = 0, sent = 0, sent_used = 0;

    while (npairs < max_pairs && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if ((int)strlen(line) < 12) continue;            /* skip very short lines */
        int ids[256];
        int n = cce_supra_encode_text(a->tokenizer, line, ids, 256);
        if (n > bs) n = bs;
        if (n < 3) continue;
        if (cce_supra_hidden_all(m, ids, n, Hbuf, bs * hid) != CCE_OK) continue;
        int held = (sent % 8 == 0);                      /* hold out whole sentences */
        for (int t = 0; t < n - 1 && npairs < max_pairs; t++) {
            memcpy(H + (size_t)npairs*hid, Hbuf + (size_t)t*hid, (size_t)hid*sizeof(float));
            tgt[npairs] = ids[t + 1];
            is_eval[npairs] = held;
            npairs++;
        }
        sent++; sent_used++;
    }
    fclose(f);
    printf("cached %d pairs from %d sentences | head [%d->%d] hid=%d\n", npairs, sent_used, in_dim, vocab, hid);
    CHECK(npairs >= 200, "enough corpus pairs cached");

    /* distillation target = FP head SOFT distribution. Teacher is frozen, so
       compute each pair's soft target ONCE and cache it (no teacher forward per
       epoch). fp_arg = FP argmax = the recovery metric. */
    float* logit = (float*)malloc((size_t)vocab * sizeof(float));
    float* dl    = (float*)malloc((size_t)vocab * sizeof(float));
    float* tlog  = (float*)malloc((size_t)vocab * sizeof(float));   /* fallback teacher buf */
    float* tprob = (float*)malloc((size_t)vocab * sizeof(float));
    int* fp_arg = (int*)malloc((size_t)npairs * sizeof(int));
    size_t tc_bytes = (size_t)npairs * vocab * sizeof(float);
    float* tcache = (tc_bytes < (size_t)1500*1024*1024) ? (float*)malloc(tc_bytes) : NULL;
    printf("teacher cache: %s (%.0f MB)\n", tcache ? "on" : "off (recompute)", tc_bytes / 1048576.0);

    int ntrain = 0, neval = 0;
    for (int p = 0; p < npairs; p++) {
        head_matvec(Wfp, bias_fp, vocab, hid, H + (size_t)p*hid, logit);   /* FP teacher logits */
        fp_arg[p] = argmax(logit, vocab);
        if (tcache) softmax_T(logit, vocab, T, tcache + (size_t)p*vocab);  /* cache soft target */
        if (is_eval[p]) neval++; else ntrain++;
    }
    printf("train=%d  held-out=%d pairs (whole-sentence split)\n", ntrain, neval);
    CHECK(neval >= 20, "enough held-out pairs");

    /* ---- QAT: shadow = FP head; ternize once/epoch + vectorized matvec; STE; batch Adam ---- */
    size_t N = (size_t)vocab * hid;
    float* W = (float*)malloc(N*sizeof(float)); memcpy(W, Wfp, N*sizeof(float));
    float* Weff = (float*)malloc(N*sizeof(float));    /* ternized weights, rebuilt per epoch */
    float* bias = (float*)malloc((size_t)vocab*sizeof(float)); memcpy(bias, bias_fp, (size_t)vocab*sizeof(float));
    float* gW=(float*)calloc(N,sizeof(float)),*mW=(float*)calloc(N,sizeof(float)),*vW=(float*)calloc(N,sizeof(float));
    float* gb=(float*)calloc(vocab,sizeof(float)),*mb=(float*)calloc(vocab,sizeof(float)),*vb=(float*)calloc(vocab,sizeof(float));
    const float b1=0.9f,b2=0.999f,eps=1e-8f;

    double first_loss=0,last_loss=0;
    for (int ep=0; ep<epochs; ep++) {
        ternize_into(W, vocab, hid, Weff);            /* quantize ONCE this epoch */
        memset(gW,0,N*sizeof(float)); memset(gb,0,(size_t)vocab*sizeof(float));
        double loss=0;
        for (int p=0;p<npairs;p++) {
            if (is_eval[p]) continue;
            const float* h=H+(size_t)p*hid;
            const float* tp;
            if (tcache) tp = tcache + (size_t)p*vocab;
            else { head_matvec(Wfp, bias_fp, vocab, hid, h, tlog); softmax_T(tlog, vocab, T, tprob); tp = tprob; }
            head_matvec(Weff, bias, vocab, hid, h, logit);        /* student ternary forward (fast) */
            loss += kd_grad_T(logit, tp, vocab, T, dl);           /* soft KD @ T */
            #pragma omp parallel for schedule(static)
            for (int o=0;o<vocab;o++){ float d=dl[o]; float* gr=gW+(size_t)o*hid; for(int i=0;i<hid;i++) gr[i]+=d*h[i]; gb[o]+=d; }
        }
        loss /= (ntrain?ntrain:1);
        if (ep==0) first_loss=loss;
        last_loss=loss;
        if (ep%4==0 || ep==epochs-1) printf("    epoch %2d  QAT distill CE = %.4f\n", ep, loss);
        float bc1=1.0f-powf(b1,(float)(ep+1)), bc2=1.0f-powf(b2,(float)(ep+1)), inv=1.0f/(float)(ntrain?ntrain:1);
        #pragma omp parallel for schedule(static)
        for (size_t k=0;k<N;k++){ float g=gW[k]*inv; mW[k]=b1*mW[k]+(1-b1)*g; vW[k]=b2*vW[k]+(1-b2)*g*g; W[k]-=lr*(mW[k]/bc1)/(sqrtf(vW[k]/bc2)+eps); }
        for (int o=0;o<vocab;o++){ float g=gb[o]*inv; mb[o]=b1*mb[o]+(1-b1)*g; vb[o]=b2*vb[o]+(1-b2)*g*g; bias[o]-=lr*(mb[o]/bc1)/(sqrtf(vb[o]/bc2)+eps); }
    }

    /* ---- eval: recover FP argmax, per split, post-hoc vs QAT (ternize each once) ---- */
    int ph_tr=0,qat_tr=0,ph_ev=0,qat_ev=0;
    ternize_into(Wfp, vocab, hid, Weff);              /* post-hoc effective weights */
    for (int p=0;p<npairs;p++){ head_matvec(Weff,bias_fp,vocab,hid,H+(size_t)p*hid,logit); int a=argmax(logit,vocab);
        if (is_eval[p]) ph_ev+=(a==fp_arg[p]); else ph_tr+=(a==fp_arg[p]); }
    ternize_into(W, vocab, hid, Weff);                /* QAT effective weights */
    for (int p=0;p<npairs;p++){ head_matvec(Weff,bias,vocab,hid,H+(size_t)p*hid,logit); int a=argmax(logit,vocab);
        if (is_eval[p]) qat_ev+=(a==fp_arg[p]); else qat_tr+=(a==fp_arg[p]); }
    double phs_ev = neval?100.0*ph_ev/neval:0, qs_ev = neval?100.0*qat_ev/neval:0;
    double phs_tr = ntrain?100.0*ph_tr/ntrain:0, qs_tr = ntrain?100.0*qat_tr/ntrain:0;

    printf("\n  === head QAT on real corpus: FP-argmax recovery ===\n");
    printf("    distill CE: %.4f -> %.4f\n", first_loss, last_loss);
    printf("    TRAIN     post-hoc=%.1f%%  QAT=%.1f%%\n", phs_tr, qs_tr);
    printf("    HELD-OUT  post-hoc=%.1f%%  QAT=%.1f%%   <-- generalization test\n", phs_ev, qs_ev);
    printf("    VERDICT: QAT %s post-hoc on UNSEEN sentences (%.1f%% vs %.1f%%)\n",
           qs_ev > phs_ev ? "BEATS" : (qs_ev == phs_ev ? "ties" : "loses to"), qs_ev, phs_ev);

    CHECK(last_loss < first_loss, "QAT distillation loss decreased");
    CHECK(qat_tr > ph_tr, "QAT recovers FP argmax better than post-hoc (in-sample)");
    CHECK(qat_ev >= ph_ev, "QAT generalizes: held-out recovery >= post-hoc");

    free(Wfp);free(W);free(Weff);free(bias);free(gW);free(mW);free(vW);free(gb);free(mb);free(vb);
    free(logit);free(dl);free(tlog);free(tprob);free(tcache);free(fp_arg);free(H);free(tgt);free(is_eval);free(Hbuf);
    cce_supra_a2a_free(a);

    printf("\nsupra_head_qat_corpus: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
