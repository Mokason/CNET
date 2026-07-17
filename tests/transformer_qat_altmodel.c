/* transformer_qat_altmodel — run the transformer_qat load+parity path on a
 * DIFFERENT model than Supra, to prove the trainer/loader/forward are not tied
 * to Supra's specific dimensions.
 *
 * Historical note: when this gate was written the Supra decomposer hardwired
 * 4 layers / 4 heads and Supra's tensor NAMES (fixed [4] LN arrays), so the
 * "different model" is a 4-layer model in Supra's naming with EVERY FREE
 * DIMENSION changed (n_embd, vocab, block, mlp), built by tools/gen_altmodel.py
 * into ./altmodel_cache. It stays that way as the free-dimension gate (D128 vs
 * 256, V2000 vs 50520, B96 vs 384, mlp512 vs 1024, head_dim 32 vs 64); the
 * decomposer has since been generalized (schema table, counted n_layer,
 * metadata n_head) and transformer_qat_gpt2names covers other layer counts,
 * GPT-2-style naming, Conv1D layout, and tied heads.
 *
 * The proof is forward parity: FP-mode cce_transformer_qat_logits must match
 * cce_supra_gpt_forward on this different model, exactly as it did for Supra.
 * (Weights are random, so QAT-recovery accuracy is meaningless here — we assert
 * parity, plus that a joint-QAT step runs and returns a finite loss at the new
 * config.)
 *
 * Build/run:  make transformer_qat_altmodel
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_safetensors.h"
#include "../include/cce/cce_transformer_qat.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("PASS: %s\n", (msg)); } \
                           else { g_fail++; printf("FAIL: %s\n", (msg)); } } while (0)

static int argmax(const float* v, int n) { int b = 0; for (int i = 1; i < n; i++) if (v[i] > v[b]) b = i; return b; }

static int discover_mlp_hidden(cce_supra_decomposed* m) {
    cce_cascade* up = cce_forest_get_resident(m->forest, "gpt.block0.mlp_up");
    if (!up || up->num_blocks < 1) return -1;
    return up->blocks[up->num_blocks - 1].weights.shape[1];
}

int main(void) {
    const char* dir = "altmodel_cache";
    /* ---------- load the DIFFERENT model via the Supra decomposer ---------- */
    cce_supra_decomposed* m = NULL;
    if (cce_supra_load_decomposed(&m, dir, NULL) != CCE_OK || !m) {
        printf("FAIL: could not load model from ./%s (run tools/gen_altmodel.py first)\n", dir);
        return 1;
    }
    int D = m->n_embd, V = m->vocab_size, B = m->block_size, H = m->n_head, L = m->n_layer;
    int Mh = discover_mlp_hidden(m);
    printf("alt model: n_layer=%d n_embd=%d n_head=%d mlp_hidden=%d vocab=%d block_size=%d\n",
           L, D, H, Mh, V, B);
    printf("(Supra for contrast: n_layer=4 n_embd=256 n_head=4 mlp_hidden=1024 vocab=50520 block=384)\n");
    CHECK(Mh > 0, "read mlp_hidden from the alt model");
    CHECK(D != 256 || V != 50520 || B != 384 || Mh != 1024, "config genuinely differs from Supra");

    /* ---------- create trainer at the alt config + load the weights ---------- */
    cce_transformer_qat_config cfg = {0};
    cfg.n_layer = L; cfg.n_embd = D; cfg.n_head = H; cfg.mlp_hidden = Mh;
    cfg.vocab = V; cfg.block_size = B; cfg.seed = 1234;
    cce_transformer_qat* t = cce_transformer_qat_create(&cfg);
    if (!t) { printf("FAIL: trainer create\n"); cce_supra_free_decomposed(m); return 1; }
    cce_result lrc = cce_transformer_qat_load_decomposed(t, m);
    CHECK(lrc == CCE_OK, "cce_transformer_qat_load_decomposed at the alt config");
    if (lrc != CCE_OK) { cce_transformer_qat_free(t); cce_supra_free_decomposed(m); return 1; }

    /* ---------- forward parity (the proof): random token sequences ---------- */
    printf("\n=== forward parity (FP trainer vs cce_supra_gpt_forward) ===\n");
    float* lt = (float*)malloc((size_t)V * sizeof(float));
    float* lr = (float*)malloc((size_t)V * sizeof(float));
    cce_transformer_qat_set_qat(t, 0, 0, 0, 0, 0);   /* FP mode */

    unsigned long long rng = 0x9E3779B97F4A7C15ULL;
    const int MAX_SEQ = 10;
    int n_seq = 0, argmax_match = 0;
    double gmax = 0.0, gsum = 0.0; long gcmp = 0;
    for (int s = 0; s < MAX_SEQ; s++) {
        int T = 6 + (s % 7);                 /* lengths 6..12, all <= block */
        if (T > B) T = B;
        int ids[32];
        for (int i = 0; i < T; i++) { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                                      ids[i] = (int)((rng >> 33) % (unsigned)V); }
        if (cce_transformer_qat_logits(t, ids, T, lt) != CCE_OK) continue;
        if (cce_supra_gpt_forward(m, ids, T, lr, V) != CCE_OK) continue;
        double seq_max = 0.0;
        for (int i = 0; i < V; i++) { double d = fabs((double)lt[i] - (double)lr[i]);
            if (d > seq_max) seq_max = d; gsum += d; gcmp++; }
        if (seq_max > gmax) gmax = seq_max;
        int at = argmax(lt, V), ar = argmax(lr, V);
        argmax_match += (at == ar);
        printf("  seq %d (T=%2d): argmax trainer=%d ref=%d %s | max|d|=%.3e\n",
               s, T, at, ar, at == ar ? "OK" : "MISMATCH", seq_max);
        n_seq++;
    }
    printf("\n  parity over %d seqs: argmax %d/%d | global max|Dlogit|=%.3e | mean|Dlogit|=%.3e\n",
           n_seq, argmax_match, n_seq, gmax, gcmp ? gsum / (double)gcmp : 0.0);
    CHECK(n_seq >= 5, "at least 5 parity sequences evaluated");
    CHECK(argmax_match == n_seq, "top-1 argmax identical on ALL sequences (alt weights loaded correctly)");
    CHECK(gmax < 1e-2, "max|Dlogit| < 1e-2 (float-vs-double accumulation only)");
    printf("  PARITY %s at the alt config\n", (n_seq >= 5 && argmax_match == n_seq) ? "PASSED" : "FAILED");

    /* ---------- joint-QAT smoke at the alt config: memorize a FIXED tiny set ----------
       Blocks ternary + STE (head/emb FP). A FIXED set of (prefix,target) pairs so
       the loss can actually fall -- this exercises the BACKWARD at the alt config,
       not just the forward. (Weights are random, so this is a "does it learn at
       all" smoke, not a quality claim.) */
    printf("\n=== joint-QAT smoke (memorize 12 fixed pairs, blocks ternary + STE) ===\n");
    #define NSMOKE 12
    #define TSMOKE 8
    static int sm_ids[NSMOKE][TSMOKE]; int sm_tgt[NSMOKE];
    for (int p = 0; p < NSMOKE; p++) {
        for (int i = 0; i < TSMOKE; i++) { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                                           sm_ids[p][i] = (int)((rng >> 33) % (unsigned)V); }
        sm_tgt[p] = sm_ids[p][TSMOKE - 1];       /* target = last token; prefix = first TSMOKE-1 */
    }
    cce_transformer_qat_set_qat(t, 1, 1, 1, 0, 0);
    double first = 0, last = 0; int ok_step = 1;
    for (int ep = 0; ep < 12; ep++) {
        double loss = 0; int nl = 0;
        for (int p = 0; p < NSMOKE; p++) {
            double l = cce_transformer_qat_step(t, sm_ids[p], TSMOKE - 1, NULL, sm_tgt[p], 5e-3f);
            if (!(l == l) || l < 0) ok_step = 0;   /* NaN or error */
            else { loss += l; nl++; }
        }
        loss = nl ? loss / nl : 0;
        if (ep == 0) first = loss; last = loss;
        if (ep % 3 == 0 || ep == 11) printf("    epoch %2d  QAT train CE = %.4f\n", ep, loss);
    }
    CHECK(ok_step, "joint-QAT step runs and returns finite loss at the alt config");
    CHECK(last < first, "joint-QAT loss falls on a fixed set (backward works at the alt config)");

    free(lt); free(lr);
    cce_transformer_qat_free(t);
    cce_supra_free_decomposed(m);
    printf("\ntransformer_qat_altmodel: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
