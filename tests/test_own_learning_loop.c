/* Own-learning loop gate — organic Tier-C intake + substitution KPI.
 *
 * Closes the edge the fault bus was designed for but never had: a Tier-C
 * residual answer is an externally-labelled training pair, and CNET used to
 * throw it away (personal_ai.c recorded the hit and dropped the pair). Every
 * adapter CNET had certified was trained on seeded rows.
 *
 * The gate proves four things:
 *   1. a residual serve emits a labelled fault row (organic intake exists);
 *   2. the row is provenance-stamped source=surprise / label_kind=residual, so
 *      organic capture is distinguishable from a synthetic seeder;
 *   3. a Tier-A certified serve emits NOTHING — the anti-collapse rule, that
 *      CNET never trains on its own certified output;
 *   4. the substitution KPI moves and is persistable.
 *
 * make own_learning_loop → OWN_LEARNING_LOOP_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/cnet_fault.h"
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

/* Slurp a file into buf for substring provenance assertions. */
static int slurp(const char *path, char *buf, size_t cap) {
    FILE *fp = fopen(path, "rb");
    size_t n;
    if (!fp) return -1;
    n = fread(buf, 1, cap - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return (int)n;
}

/* Tier A unit: identity over one-hot SYM. Mirrors the hybrid gate's admit. */
static int admit_id(PrimitiveRegistry *reg) {
    BinaryTransformNetwork *btn;
    double in[SYM][SYM], tg[SYM][SYM];
    Contract c;
    Specialist s;
    Port pin = P("owl_in"), pout = P("owl_id");
    int i;
    btn = calloc(1, sizeof *btn);
    if (!btn) return -1;
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
    if (contract_init_borrowed(&c, "owl_id", btn, (const double *)in,
                               (const double *)tg, SYM) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, "owl_id") != 0 ||
        specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    return 0;
}

int main(void) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep, tot;
    Port pin = P("owl_in");
    Port pres = P("owl_res");
    Port pid = P("owl_id");
    double in[SYM], out[SYM];
    const char *base = "tmp_own_learning.cnb";
    const char *led = "tmp_own_learning.gaps.txt";
    const char *faults = "tmp_own_learning_faults.jsonl";
    const char *kpi = "tmp_own_learning_kpi.json";
    char buf[8192];
    size_t after_residual = 0, after_local = 0;
    int i;

    remove(base);
    remove(led);
    remove(faults);
    remove(kpi);

    printf("== own-learning loop: organic Tier-C intake + substitution KPI ==\n");

    /* Capture is a no-op unless the bus is addressed — prove that first. */
    unsetenv("CNET_FAULT_LOG");
    unsetenv("CNET_RESIDUAL_CAPTURE");
    unsetenv("CNET_FAULT_MIRROR");

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;  /* force Tier C */
    pol.allow_soft = 0;
    pol.allow_residual = 1;
    pol.structure_mine_on_serve = 0; /* isolate capture from mining */

    check(personal_ai_open(&ai, base, led, NULL, &pol) == 0, "open");
    check(personal_ai_bind_residual(&ai, "res_rot1", hybrid_hermetic_residual,
                                    (void *)(uintptr_t)SYM) == 0,
          "bind hermetic residual");

    /* --- phase 0: no CNET_FAULT_LOG => capture must stay silent ---------- */
    oh(in, 0);
    memset(&rep, 0, sizeof rep);
    check(personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep) == 0,
          "serve reaches Tier C residual");
    check(rep.residual_hits == 1, "residual hit recorded");
    check(cnet_fault_count_file(faults) == 0,
          "no bus addressed => no rows written");

    /* --- phase 1: organic intake ---------------------------------------- */
    setenv("CNET_FAULT_LOG", faults, 1);
    for (i = 0; i < SYM; i++) {
        oh(in, i);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep);
    }
    after_residual = cnet_fault_count_file(faults);
    check(after_residual > 0, "residual serve emits organic fault rows");
    check(after_residual == SYM, "one row per distinct input (dedup-aware)");

    /* --- phase 2: provenance -------------------------------------------- */
    check(slurp(faults, buf, sizeof buf) > 0, "fault log readable");
    check(strstr(buf, "\"source\":\"surprise\"") != NULL,
          "rows stamped source=surprise (not a seeder)");
    check(strstr(buf, "\"label_kind\":\"residual\"") != NULL,
          "rows stamped label_kind=residual");
    check(strstr(buf, "\"note\":\"residual_serve\"") != NULL,
          "rows stamped note=residual_serve");
    check(strstr(buf, "\"unit\":\"res_4x4\"") != NULL,
          "unit keyed by port shape for vector reload");
    check(strstr(buf, "\"in\":[") != NULL && strstr(buf, "\"tgt\":[") != NULL,
          "rows carry labelled (in,tgt) vectors");

    /* Vectors must reload for registry_lora_tick to be able to train. */
    {
        double vin[SYM * SYM], vtg[SYM * SYM];
        size_t n = cnet_fault_load_vectors(faults, "res_4x4", SYM, SYM,
                                           vin, vtg, SYM);
        check(n == (size_t)SYM, "captured pairs reload as training vectors");
    }

    /* --- phase 3: anti-collapse — Tier A must never feed the learner ----- */
    check(admit_id(&ai.lane.reg) == 0, "tier A unit admitted");
    oh(in, 1);
    memset(&rep, 0, sizeof rep);
    check(personal_ai_serve(&ai, pin, pid, in, SYM, out, SYM, &rep) == 0,
          "serve hits certified Tier A");
    check(rep.local_hits == 1, "local (Tier A) hit recorded");
    check(rep.residual_captures == 0, "Tier A serve captures nothing");
    after_local = cnet_fault_count_file(faults);
    check(after_local == after_residual,
          "anti-collapse: certified output never enters the fault bus");

    /* --- phase 4: explicit off switch ------------------------------------ */
    setenv("CNET_RESIDUAL_CAPTURE", "0", 1);
    oh(in, 2);
    memset(&rep, 0, sizeof rep);
    (void)personal_ai_serve(&ai, pin, pres, in, SYM, out, SYM, &rep);
    check(cnet_fault_count_file(faults) == after_local,
          "CNET_RESIDUAL_CAPTURE=0 disables capture");
    unsetenv("CNET_RESIDUAL_CAPTURE");

    /* --- phase 5: substitution KPI --------------------------------------- */
    personal_ai_totals(&ai, &tot);
    check(tot.residual_hits > 0 && tot.local_hits > 0, "mixed tier traffic");
    /* Honest counter: serves with no bus (phase 0) and with capture disabled
       (phase 4) are residual hits that were NOT offered to the learner. */
    check(tot.residual_captures == (size_t)SYM,
          "captures count offered pairs only");
    check(tot.residual_captures < tot.residual_hits,
          "capture counter does not blindly mirror residual hits");
    check(personal_ai_kpi_json(&ai, buf, sizeof buf) > 0, "KPI json emitted");
    check(strstr(buf, "\"substitution_rate\":") != NULL,
          "KPI carries substitution_rate");
    check(strstr(buf, "\"residual_rate\":") != NULL,
          "KPI carries residual_rate");
    check(strstr(buf, "\"abstain_rate\":") != NULL,
          "KPI carries abstain_rate (guards residual_rate gaming)");
    check(strstr(buf, "\"residual_captures\":") != NULL,
          "KPI carries residual_captures");
    printf("  kpi: %s\n", buf);

    check(personal_ai_kpi_write(&ai, kpi) == 0, "KPI persists to disk");
    check(slurp(kpi, buf, sizeof buf) > 0, "KPI file readable");

    /* The claim under test: own tiers displace residual as units land. */
    {
        double own = (double)(tot.local_hits + tot.soft_hits);
        double served = own + (double)tot.residual_hits +
                        (double)tot.teacher_helps;
        check(served > 0.0 && own / served > 0.0,
              "substitution_rate > 0 once a certified unit serves");
    }

    personal_ai_close(&ai);
    remove(base);
    remove(led);
    remove(faults);
    remove(kpi);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("OWN_LEARNING_LOOP_PASS checks=%d\n", checks);
        return 0;
    }
    printf("OWN_LEARNING_LOOP_FAIL failures=%d\n", failures);
    return 1;
}
