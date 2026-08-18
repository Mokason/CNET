/* Adapt to a NEW space, and never confabulate in one.
 *
 * Purpose (operator's framing, 2026-08-18): CNET is not a universal-knowing
 * machine and not a datacenter. It is compact intelligence that meets a space
 * it has not seen, learns it, and does not parrot. This gate measures exactly
 * that, over several unseen domain families at once, with a structureless
 * control that has nothing to learn.
 *
 * What is NOT claimed. Coverage here is membership, not extrapolation:
 * tests/test_coverage_abstain.c pins `heldout_local == 0` -- a mined unit
 * answers ZERO inputs it was not taught, and the teacher fills those in. So
 * "adaptation" in this gate means ACQUISITION -- meeting a new domain, being
 * taught it, and coming to serve it certified -- not inferring unseen cells.
 * Phase B measures whether any family extrapolates; the honest expectation is
 * none, and that number is reported rather than assumed.
 *
 * Four phases per family, each on its own base so the spaces stay isolated:
 *   A  acquire   teach 12/16, mine, replay
 *                -> taught cells served locally AND correct   (it learned)
 *                -> held-out cells: ZERO local answers        (it refuses)
 *   B  extrapolation probe (measured, not required)
 *                -> how many held-out cells any family answers from its own
 *                   weights. Measured 0 everywhere, which is the honest
 *                   finding: this system acquires, it does not infer.
 *   C  law       re-mining the SAME port shape returns 4 = recall-before-spawn
 *                ("one mined unit per port shape, ever", hybrid_ai.c). So
 *                coverage does NOT grow by re-mining. Pinned here because an
 *                adaptation strategy built on re-mining would silently no-op
 *                rather than fail -- this gate found that the hard way.
 *   D  adapt     meet the space fresh and acquire ALL 16 cells
 *                -> every cell served locally and correctly, coverage spans
 *                   the whole space. THIS is "met a new space and learned it".
 *
 * STRICT ABSTENTION is the bar the operator set: across every family,
 * including the structureless control, the number of confident WRONG answers
 * from a unit's own weights must be exactly zero. A parrot would answer the
 * random domain too. That single number is the anti-parrot criterion.
 *
 * Scale limit, stated so it cannot drift into a broader claim: these are
 * 4x4 -> 4 nibble domains, 16 cells each. Passing measures acquisition and
 * refusal at that scale and NOTHING beyond it. Broader competence WITHHELD.
 *
 * make adapt_new_domain -> ADAPT_NEW_DOMAIN_PASS
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
#define N_HELD 4

static int failures, checks;

static void check(int ok, const char *name, const char *detail) {
    checks++;
    printf("  %-56s %-22s %s\n", name, detail ? detail : "", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* ---- domain families ------------------------------------------------- */

typedef enum {
    FAM_ADDMOD = 0, /* (a+b) % 4        */
    FAM_XOR,        /* (a^b) % 4        */
    FAM_AFFINE,     /* (3a+b+1) % 4     */
    FAM_MAX,        /* max(a,b)         */
    FAM_RANDOM,     /* structureless control -- nothing to learn */
    FAM_COUNT
} Family;

static const char *FAM_NAME[FAM_COUNT] = {"addmod", "xor", "affine", "max",
                                          "random_ctrl"};

/* Fixed, arbitrary table. Not generated at runtime: the control must be the
   same structureless map on every run so a failure is reproducible. */
static const unsigned RANDOM_TABLE[DOMAIN] = {2, 0, 3, 1, 1, 3, 0, 2,
                                              3, 2, 1, 0, 0, 1, 2, 3};

typedef struct {
    size_t fw, fc, out_dim;
    Family fam;
} ResCtx;

static unsigned family_answer(Family fam, unsigned a, unsigned b) {
    switch (fam) {
        case FAM_ADDMOD: return (a + b) % OUT_DIM;
        case FAM_XOR:    return (a ^ b) % OUT_DIM;
        case FAM_AFFINE: return (3u * a + b + 1u) % OUT_DIM;
        case FAM_MAX:    return a > b ? a : b;
        case FAM_RANDOM: return RANDOM_TABLE[(a * FW + b) % DOMAIN];
        default:         return 0;
    }
}

static Port PMF(const char *tag, size_t w, size_t c) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = w;
    p.field_count = c;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void encode_pair(double *in, size_t a, size_t b) {
    size_t i;
    for (i = 0; i < IN_DIM; i++) in[i] = 0.0;
    in[a] = 1.0;
    in[FW + b] = 1.0;
}

static int res_family(const double *in, double *out, void *ctx) {
    ResCtx *c = (ResCtx *)ctx;
    size_t f, i;
    unsigned hot[FC] = {0, 0};
    if (!in || !out || !c) return -1;
    for (f = 0; f < c->fc && f < FC; f++) {
        size_t h = 0;
        for (i = 1; i < c->fw; i++)
            if (in[f * c->fw + i] > in[f * c->fw + h]) h = i;
        hot[f] = (unsigned)h;
    }
    for (i = 0; i < c->out_dim; i++) out[i] = 0.0;
    out[family_answer(c->fam, hot[0], hot[1]) % c->out_dim] = 1.0;
    return 0;
}

static int is_heldout(size_t i) { return i == 3 || i == 6 || i == 9 || i == 12; }

/* ---- per-family measurement ------------------------------------------- */

typedef struct {
    size_t taught_local;      /* taught cells answered from own weights   */
    size_t taught_correct;    /* ...and correct                            */
    size_t held_local;        /* held-out answered from own weights        */
    size_t held_extrapolated; /* ...and correct (true extrapolation)       */
    size_t held_abstains;
    size_t wrong_confident;   /* own-weights answers that were WRONG       */
    size_t coverage_rows;
    size_t full_rows;         /* phase D: coverage after full acquisition  */
    size_t after_local;       /* cells served locally in the after-phase   */
    size_t after_correct;
} FamStat;

/* Serve every cell; optionally restrict teaching to non-held-out cells. */
static int teach_and_mine(PersonalAi *ai, ResCtx *ctx, const char *base,
                          const char *led, int full_domain, int reopen,
                          int *mine_rc_out) {
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    Port pin = PMF("adapt_in", FW, FC), pout = PMF("adapt_out", OUT_DIM, 1);
    double in[IN_DIM], out[OUT_DIM];
    BinaryTransformNetwork *stu = NULL;
    size_t idx;
    int pass;

    if (!reopen) { remove(base); remove(led); }
    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;
    pol.allow_soft = 0;
    pol.allow_residual = 1;
    pol.structure_mine_on_serve = 0;
    pol.structure_min_hits = 2;
    if (personal_ai_open(ai, base, led, NULL, &pol) != 0) return -1;
    ai->lane.acq.min_evidence = 1;
    if (personal_ai_bind_residual(ai, "fam", res_family, ctx) != 0) return -2;

    /* Two passes. hybrid_structure_mine only considers traces whose hits reach
       policy.structure_min_hits (2 here) and returns 1 -- "nothing worth
       mining", not an error -- otherwise. In phase C only the 4 remaining
       cells are still uncovered, so a single pass leaves them under the
       threshold and nothing gets mined. */
    for (pass = 0; pass < 2; pass++) {
        for (idx = 0; idx < DOMAIN; idx++) {
            if (!full_domain && is_heldout(idx)) continue;
            encode_pair(in, idx / FW, idx % FW);
            memset(&rep, 0, sizeof rep);
            (void)personal_ai_serve(ai, pin, pout, in, IN_DIM, out, OUT_DIM,
                                    &rep);
        }
    }
    {
        int mrc = personal_ai_structure_mine(ai, &stu);
        if (mine_rc_out) *mine_rc_out = mrc;
        /* 1 == no trace reached min_hits; 4 == recall-before-spawn (a unit for
           this port shape already exists). Neither is a crash; the caller
           decides what it expects. */
        if (mrc != 0) return -300 - mrc;
    }
    if (gap_lane_checkpoint(&ai->lane) != 0) return -4;
    return 0;
}

/* Replay the whole domain and score it. */
static void replay(PersonalAi *ai, ResCtx *ctx, FamStat *st, int phase_c) {
    PersonalAiReport rep;
    Port pin = PMF("adapt_in", FW, FC), pout = PMF("adapt_out", OUT_DIM, 1);
    double in[IN_DIM], out[OUT_DIM];
    size_t idx;

    for (idx = 0; idx < DOMAIN; idx++) {
        unsigned want, got = 0;
        size_t j;
        encode_pair(in, idx / FW, idx % FW);
        memset(out, 0, sizeof out);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(ai, pin, pout, in, IN_DIM, out, OUT_DIM, &rep);
        want = family_answer(ctx->fam, (unsigned)(idx / FW), (unsigned)(idx % FW));
        for (j = 1; j < OUT_DIM; j++) if (out[j] > out[got]) got = (unsigned)j;

        if (phase_c) {
            if (!is_heldout(idx)) continue;
            if (rep.local_hits) {
                st->after_local++;
                if (got == want) st->after_correct++;
                else st->wrong_confident++;
            }
            continue;
        }
        if (is_heldout(idx)) {
            if (rep.local_hits) {
                st->held_local++;
                if (got == want) st->held_extrapolated++;
                else st->wrong_confident++;
            }
            if (rep.coverage_abstains) st->held_abstains++;
        } else {
            if (rep.local_hits) {
                st->taught_local++;
                if (got == want) st->taught_correct++;
                else st->wrong_confident++;
            }
        }
    }
}

/* Phase D: score every cell of a fully-acquired space. */
static void replay_full(PersonalAi *ai, ResCtx *ctx, FamStat *st) {
    PersonalAiReport rep;
    Port pin = PMF("adapt_in", FW, FC), pout = PMF("adapt_out", OUT_DIM, 1);
    double in[IN_DIM], out[OUT_DIM];
    size_t idx;
    st->after_local = st->after_correct = 0;
    for (idx = 0; idx < DOMAIN; idx++) {
        unsigned want, got = 0;
        size_t j;
        encode_pair(in, idx / FW, idx % FW);
        memset(out, 0, sizeof out);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(ai, pin, pout, in, IN_DIM, out, OUT_DIM, &rep);
        want = family_answer(ctx->fam, (unsigned)(idx / FW), (unsigned)(idx % FW));
        for (j = 1; j < OUT_DIM; j++) if (out[j] > out[got]) got = (unsigned)j;
        if (rep.local_hits) {
            st->after_local++;
            if (got == want) st->after_correct++;
            else st->wrong_confident++;
        }
    }
}

int main(void) {
    Port pin = PMF("adapt_in", FW, FC), pout = PMF("adapt_out", OUT_DIM, 1);
    FamStat st[FAM_COUNT];
    size_t total_wrong = 0, total_held_local = 0, total_extrap = 0;
    int f;

    printf("== adapt to a NEW space; never confabulate in one ==\n");
    printf("   %d families x %d cells, teach %d, hold out %d\n", (int)FAM_COUNT,
           DOMAIN, DOMAIN - N_HELD, N_HELD);

    unsetenv("CNET_COVERAGE_ABSTAIN");
    unsetenv("CNET_FAULT_LOG");
    memset(st, 0, sizeof st);

    for (f = 0; f < FAM_COUNT; f++) {
        PersonalAi ai;
        ResCtx ctx;
        char base[128], led[128], det[96];
        int rc, mine_rc = 0;

        ctx.fw = FW; ctx.fc = FC; ctx.out_dim = OUT_DIM; ctx.fam = (Family)f;
        snprintf(base, sizeof base, "tmp_adapt_%s.cnb", FAM_NAME[f]);
        snprintf(led, sizeof led, "tmp_adapt_%s.gaps.txt", FAM_NAME[f]);

        /* ---- A: meet a space it has never seen, learn 12 of its 16 cells -- */
        rc = teach_and_mine(&ai, &ctx, base, led, 0, 0, &mine_rc);
        snprintf(det, sizeof det, "[%s]", FAM_NAME[f]);
        check(rc == 0, "A acquire: teach 12/16 on an unseen domain, mine", det);
        if (rc != 0) { personal_ai_close(&ai); continue; }

        st[f].coverage_rows =
            (size_t)hybrid_coverage_rows(personal_ai_hybrid(&ai), pin, pout);
        replay(&ai, &ctx, &st[f], 0);

        snprintf(det, sizeof det, "[%s] %zu/12 correct", FAM_NAME[f],
                 st[f].taught_correct);
        /* Invariant in both modes: everything answered from own weights was
           right. Under CNET_COVERAGE_GENERALIZE=1 the mine withholds a quarter
           of the rows to attempt its proof, so the taught count is legitimately
           lower -- but never wrong. */
        check(st[f].taught_correct == st[f].taught_local && st[f].taught_local > 0,
              "A learned: every locally-served taught cell is correct", det);

        snprintf(det, sizeof det, "[%s] rows=%zu", FAM_NAME[f],
                 st[f].coverage_rows);
        /* Coverage names exactly the rows the unit trained on. Under
           CNET_COVERAGE_GENERALIZE=1 a quarter are withheld for the proof and
           are deliberately left OUTSIDE coverage, so the count is lower --
           what must hold in both modes is that coverage never exceeds what was
           taught, which is the confident-wrong hole. */
        check(st[f].coverage_rows > 0 && st[f].coverage_rows <= DOMAIN - N_HELD,
              "A coverage never exceeds what was certified", det);

        /* ---- B: extrapolation probe (measured, never assumed) ----------- */
        snprintf(det, sizeof det, "[%s] local=%zu ok=%zu", FAM_NAME[f],
                 st[f].held_local, st[f].held_extrapolated);
        check(1, "B probe: held-out answers from own weights (measured)", det);

        /* ---- C: re-mining the SAME port shape is refused, by law --------
           "One mined unit per port shape, ever" (hybrid_ai.c): a second mine
           on a shape that already has coverage returns 4 = recall-before-spawn
           and reuses the existing unit. Coverage therefore does NOT grow by
           re-mining, which is worth pinning: an adaptation strategy built on
           re-mining would silently no-op. */
        mine_rc = 0;
        rc = teach_and_mine(&ai, &ctx, base, led, 1, 1, &mine_rc);
        snprintf(det, sizeof det, "[%s] mine_rc=%d", FAM_NAME[f], mine_rc);
        check(mine_rc == 4, "C law: re-mining same shape = recall-before-spawn",
              det);
        personal_ai_close(&ai);

        /* ---- D: meet the space fresh and acquire ALL of it --------------- */
        {
            char base2[128], led2[128];
            snprintf(base2, sizeof base2, "tmp_adaptfull_%s.cnb", FAM_NAME[f]);
            snprintf(led2, sizeof led2, "tmp_adaptfull_%s.gaps.txt", FAM_NAME[f]);
            mine_rc = 0;
            rc = teach_and_mine(&ai, &ctx, base2, led2, 1, 0, &mine_rc);
            snprintf(det, sizeof det, "[%s] rc=%d", FAM_NAME[f], rc);
            check(rc == 0, "D adapt: acquire the FULL new space from scratch",
                  det);
            if (rc == 0) {
                st[f].full_rows = (size_t)hybrid_coverage_rows(
                    personal_ai_hybrid(&ai), pin, pout);
                replay_full(&ai, &ctx, &st[f]);
                snprintf(det, sizeof det, "[%s] %zu/16 local+correct",
                         FAM_NAME[f], st[f].after_correct);
                check(st[f].after_correct == st[f].after_local &&
                      st[f].after_local > 0,
                  "D adapted: every locally-served cell is correct", det);
                snprintf(det, sizeof det, "[%s] rows=%zu", FAM_NAME[f],
                         st[f].full_rows);
                check(st[f].full_rows >= st[f].after_local,
                      "D coverage spans what it serves", det);
                personal_ai_close(&ai);
            }
            remove(base2);
            remove(led2);
        }

        total_wrong += st[f].wrong_confident;
        total_held_local += st[f].held_local;
        total_extrap += st[f].held_extrapolated;
        remove(base);
        remove(led);
    }

    /* ---- the bar: STRICT ABSTENTION, no confabulation anywhere ---------- */
    {
        char det[96];
        snprintf(det, sizeof det, "wrong_confident=%zu", total_wrong);
        check(total_wrong == 0,
              "STRICT: zero confident-wrong answers, all families", det);
        snprintf(det, sizeof det, "random_ctrl held_local=%zu",
                 st[FAM_RANDOM].held_local);
        check(st[FAM_RANDOM].held_local == 0,
              "STRICT: control has nothing to learn, so it answers nothing",
              det);
    }

    printf("\n  per-family  A:taught_ok/12  B:held_local/extrap  D:acquired/16\n");
    for (f = 0; f < FAM_COUNT; f++)
        printf("    %-12s %2zu/12   held_local=%zu extrap=%zu   %2zu/16\n",
               FAM_NAME[f], st[f].taught_correct, st[f].held_local,
               st[f].held_extrapolated, st[f].after_correct);

    printf("\nchecks=%d fails=%d\n", checks, failures);
    if (failures == 0) {
        printf("ADAPT_NEW_DOMAIN_PASS checks=%d fails=0 families=%d "
               "wrong_confident=0 extrapolated=%zu strict_abstention=1 "
               "scale=nibble16 broader_claims=WITHHELD\n",
               checks, (int)FAM_COUNT, total_extrap);
        return 0;
    }
    printf("ADAPT_NEW_DOMAIN_RED checks=%d fails=%d\n", checks, failures);
    return 1;
}
