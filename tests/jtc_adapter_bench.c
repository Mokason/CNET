/* jtc_adapter_bench — measure JTC accuracy before vs after LoRA adapter.
 * Writes ghost-eval compatible delta file when CNET_PROMOTE_EVAL_DELTA is set
 * (delta = (acc_on - acc_off) as fraction). */
#include "../include/cnet_heldout.h"
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Graded capability: acc_on is expected to MOVE as the adapter is retrained, so
   the fixture declares the floor rather than an exact value. Everything else in
   the declared case — which metric is graded, whether certification must
   precede serving, the adapter-off/on baselines — is asserted, so the numbers
   in the fixture are what this run is judged against. */
#define JTC_CAPABILITY_ID "json_toolcall_adapter"
#define JTC_HELDOUT_CASE  "adapter-on-routing-accuracy"
/* What this binary actually is and where its held-out pairs actually come
   from. Declared fields are compared against these, so a fixture describing a
   different skill or a different source is describing a different experiment. */
#define JTC_SKILL_ID      "json_toolcall_v2"
#define JTC_PAIRS_SOURCE  "fault bus labelled vectors (source=jtc)"

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
    CnetHeldOut heldout;
    int heldout_rc = cnet_heldout_open(&heldout, JTC_CAPABILITY_ID);
    int certify_before_serve;
    int heldout_ok = 1;

    if (heldout_rc < 0) {
        fprintf(stderr, "FAIL: declared held-out fixture is unusable\n");
        return 2;
    }
    /* Read before the run so a fixture that demands serving WITHOUT prior
       certification is refused rather than silently reinterpreted. */
    certify_before_serve =
        cnet_heldout_num(&heldout, JTC_HELDOUT_CASE, "certify_before_serve",
                         1.0) != 0.0;
    if (!certify_before_serve) {
        printf("JTC_ADAPTER_BENCH_FAIL reason=fixture_waives_certify_before_serve\n");
        cnet_heldout_verdict(&heldout, JTC_HELDOUT_CASE, 0);
        (void)cnet_heldout_finish(&heldout);
        cnet_heldout_close(&heldout);
        return 1;
    }

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

    /* --- declared held-out grading ----------------------------------------
       Every field retained in the declared case is compared against something
       this run MEASURED. The previous version only required
       `on_baseline > off_baseline`, which any pair of numbers in the right
       order satisfies -- so the two baselines could be edited freely and the
       certificate stayed green. They are now compared to the measured arms
       within the tolerance the fixture itself declares. */
    {
        char metric_name[32], skill_name[64], pairs_from[96];
        double floor = cnet_heldout_num(&heldout, JTC_HELDOUT_CASE,
                                        "minimum_accuracy_with_adapter", 0.55);
        double on_baseline = cnet_heldout_num(&heldout, JTC_HELDOUT_CASE,
                                             "adapter_on_baseline", 0.7375);
        double off_baseline = cnet_heldout_num(&heldout, JTC_HELDOUT_CASE,
                                              "adapter_off_baseline", 0.2975);
        double tolerance = cnet_heldout_num(&heldout, JTC_HELDOUT_CASE,
                                            "baseline_tolerance", 0.05);
        double on_drift, off_drift;
        (void)cnet_heldout_str(&heldout, JTC_HELDOUT_CASE, "metric",
                               metric_name, sizeof metric_name, "acc_on");
        (void)cnet_heldout_str(&heldout, JTC_HELDOUT_CASE, "skill",
                               skill_name, sizeof skill_name, JTC_SKILL_ID);
        (void)cnet_heldout_str(&heldout, JTC_HELDOUT_CASE, "held_out_pairs_from",
                               pairs_from, sizeof pairs_from, JTC_PAIRS_SOURCE);
        if (strcmp(metric_name, "acc_on") != 0) {
            printf("HELDOUT_SHAPE_MISMATCH metric declared=%s graded=acc_on\n",
                   metric_name);
            heldout_ok = 0;
        }
        if (strcmp(skill_name, JTC_SKILL_ID) != 0) {
            printf("HELDOUT_SHAPE_MISMATCH skill declared=%s ran=%s\n",
                   skill_name, JTC_SKILL_ID);
            heldout_ok = 0;
        }
        if (strcmp(pairs_from, JTC_PAIRS_SOURCE) != 0) {
            printf("HELDOUT_SHAPE_MISMATCH held_out_pairs_from declared=%s "
                   "ran=%s\n", pairs_from, JTC_PAIRS_SOURCE);
            heldout_ok = 0;
        }
        if (!(tolerance > 0.0) || tolerance > 0.5) {
            printf("HELDOUT_SHAPE_MISMATCH baseline_tolerance=%.4f "
                   "outside (0,0.5]\n", tolerance);
            heldout_ok = 0;
        }
        if (acc_on < floor) {
            printf("HELDOUT_FLOOR_MISS acc_on=%.4f floor=%.4f\n", acc_on, floor);
            heldout_ok = 0;
        }
        /* Causal baselines: the declared numbers describe the arms this bench
           measures, so they are checked against what it measured. */
        on_drift = acc_on - on_baseline;
        if (on_drift < 0) on_drift = -on_drift;
        off_drift = acc_off - off_baseline;
        if (off_drift < 0) off_drift = -off_drift;
        if (on_drift > tolerance) {
            printf("HELDOUT_BASELINE_DRIFT arm=adapter_on measured=%.4f "
                   "declared=%.4f drift=%.4f tolerance=%.4f\n",
                   acc_on, on_baseline, on_drift, tolerance);
            heldout_ok = 0;
        }
        if (off_drift > tolerance) {
            printf("HELDOUT_BASELINE_DRIFT arm=adapter_off measured=%.4f "
                   "declared=%.4f drift=%.4f tolerance=%.4f\n",
                   acc_off, off_baseline, off_drift, tolerance);
            heldout_ok = 0;
        }
        printf("HELDOUT_GRADED metric=acc_on value=%.4f floor=%.4f "
               "declared_on_baseline=%.4f declared_off_baseline=%.4f "
               "tolerance=%.4f skill=%s source=%s\n",
               acc_on, floor, on_baseline, off_baseline, tolerance, skill_name,
               pairs_from);
        cnet_heldout_verdict(&heldout, JTC_HELDOUT_CASE, heldout_ok);
    }
    if (!heldout_ok) {
        printf("JTC_ADAPTER_BENCH_FAIL faults=%d acc_off=%.4f acc_on=%.4f "
               "delta=%+.4f reason=held_out_case_not_met\n",
               faults, acc_off, acc_on, delta);
        (void)cnet_heldout_finish(&heldout);
        cnet_heldout_close(&heldout);
        free(HF); free(HT); free(base_tool); free(VF); free(VT);
        return 1;
    }

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

    /* The receipt is emitted last so it can only follow a completed run. */
    if (cnet_heldout_finish(&heldout) != 0) {
        fprintf(stderr, "FAIL: declared held-out fixture was not honoured\n");
        cnet_heldout_close(&heldout);
        free(HF); free(HT); free(base_tool); free(VF); free(VT);
        return 1;
    }
    cnet_heldout_close(&heldout);
    free(HF); free(HT); free(base_tool); free(VF); free(VT);
    return 0;
}
