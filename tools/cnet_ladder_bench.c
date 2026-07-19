/* Ladder comparison + throughput bench on tiny GGUF (hermetic) or real model.
 *
 * Builds a tiny qwen2 GGUF, then measures:
 *   A) FP baseline logits + decode wall
 *   B) Whole-forest posthoc ternary (old path)
 *   C) STE schedule with capture-hook real activations
 *   D) Export/import certified pack round-trip
 *
 * Usage:
 *   bin/cnet_ladder_bench              # tiny hermetic
 *   bin/cnet_ladder_bench path.gguf    # real (slow load)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_spec_ladder.h"
#include "../tests/tiny_model_fixture.h"

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static double relerr_vec(const float *a, const float *b, int n) {
    double num = 0, den = 0;
    int i;
    for (i = 0; i < n; ++i) {
        double d = (double)a[i] - (double)b[i];
        num += d * d;
        den += (double)b[i] * (double)b[i];
    }
    return den > 0 ? sqrt(num / den) : 0.0;
}

static int argmax_f(const float *v, int n) {
    int i, b = 0;
    for (i = 1; i < n; ++i)
        if (v[i] > v[b]) b = i;
    return b;
}

static double decode_tps(cce_gguf_qwen2 *m, int n_tok, float *lg) {
    int *toks, t;
    double t0, t1;
    if (!m || n_tok < 1) return 0;
    toks = (int *)malloc((size_t)n_tok * sizeof(int));
    if (!toks) return 0;
    for (t = 0; t < n_tok; ++t) toks[t] = (t * 7 + 3) % TL_V;
    m->cur_pos = 0;
    t0 = wall_ms();
    if (cce_gguf_qwen2_forward(m, &toks[0], 1, lg, m->vocab_size) != CCE_OK) {
        free(toks);
        return 0;
    }
    for (t = 1; t < n_tok; ++t) {
        if (m->cur_pos >= m->max_ctx) m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, &toks[t], 1, lg, m->vocab_size) !=
            CCE_OK)
            break;
    }
    t1 = wall_ms();
    free(toks);
    return t1 > t0 ? 1000.0 * (double)n_tok / (t1 - t0) : 0.0;
}

static void clone_logits(cce_gguf_qwen2 *m, const int *seq, int n, float *out) {
    m->cur_pos = 0;
    (void)cce_gguf_qwen2_forward(m, seq, n, out, m->vocab_size);
}

static int count_packed(cce_forest *f) {
    int i, n, c = 0;
    n = cce_forest_branch_count(f);
    for (i = 0; i < n; ++i) {
        char name[160];
        cce_cascade *cas;
        if (cce_forest_branch_name(f, i, name, (int)sizeof name) != CCE_OK)
            continue;
        cas = cce_forest_get_resident(f, name);
        if (cas && cas->num_blocks > 0 && cas->blocks[0].w_trit) c++;
    }
    return c;
}

static int run_tiny(void) {
    tl_weights *w;
    tl_entry ents[128];
    int n_ents, seq_eval[6] = {1, 3, 5, 7, 2, 4};
    int cal[8][8], s, t;
    char gguf_tmpl[] = "/tmp/cnet_ladder_bench_XXXXXX";
    char gguf_path[160];
    char pack_tmpl[] = "/tmp/cnet_ladder_cert_XXXXXX";
    char pack_path[160];
    int fd;
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *m;
    float *lg_fp = NULL, *lg_ph = NULL, *lg_st = NULL, *lg_tmp = NULL;
    cce_ladder_bank bank;
    cce_ladder_cfg cfg;
    cce_ladder_report reps[64];
    int n_rep, cert = 0, i, agree_ph = 0, agree_st = 0;
    double e_ph, e_st, tps_fp, tps_ph, tps_st;
    int n_stages = 0;
    const cce_ladder_stage *sched;
    uint64_t rng = 0xA5A5A5A5ULL;

    printf("== ladder bench (tiny GGUF) ==\n");
    w = (tl_weights *)malloc(sizeof *w);
    if (!w) return 1;
    tl_gen(w, 0);
    n_ents = tl_entries(w, ents, 0);
    fd = mkstemp(gguf_tmpl);
    if (fd < 0) {
        free(w);
        return 1;
    }
    close(fd);
    unlink(gguf_tmpl);
    snprintf(gguf_path, sizeof gguf_path, "%s.gguf", gguf_tmpl);
    tl_write_gguf(gguf_path, ents, n_ents);
    free(w);

    for (s = 0; s < 8; ++s)
        for (t = 0; t < 8; ++t) {
            rng = rng * 6364136223846793005ULL + 1;
            cal[s][t] = (int)((rng >> 40) % (uint64_t)TL_V);
        }

    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    if (cce_anymodel_open(&am, gguf_path) != CCE_OK || !am ||
        !am->transformer) {
        fprintf(stderr, "tiny open failed\n");
        unlink(gguf_path);
        return 1;
    }
    m = am->transformer;
    printf("  tiny L=%d D=%d V=%d branches=%d\n", m->n_layer, m->n_embd,
           m->vocab_size, cce_forest_branch_count(m->forest));

    lg_fp = (float *)calloc((size_t)m->vocab_size, sizeof(float));
    lg_ph = (float *)calloc((size_t)m->vocab_size, sizeof(float));
    lg_st = (float *)calloc((size_t)m->vocab_size, sizeof(float));
    lg_tmp = (float *)calloc((size_t)m->vocab_size, sizeof(float));
    if (!lg_fp || !lg_ph || !lg_st || !lg_tmp) return 1;

    /* A) FP baseline */
    clone_logits(m, seq_eval, 6, lg_fp);
    tps_fp = decode_tps(m, 32, lg_tmp);
    printf("  [A] FP         decode_tok_s=%.1f  argmax=%d\n", tps_fp,
           argmax_f(lg_fp, m->vocab_size));

    /* B) Whole-forest posthoc ternary */
    {
        int nq = cce_gguf_qwen2_quantize_ternary(m);
        printf("  [B] posthoc    quantized_blocks=%d\n", nq);
        clone_logits(m, seq_eval, 6, lg_ph);
        e_ph = relerr_vec(lg_ph, lg_fp, m->vocab_size);
        agree_ph = argmax_f(lg_ph, m->vocab_size) == argmax_f(lg_fp, m->vocab_size);
        tps_ph = decode_tps(m, 32, lg_tmp);
        printf("  [B] posthoc    logit_relerr=%.4f argmax_agree=%d tok_s=%.1f\n",
               e_ph, agree_ph, tps_ph);
    }
    cce_anymodel_free(am);
    am = NULL;

    /* C) Fresh load → capture → STE schedule */
    if (cce_anymodel_open(&am, gguf_path) != CCE_OK || !am) {
        fprintf(stderr, "reload failed\n");
        return 1;
    }
    m = am->transformer;
    cce_ladder_bank_init(&bank, 64, 512);
    cce_ladder_capture_model(m, &bank, &cal[0][0], 8, 8);
    printf("  [C] capture    specs=%d (real activations)\n", bank.n_specs);
    cce_ladder_cfg_default(&cfg);
    cfg.pack_trits = 1;
    cfg.n_calib = 128;
    cfg.n_holdout = 32;
    sched = cce_ladder_default_schedule(&n_stages);
    n_rep = cce_ladder_run_schedule(m, &bank, sched, n_stages, &cfg, reps, 64,
                                    NULL);
    for (i = 0; i < n_rep; ++i)
        if (reps[i].certified) cert++;
    printf("  [C] schedule   converted=%d certified=%d packed=%d "
           "real_act_used=%d\n",
           n_rep, cert, count_packed(m->forest),
           n_rep > 0 ? reps[0].used_real_acts : 0);
    for (i = 0; i < n_rep && i < 12; ++i)
        printf("      %s cert=%d err=%.3f real=%d\n", reps[i].name,
               reps[i].certified, reps[i].relerr_final, reps[i].used_real_acts);

    clone_logits(m, seq_eval, 6, lg_st);
    e_st = relerr_vec(lg_st, lg_fp, m->vocab_size);
    agree_st =
        argmax_f(lg_st, m->vocab_size) == argmax_f(lg_fp, m->vocab_size);
    tps_st = decode_tps(m, 32, lg_tmp);
    printf("  [C] STE sched  logit_relerr=%.4f argmax_agree=%d tok_s=%.1f\n",
           e_st, agree_st, tps_st);

    /* D) export / import */
    {
        int fd2 = mkstemp(pack_tmpl);
        int nw, nr;
        if (fd2 >= 0) {
            close(fd2);
            snprintf(pack_path, sizeof pack_path, "%s.ldtr", pack_tmpl);
            rename(pack_tmpl, pack_path);
            nw = cce_ladder_export_certified(m->forest, pack_path);
            printf("  [D] export     certified_written=%d path=%s\n", nw,
                   pack_path);
            for (i = 0; i < cce_forest_branch_count(m->forest); ++i) {
                char name[160];
                cce_cascade *cas;
                if (cce_forest_branch_name(m->forest, i, name,
                                          (int)sizeof name) != CCE_OK)
                    continue;
                cas = cce_forest_get_resident(m->forest, name);
                if (!cas || cas->num_blocks < 1) continue;
                free(cas->blocks[0].w_trit);
                free(cas->blocks[0].w_scale);
                free(cas->blocks[0].w_q);
                cas->blocks[0].w_trit = NULL;
                cas->blocks[0].w_scale = NULL;
                cas->blocks[0].w_q = NULL;
            }
            nr = cce_ladder_import_certified(m->forest, pack_path);
            printf("  [D] import     loaded=%d packed_now=%d\n", nr,
                   count_packed(m->forest));
            unlink(pack_path);
        }
    }

    printf("\n== COMPARISON ==\n");
    printf("  path          logit_relerr  argmax_ok  tok_s   packed\n");
    printf("  FP            0.0000        yes        %6.1f  0\n", tps_fp);
    printf("  posthoc-all   %6.4f        %s        %6.1f  (all quant)\n", e_ph,
           agree_ph ? "yes" : "NO ", tps_ph);
    printf("  STE+schedule  %6.4f        %s        %6.1f  %d\n", e_st,
           agree_st ? "yes" : "NO ", tps_st, count_packed(m->forest));
    printf("  verdict: STE schedule %s posthoc on logit_relerr\n",
           e_st <= e_ph + 1e-6 ? "≤" : ">");
    printf("LADDER_BENCH_PASS cert=%d/%d e_st=%.4f e_ph=%.4f "
           "tps_st=%.1f tps_fp=%.1f\n",
           cert, n_rep, e_st, e_ph, tps_st, tps_fp);

    cce_ladder_bank_free(&bank);
    free(lg_fp);
    free(lg_ph);
    free(lg_st);
    free(lg_tmp);
    cce_anymodel_free(am);
    unlink(gguf_path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        /* real model path: capture + schedule limited */
        cce_anymodel *am = NULL;
        cce_ladder_bank bank;
        cce_ladder_cfg cfg;
        cce_ladder_report reps[128];
        int cal[4][16], s, t, n_rep, cert = 0, i;
        uint64_t rng = 1;
        const char *path = argv[1];
        printf("== ladder bench (real) model=%s ==\n", path);
        setenv("CNET_FOREST_NO_PERSIST", "1", 1);
        setenv("CNET_INFER_FP", "1", 0);
        unsetenv("CNET_ORACLE_INT8");
        unsetenv("CNET_SPARSE_KV");
        if (cce_anymodel_open(&am, path) != CCE_OK || !am || !am->transformer) {
            fprintf(stderr, "open failed\n");
            return 1;
        }
        for (s = 0; s < 4; ++s)
            for (t = 0; t < 16; ++t) {
                rng = rng * 6364136223846793005ULL + 1;
                cal[s][t] =
                    (int)((rng >> 33) % (uint64_t)(am->transformer->vocab_size > 1
                                                       ? am->transformer->vocab_size
                                                       : 2));
            }
        cce_ladder_bank_init(&bank, 512, 256);
        cce_ladder_capture_model(am->transformer, &bank, &cal[0][0], 4, 16);
        printf("  captured specs=%d\n", bank.n_specs);
        cce_ladder_cfg_default(&cfg);
        cfg.pack_trits = 1;
        /* only first 2 stages for speed on real 9B */
        {
            cce_ladder_stage st[2] = {
                {"gate_proj", 0.45f, CCE_LADDER_STE, 24},
                {"down_proj", 0.35f, CCE_LADDER_STE, 32},
            };
            n_rep = cce_ladder_run_schedule(am->transformer, &bank, st, 2, &cfg,
                                            reps, 16, NULL);
        }
        for (i = 0; i < n_rep; ++i) {
            printf("  %s cert=%d err=%.3f real=%d\n", reps[i].name,
                   reps[i].certified, reps[i].relerr_final,
                   reps[i].used_real_acts);
            if (reps[i].certified) cert++;
        }
        printf("LADDER_BENCH_REAL converted=%d certified=%d\n", n_rep, cert);
        cce_ladder_bank_free(&bank);
        cce_anymodel_free(am);
        return n_rep > 0 ? 0 : 1;
    }
    return run_tiny();
}
