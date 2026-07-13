/* transformer_qat_real — plumb the REAL pretrained Supra weights into the
 * cce_transformer_qat QAT trainer, PROVE the plumbing via forward-parity against the
 * verified cce_supra_gpt_forward, then run a joint-QAT held-out generalization
 * experiment on the real weights.
 *
 * Task 1: cce_transformer_qat_load_decomposed copies every real weight/bias into the
 *         matching trainer P (direct [in][out] memcpy, no transpose).
 * Task 2 (the gate): FP-mode trainer logits must match cce_supra_gpt_forward
 *         (top-1 argmax identical on every sequence; max|Δlogit| small).
 * Task 3: joint QAT on the transformer blocks (qkv/proj/mlp ternary, head+emb FP)
 *         vs post-hoc ternary vs FP, held-out next-token argmax accuracy.
 *
 * Build/run:  make transformer_qat_real   (logs to logs/transformer_qat_real.log)
 *             optional args: [train_pairs] [eval_pairs] [epochs] [lr]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../include/cce/cce_safetensors.h"
#include "../include/cce/cce_transformer_qat.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("PASS: %s\n", (msg)); } \
                           else { g_fail++; printf("FAIL: %s\n", (msg)); } } while (0)

static int argmax(const float* v, int n) {
    int b = 0; for (int i = 1; i < n; i++) if (v[i] > v[b]) b = i; return b;
}

/* Discover the MLP hidden width from the real up-projection cascade. */
static int discover_mlp_hidden(cce_supra_decomposed* m) {
    cce_cascade* up = cce_forest_get_resident(m->forest, "gpt.block0.mlp_up");
    if (!up || up->num_blocks < 1) return -1;
    return up->blocks[up->num_blocks - 1].weights.shape[1];
}

int main(int argc, char** argv) {
    int   max_train = (argc > 1) ? atoi(argv[1]) : 120;
    int   max_eval  = (argc > 2) ? atoi(argv[2]) : 60;
    int   epochs    = (argc > 3) ? atoi(argv[3]) : 3;
    float lr        = (argc > 4) ? (float)atof(argv[4]) : 1e-3f;
    const int PREFIX_CAP = 48;   /* cap prefix length so steps stay cheap */

    /* ---------- load the real Supra model ---------- */
    cce_supra_a2a* a = NULL;
    if (cce_supra_a2a_load(&a, "supra_cache") != CCE_OK || !a || !a->model) {
        printf("FAIL: could not load Supra model from ./supra_cache\n");
        return 1;
    }
    cce_supra_decomposed* m = a->model;
    int D = m->n_embd, V = m->vocab_size, B = m->block_size, H = m->n_head, L = m->n_layer;
    int Mh = discover_mlp_hidden(m);
    printf("real model: n_layer=%d n_embd=%d n_head=%d mlp_hidden=%d vocab=%d block_size=%d\n",
           L, D, H, Mh, V, B);
    if (Mh <= 0) { printf("FAIL: could not read mlp_hidden\n"); cce_supra_a2a_free(a); return 1; }

    /* ---------- Task 1: create trainer with matching dims + load real weights ---------- */
    cce_transformer_qat_config cfg = {0};
    cfg.n_layer = L; cfg.n_embd = D; cfg.n_head = H; cfg.mlp_hidden = Mh;
    cfg.vocab = V; cfg.block_size = B; cfg.seed = 1234;
    cfg.qat_qkv = cfg.qat_proj = cfg.qat_mlp = cfg.qat_head = cfg.qat_emb = 0;

    cce_transformer_qat* t = cce_transformer_qat_create(&cfg);
    if (!t) { printf("FAIL: trainer create\n"); cce_supra_a2a_free(a); return 1; }

    cce_result lrc = cce_transformer_qat_load_decomposed(t, m);
    CHECK(lrc == CCE_OK, "cce_transformer_qat_load_decomposed returned CCE_OK");
    if (lrc != CCE_OK) { printf("  (rc=%d) aborting\n", (int)lrc);
        cce_transformer_qat_free(t); cce_supra_a2a_free(a); return 1; }

    /* ---------- Task 2: forward parity (the proof) ---------- */
    printf("\n=== Task 2: forward parity (FP trainer vs cce_supra_gpt_forward) ===\n");
    float* lt = (float*)malloc((size_t)V * sizeof(float));  /* trainer logits */
    float* lr_ref = (float*)malloc((size_t)V * sizeof(float)); /* reference logits */

    /* parity sequences: encode the first real corpus lines (fallback to id arrays) */
    FILE* pf = fopen("pdf_corpus.txt", "rb");
    int n_seq = 0, argmax_match = 0;
    double global_max_abs = 0.0, global_sum_abs = 0.0; long global_cmp = 0;
    const int MAX_SEQ = 10;
    char line[8192];
    while (pf && n_seq < MAX_SEQ && fgets(line, sizeof line, pf)) {
        line[strcspn(line, "\r\n")] = 0;
        if ((int)strlen(line) < 12) continue;
        int ids[256];
        int n = cce_supra_encode_text(a->tokenizer, line, ids, 256);
        if (n < 2) continue;
        if (n > 24) n = 24;               /* short seqs: parity is length-independent */
        if (n > B) n = B;

        if (cce_transformer_qat_logits(t, ids, n, lt) != CCE_OK) continue;
        if (cce_supra_gpt_forward(m, ids, n, lr_ref, V) != CCE_OK) continue;

        double seq_max = 0.0;
        for (int i = 0; i < V; i++) {
            double d = fabs((double)lt[i] - (double)lr_ref[i]);
            if (d > seq_max) seq_max = d;
            global_sum_abs += d; global_cmp++;
        }
        if (seq_max > global_max_abs) global_max_abs = seq_max;
        int at = argmax(lt, V), ar = argmax(lr_ref, V);
        int match = (at == ar);
        argmax_match += match;
        printf("  seq %d (T=%d): argmax trainer=%d ref=%d %s | max|d|=%.3e | logit[ref]=%.4f\n",
               n_seq, n, at, ar, match ? "OK" : "MISMATCH", seq_max, lr_ref[ar]);
        n_seq++;
    }
    if (pf) fclose(pf);

    /* Fallback if the corpus produced no usable sequences: synthetic id arrays. */
    if (n_seq == 0) {
        int seqs[3][12] = {
            {5, 100, 250, 12, 7, 999, 3, 42, 88, 1, 2, 3},
            {10, 20, 30, 40, 50, 60, 70, 80, 0, 0, 0, 0},
            {1, 1, 2, 3, 5, 8, 13, 21, 0, 0, 0, 0}
        };
        int lens[3] = {12, 8, 8};
        for (int s = 0; s < 3; s++) {
            for (int i = 0; i < lens[s]; i++) if (seqs[s][i] >= V) seqs[s][i] %= V;
            if (cce_transformer_qat_logits(t, seqs[s], lens[s], lt) != CCE_OK) continue;
            if (cce_supra_gpt_forward(m, seqs[s], lens[s], lr_ref, V) != CCE_OK) continue;
            double seq_max = 0.0;
            for (int i = 0; i < V; i++) {
                double d = fabs((double)lt[i] - (double)lr_ref[i]);
                if (d > seq_max) seq_max = d;
                global_sum_abs += d; global_cmp++;
            }
            if (seq_max > global_max_abs) global_max_abs = seq_max;
            int at = argmax(lt, V), ar = argmax(lr_ref, V);
            argmax_match += (at == ar);
            printf("  synth seq %d (T=%d): argmax trainer=%d ref=%d %s | max|d|=%.3e\n",
                   s, lens[s], at, ar, at==ar?"OK":"MISMATCH", seq_max);
            n_seq++;
        }
    }

    printf("\n  parity over %d sequences: argmax matched %d/%d | global max|Dlogit|=%.3e | mean|Dlogit|=%.3e\n",
           n_seq, argmax_match, n_seq, global_max_abs,
           global_cmp ? global_sum_abs / (double)global_cmp : 0.0);
    CHECK(n_seq >= 3, "at least 3 parity sequences evaluated");
    CHECK(argmax_match == n_seq, "top-1 argmax identical on ALL sequences (real weights loaded correctly)");
    CHECK(global_max_abs < 1e-2, "max|Dlogit| < 1e-2 (float-vs-double accumulation only)");

    int parity_ok = (n_seq >= 3 && argmax_match == n_seq);
    printf("  PARITY %s\n", parity_ok ? "PASSED" : "FAILED");

    /* ---------- Task 3: joint QAT held-out generalization on REAL weights ----------
       Blocks (qkv/proj/mlp) go ternary via STE; head+emb stay FP. Whole sentences
       are held out. Metric: next-token argmax accuracy vs the TRUE next token. */
    printf("\n=== Task 3: joint QAT held-out (blocks ternary, head+emb FP) ===\n");
    printf("caps: train_pairs<=%d eval_pairs<=%d epochs=%d lr=%g prefix_cap=%d\n",
           max_train, max_eval, epochs, lr, PREFIX_CAP);

    /* Build (prefix, target) pairs; hold out every 5th sentence whole. Up to 2
       cut points per sentence to get a little signal without O(n) forwards. */
    int cap_pairs = max_train + max_eval + 16;
    int*  pf_len  = (int*)malloc((size_t)cap_pairs * sizeof(int));
    int** pf_ids  = (int**)malloc((size_t)cap_pairs * sizeof(int*));
    int*  pf_tgt  = (int*)malloc((size_t)cap_pairs * sizeof(int));
    int*  pf_eval = (int*)malloc((size_t)cap_pairs * sizeof(int));
    int   npairs = 0, sent = 0, ntrain = 0, neval = 0;

    pf = fopen("pdf_corpus.txt", "rb");
    while (pf && npairs < cap_pairs && fgets(line, sizeof line, pf)) {
        line[strcspn(line, "\r\n")] = 0;
        if ((int)strlen(line) < 12) continue;
        int ids[256];
        int n = cce_supra_encode_text(a->tokenizer, line, ids, 256);
        if (n > B) n = B;
        if (n < 3) continue;
        int held = (sent % 5 == 0);
        int cuts[2]; int ncut = 0;
        cuts[ncut++] = n - 1;                 /* predict last token from full prefix */
        if (n >= 6) cuts[ncut++] = n / 2;     /* predict mid token from first half   */
        for (int ci = 0; ci < ncut && npairs < cap_pairs; ci++) {
            int k = cuts[ci];                 /* prefix ids[0..k-1] -> target ids[k] */
            int plen = k; if (plen > PREFIX_CAP) plen = PREFIX_CAP;
            int start = k - plen;             /* keep the tokens right before target */
            if (held && neval >= max_eval) continue;
            if (!held && ntrain >= max_train) continue;
            int* buf = (int*)malloc((size_t)plen * sizeof(int));
            memcpy(buf, ids + start, (size_t)plen * sizeof(int));
            pf_ids[npairs] = buf; pf_len[npairs] = plen; pf_tgt[npairs] = ids[k];
            pf_eval[npairs] = held;
            if (held) neval++; else ntrain++;
            npairs++;
        }
        sent++;
    }
    if (pf) fclose(pf);
    printf("built %d pairs from %d sentences: train=%d held-out=%d\n", npairs, sent, ntrain, neval);
    CHECK(ntrain >= 20 && neval >= 10, "enough train and held-out pairs");

    /* eval helper: next-token argmax accuracy over held-out pairs (uses current qat mode) */
    #define EVAL_HELDOUT(accvar) do { \
        int correct = 0, tot = 0; \
        for (int p = 0; p < npairs; p++) { \
            if (!pf_eval[p]) continue; \
            if (cce_transformer_qat_logits(t, pf_ids[p], pf_len[p], lt) != CCE_OK) continue; \
            if (argmax(lt, V) == pf_tgt[p]) correct++; \
            tot++; \
        } \
        accvar = tot ? 100.0 * correct / tot : 0.0; \
    } while (0)

    double acc_fp = 0, acc_ph = 0, acc_qat = 0;

    /* FP baseline (no ternary) */
    cce_transformer_qat_set_qat(t, 0, 0, 0, 0, 0);
    EVAL_HELDOUT(acc_fp);

    /* post-hoc ternary (blocks flipped to ternary, NO retrain) */
    cce_transformer_qat_set_qat(t, 1, 1, 1, 0, 0);
    EVAL_HELDOUT(acc_ph);

    /* joint QAT: train the shadow with block ternary forward + STE (head/emb FP) */
    clock_t tic = clock();
    double first_loss = 0, last_loss = 0;
    for (int ep = 0; ep < epochs; ep++) {
        double loss = 0; int nl = 0;
        for (int p = 0; p < npairs; p++) {
            if (pf_eval[p]) continue;
            double l = cce_transformer_qat_step(t, pf_ids[p], pf_len[p], NULL, pf_tgt[p], lr);
            if (l >= 0) { loss += l; nl++; }
        }
        loss = nl ? loss / nl : 0;
        if (ep == 0) first_loss = loss;
        last_loss = loss;
        printf("    epoch %d  joint-QAT train CE = %.4f\n", ep, loss);
    }
    double train_s = (double)(clock() - tic) / CLOCKS_PER_SEC;

    /* eval QAT (still ternary on blocks) */
    cce_transformer_qat_set_qat(t, 1, 1, 1, 0, 0);
    EVAL_HELDOUT(acc_qat);

    printf("\n  === joint QAT on REAL weights: held-out next-token argmax accuracy ===\n");
    printf("    train CE: %.4f -> %.4f  (%d epochs, %.1fs)\n", first_loss, last_loss, epochs, train_s);
    printf("    FP (real, no ternary) : %.1f%%\n", acc_fp);
    printf("    post-hoc ternary      : %.1f%%\n", acc_ph);
    printf("    joint QAT (retrained) : %.1f%%\n", acc_qat);
    printf("    VERDICT: joint QAT %s post-hoc on held-out (%.1f%% vs %.1f%%)\n",
           acc_qat > acc_ph ? "BEATS" : (acc_qat == acc_ph ? "ties" : "loses to"),
           acc_qat, acc_ph);

    CHECK(last_loss <= first_loss + 1e-6, "joint QAT training loss did not increase");
    CHECK(acc_qat >= acc_ph, "joint QAT held-out accuracy >= post-hoc (generalization)");

    /* cleanup */
    for (int p = 0; p < npairs; p++) free(pf_ids[p]);
    free(pf_ids); free(pf_len); free(pf_tgt); free(pf_eval);
    free(lt); free(lr_ref);
    cce_transformer_qat_free(t);
    cce_supra_a2a_free(a);

    printf("\ntransformer_qat_real: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
