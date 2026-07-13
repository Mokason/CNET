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

static bool wanted(const char* name) {
    /* NOTE: no "ffn_moe_topk" here — that node is a ggml VIEW of the argsort
       result; the sched callback fires for it before the argsort source has
       run, so its dump is stale memory. "ffn_moe_argsort" is the real op
       (full expert ordering; the top-k = its first k entries per token). */
    static const char* pats[] = {
        "attn_out-0", "ffn_norm_2-0", "ffn_moe_logits-0", "ffn_moe_probs-0",
        "ffn_moe_argsort-0", "ffn_moe_weights_norm-0", "ffn_moe_weighted-0",
    };
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++)
        if (strcmp(name, pats[i]) == 0) return true;
    return false;
}

static bool dump_cb(struct ggml_tensor* t, bool ask, void* ud) {
    (void)ud;
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
    int nt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                            toks.data(), (int32_t)toks.size(),
                            /*add_special*/ true, /*parse_special*/ false);
    if (nt <= 0) { fprintf(stderr, "tokenize failed (%d)\n", nt); return 1; }
    toks.resize(nt);
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

    if (llama_decode(ctx, llama_batch_get_one(toks.data(), nt))) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    fprintf(stderr, "done\n");
    return 0;
}
