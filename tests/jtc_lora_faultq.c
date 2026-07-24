/* jtc_lora_faultq — validate cce_lora on a queue built from REAL runtime faults.

   Unlike jtc_lora_live (which labels a uniform sample), this runs inputs through
   the actual route executor and parks ONLY the genuine misclassifications
   (served tool != hermetic-teacher tool) as faults, labels each with the trusted
   oracle target, teaches an adapter from that fault-only queue, then measures on
   a held-out stream — through the same executor with serving on/off:
     - accuracy off vs on
     - FIXES:      unit wrong (off) -> correct (on)
     - REGRESSIONS: unit correct (off) -> wrong (on)   <- the safety check
   An adapter trained only on faults must fix errors without breaking the cases
   the unit already handled. */

#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t S = 0xFA017E77;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static void sample_feat(double *feat, int nfeat) {
    for (int i = 0; i < nfeat; i++) feat[i] = 0.0;
    int k = 1 + (int)(rnd() % 4);
    for (int j = 0; j < k; j++) feat[rnd() % nfeat] = 1.0;
}

/* served tool id through the real route executor (serving state per the flag) */
static int serve_tool(const RoutePlan *plan, const double *feat, int IN, int OUT) {
    double out[CNET_JTC_N_TOOL];
    if (route_execute_ex(plan, feat, (size_t)IN, out, (size_t)OUT, NULL) != 0) return -1;
    return cnet_jtc_decode_tool(out);
}

int main(void) {
    PrimitiveRegistry reg;
    registry_init(&reg);
    BinaryTransformNetwork *student = NULL;
    if (cnet_jtc_v0_mine_admit(&reg, 0x4A54435F01ULL, &student, NULL) != 0 || !student) {
        printf("FAIL: mine_admit\n"); return 1;
    }
    const int IN = (int)student->input_count, OUT = (int)student->output_count;
    const char *unit = CNET_JTC_UNIT_NAME;

    RoutePlan plan; memset(&plan, 0, sizeof plan);
    plan.steps[0] = student; plan.names[0] = unit; plan.length = 1; plan.strict = 0;

    /* ---- phase 1: stream inputs through the executor, park REAL faults ---- */
    registry_lora_disable_serving(&reg);
    const int stream = 900;
    int seen = 0, faults = 0;
    double feat[CNET_JTC_N_FEAT], toh[CNET_JTC_N_TOOL];
    for (int i = 0; i < stream; i++) {
        sample_feat(feat, IN);
        cnet_jtc_hermetic_teacher(feat, toh, NULL);
        int teach_tool = cnet_jtc_decode_tool(toh);
        int served = serve_tool(&plan, feat, IN, OUT);
        seen++;
        if (served != teach_tool) {                     /* genuine runtime fault */
            double bad[CNET_JTC_N_TOOL];
            const double *so = btn_forward(student, feat);
            for (int o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0.0;
            if (registry_record_fault(&reg, unit, feat, bad) == 0 &&
                registry_supply_label(&reg, unit, feat, toh) == 0)   /* oracle target */
                faults++;
        }
    }
    printf("phase 1: streamed %d, parked %d real faults (unit error rate %.1f%%)\n",
           seen, faults, 100.0 * faults / seen);

    /* ---- held-out stream (fresh samples), remembered for on/off compare ---- */
    const int nte = 300;
    double *HF = malloc((size_t)nte * IN * sizeof(double));
    int *HT = malloc((size_t)nte * sizeof(int));
    for (int s = 0; s < nte; s++) {
        double *f = HF + (size_t)s * IN;
        sample_feat(f, IN);
        cnet_jtc_hermetic_teacher(f, toh, NULL);
        HT[s] = cnet_jtc_decode_tool(toh);
    }
    /* baseline (serving off) tool per held-out input */
    int *base_tool = malloc((size_t)nte * sizeof(int));
    registry_lora_disable_serving(&reg);
    int base_ok = 0;
    for (int s = 0; s < nte; s++) {
        base_tool[s] = serve_tool(&plan, HF + (size_t)s * IN, IN, OUT);
        if (base_tool[s] == HT[s]) base_ok++;
    }
    printf("held-out baseline (serving off): %d/%d = %.1f%%\n",
           base_ok, nte, 100.0 * base_ok / nte);

    /* ---- teach on the fault-only queue, sweep rank, measure fixes/regressions ---- */
    printf("\n%-6s %8s %9s %9s %8s %11s\n",
           "rank", "params", "acc_on", "fixes", "regress", "net");
    int ranks[] = { 4, 8 };
    for (size_t ri = 0; ri < sizeof(ranks) / sizeof(ranks[0]); ri++) {
        registry_lora_opts opt = registry_lora_defaults();
        opt.rank = ranks[ri]; opt.alpha = (float)(2 * ranks[ri]);
        opt.train.epochs = 1500; opt.train.lr = 0.02f;
        registry_lora_stats st;
        if (registry_teach_lora(&reg, unit, &opt, &st) != 0) { printf("FAIL teach r%d\n", ranks[ri]); return 1; }

        registry_lora_enable_serving(&reg);
        int on_ok = 0, fixes = 0, regress = 0;
        for (int s = 0; s < nte; s++) {
            int on = serve_tool(&plan, HF + (size_t)s * IN, IN, OUT);
            int correct = (on == HT[s]);
            if (correct) on_ok++;
            if (base_tool[s] != HT[s] && correct) fixes++;         /* wrong -> right */
            if (base_tool[s] == HT[s] && !correct) regress++;       /* right -> wrong */
        }
        registry_lora_disable_serving(&reg);
        printf("%-6d %8zu %6d/%d %8d %8d %+9d\n",
               ranks[ri], st.params, on_ok, nte, fixes, regress, fixes - regress);
        registry_lora_detach(&reg, unit);
    }
    printf("(queue = %d real faults; fixes should exceed regressions for a net win)\n", faults);

    free(HF); free(HT); free(base_tool);
    registry_free(&reg);
    btn_free(student); free(student);
    printf("JTC_LORA_FAULTQ_DONE\n");
    return 0;
}
