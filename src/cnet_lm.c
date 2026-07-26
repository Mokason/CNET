#include "../include/cnet_lm.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/agent_memory.h"  /* for sanitize, mcp write if needed */
#include "../include/contract/anti_repeat.h"  /* reusable anti-repeat circuit/contract logic */
#include "../include/cce/cce_learn.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>

/* Internal helpers */

static void init_default_vocab(CnetLmVocab *v) {
    const char *defs[] = {
        "the", "cat", "sat", "on", "mat", "a", "b", "c", "quick", "brown", "fox",
        "story", "tale", "lm", "generate", "concept", "speech", "to", "token", " ",
        ".", "once", "upon", "creative", "narrative", "h", "e", "l", "o", "world"
    };
    v->size = 0;
    for (size_t i = 0; i < sizeof(defs)/sizeof(defs[0]) && v->size < CNET_LM_MAX_VOCAB; i++) {
        strncpy(v->tokens[v->size], defs[i], CNET_LM_MAX_VOCAB_NAME-1);
        v->tokens[v->size][CNET_LM_MAX_VOCAB_NAME-1] = '\0';
        v->size++;
    }
}

static int vocab_index_internal(const CnetLmVocab *v, const char *tok) {
    if (!tok) return 0;
    for (int i = 0; i < v->size; i++) {
        if (strcmp(v->tokens[i], tok) == 0) return i;
    }
    /* fallback: try first char match */
    if (tok[0]) {
        for (int i = 0; i < v->size; i++) {
            if (v->tokens[i][0] == tok[0]) return i;
        }
    }
    return 0;
}

void cnet_lm_init(CnetLmModel *model) {
    if (!model) return;
    memset(model, 0, sizeof(*model));
    init_default_vocab(&model->vocab);
    model->input_dim = model->vocab.size;
    model->output_dim = model->vocab.size;
    model->has_cce = false;
    model->gpu = NULL;
}

cce_result cnet_lm_enable_gpu(CnetLmModel *model) {
    if (!model) return CCE_ERR_INVALID_ARG;
#ifdef CCE_HAVE_CUDA
    cce_gpu_ctx* g = NULL;
    if (cce_gpu_init_cuda(&g) == CCE_OK && g && g->initialized) {
        model->gpu = g;
        return CCE_OK;
    }
#endif
    return CCE_ERR_UNSUPPORTED;
}

void cnet_lm_free(CnetLmModel *model) {
    if (!model) return;
    btn_free(&model->btn);
    contract_free(&model->contract);
    if (model->has_cce) {
        cce_cascade_free(&model->cce);
    }
    if (model->gpu) {
        cce_gpu_destroy(model->gpu);
        model->gpu = NULL;
    }
    memset(model, 0, sizeof(*model));
}

double cnet_lm_train(CnetLmModel *model,
                     const char **sequences, size_t nseq,
                     size_t max_epochs, size_t growth_window,
                     double target_loss, double min_improvement,
                     const char *weights_path, const char *contract_path) {
    if (!model || !sequences || nseq == 0) return -1.0;
    (void)growth_window;
    (void)min_improvement;

    /* Build training table from sequences using current vocab */
    size_t max_pairs = 0;
    for (size_t s = 0; s < nseq; s++) {
        if (sequences[s]) max_pairs += strlen(sequences[s]);
    }
    if (max_pairs == 0) max_pairs = 64;

    double *inputs = (double*)calloc(max_pairs * (size_t)model->input_dim, sizeof(double));
    double *targets = (double*)calloc(max_pairs * (size_t)model->output_dim, sizeof(double));
    if (!inputs || !targets) {
        free(inputs); free(targets);
        return -1.0;
    }

    size_t pair_count = 0;
    for (size_t s = 0; s < nseq && pair_count < max_pairs; s++) {
        const char *str = sequences[s];
        if (!str) continue;
        /* Better tokenization for priority 3: split on space or treat as words/chars */
        char buf[256];
        strncpy(buf, str, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
        /* Improved tokenization: space or char fallback + subword-ish (merge common) */
        char *tok = strtok(buf, " ");
        char *prev_tok = NULL;
        while (tok && pair_count < max_pairs) {
            if (prev_tok) {
                int pi = vocab_index_internal(&model->vocab, prev_tok);
                int ni = vocab_index_internal(&model->vocab, tok);
                for (int v = 0; v < model->input_dim; v++)
                    inputs[pair_count * model->input_dim + v] = (v == pi ? 1.0 : 0.0);
                for (int v = 0; v < model->output_dim; v++)
                    targets[pair_count * model->output_dim + v] = (v == ni ? 0.9 : 0.1);
                pair_count++;
            }
            prev_tok = tok;
            tok = strtok(NULL, " ");
        }
        if (!prev_tok && strlen(str) > 0) {
            /* fallback char */
            for (size_t i=0; i+1<strlen(str) && pair_count<max_pairs; i++) {
                char p[2]={str[i],0}, n[2]={str[i+1],0};
                int pi = vocab_index_internal(&model->vocab, p);
                int ni = vocab_index_internal(&model->vocab, n);
                for (int v=0; v<model->input_dim; v++) inputs[pair_count*model->input_dim + v] = (v==pi?1.:0.);
                for (int v=0; v<model->output_dim; v++) targets[pair_count*model->output_dim + v] = (v==ni?0.9:0.1);
                pair_count++;
            }
        }
    }

    if (pair_count == 0) {
        free(inputs); free(targets);
        return -1.0;
    }

    /* Build CCE context-window pairs (B: align with A style for better coherence) */
    const int LM_CTX = 4;
    int cce_in_dim = LM_CTX * model->input_dim;
    // Collect per-sequence token id lists to build windows
    // For simplicity, re-scan the sequences (small)
    int *seq_ids = (int*)malloc( (max_pairs + nseq*2) * sizeof(int) ); // rough
    int seq_len = 0;
    for (size_t s = 0; s < nseq; s++) {
        const char *str = sequences[s];
        if (!str) continue;
        char buf[512];
        strncpy(buf, str, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
        char *tok = strtok(buf, " ");
        while (tok) {
            int id = vocab_index_internal(&model->vocab, tok);
            seq_ids[seq_len++] = id;
            tok = strtok(NULL, " ");
        }
        // add a separator if multiple seq
        seq_ids[seq_len++] = 0; // treat 0 as boundary
    }
    // Now build CTX pairs for CCE
    int cce_pair_count = 0;
    double *cce_inputs = (double*)calloc( (seq_len) * (size_t)cce_in_dim , sizeof(double));
    double *cce_targets = (double*)calloc( (seq_len) * (size_t)model->output_dim , sizeof(double));
    for (int i = LM_CTX; i < seq_len; i++) {
        if (seq_ids[i] == 0) continue; // skip boundaries
        for (int k=0; k<LM_CTX; k++) {
            int t = seq_ids[i - LM_CTX + k];
            if (t >= 0 && t < model->input_dim)
                cce_inputs[ cce_pair_count * cce_in_dim + k * model->input_dim + t ] = 1.0;
        }
        int ni = seq_ids[i];
        if (ni >=0 && ni < model->output_dim)
            cce_targets[ cce_pair_count * model->output_dim + ni ] = 0.9;
        cce_pair_count++;
    }

    /* CCE as primary */
    if (model->has_cce) {
        cce_cascade_free(&model->cce);
    }
    cce_cascade_init(&model->cce, 8);
    int hid = (model->input_dim < 40) ? 64 : 96;
    cce_block bb[7];
    cce_block_init_linear(&bb[0], cce_in_dim, hid, 0.01f);
    for (int i=1; i<5; i++) cce_block_init_linear(&bb[i], hid, hid, 0.01f);
    cce_block_init_linear(&bb[5], hid, model->output_dim, 0.01f);
    bb[5].type = CCE_BLOCK_LINEAR_HEAD;
    for (int i=0; i<=5; i++) cce_cascade_append(&model->cce, &bb[i]);

    /* Convert for CCE (CTX version) */
    float *finputs = (float*)malloc( (size_t)cce_pair_count * cce_in_dim * sizeof(float));
    float *ftargets = (float*)malloc( (size_t)cce_pair_count * (size_t)model->output_dim * sizeof(float));
    if (!finputs || !ftargets || cce_pair_count == 0) {
        free(finputs); free(ftargets);
        free(cce_inputs); free(cce_targets);
        cce_cascade_free(&model->cce);
        model->has_cce = false;
        free(inputs); free(targets);
        free(seq_ids);
        return -1.0;
    }
    for (size_t i=0; i< cce_pair_count * (size_t)cce_in_dim; i++) finputs[i] = (float)cce_inputs[i];
    for (size_t i=0; i< cce_pair_count * (size_t)model->output_dim; i++) ftargets[i] = (float)cce_targets[i];

    double loss = cce_train_dynamic(&model->cce, finputs, ftargets, cce_pair_count,
                                    cce_in_dim, model->output_dim,
                                    max_epochs, target_loss, 0.01f, 1 /*classify*/, model->gpu, 2 /*EXACT for quality on LM*/);

    model->has_cce = true;

    /* Keep BTN for backward compat (contracts etc), but CCE is the engine */
    btn_free(&model->btn);
    size_t hid2 = hid;
    if (btn_init(&model->btn, (size_t)model->input_dim, (size_t)model->output_dim,
                 hid2, hid2*2, 0.7, 424242u) != 0) {
        /* ignore */
    }
    Port pin = {PORT_ONEHOT, (size_t)model->input_dim, 1, ""};
    port_set_tag(&pin, "prev_token");
    Port pout = {PORT_ONEHOT, (size_t)model->output_dim, 1, ""};
    port_set_tag(&pout, "next_token");
    btn_set_ports(&model->btn, pin, pout);

    /* Note: for full CCE purity, generation uses model->cce directly */
    free(finputs);
    free(ftargets);
    free(cce_inputs);
    free(cce_targets);
    free(seq_ids);

    /* Build canon targets and contract */
    double *canon = (double*)malloc(pair_count * (size_t)model->output_dim * sizeof(double));
    if (canon) {
        for (size_t r = 0; r < pair_count; r++) {
            int tgt = 0;
            for (int v = 0; v < model->output_dim; v++) {
                if (targets[r*model->output_dim + v] > 0.5) tgt = v;
            }
            for (int v = 0; v < model->output_dim; v++) {
                canon[r*model->output_dim + v] = (v == tgt ? 1.0 : 0.0);
            }
        }
        contract_free(&model->contract);
        contract_init_borrowed(&model->contract, "cnet_lm_step",
                               &model->btn, inputs, canon, pair_count);

        if (weights_path) btn_save(&model->btn, weights_path);
        if (contract_path) contract_save(&model->contract, contract_path);
        free(canon);
    }

    free(inputs);
    free(targets);
    return loss;
}

static int lm_port_dim(const Port *port, int *dim_out) {
    if (!port || !dim_out || port->field_width == 0 || port->field_count == 0 ||
        port->field_width > CNET_LM_MAX_VOCAB / port->field_count) {
        return 0;
    }
    *dim_out = (int)(port->field_width * port->field_count);
    return *dim_out > 0 && *dim_out <= CNET_LM_MAX_VOCAB;
}

static int lm_dims_valid(size_t input_dim, size_t output_dim) {
    return input_dim > 0 && input_dim <= CNET_LM_MAX_VOCAB &&
           output_dim > 0 && output_dim <= CNET_LM_MAX_VOCAB;
}

/* btn_load allocates from dimensions stored in the artifact.  Validate its
   fixed header before handing the file to that general-purpose loader. */
static int lm_artifact_dims_valid(const char *path) {
    FILE *file;
    char magic[16];
    int version;
    unsigned long input_count;
    unsigned long output_count;
    unsigned long hidden_count;
    unsigned long max_hidden_count;
    double learning_rate;
    int parsed;

    file = fopen(path, "r");
    if (!file) return 0;
    parsed = fscanf(file, "%15s %d", magic, &version) == 2 &&
             strcmp(magic, "CNET_BTN") == 0 &&
             version >= 1 && version <= 5 &&
             fscanf(file, "%lu %lu %lu %lu %lf",
                    &input_count, &output_count, &hidden_count,
                    &max_hidden_count, &learning_rate) == 5;
    fclose(file);
    return parsed && lm_dims_valid((size_t)input_count, (size_t)output_count);
}

static int lm_load_fail(CnetLmModel *model) {
    btn_free(&model->btn);
    contract_free(&model->contract);
    model->input_dim = 0;
    model->output_dim = 0;
    return -1;
}

int cnet_lm_load(CnetLmModel *model,
                 const char *weights_path, const char *contract_path) {
    if (!model || !weights_path) return -1;
    if (!lm_artifact_dims_valid(weights_path)) return -1;

    btn_free(&model->btn);
    if (btn_load(&model->btn, weights_path) != 0) {
        return -1;
    }
    if (!lm_dims_valid(model->btn.input_count, model->btn.output_count))
        return lm_load_fail(model);

    /* Reconstruct ports minimally (assume onehot vocab sized) */
    model->input_dim = (int)model->btn.input_count;
    model->output_dim = (int)model->btn.output_count;

    if (contract_path) {
        contract_free(&model->contract);
        if (contract_load(&model->contract, contract_path) == 0) {
            int contract_dim = 0;
            if (model->contract.input_port_count > 0 &&
                (!lm_port_dim(&model->contract.input_ports[0], &contract_dim) ||
                 contract_dim != model->input_dim))
                return lm_load_fail(model);
            if (model->contract.output_port_count > 0 &&
                (!lm_port_dim(&model->contract.output_ports[0], &contract_dim) ||
                 contract_dim != model->output_dim))
                return lm_load_fail(model);
        }
    }

    /* Ensure vocab is reasonable size */
    if (model->vocab.size == 0) init_default_vocab(&model->vocab);
    if (model->input_dim > model->vocab.size) model->vocab.size = model->input_dim;

    return 0;
}

int cnet_lm_save(const CnetLmModel *model,
                 const char *weights_path, const char *contract_path) {
    if (!model) return -1;
    int ok = 0;
    if (weights_path) ok |= btn_save(&model->btn, weights_path);
    if (contract_path) ok |= contract_save(&model->contract, contract_path);
    return ok == 0 ? 0 : -1;
}

int cnet_lm_generate(const CnetLmModel *model,
                     const char *seed,
                     char *out, size_t max_steps) {
    if (!model || !seed || !out || max_steps == 0) return 0;
    if (!lm_dims_valid((size_t)model->input_dim, (size_t)model->output_dim) ||
        model->vocab.size <= 0 || model->vocab.size > CNET_LM_MAX_VOCAB) {
        out[0] = '\0';
        return 0;
    }

    /* Prefer pure CCE if available (B integration + A pure coherence) */
    if (model->has_cce) {
        const int LM_CTX = 4;
        int vsz = model->input_dim;
        int cce_in = LM_CTX * vsz;

        size_t olen = 0;
        while (olen + 1 < max_steps && seed[olen] != '\0') olen++;
        memmove(out, seed, olen);
        out[olen] = '\0';
        int ctx[8] = {0};
        int ctxl = LM_CTX;
        // seed from end of seed using token indices (char fallback for simplicity)
        for (int i = 0; i < ctxl; i++) {
            int off = (int)olen - ctxl + i;
            char ch = (off >= 0) ? out[off] : ' ';
            ctx[i] = cnet_lm_vocab_index(&model->vocab, (char[]){ch,0});
            if (ctx[i] < 0) ctx[i] = 0;
        }

        for (size_t step = 0; step < max_steps && olen + 2 < max_steps; step++) {
            float xin[512] = {0};
            for (int k=0; k<LM_CTX; k++) {
                int t = ctx[k];
                if (t >=0 && t < vsz) xin[k * vsz + t] = 1.0f;
            }
            cce_tensor tx; int xs[1] = {cce_in};
            if (cce_tensor_alloc(&tx, xs, 1) != CCE_OK) break;
            memcpy(tx.data, xin, (size_t)cce_in * sizeof(float));

            cce_tensor ty; cce_tensor_alloc(&ty, (int[]){vsz}, 1);
            cce_cascade_forward(&model->cce, &tx, &ty);

            /* nucleus + amp */
            float ww[128]; float sw = 0;
            for (int v=0; v<vsz; v++) {
                ww[v] = expf( (ty.data[v] * 3.0f) / 0.8f );
                sw += ww[v];
            }
            for (int v=0; v<vsz; v++) ww[v] /= (sw > 0 ? sw : 1);

            int id[128]; for(int v=0; v<vsz; v++) id[v]=v;
            for (int i=0; i<vsz-1; i++) for(int j=i+1; j<vsz; j++) if (ww[id[j]] > ww[id[i]]) {int t=id[i]; id[i]=id[j]; id[j]=t;}
            float cm = 0; int ne = 0;
            for (int i=0; i<vsz; i++) { cm += ww[id[i]]; ne = i+1; if (cm >= 0.9f) break; }
            if (ne < 1) ne=1;
            float rr = (float)rand() / RAND_MAX * (cm > 0 ? cm : 1);
            cm=0; int best = id[0];
            for (int i=0; i<ne; i++) { cm += ww[id[i]]; if (rr <= cm) { best = id[i]; break; } }

            const char *tok = (best >=0 && best < model->vocab.size) ? model->vocab.tokens[best] : " ";
            size_t tl = strlen(tok);
            if (tl < max_steps - olen) {
                memcpy(out + olen, tok, tl + 1);
                olen += tl;
            }

            /* shift ctx */
            for (int c=0; c<ctxl-1; c++) ctx[c] = ctx[c+1];
            ctx[ctxl-1] = best;

            cce_tensor_free(&ty);
            cce_tensor_free(&tx);
        }
        return (int)olen;
    }

    /* Legacy BTN path */
    out[0] = '\0';
    size_t len = 0;
    int start_idx = vocab_index_internal(&model->vocab, "once");
    if (start_idx < 0) start_idx = 0;
    if (model->vocab.size > 0) {
        size_t first_len = strlen(model->vocab.tokens[start_idx]);
        if (first_len < max_steps) {
            memcpy(out, model->vocab.tokens[start_idx], first_len + 1);
            len = first_len;
        }
    }
    int prev_idx = start_idx;
    int history[4] = { -1, -1, -1, -1 };  /* last 4 for anti-repeat circuit */
    history[0] = prev_idx;

    double cur_in[ CNET_LM_MAX_VOCAB * 2 ];
    double carried_hidden[ CNET_LM_MAX_VOCAB ] = {0};

    for (size_t step = 0; step < 30 && len + 5 < max_steps; step++) {
        memset(cur_in, 0, sizeof(cur_in));
        if (prev_idx >= 0 && prev_idx < model->input_dim) {
            cur_in[prev_idx] = 1.0;
        } else {
            cur_in[0] = 1.0;
        }

        /* state carry */
        for (int h = 0; h < model->output_dim && (model->input_dim + h) < (CNET_LM_MAX_VOCAB*2); h++) {
            cur_in[model->input_dim + h] = carried_hidden[h] * 0.5;
        }

        const double *pred = btn_forward((BinaryTransformNetwork*)&model->btn, cur_in);
        for (int h=0; h < model->output_dim && h < CNET_LM_MAX_VOCAB; h++) carried_hidden[h] = pred[h];

        /* Use reusable anti-repeat contract/circuit guard.
         * Proper contract logic (see contract_anti_repeat.h).
         */
        Port dummy_prev = {PORT_ONEHOT, 1, (size_t)model->vocab.size, "prev_token"};
        Port dummy_cand = {PORT_EVIDENCE, 1, (size_t)model->vocab.size, "candidate"};
        int best = 0;
        apply_anti_repeat_guard(&dummy_prev,
                                cur_in,
                                &dummy_cand,
                                pred,
                                model->vocab.size,
                                0.95,   /* very strong for this retrain */
                                1,
                                &best);

        /* Use history to avoid recent tokens (anti-repeat circuit in action) */
        if (best == history[0] || best == history[1] || best == history[2]) {
            /* force next best that is not recent */
            int alt = best;
            for (int tries = 0; tries < 5; tries++) {
                alt = (alt + 1) % model->vocab.size;
                if (alt != history[0] && alt != history[1] && alt != history[2] &&
                    !strchr(model->vocab.tokens[alt], '*')) {
                    best = alt;
                    break;
                }
            }
        }

        if (best < 0 || best >= model->vocab.size) best = (prev_idx + 1) % model->vocab.size;

        while (best < model->vocab.size && (strchr(model->vocab.tokens[best], '*') ||
               best == history[0] || best == history[1] || best == history[2])) {
            best = (best + 1) % model->vocab.size;
        }

        const char *tok = model->vocab.tokens[best];
        size_t add = strlen(tok);
        size_t add_space = len > 0 && out[len-1] != ' ' ? 1u : 0u;
        if (add_space + add >= max_steps - len) break;

        /* append with space for readability */
        if (add_space) out[len++] = ' ';
        memcpy(out + len, tok, add + 1);
        len += add;

        /* shift history for anti-repeat */
        history[3] = history[2];
        history[2] = history[1];
        history[1] = history[0];
        history[0] = best;
        prev_idx = best;
    }
    return (int)len;
}

int cnet_lm_step(const CnetLmModel *model,
                 int prev_idx, double *prev_vec, size_t vec_dim,
                 double *out_logits, size_t out_dim,
                 double *hidden_in, double *hidden_out, size_t hidden_dim) {
    if (!model || !out_logits) return -1;

    double local_in[ CNET_LM_MAX_VOCAB ] = {0};
    if (prev_vec && vec_dim > 0) {
        for (size_t i = 0; i < vec_dim && i < CNET_LM_MAX_VOCAB; i++) local_in[i] = prev_vec[i];
    } else if (prev_idx >= 0 && prev_idx < model->input_dim) {
        local_in[prev_idx] = 1.0;
    } else {
        local_in[0] = 1.0;
    }

    const double *pred = btn_forward((BinaryTransformNetwork*)&model->btn, local_in);
    for (size_t i = 0; i < out_dim && i < (size_t)model->output_dim; i++) {
        out_logits[i] = pred[i];
    }
    /* hidden carry stub for now (priority 2 later) */
    if (hidden_out && hidden_dim > 0 && hidden_in) {
        memcpy(hidden_out, hidden_in, hidden_dim * sizeof(double));
    }
    return 0;
}

int cnet_lm_add_vocab_token(CnetLmVocab *vocab, const char *tok) {
    if (!vocab || !tok || vocab->size >= CNET_LM_MAX_VOCAB) return -1;
    size_t len = strlen(tok);
    if (len >= CNET_LM_MAX_VOCAB_NAME) len = CNET_LM_MAX_VOCAB_NAME - 1;
    memcpy(vocab->tokens[vocab->size], tok, len);
    vocab->tokens[vocab->size][len] = '\0';
    return vocab->size++;
}

int cnet_lm_vocab_index(const CnetLmVocab *vocab, const char *tok) {
    return vocab_index_internal(vocab, tok);
}

void cnet_lm_head_registry_add(CnetLmHeadRegistry *reg, CnetLmModel *m, const char *name) {
    if (!reg || !m || reg->count >= 8) return;
    reg->heads[reg->count] = m;
    reg->names[reg->count] = name ? name : "head";
    reg->count++;
}

CnetLmModel *cnet_lm_route_head(const CnetLmHeadRegistry *reg, const char *query) {
    if (!reg || reg->count == 0 || !query) return reg ? reg->heads[0] : NULL;
    /* Very simple keyword routing for priority 5 */
    if (strstr(query, "fact") || strstr(query, "who") || strstr(query, "what")) {
        for (int i=0; i<reg->count; i++) if (strstr(reg->names[i], "fact")) return reg->heads[i];
    }
    if (strstr(query, "creat") || strstr(query, "story")) {
        for (int i=0; i<reg->count; i++) if (strstr(reg->names[i], "creat")) return reg->heads[i];
    }
    if (strstr(query, "speech")) {
        for (int i=0; i<reg->count; i++) if (strstr(reg->names[i], "speech")) return reg->heads[i];
    }
    return reg->heads[0];
}

int cnet_lm_speech_to_token(const double *speech_features, size_t nfeat, const CnetLmVocab *vocab) {
    if (!speech_features || nfeat == 0 || !vocab || vocab->size == 0) return 0;
    /* Crude hash to index for demo cross-modal (priority 7) */
    unsigned h = 2166136261u;
    for (size_t i=0; i<nfeat; i++) {
        int b = (int)(speech_features[i] * 10) & 0xFF;
        h ^= (unsigned)b; h *= 16777619u;
    }
    return (int)(h % (unsigned)vocab->size);
}

void cnet_lm_benchmark(const CnetLmModel *model, const char **heldout, size_t nheld,
                       int num_runs, double *out_avg_time_ms, double *out_avg_len) {
    if (!out_avg_time_ms || !out_avg_len) return;
    *out_avg_time_ms = 0.0;
    *out_avg_len = 0.0;
    if (!model || !heldout || nheld==0 || num_runs <= 0) return;

    clock_t t0 = clock();
    int total_len = 0;
    int matches = 0;

    for (int run=0; run < num_runs; run++) {
        for (size_t h=0; h < nheld; h++) {
            if (!heldout[h]) continue;
            char buf[256] = {0};
            strncpy(buf, heldout[h], 8); /* short seed */
            int gl = cnet_lm_generate(model, buf, buf, 32);
            total_len += gl;
            /* rough: if starts with seed consider partial match */
            if (strncmp(buf, heldout[h], 4) == 0) matches++;
        }
    }
    clock_t t1 = clock();
    double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
    *out_avg_time_ms = (secs / (num_runs * nheld)) * 1000.0;
    *out_avg_len = (double)total_len / (num_runs * nheld);
    /* For CHECK later we can use matches */
}

int cnet_lm_vocab_index_public(const CnetLmVocab *v, const char *t) {
    return cnet_lm_vocab_index(v, t);
}

/* === Train from multiple Hugging Face datasets (https://huggingface.co/datasets) ===
 * Fetches via agent_open_text_source (curl for remote), parses JSONL or plain text lines.
 * Dynamically builds vocab from words in the data for unique, meaningful text generation.
 * Limits lines for fast training in C demos.
 * Uses word-level tokens (split on spaces) -> much better unique generations than char.
 */

static void to_lower_simple(char *s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') *s = c + 32;
    }
}

static int collect_words(const char *text, char words[][32], int max_words) {
    if (!text) return 0;
    char buf[1024];
    strncpy(buf, text, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    int cnt = 0;
    char *tok = strtok(buf, " \t\n\r.,;:!?\"'()[]{}");
    while (tok && cnt < max_words) {
        if (strlen(tok) > 2 && strchr(tok, '*') == NULL && strchr(tok, '#') == NULL) {  /* clean for unique text */
            to_lower_simple(tok);
            int dup = 0;
            for (int i=0; i<cnt; i++) if (strcmp(words[i], tok)==0) {dup=1; break;}
            if (!dup) {
                strncpy(words[cnt], tok, 31); words[cnt][31]=0;
                cnt++;
            }
        }
        tok = strtok(NULL, " \t\n\r.,;:!?\"'()[]{}");
    }
    return cnt;
}

double cnet_lm_train_from_multiple_hf(CnetLmModel *model,
                                      const char **urls, size_t n_urls,
                                      size_t max_lines_total,
                                      size_t max_epochs, size_t growth_window,
                                      double target_loss, double min_improvement,
                                      const char *weights_path, const char *contract_path) {
    if (!model || !urls || n_urls == 0) return -1.0;

    /* 1. Fetch and collect raw text lines from multiple HF datasets */
    char all_text[32768] = {0};  /* accumulate for vocab + sequences */
    size_t lines_read = 0;
    size_t text_len = 0;

    for (size_t u = 0; u < n_urls && lines_read < max_lines_total; u++) {
        const char *url = urls[u];
        /* local fetch using curl (same pattern as build tool for HF) */
        FILE *f = NULL;
        if (strstr(url, "http") == url) {
            char cmd[8192];
            snprintf(cmd, sizeof(cmd), "curl.exe -L --silent --show-error \"%s\"", url);
            f = popen(cmd, "r");
        } else {
            f = fopen(url, "r");
        }
        if (!f) {
            /* fallback if no net */
            continue;
        }

        char line[2048];
        while (fgets(line, sizeof(line), f) && lines_read < max_lines_total) {
            lines_read++;
            /* Parse: try JSONL first for common HF (fable style) */
            char extracted[1024] = {0};
            int got = 0;
            /* simple extract without full func */
            const char *keys[] = {"context", "completion", "text", "sentence", "sentence1", NULL};
            for (int k=0; keys[k] && !got; k++) {
                char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", keys[k]);
                const char *p = strstr(line, pat);
                if (p) {
                    p += strlen(pat);
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p == ':') { p++; while (*p && isspace((unsigned char)*p)) p++; }
                    if (*p == '"') {
                        p++;
                        size_t el=0;
                        while (*p && *p != '"' && el < sizeof(extracted)-1) {
                            if (*p == '\\' && p[1]) p++;
                            extracted[el++] = *p++;
                        }
                        extracted[el] = 0;
                        got = 1;
                    }
                }
            }
            if (!got) {
                size_t copy_len = strlen(line);
                if (copy_len >= sizeof(extracted)) copy_len = sizeof(extracted) - 1;
                memcpy(extracted, line, copy_len);
                extracted[copy_len] = '\0';
                got = 1;
            }
            if (got && strlen(extracted) > 3) {
                size_t elen = strlen(extracted);
                if (text_len + elen + 2 < sizeof(all_text)) {
                    strcat(all_text, extracted);
                    strcat(all_text, " ");
                    text_len += elen + 1;
                }
            }
        }
        if (strstr(url, "http") == url) pclose(f); else fclose(f);
    }

    if (text_len < 20) {
        /* fallback data if no net or empty */
        strcpy(all_text, "the cat sat on the mat . a quick brown fox jumps . story tale once upon a time lm generate concept speech token fable trace context completion ");
        text_len = strlen(all_text);
    }

    /* 2. Build dynamic vocab from the collected text (word level for unique generations) */
    char collected_words[128][32];
    int vsize = collect_words(all_text, collected_words, 128);
    if (vsize < 4) vsize = 4; /* min */

    /* reset and set model vocab */
    memset(&model->vocab, 0, sizeof(model->vocab));
    model->vocab.size = 0;
    for (int i = 0; i < vsize && model->vocab.size < CNET_LM_MAX_VOCAB; i++) {
        if (cnet_lm_add_vocab_token(&model->vocab, collected_words[i]) < 0) break;
    }

    model->input_dim = model->vocab.size;
    model->output_dim = model->vocab.size;

    /* 3. Split all_text into sequence strings (by . or length for training pairs) */
    char *seq_list[256];
    int nseq = 0;
    char work[32768];
    memcpy(work, all_text, sizeof(work));
    work[sizeof(work)-1] = '\0';
    char *seg = strtok(work, ".");
    while (seg && nseq < 256) {
        /* clean and keep if substantial */
        while (*seg == ' ') seg++;
        if (strlen(seg) > 5) {
            seq_list[nseq] = strdup(seg); /* caller frees below */
            nseq++;
        }
        seg = strtok(NULL, ".");
    }
    if (nseq == 0) {
        seq_list[0] = strdup("the cat sat on the mat a quick brown fox story");
        nseq = 1;
    }

    /* 4. Train using the dynamic word vocab */
    double loss = cnet_lm_train(model, (const char**)seq_list, (size_t)nseq,
                                max_epochs, growth_window, target_loss, min_improvement,
                                weights_path, contract_path);

    /* cleanup */
    for (int i=0; i<nseq; i++) free(seq_list[i]);

    return loss;
}
