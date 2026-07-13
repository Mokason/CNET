// moe_parity_dump: run a prompt through llama.cpp and dump the MoE-FFN
// tensors of selected layers to files, so CNET's cce_gguf_moe_ffn_forward
// can be gated against the REFERENCE implementation on real weights
// (Arc B2 parity: same attn_out in -> same top-k selection, same weights,
// same combined expert output within tolerance).
//
// Build (needs the llama.cpp checkout that built libllama):
//   g++ -O2 -o bin/moe_parity_dump tools/moe_parity_dump.cpp \
//       -I$LLAMA/include -I$LLAMA/ggml/include \
//       -L$LLAMA/build/bin -lllama -lggml -lggml-base \
//       -Wl,-rpath,$LLAMA/build/bin
// Run:
//   ./bin/moe_parity_dump <model.gguf> <outdir> [prompt]
//
// Dump format per tensor (<outdir>/<node-name>.bin):
//   "CNDT" | i32 ggml_type(0=f32,26=i32 as-is) | i32 ndim | i64 ne[4] | payload
// Payload is f32 (converted from f16/bf16 when needed) or raw i32 for topk.

#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

static const char* g_outdir = ".";
static bool g_dump_on = true; /* prompt only: greedy decode steps don't re-dump */

static bool wanted(const char* name) {
    /* NOTE: no "ffn_moe_topk" here — that node is a ggml VIEW of the argsort
       result; the sched callback fires for it before the argsort source has
       run, so its dump is stale memory. "ffn_moe_argsort" is the real op
       (full expert ordering; the top-k = its first k entries per token). */
    static const char* pats[] = {
        "attn_out-0", "ffn_norm_2-0", "ffn_moe_logits-0", "ffn_moe_probs-0",
        "ffn_moe_argsort-0", "ffn_moe_weights_norm-0", "ffn_moe_weighted-0",
        "inp_scaled", "result_norm", "result_output",
    };
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++)
        if (strcmp(name, pats[i]) == 0) return true;
    /* full-stack parity ladder: every layer's checkpoints */
    static const char* prefixes[] = {
        "l_out-", "out_scaled-", "attn_norm-", "Vcur_normed-", "attn_post_norm-",
        "attn_out-", "ffn_mlp-", "ffn_moe-", "ffn_post_norm-",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(name, prefixes[i], n) == 0) {
            /* suffix must be a bare layer number ("ffn_moe-3", not "ffn_moe_logits-3") */
            const char* q = name + n;
            if (*q && strspn(q, "0123456789") == strlen(q)) return true;
        }
    }
    return false;
}

static bool dump_cb(struct ggml_tensor* t, bool ask, void* ud) {
    (void)ud;
    if (!g_dump_on) return !ask;
    if (ask) return wanted(t->name);
    if (!wanted(t->name)) return true;

    size_t nbytes = ggml_nbytes(t);
    std::vector<uint8_t> raw(nbytes);
    ggml_backend_tensor_get(t, raw.data(), 0, nbytes);

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.bin", g_outdir, t->name);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return true; }

    int64_t n = ggml_nelements(t);
    int32_t out_type = (t->type == GGML_TYPE_I32) ? (int32_t)GGML_TYPE_I32
                                                  : (int32_t)GGML_TYPE_F32;
    int32_t ndim = ggml_n_dims(t);
    fwrite("CNDT", 1, 4, f);
    fwrite(&out_type, 4, 1, f);
    fwrite(&ndim, 4, 1, f);
    int64_t ne[4] = { t->ne[0], t->ne[1], t->ne[2], t->ne[3] };
    fwrite(ne, 8, 4, f);

    if (t->type == GGML_TYPE_I32) {
        fwrite(raw.data(), 4, (size_t)n, f);
    } else if (t->type == GGML_TYPE_F32) {
        fwrite(raw.data(), 4, (size_t)n, f);
    } else if (t->type == GGML_TYPE_F16) {
        const ggml_fp16_t* h = (const ggml_fp16_t*)raw.data();
        for (int64_t i = 0; i < n; i++) {
            float v = ggml_fp16_to_fp32(h[i]);
            fwrite(&v, 4, 1, f);
        }
    } else if (t->type == GGML_TYPE_BF16) {
        const uint16_t* h = (const uint16_t*)raw.data();
        for (int64_t i = 0; i < n; i++) {
            uint32_t u = (uint32_t)h[i] << 16;
            float v;
            memcpy(&v, &u, 4);
            fwrite(&v, 4, 1, f);
        }
    } else {
        fprintf(stderr, "unhandled type %d for %s\n", (int)t->type, t->name);
    }
    fclose(f);
    fprintf(stderr, "dumped %s type=%d ne=[%lld,%lld,%lld,%lld]\n",
            t->name, (int)t->type,
            (long long)t->ne[0], (long long)t->ne[1],
            (long long)t->ne[2], (long long)t->ne[3]);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <model.gguf> <outdir> [prompt]\n", argv[0]);
        return 1;
    }
    const char* model_path = argv[1];
    g_outdir = argv[2];
    const char* prompt = (argc > 3) ? argv[3] : "The capital of France is";
    /* "ids:2,818" bypasses tokenization: exact token ids for parity replays.
       "seqids:..." feeds the prompt ONE token per decode call (order-of-
       evaluation experiment: does batched vs sequential prefill change the
       model's own routing/continuation?) */
    std::vector<llama_token> forced_ids;
    bool seq_mode = false;
    if (strncmp(prompt, "seqids:", 7) == 0) { seq_mode = true; prompt += 3; }
    if (strncmp(prompt, "ids:", 4) == 0) {
        const char* q = prompt + 4;
        while (*q) {
            forced_ids.push_back((llama_token)strtol(q, (char**)&q, 10));
            while (*q == ',' || *q == ' ') q++;
        }
    }

    llama_backend_init();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0; /* CPU only: leave VRAM alone */
    llama_model* model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "model load failed\n"); return 1; }
    const llama_vocab* vocab = llama_model_get_vocab(model);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 64;
    cp.n_threads = 16;
    cp.n_threads_batch = 16;
    cp.cb_eval = dump_cb;
    cp.cb_eval_user_data = nullptr;
    llama_context* ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "ctx init failed\n"); return 1; }

    std::vector<llama_token> toks(64);
    int nt;
    if (!forced_ids.empty()) {
        toks = forced_ids;
        nt = (int)toks.size();
    } else {
        nt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                            toks.data(), (int32_t)toks.size(),
                            /*add_special*/ true, /*parse_special*/ false);
        if (nt <= 0) { fprintf(stderr, "tokenize failed (%d)\n", nt); return 1; }
        toks.resize(nt);
    }
    fprintf(stderr, "tokens (%d):", nt);
    for (int i = 0; i < nt; i++) fprintf(stderr, " %d", toks[i]);
    fprintf(stderr, "\n");
    {
        char tf[1024];
        snprintf(tf, sizeof(tf), "%s/tokens.txt", g_outdir);
        FILE* f = fopen(tf, "wb");
        if (f) {
            for (int i = 0; i < nt; i++) fprintf(f, "%d\n", toks[i]);
            fclose(f);
        }
    }

    if (seq_mode) {
        /* order experiment: per-position dumps into <outdir>/p<i> */
        const char* base_outdir = g_outdir;
        for (int i = 0; i < nt; i++) {
            static char stepdir[1024];
            snprintf(stepdir, sizeof(stepdir), "%s/p%d", base_outdir, i);
            char mk[1100];
            snprintf(mk, sizeof(mk), "mkdir -p %s", stepdir);
            if (system(mk) != 0) { /* best effort */ }
            g_outdir = stepdir;
            llama_token one = toks[i];
            if (llama_decode(ctx, llama_batch_get_one(&one, 1))) {
                fprintf(stderr, "seq decode failed at %d\n", i);
                return 1;
            }
        }
        g_outdir = base_outdir;
        g_dump_on = false;
    } else if (llama_decode(ctx, llama_batch_get_one(toks.data(), nt))) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }

    /* greedy generation (argv[4] tokens): the reference continuation */
    int n_gen = (argc > 4) ? atoi(argv[4]) : 0;
    if (n_gen > 0) {
        g_dump_on = false;
        int n_vocab = llama_vocab_n_tokens(vocab);
        std::vector<llama_token> gen;
        for (int gi = 0; gi < n_gen; gi++) {
            const float* lg = llama_get_logits_ith(ctx, -1);
            int best = 0;
            for (int v = 1; v < n_vocab; v++)
                if (lg[v] > lg[best]) best = v;
            gen.push_back(best);
            llama_token nt2 = best;
            if (llama_decode(ctx, llama_batch_get_one(&nt2, 1))) {
                fprintf(stderr, "gen decode failed at %d\n", gi);
                break;
            }
        }
        char gf[1024];
        snprintf(gf, sizeof(gf), "%s/gen_tokens.txt", g_outdir);
        FILE* f = fopen(gf, "wb");
        if (f) {
            for (size_t i = 0; i < gen.size(); i++) fprintf(f, "%d\n", gen[i]);
            fclose(f);
        }
        fprintf(stderr, "greedy:");
        for (size_t i = 0; i < gen.size(); i++) fprintf(stderr, " %d", gen[i]);
        fprintf(stderr, "\n");
    }

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr, "done\n");
    return 0;
}
