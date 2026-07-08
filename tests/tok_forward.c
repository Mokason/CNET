/* tok_forward: run the model on EXPLICIT token-id sequences and print each
 * one's top-K next-token ids. Loads the model ONCE and streams prompts from
 * stdin — one prompt per line (space-separated token ids) — so a driver
 * (external driver) can ask many real-text questions cheaply. The enabler
 * for real-context work: "The capital of France is ___" instead of bare <bos>.
 *
 * Usage: echo "2 651 3312 ..." | tok_forward <model>   (CNET_ORACLE_INT8/CNET_GPU honored)
 * Prints per line: "TOP <id0> ... <id19>" (full-vocab, highest logit first).
 */

#include <stdio.h>
#include <stdlib.h>
#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_clgemm.h"

int main(int argc, char **argv) {
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *m;
    char line[8192];
    if (argc < 2) { fprintf(stderr, "usage: %s <model>  (prompts on stdin)\n", argv[0]); return 2; }
    if (cce_anymodel_open(&am, argv[1]) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]); return 1;
    }
    m = am->transformer;
    if (getenv("CNET_GPU") && getenv("CNET_GPU")[0] == '1') {
        char dev[128] = {0}; cce_clgemm *g = cce_clgemm_open(NULL, dev, sizeof dev);
        if (g) cce_gguf_set_clgemm(g);
    }
    float *logits = malloc((size_t)m->vocab_size * sizeof *logits);
    int *toks = malloc(2048 * sizeof *toks);
    fprintf(stderr, "tok_forward ready (%d-layer, vocab %d)\n", m->n_layer, m->vocab_size);
    fflush(stderr);

    while (fgets(line, sizeof line, stdin)) {
        int n = 0; char *p = line, *end;
        while (*p && n < 2048) {
            long v = strtol(p, &end, 10);
            if (end == p) break;
            toks[n++] = (int)v; p = end;
        }
        if (n == 0) { printf("TOP\n"); fflush(stdout); continue; }
        m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, toks, n, logits, m->vocab_size) != CCE_OK) {
            printf("ERR\n"); fflush(stdout); continue;
        }
        int taken[20]; for (int r = 0; r < 20; ++r) taken[r] = -1;
        printf("TOP");
        for (int r = 0; r < 20; ++r) {
            int best = -1;
            for (int v = 0; v < m->vocab_size; ++v) {
                int used = 0;
                for (int q = 0; q < r; ++q) if (taken[q] == v) used = 1;
                if (used) continue;
                if (best < 0 || logits[v] > logits[best]) best = v;
            }
            taken[r] = best; printf(" %d", best);
        }
        printf("\n"); fflush(stdout);
    }
    free(toks); free(logits); cce_anymodel_free(am);
    return 0;
}
