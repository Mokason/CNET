/* registry_lora_store_test — certify → save → detach → load → still certified */
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"
#include "../include/router/registry_lora_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t S = 0x5702E001;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static void sample_feat(double *feat, int nfeat) {
    int i, k, j;
    for (i = 0; i < nfeat; i++) feat[i] = 0.0;
    k = 1 + (int)(rnd() % 4);
    for (j = 0; j < k; j++) feat[rnd() % nfeat] = 1.0;
}

int main(void) {
    char dir[] = "/tmp/cnet_lora_store_XXXXXX";
    if (!mkdtemp(dir)) { printf("FAIL mkdtemp\n"); return 1; }
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    const char *unit;
    int IN, OUT, i, faults = 0;
    registry_lora_opts opt;
    registry_lora_cert_policy cp;
    registry_lora_cert_report cr;
    double feat[64], toh[32], bad[32];
    double *VF, *VT;
    int nvc = 120;

    registry_init(&reg);
    if (cnet_jtc_v0_mine_admit(&reg, 0x4A54435F77ULL, &student, NULL) != 0 || !student) {
        printf("FAIL mine\n"); return 1;
    }
    IN = (int)student->input_count; OUT = (int)student->output_count;
    unit = CNET_JTC_UNIT_NAME;

    for (i = 0; i < 500; i++) {
        sample_feat(feat, IN);
        cnet_jtc_hermetic_teacher(feat, toh, NULL);
        {
            RoutePlan plan; memset(&plan, 0, sizeof plan);
            double out[32];
            int teach = cnet_jtc_decode_tool(toh), served;
            plan.steps[0] = student; plan.names[0] = unit; plan.length = 1;
            route_execute_ex(&plan, feat, (size_t)IN, out, (size_t)OUT, NULL);
            served = cnet_jtc_decode_tool(out);
            if (served != teach) {
                const double *so = btn_forward(student, feat);
                int o; for (o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0;
                if (registry_record_fault(&reg, unit, feat, bad) == 0 &&
                    registry_supply_label(&reg, unit, feat, toh) == 0)
                    faults++;
            }
        }
    }
    if (faults < 40) { printf("FAIL few faults %d\n", faults); return 1; }

    opt = registry_lora_defaults();
    opt.rank = 4; opt.alpha = 8.f; opt.train.epochs = 500; opt.train.lr = 0.03f;
    if (registry_teach_lora(&reg, unit, &opt, NULL) != 0) {
        printf("FAIL teach\n"); return 1;
    }
    VF = malloc((size_t)nvc * IN * sizeof(double));
    VT = malloc((size_t)nvc * OUT * sizeof(double));
    for (i = 0; i < nvc; i++) {
        sample_feat(VF + (size_t)i * IN, IN);
        cnet_jtc_hermetic_teacher(VF + (size_t)i * IN, VT + (size_t)i * OUT, NULL);
    }
    cp = registry_lora_cert_defaults();
    cp.max_regressions = nvc / 10;
    cp.min_net_gain = 1;
    if (registry_certify_lora(&reg, unit, VF, VT, (size_t)nvc, &cp, &cr) != 1) {
        printf("FAIL certify fixes=%d regress=%d\n", cr.fixes, cr.regressions);
        /* still try save path with forced cert for store plumbing */
        /* fall through only if certified */
        free(VF); free(VT);
        return 1;
    }
    if (registry_lora_store_save(&reg, unit, dir) != 0) {
        printf("FAIL save\n"); return 1;
    }
    registry_lora_detach(&reg, unit);
    if (registry_lora_is_certified(&reg, unit)) {
        printf("FAIL still certified after detach\n"); return 1;
    }
    if (registry_lora_store_load(&reg, unit, dir, 1) != 0) {
        printf("FAIL load\n"); return 1;
    }
    if (!registry_lora_is_certified(&reg, unit)) {
        printf("FAIL not certified after load\n"); return 1;
    }
    free(VF); free(VT);
    printf("CNET_LORA_STORE_PASS faults=%d fixes=%d dir=%s\n", faults, cr.fixes, dir);
    return 0;
}
