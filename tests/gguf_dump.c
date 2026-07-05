/* gguf_dump: authoritative pure-C dump of specific tensor stats (loads only the
 * named tensors as F32 — fast, no full-model quantize). Used to verify gemma
 * norm-weight conventions directly from the file. */
#include <stdio.h>
#include <math.h>
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_tensor.h"

static void dump(const cce_gguf *g, const char *name) {
    cce_tensor t = {0};
    if (cce_gguf_load_tensor_by_name(g, name, &t) != CCE_OK) { printf("  %-32s ABSENT\n", name); return; }
    double mn = 1e30, mx = -1e30, sum = 0;
    for (size_t i = 0; i < t.numel; i++) { float v = t.data[i]; if (v < mn) mn = v; if (v > mx) mx = v; sum += v; }
    printf("  %-32s numel=%-7zu min=%+.4f mean=%+.4f max=%+.4f  first=[%.4f %.4f %.4f %.4f]\n",
           name, t.numel, mn, sum / t.numel, mx, t.data[0], t.data[1], t.data[2], t.data[3]);
    cce_tensor_free(&t);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <model.gguf> [tensor..]\n", argv[0]); return 2; }
    cce_gguf *g = NULL;
    if (cce_gguf_load(argv[1], &g) != CCE_OK) { fprintf(stderr, "load failed\n"); return 1; }
    printf("arch=%s n_layer=%d n_embd=%d n_head=%d n_kv=%d ffn=%d rope_base=%.0f rms_eps=%g vocab=%d\n",
           cce_gguf_get_arch(g), cce_gguf_get_n_layer(g), cce_gguf_get_hidden_size(g),
           cce_gguf_get_n_heads(g), cce_gguf_get_n_kv_heads(g), cce_gguf_get_feed_forward_length(g),
           cce_gguf_get_rope_freq_base(g), cce_gguf_get_rms_eps(g), cce_gguf_get_vocab_size(g));
    if (argc > 2 && strcmp(argv[2], "LIST") == 0) {
        int nt = cce_gguf_tensor_count(g);
        for (int i = 0; i < nt; i++) {
            cce_gguf_tensor_meta m = {0};
            if (cce_gguf_get_tensor_meta(g, i, &m) != CCE_OK) continue;
            const char *nm = m.name;
            const char *pat = (argc > 3) ? argv[3] : "";
            if (pat[0] == 0 || strstr(nm, pat))
                printf("  %-34s ndim=%d dims=[%d %d] ggml_type=%u\n", nm, m.ndim,
                       m.shape[0], (m.ndim>1?m.shape[1]:0), m.ggml_type);
        }
    } else if (argc > 2) { for (int i = 2; i < argc; i++) dump(g, argv[i]); }
    else {
        const char *names[] = {
            "blk.0.attn_norm.weight", "blk.0.ffn_norm.weight",
            "blk.0.attn_q_norm.weight", "blk.0.attn_k_norm.weight",
            "blk.0.post_attention_norm.weight", "blk.0.post_ffw_norm.weight",
            "output_norm.weight", "blk.0.attn_q.weight", "blk.0.attn_k.weight" };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) dump(g, names[i]);
    }
    cce_gguf_free(g);
    return 0;
}
