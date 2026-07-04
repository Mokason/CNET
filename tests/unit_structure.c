/* Unit-structure probe: for a fixed conditioning token t, mine the ENTIRE
 * TOPK function the flagship campaign tries to memorize — top-3 of the model
 * over the window for every w_cur — and report how HARD that function is,
 * BEFORE spending any training time. This is the near-miss-vs-fundamental
 * discriminator: a student that just outputs the single most common answer
 * already scores `majority`/V; exhaustive-exact certification needs V/V.
 *
 *   distinct top3   : how many different ordered top-3 answers over the V
 *                     inputs (few => structured/learnable; ~V => near-random)
 *   majority cover  : inputs mapping to the single most common top3 (the
 *                     constant-baseline accuracy a trivial student reaches)
 *   argmax variety  : distinct top-1 answers (ARGMAX-task difficulty)
 *
 * Uses the SAME model path, window discovery, and BOS anchoring as the
 * campaign, so the numbers are the campaign's actual functions.
 *
 * Usage: unit_structure <model> [V] [n_tokens_to_probe]
 *   (CNET_ORACLE_INT8=1 recommended, exactly like the campaign.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_clgemm.h"
#include "window_discover.h"

int main(int argc, char **argv) {
    size_t V = 256, NT = 6, ti, i, j;
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *m;
    int *vocab, *cond;
    float *logits;

    if (argc < 2) { fprintf(stderr, "usage: %s <model> [V] [NT]\n", argv[0]); return 2; }
    if (argc > 2) V = (size_t)atoi(argv[2]);
    if (argc > 3) NT = (size_t)atoi(argv[3]);

    if (cce_anymodel_open(&am, argv[1]) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open transformer for %s\n", argv[1]);
        return 1;
    }
    m = am->transformer;

    /* CNET_GPU=1: attach a clgemm handle so the 12B forward runs on the
       R9700s (int8 layers via the q8 seam + FP head) — else this is a
       CPU-only crawl. */
    cce_clgemm *gpu = NULL;
    if (getenv("CNET_GPU") && getenv("CNET_GPU")[0] == '1') {
        char dev[128] = {0};
        gpu = cce_clgemm_open(NULL, dev, sizeof dev);
        if (gpu) { cce_gguf_set_clgemm(gpu); printf("gpu: %s\n", dev); }
    }

    vocab = (int *)malloc(V * sizeof *vocab);
    logits = (float *)malloc((size_t)m->vocab_size * sizeof *logits);
    if (!vocab || !logits) return 1;

    int bos = m->bos_token_id;
    size_t placed = cnet_window_discover(m, logits, vocab, V, 2, bos);
    printf("model: %s  V=%lu  window=%lu/%lu  bos=%d\n", argv[1],
           (unsigned long)V, (unsigned long)placed, (unsigned long)V, bos);

    /* conditioning tokens: the campaign uses the window ids themselves */
    cond = (int *)malloc(NT * sizeof *cond);
    for (ti = 0; ti < NT; ++ti) cond[ti] = vocab[(ti * 41u + 7u) % V];

    /* per unit: mine top3 for every w_cur, characterize the function */
    int (*top3)[3] = malloc(V * sizeof *top3);
    printf("\n  t      | distinct_top3 | majority_cover | distinct_argmax\n");
    printf("  -------+---------------+----------------+----------------\n");
    double sum_maj = 0.0, sum_dist = 0.0;
    for (ti = 0; ti < NT; ++ti) {
        int t = cond[ti];
        for (i = 0; i < V; ++i) {
            int toks[3]; int n = 0;
            if (bos >= 0) toks[n++] = bos;
            toks[n++] = t;
            toks[n++] = vocab[i];
            m->cur_pos = 0;
            if (cce_gguf_qwen2_forward(m, toks, n, logits, m->vocab_size) != CCE_OK) {
                fprintf(stderr, "forward failed\n"); return 1;
            }
            int taken[3] = {-1,-1,-1};
            for (int r = 0; r < 3; ++r) {
                int best = -1; float bl = 0.0f;
                for (j = 0; j < V; ++j) {
                    if ((int)j==taken[0]||(int)j==taken[1]||(int)j==taken[2]) continue;
                    if (best<0 || logits[vocab[j]] > bl) { bl = logits[vocab[j]]; best=(int)j; }
                }
                taken[r] = best; top3[i][r] = best;
            }
        }
        /* distinct top3, majority cover, distinct argmax */
        size_t distinct = 0, maj = 0, distinct_am = 0;
        for (i = 0; i < V; ++i) {
            int dup = 0;
            for (j = 0; j < i; ++j) if (memcmp(top3[i],top3[j],sizeof top3[i])==0){dup=1;break;}
            if (!dup) {
                distinct++;
                size_t cnt = 0;
                for (j = 0; j < V; ++j) if (memcmp(top3[i],top3[j],sizeof top3[i])==0) cnt++;
                if (cnt > maj) maj = cnt;
            }
            int dupa = 0;
            for (j = 0; j < i; ++j) if (top3[i][0]==top3[j][0]){dupa=1;break;}
            if (!dupa) distinct_am++;
        }
        printf("  %6d | %13lu | %6lu/%-3lu (%2.0f%%) | %lu\n", t,
               (unsigned long)distinct, (unsigned long)maj, (unsigned long)V,
               100.0*(double)maj/(double)V, (unsigned long)distinct_am);
        sum_maj += (double)maj/(double)V; sum_dist += (double)distinct;
    }
    printf("\n  averages: majority-baseline %.1f%%  distinct_top3 %.0f/%lu\n",
           100.0*sum_maj/(double)NT, sum_dist/(double)NT, (unsigned long)V);
    printf("  => an exhaustive-EXACT student must reach 100%%; the gap from\n"
           "     the majority baseline to 100%% is what training must close.\n");

    free(top3); free(cond); free(vocab); free(logits);
    cce_anymodel_free(am);
    return 0;
}
