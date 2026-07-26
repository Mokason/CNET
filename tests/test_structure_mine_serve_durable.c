/* A5 — mine-on-serve produces a DURABLE, GATED unit with no test assistance.
 *
 * The deployed lane runs CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1
 * (config/personal-ai.env). That is the configuration that actually mines in
 * production: a Tier-C miss triggers the mine inline, not a maintenance tick.
 * Everything the gate asserts must therefore come from the product path —
 * this file never seals, never checkpoints, never writes coverage.
 *
 * Proves end to end:
 *   serve misses -> mine on serve -> seal + coverage persisted
 *   -> close -> REOPEN -> unit present, coverage loaded
 *   -> in-coverage served from own weights, out-of-coverage refused
 *
 * make structure_mine_serve_durable -> STRUCTURE_MINE_SERVE_DURABLE_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/base.h"

#define FW 4
#define FC 2
#define IN_DIM (FW * FC)
#define OUT_DIM 4
#define DOMAIN 16

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

typedef struct { size_t fw, fc, out_dim; } ResCtx;

static Port PMF(const char *tag, size_t w, size_t c) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = w;
    p.field_count = c;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

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
    for (i = 0; i < IN_DIM; i++) in[i] = 0.0;
    in[a] = 1.0;
    in[FW + b] = 1.0;
}

/* Withheld from capture so it is genuinely outside the certified domain. */
static int is_heldout(size_t i) { return i == 5; }

static int open_lane(PersonalAi *ai, ResCtx *ctx, int mine_on_serve) {
    PersonalAiPolicy pol;
    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;
    pol.allow_soft = 0;
    pol.allow_residual = 1;
    pol.structure_mine_on_serve = mine_on_serve; /* the deployed setting */
    pol.structure_min_hits = 2;
    if (personal_ai_open(ai, "tmp_sms.cnb", "tmp_sms.gaps.txt", NULL, &pol) != 0)
        return -1;
    ai->lane.acq.min_evidence = 1;
    return personal_ai_bind_residual(ai, "addmod", res_addmod, ctx);
}

int main(void) {
    PersonalAi ai;
    ResCtx ctx;
    Port pin = PMF("sms_in", FW, FC), pout = PMF("sms_out", OUT_DIM, 1);
    double in[IN_DIM], out[OUT_DIM];
    PersonalAiReport rep;
    size_t idx, mined_rows;

    ctx.fw = FW;
    ctx.fc = FC;
    ctx.out_dim = OUT_DIM;
    unsetenv("CNET_COVERAGE_ABSTAIN");
    unsetenv("CNET_FAULT_LOG");
    remove("tmp_sms.cnb");
    remove("tmp_sms.gaps.txt");
    remove("tmp_sms.cnb.coverage");

    printf("== mine-on-serve: durable + gated, product path only ==\n");

    /* ---- phase 1: serve with mining enabled inline ---------------------- */
    check(open_lane(&ai, &ctx, 1) == 0, "open with STRUCTURE_MINE_ON_SERVE=1");
    for (idx = 0; idx < DOMAIN; idx++) {
        if (is_heldout(idx)) continue;
        encode_pair(in, idx / FW, idx % FW);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(&ai, pin, pout, in, IN_DIM, out, OUT_DIM, &rep);
    }
    mined_rows = hybrid_coverage_rows(personal_ai_hybrid(&ai), pin, pout);
    check(personal_ai_hybrid(&ai)->structure_mines > 0,
          "a mine fired during serve (not a maintenance tick)");
    check(mined_rows > 0, "coverage recorded by the serve-path mine");
    check(cnb_has_unit(&ai.lane.base, "hyb_struct_0") == 1,
          "unit sealed into the base by the serve-path mine");
    /* No checkpoint here on purpose — personal_ai_structure_mine must have
       done it, or the reopen below will not see the unit. */
    personal_ai_close(&ai);

    {
        FILE *fp = fopen("tmp_sms.cnb.coverage", "r");
        check(fp != NULL, "coverage sidecar written without test help");
        if (fp) fclose(fp);
    }

    /* ---- phase 2: restart, mining OFF so nothing can re-create state ---- */
    check(open_lane(&ai, &ctx, 0) == 0, "reopen with mining disabled");
    check(cnb_has_unit(&ai.lane.base, "hyb_struct_0") == 1,
          "mined unit survived the restart");
    check(hybrid_coverage_rows(personal_ai_hybrid(&ai), pin, pout) == mined_rows,
          "coverage survived the restart with the same row count");

    /* in-coverage: served from CNET's own weights */
    encode_pair(in, 0, 0);
    memset(&rep, 0, sizeof rep);
    (void)personal_ai_serve(&ai, pin, pout, in, IN_DIM, out, OUT_DIM, &rep);
    check(rep.local_hits == 1, "in-coverage served from own weights after restart");
    {
        double truth[OUT_DIM];
        size_t j, am = 0;
        res_addmod(in, truth, &ctx);
        for (j = 1; j < OUT_DIM; j++) if (out[j] > out[am]) am = j;
        check(truth[am] > 0.5, "and the answer is correct");
    }

    /* out-of-coverage: refused, teacher answers */
    encode_pair(in, is_heldout(5) ? 1 : 0, 1); /* idx 5 == (1,1), withheld */
    memset(&rep, 0, sizeof rep);
    memset(out, 0, sizeof out);
    (void)personal_ai_serve(&ai, pin, pout, in, IN_DIM, out, OUT_DIM, &rep);
    check(rep.coverage_abstains == 1 && rep.local_hits == 0,
          "out-of-coverage refused after restart");
    check(rep.residual_hits == 1, "teacher answered the refused request");
    {
        double truth[OUT_DIM];
        size_t j, am = 0;
        res_addmod(in, truth, &ctx);
        for (j = 1; j < OUT_DIM; j++) if (out[j] > out[am]) am = j;
        check(truth[am] > 0.5, "and that answer is correct too");
    }
    personal_ai_close(&ai);

    remove("tmp_sms.cnb");
    remove("tmp_sms.gaps.txt");
    remove("tmp_sms.cnb.coverage");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("STRUCTURE_MINE_SERVE_DURABLE_PASS checks=%d rows=%zu\n", checks,
               mined_rows);
        return 0;
    }
    printf("STRUCTURE_MINE_SERVE_DURABLE_FAIL failures=%d\n", failures);
    return 1;
}
