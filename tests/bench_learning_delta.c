/* Learning performance micro-bench: structure-mine throughput + hybrid serve mix.
 * make learning_delta_bench → LEARNING_DELTA_BENCH_PASS + JSON
 *
 * Compares:
 *   - structure-mine with min_hits=3 vs min_hits=2 (hits needed before seal)
 *   - residual serve ops vs dense-style always-residual baseline
 *   - batch_label_rows produced per successful mine
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define SYM 4
#define N_TRIALS 200

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static Port P(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void oh(double *r, int h) {
    int i;
    for (i = 0; i < SYM; i++) r[i] = (i == h) ? 1.0 : 0.0;
}

/* Count residual serves needed before mine succeeds at min_hits. */
static int serves_until_mine(size_t min_hits, double *wall_out, size_t *batch_rows) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    Port pin = P("ld_in"), pres = P("ld_res");
    double in[SYM], out[SYM];
    PersonalAiReport rep;
    BinaryTransformNetwork *stu = NULL;
    int serves = 0, i;
    double t0, t1;

    personal_ai_policy_defaults(&pol);
    pol.structure_min_hits = min_hits;
    remove("tmp_learn_delta.cnb");
    remove("tmp_learn_delta.gaps.txt");
    if (personal_ai_open(&ai, "tmp_learn_delta.cnb", "tmp_learn_delta.gaps.txt",
                         NULL, &pol) != 0)
        return -1;
    if (personal_ai_bind_residual(&ai, "rot", hybrid_hermetic_residual,
                                  (void *)(uintptr_t)SYM) != 0) {
        personal_ai_close(&ai);
        return -2;
    }
    t0 = now_s();
    for (i = 0; i < 64; i++) {
        oh(in, i % SYM);
        if (personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep) != 0)
            break;
        serves++;
        if (serves >= (int)min_hits) {
            if (personal_ai_structure_mine(&ai, &stu) == 0 && stu) {
                t1 = now_s();
                if (wall_out) *wall_out = t1 - t0;
                if (batch_rows) *batch_rows = ai.hybrid.batch_label_rows;
                personal_ai_close(&ai);
                remove("tmp_learn_delta.cnb");
                remove("tmp_learn_delta.gaps.txt");
                return serves;
            }
        }
    }
    t1 = now_s();
    if (wall_out) *wall_out = t1 - t0;
    if (batch_rows) *batch_rows = ai.hybrid.batch_label_rows;
    personal_ai_close(&ai);
    remove("tmp_learn_delta.cnb");
    remove("tmp_learn_delta.gaps.txt");
    return -3;
}

int main(void) {
    int s3, s2;
    double w3 = 0, w2 = 0, w_serve = 0;
    size_t b3 = 0, b2 = 0;
    int i, local_or_soft = 0, residual = 0, ok = 0;
    double ops_hybrid = 0, ops_dense = 0;

    printf("== learning delta micro-bench ==\n");

    s3 = serves_until_mine(3, &w3, &b3);
    s2 = serves_until_mine(2, &w2, &b2);
    printf("  min_hits=3: serves_until_mine=%d wall=%.4fs batch_rows=%zu\n", s3,
           w3, b3);
    printf("  min_hits=2: serves_until_mine=%d wall=%.4fs batch_rows=%zu\n", s2,
           w2, b2);

    /* Hybrid serve mix (same shape as hybrid_bench sample) */
    {
        PersonalAi ai;
        PersonalAiPolicy pol;
        Port pin = P("hyb_in"), pid = P("hyb_id"), pres = P("hyb_res");
        double in[SYM], out[SYM];
        PersonalAiReport rep;
        double t0, t1;
        /* admit identity unit for Tier A */
        {
            BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
            double tin[SYM][SYM], tg[SYM][SYM];
            Contract c;
            Specialist s;
            int j;
            for (j = 0; j < SYM; j++) {
                oh(tin[j], j);
                oh(tg[j], j);
            }
            btn_init(btn, SYM, SYM, 8, 32, 0.5, 3);
            btn_set_ports(btn, pin, pid);
            btn_train_dynamic(btn, (const double *)tin, (const double *)tg, SYM,
                              15000, 200, 1e-6, 1e-8);
            btn_train(btn, (const double *)tin, (const double *)tg, SYM, 2000);
            memset(&c, 0, sizeof c);
            contract_init_borrowed(&c, "hyb_id", btn, (const double *)tin,
                                   (const double *)tg, SYM);
            memset(&s, 0, sizeof s);
            specialist_wrap_btn(&s, btn, "hyb_id");
            personal_ai_policy_defaults(&pol);
            remove("tmp_ld2.cnb");
            remove("tmp_ld2.gaps.txt");
            personal_ai_open(&ai, "tmp_ld2.cnb", "tmp_ld2.gaps.txt", NULL, &pol);
            specialist_admit(&ai.lane.reg, &s, &c);
            contract_free(&c);
        }
        personal_ai_bind_residual(&ai, "rot", hybrid_hermetic_residual,
                                  (void *)(uintptr_t)SYM);
        t0 = now_s();
        for (i = 0; i < N_TRIALS; i++) {
            oh(in, i % SYM);
            if (i % 4 == 0) {
                /* open residual */
                if (personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep) ==
                    0) {
                    ok++;
                    if (rep.source == PERSONAL_AI_RESIDUAL) residual++;
                    else local_or_soft++;
                }
            } else {
                if (personal_ai_serve(&ai, pin, pid, in, SYM, out, SYM, &rep) ==
                    0) {
                    ok++;
                    if (rep.source == PERSONAL_AI_LOCAL ||
                        rep.source == PERSONAL_AI_SOFT)
                        local_or_soft++;
                    else if (rep.source == PERSONAL_AI_RESIDUAL)
                        residual++;
                }
            }
        }
        t1 = now_s();
        w_serve = t1 - t0;
        /* ops model: A/B = 1 unit, residual = 4 (dense-ish) */
        ops_hybrid = (double)local_or_soft * 1.0 + (double)residual * 4.0;
        ops_dense = (double)N_TRIALS * 4.0;
        personal_ai_close(&ai);
        remove("tmp_ld2.cnb");
        remove("tmp_ld2.gaps.txt");
    }

    {
        double hit_reduce =
            (s3 > 0 && s2 > 0) ? (100.0 * (1.0 - (double)s2 / (double)s3)) : 0.0;
        double time_speedup = (w2 > 0 && w3 > 0) ? (w3 / w2) : 0.0;
        double ops_ratio = ops_dense > 0 ? ops_hybrid / ops_dense : 1.0;
        double local_rate =
            ok > 0 ? 100.0 * (double)local_or_soft / (double)ok : 0.0;

        printf("\nLEARNING_DELTA\n");
        printf("  hits_to_mine_reduction=%.1f%%  (3→2 min_hits)\n", hit_reduce);
        printf("  mine_wall_speedup=%.2fx  (3→2)\n", time_speedup);
        printf("  serve_local_or_soft_rate=%.1f%%  residual_rate=%.1f%%\n",
               local_rate, ok ? 100.0 * residual / (double)ok : 0.0);
        printf("  hybrid_ops_ratio=%.3f  (lower better vs always-dense)\n",
               ops_ratio);
        printf("  serve_trials=%d wall=%.4fs (%.0f trials/s)\n", N_TRIALS, w_serve,
               w_serve > 0 ? N_TRIALS / w_serve : 0.0);

        {
            FILE *f = fopen("logs/learning_delta.json", "w");
            if (f) {
                fprintf(f,
                        "{\n"
                        "  \"min_hits_3_serves\": %d,\n"
                        "  \"min_hits_2_serves\": %d,\n"
                        "  \"min_hits_3_wall_s\": %.6f,\n"
                        "  \"min_hits_2_wall_s\": %.6f,\n"
                        "  \"hits_to_mine_reduction_pct\": %.2f,\n"
                        "  \"mine_wall_speedup\": %.4f,\n"
                        "  \"batch_rows_3\": %zu,\n"
                        "  \"batch_rows_2\": %zu,\n"
                        "  \"serve_trials\": %d,\n"
                        "  \"local_or_soft\": %d,\n"
                        "  \"residual\": %d,\n"
                        "  \"local_or_soft_rate\": %.4f,\n"
                        "  \"hybrid_ops_ratio\": %.4f,\n"
                        "  \"serve_wall_s\": %.6f\n"
                        "}\n",
                        s3, s2, w3, w2, hit_reduce, time_speedup, b3, b2,
                        N_TRIALS, local_or_soft, residual, local_rate / 100.0,
                        ops_ratio, w_serve);
                fclose(f);
            }
        }

        if (s3 < 0 || s2 < 0 || s2 > s3) {
            printf("LEARNING_DELTA_BENCH_FAIL\n");
            return 1;
        }
        printf("LEARNING_DELTA_BENCH_PASS\n");
    }
    return 0;
}
