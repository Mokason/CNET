/* jtc_adapter_bench — measure JTC accuracy before vs after LoRA adapter.
 * Writes ghost-eval compatible delta file when CNET_PROMOTE_EVAL_DELTA is set
 * (delta = (acc_on - acc_off) as fraction). */
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t S = 0xADA07B01u;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static void sample_feat(double *feat, int nfeat) {
    int i, k, j;
    for (i = 0; i < nfeat; i++) feat[i] = 0.0;
    k = 1 + (int)(rnd() % 4);
    for (j = 0; j < k; j++) feat[rnd() % nfeat] = 1.0;
}

static int serve_tool(const RoutePlan *plan, const double *feat, int IN, int OUT) {
    double out[32];
    if (route_execute_ex(plan, feat, (size_t)IN, out, (size_t)OUT, NULL) != 0)
        return -1;
    return cnet_jtc_decode_tool(out);
}

int main(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    const char *unit;
    int IN, OUT, i, faults = 0;
    RoutePlan plan;
    const int nte = 400;
    double *HF;
    int *HT, *base_tool;
    int base_ok = 0, on_ok = 0;
    registry_lora_opts opt;
    registry_lora_cert_policy cp;
    registry_lora_cert_report cr;
    double feat[64], toh[32], bad[32];
    double *VF, *VT;
    int nvc = 200;
    double acc_off, acc_on, delta;
    const char *delta_path;

    registry_init(&reg);
    if (cnet_jtc_v0_mine_admit(&reg, 0x4A54435FB0ULL, &student, NULL) != 0 || !student) {
        printf("FAIL mine\n"); return 1;
    }
    IN = (int)student->input_count; OUT = (int)student->output_count;
    unit = CNET_JTC_UNIT_NAME;
    memset(&plan, 0, sizeof plan);
    plan.steps[0] = student; plan.names[0] = unit; plan.length = 1;

    registry_lora_disable_serving(&reg);
    for (i = 0; i < 800; i++) {
        int teach, served;
        sample_feat(feat, IN);
        cnet_jtc_hermetic_teacher(feat, toh, NULL);
        teach = cnet_jtc_decode_tool(toh);
        served = serve_tool(&plan, feat, IN, OUT);
        if (served != teach) {
            const double *so = btn_forward(student, feat);
            int o; for (o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0;
            if (registry_record_fault(&reg, unit, feat, bad) == 0 &&
                registry_supply_label(&reg, unit, feat, toh) == 0)
                faults++;
        }
    }

    HF = malloc((size_t)nte * IN * sizeof(double));
    HT = malloc((size_t)nte * sizeof(int));
    base_tool = malloc((size_t)nte * sizeof(int));
    for (i = 0; i < nte; i++) {
        sample_feat(HF + (size_t)i * IN, IN);
        cnet_jtc_hermetic_teacher(HF + (size_t)i * IN, toh, NULL);
        HT[i] = cnet_jtc_decode_tool(toh);
    }
    registry_lora_disable_serving(&reg);
    for (i = 0; i < nte; i++) {
        base_tool[i] = serve_tool(&plan, HF + (size_t)i * IN, IN, OUT);
        if (base_tool[i] == HT[i]) base_ok++;
    }
    acc_off = (double)base_ok / (double)nte;

    opt = registry_lora_defaults();
    opt.rank = 4; opt.alpha = 8.f; opt.train.epochs = 800; opt.train.lr = 0.025f;
    if (registry_teach_lora(&reg, unit, &opt, NULL) != 0) {
        printf("FAIL teach faults=%d\n", faults); return 1;
    }
    VF = malloc((size_t)nvc * IN * sizeof(double));
    VT = malloc((size_t)nvc * OUT * sizeof(double));
    for (i = 0; i < nvc; i++) {
        sample_feat(VF + (size_t)i * IN, IN);
        cnet_jtc_hermetic_teacher(VF + (size_t)i * IN, VT + (size_t)i * OUT, NULL);
    }
    cp = registry_lora_cert_defaults();
    cp.max_regressions = nvc / 15;
    cp.min_net_gain = 1;
    if (registry_certify_lora(&reg, unit, VF, VT, (size_t)nvc, &cp, &cr) != 1) {
        printf("FAIL certify fixes=%d reg=%d\n", cr.fixes, cr.regressions);
        return 1;
    }
    registry_lora_enable_serving(&reg);
    for (i = 0; i < nte; i++) {
        int t = serve_tool(&plan, HF + (size_t)i * IN, IN, OUT);
        if (t == HT[i]) on_ok++;
    }
    acc_on = (double)on_ok / (double)nte;
    delta = acc_on - acc_off;

    printf("JTC_ADAPTER_BENCH_PASS faults=%d acc_off=%.4f acc_on=%.4f delta=%+.4f "
           "cert_fixes=%d cert_regress=%d\n",
           faults, acc_off, acc_on, delta, cr.fixes, cr.regressions);

    delta_path = getenv("CNET_PROMOTE_EVAL_DELTA");
    if (!delta_path || !delta_path[0])
        delta_path = "logs/ghost_eval_delta.txt";
    {
        FILE *f = fopen(delta_path, "w");
        if (f) {
            fprintf(f, "delta %.6f\n", delta);
            fprintf(f, "acc_off %.6f\nacc_on %.6f\n", acc_off, acc_on);
            fclose(f);
            printf("wrote %s\n", delta_path);
        }
    }

    free(HF); free(HT); free(base_tool); free(VF); free(VT);
    return 0;
}
