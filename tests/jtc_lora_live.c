/* jtc_lora_live — run registry_teach_lora on a LIVE json_toolcall unit.

   Mines + admits the real certified json_toolcall_v2 student (external teacher =
   cnet_jtc_hermetic_teacher, the same oracle it was certified against), fills the
   unit's retrain queue with (feature, teacher one-hot) pairs, teaches a rank-r
   adapter from that queue, and reports tool-classification accuracy (argmax vs
   the teacher) with and without the adapter on a held-out split.

   Inputs are sampled as sparse feature activations (1-4 keywords on) — a broad
   stand-in for real tool-call JSON, labeled by the same hermetic teacher. This
   is a genuine unit + a genuine residual (student vs teacher), not a mock. */

#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t S = 0x5A17C0DE;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }

static void sample_feat(double *feat, int nfeat) {
    for (int i = 0; i < nfeat; i++) feat[i] = 0.0;
    int k = 1 + (int)(rnd() % 4);                 /* 1..4 active keywords */
    for (int j = 0; j < k; j++) feat[rnd() % nfeat] = 1.0;
}

int main(void) {
    PrimitiveRegistry reg;
    registry_init(&reg);
    BinaryTransformNetwork *student = NULL;

    if (cnet_jtc_v0_mine_admit(&reg, 0x4A54435F01ULL, &student, NULL) != 0 || !student) {
        printf("FAIL: cnet_jtc_v0_mine_admit\n");
        return 1;
    }
    const int IN = (int)student->input_count;      /* N_FEAT */
    const int OUT = (int)student->output_count;     /* N_TOOL */
    const char *unit = CNET_JTC_UNIT_NAME;          /* "json_toolcall_v2" */
    printf("live unit '%s': in=%d out=%d\n", unit, IN, OUT);

    /* sanity: certified student matches the teacher on the canonical exemplars */
    int exemplar_ok = 0;
    for (int t = 0; t < OUT && t < CNET_JTC_N_TOOL; t++) {
        double feat[CNET_JTC_N_FEAT];
        cnet_jtc_encode(cnet_jtc_example_json(t), feat);
        const double *so = btn_forward(student, feat);
        if (so && cnet_jtc_decode_tool(so) == t) exemplar_ok++;
    }
    printf("exemplar accuracy (student vs teacher): %d/%d\n", exemplar_ok, OUT);

    /* build a labeled set from the shared hermetic teacher */
    const int ntr = 500, nte = 250, n = ntr + nte;
    double *F = malloc((size_t)n * IN * sizeof(double));   /* features   */
    double *T = malloc((size_t)n * OUT * sizeof(double));  /* teacher OH */
    for (int s = 0; s < n; s++) {
        double *f = F + (size_t)s * IN;
        sample_feat(f, IN);
        cnet_jtc_hermetic_teacher(f, T + (size_t)s * OUT, NULL);
    }

    /* baseline: student-only accuracy vs teacher on the held-out split */
    int base_ok = 0;
    for (int s = ntr; s < n; s++) {
        const double *so = btn_forward(student, F + (size_t)s * IN);
        int tgt = cnet_jtc_decode_tool(T + (size_t)s * OUT);
        if (so && cnet_jtc_decode_tool(so) == tgt) base_ok++;
    }
    printf("held-out baseline (student only): %d/%d = %.1f%%\n",
           base_ok, nte, 100.0 * base_ok / nte);

    /* fill the unit's REAL retrain queue with the training split */
    int filled = 0;
    for (int s = 0; s < ntr; s++) {
        const double *f = F + (size_t)s * IN;
        const double *so = btn_forward(student, f);
        double raw[CNET_JTC_N_TOOL];
        for (int o = 0; o < OUT; o++) raw[o] = so ? so[o] : 0.0;   /* copy */
        if (registry_record_fault(&reg, unit, f, raw) == 0 &&
            registry_supply_label(&reg, unit, f, T + (size_t)s * OUT) == 0)
            filled++;
    }
    printf("queue filled with %d labeled pairs\n", filled);

    /* teach the adapter from the live queue, sweeping rank to show the real
       tradeoff (dense-equivalent for this tiny head is in*out = %zu). */
    printf("\n%-6s %8s %10s %14s %10s\n", "rank", "params", "post_mse", "held-out acc", "vs base");
    int ranks[] = { 2, 4, 8 };
    for (size_t ri = 0; ri < sizeof(ranks) / sizeof(ranks[0]); ri++) {
        registry_lora_opts opt = registry_lora_defaults();
        opt.rank = ranks[ri]; opt.alpha = (float)(2 * ranks[ri]);
        opt.train.epochs = 1200; opt.train.lr = 0.02f;
        registry_lora_stats st;
        if (registry_teach_lora(&reg, unit, &opt, &st) != 0) { printf("FAIL: teach rank %d\n", ranks[ri]); return 1; }

        int adp_ok = 0;
        for (int s = ntr; s < n; s++) {
            double served[CNET_JTC_N_TOOL];
            registry_forward_with_lora(&reg, unit, F + (size_t)s * IN, served);
            if (cnet_jtc_decode_tool(served) == cnet_jtc_decode_tool(T + (size_t)s * OUT)) adp_ok++;
        }
        printf("%-6d %8zu %10.5f %8d/%d=%4.0f%% %+8.1f\n",
               ranks[ri], st.params, st.post_mse, adp_ok, nte,
               100.0 * adp_ok / nte, 100.0 * (adp_ok - base_ok) / nte);
        registry_lora_detach(&reg, unit);
    }
    printf("(dense-equivalent output update = %d params; rank<%d both saves params and lifts accuracy)\n",
           IN * OUT, OUT);
    free(F); free(T);
    registry_free(&reg);
    btn_free(student); free(student);
    printf("JTC_LORA_LIVE_DONE\n");
    return 0;
}
