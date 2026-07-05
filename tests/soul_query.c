/* Ask the soul questions. Loads the certified base + the source model,
 * reconstructs the campaign's bos-anchored window, and for each query
 * (conditioning token t, current token w) runs BOTH:
 *   - the SOUL: the certified BTN unit acq_tk<t>q<t> on the one-hot w,
 *     reading its ordered top-3 output fields;
 *   - the ORACLE: the live model forward [bos, t, w], its window top-3;
 * and prints both as raw token ids (soul_query.py decodes to text and shows
 * whether the extracted unit reproduces the model it was distilled from).
 *
 * Usage: soul_query <model> <base.cnb> [V]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/base.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_clgemm.h"
#include "window_discover.h"

/* live-model ordered top-3 within the window for context [bos?, t, w] */
static void oracle_top3(cce_gguf_qwen2 *m, const int *vocab, int V,
                        int t, int w, float *logits, int *out3) {
    int toks[3], n = 0, taken[3] = {-1,-1,-1}, r;
    if (m->bos_token_id >= 0) toks[n++] = m->bos_token_id;
    toks[n++] = t;
    toks[n++] = w;
    m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(m, toks, n, logits, m->vocab_size) != CCE_OK) {
        out3[0] = out3[1] = out3[2] = -1;
        return;
    }
    for (r = 0; r < 3; ++r) {
        int i, best = -1; float bl = 0.0f;
        for (i = 0; i < V; ++i) {
            if (i == taken[0] || i == taken[1] || i == taken[2]) continue;
            if (best < 0 || logits[vocab[i]] > bl) { bl = logits[vocab[i]]; best = i; }
        }
        taken[r] = best; out3[r] = best;
    }
}

int main(int argc, char **argv) {
    int V = 256, i;
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *m;
    CnetBase base;
    int *vocab; float *logits; double *in;

    if (argc < 3) { fprintf(stderr, "usage: %s <model> <base.cnb> [V]\n", argv[0]); return 2; }
    if (argc > 3) V = atoi(argv[3]);

    if (cce_anymodel_open(&am, argv[1]) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open model %s\n", argv[1]); return 1;
    }
    m = am->transformer;
    { char dev[128]={0}; cce_clgemm *g = cce_clgemm_open(NULL, dev, sizeof dev);
      if (g) cce_gguf_set_clgemm(g); }

    vocab = malloc(V * sizeof *vocab);
    logits = malloc((size_t)m->vocab_size * sizeof *logits);
    in = malloc(V * sizeof *in);
    if (!vocab || !logits || !in) return 1;
    cnet_window_discover(m, logits, vocab, V, 2, m->bos_token_id);

    cnb_init(&base);
    if (cnb_load(&base, argv[2]) != 0) { fprintf(stderr, "cannot load base %s\n", argv[2]); return 1; }

    /* dump the window so the decoder can render every id */
    printf("WINDOW");
    for (i = 0; i < V; ++i) printf(" %d", vocab[i]);
    printf("\n");

    /* queries: a spread of conditioning tokens x current tokens */
    int t_idx[8]  = { 0, 1, 2, 3, 5, 8, 13, 21 };
    int w_idx[6]  = { 1, 2, 4, 7, 11, 19 };
    for (int a = 0; a < 8; ++a) {
        int t = vocab[t_idx[a]];
        char name[64];
        snprintf(name, sizeof name, "acq_tk%dq%d", t, t);
        BinaryTransformNetwork btn; Contract c;
        memset(&btn, 0, sizeof btn); memset(&c, 0, sizeof c);
        if (cnb_get_unit(&base, name, &btn, &c) != 0) {
            printf("MISS %s\n", name);   /* one of the 3 deferred, or absent */
            continue;
        }
        for (int b = 0; b < 6; ++b) {
            int w = w_idx[b];
            int soul3[3], orc3[3], r;
            /* soul: one-hot w -> unit -> ordered top-3 fields */
            const double *out;
            for (i = 0; i < V; ++i) in[i] = 0.0;
            in[w] = 1.0;
            out = btn_forward(&btn, in);
            for (r = 0; r < 3; ++r) {
                int j, best = 0;
                for (j = 1; j < V; ++j)
                    if (out[(size_t)r * V + j] > out[(size_t)r * V + best]) best = j;
                soul3[r] = best;
            }
            oracle_top3(m, vocab, V, t, vocab[w], logits, orc3);
            /* print token ids: t, w, soul top3 (as token ids), oracle top3 */
            printf("Q %d %d  SOUL %d %d %d  ORACLE %d %d %d\n",
                   t, vocab[w],
                   vocab[soul3[0]], vocab[soul3[1]], vocab[soul3[2]],
                   orc3[0] < 0 ? -1 : vocab[orc3[0]],
                   orc3[1] < 0 ? -1 : vocab[orc3[1]],
                   orc3[2] < 0 ? -1 : vocab[orc3[2]]);
        }
        btn_free(&btn);
        contract_free(&c);
    }
    cnb_free(&base);
    cce_anymodel_free(am);
    return 0;
}
