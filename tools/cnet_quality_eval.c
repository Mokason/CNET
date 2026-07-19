/* Real-model quality + latency eval for CNET stack (MTK, sparse, paging).
 *
 * Usage:
 *   bin/cnet_quality_eval <gguf> [n_gen] [prompt_token_ids...]
 *   bin/cnet_quality_eval <gguf> [n_gen]          # English chat default
 *
 * Env:
 *   CNET_GOV_PROFILE=eco|balanced|turbo
 *   CNET_MTK_ROUTES=...
 *   CNET_SPARSE_KV / CNET_DSA / CNET_KV_PAGE set by gov profile
 *   CNET_QUALITY_SKILL=/path.cmsk   optional skill to compare base vs skill
 *   CNET_QUALITY_PROMPT="..."      English user text (chat-templated)
 *   CNET_QUALITY_EXPECTED="..."    bounded answer token required in output
 *   CNET_QUALITY_SYSTEM="..."      optional system (still adds Qwythos identity)
 *   CNET_QUALITY_NO_THINK=1        empty <think></think> before answer
 *   CNET_QUALITY_RAW_IDS=1         force legacy raw-id prompt path
 *   CNET_LADDER_IMPORT=/path.ldtr  load certified trit specialists (converted)
 *
 * Reports: open_ms, gen_ms, tok/s, English detokenized text, base vs skill
 * argmax agreement, finite logits.
 */
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_gguf_tok.h"
#include "../include/cce/cce_mtk_host.h"
#include "../include/cce/cce_spec_ladder.h"
#include "../include/resource_governor.h"

#define QE_MAX_PROMPT 2048
#define QE_MAX_OUT    4096
#define QE_MAX_TEXT   16384

static int contains_expected_token(const char *text, const char *expected) {
    const char *p;
    size_t n;
    if (!text || !expected || !expected[0]) return 0;
    n = strlen(expected);
    for (p = text; (p = strstr(p, expected)) != NULL; ++p) {
        unsigned char before = p == text ? 0 : (unsigned char)p[-1];
        unsigned char after = (unsigned char)p[n];
        if ((p == text || !isalnum(before)) &&
            (after == 0 || !isalnum(after))) return 1;
    }
    return 0;
}

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int want_gpu_check(void) {
    const char *e = getenv("CNET_GPU");
    if (e && e[0] == '0' && e[1] == 0) return 0;
    if (e && (strcmp(e, "off") == 0 || strcmp(e, "cpu") == 0)) return 0;
    /* Only hard-fail GPU check when explicitly requested */
    return e && (e[0] == '1' || strcmp(e, "require") == 0);
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

static void print_english(const cce_gguf_tok *tok, const int *ids, int n,
                          const char *label, int skip_prompt_n) {
    char text[QE_MAX_TEXT];
    const int *p = ids;
    int pn = n;
    if (skip_prompt_n > 0 && skip_prompt_n < n) {
        p = ids + skip_prompt_n;
        pn = n - skip_prompt_n;
    }
    if (!tok) {
        printf("  %s: (no tokenizer — raw ids)", label);
        {
            int i;
            for (i = 0; i < pn && i < 32; ++i) printf(" %d", p[i]);
            if (pn > 32) printf(" …");
        }
        printf("\n");
        return;
    }
    (void)cce_gguf_tok_decode(tok, p, pn, text, (int)sizeof text, 1);
    printf("  %s: %s\n", label, text);
}

int main(int argc, char **argv) {
    const char *path;
    int n_gen = 32;
    int prompt[QE_MAX_PROMPT];
    int n_prompt = 0;
    int out[QE_MAX_OUT];
    int out_n = 0;
    cce_mtk_host *h = NULL;
    cce_gguf_qwen2 *m;
    cce_gguf_tok *tok = NULL;
    float *logits = NULL;
    double t0, t1, open_ms, gen_ms, tok_ms = 0;
    int V, i, agree = 0, finite_ok = 1;
    int base_ids[64], skill_ids[64];
    int n_cmp = 0;
    const char *skill = getenv("CNET_QUALITY_SKILL");
    const char *qprompt = getenv("CNET_QUALITY_PROMPT");
    const char *qsys = getenv("CNET_QUALITY_SYSTEM");
    const char *expected = getenv("CNET_QUALITY_EXPECTED");
    int no_think = getenv("CNET_QUALITY_NO_THINK") &&
                   getenv("CNET_QUALITY_NO_THINK")[0] == '1';
    int raw_ids = getenv("CNET_QUALITY_RAW_IDS") &&
                  getenv("CNET_QUALITY_RAW_IDS")[0] == '1';
    int checks = 0, fails = 0;
    int used_chat = 0;
    char tmpl[QE_MAX_TEXT];

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

    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        CHECK(contains_expected_token("4", "4"), "exact numeric answer accepted");
        CHECK(contains_expected_token("Answer: 4.", "4"), "bounded answer token accepted");
        CHECK(!contains_expected_token("44", "4"), "substring false positive rejected");
        CHECK(!contains_expected_token("14 things", "4"), "embedded digit rejected");
        CHECK(!contains_expected_token("", "4"), "empty generation rejected");
        printf("QUALITY_EXPECTED_SELFTEST %s checks=%d fails=%d\n",
               fails ? "FAIL" : "PASS", checks, fails);
        return fails ? 1 : 0;
    }

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <model.gguf> [n_gen] [prompt_ids...]\n"
                "env: CNET_GOV_PROFILE CNET_QUALITY_SKILL CNET_MTK_ROUTES\n"
                "     CNET_QUALITY_PROMPT CNET_QUALITY_SYSTEM "
                "CNET_QUALITY_NO_THINK CNET_QUALITY_RAW_IDS\n",
                argv[0]);
        return 2;
    }
    path = argv[1];
    if (argc > 2) n_gen = atoi(argv[2]);
    if (n_gen < 1) n_gen = 1;
    if (n_gen > 256) n_gen = 256;
    if (argc > 3) {
        raw_ids = 1;
        for (i = 3; i < argc && n_prompt < QE_MAX_PROMPT; ++i)
            prompt[n_prompt++] = atoi(argv[i]);
    }

    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    if (!getenv("CNET_GOV_PROFILE")) setenv("CNET_GOV_PROFILE", "balanced", 1);
    if (!getenv("CNET_MAX_CTX")) setenv("CNET_MAX_CTX", "512", 1);

    printf("== CNET quality eval ==\n");
    printf("  model=%s n_gen=%d profile=%s\n", path, n_gen,
           getenv("CNET_GOV_PROFILE"));

    /* Load GGUF tokenizer first (English path). */
    t0 = wall_ms();
    if (cce_gguf_tok_load(path, &tok) == CCE_OK && tok) {
        tok_ms = wall_ms() - t0;
        printf("  tokenizer vocab=%d eos=%d load_ms=%.1f\n",
               cce_gguf_tok_vocab_size(tok), cce_gguf_tok_eos_id(tok), tok_ms);
        CHECK(cce_gguf_tok_vocab_size(tok) > 1000, "tokenizer loaded");
    } else {
        printf("  tokenizer: unavailable (will print raw ids)\n");
        CHECK(0, "tokenizer loaded");
    }

    if (!raw_ids && tok) {
        if (!qprompt || !qprompt[0]) {
            qprompt = "What is 2+2? Answer with just the number.";
            if (!expected || !expected[0]) expected = "4";
        }
        {
            int tn = cce_gguf_tok_chat_template(tmpl, (int)sizeof tmpl, qsys,
                                                qprompt, no_think ? 0 : 1);
            CHECK(tn > 0, "chat template built");
            n_prompt = cce_gguf_tok_encode_chat(tok, qsys, qprompt,
                                                no_think ? 0 : 1, prompt,
                                                QE_MAX_PROMPT);
            CHECK(n_prompt > 4, "chat prompt encoded");
            used_chat = n_prompt > 0;
            printf("  chat_prompt_n=%d think=%s\n", n_prompt,
                   no_think ? "off" : "on");
            printf("  user: %s\n", qprompt);
            {
                char preview[512];
                int pl = tn < 200 ? tn : 200;
                if (tn > 0) {
                    memcpy(preview, tmpl, (size_t)pl);
                    preview[pl] = 0;
                    printf("  template_head: %s%s\n", preview,
                           tn > 200 ? "…" : "");
                }
            }
        }
    } else if (n_prompt == 0) {
        /* Legacy BOS-ish ids */
        prompt[0] = 1;
        prompt[1] = 198;
        prompt[2] = 271;
        prompt[3] = 11;
        n_prompt = 4;
        printf("  mode=raw_ids prompt_n=%d\n", n_prompt);
    } else {
        printf("  mode=raw_ids prompt_n=%d\n", n_prompt);
    }

    t0 = wall_ms();
    if (cce_mtk_host_open(&h, path, getenv("CNET_MTK_ROUTES")) != CCE_OK ||
        !h) {
        fprintf(stderr, "open failed\n");
        cce_gguf_tok_free(tok);
        return 1;
    }
    open_ms = wall_ms() - t0;
    m = cce_mtk_host_model(h);
    V = m->vocab_size > 0 ? m->vocab_size : 1;
    printf("  open_ms=%.1f layers=%d d=%d vocab=%d mtk_sites=%d\n", open_ms,
           m->n_layer, m->n_embd, V, cce_mtk_n_sites(cce_mtk_host_mtk(h)));
    printf("  gpu=%s name=%s oracle_int8=%s\n",
           cce_mtk_host_gpu_active(h) ? "ON" : "OFF",
           cce_mtk_host_gpu_name(h),
           (getenv("CNET_ORACLE_INT8") && getenv("CNET_ORACLE_INT8")[0] == '1')
               ? "1"
               : "0");
    {
        const char *ldtr = getenv("CNET_LADDER_IMPORT");
        if (ldtr && ldtr[0] && m->forest) {
            int nr = cce_ladder_import_certified(m->forest, ldtr);
            printf("  ldtr_import %s loaded=%d\n", ldtr, nr);
            /* Empty pack is OK (progressive conversion mid-flight). */
            if (nr < 1)
                printf("  note: empty/no specialists in LDTR (FP baseline)\n");
        } else if (ldtr && ldtr[0] && !m->forest) {
            printf("  ldtr_import skipped (no forest)\n");
        }
    }
    CHECK(m->n_layer > 0 && m->n_embd > 0, "model hparams live");
    CHECK(cce_mtk_n_sites(cce_mtk_host_mtk(h)) >= 1, "mtk sites bound");
    if (want_gpu_check())
        CHECK(cce_mtk_host_gpu_active(h), "GPU attached");
    if (tok && cce_gguf_tok_vocab_size(tok) > 0 && V > 0)
        CHECK(cce_gguf_tok_vocab_size(tok) == V ||
                  abs(cce_gguf_tok_vocab_size(tok) - V) < 64,
              "tokenizer vocab ~ model vocab");

    /* Ensure ctx large enough for chat prompt + gen */
    if (n_prompt + n_gen > 256) {
        char buf[32];
        snprintf(buf, sizeof buf, "%d", n_prompt + n_gen + 64);
        /* max ctx already set at open; warn only */
        if (m->max_ctx > 0 && n_prompt + n_gen > m->max_ctx)
            printf("  warn: prompt+gen (%d) may exceed max_ctx (%d)\n",
                   n_prompt + n_gen, m->max_ctx);
        (void)buf;
    }

    t0 = wall_ms();
    if (cce_mtk_host_generate(h, prompt, n_prompt, n_gen, out, &out_n) !=
        CCE_OK) {
        fprintf(stderr, "generate failed\n");
        cce_mtk_host_close(h);
        cce_gguf_tok_free(tok);
        return 1;
    }
    gen_ms = wall_ms() - t0;
    CHECK(out_n == n_prompt + n_gen, "generate length");
    {
        double pms = cce_mtk_host_last_prefill_ms(h);
        double dms = cce_mtk_host_last_decode_ms(h);
        double decode_tps =
            dms > 0 ? 1000.0 * (double)n_gen / dms : 0.0;
        double wall_tps =
            gen_ms > 0 ? 1000.0 * (double)n_gen / gen_ms : 0.0;
        printf("  gen_ms=%.1f wall_tok_s=%.2f (prompt=%d new=%d)\n", gen_ms,
               wall_tps, n_prompt, n_gen);
        printf("  prefill_ms=%.1f decode_ms=%.1f decode_tok_s=%.2f "
               "prefill_tok_s=%.2f\n",
               pms, dms, decode_tps,
               pms > 0 ? 1000.0 * (double)n_prompt / pms : 0.0);
    }

    printf("  ids_new:");
    for (i = n_prompt; i < out_n && i < n_prompt + 48; ++i)
        printf(" %d", out[i]);
    if (out_n - n_prompt > 48) printf(" …");
    printf("\n");

    print_english(tok, out, out_n, "text_full", 0);
    print_english(tok, out, out_n, "text_gen", n_prompt);
    if (used_chat && tok) {
        char gen[QE_MAX_TEXT];
        (void)cce_gguf_tok_decode(tok, out + n_prompt, out_n - n_prompt, gen,
                                  (int)sizeof gen, 1);
        CHECK(gen[0] != 0, "generation detokenizes to non-empty text");
        if (expected && expected[0])
            CHECK(contains_expected_token(gen, expected),
                  "generated answer contains the expected bounded token");
    }

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
            if (agree == n_cmp)
                printf("  note: skill did not change greedy path "
                       "(sites may not match model names)\n");
            else
                printf("  note: skill altered greedy generation "
                       "(knowledge swap live)\n");
            if (tok) {
                print_english(tok, base_ids, n_cmp, "base_greedy", 0);
                print_english(tok, skill_ids, n_cmp, "skill_greedy", 0);
            }
        } else {
            printf("  skill apply failed (path or site mismatch)\n");
        }
        (void)cce_mtk_host_revert(h);
    }

    {
        double dms = cce_mtk_host_last_decode_ms(h);
        double decode_tps = dms > 0 ? 1000.0 * (double)n_gen / dms : 0.0;
        int gpu_on = cce_mtk_host_gpu_active(h);
        free(logits);
        cce_mtk_host_close(h);
        cce_gguf_tok_free(tok);
        printf("QUALITY_EVAL_%s checks=%d fails=%d open_ms=%.1f "
               "decode_tok_s=%.2f wall_tok_s=%.2f chat=%d gpu=%d\n",
               fails ? "FAIL" : "PASS", checks, fails, open_ms, decode_tps,
               gen_ms > 0 ? 1000.0 * (double)n_gen / gen_ms : 0.0, used_chat,
               gpu_on);
    }
    (void)t1;
    return fails ? 1 : 0;
}
