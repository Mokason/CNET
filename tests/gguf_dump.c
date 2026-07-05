/* gguf_dump: authoritative pure-C dump of specific tensor stats (loads only the
 * named tensors as F32 — fast, no full-model quantize). Used to verify gemma
 * norm-weight conventions directly from the file. KV mode raw-parses metadata. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_tensor.h"

/* Raw GGUF v2/v3 metadata dumper (independent of the library's KV parsing). */
static uint64_t rd(FILE *f, int n) { uint64_t v = 0; if (fread(&v, n, 1, f) != 1) {} return v; }
static char *rstr(FILE *f) { uint64_t n = rd(f, 8); char *s = malloc(n + 1); if (fread(s, 1, n, f)!=n){} s[n] = 0; return s; }
static const int TSZ[13] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
static double rscalar(FILE *f, int t) {
    switch (t) {
        case 0: return (double)(uint8_t)rd(f,1); case 1: return (double)(int8_t)rd(f,1);
        case 2: return (double)(uint16_t)rd(f,2); case 3: return (double)(int16_t)rd(f,2);
        case 4: return (double)(uint32_t)rd(f,4); case 5: return (double)(int32_t)rd(f,4);
        case 6: { uint32_t u=(uint32_t)rd(f,4); float fv; memcpy(&fv,&u,4); return fv; }
        case 7: return (double)(uint8_t)rd(f,1);
        case 10: return (double)rd(f,8); case 11: return (double)(int64_t)rd(f,8);
        case 12: { uint64_t u=rd(f,8); double dv; memcpy(&dv,&u,8); return dv; }
    }
    return 0;
}
/* Decode token ids to strings from tokenizer.ggml.tokens. */
static void decode_tokens(const char *path, int argc, char **argv, int start) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    rd(f,4); rd(f,4); rd(f,8); uint64_t nkv = rd(f,8);
    char **toks = NULL; uint64_t ntok = 0;
    for (uint64_t i = 0; i < nkv; i++) {
        char *k = rstr(f); uint32_t t = (uint32_t)rd(f,4);
        if (t == 8) { char *s = rstr(f);
            if (strcmp(k,"tokenizer.ggml.tokens")==0) {} free(s); }
        else if (t == 9) {
            uint32_t et = (uint32_t)rd(f,4); uint64_t n = rd(f,8);
            if (strcmp(k,"tokenizer.ggml.tokens")==0 && et==8) {
                toks = malloc(n*sizeof(char*)); ntok=n;
                for (uint64_t j=0;j<n;j++) toks[j]=rstr(f);
                free(k); break;
            } else if (et==8) { for (uint64_t j=0;j<n;j++){char*s=rstr(f);free(s);} }
            else { for (uint64_t j=0;j<n;j++) rscalar(f,et); }
        } else rscalar(f, t);
        free(k);
    }
    for (int a = start; a < argc; a++) {
        int id = atoi(argv[a]);
        printf("  %d = %s\n", id, (toks && id>=0 && (uint64_t)id<ntok) ? toks[id] : "?");
    }
    fclose(f);
}

static void dump_kv(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    rd(f,4); rd(f,4); rd(f,8); uint64_t nkv = rd(f,8);
    for (uint64_t i = 0; i < nkv; i++) {
        char *k = rstr(f); uint32_t t = (uint32_t)rd(f,4);
        if (t == 8) { char *s = rstr(f); printf("  %-46s = \"%s\"\n", k, s); free(s); }
        else if (t == 9) {
            uint32_t et = (uint32_t)rd(f,4); uint64_t n = rd(f,8);
            printf("  %-46s = [%llu x type%u]", k, (unsigned long long)n, et);
            if (et == 8) { for (uint64_t j=0;j<n;j++){char*s=rstr(f); if(j<12)printf(" %s",s); free(s);} }
            else { for (uint64_t j=0;j<n;j++){ double v=rscalar(f,et); if(j<24)printf(" %g",v);} }
            printf("\n");
        } else { printf("  %-46s = %g\n", k, rscalar(f, t)); }
        free(k);
    }
    fclose(f);
}

static void dump(const cce_gguf *g, const char *name) {
    cce_tensor t = {0};
    if (cce_gguf_load_tensor_by_name(g, name, &t) != CCE_OK) { printf("  %-32s ABSENT\n", name); return; }
    double mn = 1e30, mx = -1e30, sum = 0;
    for (size_t i = 0; i < t.numel; i++) { float v = t.data[i]; if (v < mn) mn = v; if (v > mx) mx = v; sum += v; }
    printf("  %-32s numel=%-7zu min=%+.4g mean=%+.4g max=%+.4g\n", name, t.numel, mn, sum / t.numel, mx);
    if (getenv("FULL")) { printf("    ["); for (size_t i = 0; i < t.numel && i < 40; i++) printf("%g ", t.data[i]); printf("...]\n"); }
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
    if (argc > 2 && strcmp(argv[2], "TOK") == 0) { decode_tokens(argv[1], argc, argv, 3); cce_gguf_free(g); return 0; }
    else if (argc > 2 && strcmp(argv[2], "KV") == 0) { dump_kv(argv[1]); }
    else if (argc > 2 && strcmp(argv[2], "LIST") == 0) {
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
