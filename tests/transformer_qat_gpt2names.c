/* transformer_qat_gpt2names — the decomposer generalization gate: load a model
 * that Supra's old hardwiring could NOT load — GPT-2-STYLE tensor naming
 * (wte/wpe, h.{i}.ln_1, h.{i}.attn.c_attn, ...), 6 layers (vs the old fixed 4),
 * 2 heads (vs the old fixed 4, carried in safetensors __metadata__), Conv1D
 * [in,out] block weights (GPT-2's layout — verbatim copy, no transpose), and a
 * TIED logits head (no lm_head tensor, like the real HF gpt2 checkpoint).
 *
 * Fixture: tools/gen_altmodel.py altmodel_gpt2_cache gpt2 (random weights,
 * D128/V2000/B96/mlp512 alt-family dims). Three proofs:
 *
 *  1. GOLDEN parity — the generator embeds logits from an independent numpy
 *     forward ("golden.ids"/"golden.logits"); cce_supra_gpt_forward must match
 *     them. This pins the LOAD SEMANTICS (Conv1D no-transpose, biases, tied
 *     head) against an implementation that shares no code with CNET — a
 *     square-matrix transpose bug would fool trainer-vs-decomposer agreement
 *     but not this.
 *  2. Trainer parity — FP-mode cce_transformer_qat_logits matches
 *     cce_supra_gpt_forward on random sequences (mirrors transformer_qat_altmodel).
 *  3. Joint-QAT smoke — a QAT step runs and the loss falls at this config
 *     (backward works at 6 layers / 2 heads).
 *
 * Build/run:  make transformer_qat_gpt2names   (needs python3+numpy+safetensors)
 * Marker:     TRANSFORMER_QAT_GPT2NAMES_PASS
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
    const char* dir = "altmodel_gpt2_cache";
    char stpath[256];
    snprintf(stpath, sizeof(stpath), "%s/model.safetensors", dir);

    /* ---------- load the GPT-2-named model via the generalized decomposer ---------- */
    cce_supra_decomposed* m = NULL;
    if (cce_supra_load_decomposed(&m, dir, NULL) != CCE_OK || !m) {
        printf("FAIL: could not load model from ./%s (run tools/gen_altmodel.py %s gpt2 first)\n", dir, dir);
        return 1;
    }
    int D = m->n_embd, V = m->vocab_size, B = m->block_size, H = m->n_head, L = m->n_layer;
    int Mh = discover_mlp_hidden(m);
    printf("gpt2-named model: schema=%s n_layer=%d n_embd=%d n_head=%d mlp_hidden=%d vocab=%d block=%d head_tied=%d\n",
           m->naming_schema ? m->naming_schema : "(null)", L, D, H, Mh, V, B, m->head_tied);
    printf("(the old decomposer hardwired n_layer=4/n_head=4 + Supra names: this file has NEITHER)\n");
    CHECK(m->naming_schema && strcmp(m->naming_schema, "gpt2") == 0, "gpt2 naming schema detected");
    CHECK(L == 6, "n_layer=6 counted from the h.{i}.* tensors (not the old fixed 4)");
    CHECK(H == 2, "n_head=2 read from safetensors __metadata__ (not the old fixed 4)");
    CHECK(m->head_tied == 1, "tied logits head (no lm_head tensor, GPT-2 style)");
    CHECK(Mh > 0, "read mlp_hidden from the model");

    /* ---------- fail-closed refusals: corrupt fixture variants must NOT load ----------
     * gpt2_nohead: gpt2 naming, no n_head in __metadata__ and no config.json —
     *   the historical n_head=4 fallback is Supra-only, so this must refuse.
     * gpt2_gap: layers h.0, h.1, h.3 (h.2 missing) — a consecutive count would
     *   silently truncate to 2 layers, so this must refuse. */
    printf("\n=== fail-closed refusals (corrupt variants) ===\n");
    {
        static const char* bad_dir[2] = { "altmodel_gpt2_nohead_cache", "altmodel_gpt2_gap_cache" };
        static const char* bad_msg[2] = { "refused: gpt2 schema without n_head (no silent 4 fallback)",
                                          "refused: layer numbering gap (no silent truncation)" };
        for (int c = 0; c < 2; c++) {
            char p[256];
            snprintf(p, sizeof(p), "%s/model.safetensors", bad_dir[c]);
            FILE* fx = fopen(p, "rb");
            CHECK(fx != NULL, "corrupt fixture present (make regenerates via tools/gen_altmodel.py)");
            if (!fx) continue;   /* no local file -> loader would try a download; skip */
            fclose(fx);
            cce_supra_decomposed* bm = NULL;
            cce_result brc = cce_supra_load_decomposed(&bm, bad_dir[c], NULL);
            printf("  %s -> rc=%d model=%s\n", bad_dir[c], (int)brc, bm ? "NON-NULL" : "NULL");
            CHECK(brc != CCE_OK && bm == NULL, bad_msg[c]);
            if (bm) cce_supra_free_decomposed(bm);
        }
    }

    /* ---------- golden parity: decomposer forward vs the generator's numpy forward ---------- */
    printf("\n=== golden parity (cce_supra_gpt_forward vs independent numpy logits) ===\n");
    float* lg = (float*)malloc((size_t)V * sizeof(float));
    {
        cce_safetensors* st = NULL;
        cce_tensor gids = {0}, glog = {0};
        int ok = (cce_safetensors_load(stpath, &st) == CCE_OK && st);
        if (ok) {
            int ii = cce_safetensors_find(st, "golden.ids");
            int li = cce_safetensors_find(st, "golden.logits");
            ok = ii >= 0 && li >= 0 &&
                 cce_safetensors_load_as_tensor(st, ii, &gids) == CCE_OK &&
                 cce_safetensors_load_as_tensor(st, li, &glog) == CCE_OK &&
                 gids.ndim == 2 && glog.ndim == 2 && glog.shape[1] == V &&
                 glog.shape[0] == gids.shape[0];
        }
        CHECK(ok, "golden tensors present in the fixture");
        int gn = 0, gmatch = 0; double ggmax = 0.0;
        if (ok) {
            int NS = gids.shape[0], T = gids.shape[1];
            for (int s = 0; s < NS; s++) {
                int ids[64];
                if (T > 64) break;
                for (int i = 0; i < T; i++) ids[i] = (int)gids.data[(size_t)s * T + i];
                if (cce_supra_gpt_forward(m, ids, T, lg, V) != CCE_OK) continue;
                const float* ref = glog.data + (size_t)s * V;
                double smax = 0.0;
                for (int i = 0; i < V; i++) { double d = fabs((double)lg[i] - (double)ref[i]);
                    if (d > smax) smax = d; }
                if (smax > ggmax) ggmax = smax;
                int ac = argmax(lg, V), ar = argmax(ref, V);
                gmatch += (ac == ar);
                printf("  golden %d (T=%2d): argmax cnet=%d numpy=%d %s | max|d|=%.3e\n",
                       s, T, ac, ar, ac == ar ? "OK" : "MISMATCH", smax);
                gn++;
            }
        }
        printf("  golden parity over %d seqs: argmax %d/%d | max|Dlogit|=%.3e\n", gn, gmatch, gn, ggmax);
        CHECK(gn >= 5, "at least 5 golden sequences evaluated");
        CHECK(gn > 0 && gmatch == gn, "argmax matches numpy on ALL golden sequences");
        CHECK(gn > 0 && ggmax < 1e-2, "max|Dlogit| vs numpy < 1e-2 (Conv1D layout + tied head loaded right)");
        cce_tensor_free(&gids); cce_tensor_free(&glog);
        if (st) cce_safetensors_free(st);
    }

    /* ---------- create trainer at this config + load the weights ---------- */
    cce_transformer_qat_config cfg = {0};
    cfg.n_layer = L; cfg.n_embd = D; cfg.n_head = H; cfg.mlp_hidden = Mh;
    cfg.vocab = V; cfg.block_size = B; cfg.seed = 1234;
    cce_transformer_qat* t = cce_transformer_qat_create(&cfg);
    if (!t) { printf("FAIL: trainer create\n"); free(lg); cce_supra_free_decomposed(m); return 1; }
    cce_result lrc = cce_transformer_qat_load_decomposed(t, m);
    CHECK(lrc == CCE_OK, "cce_transformer_qat_load_decomposed at 6 layers / 2 heads");
    if (lrc != CCE_OK) { free(lg); cce_transformer_qat_free(t); cce_supra_free_decomposed(m); return 1; }

    /* ---------- trainer parity: random token sequences (mirrors altmodel) ---------- */
    printf("\n=== forward parity (FP trainer vs cce_supra_gpt_forward) ===\n");
    float* lt = (float*)malloc((size_t)V * sizeof(float));
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
        if (cce_supra_gpt_forward(m, ids, T, lg, V) != CCE_OK) continue;
        double seq_max = 0.0;
        for (int i = 0; i < V; i++) { double d = fabs((double)lt[i] - (double)lg[i]);
            if (d > seq_max) seq_max = d;
            gsum += d; gcmp++; }
        if (seq_max > gmax) gmax = seq_max;
        int at = argmax(lt, V), ar = argmax(lg, V);
        argmax_match += (at == ar);
        printf("  seq %d (T=%2d): argmax trainer=%d ref=%d %s | max|d|=%.3e\n",
               s, T, at, ar, at == ar ? "OK" : "MISMATCH", seq_max);
        n_seq++;
    }
    printf("\n  parity over %d seqs: argmax %d/%d | global max|Dlogit|=%.3e | mean|Dlogit|=%.3e\n",
           n_seq, argmax_match, n_seq, gmax, gcmp ? gsum / (double)gcmp : 0.0);
    CHECK(n_seq >= 5, "at least 5 parity sequences evaluated");
    CHECK(argmax_match == n_seq, "top-1 argmax identical on ALL sequences (weights loaded correctly)");
    CHECK(gmax < 1e-2, "max|Dlogit| < 1e-2 (float-vs-double accumulation only)");
    printf("  PARITY %s at the gpt2-named config\n", (n_seq >= 5 && argmax_match == n_seq) ? "PASSED" : "FAILED");

    /* ---------- joint-QAT smoke: backward works at 6 layers / 2 heads ---------- */
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
        if (ep == 0) first = loss;
        last = loss;
        if (ep % 3 == 0 || ep == 11) printf("    epoch %2d  QAT train CE = %.4f\n", ep, loss);
    }
    CHECK(ok_step, "joint-QAT step runs and returns finite loss at this config");
    CHECK(last < first, "joint-QAT loss falls on a fixed set (backward works at this config)");

    free(lt); free(lg);
    cce_transformer_qat_free(t);
    cce_supra_free_decomposed(m);
    printf("\ntransformer_qat_gpt2names: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) printf("TRANSFORMER_QAT_GPT2NAMES_PASS\n");
    return g_fail ? 1 : 0;
}
