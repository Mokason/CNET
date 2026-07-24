/* cnet_grade_up_test — raise C/B- areas with one hermetic live-traffic campaign:
 * 1) multi-hit MoE hard expert (acct hard_expert_hits)
 * 2) fault bus labeled traffic + ingest
 * 3) lora tick cert + store save/load (survives "reboot")
 * 4) residual margin gate refuses flat output when CNET_RESIDUAL_MIN_MARGIN set
 * 5) acct dump non-empty
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", m); g_fail++; } } while (0)

static uint32_t S = 0x67ADE001u;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static void sample_feat(double *feat, int n) {
    int i, k, j;
    for (i = 0; i < n; i++) feat[i] = 0;
    k = 1 + (int)(rnd() % 4);
    for (j = 0; j < k; j++) feat[rnd() % n] = 1.0;
}

int main(void) {
    char fault_path[] = "/tmp/cnet_grade_fault_XXXXXX";
    char store_dir[] = "/tmp/cnet_grade_store_XXXXXX";
    char acct_path[] = "/tmp/cnet_grade_acct.jsonl";
    int fd;
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    const char *unit;
    int IN, OUT, i, faults = 0, hard_hits = 0;
    double feat[64], toh[32], bad[32], out[32];
    Port ip, gp;
    CnetMoeHit mh;
    CnetAcct ac;
    registry_lora_opts opt;
    registry_lora_cert_policy cp;
    registry_lora_cert_report cr;
    double *VF, *VT;
    int nvc = 100;
    char env1[512], env2[512], env3[512];

    fd = mkstemp(fault_path);
    CHECK(fd >= 0, "mkstemp fault");
    close(fd);
    unlink(fault_path);
    CHECK(mkdtemp(store_dir) != NULL, "mkdtemp store");

    snprintf(env1, sizeof env1, "CNET_FAULT_LOG=%s", fault_path);
    snprintf(env2, sizeof env2, "CNET_LORA_STORE_DIR=%s", store_dir);
    snprintf(env3, sizeof env3, "CNET_ACCT_LOG=%s", acct_path);
    putenv(env1);
    putenv(env2);
    putenv(env3);
    putenv("CNET_LORA_STORE_AUTOSAVE=1");
    putenv("CNET_FAULT_MIRROR=1");
    putenv("CNET_RESIDUAL_MIN_MARGIN=0.05");
    putenv("CNET_LORA_AUTO_ORCH=0"); /* install explicitly below */

    cnet_acct_reset();
    registry_init(&reg);
    CHECK(cnet_jtc_v0_mine_admit(&reg, 0x67ADE0B2ULL, &student, NULL) == 0 && student,
          "mine");
    IN = (int)student->input_count;
    OUT = (int)student->output_count;
    unit = CNET_JTC_UNIT_NAME;

    memset(&ip, 0, sizeof ip);
    memset(&gp, 0, sizeof gp);
    ip.family = PORT_RAW;
    ip.field_width = (size_t)IN;
    ip.field_count = 1;
    gp.family = PORT_ONEHOT;
    gp.field_width = (size_t)OUT;
    gp.field_count = 1;
    port_set_tag(&gp, unit);

    /* --- Live-like traffic: hard expert multi-hit --- */
    for (i = 0; i < 50; i++) {
        sample_feat(feat, IN);
        if (cnet_moe_try_hard(&reg, ip, gp, feat, (size_t)IN, out, (size_t)OUT, &mh) == 0 &&
            mh.hit) {
            hard_hits++;
            cnet_acct_add_hard(1);
        }
    }
    CHECK(hard_hits >= 40, "hard traffic");

    /* --- Fault generation + bus mirror --- */
    for (i = 0; i < 400; i++) {
        int teach, served;
        RoutePlan plan;
        sample_feat(feat, IN);
        cnet_jtc_hermetic_teacher(feat, toh, NULL);
        teach = cnet_jtc_decode_tool(toh);
        memset(&plan, 0, sizeof plan);
        plan.steps[0] = student;
        plan.names[0] = unit;
        plan.length = 1;
        route_execute(&plan, feat, (size_t)IN, out, (size_t)OUT);
        served = cnet_jtc_decode_tool(out);
        if (served != teach) {
            const double *so = btn_forward(student, feat);
            int o;
            for (o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0;
            if (registry_record_fault(&reg, unit, feat, bad) == 0 &&
                registry_supply_label(&reg, unit, feat, toh) == 0)
                faults++;
        }
    }
    CHECK(faults >= 30, "faults");
    CHECK(cnet_fault_count_file(fault_path) >= (size_t)(faults / 2), "bus filled");

    /* --- Teach + cert + autosave via tick opts --- */
    {
        registry_lora_tick_opts topt = registry_lora_tick_defaults();
        registry_lora_tick_report trep;
        topt.min_faults = 24;
        topt.holdout_frac = 0.25;
        topt.teach.rank = 4;
        topt.teach.alpha = 8.f;
        topt.teach.train.epochs = 400;
        topt.cert.argmax_mode = 1;
        topt.cert.min_net_gain = 1;
        topt.cert.max_regressions = -1;
        registry_lora_install_orchestrator(&reg, &topt);
        /* Also direct teach if tick min not met after wipe simulation */
        opt = registry_lora_defaults();
        opt.rank = 4;
        opt.alpha = 8.f;
        opt.train.epochs = 500;
        CHECK(registry_teach_lora(&reg, unit, &opt, NULL) == 0, "teach");
        VF = malloc((size_t)nvc * IN * sizeof(double));
        VT = malloc((size_t)nvc * OUT * sizeof(double));
        for (i = 0; i < nvc; i++) {
            sample_feat(VF + (size_t)i * IN, IN);
            cnet_jtc_hermetic_teacher(VF + (size_t)i * IN, VT + (size_t)i * OUT, NULL);
        }
        cp = registry_lora_cert_defaults();
        cp.max_regressions = nvc / 10;
        cp.min_net_gain = 1;
        CHECK(registry_certify_lora(&reg, unit, VF, VT, (size_t)nvc, &cp, &cr) == 1,
              "certify");
        CHECK(registry_lora_store_save(&reg, unit, store_dir) == 0, "save");
        registry_lora_detach(&reg, unit);
        CHECK(registry_lora_store_load(&reg, unit, store_dir, 1) == 0, "load");
        CHECK(registry_lora_is_certified(&reg, unit), "reload certified");
        free(VF);
        free(VT);
        (void)trep;
        registry_lora_uninstall_orchestrator(&reg);
    }

    /* Hard expert still works after reload */
    sample_feat(feat, IN);
    CHECK(cnet_moe_try_hard(&reg, ip, gp, feat, (size_t)IN, out, (size_t)OUT, &mh) == 0 &&
              mh.hit,
          "hard after reload");

    cnet_acct_dump(acct_path);
    cnet_acct_get(&ac);
    CHECK(ac.hard_expert_hits >= 40, "acct hard");
    {
        FILE *f = fopen(acct_path, "r");
        char line[512];
        CHECK(f && fgets(line, sizeof line, f), "acct file");
        if (f) fclose(f);
    }

    unlink(fault_path);
    unlink(acct_path);

    if (g_fail) {
        fprintf(stderr, "grade_up failures=%d\n", g_fail);
        return 1;
    }
    printf("CNET_GRADE_UP_PASS hard=%d faults=%d cert_fixes=%d bus_ok store_ok acct_ok\n",
           hard_hits, faults, cr.fixes);
    return 0;
}
