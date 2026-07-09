/* Token-level greedy comparison driver: the CNET transformer forward vs an
 * external reference (llama.cpp) on the SAME GGUF bytes.
 *
 * Feeds explicit token ids (tokenization happens on the reference side —
 * CNET mines in id space), generates N greedy steps, prints the argmax
 * chain and the ordered top-K per step. tests/gemma4_vs_ref.py runs the
 * same ids through llama-server and diffs the chains; the gate is argmax
 * identity at every step. Softcap note: the forward returns UNCAPPED
 * logits; c*tanh(x/c) is monotonic so greedy argmax is unaffected.
 *
 * Usage: gemma4_vs_ref <model.gguf> <ids-csv> <steps> [topk]
 *   ids-csv like "2,9105,603" (include BOS explicitly if wanted).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_detect.h"

#define MAX_IDS 4096

int main(int argc, char **argv) {
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *m;
    int ids[MAX_IDS];
    int n_ids = 0;
    int steps, topk = 8;
    float *logits;
    int V;
    char *csv, *tok;
    int s;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <model.gguf> <ids-csv> <steps> [topk]\n",
                argv[0]);
        return 2;
    }
    csv = argv[2];
    for (tok = strtok(csv, ","); tok && n_ids < MAX_IDS;
         tok = strtok(NULL, ","))
        ids[n_ids++] = atoi(tok);
    steps = atoi(argv[3]);
    if (argc > 4) topk = atoi(argv[4]);
    if (n_ids < 1 || steps < 1 || topk < 1 || topk > 32) {
        fprintf(stderr, "bad ids/steps/topk\n");
        return 2;
    }

    if (cce_anymodel_open(&am, argv[1]) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open transformer for %s\n", argv[1]);
        return 1;
    }
    m = am->transformer;
    V = m->vocab_size;
    logits = (float *)malloc((size_t)V * sizeof *logits);
    if (!logits) return 1;

    printf("model: layers=%d hidden=%d vocab=%d  prompt_ids=%d steps=%d\n",
           m->n_layer, m->n_embd, V, n_ids, steps);

    if (getenv("GVR_DUMP_EMB")) {
        const float *r2 = m->tok_emb.data + (size_t)2 * m->n_embd;
        int di;
        printf("tok_emb row2 [0:16]  =");
        for (di = 0; di < 16; di++) printf(" %.6f", (double)r2[di]);
        printf("\ntok_emb row2 [256:8] =");
        for (di = 256; di < 264; di++) printf(" %.6f", (double)r2[di]);
        printf("\ntok_emb row2 [3836:4]=");
        for (di = 3836; di < 3840; di++) printf(" %.6f", (double)r2[di]);
        printf("\n");
    }

    m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(m, ids, n_ids, logits, V) != CCE_OK) {
        fprintf(stderr, "prompt forward REFUSED/failed\n");
        return 1;
    }

    for (s = 0; s < steps; s++) {
        /* ordered top-K, strict > tie-break keeps the earliest id (the
           oracle's convention) */
        int top[32];
        float tv[32];
        int k, j;
        for (k = 0; k < topk; k++) {
            int bi = -1;
            float bv = 0.0f;
            for (j = 0; j < V; j++) {
                int taken = 0, t2;
                for (t2 = 0; t2 < k; t2++)
                    if (top[t2] == j) taken = 1;
                if (taken) continue;
                if (bi == -1 || logits[j] > bv) { bv = logits[j]; bi = j; }
            }
            top[k] = bi;
            tv[k] = bv;
        }
        printf("STEP %3d next=%d top%d=", s, top[0], topk);
        for (k = 0; k < topk; k++)
            printf("%d:%.4f%s", top[k], (double)tv[k],
                   k + 1 < topk ? "," : "\n");

        if (s + 1 < steps) {
            int next = top[0];
            if (cce_gguf_qwen2_forward(m, &next, 1, logits, V) != CCE_OK) {
                fprintf(stderr, "step %d forward failed\n", s);
                return 1;
            }
        }
    }
    printf("DONE\n");
    return 0;
}
