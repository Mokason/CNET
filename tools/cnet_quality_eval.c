/* Real-model quality + latency eval for CNET stack (MTK, sparse, paging).
 *
 * Usage:
 *   bin/cnet_quality_eval <gguf> [n_gen] [prompt_token_ids...]
 * Env:
 *   CNET_GOV_PROFILE=eco|balanced|turbo
 *   CNET_MTK_ROUTES=...
 *   CNET_SPARSE_KV / CNET_DSA / CNET_KV_PAGE set by gov profile
 *   CNET_QUALITY_SKILL=/path.cmsk   optional skill to compare base vs skill
 *
 * Reports: open_ms, gen_ms, tok/s, base vs skill argmax agreement, finite
 * logits, optional detokenize via tokenizer dump if available.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_mtk_host.h"
#include "../include/resource_governor.h"

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int argmax_f(const float *v, int n) {
    int i, b = 0;
    if (n < 1) return 0;
    for (i = 1; i < n; ++i)
        if (v[i] > v[b]) b = i;
    return b;
}

static int finite_vec(const float *v, int n) {
    int i;
    for (i = 0; i < n; ++i)
        if (!isfinite(v[i])) return 0;
    return 1;
}

/* Best-effort: load token strings from GGUF metadata via cce_gguf raw. */
static void try_print_tokens(const char *gguf_path, const int *ids, int n) {
    cce_gguf *g = NULL;
    int i;
    if (cce_gguf_load(gguf_path, &g) != CCE_OK || !g) {
        printf("  (no detokenize — raw ids only)\n");
        return;
    }
    printf("  text_approx: ");
    for (i = 0; i < n; ++i) {
        /* Prefer piece API if present; else print id. */
        const char *piece = NULL;
#ifdef CCE_HAS_TOKEN_PIECE
        piece = cce_gguf_token_to_piece(g, ids[i]);
#endif
        if (piece && piece[0])
            printf("%s", piece);
        else
            printf("[%d]", ids[i]);
    }
    printf("\n");
    cce_gguf_free(g);
    (void)ids;
}

int main(int argc, char **argv) {
    const char *path;
    int n_gen = 16;
    int prompt[64];
    int n_prompt = 0;
    int out[128];
    int out_n = 0;
    cce_mtk_host *h = NULL;
    cce_gguf_qwen2 *m;
    float *logits = NULL;
    double t0, t1, open_ms, gen_ms;
    int V, i, agree = 0, finite_ok = 1;
    int base_ids[64], skill_ids[64];
    int n_cmp = 0;
    const char *skill = getenv("CNET_QUALITY_SKILL");
    int checks = 0, fails = 0;

#define CHECK(c, msg)                                                          \
    do {                                                                       \
        checks++;                                                              \
        if (c)                                                                 \
            printf("  ok  %s\n", msg);                                         \
        else {                                                                 \
            fails++;                                                           \
            printf("  FAIL %s\n", msg);                                        \
        }                                                                      \
    } while (0)

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <model.gguf> [n_gen] [prompt_ids...]\n"
                "env: CNET_GOV_PROFILE CNET_QUALITY_SKILL CNET_MTK_ROUTES\n",
                argv[0]);
        return 2;
    }
    path = argv[1];
    if (argc > 2) n_gen = atoi(argv[2]);
    if (n_gen < 1) n_gen = 1;
    if (n_gen > 64) n_gen = 64;
    if (argc > 3) {
        for (i = 3; i < argc && n_prompt < 64; ++i)
            prompt[n_prompt++] = atoi(argv[i]);
    } else {
        /* Default BOS-ish prompt ids (model-dependent; still valid decode). */
        prompt[0] = 1;
        prompt[1] = 198;
        prompt[2] = 271;
        prompt[3] = 11;
        n_prompt = 4;
    }

    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    if (!getenv("CNET_GOV_PROFILE")) setenv("CNET_GOV_PROFILE", "balanced", 1);
    if (!getenv("CNET_MAX_CTX")) setenv("CNET_MAX_CTX", "256", 1);

    printf("== CNET quality eval ==\n");
    printf("  model=%s n_gen=%d prompt_n=%d profile=%s\n", path, n_gen,
           n_prompt, getenv("CNET_GOV_PROFILE"));

    t0 = wall_ms();
    if (cce_mtk_host_open(&h, path, getenv("CNET_MTK_ROUTES")) != CCE_OK ||
        !h) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    open_ms = wall_ms() - t0;
    m = cce_mtk_host_model(h);
    V = m->vocab_size > 0 ? m->vocab_size : 1;
    printf("  open_ms=%.1f layers=%d d=%d vocab=%d mtk_sites=%d\n", open_ms,
           m->n_layer, m->n_embd, V, cce_mtk_n_sites(cce_mtk_host_mtk(h)));
    CHECK(m->n_layer > 0 && m->n_embd > 0, "model hparams live");
    CHECK(cce_mtk_n_sites(cce_mtk_host_mtk(h)) >= 1, "mtk sites bound");

    /* Baseline generate */
    t0 = wall_ms();
    if (cce_mtk_host_generate(h, prompt, n_prompt, n_gen, out, &out_n) !=
        CCE_OK) {
        fprintf(stderr, "generate failed\n");
        cce_mtk_host_close(h);
        return 1;
    }
    gen_ms = wall_ms() - t0;
    CHECK(out_n == n_prompt + n_gen, "generate length");
    printf("  gen_ms=%.1f tok_s=%.2f (prompt=%d new=%d)\n", gen_ms,
           gen_ms > 0 ? 1000.0 * (double)n_gen / gen_ms : 0.0, n_prompt, n_gen);
    printf("  ids:");
    for (i = 0; i < out_n; ++i) printf(" %d", out[i]);
    printf("\n");
    try_print_tokens(path, out, out_n);

    /* Finite logits on last step */
    logits = (float *)malloc((size_t)V * sizeof(float));
    if (logits) {
        int last = out[out_n - 1];
        m->cur_pos = out_n > 0 ? out_n - 1 : 0;
        if (cce_gguf_qwen2_forward(m, &last, 1, logits, V) == CCE_OK) {
            finite_ok = finite_vec(logits, V);
            CHECK(finite_ok, "logits finite");
            printf("  last_argmax=%d max_logit=%.4g\n", argmax_f(logits, V),
                   (double)logits[argmax_f(logits, V)]);
        } else {
            CHECK(0, "last forward");
        }
    }

    /* Optional skill vs base quality delta */
    if (skill && skill[0] && logits) {
        n_cmp = n_gen < 8 ? n_gen : 8;
        (void)cce_mtk_host_revert(h);
        m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, prompt, n_prompt, logits, V) == CCE_OK) {
            int t = argmax_f(logits, V);
            for (i = 0; i < n_cmp; ++i) {
                base_ids[i] = t;
                if (cce_gguf_qwen2_forward(m, &t, 1, logits, V) != CCE_OK)
                    break;
                t = argmax_f(logits, V);
            }
        }
        if (cce_mtk_host_apply_skill(h, skill, 1.f) == CCE_OK) {
            m->cur_pos = 0;
            if (cce_gguf_qwen2_forward(m, prompt, n_prompt, logits, V) ==
                CCE_OK) {
                int t = argmax_f(logits, V);
                for (i = 0; i < n_cmp; ++i) {
                    skill_ids[i] = t;
                    if (cce_gguf_qwen2_forward(m, &t, 1, logits, V) != CCE_OK)
                        break;
                    t = argmax_f(logits, V);
                }
            }
            for (i = 0; i < n_cmp; ++i)
                if (base_ids[i] == skill_ids[i]) agree++;
            printf("  skill_vs_base argmax_agree=%d/%d (%.0f%%) skill=%s\n",
                   agree, n_cmp, n_cmp ? 100.0 * agree / n_cmp : 0.0, skill);
            CHECK(n_cmp > 0, "skill compare ran");
            /* Skill should change at least something OR be a no-op skill —
               report only; do not fail if skill is empty for this model. */
            if (agree == n_cmp)
                printf("  note: skill did not change greedy path "
                       "(sites may not match model names)\n");
            else
                printf("  note: skill altered greedy generation "
                       "(knowledge swap live)\n");
        } else {
            printf("  skill apply failed (path or site mismatch)\n");
        }
        (void)cce_mtk_host_revert(h);
    }

    free(logits);
    cce_mtk_host_close(h);
    printf("QUALITY_EVAL_PASS checks=%d fails=%d open_ms=%.1f gen_tok_s=%.2f\n",
           checks, fails,
           open_ms, gen_ms > 0 ? 1000.0 * (double)n_gen / gen_ms : 0.0);
    return fails ? 1 : 0;
}
