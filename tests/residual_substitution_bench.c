/* Substitution bench (M4 / spike S3) — does own learning displace the residual?
 *
 * This is the experiment that decides whether the flywheel is real. Everything
 * before it (organic capture, the reservoir) exists to make this runnable.
 *
 * Claim under test: units mined from CAPTURED REAL TRAFFIC serve requests that
 * would otherwise have gone to Tier C, so residual_rate falls on replay.
 *
 * Design — two arms, identical traffic, one difference:
 *
 *   arm CONTROL    capture TRAIN traffic ......................  replay REPLAY
 *   arm CONSOLIDATE capture TRAIN traffic -> structure mine ->   replay REPLAY
 *
 * Measuring "before" on the same instance would be dishonest twice over: the
 * before-pass would itself capture the held-out inputs into the reservoir, and
 * warm-up effects would be confounded with learning. A separate control arm
 * keeps the only difference the consolidation step.
 *
 * The port is multi-field one-hot (field_count=2), which the old miner could
 * not expand — it would have mined a single exemplar. So this bench only has a
 * treatment arm at all because of the reservoir (M3).
 *
 * HELDOUT pairs are never served during capture, so serving them from Tier A
 * afterwards is generalisation, not replay of a memorised row. Both numbers are
 * reported; only the replay-set rate gates.
 *
 * Honesty rules enforced here:
 *   - a fall in residual_rate bought by abstaining more is NOT a win;
 *   - no PASS unless the treatment arm strictly beats the control;
 *   - otherwise print SUBSTITUTION_BENCH_INCONCLUSIVE with the numbers.
 *
 * make residual_substitution_bench → SUBSTITUTION_BENCH_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"

#define FW 4              /* field width  */
#define FC 2              /* field count → in_dim = FW*FC = 8 */
#define OUT_DIM 4
#define DOMAIN (FW * FW)  /* 16 (a,b) pairs */
#define N_HELDOUT 4

typedef struct {
    size_t served;
    size_t local;      /* Tier A — CNET's own certified weights */
    size_t residual;   /* Tier C — the external teacher */
    size_t abstain;
    size_t heldout_local;
    size_t heldout_total;
    size_t correct;          /* served answer matched ground truth */
    size_t heldout_correct;  /* ... on inputs never seen during capture */
    size_t heldout_residual; /* held-out answered by the teacher instead */
    size_t coverage_abstains;
    size_t coverage_rows;
    size_t reservoir_rows;
    size_t reservoir_mines;
    int admitted;
    int mine_rc;
} ArmResult;

typedef struct {
    size_t fw, fc, out_dim;
} ResCtx;

static Port PMF(const char *tag, size_t width, size_t count) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = width;
    p.field_count = count;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

/* External teacher: compositional (a+b) mod OUT_DIM. */
static int res_addmod(const double *in, double *out, void *ctx) {
    ResCtx *c = (ResCtx *)ctx;
    size_t f, i, sum = 0;
    if (!in || !out || !c) return -1;
    for (f = 0; f < c->fc; f++) {
        size_t hot = 0;
        for (i = 1; i < c->fw; i++)
            if (in[f * c->fw + i] > in[f * c->fw + hot]) hot = i;
        sum += hot;
    }
    for (i = 0; i < c->out_dim; i++) out[i] = 0.0;
    out[sum % c->out_dim] = 1.0;
    return 0;
}

static void encode_pair(double *in, size_t a, size_t b) {
    size_t i;
    for (i = 0; i < FW * FC; i++) in[i] = 0.0;
    in[a] = 1.0;
    in[FW + b] = 1.0;
}

/* Held-out pairs: never served during capture. Fixed, not random, so the gate
   is deterministic and reproducible. */
static int is_heldout(size_t idx) {
    return idx == 3 || idx == 6 || idx == 9 || idx == 12;
}

static int run_arm(int consolidate, int capture_all, const char *tag,
                   ArmResult *res) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep, before, after;
    ResCtx ctx;
    Port pin = PMF("sub_in", FW, FC);
    Port pout = PMF("sub_out", OUT_DIM, 1);
    double in[FW * FC], out[OUT_DIM];
    char base[128], led[128], cov[160];
    const HybridAi *h;
    size_t idx;

    memset(res, 0, sizeof *res);
    snprintf(base, sizeof base, "tmp_subbench_%s.cnb", tag);
    snprintf(led, sizeof led, "tmp_subbench_%s.gaps.txt", tag);
    snprintf(cov, sizeof cov, "%s.coverage", base);
    remove(base);
    remove(led);
    remove(cov);

    ctx.fw = FW;
    ctx.fc = FC;
    ctx.out_dim = OUT_DIM;

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;   /* isolate Tier A vs Tier C */
    pol.allow_soft = 0;
    pol.allow_residual = 1;
    pol.structure_mine_on_serve = 0; /* mine explicitly, once */
    pol.structure_min_hits = 2;

    if (personal_ai_open(&ai, base, led, NULL, &pol) != 0) return -1;
    ai.lane.acq.min_evidence = 1;
    if (personal_ai_bind_residual(&ai, "addmod", res_addmod, &ctx) != 0) {
        personal_ai_close(&ai);
        return -1;
    }

    /* ---- capture traffic (E2 withholds a slice; E1 sees the whole shape) - */
    for (idx = 0; idx < DOMAIN; idx++) {
        if (!capture_all && is_heldout(idx)) continue;
        encode_pair(in, idx / FW, idx % FW);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(&ai, pin, pout, in, FW * FC, out, OUT_DIM,
                                &rep);
    }
    h = personal_ai_hybrid(&ai);
    res->reservoir_rows = hybrid_reservoir_rows(h);

    /* ---- consolidation: the only difference between the arms ------------- */
    if (consolidate) {
        BinaryTransformNetwork *stu = NULL;
        res->mine_rc = personal_ai_structure_mine(&ai, &stu);
        if (res->mine_rc == 0) res->admitted = 1;
        res->reservoir_mines = h->reservoir_mines;
    }

    /* ---- replay the whole domain, measure only this phase ---------------- */
    personal_ai_totals(&ai, &before);
    for (idx = 0; idx < DOMAIN; idx++) {
        int held = is_heldout(idx);
        int ok;
        size_t j, am = 0;
        double truth[OUT_DIM];
        encode_pair(in, idx / FW, idx % FW);
        memset(out, 0, sizeof out);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(&ai, pin, pout, in, FW * FC, out, OUT_DIM,
                                &rep);
        /* Displacing the residual with a WRONG answer is not learning. Score
           the served answer against the teacher's ground truth, not the tier. */
        res_addmod(in, truth, &ctx);
        for (j = 1; j < OUT_DIM; j++)
            if (out[j] > out[am]) am = j;
        ok = truth[am] > 0.5;
        if (ok) res->correct++;
        if (held) {
            res->heldout_total++;
            if (rep.local_hits) res->heldout_local++;
            if (rep.residual_hits) res->heldout_residual++;
            if (ok) res->heldout_correct++;
        }
    }
    personal_ai_totals(&ai, &after);
    res->coverage_abstains = after.coverage_abstains - before.coverage_abstains;
    res->coverage_rows = hybrid_coverage_rows(h, pin, pout);

    res->local = after.local_hits - before.local_hits;
    res->residual = after.residual_hits - before.residual_hits;
    res->abstain = after.abstains - before.abstains;
    res->served = res->local + res->residual;

    personal_ai_close(&ai);
    remove(base);
    remove(led);
    remove(cov);
    return 0;
}

static double rate(size_t num, size_t den) {
    return den ? (double)num / (double)den : 0.0;
}

int main(void) {
    ArmResult control, treat, gen;
    double r_before, r_after, a_before, a_after, sub_before, sub_after;
    double acc_before, acc_after, heldout_gen, gen_acc;
    int pass;

    printf("== substitution bench: does own learning displace the residual? ==\n");
    printf("   domain=%d heldout=%d port=onehot(w=%d,f=%d) multi-field\n",
           DOMAIN, N_HELDOUT, FW, FC);
    printf("   E1 in-coverage substitution (gates)  "
           "E2 out-of-coverage generalisation (reported)\n");

    /* E1 — the claim the flywheel actually makes: traffic this port shape has
       already seen gets served from CNET's own weights instead of the teacher.
       Both arms capture the whole shape; only consolidation differs. */
    if (run_arm(0, 1, "control", &control) != 0) {
        printf("SUBSTITUTION_BENCH_FAIL control arm did not run\n");
        return 1;
    }
    if (run_arm(1, 1, "treat", &treat) != 0) {
        printf("SUBSTITUTION_BENCH_FAIL treatment arm did not run\n");
        return 1;
    }
    /* E2 — the STRONGER claim, deliberately tested to failure: does a unit
       mined from partial traffic generalise to inputs it never saw? */
    if (run_arm(1, 0, "gen", &gen) != 0) {
        printf("SUBSTITUTION_BENCH_FAIL generalisation arm did not run\n");
        return 1;
    }

    r_before = rate(control.residual, control.served);
    r_after = rate(treat.residual, treat.served);
    sub_before = rate(control.local, control.served);
    sub_after = rate(treat.local, treat.served);
    a_before = rate(control.abstain, control.served + control.abstain);
    a_after = rate(treat.abstain, treat.served + treat.abstain);
    acc_before = rate(control.correct, DOMAIN);
    acc_after = rate(treat.correct, DOMAIN);
    heldout_gen = rate(gen.heldout_local, gen.heldout_total);
    gen_acc = rate(gen.correct, DOMAIN);

    printf("\n  arm=control     reservoir_rows=%zu mined=no\n",
           control.reservoir_rows);
    printf("    replay served=%zu local=%zu residual=%zu abstain=%zu\n",
           control.served, control.local, control.residual, control.abstain);
    printf("  arm=consolidate reservoir_rows=%zu mined=%s mine_rc=%d "
           "reservoir_mines=%zu\n",
           treat.reservoir_rows, treat.admitted ? "yes" : "no", treat.mine_rc,
           treat.reservoir_mines);
    printf("    replay served=%zu local=%zu residual=%zu abstain=%zu\n",
           treat.served, treat.local, treat.residual, treat.abstain);

    printf("\n  residual_rate     before=%.4f after=%.4f delta=%+.4f\n",
           r_before, r_after, r_after - r_before);
    printf("  substitution_rate before=%.4f after=%.4f delta=%+.4f\n",
           sub_before, sub_after, sub_after - sub_before);
    printf("  abstain_rate      before=%.4f after=%.4f delta=%+.4f\n",
           a_before, a_after, a_after - a_before);
    printf("  accuracy          before=%.4f after=%.4f delta=%+.4f\n",
           acc_before, acc_after, acc_after - acc_before);

    printf("\n  E2 out-of-coverage (mined from %d of %d pairs, coverage_rows=%zu):\n",
           DOMAIN - N_HELDOUT, DOMAIN, gen.coverage_rows);
    printf("    heldout served from own weights: %zu/%zu (%.4f)\n",
           gen.heldout_local, gen.heldout_total, heldout_gen);
    printf("    heldout deferred to teacher:     %zu/%zu\n",
           gen.heldout_residual, gen.heldout_total);
    printf("    heldout CORRECT:                 %zu/%zu\n",
           gen.heldout_correct, gen.heldout_total);
    printf("    coverage abstains: %zu   replay accuracy: %.4f\n",
           gen.coverage_abstains, gen_acc);
    if (gen.heldout_local > gen.heldout_correct) {
        printf("    WARNING: the unit answered %zu unseen input(s) from its own\n"
               "             weights and got them WRONG — a certified badge on an\n"
               "             uncertified claim. S6 coverage gating is not holding.\n",
               gen.heldout_local - gen.heldout_correct);
    }

    /* S6: out-of-coverage inputs must never be answered from own weights and
       got wrong. Abstaining to the teacher is the correct behaviour; a wrong
       local answer is a certified badge on an uncertified claim. */
    if (gen.heldout_local > gen.heldout_correct) {
        printf("\nSUBSTITUTION_BENCH_INCONCLUSIVE reason=out_of_coverage_wrong "
               "heldout_local=%zu heldout_correct=%zu heldout_total=%zu "
               "coverage_abstains=%zu\n",
               gen.heldout_local, gen.heldout_correct, gen.heldout_total,
               gen.coverage_abstains);
        return 1;
    }
    /* The teacher must still answer what the unit declined — abstention that
       drops the request on the floor is not a fix. */
    if (gen.heldout_correct < gen.heldout_total) {
        printf("\nSUBSTITUTION_BENCH_INCONCLUSIVE reason=heldout_not_answered "
               "heldout_correct=%zu/%zu residual=%zu local=%zu\n",
               gen.heldout_correct, gen.heldout_total, gen.heldout_residual,
               gen.heldout_local);
        return 1;
    }
    /* Displacing the teacher with wrong answers is a regression, not a win. */
    if (acc_after < acc_before - 1e-9) {
        printf("\nSUBSTITUTION_BENCH_INCONCLUSIVE reason=accuracy_regressed "
               "acc_before=%.4f acc_after=%.4f rate_before=%.4f "
               "rate_after=%.4f\n",
               acc_before, acc_after, r_before, r_after);
        return 1;
    }
    /* A drop in residual_rate bought by abstaining more is not a win. */
    if (a_after > a_before + 1e-9) {
        printf("\nSUBSTITUTION_BENCH_INCONCLUSIVE reason=abstain_rate_rose "
               "rate_before=%.4f rate_after=%.4f abstain_before=%.4f "
               "abstain_after=%.4f\n",
               r_before, r_after, a_before, a_after);
        return 1;
    }
    pass = (r_after < r_before - 1e-9) && treat.admitted;
    if (!pass) {
        printf("\nSUBSTITUTION_BENCH_INCONCLUSIVE reason=%s "
               "rate_before=%.4f rate_after=%.4f delta=%+.4f admitted=%d\n",
               treat.admitted ? "residual_rate_did_not_fall" : "no_unit_admitted",
               r_before, r_after, r_after - r_before, treat.admitted);
        return 1;
    }
    /* The marker carries the generalisation numbers too, so a green gate can
       never be read as "CNET generalises beyond what it captured". */
    printf("\nSUBSTITUTION_BENCH_PASS rate_before=%.4f rate_after=%.4f "
           "delta=%+.4f substitution=%.4f accuracy=%.4f abstain_flat=%d "
           "heldout_local=%zu/%zu heldout_deferred=%zu/%zu "
           "heldout_correct=%zu/%zu coverage_abstains=%zu\n",
           r_before, r_after, r_after - r_before, sub_after, acc_after,
           a_after <= a_before + 1e-9, gen.heldout_local, gen.heldout_total,
           gen.heldout_residual, gen.heldout_total, gen.heldout_correct,
           gen.heldout_total, gen.coverage_abstains);
    return 0;
}
