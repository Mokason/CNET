/* personal_ai_lora_tick — end-to-end: the LIVE orchestrator (personal_ai_tick)
   with a LOADED base, adapter hook reordered AHEAD of gap_lane's dense heal.

   Two scenarios prove "cheap-retrain-first, dense-heal-fallback":
     A. reasonable gate -> the low-rank adapter CERTIFIES, is marked non-RESET so
        gap_lane's PRIM_RESET-gated heal SKIPS the unit, and the adapter serves
        (base student unchanged, executor serves base+delta).
     B. strict gate     -> the adapter is REJECTED, the unit stays PRIM_RESET, and
        gap_lane's dense heal runs as the fallback (base student itself improves).

   Seals json_toolcall_v2 into a fresh base, personal_ai_open()s it, parks
   oracle-labeled executor-detected faults, installs the orchestrator, ticks. No
   GPU / teacher lane. */

#include "../include/personal_ai.h"
#include "../include/gap_lane.h"
#include "../include/base.h"
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t S = 0x9E3779B9u;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }

/* Real production tool-call traffic: realistic JSON requests built from the
   actual tool names + arg keywords, encoded through the production
   cnet_jtc_encode path (substring keyword match) — not random feature bits. The
   mix mirrors real traffic: mostly clean canonical requests, plus underspecified
   (no "tool" wrapper — infer from args) and ambiguous (a second tool's name in a
   value) requests, which are where the closed-set student actually errs. */
static const char *const TRAFFIC_TOOL[8] = {
    "calculator", "memory_store", "memory_recall", "file_read",
    "cnet_recall", "web_search", "wiki_lookup", "final"
};
static const char *const TRAFFIC_ARGS[8] = {
    "\"expr\":\"n\"", "\"key\":\"k\",\"value\":\"v\"", "\"query\":\"q\"",
    "\"path\":\"f.txt\"", "\"cond\":0,\"current\":1", "\"query\":\"web thing\"",
    "\"query\":\"topic\"", "\"answer\":\"ok\""
};
static void gen_traffic(char *buf, size_t cap) {
    int t = (int)(rnd() % 8);
    int style = (int)(rnd() % 10);
    if (t == 7)                                   /* final: no tool wrapper */
        snprintf(buf, cap, "{\"final\":\"stop\",%s}", TRAFFIC_ARGS[7]);
    else if (style < 6)                           /* 60% clean canonical */
        snprintf(buf, cap, "{\"tool\":\"%s\",\"args\":{%s}}", TRAFFIC_TOOL[t], TRAFFIC_ARGS[t]);
    else if (style < 8)                           /* 20% underspecified (args only) */
        snprintf(buf, cap, "{\"args\":{%s}}", TRAFFIC_ARGS[t]);
    else {                                        /* 20% ambiguous (2nd tool named in a value) */
        int t2 = (int)(rnd() % 7);
        snprintf(buf, cap, "{\"tool\":\"%s\",\"args\":{%s,\"note\":\"see %s\"}}",
                 TRAFFIC_TOOL[t], TRAFFIC_ARGS[t], TRAFFIC_TOOL[t2]);
    }
}
static void sample_feat(double *feat, int nfeat) {
    char json[256];
    gen_traffic(json, sizeof json);
    cnet_jtc_encode(json, feat);      /* production encoder -> CNET_JTC_N_FEAT bits */
    (void)nfeat;
}
static BinaryTransformNetwork *find_btn(PrimitiveRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return reg->entries[i].btn;
    return NULL;
}
static int serve_tool(const RoutePlan *plan, const double *feat, int IN, int OUT) {
    double out[CNET_JTC_N_TOOL];
    if (route_execute_ex(plan, feat, (size_t)IN, out, (size_t)OUT, NULL) != 0) return -1;
    return cnet_jtc_decode_tool(out);
}

/* Representative validation sampler handed to registry_lora_tick: sparse
   features across the distribution (base-correct AND base-wrong), labeled by the
   shared hermetic teacher — so the in-tick gate can measure regressions. */
static size_t jtc_validate(const char *unit, int in_dim, int out_dim,
                           double *inputs, double *targets, size_t cap, void *ctx) {
    (void)unit; (void)ctx;
    for (size_t s = 0; s < cap; s++) {
        double *f = inputs + s * (size_t)in_dim;
        sample_feat(f, in_dim);
        cnet_jtc_hermetic_teacher(f, targets + s * (size_t)out_dim, NULL);
    }
    return cap;
}

/* returns 1 if the scenario matched its expectation, else 0 */
static int run_scenario(const char *tag, int strict_gate) {
    const char *base_path = "tmp_pa_lora.cnb", *ledger = "tmp_pa_lora.ledger", *inbox = "tmp_pa_lora.inbox";
    remove(base_path);
    { CnetBase base; cnb_init(&base);
      if (cnet_jtc_ensure_sealed(&base, NULL) < 0 || cnb_save(&base, base_path) != 0) {
          printf("FAIL: seal/save\n"); cnb_free(&base); return 0; }
      cnb_free(&base); }

    PersonalAiPolicy pol; personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0; pol.teach_inline = 0;
    PersonalAi ai;
    if (personal_ai_open(&ai, base_path, ledger, inbox, &pol) != 0) { printf("FAIL: open\n"); return 0; }
    PrimitiveRegistry *reg = &ai.lane.reg;
    const char *unit = CNET_JTC_UNIT_NAME;
    BinaryTransformNetwork *stu = find_btn(reg, unit);
    if (!stu) { printf("FAIL: no unit\n"); personal_ai_close(&ai); return 0; }
    const int IN = (int)stu->input_count, OUT = (int)stu->output_count;

    RoutePlan plan; memset(&plan, 0, sizeof plan);
    plan.steps[0] = stu; plan.names[0] = unit; plan.length = 1; plan.strict = 0;

    const int nte = 300;
    double *HF = malloc((size_t)nte * IN * sizeof(double));
    int *HT = malloc((size_t)nte * sizeof(int));
    double toh[CNET_JTC_N_TOOL];
    for (int s = 0; s < nte; s++) { double *f = HF + (size_t)s * IN; sample_feat(f, IN);
        cnet_jtc_hermetic_teacher(f, toh, NULL); HT[s] = cnet_jtc_decode_tool(toh); }
    int base_ok = 0;
    for (int s = 0; s < nte; s++) if (serve_tool(&plan, HF + (size_t)s * IN, IN, OUT) == HT[s]) base_ok++;

    double feat[CNET_JTC_N_FEAT]; int faults = 0;
    for (int i = 0; i < 900; i++) {
        sample_feat(feat, IN); cnet_jtc_hermetic_teacher(feat, toh, NULL);
        if (serve_tool(&plan, feat, IN, OUT) != cnet_jtc_decode_tool(toh)) {
            double bad[CNET_JTC_N_TOOL]; const double *so = btn_forward(stu, feat);
            for (int o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0.0;
            if (registry_record_fault(reg, unit, feat, bad) == 0 &&
                registry_supply_label(reg, unit, feat, toh) == 0) faults++;
        }
    }

    registry_lora_tick_opts topt = registry_lora_tick_defaults();
    topt.min_faults = 64; topt.holdout_frac = 0.25;
    topt.teach.rank = 8; topt.teach.alpha = 16.0f;
    topt.teach.train.epochs = 1500; topt.teach.train.lr = 0.02f;
    /* Regression-aware in-tick gate: certify on a representative sample, so
       strict (max_regressions=0) genuinely rejects on right->wrong flips. */
    topt.validate = jtc_validate;
    topt.validate_cap = 200;
    topt.cert.argmax_mode = 1;
    topt.cert.max_regressions = strict_gate ? 0 : faults / 10;
    topt.cert.min_net_gain = 1;
    registry_lora_install_orchestrator(reg, &topt);

    GapLaneTickReport trep; memset(&trep, 0, sizeof trep);
    int rc = personal_ai_tick(&ai, &trep);
    int cert_after = registry_lora_is_certified(reg, unit);

    int base_post = 0, served_post = 0;
    for (int s = 0; s < nte; s++) {
        const double *so = btn_forward(stu, HF + (size_t)s * IN);
        if (so && cnet_jtc_decode_tool(so) == HT[s]) base_post++;
        if (serve_tool(&plan, HF + (size_t)s * IN, IN, OUT) == HT[s]) served_post++;
    }
    int heal = 100 * (base_post - base_ok) / nte;
    int adapt = 100 * (served_post - base_post) / nte;
    printf("\n[%s]  faults=%d  rc=%d  certified=%d\n", tag, faults, rc, cert_after);
    printf("  baseline %d%% | base-student post %d%% (heal %+d) | served %d%% (adapter %+d)\n",
           100*base_ok/nte, 100*base_post/nte, heal, 100*served_post/nte, adapt);

    int ok;
    if (strict_gate) {   /* expect: adapter rejected, dense heal fallback ran */
        ok = (rc == 0 && cert_after == 0 && heal > 0);
        printf("  -> %s: gate REJECTED the adapter; gap_lane dense heal served as fallback\n",
               ok ? "PASS" : "FAIL");
    } else {             /* expect: adapter certified + serves, dense heal skipped */
        ok = (rc == 0 && cert_after == 1 && adapt > 0 && heal == 0);
        printf("  -> %s: adapter CERTIFIED + served; dense heal skipped (unit marked non-RESET)\n",
               ok ? "PASS" : "FAIL");
    }
    registry_lora_uninstall_orchestrator(reg);
    free(HF); free(HT); personal_ai_close(&ai);
    remove(base_path); remove(ledger); remove(inbox);
    return ok;
}

int main(void) {
    int a = run_scenario("A: adapter-first, reasonable gate", 0);
    S = 0x9E3779B9u;   /* same stream so both scenarios see the same data */
    int b = run_scenario("B: strict gate -> dense-heal fallback", 1);
    printf("\n%s: cheap-adapter-first with dense-heal fallback, driven by personal_ai_tick\n",
           (a && b) ? "ALL PASS" : "FAIL");
    printf("PA_LORA_TICK_DONE\n");
    return (a && b) ? 0 : 1;
}
