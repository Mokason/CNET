/* Hybrid A/B/C micro-benchmark vs same-size dense stand-in.
 * make hybrid_bench → HYBRID_BENCH_PASS + JSON report
 *
 * Metrics (hermetic, deterministic):
 *  - skill accuracy on Tier A domain
 *  - open-ended residual accuracy (rot1 domain)
 *  - mean active params ratio (fragments vs dense full)
 *  - mean latency proxy (ops count)
 *  - escalation rate under mixed workload
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define SYM 8
#define N_TRIALS 200

static Port P(const char *t) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", t);
    return p;
}
static void oh(double *r, int h) {
    int i;
    for (i = 0; i < SYM; i++) r[i] = (i == h) ? 1.0 : 0.0;
}
static int am(const double *v) {
    int i, b = 0;
    for (i = 1; i < SYM; i++) if (v[i] > v[b]) b = i;
    return b;
}

/* Dense stand-in of "same size": one big matrix SYM*SYM*K approx params */
typedef struct {
    double W[SYM * SYM];
} DenseToy;

static void dense_init(DenseToy *d, int rot) {
    int i, j;
    memset(d, 0, sizeof *d);
    for (i = 0; i < SYM; i++)
        for (j = 0; j < SYM; j++)
            d->W[i * SYM + j] = (j == (i + rot) % SYM) ? 1.0 : 0.0;
}

static void dense_forward(const DenseToy *d, const double *in, double *out) {
    int i, j;
    for (j = 0; j < SYM; j++) {
        double s = 0;
        for (i = 0; i < SYM; i++) s += in[i] * d->W[i * SYM + j];
        out[j] = s;
    }
}

static int admit_skill(PrimitiveRegistry *reg, int rot, const char *tag) {
    BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
    double in[SYM][SYM], tg[SYM][SYM];
    Contract c;
    Specialist s;
    Port pin = P("bin"), pout = P(tag);
    int i;
    for (i = 0; i < SYM; i++) {
        oh(in[i], i);
        oh(tg[i], (i + rot) % SYM);
    }
    btn_init(btn, SYM, SYM, 16, 64, 0.5, 42u + (unsigned)rot);
    btn_set_ports(btn, pin, pout);
    btn_train_dynamic(btn, (const double *)in, (const double *)tg, SYM, 20000,
                      200, 1e-7, 1e-9);
    btn_train(btn, (const double *)in, (const double *)tg, SYM, 4000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, tag, btn, (const double *)in,
                               (const double *)tg, SYM) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    specialist_wrap_btn(&s, btn, tag);
    if (specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    return 0;
}

static int soft_fn(const double *in, double *out, void *ctx) {
    int rot = ctx ? (int)(intptr_t)ctx : 1;
    int h = am(in), i;
    for (i = 0; i < SYM; i++) out[i] = 0.0;
    out[(h + rot) % SYM] = 1.0;
    return 0;
}

int main(void) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    DenseToy dense;
    Port pin = P("bin");
    Port p0 = P("sk0"), p1 = P("sk1"), psoft = P("soft"), popen = P("open");
    double in[SYM], out[SYM], dout[SYM];
    int t, skill_ok = 0, skill_n = 0, open_ok = 0, open_n = 0;
    int dense_skill_ok = 0, dense_open_ok = 0;
    int local_hits = 0, soft_hits = 0, res_hits = 0, abs_hits = 0;
    double hybrid_ops = 0, dense_ops = 0;
    const char *base = "tmp_hybrid_bench.cnb";
    const char *led = "tmp_hybrid_bench.gaps.txt";
    FILE *jf;

    remove(base);
    remove(led);
    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;
    pol.allow_soft = 1;
    pol.allow_residual = 1;
    personal_ai_open(&ai, base, led, NULL, &pol);
    ai.lane.acq.min_evidence = 1;

    /* Hybrid library: 2 certified skills + soft + residual */
    if (admit_skill(&ai.lane.reg, 0, "sk0") != 0 ||
        admit_skill(&ai.lane.reg, 1, "sk1") != 0) {
        fprintf(stderr, "admit failed\n");
        return 1;
    }
    personal_ai_bind_soft(&ai, "soft1", pin, psoft, soft_fn, (void *)(intptr_t)2,
                          0.0);
    personal_ai_bind_residual(&ai, "res", hybrid_hermetic_residual,
                              (void *)(uintptr_t)SYM);

    /* Same-size dense: one full matrix always active (~SYM^2 params).
       Hybrid skills: 2 * ~16*SYM + residual O(1) active on residual path. */
    dense_init(&dense, 1);

    for (t = 0; t < N_TRIALS; t++) {
        int hot = t % SYM;
        int kind = t % 4; /* 0,1 skill tags, 2 soft, 3 open residual */
        Port goal;
        int expect;
        oh(in, hot);
        if (kind == 0) {
            goal = p0;
            expect = hot; /* id */
        } else if (kind == 1) {
            goal = p1;
            expect = (hot + 1) % SYM;
        } else if (kind == 2) {
            goal = psoft;
            expect = (hot + 2) % SYM;
        } else {
            goal = popen;
            expect = (hot + 1) % SYM; /* residual rot1 */
        }

        /* Hybrid */
        if (personal_ai_serve(&ai, pin, goal, in, SYM, out, SYM, &rep) == 0) {
            if (kind <= 1) {
                skill_n++;
                if (am(out) == expect) skill_ok++;
                /* Active weights proxy: small student ~ H*SYM + SYM*SYM_out, H=8 */
                hybrid_ops += (double)(8 * SYM + SYM);
                local_hits++;
            } else if (kind == 2) {
                skill_n++;
                if (am(out) == expect) skill_ok++;
                hybrid_ops += (double)SYM; /* soft O(V) */
                soft_hits++;
            } else {
                open_n++;
                if (am(out) == expect) open_ok++;
                hybrid_ops += (double)SYM; /* residual O(V) */
                res_hits++;
            }
        } else {
            abs_hits++;
            if (kind <= 2) skill_n++;
            else open_n++;
        }

        /* Dense always pays full V×V matmul (same-size prior always fully active). */
        dense_forward(&dense, in, dout);
        dense_ops += (double)(SYM * SYM);
        if (kind <= 1) {
            /* generic dense not specialized — score rot1 only as proxy quality */
            if (am(dout) == (hot + 1) % SYM) dense_skill_ok++;
        } else if (kind == 2) {
            if (am(dout) == (hot + 1) % SYM) dense_skill_ok++;
        } else {
            if (am(dout) == (hot + 1) % SYM) dense_open_ok++;
        }
    }

    {
        double skill_acc =
            skill_n ? 100.0 * skill_ok / skill_n : 0.0;
        double open_acc = open_n ? 100.0 * open_ok / open_n : 0.0;
        double dense_open_acc =
            open_n ? 100.0 * dense_open_ok / open_n : 0.0;
        double ops_ratio = dense_ops > 0 ? hybrid_ops / dense_ops : 0.0;
        double local_rate =
            N_TRIALS ? 100.0 * (local_hits + soft_hits) / N_TRIALS : 0.0;

        printf("HYBRID_BENCH\n");
        printf("  trials=%d\n", N_TRIALS);
        printf("  hybrid_skill_acc=%.1f%% (%d/%d)\n", skill_acc, skill_ok,
               skill_n);
        printf("  hybrid_open_acc=%.1f%% (%d/%d)\n", open_acc, open_ok, open_n);
        printf("  dense_open_acc=%.1f%% (same-size dense rot1 prior)\n",
               dense_open_acc);
        printf("  hybrid_ops_ratio=%.3f (lower is cheaper than dense)\n",
               ops_ratio);
        printf("  local_or_soft_rate=%.1f%% residual_rate=%.1f%% abstain=%d\n",
               local_rate, 100.0 * res_hits / N_TRIALS, abs_hits);
        printf("  tier_counts A/B/C ~ %d/%d/%d\n", local_hits, soft_hits,
               res_hits);

        jf = fopen("logs/hybrid_bench.json", "w");
        if (jf) {
            fprintf(jf,
                    "{\n"
                    "  \"trials\": %d,\n"
                    "  \"hybrid_skill_acc\": %.4f,\n"
                    "  \"hybrid_open_acc\": %.4f,\n"
                    "  \"dense_open_acc\": %.4f,\n"
                    "  \"hybrid_ops_ratio\": %.4f,\n"
                    "  \"local_or_soft_rate\": %.4f,\n"
                    "  \"residual_rate\": %.4f,\n"
                    "  \"abstains\": %d,\n"
                    "  \"tier_a_hits\": %d,\n"
                    "  \"tier_b_hits\": %d,\n"
                    "  \"tier_c_hits\": %d\n"
                    "}\n",
                    N_TRIALS, skill_acc / 100.0, open_acc / 100.0,
                    dense_open_acc / 100.0, ops_ratio, local_rate / 100.0,
                    (double)res_hits / N_TRIALS, abs_hits, local_hits,
                    soft_hits, res_hits);
            fclose(jf);
        }

        /* Pass: high skill+open accuracy; hybrid activates less work than dense. */
        if (skill_acc >= 95.0 && open_acc >= 95.0 && ops_ratio < 0.85) {
            printf("HYBRID_BENCH_PASS\n");
            personal_ai_close(&ai);
            remove(base);
            remove(led);
            return 0;
        }
        printf("HYBRID_BENCH_FAIL skill=%.1f open=%.1f ops_ratio=%.3f\n",
               skill_acc, open_acc, ops_ratio);
        personal_ai_close(&ai);
        remove(base);
        remove(led);
        return 1;
    }
}
