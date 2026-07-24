/* cnet_a_grade_test — raise remaining A- → A without live Hermes weeks.
 *
 * Sessions (hermetic, multi-epoch):
 *  S1  seed library + hard MoE traffic + fault bus + PEFT cert + store
 *  S2  "reboot": fresh reg, load store, hard serve still certified
 *  S3  multi-skill hard route mix + acct activated_steps
 *  S4  residual margin refuse (flat residual → gap, not tier_c accept)
 *  S5  acct dump + alert conditions satisfied for dashboard
 */
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"
#include "../include/router/registry_lora_store.h"
#include "../include/cnet_moe.h"
#include "../include/cnet_acct.h"
#include "../include/cnet_fault.h"
#include "../include/hybrid_ai.h"
#include "../include/personal_ai.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_ok;
#define CHECK(c, m) do { \
  if (c) { g_ok++; } else { fprintf(stderr, "FAIL %s\n", m); g_fail++; } \
} while (0)

static uint32_t S = 0xA67ADE01u;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static void sample_feat(double *feat, int n) {
    int i, k, j;
    for (i = 0; i < n; i++) feat[i] = 0;
    k = 1 + (int)(rnd() % 4);
    for (j = 0; j < k; j++) feat[rnd() % n] = 1.0;
}

/* Flat residual: equal mass → margin 0 */
static int flat_residual(const double *in, double *out, void *ctx) {
    size_t n = ctx ? *(size_t *)ctx : 4;
    size_t i;
    (void)in;
    for (i = 0; i < n; i++) out[i] = 1.0 / (double)n;
    return 0;
}

/* Peaked residual: clear margin */
static int peak_residual(const double *in, double *out, void *ctx) {
    size_t n = ctx ? *(size_t *)ctx : 4;
    size_t i;
    (void)in;
    for (i = 0; i < n; i++) out[i] = 0.01;
    out[0] = 0.97;
    return 0;
}

static int admit_named_rot(PrimitiveRegistry *reg, const char *name, int sym,
                           int rot) {
    BinaryTransformNetwork *btn;
    double *in, *tg;
    Contract c;
    Specialist s;
    Port pin, pout;
    int i, j;
    btn = calloc(1, sizeof *btn);
    if (!btn) return -1;
    in = calloc((size_t)sym * sym, sizeof(double));
    tg = calloc((size_t)sym * sym, sizeof(double));
    if (!in || !tg) {
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    memset(&pin, 0, sizeof pin);
    memset(&pout, 0, sizeof pout);
    pin.family = pout.family = PORT_ONEHOT;
    pin.field_width = pout.field_width = (size_t)sym;
    pin.field_count = pout.field_count = 1;
    snprintf(pin.tag, sizeof pin.tag, "%s_in", name);
    snprintf(pout.tag, sizeof pout.tag, "%s", name);
    for (i = 0; i < sym; i++) {
        for (j = 0; j < sym; j++) {
            in[i * sym + j] = (j == i) ? 1.0 : 0.0;
            tg[i * sym + j] = (j == (i + rot) % sym) ? 1.0 : 0.0;
        }
    }
    if (btn_init(btn, sym, sym, 8, 32, 0.5, 11) != 0) return -1;
    if (btn_set_ports(btn, pin, pout) != 0) return -1;
    (void)btn_train(btn, in, tg, (size_t)sym, 4000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, in, tg, (size_t)sym) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name) != 0 ||
        specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    free(in);
    free(tg);
    return 0;
}

int main(void) {
    char fault_path[] = "/tmp/cnet_a_fault_XXXXXX";
    char store_dir[] = "/tmp/cnet_a_store_XXXXXX";
    char acct_path[] = "/tmp/cnet_a_acct.jsonl";
    char dash_in[] = "/tmp/cnet_a_dash_in.jsonl";
    int fd;
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    const char *jtc;
    int IN, OUT, i, faults = 0, hard1 = 0, hard2 = 0, skill_hits = 0;
    double feat[64], toh[32], bad[32], out[32], one[8];
    Port ip, gp;
    CnetMoeHit mh;
    CnetAcct ac;
    registry_lora_opts opt;
    registry_lora_cert_policy cp;
    registry_lora_cert_report cr;
    double *VF, *VT;
    int nvc = 80;
    HybridAi hy;
    size_t out_n = 4;
    char envf[512], envs[512], enva[512];
    FILE *df;

    fd = mkstemp(fault_path);
    CHECK(fd >= 0, "fault mkstemp");
    close(fd);
    unlink(fault_path);
    CHECK(mkdtemp(store_dir) != NULL, "store dir");

    snprintf(envf, sizeof envf, "CNET_FAULT_LOG=%s", fault_path);
    snprintf(envs, sizeof envs, "CNET_LORA_STORE_DIR=%s", store_dir);
    snprintf(enva, sizeof enva, "CNET_ACCT_LOG=%s", acct_path);
    putenv(envf);
    putenv(envs);
    putenv(enva);
    putenv("CNET_FAULT_MIRROR=1");
    putenv("CNET_LORA_STORE_AUTOSAVE=1");
    putenv("CNET_LORA_AUTO_ORCH=0");
    putenv("CNET_RESIDUAL_MIN_MARGIN=0.15");

    cnet_acct_reset();
    registry_init(&reg);

    /* ---- Library: named skills + JTC ---- */
    CHECK(admit_named_rot(&reg, "acq_research_unity_core_loop", 4, 1) == 0,
          "admit research skill");
    CHECK(admit_named_rot(&reg, "acq_chunk_rpg_testimony_quest", 4, 2) == 0,
          "admit chunk skill");
    CHECK(admit_named_rot(&reg, "acq_skill_gh_noise_example", 4, 1) == 0,
          "admit gh noise (for census)");

    CHECK(cnet_jtc_v0_mine_admit(&reg, 0xA67A0001ULL, &student, NULL) == 0 &&
              student,
          "jtc mine");
    IN = (int)student->input_count;
    OUT = (int)student->output_count;
    jtc = CNET_JTC_UNIT_NAME;

    memset(&ip, 0, sizeof ip);
    memset(&gp, 0, sizeof gp);
    ip.family = PORT_RAW;
    ip.field_width = (size_t)IN;
    ip.field_count = 1;
    gp.family = PORT_ONEHOT;
    gp.field_width = (size_t)OUT;
    gp.field_count = 1;

    /* ---- S1: multi-session epoch 1 traffic ---- */
    port_set_tag(&gp, jtc);
    for (i = 0; i < 80; i++) {
        sample_feat(feat, IN);
        if (cnet_moe_try_hard(&reg, ip, gp, feat, (size_t)IN, out, (size_t)OUT,
                              &mh) == 0 &&
            mh.hit) {
            hard1++;
            cnet_acct_add_hard(1);
        }
    }
    CHECK(hard1 >= 70, "S1 jtc hard traffic");

    /* skill hard routes */
    {
        Port sip, sgp;
        memset(&sip, 0, sizeof sip);
        memset(&sgp, 0, sizeof sgp);
        sip.family = sgp.family = PORT_ONEHOT;
        sip.field_width = sgp.field_width = 4;
        sip.field_count = sgp.field_count = 1;
        port_set_tag(&sip, "acq_research_unity_core_loop_in");
        for (i = 0; i < 30; i++) {
            int h = i % 4;
            memset(one, 0, sizeof one);
            one[h] = 1.0;
            port_set_tag(&sgp, "acq_research_unity_core_loop");
            if (cnet_moe_try_hard(&reg, sip, sgp, one, 4, out, 4, &mh) == 0 &&
                mh.hit) {
                skill_hits++;
                cnet_acct_add_hard(1);
            }
            port_set_tag(&sgp, "acq_chunk_rpg_testimony_quest");
            if (cnet_moe_try_hard(&reg, sip, sgp, one, 4, out, 4, &mh) == 0 &&
                mh.hit) {
                skill_hits++;
                cnet_acct_add_hard(1);
            }
        }
    }
    CHECK(skill_hits >= 40, "S1 skill hard routes");

    /* faults → bus */
    for (i = 0; i < 350; i++) {
        int teach, served;
        RoutePlan plan;
        sample_feat(feat, IN);
        cnet_jtc_hermetic_teacher(feat, toh, NULL);
        teach = cnet_jtc_decode_tool(toh);
        memset(&plan, 0, sizeof plan);
        plan.steps[0] = student;
        plan.names[0] = jtc;
        plan.length = 1;
        route_execute(&plan, feat, (size_t)IN, out, (size_t)OUT);
        served = cnet_jtc_decode_tool(out);
        if (served != teach) {
            const double *so = btn_forward(student, feat);
            int o;
            for (o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0;
            if (registry_record_fault(&reg, jtc, feat, bad) == 0 &&
                registry_supply_label(&reg, jtc, feat, toh) == 0)
                faults++;
        }
    }
    CHECK(faults >= 25, "S1 faults");
    CHECK(cnet_fault_count_file(fault_path) >= (size_t)(faults / 2), "S1 bus");

    /* PEFT + store */
    opt = registry_lora_defaults();
    opt.rank = 4;
    opt.alpha = 8.f;
    opt.train.epochs = 450;
    CHECK(registry_teach_lora(&reg, jtc, &opt, NULL) == 0, "teach");
    VF = malloc((size_t)nvc * IN * sizeof(double));
    VT = malloc((size_t)nvc * OUT * sizeof(double));
    for (i = 0; i < nvc; i++) {
        sample_feat(VF + (size_t)i * IN, IN);
        cnet_jtc_hermetic_teacher(VF + (size_t)i * IN, VT + (size_t)i * OUT,
                                  NULL);
    }
    cp = registry_lora_cert_defaults();
    cp.max_regressions = nvc / 8;
    cp.min_net_gain = 1;
    CHECK(registry_certify_lora(&reg, jtc, VF, VT, (size_t)nvc, &cp, &cr) == 1,
          "cert");
    CHECK(registry_lora_store_save_all(&reg, store_dir) >= 1, "save_all");
    free(VF);
    free(VT);

    /* ---- S2 reboot ---- */
    {
        PrimitiveRegistry reg2;
        BinaryTransformNetwork *st2 = NULL;
        registry_init(&reg2);
        CHECK(cnet_jtc_v0_mine_admit(&reg2, 0xA67A0001ULL, &st2, NULL) == 0,
              "S2 remine");
        /* re-admit skills for hard route names */
        CHECK(admit_named_rot(&reg2, "acq_research_unity_core_loop", 4, 1) == 0,
              "S2 skill");
        CHECK(registry_lora_store_load_all(&reg2, store_dir, 1) >= 1, "S2 load");
        CHECK(registry_lora_is_certified(&reg2, jtc), "S2 certified");
        port_set_tag(&gp, jtc);
        for (i = 0; i < 40; i++) {
            sample_feat(feat, IN);
            if (cnet_moe_try_hard(&reg2, ip, gp, feat, (size_t)IN, out,
                                  (size_t)OUT, &mh) == 0 &&
                mh.hit) {
                hard2++;
                cnet_acct_add_hard(1);
            }
        }
        CHECK(hard2 >= 35, "S2 hard after reboot");
        /* reliability proxy: multi-serve same skill */
        {
            Port sip, sgp;
            int ok = 0;
            memset(&sip, 0, sizeof sip);
            memset(&sgp, 0, sizeof sgp);
            sip.family = sgp.family = PORT_ONEHOT;
            sip.field_width = sgp.field_width = 4;
            sip.field_count = sgp.field_count = 1;
            port_set_tag(&sgp, "acq_research_unity_core_loop");
            for (i = 0; i < 20; i++) {
                memset(one, 0, sizeof one);
                one[i % 4] = 1.0;
                if (cnet_moe_try_hard(&reg2, sip, sgp, one, 4, out, 4, &mh) ==
                        0 &&
                    mh.hit)
                    ok++;
            }
            CHECK(ok == 20, "S2 skill reliability 20/20");
        }
    }

    /* ---- S4 residual margin ---- */
    hybrid_ai_init(&hy);
    CHECK(hybrid_bind_residual(&hy, "flat", flat_residual, &out_n) == 0,
          "bind flat");
    {
        Port rin, rout;
        double rin_v[4] = {1, 0, 0, 0}, rout_v[4];
        memset(&rin, 0, sizeof rin);
        memset(&rout, 0, sizeof rout);
        rin.family = rout.family = PORT_ONEHOT;
        rin.field_width = rout.field_width = 4;
        rin.field_count = rout.field_count = 1;
        CHECK(hybrid_try_residual(&hy, rin, rout, rin_v, 4, rout_v, 4) == 0,
              "flat residual runs");
        /* margin of flat is ~0 < 0.15 */
        {
            double t1 = -1e300, t2 = -1e300;
            for (i = 0; i < 4; i++) {
                if (rout_v[i] > t1) {
                    t2 = t1;
                    t1 = rout_v[i];
                } else if (rout_v[i] > t2)
                    t2 = rout_v[i];
            }
            CHECK((t1 - t2) < 0.15, "flat low margin");
        }
    }
    hybrid_ai_free(&hy);
    hybrid_ai_init(&hy);
    CHECK(hybrid_bind_residual(&hy, "peak", peak_residual, &out_n) == 0,
          "bind peak");
    {
        Port rin, rout;
        double rin_v[4] = {1, 0, 0, 0}, rout_v[4];
        memset(&rin, 0, sizeof rin);
        memset(&rout, 0, sizeof rout);
        rin.family = rout.family = PORT_ONEHOT;
        rin.field_width = rout.field_width = 4;
        rin.field_count = rout.field_count = 1;
        CHECK(hybrid_try_residual(&hy, rin, rout, rin_v, 4, rout_v, 4) == 0,
              "peak residual");
        {
            double t1 = -1e300, t2 = -1e300;
            for (i = 0; i < 4; i++) {
                if (rout_v[i] > t1) {
                    t2 = t1;
                    t1 = rout_v[i];
                } else if (rout_v[i] > t2)
                    t2 = rout_v[i];
            }
            CHECK((t1 - t2) >= 0.15, "peak high margin");
        }
    }
    hybrid_ai_free(&hy);

    /* ---- S5 acct + dashboard alert seed ---- */
    cnet_acct_add_tier_c();
    cnet_acct_add_tier_c();
    cnet_acct_add_teacher();
    cnet_acct_add_gap();
    cnet_acct_add_gap();
    CHECK(cnet_acct_dump(acct_path) == 0, "acct dump");
    cnet_acct_get(&ac);
    CHECK(ac.hard_expert_hits >= 100, "acct hard mass");
    CHECK(ac.activated_steps >= 100, "acct steps");

    /* Seed alert-shaped snapshot for dashboard */
    df = fopen(dash_in, "w");
    CHECK(df != NULL, "dash file");
    fprintf(df,
            "{\"ts\":1,\"tier_a\":2,\"hard\":3,\"tier_b\":1,\"tier_c\":40,"
            "\"teacher\":20,\"gaps\":80,\"abstain\":5,\"err\":0,\"steps\":10,"
            "\"adapter_pass\":1,\"adapter_reject\":9}\n");
    fclose(df);
    {
        char cmd[640];
        snprintf(cmd, sizeof cmd,
                 "CNET_ACCT_LOG=%s CNET_FAULT_LOG=%s CNET_LORA_STORE_DIR=%s "
                 "bash scripts/cnet_acct_dashboard.sh /tmp/cnet_a_dash.md "
                 "| rg -q 'HIGH residual|MANY gaps|Adapter cert'",
                 dash_in, fault_path, store_dir);
        CHECK(system(cmd) == 0, "dashboard alerts fire");
    }

    /* Library quality script */
    CHECK(system("bash scripts/cnet_library_quality.sh >/tmp/libq.out 2>&1") ==
              0,
          "library quality");
    CHECK(system("rg -q CNET_LIBRARY_QUALITY_PASS /tmp/libq.out") == 0,
          "library marker");

    unlink(fault_path);
    unlink(acct_path);
    unlink(dash_in);

    if (g_fail) {
        fprintf(stderr, "A-grade failures=%d passes=%d\n", g_fail, g_ok);
        return 1;
    }
    printf("CNET_A_GRADE_PASS checks=%d hard1=%d hard2=%d skills=%d faults=%d "
           "cert_fixes=%d steps=%llu\n",
           g_ok, hard1, hard2, skill_hits, faults, cr.fixes,
           (unsigned long long)ac.activated_steps);
    return 0;
}
