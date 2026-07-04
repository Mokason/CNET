#ifndef CNET_WINDOW_DISCOVER_H
#define CNET_WINDOW_DISCOVER_H

/* Window discovery, shared by flagship_run and depth_probe (both must
 * derive the IDENTICAL window or the depth gate compares apples to
 * oranges).
 *
 * WHY: an arbitrary id window (2000..2000+V) yields CONSTANT conditional
 * slices on real models — measured on BOTH the gemma MTP draft and the
 * gemma4-v2 12B: 0 variation over 128 probe contexts, every top-3 equal to
 * the tie-break pattern [first,second,third] of flat logits. The model's
 * attractor tokens live outside the window, so the restricted argmax never
 * varies and every mined unit is one constant function. Building the window
 * from the model's OWN most frequent full-vocab argmax choices over a
 * deterministic probe spread makes the restricted decisions actually vary.
 * (Extracted from flagship_run's PAIR-only discover_pair_window — the same
 * failure mode applies to ARGMAX and TOPK.)
 *
 * n_ctx = probe context length: 3 reproduces the historical PAIR discovery
 * byte-for-byte; 2 matches the ARGMAX/TOPK query shape. Deterministic, so
 * resume across runs sees identical windows (and identical unit tags).
 * Returns the number of discovered tokens placed (rest keep defaults). */
static size_t cnet_window_discover(cce_gguf_qwen2 *m, float *logits,
                                   int *vocab, size_t V, int n_ctx, int bos) {
    unsigned *hist;
    size_t i, placed = 0;
    int probes = 128;

    /* BOS-anchored models: the window IS the model's ordered top-V
       continuations of [bos] — its natural sentence-starters. Live by
       construction (real probability mass, not junk-context attractors),
       one forward, deterministic. The histogram fallback below serves
       BOS-less models. */
    if (bos >= 0) {
        int tok = bos;
        char *taken = (char *)calloc((size_t)m->vocab_size, 1);
        if (!taken) return 0;
        m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, &tok, 1, logits,
                                   m->vocab_size) != CCE_OK) {
            free(taken);
            return 0;
        }
        for (placed = 0; placed < V; ++placed) {
            size_t a, best = (size_t)-1;
            float bl = 0.0f;
            for (a = 0; a < (size_t)m->vocab_size; ++a) {
                if (taken[a]) continue;
                if (best == (size_t)-1 || logits[a] > bl) {
                    bl = logits[a];
                    best = a;
                }
            }
            taken[best] = 1;
            vocab[placed] = (int)best;
        }
        free(taken);
        return placed;
    }

    hist = (unsigned *)calloc((size_t)m->vocab_size, sizeof *hist);
    if (!hist) return 0;
    for (i = 0; i < (size_t)probes; ++i) {
        int tokens[4];
        int n = 0;
        size_t a, best = 0;
        /* bos >= 0 anchors the context: gemma-family models collapse to
           <pad> on BOS-less raw-token contexts (measured: 1 distinct argmax
           over 128 probes on both real models) — the probes must live in
           the same distribution the oracle queries. */
        if (bos >= 0) tokens[n++] = bos;
        tokens[n++] = 2000 + (int)((i * 37u) % 4096u);
        tokens[n++] = 2000 + (int)((i * 91u + 17u) % 4096u);
        if (n_ctx >= 3) tokens[n++] = 2000 + (int)((i * 53u + 5u) % 4096u);
        m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, tokens, n, logits,
                                   m->vocab_size) != CCE_OK) continue;
        for (a = 1; a < (size_t)m->vocab_size; ++a)
            if (logits[a] > logits[best]) best = a;
        hist[best]++;
    }
    while (placed < V) {
        size_t a, best = 0;
        for (a = 1; a < (size_t)m->vocab_size; ++a)
            if (hist[a] > hist[best]) best = a;
        if (hist[best] == 0) break;      /* fewer distinct choices than V */
        vocab[placed++] = (int)best;
        hist[best] = 0;
    }
    free(hist);
    return placed;
}

#endif /* CNET_WINDOW_DISCOVER_H */
