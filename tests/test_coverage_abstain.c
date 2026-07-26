/* Coverage-gated abstention (S6) — a unit must not answer outside its contract.
 *
 * S3/E2 measured the failure: a unit mined from 12 of 16 pairs answered all 4
 * unseen inputs from its own weights and got all 4 wrong, dropping accuracy
 * from 1.0000 to 0.7500 while residual_rate reported a perfect 1.0 -> 0.0 win.
 *
 * Confidence cannot detect this. A mined BTN emits a saturated one-hot, so
 * port_margin is exactly 1.00000 on the inputs it has never seen and gets
 * wrong — margin-based abstention (CNET_RESIDUAL_MIN_MARGIN, soft min_margin,
 * cnet_governance_decide) is blind here. Coverage must be membership: a
 * contract certifies over a domain, and answering outside it is an uncertified
 * claim wearing a certified badge.
 *
 * This gate proves the mechanism, its blast radius, and that it is the thing
 * doing the work:
 *   1. no record  => default-allow (hand-admitted full-domain units untouched)
 *   2. in-coverage  => still served from own weights, still correct
 *   3. out-of-coverage => Tier A declines, teacher answers, answer is correct
 *   4. CNET_COVERAGE_ABSTAIN=0 => the wrong answers come back (gate is load-bearing)
 *   5. PORT_RAW ungated (documented limit, asserted so it cannot drift silently)
 *
 * make coverage_abstain → COVERAGE_ABSTAIN_PASS
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
#include "../include/contract/contract.h"

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

static int is_heldout(size_t i) { return i == 3 || i == 6 || i == 9 || i == 12; }

/* Open an instance on a named base, optionally capturing+mining first, then
 * replay the whole domain. reopen=1 skips capture/mine so the run exercises
 * only what survived on disk. */
static int build_and_replay_ex(PersonalAi *ai, ResCtx *ctx, const char *tag,
                               int reopen, int seal,
                               size_t *heldout_local, size_t *heldout_correct,
                               size_t *heldout_residual, size_t *abstains) {
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    Port pin = PMF("cov_in", FW, FC), pout = PMF("cov_out", OUT_DIM, 1);
    double in[IN_DIM], out[OUT_DIM];
    char base[128], led[128];
    BinaryTransformNetwork *stu = NULL;
    size_t idx;

    snprintf(base, sizeof base, "tmp_cov_%s.cnb", tag);
    snprintf(led, sizeof led, "tmp_cov_%s.gaps.txt", tag);
    if (!reopen) {
        remove(base);
        remove(led);
    }

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;
    pol.allow_soft = 0;
    pol.allow_residual = 1;
    pol.structure_mine_on_serve = 0;
    pol.structure_min_hits = 2;
    if (personal_ai_open(ai, base, led, NULL, &pol) != 0) return -1;
    ai->lane.acq.min_evidence = 1;
    if (personal_ai_bind_residual(ai, "addmod", res_addmod, ctx) != 0) return -1;

    if (!reopen) {
        for (idx = 0; idx < DOMAIN; idx++) {
            if (is_heldout(idx)) continue;
            encode_pair(in, idx / FW, idx % FW);
            memset(&rep, 0, sizeof rep);
            (void)personal_ai_serve(ai, pin, pout, in, IN_DIM, out, OUT_DIM,
                                    &rep);
        }
        if (personal_ai_structure_mine(ai, &stu) != 0) return -2;
        /* Durably seal the mined unit the way the live serve path does
           (soul_host structure seal: student self-labels + cnb_add_unit), so
           the reopened base really does hold a mined unit for coverage to
           gate. personal_ai's own mine path admits to the registry only. */
        if (seal && stu) {
            double si[DOMAIN * IN_DIM], st[DOMAIN * OUT_DIM];
            Contract c;
            size_t n = 0;
            for (idx = 0; idx < DOMAIN; idx++) {
                if (is_heldout(idx)) continue;
                encode_pair(si + n * IN_DIM, idx / FW, idx % FW);
                /* Seal against the teacher labels the unit was certified on —
                   the same rows the coverage record holds. Raw btn_forward
                   output is not canonical for the port and the contract
                   rightly refuses it. */
                res_addmod(si + n * IN_DIM, st + n * OUT_DIM, ctx);
                n++;
            }
            memset(&c, 0, sizeof c);
            if (contract_init_borrowed(&c, "hyb_struct_0", stu, si, st, n) != 0)
                return -5;
            if (cnb_add_unit(&ai->lane.base, stu, &c, NULL) != 0) {
                contract_free(&c);
                return -6;
            }
            contract_free(&c);
        }
        if (gap_lane_checkpoint(&ai->lane) != 0) return -3;
    }

    *heldout_local = *heldout_correct = *heldout_residual = *abstains = 0;
    for (idx = 0; idx < DOMAIN; idx++) {
        double truth[OUT_DIM];
        size_t j, am = 0;
        encode_pair(in, idx / FW, idx % FW);
        memset(out, 0, sizeof out);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(ai, pin, pout, in, IN_DIM, out, OUT_DIM, &rep);
        if (!is_heldout(idx)) continue;
        res_addmod(in, truth, ctx);
        for (j = 1; j < OUT_DIM; j++) if (out[j] > out[am]) am = j;
        if (rep.local_hits) (*heldout_local)++;
        if (rep.residual_hits) (*heldout_residual)++;
        if (rep.coverage_abstains) (*abstains)++;
        if (truth[am] > 0.5) (*heldout_correct)++;
    }
    return 0;
}

static int build_and_replay(PersonalAi *ai, ResCtx *ctx, const char *tag,
                            size_t *heldout_local, size_t *heldout_correct,
                            size_t *heldout_residual, size_t *abstains) {
    return build_and_replay_ex(ai, ctx, tag, 0, 0, heldout_local,
                               heldout_correct, heldout_residual, abstains);
}

int main(void) {
    PersonalAi ai;
    ResCtx ctx;
    Port pin = PMF("cov_in", FW, FC), pout = PMF("cov_out", OUT_DIM, 1);
    size_t hl, hc, hr, ab;

    ctx.fw = FW;
    ctx.fc = FC;
    ctx.out_dim = OUT_DIM;
    printf("== coverage-gated abstention: refuse outside the certified domain ==\n");

    unsetenv("CNET_COVERAGE_ABSTAIN");
    unsetenv("CNET_FAULT_LOG");

    /* ---- 1. default-allow when nothing was recorded --------------------- */
    {
        HybridAi h;
        double v[IN_DIM];
        hybrid_ai_init(&h);
        encode_pair(v, 0, 0);
        check(hybrid_coverage_admits(&h, pin, pout, v, IN_DIM) == 1,
              "no coverage record => default-allow (existing units untouched)");
        check(hybrid_coverage_rows(&h, pin, pout) == 0, "no rows recorded yet");
        hybrid_ai_free(&h);
    }

    /* ---- 2/3. gate ON: in-coverage serves, out-of-coverage defers -------- */
    check(build_and_replay(&ai, &ctx, "on", &hl, &hc, &hr, &ab) == 0,
          "capture 12/16, mine, replay (gate ON)");
    check(hybrid_coverage_rows(personal_ai_hybrid(&ai), pin, pout) == 12,
          "coverage recorded = the 12 rows the contract certified");
    {
        double seen[IN_DIM], unseen[IN_DIM];
        encode_pair(seen, 0, 0);   /* idx 0 — captured */
        encode_pair(unseen, 0, 3); /* idx 3 — held out */
        check(hybrid_coverage_admits(personal_ai_hybrid(&ai), pin, pout, seen,
                                     IN_DIM) == 1,
              "captured input is inside certified coverage");
        check(hybrid_coverage_admits(personal_ai_hybrid(&ai), pin, pout, unseen,
                                     IN_DIM) == 0,
              "unseen input is outside certified coverage");
    }
    check(hl == 0, "no held-out input answered from own weights");
    check(ab == 4, "all 4 held-out inputs hit the coverage abstain");
    check(hr == 4, "teacher answered every declined request (not dropped)");
    check(hc == 4, "held-out answers are CORRECT (was 0/4 before S6)");
    {
        PersonalAiReport tot;
        personal_ai_totals(&ai, &tot);
        check(tot.local_hits >= 12, "in-coverage traffic still served locally");
        check(tot.coverage_abstains == 4, "totals record the coverage refusals");
    }
    personal_ai_close(&ai);

    /* ---- 4. the gate is load-bearing ------------------------------------ */
    setenv("CNET_COVERAGE_ABSTAIN", "0", 1);
    check(build_and_replay(&ai, &ctx, "off", &hl, &hc, &hr, &ab) == 0,
          "same run with CNET_COVERAGE_ABSTAIN=0");
    check(ab == 0, "gate disabled => no coverage abstains");
    check(hl == 4, "gate disabled => unit answers all held-out itself");
    check(hc == 0, "gate disabled => every held-out answer is WRONG again");
    printf("      ^ this is the S3/E2 regression reproduced on demand:\n"
           "        without the gate the unit displaces a correct teacher\n"
           "        answer with a confident wrong one.\n");
    personal_ai_close(&ai);
    unsetenv("CNET_COVERAGE_ABSTAIN");

    /* ---- 5. S7: the gate survives a restart ----------------------------- */
    {
        int rc = build_and_replay_ex(&ai, &ctx, "rt", 0, 1, &hl, &hc, &hr, &ab);
        if (rc != 0) printf("      build_and_replay_ex(seal) rc=%d\n", rc);
        check(rc == 0, "S7: capture 12/16, mine, durable seal, checkpoint");
    }
    check(ab == 4, "S7: gate holds in the original process");
    personal_ai_close(&ai);
    {
        FILE *fp = fopen("tmp_cov_rt.cnb.coverage", "r");
        check(fp != NULL, "S7: coverage persisted beside the base");
        if (fp) {
            char hdr[32] = {0};
            check(fgets(hdr, sizeof hdr, fp) != NULL &&
                      strncmp(hdr, "CNET_COVERAGE v1", 16) == 0,
                  "S7: sidecar carries a versioned header");
            fclose(fp);
        }
    }
    /* Reopen the same base — no capture, no mine. Whatever gates now came off
       disk, exactly as it would after a lane restart. */
    check(build_and_replay_ex(&ai, &ctx, "rt", 1, 0, &hl, &hc, &hr, &ab) == 0,
          "S7: reopen base without re-mining");
    check(hybrid_coverage_rows(personal_ai_hybrid(&ai), pin, pout) == 12,
          "S7: 12 certified rows restored from disk");
    check(hl == 0, "S7: still no held-out input answered from own weights");
    check(ab == 4, "S7: coverage abstains survive the restart");
    check(hr == 4, "S7: teacher still answers the declined requests");
    check(hc == 4, "S7: held-out still CORRECT 4/4 after restart");
    {
        PersonalAiReport tot;
        personal_ai_totals(&ai, &tot);
        check(tot.local_hits >= 12,
              "S7: in-coverage traffic still served locally after restart");
    }
    personal_ai_close(&ai);

    /* Bit-exactness: membership is memcmp, so a lossy round-trip would
       silently abstain on inputs that ARE covered. */
    {
        PersonalAi a2;
        double seen[IN_DIM];
        check(build_and_replay_ex(&a2, &ctx, "rt", 1, 0, &hl, &hc, &hr, &ab) == 0,
              "S7: second reopen");
        encode_pair(seen, 0, 0);
        check(hybrid_coverage_admits(personal_ai_hybrid(&a2), pin, pout, seen,
                                     IN_DIM) == 1,
              "S7: restored rows match bit-exactly (%.17g round-trip)");
        personal_ai_close(&a2);
    }

    /* The sidecar is what carries the gate across the restart: delete it,
       reopen the same base (unit still sealed in the CNB), and the confident
       wrong answers come straight back. */
    {
        PersonalAi a3;
        check(remove("tmp_cov_rt.cnb.coverage") == 0, "S7: remove sidecar only");
        check(build_and_replay_ex(&a3, &ctx, "rt", 1, 0, &hl, &hc, &hr, &ab) == 0,
              "S7: reopen with unit but no coverage file");
        check(ab == 0, "S7: no coverage restored => no abstains");
        check(hl == 4, "S7: unit answers all held-out itself again");
        check(hc == 0, "S7: and gets all 4 WRONG — the sidecar is load-bearing");
        personal_ai_close(&a3);
    }

    /* ---- 6. PORT_RAW is not gated (documented limit) -------------------- */
    {
        HybridAi h;
        Port rin, rout;
        double v[4] = {0.1, 0.2, 0.3, 0.4};
        double rows[4] = {9.0, 9.0, 9.0, 9.0};
        hybrid_ai_init(&h);
        memset(&rin, 0, sizeof rin);
        memset(&rout, 0, sizeof rout);
        rin.family = PORT_RAW;
        rin.field_width = 4;
        rin.field_count = 1;
        snprintf(rin.tag, sizeof rin.tag, "raw_in");
        rout = rin;
        snprintf(rout.tag, sizeof rout.tag, "raw_out");
        check(hybrid_coverage_record(&h, rin, rout, "raw_unit", rows, 1, 4) == 0,
              "coverage can be recorded for a RAW port");
        check(hybrid_coverage_admits(&h, rin, rout, v, 4) == 1,
              "RAW inputs stay ungated — exact match is meaningless there");
        hybrid_ai_free(&h);
    }

    remove("tmp_cov_on.cnb");
    remove("tmp_cov_on.gaps.txt");
    remove("tmp_cov_on.cnb.coverage");
    remove("tmp_cov_off.cnb");
    remove("tmp_cov_off.gaps.txt");
    remove("tmp_cov_off.cnb.coverage");
    remove("tmp_cov_rt.cnb");
    remove("tmp_cov_rt.gaps.txt");
    remove("tmp_cov_rt.cnb.coverage");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("COVERAGE_ABSTAIN_PASS checks=%d heldout_correct=4/4 "
               "was=0/4\n", checks);
        return 0;
    }
    printf("COVERAGE_ABSTAIN_FAIL failures=%d\n", failures);
    return 1;
}
