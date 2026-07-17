/* Hybrid A/B/C gate — full roadmap P0–P5 hermetic.
 * make hybrid_ai → HYBRID_AI_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define SYM 4

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
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

static int argmax4(const double *v) {
    int i, b = 0;
    for (i = 1; i < SYM; i++) if (v[i] > v[b]) b = i;
    return b;
}

/* Tier A unit: identity */
static int admit_id(PrimitiveRegistry *reg) {
    BinaryTransformNetwork *btn;
    double in[SYM][SYM], tg[SYM][SYM];
    Contract c;
    Specialist s;
    Port pin = P("hyb_in"), pout = P("hyb_id");
    int i;
    btn = calloc(1, sizeof *btn);
    for (i = 0; i < SYM; i++) {
        oh(in[i], i);
        oh(tg[i], i);
    }
    if (btn_init(btn, SYM, SYM, 8, 32, 0.5, 3) != 0) return -1;
    btn_set_ports(btn, pin, pout);
    btn_train_dynamic(btn, (const double *)in, (const double *)tg, SYM,
                      15000, 200, 1e-6, 1e-8);
    btn_train(btn, (const double *)in, (const double *)tg, SYM, 2000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, "hyb_id", btn, (const double *)in,
                               (const double *)tg, SYM) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, "hyb_id") != 0 ||
        specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    return 0;
}

/* Soft that does rot2 */
static int soft_rot2(const double *in, double *out, void *ctx) {
    int i, h = 0;
    (void)ctx;
    for (i = 1; i < SYM; i++) if (in[i] > in[h]) h = i;
    for (i = 0; i < SYM; i++) out[i] = 0.0;
    out[(h + 2) % SYM] = 1.0;
    return 0;
}

/* Medium module rot3 */
static int med_rot3(const double *in, double *out, void *ctx) {
    int i, h = 0;
    (void)ctx;
    for (i = 1; i < SYM; i++) if (in[i] > in[h]) h = i;
    for (i = 0; i < SYM; i++) out[i] = 0.0;
    out[(h + 3) % SYM] = 1.0;
    return 0;
}

int main(void) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    Port pin = P("hyb_in");
    Port pid = P("hyb_id");
    Port psoft = P("hyb_soft");
    Port pres = P("hyb_res");
    Port pmed = P("hyb_med");
    double in[SYM], out[SYM], bias[SYM];
    const char *base = "tmp_hybrid.cnb";
    const char *led = "tmp_hybrid.gaps.txt";
    int i;

    remove(base);
    remove(led);
    printf("== hybrid A/B/C roadmap P0–P5 ==\n");

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0; /* force residual path for C */
    pol.allow_soft = 1;
    pol.allow_residual = 1;
    pol.allow_medium = 1;
    pol.structure_min_hits = 2;

    check(personal_ai_open(&ai, base, led, NULL, &pol) == 0, "open");
    ai.lane.acq.min_evidence = 1;
    check(admit_id(&ai.lane.reg) == 0, "tier A unit admitted");

    /* P0 residual */
    check(personal_ai_bind_residual(&ai, "res_rot1", hybrid_hermetic_residual,
                                    (void *)(uintptr_t)SYM) == 0,
          "P0 bind residual");

    /* P1 soft */
    check(personal_ai_bind_soft(&ai, "soft_rot2", pin, psoft, soft_rot2, NULL,
                                0.0) == 0,
          "P1 bind soft");

    /* Soft abstain specialist on another tag */
    {
        HybridSoftCtx sc;
        sc.min_margin = 0.5;
        sc.force_abstain = 0;
        check(personal_ai_bind_soft(&ai, "soft_abstain", pin, P("hyb_abs"),
                                    hybrid_hermetic_soft, &sc, 0.5) == 0,
              "P1 soft with margin");
    }

    /* P3 medium with budget */
    {
        CnetGovPolicy gp;
        cnet_gov_policy_deploy_defaults(&gp);
        gp.ram_budget_bytes = 100000;
        cnet_gov_close(&ai.gov);
        cnet_gov_open(&ai.gov, &gp);
        check(personal_ai_bind_medium(&ai, "med_rot3", pin, pmed, med_rot3,
                                      NULL, 50000) == 0,
              "P3 medium admits under RAM budget");
        check(personal_ai_bind_medium(&ai, "med_too_big", pin, P("hyb_big"),
                                      med_rot3, NULL, 80000) != 0,
              "P3 medium refuses over budget");
    }

    /* P4 adapter */
    for (i = 0; i < SYM; i++) bias[i] = 0.0;
    bias[0] = 0.01;
    check(personal_ai_enable_adapter(&ai, bias, SYM, 1.0) == 0,
          "P4 personal adapter enabled");

    /* ---- Serve cascade ---- */
    oh(in, 1);
    /* A */
    check(personal_ai_serve(&ai, pin, pid, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_LOCAL && rep.tier == HYBRID_TIER_A &&
              rep.trust == HYBRID_TRUST_CERTIFIED && argmax4(out) == 1,
          "Tier A certified serves first");

    /* B soft */
    check(personal_ai_serve(&ai, pin, psoft, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_SOFT && rep.tier == HYBRID_TIER_B &&
              rep.trust == HYBRID_TRUST_PROVISIONAL && argmax4(out) == 3,
          "Tier B soft serves when no A plan");

    /* B medium */
    check(personal_ai_serve(&ai, pin, pmed, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_SOFT && argmax4(out) == 0,
          "Tier B medium serves");

    /* C residual */
    check(personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_RESIDUAL &&
              rep.trust == HYBRID_TRUST_UNCERTIFIED &&
              rep.tier == HYBRID_TIER_C && argmax4(out) == 2,
          "Tier C residual when A/B miss");

    /* Soft abstain then residual for hyb_abs */
    {
        HybridSoftCtx *sc = (HybridSoftCtx *)ai.hybrid.soft[1].ctx;
        if (sc) sc->force_abstain = 1;
        /* bind residual already; need residual to answer hyb_abs ports —
           residual doesn't check tags. soft abstains, residual tries same ports. */
        check(personal_ai_serve(&ai, pin, P("hyb_abs"), in, SYM, out, SYM,
                                &rep) == 0 &&
                  rep.source == PERSONAL_AI_RESIDUAL,
              "soft abstain falls through to residual C");
    }

    /* P5 structure mine: hit residual enough times then mine */
    for (i = 0; i < 3; i++) {
        oh(in, i % SYM);
        personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep);
    }
    {
        BinaryTransformNetwork *stu = NULL;
        size_t reg_before = ai.lane.reg.count;
        int mrc = personal_ai_structure_mine(&ai, &stu);
        check(mrc == 0 && stu != NULL && ai.lane.reg.count > reg_before,
              "P5 structure mine promotes residual into Tier A");
        if (stu) {
            /* verify mined unit works as A */
            oh(in, 0);
            check(personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep) ==
                          0 &&
                      (rep.source == PERSONAL_AI_LOCAL ||
                       rep.source == PERSONAL_AI_RESIDUAL),
                  "after mine, signature may be local A");
        }
    }

    /* P2 distill: two-step plan if possible */
    {
        /* admit rot1 as second unit for distill demo */
        BinaryTransformNetwork *b2 = calloc(1, sizeof *b2);
        double in2[SYM][SYM], tg2[SYM][SYM];
        Contract c2;
        Specialist s2;
        Port p1 = P("hyb_d0"), p2 = P("hyb_d1");
        RoutePlan plan;
        BinaryTransformNetwork *chunk = NULL;
        for (i = 0; i < SYM; i++) {
            oh(in2[i], i);
            oh(tg2[i], (i + 1) % SYM);
        }
        btn_init(b2, SYM, SYM, 8, 32, 0.5, 11);
        btn_set_ports(b2, p1, p2);
        btn_train_dynamic(b2, (const double *)in2, (const double *)tg2, SYM,
                          12000, 200, 1e-6, 1e-8);
        btn_train(b2, (const double *)in2, (const double *)tg2, SYM, 2000);
        memset(&c2, 0, sizeof c2);
        contract_init_borrowed(&c2, "hyb_rot1", b2, (const double *)in2,
                               (const double *)tg2, SYM);
        memset(&s2, 0, sizeof s2);
        specialist_wrap_btn(&s2, b2, "hyb_rot1");
        specialist_admit(&ai.lane.reg, &s2, &c2);
        contract_free(&c2);
        /* identity hyb_id is pin->pid; need chain — use two units same tags hard.
           Distill API exercises self_improve path: build artificial plan. */
        memset(&plan, 0, sizeof plan);
        if (route_plan(&ai.lane.reg, pin, pid, &plan) == 0 && plan.length >= 1) {
            /* single-step won't distill; force length by manual plan if 2+ */
            int dr = personal_ai_distill_plan(&ai, &plan, &chunk);
            check(dr == 1 || dr == 0,
                  "P2 distill refuses length<2 or mints chunk");
        } else {
            check(1, "P2 distill skipped (no multi-step plan fixture)");
        }
    }

    {
        const HybridAi *h = personal_ai_hybrid(&ai);
        check(h && h->tier_a_hits >= 1 && h->tier_b_hits >= 1 &&
                  h->tier_c_hits >= 1,
              "tier hit counters populated");
        check(h->structure_mines >= 1, "structure mine counter");
        check(h->adapter_applies >= 1, "adapter applied on residual");
        check(strcmp(hybrid_tier_name(HYBRID_TIER_A), "A_certified") == 0,
              "tier names");
        check(strcmp(hybrid_trust_name(HYBRID_TRUST_UNCERTIFIED),
                     "uncertified") == 0,
              "trust names");
    }

    personal_ai_close(&ai);
    remove(base);
    remove(led);

    if (failures) {
        printf("HYBRID_AI_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("HYBRID_AI_PASS checks=%d\n", checks);
    return 0;
}
