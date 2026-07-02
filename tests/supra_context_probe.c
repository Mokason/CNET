/* supra_context_probe — measure how long a context the decomposed Supra model
 * can actually generate.
 *
 * Two distinct quantities:
 *   1. HARD CEILING  = model->block_size (positional-embedding table size).
 *      cce_supra_generate_text refuses to step past pos < block_size, so this is
 *      an absolute wall the architecture cannot cross.
 *   2. COHERENCE LIMIT = the position at which the output collapses into a loop
 *      (low distinct-token ratio in a sliding window). This is the *usable*
 *      context length, which is normally much shorter than the hard ceiling.
 *
 * We fill the whole window once, then report a per-window distinct-ratio table
 * so the degeneration onset is visible, plus the longest immediate-repeat run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/cce/cce_safetensors.h"

#define WIN 32   /* sliding window for the distinct-token-ratio degeneration metric */

static float distinct_ratio(const int* ids, int start, int n) {
    /* fraction of distinct ids in ids[start .. start+n) (O(n^2), n=WIN is tiny) */
    int distinct = 0;
    for (int i = start; i < start + n; i++) {
        int seen = 0;
        for (int j = start; j < i; j++) if (ids[j] == ids[i]) { seen = 1; break; }
        if (!seen) distinct++;
    }
    return (float)distinct / (float)n;
}

static void run_one(cce_supra_a2a* a, const char* prompt, float temp, int topk) {
    cce_supra_decomposed* m = a->model;
    int cap = m->block_size;

    int pids[512]; int np = 0;
    if (a->tokenizer) np = cce_supra_encode_text(a->tokenizer, prompt, pids, 512);

    /* ask for far more than the wall so the wall (not max_new) is what stops us */
    int want = cap + 64;
    int* gens = (int*)malloc((size_t)want * sizeof(int));
    if (!gens) { printf("  OOM\n"); return; }

    int ng = cce_supra_generate_text(m, pids, np, gens, want, temp, topk);

    /* stop reason: did we hit the positional wall, or an <|endoftext|> (50256)? */
    const char* stop;
    int room = cap - np;                       /* generation slots left after prefill */
    if (room < 0) room = 0;
    if (ng > 0 && gens[ng - 1] == 50256)       stop = "EOT (<|endoftext|>)";
    else if (ng >= room)                       stop = "HARD CEILING (block_size wall)";
    else                                       stop = "max_new exhausted (unexpected)";

    /* degeneration scan: longest immediate-repeat run + first window that collapses */
    int longest_run = 1, cur = 1, first_collapse = -1;
    for (int i = 1; i < ng; i++) {
        if (gens[i] == gens[i - 1]) { cur++; if (cur > longest_run) longest_run = cur; }
        else cur = 1;
    }
    for (int s = 0; s + WIN <= ng; s++) {
        if (distinct_ratio(gens, s, WIN) < 0.30f) { first_collapse = s; break; }
    }

    printf("\n  prompt=\"%s\"  (temp=%.2f top_k=%d)\n", prompt, temp, topk);
    printf("    prompt tokens          : %d\n", np);
    printf("    generated tokens       : %d\n", ng);
    printf("    total context reached  : %d / %d positions\n", np + ng, cap);
    printf("    stop reason            : %s\n", stop);
    printf("    longest repeat-run     : %d identical tokens in a row\n", longest_run);
    if (first_collapse >= 0)
        printf("    coherence collapse at  : gen token #%d (window distinct-ratio < 0.30)\n",
               first_collapse);
    else
        printf("    coherence collapse at  : none detected (stayed varied to the wall)\n");

    /* per-window distinct-ratio table, sampled every WIN tokens */
    printf("    distinct-ratio by window (each = %d tokens):\n      ", WIN);
    int col = 0;
    for (int s = 0; s + WIN <= ng; s += WIN) {
        printf("[%3d]%.2f ", s, distinct_ratio(gens, s, WIN));
        if (++col % 6 == 0) printf("\n      ");
    }
    printf("\n");

    /* full decoded text (large buffer; a token can be several chars) */
    int bufsz = (ng + 8) * 12 + 64;
    char* txt = (char*)malloc((size_t)bufsz);
    if (txt && a->tokenizer) {
        cce_supra_decode_text(a->tokenizer, gens, ng, txt, bufsz);
        printf("    --- decoded continuation ---\n%s\n    --- end ---\n", txt);
    }
    free(txt);
    free(gens);
}

int main(int argc, char** argv) {
    const char* prompt = (argc > 1) ? argv[1] : "Once upon a time";

    cce_supra_a2a* a = NULL;
    if (cce_supra_a2a_load(&a, "supra_cache") != CCE_OK || !a || !a->model) {
        printf("Could not load Supra model from ./supra_cache (model.safetensors missing?)\n");
        return 1;
    }
    cce_supra_decomposed* m = a->model;

    printf("=== Supra decomposed model — context probe ===\n");
    printf("  n_layer=%d  n_embd=%d  n_head=%d  vocab=%d\n",
           m->n_layer, m->n_embd, m->n_head, m->vocab_size);
    printf("  block_size (HARD context ceiling) = %d positions\n", m->block_size);

    /* low temp ~ greedy: shows the model's intrinsic loop point.
       higher temp: shows how far varied sampling can push usable length. */
    run_one(a, prompt, 0.20f, 40);
    run_one(a, prompt, 0.80f, 40);

    cce_supra_a2a_free(a);
    return 0;
}
